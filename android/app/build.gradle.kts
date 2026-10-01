// Pound — Android application module.

plugins {
    id("com.android.application")
}

// ---------------------------------------------------------------------------
// Native build
// ---------------------------------------------------------------------------
//
// `cmake/jni/CMakeLists.txt` is a thin wrapper around the repository's own
// top-level CMakeLists.txt rather than a copy of it, so the APK's native image
// is built by exactly the same graph as the desktop builds. In particular it
// still honours cmake/toolchains/android-arm64.cmake and its NDK pinning, so
// `externalNativeBuild` must not be given a competing `toolchainFile`.

val poundSdlJavaDir: Directory =
    rootProject.file("../extern/sdl/sdl/android-project/app/src/main/java")

// Read once so `signingConfigs` and `buildTypes` agree, and so an unsigned
// local build can be told apart from a misconfigured CI one.
val poundKeyStorePath: String? = providers.environmentVariable("POUND_KEYSTORE_PATH").orNull
    ?.takeIf { it.isNotBlank() && file(it).exists() }

// The single ABI Pound builds for. Named once because the ABI list, the JNI
// staging hook and cmake/toolchains/android-arm64.cmake all have to agree, and a
// disagreement shows up as an APK with no native image rather than as an error.
val poundAbis = listOf("arm64-v8a")

android {
    namespace = "dev.pound.emulator"
    compileSdk = 35
    ndkVersion = "27.2.12479018"

    defaultConfig {
        // Mirrors ANDROID_PLATFORM in cmake/toolchains/android-arm64.cmake.
        minSdk = 26
        targetSdk = 35

        versionCode = 1
        versionName = "0.1.0"

        externalNativeBuild {
            cmake {
                // c++_static keeps libc++ inside libmain.so, so the APK ships a
                // single native image instead of also needing libc++_shared.so.
                arguments += listOf("-DANDROID_STL=c++_static")
                abiFilters += poundAbis
            }
        }

        ndk {
            abiFilters += poundAbis
        }
    }

    externalNativeBuild {
        cmake {
            path = file("jni/CMakeLists.txt")
        }
    }

    signingConfigs {
        // Only populated when a keystore is actually available, so an unsigned
        // CI or local run never fails on missing credentials.
        if (poundKeyStorePath != null) {
            create("poundRelease") {
                storeFile = file(poundKeyStorePath)
                storePassword = providers.environmentVariable("POUND_KEYSTORE_PASSWORD").orNull
                keyAlias = providers.environmentVariable("POUND_KEY_ALIAS").orNull
                keyPassword = providers.environmentVariable("POUND_KEY_PASSWORD").orNull
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            isShrinkResources = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            // No keystore means an unsigned release APK, which is what a local
            // build should produce. Assigning a signing config with a null
            // storeFile would instead fail the build with a message about
            // credentials that says nothing about the real cause.
            signingConfig = signingConfigs.findByName("poundRelease")
        }
        debug {
            isMinifyEnabled = false
            applicationIdSuffix = ".debug"
            versionNameSuffix = "-debug"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        jniLibs {
            // SDL dlopen()s libmain.so by name; compressing it breaks the
            // extract-to-filesystem path SDL relies on on older API levels.
            useLegacyPackaging = true
        }
    }

    lint {
        abortOnError = false
    }

    sourceSets {
        getByName("main") {
            // SDL's Java runtime lives in package org.libsdl.app and is compiled
            // from the vendored SDL tree. Nothing in it references the
            // generated R or BuildConfig classes, so Pound's own namespace can
            // stay independent.
            java.srcDir(poundSdlJavaDir)
        }
    }
}

androidComponents {
    onVariants { variant ->
        val nativeBuildTask = "externalNativeBuild${variant.name.replaceFirstChar { it.uppercase() }}"

        // AGP merges src/main/jniLibs before the native build has necessarily
        // finished populating it, which yields an APK with no libmain.so.
        // Ordering the merge behind the native build is the same hook SDL's own
        // reference project uses.
        val jniMergeTasks = tasks.matching {
            it.name.startsWith("merge") && it.name.endsWith("JniLibFolders")
        }

        jniMergeTasks.configureEach { dependsOn(nativeBuildTask) }

        // Collect whatever the native build produced. The layout under .cxx is an
        // AGP implementation detail, so it is discovered rather than assumed, and
        // the collection is scoped to the configured ABIs: a leftover image from
        // an ABI that is no longer built would otherwise be packaged silently.
        jniMergeTasks.configureEach {
            doLast {
                val nativeRoot = layout.projectDirectory.dir(".cxx").asFile
                val staged = mutableListOf<File>()

                if (nativeRoot.isDirectory) {
                    nativeRoot.walkTopDown().forEach { candidate ->
                        if (candidate.isFile && candidate.name.endsWith(".so")) {
                            staged += candidate
                        }
                    }
                }

                // The ABI directory is a whole path segment, so compare segments
                // rather than substrings ("x86_64" is not a prefix of "arm64-v8a",
                // but "64" would match both).
                fun abiOf(candidate: File): String? =
                    candidate.path.split('/', '\\').firstOrNull { poundAbis.contains(it) }

                val mainImages = staged.filter { it.name == "libmain.so" }

                if (mainImages.isEmpty()) {
                    throw GradleException(
                        "No libmain.so was produced under $nativeRoot. The native build either " +
                            "did not run or failed; see the $nativeBuildTask output above."
                    )
                }

                val unscoped = mainImages.filter { abiOf(it) == null }

                if (unscoped.isNotEmpty()) {
                    throw GradleException(
                        "Found libmain.so outside any configured ABI directory " +
                            "(${poundAbis.joinToString()}):\n" +
                            unscoped.joinToString("\n") { "  ${it.path}" } +
                            "\nA stale build for a removed ABI is the usual cause; run `./gradlew clean`."
                    )
                }

                val inScope = staged.filter { abiOf(it) != null }

                // AGP keeps several copies of the same shared object under .cxx --
                // the raw CMake tree, its stripped intermediate, and the object
                // directory -- and they legitimately differ in size, so the newest
                // one is packaged rather than asserting they are byte-identical.
                val packagedByAbi = inScope
                    .groupBy { abiOf(it)!! }
                    .mapValues { (_, candidates) ->
                        candidates.groupBy { it.name }
                            .map { (_, sameName) -> sameName.maxBy { it.lastModified() } }
                    }

                packagedByAbi.forEach { (abi, images) ->
                    val abiDir = layout.projectDirectory.dir("src/main/jniLibs/$abi").asFile
                    abiDir.mkdirs()

                    // Remove images from earlier builds so a configuration change
                    // cannot leave an orphan behind in the packaged APK.
                    abiDir.listFiles { f -> f.isFile && f.name.endsWith(".so") }
                        ?.forEach { it.delete() }

                    images.forEach { source ->
                        val target = File(abiDir, source.name)
                        source.copyTo(target, overwrite = true)
                        logger.lifecycle(
                            "Packaging native image: $abi/${target.name} (${target.length()} bytes)"
                        )
                    }
                }
            }
        }
    }
}

dependencies {
    implementation("androidx.appcompat:appcompat:1.7.0")
}
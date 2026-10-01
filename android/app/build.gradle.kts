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
                abiFilters += listOf("arm64-v8a")
            }
        }

        ndk {
            abiFilters += listOf("arm64-v8a")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("jni/CMakeLists.txt")
        }
    }

    signingConfigs {
        // Only consulted when the matching environment variables are present, so
        // an unsigned CI run never fails on missing credentials.
        create("poundRelease") {
            val storePath = providers.environmentVariable("POUND_KEYSTORE_PATH").orNull
            if (storePath != null) {
                storeFile = file(storePath)
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
            signingConfig = signingConfigs.getByName("poundRelease")
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

        // Collect whatever the native build produced. The layout under .cxx is
        // an AGP implementation detail, so it is discovered rather than assumed;
        // what matters is that exactly one libmain.so exists.
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

                val mainImages = staged.filter { it.name == "libmain.so" }

                if (mainImages.isEmpty()) {
                    throw GradleException(
                        "No libmain.so was produced under $nativeRoot. The native build either " +
                            "did not run or failed; see the $nativeBuildTask output above."
                    )
                }

                if (mainImages.size > 1) {
                    throw GradleException(
                        "Expected exactly one libmain.so under $nativeRoot but found " +
                            "${mainImages.size}: ${mainImages.joinToString { it.path }}. " +
                            "A stale previous native build is the usual cause; run `./gradlew clean`."
                    )
                }

                val abiDir = layout.projectDirectory.dir("src/main/jniLibs/arm64-v8a").asFile
                abiDir.mkdirs()

                // Remove images from earlier builds so a configuration change
                // cannot leave an orphan behind in the packaged APK.
                abiDir.listFiles { f -> f.isFile && f.name.endsWith(".so") }
                    ?.forEach { it.delete() }

                staged.forEach { source ->
                    val target = File(abiDir, source.name)
                    source.copyTo(target, overwrite = true)
                    logger.lifecycle(
                        "Packaging native image: arm64-v8a/${target.name} (${target.length()} bytes)"
                    )
                }
            }
        }
    }
}

dependencies {
    implementation("androidx.appcompat:appcompat:1.7.0")
}
// Pound — Android build settings.
//
// The native emulator image is produced by the top-level CMake project through
// `cmake --preset android-arm64-release`, and CMake stages the result into
// `app/src/main/jniLibs/arm64-v8a/`. Gradle is therefore only responsible for
// the Java side (SDLActivity, the manifest, resources) and for packaging that
// staged native image into an APK.

pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "Pound"

include(":app")
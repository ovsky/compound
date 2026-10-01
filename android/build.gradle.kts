// Top-level build file: configuration common to every module.
//
// The Android Gradle Plugin version is pinned rather than resolved so that CI
// produces byte-identical APKs from one commit to the next.

plugins {
    id("com.android.application") version "8.7.3" apply false
}

tasks.register<Delete>("clean") {
    delete(layout.buildDirectory)
    delete(file(".gradle"))
}
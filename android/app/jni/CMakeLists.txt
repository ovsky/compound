# -----------------------------------------------------------------------------
# Pound — Android native build entry point
# -----------------------------------------------------------------------------
#
#   cd android
#   ./gradlew assembleRelease
#
# This file exists only to hand control to the repository's own
# CMakeLists.txt. The APK's native image is therefore produced by exactly the
# same target graph as the desktop builds -- same sources, same compiler flags,
# same Ballistic/JIT resolution -- instead of a second, drifting copy of the
# build graph.
#
# It deliberately does NOT call project(): the repository's CMakeLists.txt does,
# and adding a project() here would move CMAKE_SOURCE_DIR to this directory and
# break every path the repository resolves relative to its own root.

cmake_minimum_required(VERSION 3.25)

get_filename_component(REPOSITORY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)

if (NOT EXISTS "${REPOSITORY_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
            "Pound: repository root not found at ${REPOSITORY_ROOT}. "
            "android/app/jni/CMakeLists.txt expects to live at <repo>/android/app/jni/.")
endif ()

# -----------------------------------------------------------------------------
# Locate the NDK
# -----------------------------------------------------------------------------
#
# Gradle exports ANDROID_NDK_HOME for the externalNativeBuild step, but the
# command line (`cmake --preset android-arm64-release`) does not, so the
# standard SDK environment variables are honoured as a fallback. The full
# resolution order lives in cmake/toolchains/android-arm64.cmake.

if (NOT DEFINED ANDROID_NDK)
    if (DEFINED ENV{ANDROID_NDK_HOME})
        set(ANDROID_NDK "$ENV{ANDROID_NDK_HOME}")
    elseif (DEFINED ENV{ANDROID_NDK_ROOT})
        set(ANDROID_NDK "$ENV{ANDROID_NDK_ROOT}")
    elseif (DEFINED ENV{ANDROID_SDK_ROOT})
        # Match the newest installed revision rather than pinning a path, so a
        # machine with several NDKs installed still works.
        file(GLOB _ndk_versions "$ENV{ANDROID_SDK_ROOT}/ndk/*")
        list(SORT _ndk_versions)
        list(REVERSE _ndk_versions)

        foreach (_ndk IN LISTS _ndk_versions)
            if (IS_DIRECTORY "${_ndk}")
                set(ANDROID_NDK "${_ndk}")
                break()
            endif ()
        endforeach ()
    endif ()
endif ()

if (NOT ANDROID_NDK)
    message(FATAL_ERROR
            "Pound: ANDROID_NDK is not set. Install the NDK through Android Studio or set "
            "ANDROID_NDK_HOME before building.")
endif ()

set(ANDROID_NDK "${ANDROID_NDK}" CACHE PATH "Path to the Android NDK")

message(STATUS "[Pound] Android NDK: ${ANDROID_NDK}")

# -----------------------------------------------------------------------------
# Point CMake at the repository's toolchain and build graph
# -----------------------------------------------------------------------------

# Only injected when CMakePresets did not already select one, so the
# command-line and Gradle entry points cannot disagree about the toolchain.
if (NOT CMAKE_TOOLCHAIN_FILE)
    set(POUND_ANDROID_TOOLCHAIN "${REPOSITORY_ROOT}/cmake/toolchains/android-arm64.cmake")

    if (NOT EXISTS "${POUND_ANDROID_TOOLCHAIN}")
        message(FATAL_ERROR "Pound: missing toolchain file: ${POUND_ANDROID_TOOLCHAIN}")
    endif ()

    set(CMAKE_TOOLCHAIN_FILE "${POUND_ANDROID_TOOLCHAIN}" CACHE FILEPATH
            "Pound Android toolchain" FORCE)
endif ()

# An explicit binary directory keeps this wrapper's CMake state from colliding
# with AGP's own build bookkeeping under .cxx/.
add_subdirectory("${REPOSITORY_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/pound")
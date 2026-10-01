# -----------------------------------------------------------------------------
# Android NDK toolchain (ARM64)
# -----------------------------------------------------------------------------
#
#   cmake --preset android-arm64-debug
#   cmake --build --preset android-arm64-debug
#
# Override the NDK location with -DANDROID_NDK=/path/to/ndk or by exporting
# ANDROID_NDK. CMake's own `android.toolchain.cmake` is deliberately *not*
# used: it only supports legacy behaviour, whereas the NDK ships a per-ABI
# toolchain file under <ndk>/build/cmake/android.toolchain.cmake that handles
# the 16 KiB page-size and API-level requirements of modern Play targets.

# Declared before the fallback lookup below, which builds a default path from it.
set(POUND_ANDROID_NDK_VERSION "27.2.12479018" CACHE STRING "Expected NDK revision")

if (NOT DEFINED ANDROID_NDK)
    if (DEFINED ENV{ANDROID_NDK})
        set(ANDROID_NDK "$ENV{ANDROID_NDK}" CACHE PATH "Path to the Android NDK")
    else ()
        # The default NDK location used by Android Studio on Windows and macOS.
        if (WIN32)
            set(_default_ndk "$ENV{ANDROID_SDK_ROOT}/ndk/$ENV{POUND_ANDROID_NDK_VERSION}")
        elseif (APPLE)
            set(_default_ndk "$ENV{ANDROID_SDK_ROOT}/ndk/$ENV{POUND_ANDROID_NDK_VERSION}")
        else ()
            set(_default_ndk "$ENV{ANDROID_NDK_ROOT}/ndk/$ENV{POUND_ANDROID_NDK_VERSION}")
        endif ()

        if (_default_ndk AND EXISTS "${_default_ndk}")
            set(ANDROID_NDK "${_default_ndk}" CACHE PATH "Path to the Android NDK")
        endif ()
    endif ()
endif ()

if (NOT ANDROID_NDK)
    message(FATAL_ERROR
            "[Pound] ANDROID_NDK is not set. Install the NDK and either export ANDROID_NDK or "
            "pass -DANDROID_NDK=/path/to/android-ndk.")
endif ()

if (NOT IS_DIRECTORY "${ANDROID_NDK}")
    message(FATAL_ERROR "[Pound] ANDROID_NDK does not exist: ${ANDROID_NDK}")
endif ()

if (EXISTS "${ANDROID_NDK}/source.properties")
    file(READ "${ANDROID_NDK}/source.properties" _ndk_props)
    string(REGEX MATCH "Pkg\\.Revision[ \t]*=[ \t]*([^ \t\r\n]+)" _ndk_rev "${_ndk_props}")

    if (_ndk_rev AND NOT _ndk_rev STREQUAL POUND_ANDROID_NDK_VERSION)
        message(STATUS
                "[Pound] NDK revision ${_ndk_rev} differs from the pinned "
                "${POUND_ANDROID_NDK_VERSION}.")
    endif ()
endif ()

# ARM64 is the only ABI a Switch 2 emulator is useful on: emulating AArch64 on
# 32-bit ARM would require a second translation layer.
set(ANDROID_ABI "arm64-v8a" CACHE STRING "Android ABI")
set(ANDROID_PLATFORM "android-26" CACHE STRING "Minimum supported Android API level")
set(POUND_ANDROID_ABI_DIR "${ANDROID_ABI}" CACHE STRING "Android ABI directory name")

# 16 KiB page size is mandatory for devices targeting Android 15+; see
# https://developer.android.com/guide/practices/page-sizes
set(ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES ON CACHE BOOL "Support 16 KiB pages")

include("${ANDROID_NDK}/build/cmake/android.toolchain.cmake")

# The NDK toolchain file hard-codes these to the build host, which breaks
# cross-compiling from Windows to Android arm64.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BEFORE)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

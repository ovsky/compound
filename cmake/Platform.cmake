include_guard(GLOBAL)

# -----------------------------------------------------------------------------
# Pound target detection
# -----------------------------------------------------------------------------
#
# Resolves a coarse platform id (`windows`, `mac`, `linux`, `android`) plus a
# host-architecture id, and normalises the handful of CMake variables the rest
# of the build needs. Called from the top-level CMakeLists *after* `project()`.

# Maps CMAKE_SYSTEM_NAME onto POUND_PLATFORM.
function(pound_detect_platform out_platform out_android out_posix)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(platform "windows")
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(platform "mac")
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Android")
        # Must be tested before Linux: the NDK reports CMAKE_SYSTEM_NAME as
        # "Android" but leaves __linux__ defined, so order matters.
        set(platform "android")
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(platform "linux")
    else ()
        message(FATAL_ERROR
                "[Pound] Unsupported target system '${CMAKE_SYSTEM_NAME}'. "
                "Pound supports Windows, macOS, Linux and Android.")
    endif ()

    if (platform STREQUAL "windows")
        set(is_android FALSE)
        set(is_posix  FALSE)
    elseif (platform STREQUAL "android")
        set(is_android TRUE)
        set(is_posix  TRUE)
    else ()
        set(is_android FALSE)
        set(is_posix  TRUE)
    endif ()

    set(${out_platform} "${platform}" PARENT_SCOPE)
    set(${out_android} "${is_android}" PARENT_SCOPE)
    set(${out_posix}  "${is_posix}" PARENT_SCOPE)
endfunction()

# Maps CMAKE_SYSTEM_PROCESSOR onto `arm64` or `x86_64`.
function(pound_detect_architecture out_arch)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" processor)

    if (processor MATCHES "^(aarch64|arm64|arm64e|arm64-v8a)$")
        set(arch "arm64")
    elseif (processor MATCHES "^(x86_64|amd64|x64)$")
        set(arch "x86_64")
    else ()
        message(FATAL_ERROR
                "[Pound] Unsupported target processor '${CMAKE_SYSTEM_PROCESSOR}'. "
                "Pound requires 64-bit ARM or 64-bit x86.")
    endif ()

    set(${out_arch} "${arch}" PARENT_SCOPE)
endfunction()

# -----------------------------------------------------------------------------
# Shared-library naming
# -----------------------------------------------------------------------------
#
# Deliberately empty of overrides. CMake already picks the right per-platform
# defaults (no prefix on Windows, `lib` + `.dylib` on macOS, `lib` + `.so` on
# Linux and Android), and this project needs no global normalisation.
#
# In particular, do NOT set CMAKE_SHARED_LIBRARY_PREFIX to "lib" here. Only
# Pound's own plugin carries that prefix -- SDL3's Windows DLL must stay
# `SDL3.dll`, because SDL3's own CMake exports the runtime loader against that
# name. A global override would silently rename it to `libSDL3.dll` and the
# failure would only appear at startup on a user's machine.
#
# The prefix is therefore applied per target; see the PoundGui block in the
# top-level CMakeLists.txt.

function(pound_set_shared_library_defaults)
    message(STATUS "[Pound] Shared library naming uses CMake defaults; PoundGui prefixes itself.")
endfunction()

# -----------------------------------------------------------------------------
# JNI packaging for Android
# -----------------------------------------------------------------------------
#
# The Gradle build expects every shared library and the executable under
# jniLibs/<abi>/. `pound_collect_jni_artifacts` is the single place that knows
# the on-device layout, so both the CMake and Gradle sides agree.

set(POUND_ANDROID_ABI_DIR
        "${ANDROID_ABI}"
        CACHE STRING "Android ABI directory name (arm64-v8a, x86_64, ...)"
        )

function(pound_collect_jni_artifacts binary_dir)
    set(abi "${POUND_ANDROID_ABI_DIR}")

    if (abi STREQUAL "")
        message(FATAL_ERROR "[Pound] POUND_ANDROID_ABI_DIR must be set for Android builds.")
    endif ()

    set(staging "${binary_dir}/jniLibs/${abi}")
    file(MAKE_DIRECTORY "${staging}")

    # Wildcards are used deliberately: the exact file set depends on which
    # third-party targets are enabled, and a hard-coded list silently rots.
    file(GLOB runtime_libs
            "${POUND_RUNTIME_LIBRARY_DIR}/*.so"
            "${POUND_RUNTIME_LIBRARY_DIR}/*.dylib"
            )

    foreach (lib IN LISTS runtime_libs)
        get_filename_component(lib_name "${lib}" NAME)
        message(STATUS "[Pound] Packaging JNI artifact: ${abi}/${lib_name}")
        file(COPY "${lib}" DESTINATION "${staging}")
    endforeach()

    # The emulator image itself (libmain.so) is staged after the link by
    # cmake/StageJniArtifacts.cmake; at configure time it does not exist yet.
    message(STATUS "[Pound] JNI staging directory ready: ${staging}")

    set(POUND_JNI_STAGING_DIR "${staging}" PARENT_SCOPE)
endfunction()

# -----------------------------------------------------------------------------
# Sanitiser availability
# -----------------------------------------------------------------------------
#
# ASan/UBSan are unavailable (or actively harmful) for cross-compiled and
# Android targets, so a preset that requests them is downgraded rather than
# failing the link.

function(pound_resolve_sanitizers)
    if (NOT DEFINED POUND_SANITIZERS OR POUND_SANITIZERS STREQUAL "")
        set(POUND_SANITIZERS "" CACHE STRING "" FORCE)
        return()
    endif ()

    if (CMAKE_CROSSCOMPILING)
        message(STATUS
                "[Pound] Dropping sanitizers: cross-compiling to ${CMAKE_SYSTEM_NAME}.")
        set(POUND_SANITIZERS "" CACHE STRING "" FORCE)
        return()
    endif ()

    if (CMAKE_SYSTEM_NAME STREQUAL "Android")
        message(STATUS "[Pound] Dropping sanitizers: unsupported on Android.")
        set(POUND_SANITIZERS "" CACHE STRING "" FORCE)
        return()
    endif ()
endfunction()

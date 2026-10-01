# -----------------------------------------------------------------------------
# Linux aarch64 cross toolchain
# -----------------------------------------------------------------------------
#
#   cmake --preset linux-aarch64-release
#   cmake --build --preset linux-aarch64-release
#
# Targets arm64 Linux from an x86_64 host using the aarch64-linux-gnu GNU
# toolchain plus sysroot. Used by the CI lane that validates the ARM64 code
# path; pass -D<a|b> to select a different cross prefix.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(POUND_LINUX_CROSS_PREFIX "aarch64-linux-gnu" CACHE STRING "Cross toolchain prefix")

# Honour an externally supplied toolchain (Debian multiarch, Fedora, Homebrew,
# Android NDK) before falling back to the plain-prefix lookup.
if (DEFINED POUND_LINUX_SYSROOT AND NOT POUND_LINUX_SYSROOT STREQUAL "")
    set(CMAKE_SYSROOT "${POUND_LINUX_SYSROOT}" CACHE PATH "Target sysroot")
endif ()

set(CMAKE_C_COMPILER   "${POUND_LINUX_CROSS_PREFIX}-gcc")
set(CMAKE_CXX_COMPILER "${POUND_LINUX_CROSS_PREFIX}-g++")

set(CMAKE_C_COMPILER_TARGET   aarch64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET aarch64-linux-gnu)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Cross-compiled test binaries cannot be executed directly; CTest needs to be
# told so that `ctest` runs them under QEMU user-mode emulation.
find_program(POUND_QEMU_AARCH64 NAMES qemu-aarch64 qemu-aarch64-static)

if (POUND_QEMU_AARCH64)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${POUND_QEMU_AARCH64}" CACHE FILEPATH
            "Emulator used to run cross-compiled test binaries")
    message(STATUS "[Pound] aarch64 test binaries will run under ${POUND_QEMU_AARCH64}.")
else ()
    message(STATUS
            "[Pound] qemu-aarch64 not found; cross-compiled test binaries will be "
            "built but not executed.")
endif ()

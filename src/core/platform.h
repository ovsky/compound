//! Compile-time platform and architecture detection.
//!
//! Exactly one `POUND_PLATFORM_*` selector is 1 and the rest are 0, so the
//! header can be used directly in `#if` expressions.
//!
//! Android is detected *before* Linux because the NDK toolchain also defines
//! `__linux__`. Code that must distinguish a glibc Linux host from an Android
//! (bionic) target should test `POUND_PLATFORM_ANDROID`, not `POUND_PLATFORM_LINUX`.

#ifndef POUND_PLATFORM_H
#define POUND_PLATFORM_H

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

#if defined(_WIN32) || defined(_WIN64)

#define POUND_PLATFORM_WINDOWS 1
#define POUND_PLATFORM_APPLE   0
#define POUND_PLATFORM_LINUX   0
#define POUND_PLATFORM_ANDROID 0
#define POUND_PLATFORM_POSIX   0
#define POUND_PLATFORM_MOBILE  0

#elif defined(__ANDROID__)

#define POUND_PLATFORM_WINDOWS 0
#define POUND_PLATFORM_APPLE   0
#define POUND_PLATFORM_LINUX   1
#define POUND_PLATFORM_ANDROID 1
#define POUND_PLATFORM_POSIX   1
#define POUND_PLATFORM_MOBILE  1

#elif defined(__APPLE__)

#define POUND_PLATFORM_WINDOWS 0
#define POUND_PLATFORM_APPLE   1
#define POUND_PLATFORM_LINUX   0
#define POUND_PLATFORM_ANDROID 0
#define POUND_PLATFORM_POSIX   1
#define POUND_PLATFORM_MOBILE  0

#elif defined(__linux__)

#define POUND_PLATFORM_WINDOWS 0
#define POUND_PLATFORM_APPLE   0
#define POUND_PLATFORM_LINUX   1
#define POUND_PLATFORM_ANDROID 0
#define POUND_PLATFORM_POSIX   1
#define POUND_PLATFORM_MOBILE  0

#else

#error "Unknown Platform"

#endif

// -----------------------------------------------------------------------------
// Architecture
// -----------------------------------------------------------------------------
//
// Ballistic emits host code for the machine it runs on and Pound targets
// 64-bit ARM (Android devices, Apple Silicon, ARM servers) and 64-bit x86
// (Windows, desktop Linux). 32-bit targets are rejected outright.

#if defined(__aarch64__) || defined(_M_ARM64)

#define POUND_ARCHITECTURE_ARM64 1
#define POUND_ARCHITECTURE_X86   0
#define POUND_ARCHITECTURE_ARM   1

#elif defined(__x86_64__) || defined(_M_X64)

#define POUND_ARCHITECTURE_ARM64 0
#define POUND_ARCHITECTURE_X86   1
#define POUND_ARCHITECTURE_ARM   0

#else

#error "Pound requires a 64-bit ARM or x86 environment."

#endif

/// Endianness of the emulated guest (Nintendo Switch 2, a little-endian AArch64
/// system). Ballistic decodes little-endian instruction words.
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)

#define POUND_GUEST_ENDIAN_BIG 1
#define POUND_GUEST_ENDIAN_LIT 0

#else

#define POUND_GUEST_ENDIAN_BIG 0
#define POUND_GUEST_ENDIAN_LIT 1

#endif

/// Human readable platform id, mirroring CMake's `POUND_PLATFORM` string.
#if POUND_PLATFORM_WINDOWS
#define POUND_PLATFORM_NAME "windows"
#elif POUND_PLATFORM_APPLE
#define POUND_PLATFORM_NAME "mac"
#elif POUND_PLATFORM_ANDROID
#define POUND_PLATFORM_NAME "android"
#elif POUND_PLATFORM_LINUX
#define POUND_PLATFORM_NAME "linux"
#else
#define POUND_PLATFORM_NAME "unknown"
#endif

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* POUND_PLATFORM_H */

/*** end of file ***/

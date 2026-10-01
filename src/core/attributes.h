//! Compiler specific attribute macros.

#ifndef POUND_ATTRIBUTES_H
#define POUND_ATTRIBUTES_H

// -----------------------------------------------------------------------------
// Compiler identification
// -----------------------------------------------------------------------------

#if defined(_MSC_VER)

/// MSVC in its clang-cl driver mode. `__FILE_NAME__` is not available, only `__FILE__`.
#define POUND_COMPILER_MSVC   1
#define POUND_COMPILER_CLANG  0
#define POUND_COMPILER_GCC    0

#elif defined(__clang__)

#define POUND_COMPILER_MSVC   0
#define POUND_COMPILER_CLANG  1
#define POUND_COMPILER_GCC    0

#elif defined(__GNUC__)

#define POUND_COMPILER_MSVC   0
#define POUND_COMPILER_CLANG  0
#define POUND_COMPILER_GCC    1

#else
#error "Unsupported compiler: Pound requires Clang, clang-cl or GCC."
#endif

// -----------------------------------------------------------------------------
// Branch prediction
// -----------------------------------------------------------------------------

/// POUND_HOT()/POUND_COLD()
/// Marks a function as hot or cold. Hot makes the compiler optimize it more
/// aggressively. Cold marks the function as rarely executed.
///
/// Usage:
/// POUND_HOT POUND_error_t emit_instruction(...);
#define POUND_HOT  __attribute__((hot))
#define POUND_COLD __attribute__((cold))

/// POUND_LIKELY(x)/POUND_UNLIKELY(x)
/// Hints to the CPU branch predictor. Should only be used in hot functions.
///
/// Usage: if (POUND_UNLIKELY(ptr == NULL)) { ... }
#define POUND_LIKELY(x)   __builtin_expect(!!(x), 1)
#define POUND_UNLIKELY(x) __builtin_expect(!!(x), 0)

// -----------------------------------------------------------------------------
// Code and data layout
// -----------------------------------------------------------------------------

/// POUND_ALIGNED(x)
/// Aligns a variable or a structure to x bytes.
///
/// Usage: POUND_ALIGNED(64)  struct data { ... };
#define POUND_ALIGNED(x) __attribute__((aligned(x)))

/// POUND_NORETURN
/// Tells the compiler a function never returns to its caller.
#define POUND_NORETURN __attribute__((noreturn))

/// POUND_UNUSED
/// Silences "unused parameter/variable" diagnostics without casting to void.
#define POUND_UNUSED(x) ((void)(x))

/// POUND_PURE
/// Declares a function as having no observable side effects other than its
/// return value, enabling the compiler to hoist and CSE calls.
#define POUND_PURE __attribute__((pure))

/// POUND_MALLOC
/// Declares an allocator whose return value aliases none of its arguments.
#define POUND_MALLOC __attribute__((malloc))

// -----------------------------------------------------------------------------
// Aliasing
// -----------------------------------------------------------------------------

/// POUND_RESTRICT
/// Tells the compiler that a pointer does not alias any other pointer in
/// current scope.
#define POUND_RESTRICT __restrict__

// -----------------------------------------------------------------------------
// Linkage
// -----------------------------------------------------------------------------

/// POUND_EXPORT
/// Marks a function or global variable for export.
///
/// Usage: POUND_EXPORT void POUND_public_api_function(void);
#define POUND_EXPORT __attribute__((visibility("default")))

/// POUND_WEAK
/// Marks a function or variable as weak, allowing the host application to override it at
/// link-time by providing a strong symbol with the same name.
///
/// Usage: POUND_WEAK void POUND_default_logger(...);
#define POUND_WEAK __attribute__((weak))

/// POUND_THREAD_LOCAL
/// Declares a variable with thread-local storage duration.
#define POUND_THREAD_LOCAL _Thread_local

/// POUND_NOINLINE
/// Prevent function inlining.
///
/// Usage: POUND_NOINLINE void my_function(void);
#define POUND_NOINLINE __attribute__((noinline))

/// POUND_ALWAYS_INLINE
/// Force inlining. Use sparingly: it disables the compiler's cost model.
#define POUND_ALWAYS_INLINE __attribute__((always_inline)) inline

#endif // POUND_ATTRIBUTES_H

/*** end of file ***/

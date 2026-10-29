#ifndef POUND_VALIDATION_H
#define POUND_VALIDATION_H

//! Checked arithmetic and range validation shared by loaders and allocators.
//!
//! The parsers in `src/core/fs` each grew their own offset checks because they
//! were written before a container had a common way to ask "does this range
//! fit?". The result was correct but repetitive: the same `offset > size ||
//! length > size - offset` dance, with a different error code and a slightly
//! different overflow handling in each file.
//!
//! This module is the shared, testable version. It is deliberately small and
//! allocation-free, and it does not log: a validator is called on data from a
//! file or from a guest, so an invalid range is an ordinary answer and the
//! caller has the context to decide whether to log it. The arithmetic errors
//! are still distinct from range errors, because a wrapped offset and an
//! out-of-bounds offset usually mean different things to the caller.

#include "attributes.h"
#include "errors.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/// Adds two `size_t` values, refusing to wrap.
///
/// Returns:
/// - `POUND_SUCCESS`.
/// - `POUND_ERROR_INVALID_ARGUMENT` -- `out` is NULL.
/// - `POUND_ERROR_GUEST_ADDRESS_OVERFLOW` -- `a + b` does not fit in `size_t`.
error_t validation_add_size(size_t a, size_t b, size_t *POUND_RESTRICT out);

/// Multiplies two `size_t` values, refusing to wrap.
///
/// Returns:
/// - `POUND_SUCCESS`.
/// - `POUND_ERROR_INVALID_ARGUMENT` -- `out` is NULL.
/// - `POUND_ERROR_GUEST_ADDRESS_OVERFLOW` -- `a * b` does not fit in `size_t`.
error_t validation_mul_size(size_t a, size_t b, size_t *POUND_RESTRICT out);

/// Rounds `value` up to the next multiple of `alignment`.
///
/// `alignment` must be a non-zero power of two. A value that is already aligned
/// is returned unchanged, and no operation is performed that could overflow
/// when the value is exactly at the top of the address space.
///
/// Returns:
/// - `POUND_SUCCESS`.
/// - `POUND_ERROR_INVALID_ARGUMENT` -- `out` is NULL, or `alignment` is zero or
///   not a power of two.
/// - `POUND_ERROR_GUEST_ADDRESS_OVERFLOW` -- the rounded value does not fit.
error_t validation_align_up_size(size_t value,
                                 size_t alignment,
                                 size_t *POUND_RESTRICT out);

/// Reports whether `value` is a non-zero power of two.
bool validation_is_power_of_two(size_t value);

/// Checks that `[offset, offset + length)` lies inside `[0, total)`.
///
/// A zero-length range is accepted when `offset <= total`, which is what a
/// parser wants for an optional empty section. The subtraction is arranged so
/// that no intermediate value can wrap, regardless of how large `offset` or
/// `length` is.
///
/// Returns:
/// - `POUND_SUCCESS`.
/// - `POUND_ERROR_GUEST_ADDRESS_OUT_OF_BOUNDS` -- the range starts past `total`
///   or extends past it.
error_t validation_range_size(size_t offset, size_t length, size_t total);

/// Checks that `[base, base + length)` lies inside `[0, limit)` for 64-bit
/// guest addresses.
///
/// Returns:
/// - `POUND_SUCCESS`, and `*out_end` receives `base + length`.
/// - `POUND_ERROR_INVALID_ARGUMENT` -- `out_end` is NULL.
/// - `POUND_ERROR_GUEST_ADDRESS_OVERFLOW` -- `base + length` wraps.
/// - `POUND_ERROR_GUEST_ADDRESS_OUT_OF_BOUNDS` -- the range extends past
///   `limit`.
error_t validation_range_u64(uint64_t base,
                             uint64_t length,
                             uint64_t limit,
                             uint64_t *POUND_RESTRICT out_end);

/// Reports whether `[address, address + length)` is fully inside
/// `[base, base + size)`, without overflowing.
///
/// This is the predicate form for hot paths that only need a yes/no answer and
/// already have a reason to reject on false.
bool validation_range_contains(uint64_t base,
                               uint64_t size,
                               uint64_t address,
                               uint64_t length);

#ifdef __cplusplus
}
#endif

#endif // POUND_VALIDATION_H

/*** end of file ***/

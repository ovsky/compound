//! Binding the W^X code cache to Ballistic's allocator interface.
//!
//! Ballistic does not call malloc. It asks for aligned memory, for memory it can
//! write machine code into, and for that memory to be flipped between writable and
//! executable, through six callbacks in [`bal_allocator_t`]. This module is the
//! implementation of those six callbacks on top of
//! [`jit_cache_t`](jit/jit_cache.h).
//!
//! # Why a separate translation unit
//!
//! `jit_cache` itself has no dependency on Ballistic. That is deliberate: the
//! cache is ordinary host memory management and is fully testable without a JIT
//! engine present, which matters because Ballistic ships no ARM64 prebuild, so on
//! the Android and aarch64 lanes the cache has to be usable and verified on its
//! own. This file is the only thing that knows about `bal_*`, so the dependency
//! runs in one direction and only on the lanes that have the engine.
//!
//! # What the callbacks can and cannot report
//!
//! All six return `void` or a bare pointer, so none of them can hand Ballistic a
//! status code. Every failure is therefore turned into the strongest signal the
//! signature allows plus a log record: a failed allocation yields `NULL` (or a
//! buffer with both pointers `NULL`, which is what the interface specifies), and a
//! failed protection change is logged at error level naming the buffer. There is
//! no silent path, because a Ballistic block silently left un-executable is a jump
//! into a read-only page two million instructions into a game.
//!
//! # Pointer identity in the executable buffer
//!
//! `bal_executable_buffer_t` carries both an `rw_pointer` and an `rx_pointer`. This
//! cache hands out the *same* address for both, because a user-mode Windows process
//! cannot map one range twice under two protections and Win32's `VirtualAlloc` has
//! a single `flProtect` per reservation. The callbacks therefore take whichever
//! pointer is non-NULL, preferring `rw_pointer`, which is correct whether the two
//! alias or not. The contract Ballistic states -- write through one, execute through
//! the other -- is satisfied identically either way.

#ifndef POUND_JIT_BALLISTIC_H
#define POUND_JIT_BALLISTIC_H

#include "errors.h"
#include "jit/jit_cache.h"
#include "bal_memory.h"

/// Populates `out_allocator` with callbacks that serve requests from `cache`.
///
/// The cache itself is *not* copied or referenced-counted: it must outlive
/// `out_allocator`, and every callback it installs is valid for as long as the
/// cache is initialised. Destroying the cache while Ballistic still holds the
/// allocator is a use-after-free the cache cannot detect, so teardown order is the
/// engine's responsibility.
///
/// Returns `POUND_SUCCESS`, or a typed error with a log record:
/// `POUND_ERROR_INVALID_ARGUMENT` for a NULL pointer, and
/// `POUND_ERROR_NOT_INITIALIZED` when `cache` has not been initialised.
error_t jit_cache_bind_ballistic(jit_cache_t *POUND_RESTRICT cache, bal_allocator_t *POUND_RESTRICT out_allocator);

/// Writes a `bal_executable_buffer_t` describing the executable block at `block`.
///
/// `rw_pointer` and `rx_pointer` are set to the same address; see the module comment
/// for why, and for why that is not a loss.
///
/// Returns `POUND_SUCCESS`, or a typed error with a log record. A block that is not
/// a live executable block of `cache` is refused with `POUND_ERROR_DOUBLE_FREE`,
/// which is what the enum's "not a live object of the right kind" code means here.
error_t jit_cache_executable_buffer(const jit_cache_t *POUND_RESTRICT cache,
                                    void *POUND_RESTRICT                  block,
                                    bal_executable_buffer_t *POUND_RESTRICT out_buffer);

#endif // POUND_JIT_BALLISTIC_H

/*** end of file ***/

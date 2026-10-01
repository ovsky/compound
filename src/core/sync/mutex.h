//! Minimal recursive-free mutex abstraction for Pound core subsystems.
//!
//! The guest-facing allocators (pool, slab) and the JIT code cache are shared
//! between the emulated CPU thread, GUI threads and the hot-reload path, so
//! they need mutual exclusion. `PoundCore` deliberately depends on nothing but
//! mimalloc, so it cannot use SDL's mutex wrappers, and pulling `<threads.h>`
//! would leave clang-cl without a C11 threads implementation.
//!
//! Instead this maps onto the two primitives every supported target already
//! ships:
//!
//! - Windows: `SRWLOCK`, an acquire/release reader-writer lock used here in its
//!   exclusive mode. It is statically initialisable, so `mutex_init` cannot
//!   fail.
//! - POSIX: `pthread_mutex_t`, initialised as a default (non-recursive) mutex.
//!
//! The type is opaque in the header so that no platform header leaks into
//! `PoundCore`'s consumers.

#ifndef POUND_MUTEX_H
#define POUND_MUTEX_H

#include "attributes.h"
#include "errors.h"
#include "platform.h"

#if POUND_PLATFORM_WINDOWS

#include <windows.h>

/// Number of `SRWLOCK`s needed, for static initialisation by callers that
/// prefer a file-scope mutex.
#define POUND_MUTEX_STATIC_INIT SRWLOCK_INIT

typedef struct
{
    SRWLOCK lock;
} mutex_t;

#elif POUND_PLATFORM_POSIX

#include <pthread.h>

#define POUND_MUTEX_STATIC_INIT PTHREAD_MUTEX_INITIALIZER

typedef struct
{
    pthread_mutex_t lock;
} mutex_t;

#else
#error "pound_mutex_t has no implementation for this platform."
#endif

/// Initialises `mutex`.
///
/// Returns `POUND_SUCCESS`. On Windows the lock needs no runtime
/// initialisation, so this cannot fail; on POSIX an invalid mutex is detected
/// here rather than at the first acquire, where the failure would surface at a
/// far less useful point in the stack.
error_t mutex_init(mutex_t *POUND_RESTRICT mutex);

/// Releases all resources held by `mutex`.
///
/// A locked mutex is released by the platform contract rather than reported as
/// an error: on Windows `SRWLOCK` has no destroy call at all, and on POSIX an
/// abandoned lock would leave the object unusable. Callers are responsible for
/// not destroying a mutex that is still held.
void mutex_destroy(mutex_t *POUND_RESTRICT mutex);

/// Acquires exclusive ownership, blocking until it is available.
void mutex_lock(mutex_t *POUND_RESTRICT mutex);

/// Releases exclusive ownership acquired by `mutex_lock` on the same thread.
///
/// The behaviour on an unlocked mutex is undefined by both underlying
/// primitives, so this is documented rather than checked; the allocators pair
/// lock and unlock on every path, including the error paths.
void mutex_unlock(mutex_t *POUND_RESTRICT mutex);

#endif // POUND_MUTEX_H

/*** end of file ***/
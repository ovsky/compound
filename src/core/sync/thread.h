//! A detached-worker thread, which is the shape the JIT engine requires.
//!
//! `bal_engine_run_thread` is documented as needing "a dedicated thread": it is a
//! function that runs until the guest stops, and the engine keeps per-thread state
//! against the thread that entered it. Calling it on the thread that also owns the
//! emulator's UI is therefore not a style preference -- the engine returns without
//! executing anything, which looks exactly like a guest program that does nothing.
//!
//! So this is not test scaffolding. The emulator needs it for the same reason: the
//! engine's execution loop has to have a thread of its own that the GUI thread can ask
//! it to stop.
//!
//! # Why this exists rather than C11 threads
//!
//! `PoundCore` deliberately depends on nothing but mimalloc, for the same reason
//! `mutex` exists: SDL's wrappers are not available to `PoundCore`, and `<threads.h>`
//! would leave clang-cl without an implementation. So this maps onto the two thread
//! primitives every supported target already ships -- `CreateThread` on Windows and
//! `pthread_create` on POSIX -- and the type is opaque so no platform header leaks
//! into consumers.
//!
//! # Why only the joinable form
//!
//! There is no "detach" here, and that is deliberate. A detached thread that is still
//! running when the cache, the CPU state or the engine is destroyed is a use-after-free
//! that nothing in Pound could detect: the store happens on a stack frame nobody
//! owns any more. `thread_join` is therefore the only exit, so that a caller cannot
//! lose track of a thread that is still inside guest code.

#ifndef POUND_THREAD_H
#define POUND_THREAD_H

#include "attributes.h"
#include "errors.h"
#include "platform.h"

#if POUND_PLATFORM_WINDOWS

#include <windows.h>

/// The platform's own thread handle, wrapped so no platform header reaches consumers.
///
/// `NULL` before `thread_start` and after `thread_join`.
typedef struct
{
    HANDLE handle;
} thread_t;

#elif POUND_PLATFORM_POSIX

#include <pthread.h>

typedef struct
{
    pthread_t handle;
    int       started;
} thread_t;

#else
#error "thread_t has no implementation for this platform."
#endif

/// The work a thread performs.
///
/// `context` is the pointer given to `thread_start`, returned unchanged. The return
/// value is discarded: a thread's outcome belongs to whatever it was working on, and
/// reporting it here would mean inventing a channel for it. `thread_run` returning
/// early is normal -- a JIT engine returns as soon as it is asked to stop.
typedef void (*thread_entry_t)(void *context);

/// Starts `entry` on a new thread, with `context` passed to it unchanged.
///
/// Returns `POUND_SUCCESS`, or a typed error with a log record:
/// `POUND_ERROR_INVALID_ARGUMENT` for a NULL thread, a NULL entry, or a thread that
/// has already been started, and `POUND_ERROR_ALLOCATION_FAILED` when the platform
/// refuses to create one.
///
/// The new thread starts running before this returns, so anything `entry` touches
/// must already be published -- which is why the error is returned for an
/// already-started thread rather than the thread being silently replaced.
error_t thread_start(thread_t *POUND_RESTRICT thread, thread_entry_t entry, void *context);

/// Waits for the thread to finish, then releases the platform handle.
///
/// Returns `POUND_SUCCESS`, or a typed error with a log record:
/// `POUND_ERROR_INVALID_ARGUMENT` for a NULL or never-started thread, and
/// `POUND_ERROR_THREAD_FAILED` if the wait itself failed.
///
/// Calling this on a thread that was never started is refused rather than treated as
/// a no-op, because the two mean different things to a caller cleaning up after a
/// failure partway through a bring-up sequence: one is "nothing to do", the other is
/// "a thread is running and you have lost track of it".
///
/// Joining a thread from inside that thread is undefined on both platforms and is not
/// checked; the caller has the same information the platform would refuse on.
error_t thread_join(thread_t *POUND_RESTRICT thread);

#endif // POUND_THREAD_H

/*** end of thread.h ***/
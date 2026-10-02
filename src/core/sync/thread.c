//! Portable worker threads. See `thread.h` for why this exists and why there is no
//! detached form.

#include "thread.h"

#include "log.h"

#include <stdlib.h>

/// What a thread is handed at creation.
///
/// One allocation holding both fields, rather than two allocations, so the pair cannot
/// be published to the running thread separately -- which would be a torn read on the
/// new thread, and exactly the kind of race a thread module exists to not have. It is
/// freed by the trampoline, since it exists only to reach that trampoline.
typedef struct
{
    thread_entry_t entry;
    void *         context;
} thread_arguments_t;

#if POUND_PLATFORM_WINDOWS

#include <process.h>

/// The platform entry point. `_beginthreadex` takes an argument list C has no type for,
/// so the arguments travel as one opaque pointer and are unwrapped here.
static unsigned __stdcall
trampoline(void *raw)
{
    // Copied out before the free, because `entry` and `context` are read after it and a
    // use-after-free would be indistinguishable from a corrupted pair.
    thread_arguments_t *const arguments = (thread_arguments_t *)raw;
    const thread_entry_t entry          = arguments->entry;
    void *const          context        = arguments->context;

    free(arguments);

    entry(context);

    return 0U;
}

#elif POUND_PLATFORM_POSIX

/// The platform entry point. `pthread_create` takes an argument list C has no type for,
/// so the arguments travel as the one-pointer slot every thread starts with.
static void *
trampoline(void *raw)
{
    thread_arguments_t *const arguments = (thread_arguments_t *)raw;
    const thread_entry_t entry          = arguments->entry;
    void *const          context        = arguments->context;

    free(arguments);

    entry(context);

    return NULL;
}

#endif

error_t
thread_start(thread_t *POUND_RESTRICT thread, const thread_entry_t entry, void *context)
{
    if (POUND_UNLIKELY((NULL == thread) || (NULL == entry)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing to start a thread with a %s target.",
                        (NULL == thread) ? "NULL" : "NULL entry function");

        return POUND_ERROR_INVALID_ARGUMENT;
    }

#if POUND_PLATFORM_POSIX
    if (POUND_UNLIKELY(0 != thread->started))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing to start a thread that is already running. The running one is "
                        "still executing and nothing here can join it.");

        return POUND_ERROR_INVALID_ARGUMENT;
    }
#else
    if (POUND_UNLIKELY(NULL != thread->handle))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing to start a thread whose previous handle was never joined. The "
                        "previous one may still be running.");

        return POUND_ERROR_INVALID_ARGUMENT;
    }
#endif

    // One allocation rather than two, so the context and the entry pointer cannot be
    // published to a running thread separately.
    thread_arguments_t *const arguments = (thread_arguments_t *)malloc(sizeof(*arguments));

    if (POUND_UNLIKELY(NULL == arguments))
    {
        POUND_LOG_ERROR(&thread_logger, "Could not allocate a thread's arguments.");

        return POUND_ERROR_ALLOCATION_FAILED;
    }

    arguments->entry   = entry;
    arguments->context = context;

#if POUND_PLATFORM_WINDOWS
    uintptr_t const raw = _beginthreadex(NULL,
                                         0U,
                                         trampoline,
                                         arguments,
                                         0U,
                                         NULL);

    if (POUND_UNLIKELY(0U == raw))
    {
        free(arguments);

        POUND_LOG_ERROR(&thread_logger,
                        "The platform refused to create a thread (Windows error %lu).",
                        (unsigned long)GetLastError());

        return POUND_ERROR_ALLOCATION_FAILED;
    }

    // `_beginthreadex` returns a handle, not a `HANDLE`, and closing it is what makes
    // the thread's kernel object reclaimable. It stays open for `thread_join`.
    thread->handle = (HANDLE)raw;
#else
    if (POUND_UNLIKELY(0 != pthread_create(&thread->handle, NULL, trampoline, arguments)))
    {
        free(arguments);

        POUND_LOG_ERROR(&thread_logger, "The platform refused to create a thread.");

        return POUND_ERROR_ALLOCATION_FAILED;
    }

    thread->started = 1;
#endif

    POUND_LOG_DEBUG(&thread_logger, "Started a thread.");

    return POUND_SUCCESS;
}

error_t
thread_join(thread_t *POUND_RESTRICT thread)
{
    if (POUND_UNLIKELY(NULL == thread))
    {
        POUND_LOG_ERROR(&thread_logger, "Refusing to join a NULL thread.");

        return POUND_ERROR_INVALID_ARGUMENT;
    }

#if POUND_PLATFORM_WINDOWS
    if (POUND_UNLIKELY(NULL == thread->handle))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing to join a thread that was never started, or has already been "
                        "joined. Neither can be waited on, and the first means a thread may be "
                        "running that this object cannot account for.");

        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(WAIT_OBJECT_0 != WaitForSingleObject(thread->handle, INFINITE)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Waiting for a thread failed (Windows error %lu). It may still be running.",
                        (unsigned long)GetLastError());

        // The handle is deliberately *not* closed on this path. Losing it would lose the
        // only reference to a thread that is very likely still executing, and the caller
        // is better served by a second, failing attempt than by a silent leak.
        return POUND_ERROR_THREAD_FAILED;
    }

    CloseHandle(thread->handle);
    thread->handle = NULL;
#else
    if (POUND_UNLIKELY(0 == thread->started))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing to join a thread that was never started, or has already been "
                        "joined. Neither can be waited on, and the first means a thread may be "
                        "running that this object cannot account for.");

        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(0 != pthread_join(thread->handle, NULL)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Waiting for a thread failed. It may still be running.");

        return POUND_ERROR_THREAD_FAILED;
    }

    thread->started = 0;
#endif

    return POUND_SUCCESS;
}

/*** end of thread.c ***/
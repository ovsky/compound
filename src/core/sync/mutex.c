#include "mutex.h"
#include "log.h"
#include <stdlib.h>

error_t
mutex_init(mutex_t *POUND_RESTRICT mutex)
{
    if (POUND_UNLIKELY(NULL == mutex))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: mutex context is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

#if POUND_PLATFORM_WINDOWS

    // SRWLOCK requires no initialisation beyond the zero/INIT value, but the
    // struct may have been handed in holding garbage, so assign the documented
    // initial state rather than trusting the caller's storage.
    InitializeSRWLock(&mutex->lock);

#else

    // PTHREAD_MUTEX_INITIALIZER is the default, non-recursive, non-robust
    // mutex: the allocators never re-enter their own lock, and a robust mutex
    // would only add an ownership protocol nothing here implements.
    if (0 != pthread_mutex_init(&mutex->lock, NULL))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: pthread_mutex_init failed.");
        return POUND_ERROR_ALLOCATION_FAILED;
    }

#endif // POUND_PLATFORM_WINDOWS

    return POUND_SUCCESS;
}

void
mutex_destroy(mutex_t *POUND_RESTRICT mutex)
{
    if (POUND_UNLIKELY(NULL == mutex))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: mutex context is NULL.");
        return;
    }

#if POUND_PLATFORM_WINDOWS

    // No destroy operation exists for SRWLOCK; releasing the handle is all the
    // platform requires. Nothing to do.
    POUND_UNUSED(mutex);

#else

    const int error = pthread_mutex_destroy(&mutex->lock);

    if (0 != error)
    {
        // EBUSY here means the mutex was still locked. That is a caller bug, not
        // a recoverable condition, so it is reported loudly and the object is
        // left alone rather than destroyed anyway.
        POUND_LOG_ERROR(&thread_logger,
                        "pthread_mutex_destroy failed with %d; the mutex was likely still locked.",
                        error);
    }

#endif // POUND_PLATFORM_WINDOWS
}

void
mutex_lock(mutex_t *POUND_RESTRICT mutex)
{
    if (POUND_UNLIKELY(NULL == mutex))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: mutex context is NULL.");
        return;
    }

#if POUND_PLATFORM_WINDOWS

    AcquireSRWLockExclusive(&mutex->lock);

#else

    const int error = pthread_mutex_lock(&mutex->lock);

    if (0 != error)
    {
        // EINVAL for an uninitialised or already-destroyed mutex, EDEADLK for
        // a recursive acquire of a non-recursive mutex. Both indicate a caller
        // bug; there is nothing useful to fall back to, so the lock is simply
        // not held and the caller's invariants are already broken.
        POUND_LOG_ERROR(&thread_logger, "pthread_mutex_lock failed with %d.", error);
    }

#endif // POUND_PLATFORM_WINDOWS
}

void
mutex_unlock(mutex_t *POUND_RESTRICT mutex)
{
    if (POUND_UNLIKELY(NULL == mutex))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: mutex context is NULL.");
        return;
    }

#if POUND_PLATFORM_WINDOWS

    ReleaseSRWLockExclusive(&mutex->lock);

#else

    const int error = pthread_mutex_unlock(&mutex->lock);

    if (0 != error)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "pthread_mutex_unlock failed with %d; the mutex was not held by this thread.",
                        error);
    }

#endif // POUND_PLATFORM_WINDOWS
}

/*** end of file ***/
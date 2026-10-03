#include "log_ring.h"

#include "memory/memory.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/// The alignment every ring allocation uses.
///
/// `pound_log_record_t` contains a `uint64_t`, so 8 is what the type needs and
/// what the allocator will happily provide.
#define POUND_LOG_RING_ALIGNMENT 8U

/// The sink installed when the ring is the thread's callback.
///
/// Formats the record into the next slot and then hands the original arguments to
/// the remembered sink. The order matters: the copy taken here is consumed by
/// `vsnprintf`, so the caller's `va_list` has to be duplicated first and the
/// original passed along untouched.
static void
pound_log_ring_sink(void *POUND_RESTRICT user_data, log_data_t *POUND_RESTRICT data, const char *format, va_list args)
{
    pound_log_ring_t *ring = user_data;

    if ((NULL == ring) || (NULL == data) || (NULL == format))
    {
        return;
    }

    va_list copy;
    va_copy(copy, args);

    mutex_lock(&ring->lock);

    if (ring->initialized)
    {
        pound_log_record_t *const record = &ring->records[ring->next];

        record->sequence = ring->next_sequence;
        record->level    = data->level;

        const int written = vsnprintf(record->text, sizeof(record->text), format, copy);

        if (written < 0)
        {
            // `vsnprintf` reports a failure by a negative return and may have left
            // the buffer unspecified. Saying so in the slot keeps the failure in the
            // log it belongs to, rather than dropping the record and leaving a hole.
            (void)snprintf(record->text, sizeof(record->text), "(formatting this record failed)");
        }

        ring->next_sequence++;
        ring->next = (ring->next + 1U) % ring->capacity;

        if (ring->count < ring->capacity)
        {
            ring->count++;
        }
    }

    mutex_unlock(&ring->lock);

    va_end(copy);

    // Passed on after the lock is released: a sink is arbitrary caller code and a
    // sink that logs -- or that blocks -- must not be able to stall a ring write or
    // deadlock against it.
    if (NULL != ring->previous_log)
    {
        ring->previous_log(ring->previous_user_data, data, format, args);
    }
}

error_t
pound_log_ring_init(pound_log_ring_t *POUND_RESTRICT ring, const size_t capacity)
{
    if (POUND_UNLIKELY(NULL == ring))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: ring is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(0U == capacity))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: capacity is zero.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(capacity > (SIZE_MAX / sizeof(pound_log_record_t))))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: a capacity of %zu records would need more bytes "
                        "than a size can describe.",
                        capacity);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    memset(ring, 0, sizeof(*ring));

    pound_log_record_t *const records
        = memory_subsystem_allocate(POUND_LOG_RING_ALIGNMENT, capacity * sizeof(pound_log_record_t));

    if (POUND_UNLIKELY(NULL == records))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: could not allocate %zu record(s) of %zu bytes.",
                        capacity,
                        sizeof(pound_log_record_t));
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    memset(records, 0, capacity * sizeof(pound_log_record_t));

    if (POUND_SUCCESS != mutex_init(&ring->lock))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the ring's mutex could not be created.");
        memory_subsystem_free(records);
        return POUND_ERROR_THREAD_FAILED;
    }

    ring->records       = records;
    ring->capacity      = capacity;
    ring->next_sequence = 1U;
    ring->initialized   = true;
    return POUND_SUCCESS;
}

void
pound_log_ring_shutdown(pound_log_ring_t *POUND_RESTRICT ring)
{
    if (NULL == ring)
    {
        return;
    }

    if (false == ring->initialized)
    {
        return;
    }

    if (true == ring->installed)
    {
        pound_log_ring_uninstall(ring);
    }

    mutex_destroy(&ring->lock);
    memory_subsystem_free(ring->records);

    ring->records       = NULL;
    ring->capacity      = 0U;
    ring->count         = 0U;
    ring->next          = 0U;
    ring->next_sequence = 0U;
    ring->initialized   = false;
}

error_t
pound_log_ring_install(pound_log_ring_t *POUND_RESTRICT ring)
{
    if (POUND_UNLIKELY(NULL == ring))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: ring is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(false == ring->initialized))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the ring was never passed through pound_log_ring_init.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    if (POUND_UNLIKELY(true == ring->installed))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: this ring is already the log sink; installing it "
                        "again would make it call itself.");
        return POUND_ERROR_ALREADY_INITIALIZED;
    }

    ring->previous_log       = thread_logger.log;
    ring->previous_user_data = thread_logger.user_data;

    thread_logger.log       = pound_log_ring_sink;
    thread_logger.user_data = ring;
    ring->installed         = true;

    POUND_LOG_INFO(&thread_logger,
                   "Capturing the last %zu log record(s) for the Logs page; output still goes "
                   "to the previous sink.",
                   ring->capacity);
    return POUND_SUCCESS;
}

void
pound_log_ring_uninstall(pound_log_ring_t *POUND_RESTRICT ring)
{
    if (NULL == ring)
    {
        return;
    }

    if (false == ring->installed)
    {
        return;
    }

    // Only restored when this ring is still the callback. Somebody who installed a
    // sink on top of this one owns that decision, and restoring underneath them
    // would silently discard their sink.
    if (thread_logger.log != pound_log_ring_sink)
    {
        POUND_LOG_WARN(&thread_logger,
                       "Not restoring the previous log sink: another sink has been installed on "
                       "top of this ring.");
        ring->installed = false;
        return;
    }

    thread_logger.log       = ring->previous_log;
    thread_logger.user_data = ring->previous_user_data;

    ring->previous_log       = NULL;
    ring->previous_user_data = NULL;
    ring->installed          = false;
}

size_t
pound_log_ring_count(pound_log_ring_t *POUND_RESTRICT ring)
{
    if ((NULL == ring) || (false == ring->initialized))
    {
        return 0U;
    }

    mutex_lock(&ring->lock);
    const size_t count = ring->count;
    mutex_unlock(&ring->lock);

    return count;
}

uint64_t
pound_log_ring_newest_sequence(pound_log_ring_t *POUND_RESTRICT ring)
{
    if ((NULL == ring) || (false == ring->initialized))
    {
        return 0U;
    }

    mutex_lock(&ring->lock);
    const uint64_t sequence = (0U == ring->count) ? 0U : (ring->next_sequence - 1U);
    mutex_unlock(&ring->lock);

    return sequence;
}

void
pound_log_ring_clear(pound_log_ring_t *POUND_RESTRICT ring)
{
    if (NULL == ring)
    {
        return;
    }

    if (false == ring->initialized)
    {
        return;
    }

    mutex_lock(&ring->lock);
    ring->count = 0U;
    ring->next  = 0U;
    mutex_unlock(&ring->lock);
}

size_t
pound_log_ring_snapshot(pound_log_ring_t *POUND_RESTRICT ring,
                        pound_log_record_t *POUND_RESTRICT out,
                        const size_t capacity)
{
    if (NULL == ring)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: ring is NULL.");
        return 0U;
    }

    if (false == ring->initialized)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the ring was never passed through pound_log_ring_init.");
        return 0U;
    }

    if (0U == capacity)
    {
        return 0U;
    }

    if (POUND_UNLIKELY(NULL == out))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: out is NULL but capacity is %zu.", capacity);
        return 0U;
    }

    mutex_lock(&ring->lock);

    const size_t available = ring->count;
    const size_t wanted    = (available < capacity) ? available : capacity;

    // The newest `wanted` records, oldest first. `next` is where the write after
    // those will land, so the oldest of the records being copied sits `wanted`
    // slots behind it -- which is the same arithmetic whether or not the ring has
    // wrapped, because the modulo absorbs the difference.
    const size_t first = (ring->next + ring->capacity - wanted) % ring->capacity;

    for (size_t i = 0U; i < wanted; i++)
    {
        out[i] = ring->records[(first + i) % ring->capacity];
    }

    mutex_unlock(&ring->lock);

    return wanted;
}

void
pound_log_ring_log_summary(pound_log_ring_t *POUND_RESTRICT ring)
{
    if (NULL == ring)
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: ring is NULL.");
        return;
    }

    if (false == ring->initialized)
    {
        POUND_LOG_INFO(&thread_logger, "Log ring: not initialised.");
        return;
    }

    mutex_lock(&ring->lock);
    const size_t   count    = ring->count;
    const size_t   capacity = ring->capacity;
    const uint64_t newest   = (0U == count) ? 0U : (ring->next_sequence - 1U);
    mutex_unlock(&ring->lock);

    POUND_LOG_INFO(&thread_logger,
                   "Log ring: %zu of %zu record(s) held, newest sequence %llu, sink %s.",
                   count,
                   capacity,
                   (unsigned long long)newest,
                   (true == ring->installed) ? "installed" : "detached");
}

/*** end of file ***/
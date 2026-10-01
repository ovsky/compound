#include "memory.h"

#include "log.h"
#include "mimalloc-stats.h"
#include "mimalloc.h"
#include <stdint.h>
#include <stdlib.h>

struct memory_allocator
{
    void *(*allocate)(memory_allocator_t *POUND_RESTRICT allocator, size_t alignment, size_t bytes);
    void (*free)(memory_allocator_t *POUND_RESTRICT allocator, void *pointer);
    size_t (*get_usable_size)(const void *POUND_RESTRICT pointer);
    size_t (*get_heap_size)(void);
    size_t memory_used_by_bucket[MEMORY_BUCKET_COUNT];
    size_t total_memory_used;
};

static void  *host_allocate(memory_allocator_t *POUND_RESTRICT allocator,
                            size_t                             alignment,
                            size_t                             bytes);
static void   host_free(memory_allocator_t *POUND_RESTRICT allocator, void *pointer);
static size_t host_get_usable_size(const void *POUND_RESTRICT pointer);
static size_t host_get_heap_size(void);

memory_allocator_t g_host_allocator = {
    .allocate          = host_allocate,
    .free              = host_free,
    .get_usable_size   = host_get_usable_size,
    .get_heap_size     = host_get_heap_size,
    .memory_used_by_bucket = { 0 },
    .total_memory_used = 0,
};

POUND_THREAD_LOCAL memory_allocator_t  *tls_current_allocator    = &g_host_allocator;
POUND_THREAD_LOCAL memory_bucket_type_t tls_current_bucket_index = MEMORY_BUCKET_NONE;

/// Credits `bytes` to `bucket` and to the running total.
///
/// The counters are plain `size_t`s. Pound only mutates them from the thread
/// that owns the current bucket, so no synchronisation is required for the
/// single-threaded accounting model documented in `memory.h`.
static void
account_allocate(memory_allocator_t *POUND_RESTRICT allocator,
                 memory_bucket_type_t                       bucket,
                 const size_t                               bytes)
{
    allocator->memory_used_by_bucket[bucket] += bytes;
    allocator->total_memory_used += bytes;
}

/// Debits `bytes` from `bucket` and from the running total, saturating at zero.
///
/// Saturating rather than wrapping means a cross-bucket free degrades the
/// per-bucket breakdown but can never make the allocator report a nonsensical
/// value close to `SIZE_MAX`.
static void
account_free(memory_allocator_t *POUND_RESTRICT allocator,
             memory_bucket_type_t                       bucket,
             const size_t                               bytes)
{
    size_t *bucket_total = &allocator->memory_used_by_bucket[bucket];

    if (*bucket_total < bytes)
    {
        POUND_LOG_WARN(&thread_logger,
                       "Bucket %d underflowed while releasing %zu bytes; clamping to zero.",
                       (int)bucket,
                       bytes);
        *bucket_total = 0;
    }
    else
    {
        *bucket_total -= bytes;
    }

    if (allocator->total_memory_used < bytes)
    {
        POUND_LOG_WARN(&thread_logger,
                       "Total memory underflowed while releasing %zu bytes; clamping to zero.",
                       bytes);
        allocator->total_memory_used = 0;
    }
    else
    {
        allocator->total_memory_used -= bytes;
    }
}

/// Returns `true` when `alignment` is a non-zero power of two.
///
/// mimalloc's `mi_malloc_aligned` rejects other values, but validating here
/// keeps the failure attributable to the caller.
static bool
is_alignment_valid(const size_t alignment)
{
    return (alignment != 0U) && ((alignment & (alignment - 1U)) == 0U);
}

bool
memory_bucket_is_valid(const memory_bucket_type_t bucket)
{
    return (bucket >= MEMORY_BUCKET_NONE) && (bucket < MEMORY_BUCKET_COUNT);
}

void
memory_subsystem_init(void)
{
    tls_current_allocator    = &g_host_allocator;
    tls_current_bucket_index = MEMORY_BUCKET_NONE;
}

void
memory_subsystem_destroy(void)
{
    tls_current_allocator    = NULL;
    tls_current_bucket_index = MEMORY_BUCKET_NONE;
}

memory_allocator_t *
memory_subsystem_set_allocator(memory_allocator_t *POUND_RESTRICT allocator)
{
    memory_allocator_t *POUND_RESTRICT previous_allocator = tls_current_allocator;
    tls_current_allocator                                 = allocator;
    return previous_allocator;
}

memory_allocator_t *
memory_subsystem_get_allocator(void)
{
    return tls_current_allocator;
}

void *
memory_subsystem_allocate(const size_t alignment, const size_t bytes)
{
    if (POUND_UNLIKELY(tls_current_allocator == NULL))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: memory subsystem is not initialised on this thread.");
        return NULL;
    }

    if (POUND_UNLIKELY(false == is_alignment_valid(alignment)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: alignment %zu is not a non-zero power of two.",
                        alignment);
        return NULL;
    }

    if (POUND_UNLIKELY(bytes == 0U))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: requested size is zero.");
        return NULL;
    }

    return tls_current_allocator->allocate(tls_current_allocator, alignment, bytes);
}

void
memory_subsystem_free(void *POUND_RESTRICT pointer)
{
    if (NULL == pointer)
    {
        return;
    }

    if (POUND_UNLIKELY(tls_current_allocator == NULL))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Dropping pointer because the memory subsystem is not "
                        "initialised on this thread.");
        return;
    }

    tls_current_allocator->free(tls_current_allocator, pointer);
}

memory_bucket_type_t
memory_subsystem_set_bucket(const memory_bucket_type_t bucket)
{
    const memory_bucket_type_t old_bucket_index = tls_current_bucket_index;

    if (POUND_UNLIKELY(false == memory_bucket_is_valid(bucket)))
    {
        // `memory_bucket_type_t` declares no negative enumerator, so its
        // underlying type is unsigned and there is no sign left to inspect by
        // the time a value reaches here: casting a negative int to it yields a
        // value far *above* the range, not below it. Every out-of-range bucket
        // therefore clamps to the last real bucket, which keeps the index
        // inside `memory_used_by_bucket[]` either way.
        POUND_LOG_WARN(&thread_logger,
                       "Clamping out-of-range bucket %d into [0, %d].",
                       (int)bucket,
                       (int)MEMORY_BUCKET_COUNT - 1);
        tls_current_bucket_index = (memory_bucket_type_t)(MEMORY_BUCKET_COUNT - 1);
    }
    else
    {
        tls_current_bucket_index = bucket;
    }

    return old_bucket_index;
}

memory_bucket_type_t
memory_subsystem_get_bucket(void)
{
    return tls_current_bucket_index;
}

size_t
memory_subsystem_get_usable_size(const void *POUND_RESTRICT pointer)
{
    if (NULL == pointer)
    {
        return 0U;
    }

    if (POUND_UNLIKELY(tls_current_allocator == NULL))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: memory subsystem is not initialised on this thread.");
        return 0U;
    }

    const size_t usable_size = tls_current_allocator->get_usable_size(pointer);
    return usable_size;
}

size_t
memory_subsystem_get_memory_used_by_bucket(const int bucket)
{
    if (POUND_UNLIKELY(tls_current_allocator == NULL))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: memory subsystem is not initialised on this thread.");
        return 0U;
    }

    if (bucket < 0)
    {
        return tls_current_allocator->total_memory_used;
    }

    if ((bucket >= 0) && (bucket < (int)MEMORY_BUCKET_COUNT))
    {
        return tls_current_allocator->memory_used_by_bucket[bucket];
    }

    POUND_LOG_WARN(&thread_logger,
                   "Bucket %d is out of range [0, %d); returning 0.",
                   bucket,
                   (int)MEMORY_BUCKET_COUNT);
    return 0U;
}

size_t
memory_subsystem_get_heap_size(void)
{
    if (POUND_UNLIKELY(tls_current_allocator == NULL))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: memory subsystem is not initialised on this thread.");
        return 0U;
    }

    const size_t heap_size = tls_current_allocator->get_heap_size();
    return heap_size;
}

static void *
host_allocate(memory_allocator_t *POUND_RESTRICT allocator,
              const size_t                       alignment,
              const size_t                       bytes)
{
    void *POUND_RESTRICT pointer = mi_malloc_aligned(bytes, alignment);

    if (POUND_UNLIKELY(pointer == NULL))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "mi_malloc_aligned(%zu, %zu) failed.",
                        bytes,
                        alignment);
        return NULL;
    }

    const size_t              usable_size = mi_malloc_usable_size(pointer);
    const memory_bucket_type_t bucket      = tls_current_bucket_index;

    account_allocate(allocator, bucket, usable_size);
    return pointer;
}

static void
host_free(memory_allocator_t *POUND_RESTRICT allocator, void *pointer)
{
    if (POUND_UNLIKELY(NULL == pointer))
    {
        return;
    }

    const size_t              usable_size = mi_malloc_usable_size(pointer);
    const memory_bucket_type_t bucket      = tls_current_bucket_index;

    account_free(allocator, bucket, usable_size);
    mi_free(pointer);
}

static size_t
host_get_usable_size(const void *POUND_RESTRICT pointer)
{
    if (POUND_UNLIKELY(NULL == pointer))
    {
        return 0U;
    }

    const size_t usable_size = mi_usable_size(pointer);
    return usable_size;
}

static size_t
host_get_heap_size(void)
{
    mi_stats_t_decl(stats);
    mi_stats_get(&stats);
    return (size_t)stats.reserved.current;
}

/*** end of file ***/

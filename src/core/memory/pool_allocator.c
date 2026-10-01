#include "pool_allocator.h"
#include "log.h"
#include <string.h>

/// Rounds `value` up to the next multiple of `alignment`.
///
/// `alignment` is always a power of two, validated at init, so this is a mask
/// rather than a division. Returns false on overflow, which the callers treat
/// as a malformed configuration rather than wrapping silently.
static bool
align_up_checked(size_t value, size_t alignment, size_t *POUND_RESTRICT out)
{
    const size_t mask = alignment - 1U;

    if (value > (SIZE_MAX - mask))
    {
        return false;
    }

    *out = (value + mask) & ~mask;
    return true;
}

/// Returns the header that precedes `payload`.
static pool_block_t *
block_header(const pool_allocator_t *POUND_RESTRICT pool, const void *POUND_RESTRICT payload)
{
    return (pool_block_t *)(void *)((uint8_t *) payload - pool->header_stride);
}

/// Returns the payload that follows `header`.
static void *
block_payload(const pool_allocator_t *POUND_RESTRICT pool, const pool_block_t *POUND_RESTRICT header)
{
    return (void *)(uint8_t *)(uintptr_t) header + pool->header_stride;
}

/// Validates that `header` is a real block belonging to `pool`'s arena.
///
/// Used by every entry point that accepts a caller-supplied pointer, so that a
/// bad pointer produces a typed error and a log record instead of corrupting a
/// free list. `expected_magic` is the state the caller requires: `free` demands
/// `POOL_BLOCK_MAGIC_LIVE` (so a second free is rejected), while the read-only
/// queries accept either state.
///
/// On success writes the owning class into `out_class`.
static error_t
validate_block(const pool_allocator_t *POUND_RESTRICT pool,
               pool_block_t *POUND_RESTRICT              header,
               uint32_t                                  expected_magic,
               const char *POUND_RESTRICT                operation,
               pool_class_t *POUND_RESTRICT              out_class)
{
    const uintptr_t base    = (uintptr_t)pool->arena;
    const uintptr_t payload = (uintptr_t)block_payload(pool, header);

    if ((payload < base) || ((payload - base) >= pool->arena_size))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "%s refused: %p is not inside the arena [%p, %p).",
                        operation,
                        (const void *) payload,
                        (const void *)pool->arena,
                        (const void *)(base + pool->arena_size));
        return POUND_ERROR_CORRUPTED;
    }

    const bool magic_is_live = (POOL_BLOCK_MAGIC_LIVE == header->magic);
    const bool magic_is_free = (POOL_BLOCK_MAGIC_FREE == header->magic);

    if (!magic_is_live && !magic_is_free)
    {
        // Neither magic matches, so the block was never carved by this pool, or
        // the arena memory has been overwritten since. Pushing an unknown block
        // onto a free list is how a bad free becomes a wild write, so this is
        // reported and refused.
        POUND_LOG_ERROR(&thread_logger,
                        "%s refused: %p carries magic 0x%08X, which is neither live (0x%08X) nor "
                        "free (0x%08X); it was never allocated by this pool.",
                        operation,
                        (const void *) payload,
                        (unsigned int)header->magic,
                        (unsigned int)POOL_BLOCK_MAGIC_LIVE,
                        (unsigned int)POOL_BLOCK_MAGIC_FREE);
        return POUND_ERROR_DOUBLE_FREE;
    }

    if (expected_magic != header->magic)
    {
        // The block is real but in the wrong state. For `free` this is a double
        // free, which is the whole reason the caller asked for a specific magic.
        POUND_LOG_ERROR(&thread_logger,
                        "%s refused: %p is already free; this is a double free.",
                        operation,
                        (const void *) payload);
        return POUND_ERROR_DOUBLE_FREE;
    }

    if (header->class_index >= pool->class_count)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "%s refused: %p names class %u but the pool has %zu classes.",
                        operation,
                        (const void *) payload,
                        (unsigned int)header->class_index,
                        pool->class_count);
        return POUND_ERROR_CORRUPTED;
    }

    *out_class = pool->classes[header->class_index];
    return POUND_SUCCESS;
}

/// Fills `bytes` at `payload` with the poison pattern.
static void
poison_range(pool_allocator_t *POUND_RESTRICT pool, void *POUND_RESTRICT payload, size_t bytes)
{
    if (pool->poison_enabled)
    {
        memset(payload, (int)pool->poison_byte, bytes);
    }
}

error_t
pool_allocator_init(pool_allocator_t *POUND_RESTRICT pool,
                    void *POUND_RESTRICT           arena,
                    size_t                         arena_size,
                    const pool_config_t *POUND_RESTRICT config)
{
    if (POUND_UNLIKELY(NULL == pool))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: pool context is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(NULL == arena))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: arena is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(0U == arena_size))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: arena_size is zero.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(arena_size < sizeof(pool_block_t)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: an arena of %zu bytes cannot hold even one block header.",
                        arena_size);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    // Resolve the configuration, filling in defaults for every unset field so a
    // caller can pass a partially initialised struct.
    pool_config_t resolved = { 0 };
    resolved.alignment      = POOL_ALLOCATOR_DEFAULT_ALIGNMENT;

    if (NULL != config)
    {
        resolved = *config;
    }

    if (0U == resolved.alignment)
    {
        resolved.alignment = POOL_ALLOCATOR_DEFAULT_ALIGNMENT;
    }

    if (0U == resolved.growth_permille)
    {
        resolved.growth_permille = 1250U;
    }

    if (0U == resolved.min_payload)
    {
        resolved.min_payload = resolved.alignment;
    }

    if (0U == resolved.max_payload)
    {
        resolved.max_payload = 4096U;
    }

    if (0U != (resolved.alignment & (resolved.alignment - 1U)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: alignment %zu is not a power of two.",
                        resolved.alignment);
        return POUND_ERROR_MEMORY_ALIGNMENT;
    }

    if ((resolved.alignment < POUND_POOL_MIN_ALIGNMENT) || (resolved.alignment > POUND_POOL_MAX_ALIGNMENT))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: alignment %zu is outside the supported range [%d, %d].",
                        resolved.alignment,
                        (int)POUND_POOL_MIN_ALIGNMENT,
                        (int)POUND_POOL_MAX_ALIGNMENT);
        return POUND_ERROR_MEMORY_ALIGNMENT;
    }

    if (resolved.growth_permille <= 1000U)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: growth_permille %u must exceed 1000 so size classes "
                        "strictly increase.",
                        (unsigned int)resolved.growth_permille);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (resolved.min_payload > resolved.max_payload)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: min_payload %zu exceeds max_payload %zu.",
                        resolved.min_payload,
                        resolved.max_payload);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(0U != ((uintptr_t)arena % (uintptr_t)resolved.alignment)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: arena %p is not a multiple of the %zu-byte alignment.",
                        arena,
                        resolved.alignment);
        return POUND_ERROR_MEMORY_ALIGNMENT;
    }

    // Start from a zeroed structure so a failed init never leaves the caller
    // with a pool whose counters disagree with its arena.
    memset(pool, 0, sizeof(*pool));
    pool->arena          = (uint8_t *)arena;
    pool->arena_size     = arena_size;
    pool->alignment      = resolved.alignment;
    pool->poison_enabled = resolved.poison;
    pool->poison_byte    = (uint8_t)POOL_ALLOCATOR_POISON_BYTE;

    size_t header_stride = 0U;

    if (POUND_UNLIKELY(!align_up_checked(sizeof(pool_block_t), resolved.alignment, &header_stride)))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: block header alignment overflowed.");
        return POUND_ERROR_MEMORY_ALIGNMENT;
    }

    pool->header_stride = header_stride;

    // Build the class table, then carve the arena class by class from smallest
    // to largest. Greedy small-first is deliberate: it guarantees the classes a
    // guest allocates from constantly are always present, and a large
    // allocation failing is a much cheaper failure than small ones.
    size_t payload = resolved.min_payload;

    while (payload <= resolved.max_payload)
    {
        if (POUND_UNLIKELY(pool->class_count >= POOL_ALLOCATOR_MAX_CLASSES))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "Aborting function: configuration needs more than %d size classes.",
                            (int)POOL_ALLOCATOR_MAX_CLASSES);
            return POUND_ERROR_INVALID_ARGUMENT;
        }

        pool_class_t *const pool_class = &pool->classes[pool->class_count];

        size_t stride = 0U;

        if (POUND_UNLIKELY(!align_up_checked(header_stride + payload, resolved.alignment, &stride)))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "Aborting function: stride for a %zu-byte payload overflowed.",
                            payload);
            return POUND_ERROR_ALLOCATION_FAILED;
        }

        pool_class->payload_size = payload;
        pool_class->stride       = stride;
        pool_class->free_head    = NULL;
        pool_class->used         = 0U;
        pool_class->peak_used    = 0U;
        pool_class->capacity     = 0U;
        pool_class->allocations  = 0U;
        pool_class->frees        = 0U;

        pool->class_count++;

        // Advance to the next class: grow by growth_permille, then round up to
        // the alignment. A class whose growth would jump straight past
        // max_payload ends the table, so the largest class never overshoots the
        // caller's ceiling by more than one alignment quantum.
        const size_t previous = payload;
        const size_t grown    = previous + (((previous * (size_t)(resolved.growth_permille - 1000U))
                                          + 999U) / 1000U);

        size_t next = 0U;

        if (POUND_UNLIKELY(!align_up_checked(grown, resolved.alignment, &next)))
        {
            POUND_LOG_ERROR(&thread_logger, "Aborting function: size class growth overflowed.");
            return POUND_ERROR_ALLOCATION_FAILED;
        }

        if (next <= previous)
        {
            // Rounding to the alignment can collapse a small growth onto the
            // same size; step by one alignment unit to guarantee progress.
            if (POUND_UNLIKELY(!align_up_checked(previous + resolved.alignment,
                                                 resolved.alignment,
                                                 &next)))
            {
                POUND_LOG_ERROR(&thread_logger, "Aborting function: size class growth overflowed.");
                return POUND_ERROR_ALLOCATION_FAILED;
            }
        }

        payload = next;
    }

    // Carve. A class that cannot fit even one block simply ends up with no
    // capacity, which `alloc` reports as a failure rather than as corruption.
    for (size_t i = 0U; i < pool->class_count; ++i)
    {
        pool_class_t *const pool_class = &pool->classes[i];

        while ((pool->arena_size - pool->bump) >= pool_class->stride)
        {
            pool_block_t *const block = (pool_block_t *)(void *)(pool->arena + pool->bump);

            block->magic       = POOL_BLOCK_MAGIC_FREE;
            block->class_index = (uint32_t)i;
            block->next        = pool_class->free_head;

            pool_class->free_head = block;
            pool_class->capacity++;

            pool->bump += pool_class->stride;
        }
    }

    if (POUND_UNLIKELY(0U == pool->classes[0].capacity))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: a %zu-byte arena cannot hold one %zu-byte block.",
                        arena_size,
                        pool->classes[0].stride);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    if (POUND_UNLIKELY(POUND_SUCCESS != mutex_init(&pool->lock)))
    {
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    POUND_LOG_INFO(&thread_logger,
                   "Initialised a pool allocator: %zu classes, %zu bytes of arena, %zu committed.",
                   pool->class_count,
                   arena_size,
                   pool->bump);
    return POUND_SUCCESS;
}

void
pool_allocator_reset(pool_allocator_t *POUND_RESTRICT pool)
{
    if (POUND_UNLIKELY(NULL == pool))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: pool context is NULL.");
        return;
    }

    if (NULL == pool->arena)
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: pool was never initialised.");
        return;
    }

    mutex_lock(&pool->lock);

    pool->bump              = 0U;
    pool->bytes_in_use      = 0U;
    pool->peak_bytes_in_use = 0U;
    pool->total_requests    = 0U;
    pool->total_failures    = 0U;
    pool->arena_exhaustions = 0U;
    pool->total_allocations = 0U;
    pool->total_frees       = 0U;

    // Re-carve without re-deriving the class table: the sizes are unchanged, so
    // only the free lists and per-class counters need rebuilding.
    for (size_t i = 0U; i < pool->class_count; ++i)
    {
        pool_class_t *const pool_class = &pool->classes[i];

        pool_class->free_head  = NULL;
        pool_class->used       = 0U;
        pool_class->peak_used  = 0U;
        pool_class->capacity   = 0U;
        pool_class->allocations = 0U;
        pool_class->frees      = 0U;

        while ((pool->arena_size - pool->bump) >= pool_class->stride)
        {
            pool_block_t *const block = (pool_block_t *)(void *)(pool->arena + pool->bump);

            block->magic       = POOL_BLOCK_MAGIC_FREE;
            block->class_index = (uint32_t)i;
            block->next        = pool_class->free_head;

            pool_class->free_head = block;
            pool_class->capacity++;

            pool->bump += pool_class->stride;
        }
    }

    mutex_unlock(&pool->lock);
}

void
pool_allocator_destroy(pool_allocator_t *POUND_RESTRICT pool)
{
    if (POUND_UNLIKELY(NULL == pool))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: pool context is NULL.");
        return;
    }

    if (NULL == pool->arena)
    {
        return;
    }

    if (0U != pool->total_allocations - pool->total_frees)
    {
        POUND_LOG_WARN(&thread_logger,
                       "Destroying a pool with %llu bytes still live across %zu classes.",
                       (unsigned long long)pool->bytes_in_use,
                       pool->class_count);
    }

    mutex_destroy(&pool->lock);
    memset(pool, 0, sizeof(*pool));
}

size_t
pool_allocator_class_for_size(const pool_allocator_t *POUND_RESTRICT pool, size_t bytes)
{
    if (POUND_UNLIKELY(NULL == pool))
    {
        return 0U;
    }

    for (size_t i = 0U; i < pool->class_count; ++i)
    {
        if (bytes <= pool->classes[i].payload_size)
        {
            return i;
        }
    }

    return pool->class_count;
}

void *
pool_allocator_alloc(pool_allocator_t *POUND_RESTRICT pool, size_t bytes, size_t alignment)
{
    if (POUND_UNLIKELY(NULL == pool))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: pool context is NULL.");
        return NULL;
    }

    if (POUND_UNLIKELY(NULL == pool->arena))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: pool was never initialised.");
        return NULL;
    }

    if (POUND_UNLIKELY(alignment > pool->alignment))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: requested %zu-byte alignment but the pool guarantees %zu. "
                        "Raise the pool's alignment at init rather than rounding a pointer down.",
                        alignment,
                        pool->alignment);
        return NULL;
    }

    // A zero-byte request selects the smallest class. Callers that treat this as
    // "give me a unique address" get one; callers that compute `bytes + 1` by
    // hand get no wasted block.
    const size_t wanted = (0U == bytes) ? 1U : bytes;

    mutex_lock(&pool->lock);

    pool->total_requests++;

    const size_t index = pool_allocator_class_for_size(pool, wanted);

    if (index >= pool->class_count)
    {
        pool->total_failures++;

        POUND_LOG_ERROR(&thread_logger,
                        "Allocation of %zu bytes refused: the largest size class holds %zu bytes.",
                        wanted,
                        pool->classes[pool->class_count - 1U].payload_size);

        mutex_unlock(&pool->lock);
        return NULL;
    }

    pool_class_t *const pool_class = &pool->classes[index];

    if (POUND_UNLIKELY(NULL == pool_class->free_head))
    {
        pool->total_failures++;
        pool->arena_exhaustions++;

        POUND_LOG_ERROR(&thread_logger,
                        "Allocation of %zu bytes refused: class %zu is exhausted with %zu blocks live.",
                        wanted,
                        index,
                        pool_class->used);

        mutex_unlock(&pool->lock);
        return NULL;
    }

    pool_block_t *const block    = pool_class->free_head;
    pool_class->free_head        = block->next;
    pool_class->used++;
    pool_class->allocations++;

    if (pool_class->used > pool_class->peak_used)
    {
        pool_class->peak_used = pool_class->used;
    }

    // Mark the block live. Its magic now distinguishes it from a free block,
    // which is what lets `free` reject a double free and lets the read-only
    // queries resolve a live pointer.
    block->magic = POOL_BLOCK_MAGIC_LIVE;
    block->next  = NULL;

    pool->total_allocations++;
    pool->bytes_in_use += pool_class->payload_size;

    if (pool->bytes_in_use > pool->peak_bytes_in_use)
    {
        pool->peak_bytes_in_use = pool->bytes_in_use;
    }

    void *const payload = block_payload(pool, block);

    poison_range(pool, payload, pool_class->payload_size);

    mutex_unlock(&pool->lock);
    return payload;
}

void
pool_allocator_free(pool_allocator_t *POUND_RESTRICT pool, void *POUND_RESTRICT payload)
{
    if (POUND_UNLIKELY(NULL == pool))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring function: pool context is NULL.");
        return;
    }

    if (POUND_UNLIKELY(NULL == payload))
    {
        // Matches free(NULL), which every allocator is required to treat as a
        // no-op rather than an error.
        return;
    }

    if (POUND_UNLIKELY(NULL == pool->arena))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: pool was never initialised.");
        return;
    }

    mutex_lock(&pool->lock);

    pool_block_t *const block  = block_header(pool, payload);
    pool_class_t  validated = { 0 };
    const error_t  validation
        = validate_block(pool, block, POOL_BLOCK_MAGIC_LIVE, "free", &validated);

    if (POUND_UNLIKELY(POUND_SUCCESS != validation))
    {
        pool->total_failures++;
        mutex_unlock(&pool->lock);
        return;
    }

    // `validate_block` succeeded, so the class index is in range; re-derive the
    // class from the pool rather than trusting the copied struct.
    pool_class_t *const pool_class = &pool->classes[block->class_index];

    if (POUND_UNLIKELY(0U == pool_class->used))
    {
        // The magic and the arena bounds both checked out but the class claims
        // nothing is live. That means the counters and the free list have
        // diverged, which is unrecoverable without tearing the pool down.
        POUND_LOG_ERROR(&thread_logger,
                        "free refused: %p validates but class %u reports zero live blocks.",
                        payload,
                        (unsigned int)block->class_index);
        pool->total_failures++;
        mutex_unlock(&pool->lock);
        return;
    }

    pool_class->used--;
    pool_class->frees++;

    block->magic = POOL_BLOCK_MAGIC_FREE;
    block->next  = pool_class->free_head;

    pool_class->free_head = block;

    pool->total_frees++;
    pool->bytes_in_use -= pool_class->payload_size;

    poison_range(pool, payload, pool_class->payload_size);

    mutex_unlock(&pool->lock);
}

size_t
pool_allocator_get_usable_size(const pool_allocator_t *POUND_RESTRICT pool,
                               const void *POUND_RESTRICT       payload)
{
    if (POUND_UNLIKELY(NULL == pool) || (POUND_UNLIKELY(NULL == payload)))
    {
        return 0U;
    }

    if (NULL == pool->arena)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: pool was never initialised.");
        return 0U;
    }

    mutex_t *const lock = (mutex_t *)(uintptr_t)(void *)&pool->lock;

    mutex_lock(lock);

    pool_block_t *const block    = block_header(pool, payload);
    pool_class_t  validated     = { 0 };

    // Either state is acceptable here: the caller is asking how much room this
    // block has, which does not depend on whether it is currently handed out.
    // A block that matches neither magic was never ours and resolves to 0.
    const bool live = (POOL_BLOCK_MAGIC_LIVE == block->magic);
    const bool free = (POOL_BLOCK_MAGIC_FREE == block->magic);

    size_t result = 0U;

    if (live || free)
    {
        const error_t validation = validate_block(pool,
                                                  block,
                                                  live ? POOL_BLOCK_MAGIC_LIVE
                                                       : POOL_BLOCK_MAGIC_FREE,
                                                  "get_usable_size",
                                                  &validated);

        if (POUND_SUCCESS == validation)
        {
            result = validated.payload_size;
        }
    }
    else
    {
        validate_block(pool, block, POOL_BLOCK_MAGIC_LIVE, "get_usable_size", &validated);
    }

    mutex_unlock(lock);
    return result;
}

error_t
pool_allocator_get_stats(const pool_allocator_t *POUND_RESTRICT pool,
                         pool_stats_t *POUND_RESTRICT          out)
{
    if (POUND_UNLIKELY(NULL == pool))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: pool context is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(NULL == out))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: out is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(NULL == pool->arena))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: pool was never initialised.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    mutex_t *const lock = (mutex_t *)(uintptr_t)(void *)&pool->lock;

    mutex_lock(lock);

    out->arena_size         = pool->arena_size;
    out->arena_committed    = pool->bump;
    out->bytes_in_use       = pool->bytes_in_use;
    out->peak_bytes_in_use  = pool->peak_bytes_in_use;
    out->class_count        = pool->class_count;
    out->total_requests     = pool->total_requests;
    out->total_failures     = pool->total_failures;
    out->arena_exhaustions  = pool->arena_exhaustions;
    out->total_allocations  = pool->total_allocations;
    out->total_frees        = pool->total_frees;

    mutex_unlock(lock);
    return POUND_SUCCESS;
}

size_t
pool_allocator_leak_bytes(const pool_allocator_t *POUND_RESTRICT pool)
{
    if (POUND_UNLIKELY(NULL == pool) || (POUND_UNLIKELY(NULL == pool->arena)))
    {
        return 0U;
    }

    mutex_t *const lock = (mutex_t *)(uintptr_t)(void *)&pool->lock;

    mutex_lock(lock);
    const size_t live = pool->bytes_in_use;
    mutex_unlock(lock);

    return live;
}

void
pool_allocator_set_poison(pool_allocator_t *POUND_RESTRICT pool, bool enabled)
{
    if (POUND_UNLIKELY(NULL == pool))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: pool context is NULL.");
        return;
    }

    mutex_lock(&pool->lock);
    pool->poison_enabled = enabled;
    mutex_unlock(&pool->lock);
}

/*** end of file ***/
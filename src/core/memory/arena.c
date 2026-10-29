#include "arena.h"

#include "log.h"
#include "validation.h"

#include <string.h>

/// Returns true when `arena` is safe to dereference as an initialised arena.
static bool
arena_is_live(const pound_arena_t *POUND_RESTRICT arena)
{
    return (arena != NULL) && (arena->magic == POUND_ARENA_MAGIC) && (arena->base != NULL)
        && (arena->capacity > 0U);
}

error_t
pound_arena_init(pound_arena_t *POUND_RESTRICT arena,
                 void *POUND_RESTRICT          buffer,
                 const size_t                  size,
                 const size_t                  alignment,
                 const uint64_t                owner)
{
    if ((NULL == arena) || (NULL == buffer) || (0U == size))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting arena init: arena=%p, buffer=%p, size=%zu.",
                        (const void *)arena,
                        (const void *)buffer,
                        size);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (false == validation_is_power_of_two(alignment))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting arena init: alignment %zu is not a power of two.",
                        alignment);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (((uintptr_t)buffer & (uintptr_t)(alignment - 1U)) != 0U)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting arena init: buffer %p is not aligned to %zu bytes.",
                        (const void *)buffer,
                        alignment);
        return POUND_ERROR_MEMORY_ALIGNMENT;
    }

    memset(arena, 0, sizeof(*arena));
    arena->base              = (uint8_t *)buffer;
    arena->capacity          = size;
    arena->owner             = owner;
    arena->default_alignment = alignment;
    arena->generation        = 1U;
    arena->magic             = POUND_ARENA_MAGIC;
    return POUND_SUCCESS;
}

error_t
pound_arena_reset(pound_arena_t *POUND_RESTRICT arena)
{
    if (NULL == arena)
    {
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (false == arena_is_live(arena))
    {
        return POUND_ERROR_NOT_INITIALIZED;
    }

    if (arena->used > 0U)
    {
        memset(arena->base, 0, arena->used);
    }

    arena->used              = 0U;
    arena->allocation_count  = 0U;
    arena->failed_allocations = 0U;
    arena->generation++;
    return POUND_SUCCESS;
}

error_t
pound_arena_allocate(pound_arena_t *POUND_RESTRICT arena,
                     const size_t                  size,
                     const size_t                  alignment,
                     void **POUND_RESTRICT         out)
{
    if (NULL == out)
    {
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    *out = NULL;

    if (NULL == arena)
    {
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (false == arena_is_live(arena))
    {
        return POUND_ERROR_NOT_INITIALIZED;
    }

    if (0U == size)
    {
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const size_t requested = (0U == alignment) ? arena->default_alignment : alignment;

    if (false == validation_is_power_of_two(requested))
    {
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if ((requested < arena->default_alignment)
        || ((requested % arena->default_alignment) != 0U))
    {
        return POUND_ERROR_MEMORY_ALIGNMENT;
    }

    // Align the absolute address, not just the offset: the backing buffer is
    // guaranteed to satisfy the default alignment, not every stricter alignment
    // a later caller may request.
    const size_t base_address = (size_t)(uintptr_t)arena->base;
    size_t       unaligned    = 0U;

    if (__builtin_add_overflow(base_address, arena->used, &unaligned))
    {
        arena->failed_allocations++;
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    size_t aligned = 0U;

    if (POUND_SUCCESS != validation_align_up_size(unaligned, requested, &aligned))
    {
        arena->failed_allocations++;
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    const size_t start = aligned - base_address;
    size_t       end   = 0U;

    if ((POUND_SUCCESS != validation_add_size(start, size, &end)) || (end > arena->capacity))
    {
        arena->failed_allocations++;
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    uint8_t *const block = arena->base + start;
    memset(block, 0, size);

    arena->used = end;
    if (end > arena->high_water)
    {
        arena->high_water = end;
    }
    arena->allocation_count++;
    *out = block;
    return POUND_SUCCESS;
}

pound_arena_checkpoint_t
pound_arena_checkpoint(const pound_arena_t *POUND_RESTRICT arena)
{
    pound_arena_checkpoint_t checkpoint = { 0U, 0U, 0U };

    if (false == arena_is_live(arena))
    {
        return checkpoint;
    }

    checkpoint.used             = arena->used;
    checkpoint.allocation_count = arena->allocation_count;
    checkpoint.generation       = arena->generation;
    return checkpoint;
}

error_t
pound_arena_rewind(pound_arena_t *POUND_RESTRICT               arena,
                   const pound_arena_checkpoint_t checkpoint)
{
    if (NULL == arena)
    {
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (false == arena_is_live(arena))
    {
        return POUND_ERROR_NOT_INITIALIZED;
    }

    if ((checkpoint.generation != arena->generation) || (checkpoint.used > arena->used)
        || (checkpoint.allocation_count > arena->allocation_count))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting arena rewind: checkpoint {%zu, %u, gen %u} is not in the "
                        "live scope {%zu, %u, gen %u}.",
                        checkpoint.used,
                        (unsigned int)checkpoint.allocation_count,
                        (unsigned int)checkpoint.generation,
                        arena->used,
                        (unsigned int)arena->allocation_count,
                        (unsigned int)arena->generation);
        return POUND_ERROR_CORRUPTED;
    }

    if (checkpoint.used < arena->used)
    {
        memset(arena->base + checkpoint.used, 0, arena->used - checkpoint.used);
    }

    arena->used             = checkpoint.used;
    arena->allocation_count = checkpoint.allocation_count;
    return POUND_SUCCESS;
}

size_t
pound_arena_remaining(const pound_arena_t *POUND_RESTRICT arena)
{
    if (false == arena_is_live(arena))
    {
        return 0U;
    }

    return arena->capacity - arena->used;
}

size_t
pound_arena_used(const pound_arena_t *POUND_RESTRICT arena)
{
    return arena_is_live(arena) ? arena->used : 0U;
}

size_t
pound_arena_high_water(const pound_arena_t *POUND_RESTRICT arena)
{
    return arena_is_live(arena) ? arena->high_water : 0U;
}

uint64_t
pound_arena_owner(const pound_arena_t *POUND_RESTRICT arena)
{
    return arena_is_live(arena) ? arena->owner : 0U;
}

bool
pound_arena_is_initialized(const pound_arena_t *POUND_RESTRICT arena)
{
    return arena_is_live(arena);
}

/*** end of file ***/

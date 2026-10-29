//! Unit tests for `memory/arena.h`.
//!
//! The arena is the loader's allocation strategy: it carves one backing buffer
//! into segments that live and die with a title. The tests below check the
//! properties a loader depends on -- alignment, zeroing, overflow refusal, and
//! a rewind that cannot move the cursor outside the live scope -- rather than
//! the free-list behaviour of the pool and slab allocators.

#include "pound_test.h"

#include "memory/arena.h"
#include <string.h>

POUND_ALIGNED(64) static uint8_t arena_buffer[512];

POUND_TEST(arena, init_rejects_invalid_arguments)
{
    pound_arena_t arena;

    POUND_CHECK_EQ_I64(pound_arena_init(NULL, arena_buffer, sizeof(arena_buffer), 16U, 1U),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(pound_arena_init(&arena, NULL, sizeof(arena_buffer), 16U, 1U),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(pound_arena_init(&arena, arena_buffer, 0U, 16U, 1U),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(pound_arena_init(&arena, arena_buffer, sizeof(arena_buffer), 3U, 1U),
                       POUND_ERROR_INVALID_ARGUMENT);
}

POUND_TEST(arena, init_rejects_a_misaligned_buffer)
{
    pound_arena_t arena;

    POUND_CHECK_EQ_I64(pound_arena_init(&arena, arena_buffer + 1, 128U, 64U, 1U),
                       POUND_ERROR_MEMORY_ALIGNMENT);
}

POUND_TEST(arena, init_records_owner_and_capacity)
{
    pound_arena_t arena;

    POUND_REQUIRE(POUND_SUCCESS
                  == pound_arena_init(&arena, arena_buffer, sizeof(arena_buffer), 16U, 0x1234U));
    POUND_CHECK(pound_arena_is_initialized(&arena));
    POUND_CHECK_EQ_U64(pound_arena_owner(&arena), 0x1234U);
    POUND_CHECK_EQ_U64(pound_arena_used(&arena), 0U);
    POUND_CHECK_EQ_U64(pound_arena_remaining(&arena), sizeof(arena_buffer));
}

POUND_TEST(arena, allocate_aligns_and_zeroes_each_block)
{
    pound_arena_t arena;

    POUND_REQUIRE(POUND_SUCCESS
                  == pound_arena_init(&arena, arena_buffer, sizeof(arena_buffer), 16U, 1U));

    // Poison the backing store so a non-zeroed allocation is detectable.
    memset(arena_buffer, 0xA5, sizeof(arena_buffer));

    void *first = NULL;
    void *second = NULL;

    POUND_REQUIRE(POUND_SUCCESS == pound_arena_allocate(&arena, 3U, 64U, &first));
    POUND_REQUIRE(POUND_SUCCESS == pound_arena_allocate(&arena, 8U, 16U, &second));

    POUND_CHECK_PTR_EQ(first, arena_buffer);
    POUND_CHECK_EQ_U64(((uintptr_t)second % 16U), 0U);
    POUND_CHECK((uint8_t *)second >= (uint8_t *)first + 3);

    for (size_t i = 0; i < 3U; ++i)
    {
        POUND_CHECK_EQ_U64(((uint8_t *)first)[i], 0U);
    }

    for (size_t i = 0; i < 8U; ++i)
    {
        POUND_CHECK_EQ_U64(((uint8_t *)second)[i], 0U);
    }
}

POUND_TEST(arena, allocate_rejects_bad_sizes_and_alignments)
{
    pound_arena_t arena;

    POUND_REQUIRE(POUND_SUCCESS
                  == pound_arena_init(&arena, arena_buffer, sizeof(arena_buffer), 16U, 1U));

    void *out = (void *)1;

    POUND_CHECK_EQ_I64(pound_arena_allocate(&arena, 0U, 16U, &out),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK(out == NULL);

    POUND_CHECK_EQ_I64(pound_arena_allocate(&arena, 8U, 3U, &out), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK(out == NULL);

    POUND_CHECK_EQ_I64(pound_arena_allocate(&arena, 8U, 8U, &out),
                       POUND_ERROR_MEMORY_ALIGNMENT);
    POUND_CHECK(out == NULL);
}

POUND_TEST(arena, allocation_that_does_not_fit_is_refused_and_counted)
{
    pound_arena_t arena;

    POUND_REQUIRE(POUND_SUCCESS
                  == pound_arena_init(&arena, arena_buffer, 64U, 16U, 1U));

    void *out = NULL;

    POUND_CHECK_EQ_I64(pound_arena_allocate(&arena, 65U, 16U, &out),
                       POUND_ERROR_ALLOCATION_FAILED);
    POUND_CHECK(out == NULL);
    POUND_CHECK_EQ_U64(arena.failed_allocations, 1U);
    POUND_CHECK_EQ_U64(pound_arena_used(&arena), 0U);
}

POUND_TEST(arena, reset_zeroes_the_live_scope_and_keeps_high_water)
{
    pound_arena_t arena;

    POUND_REQUIRE(POUND_SUCCESS
                  == pound_arena_init(&arena, arena_buffer, sizeof(arena_buffer), 16U, 1U));

    void *block = NULL;

    POUND_REQUIRE(POUND_SUCCESS == pound_arena_allocate(&arena, 32U, 16U, &block));
    memset(block, 0x5A, 32U);
    POUND_CHECK_EQ_U64(pound_arena_high_water(&arena), 32U);

    POUND_REQUIRE(POUND_SUCCESS == pound_arena_reset(&arena));

    POUND_CHECK_EQ_U64(pound_arena_used(&arena), 0U);
    POUND_CHECK_EQ_U64(pound_arena_high_water(&arena), 32U);

    for (size_t i = 0; i < 32U; ++i)
    {
        POUND_CHECK_EQ_U64(arena_buffer[i], 0U);
    }
}

POUND_TEST(arena, rewind_releases_only_the_later_scope)
{
    pound_arena_t arena;

    POUND_REQUIRE(POUND_SUCCESS
                  == pound_arena_init(&arena, arena_buffer, sizeof(arena_buffer), 16U, 1U));

    void *first = NULL;
    POUND_REQUIRE(POUND_SUCCESS == pound_arena_allocate(&arena, 16U, 16U, &first));
    memset(first, 0x11, 16U);

    const pound_arena_checkpoint_t checkpoint = pound_arena_checkpoint(&arena);

    void *second = NULL;
    POUND_REQUIRE(POUND_SUCCESS == pound_arena_allocate(&arena, 16U, 16U, &second));
    memset(second, 0x22, 16U);

    POUND_REQUIRE(POUND_SUCCESS == pound_arena_rewind(&arena, checkpoint));
    POUND_CHECK_EQ_U64(pound_arena_used(&arena), 16U);
    POUND_CHECK_EQ_U64(arena.allocation_count, 1U);

    for (size_t i = 0; i < 16U; ++i)
    {
        POUND_CHECK_EQ_U64(arena_buffer[i], 0x11U);
    }

    for (size_t i = 16U; i < 32U; ++i)
    {
        POUND_CHECK_EQ_U64(arena_buffer[i], 0U);
    }
}

POUND_TEST(arena, a_checkpoint_from_a_previous_generation_is_refused)
{
    pound_arena_t arena;

    POUND_REQUIRE(POUND_SUCCESS
                  == pound_arena_init(&arena, arena_buffer, sizeof(arena_buffer), 16U, 1U));

    const pound_arena_checkpoint_t stale = pound_arena_checkpoint(&arena);

    POUND_REQUIRE(POUND_SUCCESS == pound_arena_reset(&arena));
    POUND_CHECK_EQ_I64(pound_arena_rewind(&arena, stale), POUND_ERROR_CORRUPTED);
}

POUND_TEST(arena, a_rewind_past_the_cursor_is_refused)
{
    pound_arena_t arena;

    POUND_REQUIRE(POUND_SUCCESS
                  == pound_arena_init(&arena, arena_buffer, sizeof(arena_buffer), 16U, 1U));

    void *first = NULL;
    POUND_REQUIRE(POUND_SUCCESS == pound_arena_allocate(&arena, 16U, 16U, &first));

    const pound_arena_checkpoint_t checkpoint = pound_arena_checkpoint(&arena);

    POUND_REQUIRE(POUND_SUCCESS == pound_arena_allocate(&arena, 16U, 16U, &first));
    (void)pound_arena_rewind(&arena, checkpoint);

    // Fabricating a checkpoint ahead of the live cursor must not move it
    // forward: that would hand the same bytes to two owners at once.
    pound_arena_checkpoint_t forged = checkpoint;
    forged.used                      = 512U;

    POUND_CHECK_EQ_I64(pound_arena_rewind(&arena, forged), POUND_ERROR_CORRUPTED);
    POUND_CHECK_EQ_U64(pound_arena_used(&arena), 16U);
}

POUND_TEST_SUITE(arena,
                 POUND_TEST_CASE(arena, init_rejects_invalid_arguments),
                 POUND_TEST_CASE(arena, init_rejects_a_misaligned_buffer),
                 POUND_TEST_CASE(arena, init_records_owner_and_capacity),
                 POUND_TEST_CASE(arena, allocate_aligns_and_zeroes_each_block),
                 POUND_TEST_CASE(arena, allocate_rejects_bad_sizes_and_alignments),
                 POUND_TEST_CASE(arena, allocation_that_does_not_fit_is_refused_and_counted),
                 POUND_TEST_CASE(arena, reset_zeroes_the_live_scope_and_keeps_high_water),
                 POUND_TEST_CASE(arena, rewind_releases_only_the_later_scope),
                 POUND_TEST_CASE(arena, a_checkpoint_from_a_previous_generation_is_refused),
                 POUND_TEST_CASE(arena, a_rewind_past_the_cursor_is_refused));

/*** end of file ***/

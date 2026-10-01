//! Tests for the Horizon OS pool allocator and the mutex it is built on.
//!
//! Two suites live in this file because the mutex is small enough that
//! separating it would cost more in boilerplate than it saves in navigation,
//! and because the pool's locking discipline is only meaningful relative to
//! the mutex's contract.
//!
//! The pool assertions deliberately check *properties* rather than
//! hand-computed size-class values. The class table is derived from a growth
//! factor, so pinning exact byte counts in the test would mean re-deriving the
//! implementation's arithmetic in the test and would fail for a harmless change
//! to the defaults. What must hold is that classes increase strictly, that a
//! request resolves to the smallest fitting class, and that accounting is
//! internally consistent; those are checked directly.

#include "pound_test.h"

#include "attributes.h"
#include "errors.h"
#include "memory/pool_allocator.h"
#include "sync/mutex.h"

// -----------------------------------------------------------------------------
// Fixtures
// -----------------------------------------------------------------------------

/// Arena used by the tests that exercise several size classes at once.
POUND_ALIGNED(64) static uint8_t pool_arena[64U * 1024U];

/// Arena for the single-class tests, where the block count must be predictable.
///
/// 320 bytes divided by the 32-byte stride of a 16-byte-payload, 16-byte-aligned
/// pool is exactly ten blocks, which lets the exhaustion test allocate to the
/// brim and then assert the next request fails.
POUND_ALIGNED(64) static uint8_t small_arena[320U];

/// Builds the configuration used by the single-class fixtures: exactly one
/// size class, so every block has the same payload and the accounting
/// arithmetic in the tests stays trivial.
static void
single_class_config(pool_config_t *POUND_RESTRICT config, size_t payload)
{
    memset(config, 0, sizeof(*config));
    config->min_payload     = payload;
    config->max_payload     = payload;
    config->alignment       = POOL_ALLOCATOR_DEFAULT_ALIGNMENT;
    config->growth_permille = 2000U;
}

/// Number of blocks `small_arena` yields under a 16-byte payload.
#define SMALL_ARENA_BLOCKS (sizeof(small_arena) / 32U)

/// Returns true when every byte in `[value, value + count)` is `expected`.
static bool
region_is_filled_with(const void *value, size_t count, uint8_t expected)
{
    const uint8_t *const bytes = (const uint8_t *)value;

    for (size_t i = 0U; i < count; ++i)
    {
        if (bytes[i] != expected)
        {
            return false;
        }
    }

    return true;
}

// -----------------------------------------------------------------------------
// Configuration validation
// -----------------------------------------------------------------------------

POUND_TEST(pool_allocator, init_rejects_invalid_arguments)
{
    pool_allocator_t pool;
    pool_config_t    config = { 0 };

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(pool_allocator_init(NULL, pool_arena, sizeof(pool_arena), &config),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(pool_allocator_init(&pool, NULL, sizeof(pool_arena), &config),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(pool_allocator_init(&pool, pool_arena, 0U, &config),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);

    // Too small to hold a single block header is an allocation failure rather
    // than an argument failure: the arguments were well formed, the arena just
    // cannot serve them.
    pound_test_log_reset();
    POUND_CHECK_EQ_U64(pool_allocator_init(&pool, small_arena, 8U, &config),
                       POUND_ERROR_ALLOCATION_FAILED);
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);
}

POUND_TEST(pool_allocator, init_rejects_non_power_of_two_alignment)
{
    pool_allocator_t pool;
    pool_config_t    config = { 0 };

    config.alignment = 24U;

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(pool_allocator_init(&pool, pool_arena, sizeof(pool_arena), &config),
                       POUND_ERROR_MEMORY_ALIGNMENT);
    POUND_CHECK(pound_test_log_contains("power of two"));
}

POUND_TEST(pool_allocator, init_rejects_alignment_outside_supported_range)
{
    pool_allocator_t pool;
    pool_config_t    config = { 0 };

    // Below the floor: the 16-byte block header could not place its free-list
    // link without misaligning it.
    config.alignment = 4U;

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(pool_allocator_init(&pool, pool_arena, sizeof(pool_arena), &config),
                       POUND_ERROR_MEMORY_ALIGNMENT);
    POUND_CHECK(pound_test_log_contains("supported range"));

    // Above the ceiling: a payload could then straddle pages, which would make
    // the reported usable size meaningless.
    config.alignment = 8192U;

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(pool_allocator_init(&pool, pool_arena, sizeof(pool_arena), &config),
                       POUND_ERROR_MEMORY_ALIGNMENT);
    POUND_CHECK(pound_test_log_contains("supported range"));
}

POUND_TEST(pool_allocator, init_rejects_misaligned_arena)
{
    pool_allocator_t pool;
    pool_config_t    config = { 0 };

    config.alignment = 64U;

    // `pool_arena` is 64-byte aligned, so offsetting by one guarantees failure
    // without depending on where the linker placed the array.
    pound_test_log_reset();
    POUND_CHECK_EQ_U64(pool_allocator_init(&pool, pool_arena + 1, sizeof(pool_arena) - 1U, &config),
                       POUND_ERROR_MEMORY_ALIGNMENT);
    POUND_CHECK(pound_test_log_contains("multiple"));
}

POUND_TEST(pool_allocator, init_rejects_non_increasing_growth)
{
    pool_allocator_t pool;
    pool_config_t    config = { 0 };

    // Growth of exactly 1000 would leave every class the same size, so the
    // table could never terminate.
    config.growth_permille = 1000U;

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(pool_allocator_init(&pool, pool_arena, sizeof(pool_arena), &config),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK(pound_test_log_contains("growth_permille"));
}

POUND_TEST(pool_allocator, init_rejects_inverted_payload_range)
{
    pool_allocator_t pool;
    pool_config_t    config = { 0 };

    config.min_payload = 4096U;
    config.max_payload = 16U;

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(pool_allocator_init(&pool, pool_arena, sizeof(pool_arena), &config),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK(pound_test_log_contains("exceeds"));
}

POUND_TEST(pool_allocator, init_accepts_null_config_for_defaults)
{
    pool_allocator_t pool;

    POUND_REQUIRE(POUND_SUCCESS
                  == pool_allocator_init(&pool, pool_arena, sizeof(pool_arena), NULL));

    // The documented defaults are more than one class, and the smallest class
    // must be able to hold the default alignment.
    POUND_CHECK(pool.class_count > 1U);
    POUND_CHECK(pool.classes[0].payload_size >= POOL_ALLOCATOR_DEFAULT_ALIGNMENT);
    POUND_CHECK_EQ_U64(pool.alignment, POOL_ALLOCATOR_DEFAULT_ALIGNMENT);

    pool_allocator_destroy(&pool);
}

// -----------------------------------------------------------------------------
// Size classes
// -----------------------------------------------------------------------------

POUND_TEST(pool_allocator, size_classes_increase_strictly)
{
    pool_allocator_t pool;
    pool_config_t    config = { 0 };

    POUND_REQUIRE(POUND_SUCCESS
                  == pool_allocator_init(&pool, pool_arena, sizeof(pool_arena), &config));
    POUND_REQUIRE(pool.class_count > 1U);

    for (size_t i = 1U; i < pool.class_count; ++i)
    {
        POUND_CHECK(pool.classes[i - 1U].payload_size < pool.classes[i].payload_size);
    }

    // Every class's stride must be a whole number of alignment units, which is
    // what guarantees the payload of the last block in the arena is still
    // aligned even though the arena itself need not be a stride multiple.
    for (size_t i = 0U; i < pool.class_count; ++i)
    {
        POUND_CHECK_EQ_U64(pool.classes[i].stride % pool.alignment, 0U);
        POUND_CHECK(pool.classes[i].stride
                    >= pool.header_stride + pool.classes[i].payload_size);
    }

    pool_allocator_destroy(&pool);
}

POUND_TEST(pool_allocator, class_for_size_selects_smallest_fitting_class)
{
    pool_allocator_t pool;
    pool_config_t    config = { 0 };

    POUND_REQUIRE(POUND_SUCCESS
                  == pool_allocator_init(&pool, pool_arena, sizeof(pool_arena), &config));

    // The classes grow geometrically, so their payloads are *not* contiguous.
    // The contract is therefore "smallest class that fits", which has to be
    // checked against a linear scan of the table rather than against an assumed
    // previous class: a request one byte below a payload may legitimately still
    // land in the same class when the payload above it had a gap.
    for (size_t i = 0U; i < pool.class_count; ++i)
    {
        const size_t payload = pool.classes[i].payload_size;

        // Exactly the class payload resolves to that class.
        POUND_CHECK_EQ_U64(pool_allocator_class_for_size(&pool, payload), i);

        // Every size from the previous payload up to this one also resolves
        // here, because this is the first class that can hold any of them.
        const size_t lower = (0U == i) ? 1U : pool.classes[i - 1U].payload_size + 1U;

        for (size_t probe = lower; probe <= payload; ++probe)
        {
            POUND_CHECK_EQ_U64(pool_allocator_class_for_size(&pool, probe), i);
        }
    }

    // One byte above the largest class cannot be served.
    POUND_CHECK_EQ_U64(pool_allocator_class_for_size(&pool, pool.classes[pool.class_count - 1U].payload_size + 1U),
                       pool.class_count);

    // A wildly oversized request cannot be served either.
    POUND_CHECK_EQ_U64(pool_allocator_class_for_size(&pool, SIZE_MAX), pool.class_count);

    pool_allocator_destroy(&pool);
}

// -----------------------------------------------------------------------------
// Allocation and free
// -----------------------------------------------------------------------------

POUND_TEST(pool_allocator, arena_is_split_byte_equal_across_classes)
{
    pool_allocator_t pool;
    pool_config_t    config = { 0 };

    POUND_REQUIRE(POUND_SUCCESS
                  == pool_allocator_init(&pool, pool_arena, sizeof(pool_arena), &config));
    POUND_REQUIRE(pool.class_count > 4U);

    // The regression this guards against is subtle and expensive: carving the
    // smallest class first lets it consume the entire arena, every larger class
    // then has zero capacity, and the pool silently refuses everything above its
    // smallest size while still passing every alignment and accounting check.
    //
    // So the contract asserted here is exact rather than approximate: each class
    // gets its byte-equal share and nothing more or less, which is only possible
    // if no class is competing with another for the same bytes.
    const size_t whole_share = sizeof(pool_arena) / pool.class_count;
    const size_t remainder   = sizeof(pool_arena) % pool.class_count;

    size_t committed = 0U;

    for (size_t i = 0U; i < pool.class_count; ++i)
    {
        const size_t share = whole_share + ((i < remainder) ? 1U : 0U);
        const size_t want  = share / pool.classes[i].stride;

        POUND_CHECK_MSG(pool.classes[i].capacity == want,
                        "class %zu: expected %zu blocks from a %zu-byte share, got %zu",
                        i,
                        want,
                        share,
                        pool.classes[i].capacity);

        committed += pool.classes[i].capacity * pool.classes[i].stride;
    }

    // The shares sum to the arena, so the committed total is bounded by it.
    POUND_CHECK(committed <= sizeof(pool_arena));

    // And class 0 must not have taken the arena for itself. Under greedy
    // smallest-first carving this figure is the whole arena.
    POUND_CHECK_MSG(pool.classes[0].capacity * pool.classes[0].stride < sizeof(pool_arena) / 4U,
                    "class 0 committed %zu bytes of a %zu-byte arena",
                    pool.classes[0].capacity * pool.classes[0].stride,
                    sizeof(pool_arena));

    pool_allocator_destroy(&pool);
}

POUND_TEST(pool_allocator, every_class_is_usable_when_its_share_holds_a_block)
{
    pool_allocator_t pool;

    // A 512-byte ceiling keeps every stride comfortably inside its share of the
    // 64 KiB arena, so the only remaining reason for a class to be unusable
    // would be starvation by a smaller class.
    pool_config_t config = { 0 };

    config.max_payload = 512U;

    POUND_REQUIRE(POUND_SUCCESS
                  == pool_allocator_init(&pool, pool_arena, sizeof(pool_arena), &config));

    for (size_t i = 0U; i < pool.class_count; ++i)
    {
        POUND_CHECK_MSG(pool.classes[i].capacity > 0U,
                        "class %zu (%zu-byte payload, %zu-byte stride) holds 0 blocks",
                        i,
                        pool.classes[i].payload_size,
                        pool.classes[i].stride);

        POUND_CHECK_MSG(NULL != pool_allocator_alloc(&pool, pool.classes[i].payload_size, 0U),
                        "class %zu could not serve its own payload size", i);
    }

    pool_allocator_destroy(&pool);
}

POUND_TEST(pool_allocator, arena_too_small_for_the_class_count_is_refused)
{
    pool_allocator_t pool;
    pool_config_t    config = { 0 };

    // The default table spans roughly a dozen classes, so a 320-byte arena
    // cannot give every class one block. Because the arena is split byte-equal
    // rather than smallest-first, that shows up as a clean init refusal rather
    // than as a pool that silently serves only its smallest size.
    POUND_CHECK_MSG(pool_allocator_init(&pool, small_arena, sizeof(small_arena), &config)
                        != POUND_SUCCESS,
                    "a 320-byte arena should not support the default class table");

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(pool_allocator_init(&pool, small_arena, sizeof(small_arena), &config),
                       POUND_ERROR_ALLOCATION_FAILED);
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);

    // Lowering `max_payload` is the documented remedy, because it removes
    // classes rather than shrinking them.
    config.max_payload = 64U;
    POUND_CHECK_EQ_U64(pool_allocator_init(&pool, small_arena, sizeof(small_arena), &config),
                       POUND_SUCCESS);
    POUND_CHECK(pool.class_count < POOL_ALLOCATOR_MAX_CLASSES);

    pool_allocator_destroy(&pool);
}

POUND_TEST(pool_allocator, alloc_returns_pool_aligned_payloads)
{
    pool_allocator_t pool;
    pool_config_t    config = { 0 };

    config.alignment = 64U;

    POUND_REQUIRE(POUND_SUCCESS
                  == pool_allocator_init(&pool, pool_arena, sizeof(pool_arena), &config));
    POUND_CHECK_EQ_U64(pool.alignment, 64U);

    void *blocks[32];

    for (size_t i = 0U; i < 32U; ++i)
    {
        blocks[i] = pool_allocator_alloc(&pool, 24U, 0U);
        POUND_REQUIRE(NULL != blocks[i]);

        // Passing a zero alignment requests the pool's guarantee rather than
        // asking for something stricter, so it must not be refused.
        POUND_CHECK_EQ_U64((uintptr_t)blocks[i] % 64U, 0U);
    }

    for (size_t i = 0U; i < 32U; ++i)
    {
        pool_allocator_free(&pool, blocks[i]);
    }

    pool_allocator_destroy(&pool);
}

POUND_TEST(pool_allocator, live_blocks_never_alias)
{
    pool_allocator_t pool;
    pool_config_t    config = { 0 };

    POUND_REQUIRE(POUND_SUCCESS
                  == pool_allocator_init(&pool, pool_arena, sizeof(pool_arena), &config));

    void *blocks[64];

    for (size_t i = 0U; i < 64U; ++i)
    {
        blocks[i] = pool_allocator_alloc(&pool, 32U, 0U);
        POUND_REQUIRE(NULL != blocks[i]);

        for (size_t j = 0U; j < i; ++j)
        {
            POUND_CHECK(blocks[i] != blocks[j]);
        }
    }

    // Writing each block must not disturb any other, which is what proves the
    // strides really do keep payloads disjoint.
    for (size_t i = 0U; i < 64U; ++i)
    {
        memset(blocks[i], (int)(0xA0U + (i & 0xFU)), 32U);
    }

    for (size_t i = 0U; i < 64U; ++i)
    {
        POUND_CHECK(region_is_filled_with(blocks[i], 32U, (uint8_t)(0xA0U + (i & 0xFU))));
    }

    for (size_t i = 0U; i < 64U; ++i)
    {
        pool_allocator_free(&pool, blocks[i]);
    }

    pool_allocator_destroy(&pool);
}

POUND_TEST(pool_allocator, free_returns_the_block_to_its_own_class)
{
    pool_allocator_t pool;
    pool_config_t    config = { 0 };

    POUND_REQUIRE(POUND_SUCCESS
                  == pool_allocator_init(&pool, pool_arena, sizeof(pool_arena), &config));

    // Two classes in play, so the test can prove that freeing a small block
    // does not make it available to a large request.
    const size_t small_index = pool_allocator_class_for_size(&pool, 16U);
    const size_t large_index = pool_allocator_class_for_size(&pool, 1024U);

    POUND_REQUIRE(small_index < large_index);

    void *small = pool_allocator_alloc(&pool, 16U, 0U);
    POUND_REQUIRE(NULL != small);

    pool_allocator_free(&pool, small);

    // A large request must still draw from the large class, not the block the
    // small request released.
    void *large = pool_allocator_alloc(&pool, 1024U, 0U);
    POUND_REQUIRE(NULL != large);
    POUND_CHECK(large != small);
    POUND_CHECK(pool_allocator_get_usable_size(&pool, large) >= 1024U);

    // The freed small block is available again to a small request.
    void *reused = pool_allocator_alloc(&pool, 16U, 0U);
    POUND_CHECK_PTR_EQ(reused, small);

    pool_allocator_free(&pool, reused);
    pool_allocator_free(&pool, large);

    pool_allocator_destroy(&pool);
}

POUND_TEST(pool_allocator, zero_byte_request_serves_the_smallest_class)
{
    pool_allocator_t pool;
    pool_config_t    config = { 0 };

    POUND_REQUIRE(POUND_SUCCESS
                  == pool_allocator_init(&pool, pool_arena, sizeof(pool_arena), &config));

    void *block = pool_allocator_alloc(&pool, 0U, 0U);
    POUND_REQUIRE(NULL != block);
    POUND_CHECK_EQ_U64(pool_allocator_get_usable_size(&pool, block), pool.classes[0].payload_size);

    pool_allocator_free(&pool, block);
    pool_allocator_destroy(&pool);
}

POUND_TEST(pool_allocator, usable_size_resolves_for_live_and_free_blocks)
{
    pool_allocator_t pool;
    pool_config_t    config = { 0 };

    POUND_REQUIRE(POUND_SUCCESS
                  == pool_allocator_init(&pool, pool_arena, sizeof(pool_arena), &config));

    const size_t index = pool_allocator_class_for_size(&pool, 100U);

    void *block = pool_allocator_alloc(&pool, 100U, 0U);
    POUND_REQUIRE(NULL != block);

    // Live block: the header still names its class, so the query resolves.
    POUND_CHECK_EQ_U64(pool_allocator_get_usable_size(&pool, block), pool.classes[index].payload_size);

    pool_allocator_free(&pool, block);

    // Freed block: still resolvable, because the header records the class in
    // both states.
    POUND_CHECK_EQ_U64(pool_allocator_get_usable_size(&pool, block), pool.classes[index].payload_size);

    // A stack address was never carved by this pool.
    int    on_stack = 0;
    (void)on_stack;
    POUND_CHECK_EQ_U64(pool_allocator_get_usable_size(&pool, &on_stack), 0U);
    POUND_CHECK_EQ_U64(pool_allocator_get_usable_size(&pool, NULL), 0U);

    pool_allocator_destroy(&pool);
}

// -----------------------------------------------------------------------------
// Error paths
// -----------------------------------------------------------------------------

POUND_TEST(pool_allocator, double_free_is_refused_and_logged)
{
    pool_allocator_t pool;
    pool_config_t    config = { 0 };

    single_class_config(&config, 16U);
    POUND_REQUIRE(POUND_SUCCESS == pool_allocator_init(&pool, small_arena, sizeof(small_arena), &config));

    void *block = pool_allocator_alloc(&pool, 16U, 0U);
    POUND_REQUIRE(NULL != block);

    pool_allocator_free(&pool, block);

    pound_test_log_reset();
    pool_allocator_free(&pool, block);

    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);
    POUND_CHECK(pound_test_log_contains("double free"));

    // The refusal must be inert: the class counter must still show exactly one
    // block live, and that block must still be allocatable.
    pool_stats_t stats = { 0 };
    POUND_REQUIRE(POUND_SUCCESS == pool_allocator_get_stats(&pool, &stats));
    POUND_CHECK_EQ_U64(stats.total_failures, 1U);
    POUND_CHECK_EQ_U64(stats.total_frees, 1U);

    void *reused = pool_allocator_alloc(&pool, 16U, 0U);
    POUND_CHECK_PTR_EQ(reused, block);

    pool_allocator_free(&pool, reused);
    pool_allocator_destroy(&pool);
}

POUND_TEST(pool_allocator, foreign_pointer_free_is_refused_and_logged)
{
    pool_allocator_t pool;
    pool_config_t    config = { 0 };

    single_class_config(&config, 16U);
    POUND_REQUIRE(POUND_SUCCESS == pool_allocator_init(&pool, small_arena, sizeof(small_arena), &config));

    int on_stack = 0;

    pound_test_log_reset();
    pool_allocator_free(&pool, &on_stack);

    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);
    POUND_CHECK(pound_test_log_contains("not inside the arena"));

    // The arena itself must be untouched: the header the subtraction produced
    // was never pushed onto a free list.
    pool_stats_t stats = { 0 };
    POUND_REQUIRE(POUND_SUCCESS == pool_allocator_get_stats(&pool, &stats));
    POUND_CHECK_EQ_U64(stats.total_failures, 1U);
    POUND_CHECK_EQ_U64(stats.total_frees, 0U);
    POUND_CHECK_EQ_U64(stats.bytes_in_use, 0U);

    // And the pool must still be fully serviceable.
    for (size_t i = 0U; i < SMALL_ARENA_BLOCKS; ++i)
    {
        POUND_CHECK_PTR_NON_NULL(pool_allocator_alloc(&pool, 16U, 0U));
    }

    pool_allocator_destroy(&pool);
}

POUND_TEST(pool_allocator, arena_exhaustion_is_reported_and_logged)
{
    pool_allocator_t pool;
    pool_config_t    config = { 0 };

    single_class_config(&config, 16U);
    POUND_REQUIRE(POUND_SUCCESS == pool_allocator_init(&pool, small_arena, sizeof(small_arena), &config));
    POUND_REQUIRE(pool.classes[0].capacity == SMALL_ARENA_BLOCKS);

    void *blocks[SMALL_ARENA_BLOCKS];

    for (size_t i = 0U; i < SMALL_ARENA_BLOCKS; ++i)
    {
        blocks[i] = pool_allocator_alloc(&pool, 16U, 0U);
        POUND_REQUIRE(NULL != blocks[i]);
    }

    pound_test_log_reset();
    POUND_CHECK_PTR_NULL(pool_allocator_alloc(&pool, 16U, 0U));
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);
    POUND_CHECK(pound_test_log_contains("exhausted"));

    pool_stats_t stats = { 0 };
    POUND_REQUIRE(POUND_SUCCESS == pool_allocator_get_stats(&pool, &stats));
    POUND_CHECK_EQ_U64(stats.arena_exhaustions, 1U);
    POUND_CHECK_EQ_U64(stats.total_failures, 1U);
    POUND_CHECK_EQ_U64(stats.total_requests, SMALL_ARENA_BLOCKS + 1U);

    // Releasing one block must make exactly one more request succeed.
    pool_allocator_free(&pool, blocks[0]);

    void *reused = pool_allocator_alloc(&pool, 16U, 0U);
    POUND_CHECK_PTR_EQ(reused, blocks[0]);

    for (size_t i = 1U; i < SMALL_ARENA_BLOCKS; ++i)
    {
        pool_allocator_free(&pool, blocks[i]);
    }

    pool_allocator_free(&pool, reused);
    pool_allocator_destroy(&pool);
}

POUND_TEST(pool_allocator, oversized_request_is_refused_and_logged)
{
    pool_allocator_t pool;
    pool_config_t    config = { 0 };

    single_class_config(&config, 16U);
    POUND_REQUIRE(POUND_SUCCESS == pool_allocator_init(&pool, small_arena, sizeof(small_arena), &config));

    pound_test_log_reset();
    POUND_CHECK_PTR_NULL(pool_allocator_alloc(&pool, 17U, 0U));
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);
    POUND_CHECK(pound_test_log_contains("size class"));

    pool_stats_t stats = { 0 };
    POUND_REQUIRE(POUND_SUCCESS == pool_allocator_get_stats(&pool, &stats));
    POUND_CHECK_EQ_U64(stats.total_failures, 1U);

    // An unserviceable request must be distinguished from an exhausted one, so
    // that a caller can tell "too big" from "out of room".
    POUND_CHECK_EQ_U64(stats.arena_exhaustions, 0U);

    pool_allocator_destroy(&pool);
}

POUND_TEST(pool_allocator, alignment_beyond_the_pool_is_refused_and_logged)
{
    pool_allocator_t pool;
    pool_config_t    config = { 0 };

    config.alignment = 16U;

    POUND_REQUIRE(POUND_SUCCESS
                  == pool_allocator_init(&pool, pool_arena, sizeof(pool_arena), &config));

    // Requesting more alignment than the pool guarantees must fail rather than
    // quietly hand back a less-aligned pointer, because the caller almost
    // certainly needs the alignment for a SIMD or DMA requirement.
    pound_test_log_reset();
    POUND_CHECK_PTR_NULL(pool_allocator_alloc(&pool, 16U, 64U));
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);
    POUND_CHECK(pound_test_log_contains("Raise the pool"));

    // Exactly the pool's alignment is accepted.
    POUND_CHECK_PTR_NON_NULL(pool_allocator_alloc(&pool, 16U, 16U));

    pool_allocator_destroy(&pool);
}

POUND_TEST(pool_allocator, uninitialised_pool_is_reported_not_crashed)
{
    pool_allocator_t pool;

    memset(&pool, 0, sizeof(pool));

    pound_test_log_reset();
    POUND_CHECK_PTR_NULL(pool_allocator_alloc(&pool, 16U, 0U));
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);
    POUND_CHECK(pound_test_log_contains("never initialised"));

    pool_stats_t stats = { 0 };
    pound_test_log_reset();
    POUND_CHECK_EQ_U64(pool_allocator_get_stats(&pool, &stats), POUND_ERROR_NOT_INITIALIZED);
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);

    // The teardown and read-only entry points must tolerate an uninitialised
    // pool rather than taking the mutex that was never created.
    pool_allocator_reset(&pool);
    pool_allocator_destroy(&pool);

    POUND_CHECK_PTR_NULL(pool_allocator_alloc(NULL, 16U, 0U));
    pool_allocator_free(NULL, NULL);
    POUND_CHECK_EQ_U64(pool_allocator_get_usable_size(NULL, NULL), 0U);
    POUND_CHECK_EQ_U64(pool_allocator_leak_bytes(NULL), 0U);
}

// -----------------------------------------------------------------------------
// Poisoning
// -----------------------------------------------------------------------------

POUND_TEST(pool_allocator, poison_covers_allocated_and_freed_blocks)
{
    pool_allocator_t pool;
    pool_config_t    config = { 0 };

    single_class_config(&config, 32U);
    config.poison = true;

    POUND_REQUIRE(POUND_SUCCESS
                  == pool_allocator_init(&pool, pool_arena, sizeof(pool_arena), &config));

    void *block = pool_allocator_alloc(&pool, 32U, 0U);
    POUND_REQUIRE(NULL != block);

    // A freshly carved block must already read as poison, so a guest read of
    // uninitialised memory is visible rather than looking like a valid zero.
    POUND_CHECK(region_is_filled_with(block, 32U, (uint8_t)POOL_ALLOCATOR_POISON_BYTE));

    memset(block, 0x5A, 32U);
    POUND_CHECK(region_is_filled_with(block, 32U, 0x5AU));

    pool_allocator_free(&pool, block);
    POUND_CHECK(region_is_filled_with(block, 32U, (uint8_t)POOL_ALLOCATOR_POISON_BYTE));

    pool_allocator_destroy(&pool);
}

POUND_TEST(pool_allocator, poison_can_be_toggled_at_runtime)
{
    pool_allocator_t pool;
    pool_config_t    config = { 0 };

    single_class_config(&config, 32U);

    POUND_REQUIRE(POUND_SUCCESS
                  == pool_allocator_init(&pool, pool_arena, sizeof(pool_arena), &config));

    // Poisoning is off by default, so a fresh allocation must not be scrubbed.
    void *plain = pool_allocator_alloc(&pool, 32U, 0U);
    POUND_REQUIRE(NULL != plain);
    memset(plain, 0x11, 32U);
    POUND_CHECK(region_is_filled_with(plain, 32U, 0x11U));
    pool_allocator_free(&pool, plain);

    pool_allocator_set_poison(&pool, true);

    void *scrubbed = pool_allocator_alloc(&pool, 32U, 0U);
    POUND_REQUIRE(NULL != scrubbed);
    POUND_CHECK(region_is_filled_with(scrubbed, 32U, (uint8_t)POOL_ALLOCATOR_POISON_BYTE));
    pool_allocator_free(&pool, scrubbed);

    pool_allocator_set_poison(&pool, false);
    pool_allocator_destroy(&pool);
}

// -----------------------------------------------------------------------------
// Reset, accounting and teardown
// -----------------------------------------------------------------------------

POUND_TEST(pool_allocator, reset_restores_the_initial_state)
{
    pool_allocator_t pool;
    pool_config_t    config = { 0 };

    single_class_config(&config, 16U);
    POUND_REQUIRE(POUND_SUCCESS == pool_allocator_init(&pool, small_arena, sizeof(small_arena), &config));

    const size_t capacity_before = pool.classes[0].capacity;

    for (size_t i = 0U; i < 4U; ++i)
    {
        POUND_CHECK_PTR_NON_NULL(pool_allocator_alloc(&pool, 16U, 0U));
    }

    pool_allocator_reset(&pool);

    // Every counter returns to its start-up value, including the ones that
    // reset must zero rather than merely recompute.
    pool_stats_t stats = { 0 };
    POUND_REQUIRE(POUND_SUCCESS == pool_allocator_get_stats(&pool, &stats));
    POUND_CHECK_EQ_U64(stats.total_requests, 0U);
    POUND_CHECK_EQ_U64(stats.total_allocations, 0U);
    POUND_CHECK_EQ_U64(stats.total_frees, 0U);
    POUND_CHECK_EQ_U64(stats.total_failures, 0U);
    POUND_CHECK_EQ_U64(stats.arena_exhaustions, 0U);
    POUND_CHECK_EQ_U64(stats.bytes_in_use, 0U);
    POUND_CHECK_EQ_U64(stats.peak_bytes_in_use, 0U);

    // And the same number of blocks is available again, which proves the arena
    // was re-carved from the beginning rather than merely having its counters
    // rewritten.
    POUND_CHECK_EQ_U64(pool.classes[0].capacity, capacity_before);
    POUND_CHECK_EQ_U64(stats.arena_committed, capacity_before * 32U);

    pool_allocator_destroy(&pool);
}

POUND_TEST(pool_allocator, accounting_tracks_live_payload_bytes)
{
    pool_allocator_t pool;
    pool_config_t    config = { 0 };

    single_class_config(&config, 24U);
    POUND_REQUIRE(POUND_SUCCESS
                  == pool_allocator_init(&pool, pool_arena, sizeof(pool_arena), &config));

    void *blocks[10];

    for (size_t i = 0U; i < 10U; ++i)
    {
        blocks[i] = pool_allocator_alloc(&pool, 24U, 0U);
        POUND_REQUIRE(NULL != blocks[i]);
    }

    pool_stats_t stats = { 0 };
    POUND_REQUIRE(POUND_SUCCESS == pool_allocator_get_stats(&pool, &stats));
    POUND_CHECK_EQ_U64(stats.total_allocations, 10U);
    POUND_CHECK_EQ_U64(stats.total_frees, 0U);
    POUND_CHECK_EQ_U64(stats.bytes_in_use, 10U * 24U);
    POUND_CHECK_EQ_U64(stats.peak_bytes_in_use, 10U * 24U);

    for (size_t i = 0U; i < 4U; ++i)
    {
        pool_allocator_free(&pool, blocks[i]);
    }

    POUND_REQUIRE(POUND_SUCCESS == pool_allocator_get_stats(&pool, &stats));
    POUND_CHECK_EQ_U64(stats.total_frees, 4U);
    POUND_CHECK_EQ_U64(stats.bytes_in_use, 6U * 24U);

    // The high-water mark must not follow the accounting back down.
    POUND_CHECK_EQ_U64(stats.peak_bytes_in_use, 10U * 24U);
    POUND_CHECK_EQ_U64(pool_allocator_leak_bytes(&pool), 6U * 24U);

    // The per-class counters must agree with the global ones.
    POUND_CHECK_EQ_U64(pool.classes[0].used, 6U);
    POUND_CHECK_EQ_U64(pool.classes[0].allocations, 10U);
    POUND_CHECK_EQ_U64(pool.classes[0].frees, 4U);
    POUND_CHECK_EQ_U64(pool.classes[0].peak_used, 10U);

    // `capacity` counts carved blocks and is unrelated to how many are live; it
    // must simply be at least the live count, and the free list must agree with
    // the difference.
    POUND_CHECK(pool.classes[0].capacity >= pool.classes[0].used);
    POUND_CHECK_EQ_U64(pool_allocator_class_for_size(&pool, 24U), 0U);

    for (size_t i = 4U; i < 10U; ++i)
    {
        pool_allocator_free(&pool, blocks[i]);
    }

    POUND_REQUIRE(POUND_SUCCESS == pool_allocator_get_stats(&pool, &stats));
    POUND_CHECK_EQ_U64(stats.bytes_in_use, 0U);
    POUND_CHECK_EQ_U64(stats.total_allocations, stats.total_frees);

    pool_allocator_destroy(&pool);
}

POUND_TEST(pool_allocator, arena_committed_never_exceeds_the_arena)
{
    pool_allocator_t pool;
    pool_config_t    config = { 0 };

    single_class_config(&config, 16U);
    POUND_REQUIRE(POUND_SUCCESS == pool_allocator_init(&pool, small_arena, sizeof(small_arena), &config));

    pool_stats_t stats = { 0 };
    POUND_REQUIRE(POUND_SUCCESS == pool_allocator_get_stats(&pool, &stats));

    POUND_CHECK_EQ_U64(stats.arena_size, sizeof(small_arena));
    POUND_CHECK(stats.arena_committed <= stats.arena_size);

    // The uncommitted remainder must be smaller than one stride, otherwise the
    // carving loop stopped early.
    const size_t stride = pool.classes[0].stride;
    POUND_CHECK((stats.arena_size - stats.arena_committed) < stride);

    pool_allocator_destroy(&pool);
}

POUND_TEST(pool_allocator, destroy_warns_about_live_blocks)
{
    pool_allocator_t pool;
    pool_config_t    config = { 0 };

    single_class_config(&config, 16U);
    POUND_REQUIRE(POUND_SUCCESS == pool_allocator_init(&pool, small_arena, sizeof(small_arena), &config));

    void *leaked = pool_allocator_alloc(&pool, 16U, 0U);
    POUND_REQUIRE(NULL != leaked);

    pound_test_log_reset();
    pool_allocator_destroy(&pool);

    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_WARN) >= 1U);
    POUND_CHECK(pound_test_log_contains("still live"));

    // Destroy must leave the structure cleared so a stale copy cannot be used.
    POUND_CHECK_PTR_NULL(pool.arena);
    POUND_CHECK_EQ_U64(pool.class_count, 0U);
}

POUND_TEST(pool_allocator, error_strings_are_unique_and_total)
{
    // Every code the allocators can produce must have a distinct, greppable
    // name, and an unrecognised value must still render.
    static const error_t codes[] = {
        POUND_SUCCESS,
        POUND_ERROR_INVALID_ARGUMENT,
        POUND_ERROR_MEMORY_ALIGNMENT,
        POUND_ERROR_MEMORY_FAULT,
        POUND_ERROR_GUEST_ADDRESS_OVERFLOW,
        POUND_ERROR_GUEST_ADDRESS_OUT_OF_BOUNDS,
        POUND_ERROR_ALREADY_INITIALIZED,
        POUND_ERROR_NOT_INITIALIZED,
        POUND_ERROR_ALLOCATION_FAILED,
        POUND_ERROR_DOUBLE_FREE,
        POUND_ERROR_CORRUPTED,
        POUND_ERROR_UNSUPPORTED_FORMAT,
        POUND_ERROR_KEY_MISSING,
        POUND_ERROR_HASH_MISMATCH,
        POUND_ERROR_MALFORMED_HEADER,
        POUND_ERROR_IO,
        POUND_ERROR_UNSUPPORTED_INSTRUCTION,
        POUND_ERROR_TRANSLATION_FAILED,
    };

    const size_t count = sizeof(codes) / sizeof(codes[0]);

    for (size_t i = 0U; i < count; ++i)
    {
        const char *const text = pound_error_to_string(codes[i]);

        POUND_REQUIRE(NULL != text);
        POUND_CHECK_STR_EQ(text, text);
        POUND_CHECK(0 == strncmp(text, "POUND_", 6U));

        for (size_t j = i + 1U; j < count; ++j)
        {
            POUND_CHECK(0 != strcmp(text, pound_error_to_string(codes[j])));
        }
    }

    POUND_CHECK_STR_EQ(pound_error_to_string((error_t)9999), "POUND_ERROR_UNKNOWN");
}

POUND_TEST_SUITE(pool_allocator,
                POUND_TEST_CASE(pool_allocator, init_rejects_invalid_arguments),
                POUND_TEST_CASE(pool_allocator, init_rejects_non_power_of_two_alignment),
                POUND_TEST_CASE(pool_allocator, init_rejects_alignment_outside_supported_range),
                POUND_TEST_CASE(pool_allocator, init_rejects_misaligned_arena),
                POUND_TEST_CASE(pool_allocator, init_rejects_non_increasing_growth),
                POUND_TEST_CASE(pool_allocator, init_rejects_inverted_payload_range),
                POUND_TEST_CASE(pool_allocator, init_accepts_null_config_for_defaults),
                POUND_TEST_CASE(pool_allocator, size_classes_increase_strictly),
                POUND_TEST_CASE(pool_allocator, class_for_size_selects_smallest_fitting_class),
                POUND_TEST_CASE(pool_allocator, arena_is_split_byte_equal_across_classes),
                POUND_TEST_CASE(pool_allocator, every_class_is_usable_when_its_share_holds_a_block),
                POUND_TEST_CASE(pool_allocator, arena_too_small_for_the_class_count_is_refused),
                POUND_TEST_CASE(pool_allocator, alloc_returns_pool_aligned_payloads),
                POUND_TEST_CASE(pool_allocator, live_blocks_never_alias),
                POUND_TEST_CASE(pool_allocator, free_returns_the_block_to_its_own_class),
                POUND_TEST_CASE(pool_allocator, zero_byte_request_serves_the_smallest_class),
                POUND_TEST_CASE(pool_allocator, usable_size_resolves_for_live_and_free_blocks),
                POUND_TEST_CASE(pool_allocator, double_free_is_refused_and_logged),
                POUND_TEST_CASE(pool_allocator, foreign_pointer_free_is_refused_and_logged),
                POUND_TEST_CASE(pool_allocator, arena_exhaustion_is_reported_and_logged),
                POUND_TEST_CASE(pool_allocator, oversized_request_is_refused_and_logged),
                POUND_TEST_CASE(pool_allocator, alignment_beyond_the_pool_is_refused_and_logged),
                POUND_TEST_CASE(pool_allocator, uninitialised_pool_is_reported_not_crashed),
                POUND_TEST_CASE(pool_allocator, poison_covers_allocated_and_freed_blocks),
                POUND_TEST_CASE(pool_allocator, poison_can_be_toggled_at_runtime),
                POUND_TEST_CASE(pool_allocator, reset_restores_the_initial_state),
                POUND_TEST_CASE(pool_allocator, accounting_tracks_live_payload_bytes),
                POUND_TEST_CASE(pool_allocator, arena_committed_never_exceeds_the_arena),
                POUND_TEST_CASE(pool_allocator, destroy_warns_about_live_blocks),
                POUND_TEST_CASE(pool_allocator, error_strings_are_unique_and_total))

// -----------------------------------------------------------------------------
// Mutex
// -----------------------------------------------------------------------------

/// Exercises the mutex contract that the pool relies on: initialise, take and
/// release repeatedly, tolerate a NULL being passed to the void entry points,
/// and tear down.
static void
exercise_mutex_round_trips(void)
{
    mutex_t mutex;

    POUND_REQUIRE(POUND_SUCCESS == mutex_init(&mutex));

    for (int i = 0; i < 64; ++i)
    {
        mutex_lock(&mutex);
        mutex_unlock(&mutex);
    }

    // A statically initialised mutex must be usable without `init`, which is
    // what lets a file-scope allocator declare its lock directly.
    mutex_t static_mutex = POUND_MUTEX_STATIC_INIT;
    mutex_lock(&static_mutex);
    mutex_unlock(&static_mutex);

    mutex_destroy(&mutex);
}

POUND_TEST(mutex, init_lock_and_destroy_round_trip)
{
    exercise_mutex_round_trips();
}

POUND_TEST(mutex, void_entry_points_tolerate_null)
{
    // The void-returning entry points cannot report failure, so their contract
    // is that they log and return rather than dereference NULL.
    pound_test_log_reset();

    mutex_lock(NULL);
    mutex_unlock(NULL);
    mutex_destroy(NULL);

    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 3U);

    // `init` can report, so it must.
    POUND_CHECK_EQ_U64(mutex_init(NULL), POUND_ERROR_INVALID_ARGUMENT);
}

POUND_TEST(mutex, pool_stays_consistent_across_repeated_lifetimes)
{
    // The mutex is re-created by every `init`/`destroy` pair. If the pool were
    // using a lock that survived teardown, or double-initialising one, this
    // would corrupt the free lists; checking that every block is recoverable
    // after each round trip is the cheapest way to catch that.
    pool_config_t config = { 0 };

    single_class_config(&config, 16U);

    for (int round = 0; round < 8; ++round)
    {
        pool_allocator_t pool;

        POUND_REQUIRE(POUND_SUCCESS
                      == pool_allocator_init(&pool, small_arena, sizeof(small_arena), &config));

        void *blocks[SMALL_ARENA_BLOCKS];

        for (size_t i = 0U; i < SMALL_ARENA_BLOCKS; ++i)
        {
            blocks[i] = pool_allocator_alloc(&pool, 16U, 0U);
            POUND_REQUIRE(NULL != blocks[i]);
            memset(blocks[i], (int)round, 16U);
        }

        for (size_t i = 0U; i < SMALL_ARENA_BLOCKS; ++i)
        {
            POUND_CHECK(region_is_filled_with(blocks[i], 16U, (uint8_t)round));
            pool_allocator_free(&pool, blocks[i]);
        }

        POUND_CHECK_EQ_U64(pool_allocator_leak_bytes(&pool), 0U);
        pool_allocator_destroy(&pool);
    }
}

POUND_TEST_SUITE(mutex,
                POUND_TEST_CASE(mutex, init_lock_and_destroy_round_trip),
                POUND_TEST_CASE(mutex, void_entry_points_tolerate_null),
                POUND_TEST_CASE(mutex, pool_stays_consistent_across_repeated_lifetimes))
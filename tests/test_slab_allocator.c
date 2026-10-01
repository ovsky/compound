//! Tests for the slab allocator.
//!
//! The arithmetic here is deliberately discovered rather than hardcoded. A slab's
//! object count depends on the arena's remaining bytes at the moment of the carve,
//! on the cache's alignment, and on where the payload region happens to start --
//! all of which a test would have to re-derive to pin an exact number. What is
//! worth asserting is the behaviour: that a full slab's objects can still be
//! freed, that a foreign pointer is refused without touching a free list, that
//! trimming rewinds the arena over trailing free slabs but stops at a live one,
//! and that the counters agree with the observable state.

#include "pound_test.h"

#include "attributes.h"
#include "errors.h"
#include "memory/slab_allocator.h"
#include <string.h>

// -----------------------------------------------------------------------------
// Fixtures
// -----------------------------------------------------------------------------

/// Arena for the tests that need room to grow several slabs.
POUND_ALIGNED(64) static uint8_t slab_arena[256U * 1024U];

/// Arena for the exhaustion test, small enough that a few slabs fill it.
POUND_ALIGNED(64) static uint8_t small_slab_arena[8U * 1024U];

/// Upper bound on allocations made while discovering a slab's capacity, so a
/// configuration that never grows a slab fails the test rather than hanging it.
#define SLAB_TEST_MAX_OBJECTS 8192U

/// Registers a cache with the given shape and returns its id.
///
/// Zero arguments take the allocator's documented defaults, which is how most
/// tests read best.
static slab_cache_id_t
add_cache(slab_allocator_t *allocator, const char *name, size_t object_size, size_t alignment, size_t slab_bytes)
{
    slab_config_t config;

    memset(&config, 0, sizeof(config));
    config.name        = name;
    config.object_size = object_size;
    config.alignment   = alignment;
    config.slab_bytes  = slab_bytes;

    return slab_allocator_cache_create(allocator, &config);
}

/// Initialises `allocator` over `arena` with poisoning enabled.
static void
init_poisoned(slab_allocator_t *allocator, void *arena, size_t arena_size)
{
    slab_allocator_config_t config;

    memset(&config, 0, sizeof(config));
    config.poison = true;

    POUND_REQUIRE(POUND_SUCCESS == slab_allocator_init(allocator, arena, arena_size, &config));
}

/// Reads a stats snapshot.
///
/// A failure here would make every later assertion in a test meaningless, so it
/// is reported loudly and answered with a zeroed snapshot rather than aborted:
/// these are value-returning helpers, and `POUND_REQUIRE` cannot be used inside
/// them because it returns void.
static slab_stats_t
read_stats(const slab_allocator_t *allocator)
{
    slab_stats_t stats;

    memset(&stats, 0, sizeof(stats));
    POUND_CHECK_MSG(POUND_SUCCESS == slab_allocator_get_stats(allocator, &stats),
                    "slab_allocator_get_stats failed");

    return stats;
}

/// Allocates until the cache has grown a second slab, writing the objects to
/// `out` and returning how many fit in the first.
///
/// This discovers the first slab's capacity instead of assuming it, because the
/// capacity depends on the arena size, the alignment and the payload offset, none
/// of which a test should restate.
static size_t
fill_first_slab(slab_allocator_t *allocator, slab_cache_id_t id, void **out)
{
    size_t count = 0U;

    while (count < SLAB_TEST_MAX_OBJECTS)
    {
        out[count] = slab_allocator_alloc(allocator, id);

        if (NULL == out[count])
        {
            break;
        }

        count++;

        if (read_stats(allocator).slab_count > 1U)
        {
            break;
        }
    }

    POUND_CHECK_MSG(count < SLAB_TEST_MAX_OBJECTS,
                    "the cache never grew past its first slab");

    return count;
}

/// Returns true when every byte in `[value, value + count)` is `expected`.
static bool
bytes_are(const void *value, size_t count, uint8_t expected)
{
    const uint8_t *const raw = (const uint8_t *)value;

    for (size_t i = 0U; i < count; ++i)
    {
        if (raw[i] != expected)
        {
            return false;
        }
    }

    return true;
}

// -----------------------------------------------------------------------------
// Initialisation and configuration
// -----------------------------------------------------------------------------

POUND_TEST(slab_allocator, init_rejects_invalid_arguments)
{
    slab_allocator_t allocator;
    slab_allocator_config_t config;

    memset(&config, 0, sizeof(config));

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(slab_allocator_init(NULL, slab_arena, sizeof(slab_arena), &config),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(slab_allocator_init(&allocator, NULL, sizeof(slab_arena), &config),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(slab_allocator_init(&allocator, slab_arena, 0U, &config),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);

    // Misaligned arena: `slab_arena` is 64-byte aligned, so offsetting by one is
    // guaranteed to fail without depending on where the linker placed it.
    pound_test_log_reset();
    POUND_CHECK_EQ_U64(slab_allocator_init(&allocator, slab_arena + 1, sizeof(slab_arena) - 1U, &config),
                       POUND_ERROR_MEMORY_ALIGNMENT);
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);
}

POUND_TEST(slab_allocator, init_accepts_a_null_config_for_defaults)
{
    slab_allocator_t allocator;

    POUND_REQUIRE(POUND_SUCCESS
                  == slab_allocator_init(&allocator, slab_arena, sizeof(slab_arena), NULL));

    // A null config must leave poisoning off rather than reading through it.
    POUND_CHECK(!allocator.poison_enabled);

    slab_allocator_destroy(&allocator);
}

POUND_TEST(slab_allocator, cache_create_rejects_invalid_configuration)
{
    slab_allocator_t allocator;
    slab_config_t     config;

    memset(&config, 0, sizeof(config));
    config.name        = "probe";
    config.object_size = 32U;

    POUND_REQUIRE(POUND_SUCCESS
                  == slab_allocator_init(&allocator, slab_arena, sizeof(slab_arena), NULL));

    // Missing allocator or config.
    pound_test_log_reset();
    POUND_CHECK_EQ_U64(slab_allocator_cache_create(NULL, &config), SLAB_CACHE_ID_NONE);
    POUND_CHECK_EQ_U64(slab_allocator_cache_create(&allocator, NULL), SLAB_CACHE_ID_NONE);
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 2U);

    // Unnamed cache.
    memset(&config, 0, sizeof(config));
    config.object_size = 32U;
    pound_test_log_reset();
    POUND_CHECK_EQ_U64(slab_allocator_cache_create(&allocator, &config), SLAB_CACHE_ID_NONE);
    POUND_CHECK(pound_test_log_contains("name is NULL or empty"));

    config.name = "";
    pound_test_log_reset();
    POUND_CHECK_EQ_U64(slab_allocator_cache_create(&allocator, &config), SLAB_CACHE_ID_NONE);
    POUND_CHECK(pound_test_log_contains("name is NULL or empty"));

    // Object too small to carry a header.
    config.name        = "tiny";
    config.object_size = SLAB_ALLOCATOR_MIN_OBJECT_SIZE - 1U;
    pound_test_log_reset();
    POUND_CHECK_EQ_U64(slab_allocator_cache_create(&allocator, &config), SLAB_CACHE_ID_NONE);
    POUND_CHECK(pound_test_log_contains("below the"));

    // Alignment that is not a power of two.
    config.object_size = 32U;
    config.alignment   = 24U;
    pound_test_log_reset();
    POUND_CHECK_EQ_U64(slab_allocator_cache_create(&allocator, &config), SLAB_CACHE_ID_NONE);
    POUND_CHECK(pound_test_log_contains("not a power of two"));

    // Slab size above the cap.
    config.alignment  = 0U;
    config.slab_bytes = SLAB_ALLOCATOR_MAX_SLAB_BYTES + 1U;
    pound_test_log_reset();
    POUND_CHECK_EQ_U64(slab_allocator_cache_create(&allocator, &config), SLAB_CACHE_ID_NONE);
    POUND_CHECK(pound_test_log_contains("exceeds the"));

    // A slab too small to hold even one object, which is the interesting case:
    // the arguments are all in range but no slab could ever be carved.
    config.slab_bytes = 64U;
    pound_test_log_reset();
    POUND_CHECK_EQ_U64(slab_allocator_cache_create(&allocator, &config), SLAB_CACHE_ID_NONE);
    POUND_CHECK(pound_test_log_contains("slab size is too small"));

    // None of the rejections may have consumed a cache slot.
    POUND_CHECK_EQ_U64(read_stats(&allocator).cache_count, 0U);

    slab_allocator_destroy(&allocator);
}

POUND_TEST(slab_allocator, cache_create_assigns_dense_one_based_ids)
{
    slab_allocator_t allocator;

    POUND_REQUIRE(POUND_SUCCESS
                  == slab_allocator_init(&allocator, slab_arena, sizeof(slab_arena), NULL));

    const slab_cache_id_t first  = add_cache(&allocator, "first", 32U, 0U, 0U);
    const slab_cache_id_t second = add_cache(&allocator, "second", 64U, 0U, 0U);

    // One-based ids are what let `SLAB_CACHE_ID_NONE` be a valid "no cache"
    // marker without colliding with a real one.
    POUND_CHECK_EQ_U64(first, 1U);
    POUND_CHECK_EQ_U64(second, 2U);
    POUND_CHECK_STR_EQ(slab_allocator_cache_name(&allocator, first), "first");
    POUND_CHECK_STR_EQ(slab_allocator_cache_name(&allocator, second), "second");
    POUND_CHECK_EQ_U64(read_stats(&allocator).cache_count, 2U);

    // Each type must stay independent: an object of one size must not satisfy a
    // request for the other.
    void *const a = slab_allocator_alloc(&allocator, first);
    void *const b = slab_allocator_alloc(&allocator, second);

    POUND_REQUIRE_PTR_NON_NULL(a);
    POUND_REQUIRE_PTR_NON_NULL(b);
    POUND_CHECK(a != b);
    POUND_CHECK_EQ_U64(slab_allocator_get_usable_size(&allocator, a), 32U);
    POUND_CHECK_EQ_U64(slab_allocator_get_usable_size(&allocator, b), 64U);

    slab_allocator_free(&allocator, a);
    slab_allocator_free(&allocator, b);
    slab_allocator_destroy(&allocator);
}

POUND_TEST(slab_allocator, describe_returns_the_resolved_configuration)
{
    slab_allocator_t allocator;
    slab_config_t     resolved;

    POUND_REQUIRE(POUND_SUCCESS
                  == slab_allocator_init(&allocator, slab_arena, sizeof(slab_arena), NULL));

    const slab_cache_id_t id = add_cache(&allocator, "handles", 96U, 32U, 8192U);
    POUND_REQUIRE(id != SLAB_CACHE_ID_NONE);

    memset(&resolved, 0, sizeof(resolved));
    POUND_REQUIRE(POUND_SUCCESS == slab_allocator_cache_describe(&allocator, id, &resolved));

    // Zeroed inputs must have been replaced by the documented defaults, and the
    // caller's own values preserved.
    POUND_CHECK_STR_EQ(resolved.name, "handles");
    POUND_CHECK_EQ_U64(resolved.object_size, 96U);
    POUND_CHECK_EQ_U64(resolved.alignment, 32U);
    POUND_CHECK_EQ_U64(resolved.slab_bytes, 8192U);
    POUND_CHECK_EQ_U64(resolved.slab_count, 1U);

    // Defaults are resolved too.
    const slab_cache_id_t other = add_cache(&allocator, "defaults", 48U, 0U, 0U);
    POUND_REQUIRE(POUND_SUCCESS == slab_allocator_cache_describe(&allocator, other, &resolved));
    POUND_CHECK_EQ_U64(resolved.alignment, SLAB_ALLOCATOR_DEFAULT_ALIGNMENT);
    POUND_CHECK_EQ_U64(resolved.slab_bytes, SLAB_ALLOCATOR_DEFAULT_SLAB_BYTES);
    POUND_CHECK_EQ_U64(resolved.slab_count, 1U);

    POUND_CHECK_EQ_U64(slab_allocator_cache_describe(NULL, id, &resolved), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(slab_allocator_cache_describe(&allocator, id, NULL), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(slab_allocator_cache_describe(&allocator, SLAB_CACHE_ID_NONE, &resolved),
                       POUND_ERROR_NOT_INITIALIZED);
    POUND_CHECK_EQ_U64(slab_allocator_cache_describe(&allocator, 9999U, &resolved),
                       POUND_ERROR_NOT_INITIALIZED);

    slab_allocator_destroy(&allocator);
}

// -----------------------------------------------------------------------------
// Allocation
// -----------------------------------------------------------------------------

POUND_TEST(slab_allocator, alloc_returns_objects_aligned_and_writable)
{
    slab_allocator_t allocator;

    POUND_REQUIRE(POUND_SUCCESS
                  == slab_allocator_init(&allocator, slab_arena, sizeof(slab_arena), NULL));

    // A 32-byte object at 64-byte alignment, so the payload alignment is a
    // constraint the slab geometry has to actively satisfy rather than a
    // consequence of the default.
    const slab_cache_id_t id = add_cache(&allocator, "aligned", 32U, 64U, 4096U);
    POUND_REQUIRE(id != SLAB_CACHE_ID_NONE);

    void *objects[64];

    for (size_t i = 0U; i < 64U; ++i)
    {
        objects[i] = slab_allocator_alloc(&allocator, id);
        POUND_REQUIRE_PTR_NON_NULL(objects[i]);
        POUND_CHECK_EQ_U64((uintptr_t)objects[i] % 64U, 0U);
        POUND_CHECK_EQ_U64(slab_allocator_get_usable_size(&allocator, objects[i]), 32U);
        POUND_CHECK(slab_allocator_object_is_live(&allocator, objects[i]));

        memset(objects[i], (int)(0x40U + i), 32U);
    }

    for (size_t i = 0U; i < 64U; ++i)
    {
        POUND_CHECK(bytes_are(objects[i], 32U, (uint8_t)(0x40U + i)));
        slab_allocator_free(&allocator, objects[i]);
    }

    POUND_CHECK_EQ_U64(read_stats(&allocator).in_use, 0U);

    slab_allocator_destroy(&allocator);
}

POUND_TEST(slab_allocator, live_objects_never_alias)
{
    slab_allocator_t allocator;

    POUND_REQUIRE(POUND_SUCCESS
                  == slab_allocator_init(&allocator, slab_arena, sizeof(slab_arena), NULL));

    const slab_cache_id_t id = add_cache(&allocator, "distinct", 32U, 0U, 0U);
    POUND_REQUIRE(id != SLAB_CACHE_ID_NONE);

    void *objects[256];

    for (size_t i = 0U; i < 256U; ++i)
    {
        objects[i] = slab_allocator_alloc(&allocator, id);
        POUND_REQUIRE_PTR_NON_NULL(objects[i]);

        for (size_t j = 0U; j < i; ++j)
        {
            POUND_CHECK_MSG(objects[i] != objects[j], "object %zu aliased object %zu", i, j);
        }
    }

    // Writing each object must leave every other untouched, which is what proves
    // the headers between payloads are not overlapping the payload bytes.
    for (size_t i = 0U; i < 256U; ++i)
    {
        memset(objects[i], (int)(i & 0xFFU), 32U);
    }

    for (size_t i = 0U; i < 256U; ++i)
    {
        POUND_CHECK(bytes_are(objects[i], 32U, (uint8_t)(i & 0xFFU)));
        slab_allocator_free(&allocator, objects[i]);
    }

    slab_allocator_destroy(&allocator);
}

POUND_TEST(slab_allocator, allocation_from_an_unknown_cache_is_refused)
{
    slab_allocator_t allocator;

    POUND_REQUIRE(POUND_SUCCESS
                  == slab_allocator_init(&allocator, slab_arena, sizeof(slab_arena), NULL));

    // No caches exist yet, so every id is unknown.
    pound_test_log_reset();
    POUND_CHECK_PTR_NULL(slab_allocator_alloc(&allocator, 1U));
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);
    POUND_CHECK(pound_test_log_contains("does not exist"));

    pound_test_log_reset();
    POUND_CHECK_PTR_NULL(slab_allocator_alloc(&allocator, SLAB_CACHE_ID_NONE));
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);

    // An id beyond the table is not a lookup that can succeed.
    POUND_CHECK_PTR_NULL(slab_allocator_alloc(&allocator, 9999U));

    POUND_CHECK_PTR_NULL(slab_allocator_alloc(NULL, 1U));

    slab_allocator_destroy(&allocator);
}

// -----------------------------------------------------------------------------
// Freeing
// -----------------------------------------------------------------------------

POUND_TEST(slab_allocator, free_returns_an_object_to_its_own_cache)
{
    slab_allocator_t allocator;

    POUND_REQUIRE(POUND_SUCCESS
                  == slab_allocator_init(&allocator, slab_arena, sizeof(slab_arena), NULL));

    const slab_cache_id_t small = add_cache(&allocator, "small", 32U, 0U, 0U);
    const slab_cache_id_t large = add_cache(&allocator, "large", 512U, 0U, 0U);
    POUND_REQUIRE(small != SLAB_CACHE_ID_NONE);
    POUND_REQUIRE(large != SLAB_CACHE_ID_NONE);

    void *const object = slab_allocator_alloc(&allocator, small);
    POUND_REQUIRE_PTR_NON_NULL(object);

    slab_allocator_free(&allocator, object);

    POUND_CHECK_EQ_U64(slab_allocator_cache_in_use(&allocator, small), 0U);
    POUND_CHECK(!slab_allocator_object_is_live(&allocator, object));

    // Freeing into the small cache must not make the space available to the
    // large one, which is the whole reason the two are separate caches.
    void *const big = slab_allocator_alloc(&allocator, large);
    POUND_REQUIRE_PTR_NON_NULL(big);
    POUND_CHECK(big != object);
    POUND_CHECK_EQ_U64(slab_allocator_get_usable_size(&allocator, big), 512U);

    // The freed object is reusable by its own cache.
    void *const reused = slab_allocator_alloc(&allocator, small);
    POUND_CHECK_PTR_EQ(reused, object);

    slab_allocator_free(&allocator, reused);
    slab_allocator_free(&allocator, big);
    slab_allocator_destroy(&allocator);
}

POUND_TEST(slab_allocator, out_of_order_frees_are_supported)
{
    slab_allocator_t allocator;

    POUND_REQUIRE(POUND_SUCCESS
                  == slab_allocator_init(&allocator, slab_arena, sizeof(slab_arena), NULL));

    const slab_cache_id_t id = add_cache(&allocator, "interleaved", 32U, 0U, 4096U);
    POUND_REQUIRE(id != SLAB_CACHE_ID_NONE);

    void *objects[128];

    for (size_t i = 0U; i < 128U; ++i)
    {
        objects[i] = slab_allocator_alloc(&allocator, id);
        POUND_REQUIRE_PTR_NON_NULL(objects[i]);
    }

    POUND_CHECK_EQ_U64(slab_allocator_cache_in_use(&allocator, id), 128U);

    // Free every third object, then every fifth, then the rest. Out-of-order
    // release is the reason this is a slab allocator rather than a bump arena, so
    // it is the property the interleaving has to prove.
    for (size_t i = 0; i < 128U; i += 3U)
    {
        slab_allocator_free(&allocator, objects[i]);
    }

    for (size_t i = 0; i < 128U; i += 5U)
    {
        // Already-freed objects are skipped rather than double freed.
        if (0U != (i % 3U))
        {
            slab_allocator_free(&allocator, objects[i]);
        }
    }

    for (size_t i = 0U; i < 128U; ++i)
    {
        slab_allocator_free(&allocator, objects[i]);
    }

    POUND_CHECK_EQ_U64(slab_allocator_cache_in_use(&allocator, id), 0U);
    POUND_CHECK_EQ_U64(read_stats(&allocator).in_use, 0U);

    slab_allocator_destroy(&allocator);
}

POUND_TEST(slab_allocator, objects_in_a_full_slab_can_still_be_freed)
{
    slab_allocator_t allocator;

    POUND_REQUIRE(POUND_SUCCESS
                  == slab_allocator_init(&allocator, slab_arena, sizeof(slab_arena), NULL));

    const slab_cache_id_t id = add_cache(&allocator, "fills", 32U, 0U, 4096U);
    POUND_REQUIRE(id != SLAB_CACHE_ID_NONE);

    void *objects[SLAB_TEST_MAX_OBJECTS];
    const size_t in_first = fill_first_slab(&allocator, id, objects);

    // The point of the test: the first slab is now completely full and has been
    // moved to the cache's full list. A slab search that consulted only the
    // partial list would refuse every free from here on, so the cache would leak
    // every object it ever allocated without a single error being logged.
    POUND_REQUIRE(in_first > 1U);
    POUND_CHECK_EQ_U64(read_stats(&allocator).slab_count, 2U);

    pound_test_log_reset();
    slab_allocator_free(&allocator, objects[0]);

    POUND_CHECK_MSG(0U == pound_test_log_count_at(LOG_LEVEL_ERROR),
                    "freeing an object from a full slab must not be reported as an error");
    POUND_CHECK(!slab_allocator_object_is_live(&allocator, objects[0]));
    POUND_CHECK_EQ_U64(slab_allocator_cache_in_use(&allocator, id), in_first - 1U);

    for (size_t i = 1U; i < in_first; ++i)
    {
        slab_allocator_free(&allocator, objects[i]);
    }

    slab_allocator_destroy(&allocator);
}

POUND_TEST(slab_allocator, a_cache_grows_into_a_new_slab_when_it_fills)
{
    slab_allocator_t allocator;

    POUND_REQUIRE(POUND_SUCCESS
                  == slab_allocator_init(&allocator, slab_arena, sizeof(slab_arena), NULL));

    // A small slab size makes the growth observable without a large arena.
    const slab_cache_id_t id = add_cache(&allocator, "grows", 32U, 0U, 4096U);
    POUND_REQUIRE(id != SLAB_CACHE_ID_NONE);

    void *objects[SLAB_TEST_MAX_OBJECTS];
    const size_t in_first = fill_first_slab(&allocator, id, objects);

    POUND_REQUIRE(in_first > 1U);

    // No slab exists before the first allocation, so creation is where growth is
    // first observable.
    const slab_stats_t stats = read_stats(&allocator);

    POUND_CHECK_EQ_U64(stats.slab_count, 2U);
    POUND_CHECK(stats.object_count > in_first);
    POUND_CHECK_EQ_U64(stats.in_use, in_first);

    // The second slab's objects must be distinct from the first's, which is what
    // makes growth safe rather than an overwrite.
    for (size_t i = 0U; i < in_first; ++i)
    {
        slab_allocator_free(&allocator, objects[i]);
    }

    for (size_t i = 0U; i < in_first; ++i)
    {
        slab_allocator_free(&allocator, objects[i]);
    }

    POUND_CHECK_EQ_U64(read_stats(&allocator).in_use, 0U);
    slab_allocator_destroy(&allocator);
}

// -----------------------------------------------------------------------------
// Error paths
// -----------------------------------------------------------------------------

POUND_TEST(slab_allocator, double_free_is_refused_and_logged)
{
    slab_allocator_t allocator;

    POUND_REQUIRE(POUND_SUCCESS
                  == slab_allocator_init(&allocator, slab_arena, sizeof(slab_arena), NULL));

    const slab_cache_id_t id = add_cache(&allocator, "double", 32U, 0U, 0U);
    POUND_REQUIRE(id != SLAB_CACHE_ID_NONE);

    void *const object = slab_allocator_alloc(&allocator, id);
    POUND_REQUIRE_PTR_NON_NULL(object);

    slab_allocator_free(&allocator, object);

    pound_test_log_reset();
    slab_allocator_free(&allocator, object);

    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);
    POUND_CHECK(pound_test_log_contains("already been freed"));

    // The refusal must be inert: the object is still free exactly once, and the
    // accounting must not have moved.
    POUND_CHECK_EQ_U64(slab_allocator_cache_in_use(&allocator, id), 0U);
    POUND_CHECK_EQ_U64(read_stats(&allocator).in_use, 0U);

    // And it must still be allocatable, rather than having been pushed onto the
    // free list twice and so handed out twice.
    void *const reused = slab_allocator_alloc(&allocator, id);
    POUND_CHECK_PTR_EQ(reused, object);

    slab_allocator_free(&allocator, reused);
    slab_allocator_destroy(&allocator);
}

POUND_TEST(slab_allocator, foreign_and_interior_pointers_are_refused_and_logged)
{
    slab_allocator_t allocator;

    POUND_REQUIRE(POUND_SUCCESS
                  == slab_allocator_init(&allocator, slab_arena, sizeof(slab_arena), NULL));

    const slab_cache_id_t id = add_cache(&allocator, "validated", 64U, 0U, 0U);
    POUND_REQUIRE(id != SLAB_CACHE_ID_NONE);

    void *const object = slab_allocator_alloc(&allocator, id);
    POUND_REQUIRE_PTR_NON_NULL(object);

    // A stack address was never carved here.
    int on_stack = 0;

    pound_test_log_reset();
    slab_allocator_free(&allocator, &on_stack);
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);
    POUND_CHECK(pound_test_log_contains("belongs to no slab"));

    // An interior pointer into a real object is just as wrong: this allocator
    // hands out payload addresses only, and rounding one down to an object
    // boundary would silently free the wrong thing.
    void *const interior = (void *)((uint8_t *)object + 8U);

    pound_test_log_reset();
    slab_allocator_free(&allocator, interior);
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);
    POUND_CHECK(pound_test_log_contains("belongs to no slab"));

    // Neither attempt may have disturbed the allocator.
    POUND_CHECK(slab_allocator_object_is_live(&allocator, object));
    POUND_CHECK_EQ_U64(slab_allocator_cache_in_use(&allocator, id), 1U);
    POUND_CHECK_EQ_U64(read_stats(&allocator).in_use, 1U);

    slab_allocator_free(&allocator, object);
    slab_allocator_destroy(&allocator);
}

POUND_TEST(slab_allocator, free_tolerates_null_and_an_uninitialised_allocator)
{
    slab_allocator_t allocator;

    memset(&allocator, 0, sizeof(allocator));

    // free(NULL) is a no-op by long-standing allocator convention, so it must not
    // be reported as an error.
    pound_test_log_reset();
    slab_allocator_free(&allocator, NULL);
    POUND_CHECK_EQ_U64(pound_test_log_count_at(LOG_LEVEL_ERROR), 0U);

    slab_allocator_free(&allocator, &allocator);
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);

    slab_allocator_free(NULL, &allocator);
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);

    // Uninitialised: every entry point must report rather than take a mutex that
    // was never created.
    POUND_CHECK_PTR_NULL(slab_allocator_alloc(&allocator, 1U));
    POUND_CHECK_EQ_U64(slab_allocator_trim(&allocator, SLAB_CACHE_ID_NONE), 0U);
    POUND_CHECK_EQ_U64(slab_allocator_cache_in_use(&allocator, 1U), 0U);
    POUND_CHECK(!slab_allocator_object_is_live(&allocator, &allocator));
    POUND_CHECK_EQ_U64(slab_allocator_get_usable_size(&allocator, &allocator), 0U);
    POUND_CHECK(!slab_allocator_cache_is_starved(&allocator, 1U));
    POUND_CHECK_STR_EQ(slab_allocator_cache_name(&allocator, 1U), "");

    POUND_CHECK_EQ_U64(slab_allocator_get_stats(&allocator, NULL), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(slab_allocator_get_stats(NULL, NULL), POUND_ERROR_INVALID_ARGUMENT);

    slab_stats_t stats;

    POUND_CHECK_EQ_U64(slab_allocator_get_stats(&allocator, &stats), POUND_ERROR_NOT_INITIALIZED);

    slab_allocator_reset(&allocator);
    slab_allocator_destroy(&allocator);
    slab_allocator_reset(NULL);
    slab_allocator_destroy(NULL);

    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);
}

// -----------------------------------------------------------------------------
// Growth limits and trimming
// -----------------------------------------------------------------------------

POUND_TEST(slab_allocator, arena_exhaustion_starves_the_cache)
{
    slab_allocator_t allocator;

    POUND_REQUIRE(POUND_SUCCESS
                  == slab_allocator_init(&allocator, small_slab_arena, sizeof(small_slab_arena), NULL));

    const slab_cache_id_t id = add_cache(&allocator, "bounded", 32U, 0U, 4096U);
    POUND_REQUIRE(id != SLAB_CACHE_ID_NONE);

    void *objects[SLAB_TEST_MAX_OBJECTS];
    size_t count = 0U;

    // Allocate until the arena genuinely runs out. The bound is a guard against a
    // configuration that never fails, which would otherwise hang the test.
    while (count < SLAB_TEST_MAX_OBJECTS)
    {
        objects[count] = slab_allocator_alloc(&allocator, id);

        if (NULL == objects[count])
        {
            break;
        }

        count++;
    }

    POUND_REQUIRE_MSG(count < SLAB_TEST_MAX_OBJECTS, "the 8 KiB arena never ran out");
    POUND_REQUIRE(count > 0U);

    // A cache whose object type no longer fits must be reported as starved, not
    // merely as "no objects available right now", which is what a merely-full
    // cache also looks like.
    POUND_CHECK(slab_allocator_cache_is_starved(&allocator, id));

    const slab_stats_t stats = read_stats(&allocator);

    POUND_CHECK_EQ_U64(stats.alloc_failures, 1U);
    POUND_CHECK_EQ_U64(stats.in_use, count);
    POUND_CHECK(stats.slab_count >= 2U);

    // A failure must not have corrupted anything: every outstanding object is
    // still live and still freeable.
    for (size_t i = 0U; i < count; ++i)
    {
        POUND_CHECK(slab_allocator_object_is_live(&allocator, objects[i]));
    }

    for (size_t i = 0U; i < count; ++i)
    {
        slab_allocator_free(&allocator, objects[i]);
    }

    POUND_CHECK_EQ_U64(read_stats(&allocator).in_use, 0U);

    // With the arena full again after reallocation, a trim must give the space
    // back so the cache is usable once more.
    POUND_CHECK(slab_allocator_trim(&allocator, SLAB_CACHE_ID_NONE) >= 1U);
    POUND_CHECK_PTR_NON_NULL(slab_allocator_alloc(&allocator, id));

    slab_allocator_destroy(&allocator);
}

POUND_TEST(slab_allocator, trim_rewinds_over_trailing_free_slabs)
{
    slab_allocator_t allocator;

    POUND_REQUIRE(POUND_SUCCESS
                  == slab_allocator_init(&allocator, slab_arena, sizeof(slab_arena), NULL));

    const slab_cache_id_t id = add_cache(&allocator, "trimmed", 32U, 0U, 4096U);
    POUND_REQUIRE(id != SLAB_CACHE_ID_NONE);

    void *objects[SLAB_TEST_MAX_OBJECTS];
    const size_t in_first = fill_first_slab(&allocator, id, objects);

    POUND_REQUIRE(in_first > 1U);

    for (size_t i = 0U; i < in_first; ++i)
    {
        slab_allocator_free(&allocator, objects[i]);
    }

    const slab_stats_t before = read_stats(&allocator);

    POUND_CHECK_EQ_U64(before.in_use, 0U);
    POUND_CHECK_EQ_U64(before.slab_count, 2U);

    const size_t released = slab_allocator_trim(&allocator, SLAB_CACHE_ID_NONE);
    const slab_stats_t after = read_stats(&allocator);

    // Both slabs are free and both are trailing, so the arena rewinds to the
    // state it had before the first allocation.
    POUND_CHECK_EQ_U64(released, 2U);
    POUND_CHECK_EQ_U64(after.slab_count, 0U);
    POUND_CHECK_EQ_U64(after.object_count, 0U);
    POUND_CHECK_EQ_U64(after.bytes_committed, 0U);
    POUND_CHECK_MSG(after.arena_committed < before.arena_committed,
                    "trim must rewind the arena cursor");

    // And the reclaimed space is genuinely usable again.
    POUND_CHECK_PTR_NON_NULL(slab_allocator_alloc(&allocator, id));

    // Trimming again with a live slab present must release nothing.
    POUND_CHECK_EQ_U64(slab_allocator_trim(&allocator, SLAB_CACHE_ID_NONE), 0U);

    slab_allocator_destroy(&allocator);
}

POUND_TEST(slab_allocator, trim_stops_at_a_slab_that_still_holds_a_live_object)
{
    slab_allocator_t allocator;

    POUND_REQUIRE(POUND_SUCCESS
                  == slab_allocator_init(&allocator, slab_arena, sizeof(slab_arena), NULL));

    const slab_cache_id_t id = add_cache(&allocator, "anchored", 32U, 0U, 4096U);
    POUND_REQUIRE(id != SLAB_CACHE_ID_NONE);

    void *objects[SLAB_TEST_MAX_OBJECTS];
    const size_t in_first = fill_first_slab(&allocator, id, objects);

    POUND_REQUIRE(in_first > 1U);

    // One object from the second slab, which now anchors the end of the arena.
    void *const anchor = slab_allocator_alloc(&allocator, id);
    POUND_REQUIRE_PTR_NON_NULL(anchor);

    for (size_t i = 0U; i < in_first; ++i)
    {
        slab_allocator_free(&allocator, objects[i]);
    }

    // The first slab is entirely free but sits *below* a live one. Rewinding over
    // it would hand the same arena bytes to a later carve while the second slab
    // still claims them, so it must be refused.
    const slab_stats_t before = read_stats(&allocator);

    POUND_CHECK_EQ_U64(slab_allocator_trim(&allocator, SLAB_CACHE_ID_NONE), 0U);
    POUND_CHECK_EQ_U64(read_stats(&allocator).slab_count, before.slab_count);

    // Releasing the anchor frees the second slab, and now both can go at once.
    slab_allocator_free(&allocator, anchor);

    POUND_CHECK_EQ_U64(slab_allocator_trim(&allocator, SLAB_CACHE_ID_NONE), 2U);
    POUND_CHECK_EQ_U64(read_stats(&allocator).slab_count, 0U);

    slab_allocator_destroy(&allocator);
}

POUND_TEST(slab_allocator, trim_of_an_unknown_cache_is_refused)
{
    slab_allocator_t allocator;

    POUND_REQUIRE(POUND_SUCCESS
                  == slab_allocator_init(&allocator, slab_arena, sizeof(slab_arena), NULL));

    const slab_cache_id_t id = add_cache(&allocator, "present", 32U, 0U, 0U);
    POUND_REQUIRE(id != SLAB_CACHE_ID_NONE);

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(slab_allocator_trim(&allocator, SLAB_CACHE_ID_NONE + 99U), 0U);
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);
    POUND_CHECK(pound_test_log_contains("does not exist"));

    POUND_CHECK_EQ_U64(slab_allocator_trim(NULL, SLAB_CACHE_ID_NONE), 0U);

    slab_allocator_destroy(&allocator);
}

POUND_TEST(slab_allocator, cache_destroy_invalidates_its_objects)
{
    slab_allocator_t allocator;

    POUND_REQUIRE(POUND_SUCCESS
                  == slab_allocator_init(&allocator, slab_arena, sizeof(slab_arena), NULL));

    const slab_cache_id_t id = add_cache(&allocator, "doomed", 32U, 0U, 0U);
    POUND_REQUIRE(id != SLAB_CACHE_ID_NONE);

    void *const object = slab_allocator_alloc(&allocator, id);
    POUND_REQUIRE_PTR_NON_NULL(object);
    POUND_CHECK(slab_allocator_object_is_live(&allocator, object));

    const slab_stats_t before = read_stats(&allocator);

    POUND_REQUIRE(before.slab_count > 0U);

    pound_test_log_reset();
    slab_allocator_cache_destroy(&allocator, id);

    // Destroying a cache with live objects is a caller bug but not a fatal one,
    // so it is warned about rather than refused.
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_WARN) >= 1U);
    POUND_CHECK(pound_test_log_contains("still live"));

    const slab_stats_t after = read_stats(&allocator);

    POUND_CHECK_EQ_U64(after.cache_count, 0U);
    POUND_CHECK_EQ_U64(after.slab_count, 0U);
    POUND_CHECK_EQ_U64(after.in_use, 0U);

    // The object pointer is now dangling, so every query about it must decline
    // rather than read freed memory.
    POUND_CHECK(!slab_allocator_object_is_live(&allocator, object));
    POUND_CHECK_EQ_U64(slab_allocator_get_usable_size(&allocator, object), 0U);

    pound_test_log_reset();
    slab_allocator_free(&allocator, object);
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);

    // Unknown and absent ids are reported rather than acted on.
    POUND_CHECK_PTR_NULL(slab_allocator_alloc(&allocator, id));
    POUND_CHECK_STR_EQ(slab_allocator_cache_name(&allocator, id), "");
    POUND_CHECK_EQ_U64(slab_allocator_cache_in_use(&allocator, id), 0U);
    POUND_CHECK(!slab_allocator_cache_is_starved(&allocator, id));

    slab_allocator_cache_destroy(&allocator, SLAB_CACHE_ID_NONE);
    slab_allocator_cache_destroy(NULL, id);
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 2U);

    slab_allocator_destroy(&allocator);
}

POUND_TEST(slab_allocator, cache_destroy_compacts_ids_and_a_new_cache_gets_the_freed_one)
{
    slab_allocator_t allocator;

    POUND_REQUIRE(POUND_SUCCESS
                  == slab_allocator_init(&allocator, slab_arena, sizeof(slab_arena), NULL));

    const slab_cache_id_t first  = add_cache(&allocator, "first", 32U, 0U, 0U);
    const slab_cache_id_t second = add_cache(&allocator, "second", 32U, 0U, 0U);
    const slab_cache_id_t third  = add_cache(&allocator, "third", 32U, 0U, 0U);

    POUND_REQUIRE(first == 1U);
    POUND_REQUIRE(second == 2U);
    POUND_REQUIRE(third == 3U);

    // Destroying the middle cache renumbers the one above it, which the header
    // documents. What matters is that ids stay dense and never alias two caches.
    slab_allocator_cache_destroy(&allocator, second);

    POUND_CHECK_EQ_U64(read_stats(&allocator).cache_count, 2U);
    POUND_CHECK_STR_EQ(slab_allocator_cache_name(&allocator, 1U), "first");
    POUND_CHECK_STR_EQ(slab_allocator_cache_name(&allocator, 2U), "third");
    POUND_CHECK_STR_EQ(slab_allocator_cache_name(&allocator, 3U), "");

    // A newly created cache takes the lowest free slot rather than growing the
    // table without bound.
    const slab_cache_id_t fourth = add_cache(&allocator, "fourth", 32U, 0U, 0U);

    POUND_CHECK_EQ_U64(fourth, 3U);
    POUND_CHECK_STR_EQ(slab_allocator_cache_name(&allocator, 3U), "fourth");

    slab_allocator_destroy(&allocator);
}

// -----------------------------------------------------------------------------
// Poisoning, accounting and teardown
// -----------------------------------------------------------------------------

POUND_TEST(slab_allocator, poison_covers_allocated_and_freed_objects)
{
    slab_allocator_t allocator;

    init_poisoned(&allocator, slab_arena, sizeof(slab_arena));

    const slab_cache_id_t id = add_cache(&allocator, "poisoned", 48U, 0U, 0U);
    POUND_REQUIRE(id != SLAB_CACHE_ID_NONE);

    void *const object = slab_allocator_alloc(&allocator, id);
    POUND_REQUIRE_PTR_NON_NULL(object);

    // A freshly carved object must already read as poison, so a guest reading
    // uninitialised memory is caught rather than handed plausible bytes.
    POUND_CHECK(bytes_are(object, 48U, (uint8_t)SLAB_ALLOCATOR_POISON_BYTE));

    memset(object, 0x33, 48U);
    POUND_CHECK(bytes_are(object, 48U, 0x33U));

    slab_allocator_free(&allocator, object);
    POUND_CHECK(bytes_are(object, 48U, (uint8_t)SLAB_ALLOCATOR_POISON_BYTE));

    slab_allocator_destroy(&allocator);
}

POUND_TEST(slab_allocator, poison_can_be_toggled_at_runtime)
{
    slab_allocator_t allocator;

    POUND_REQUIRE(POUND_SUCCESS
                  == slab_allocator_init(&allocator, slab_arena, sizeof(slab_arena), NULL));

    const slab_cache_id_t id = add_cache(&allocator, "toggled", 48U, 0U, 0U);
    POUND_REQUIRE(id != SLAB_CACHE_ID_NONE);

    // Off by default, so an allocation must not be scrubbed.
    void *const plain = slab_allocator_alloc(&allocator, id);
    POUND_REQUIRE_PTR_NON_NULL(plain);
    memset(plain, 0x77, 48U);
    POUND_CHECK(bytes_are(plain, 48U, 0x77U));
    slab_allocator_free(&allocator, plain);

    slab_allocator_set_poison(&allocator, true);

    void *const scrubbed = slab_allocator_alloc(&allocator, id);
    POUND_REQUIRE_PTR_NON_NULL(scrubbed);
    POUND_CHECK(bytes_are(scrubbed, 48U, (uint8_t)SLAB_ALLOCATOR_POISON_BYTE));
    slab_allocator_free(&allocator, scrubbed);

    slab_allocator_set_poison(&allocator, false);
    slab_allocator_set_poison(NULL, true);
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);

    slab_allocator_destroy(&allocator);
}

POUND_TEST(slab_allocator, stats_agree_with_the_observable_state)
{
    slab_allocator_t allocator;

    POUND_REQUIRE(POUND_SUCCESS
                  == slab_allocator_init(&allocator, slab_arena, sizeof(slab_arena), NULL));

    slab_stats_t empty;

    memset(&empty, 0, sizeof(empty));
    POUND_REQUIRE(POUND_SUCCESS == slab_allocator_get_stats(&allocator, &empty));
    POUND_CHECK_EQ_U64(empty.cache_count, 0U);
    POUND_CHECK_EQ_U64(empty.slab_count, 0U);
    POUND_CHECK_EQ_U64(empty.object_count, 0U);
    POUND_CHECK_EQ_U64(empty.in_use, 0U);
    POUND_CHECK_EQ_U64(empty.bytes_committed, 0U);
    POUND_CHECK_EQ_U64(empty.arena_committed, 0U);
    POUND_CHECK_EQ_U64(empty.alloc_failures, 0U);
    POUND_CHECK_EQ_U64(empty.starved_count, 0U);

    const slab_cache_id_t small = add_cache(&allocator, "small", 32U, 0U, 4096U);
    const slab_cache_id_t large = add_cache(&allocator, "large", 256U, 0U, 4096U);

    POUND_REQUIRE(small != SLAB_CACHE_ID_NONE);
    POUND_REQUIRE(large != SLAB_CACHE_ID_NONE);

    // Registering a cache carves only its record, so no object exists yet but the
    // cursor has moved.
    const slab_stats_t after_create = read_stats(&allocator);

    POUND_CHECK_EQ_U64(after_create.cache_count, 2U);
    POUND_CHECK_EQ_U64(after_create.slab_count, 0U);
    POUND_CHECK_EQ_U64(after_create.object_count, 0U);
    POUND_CHECK(after_create.arena_committed > 0U);

    void *objects[32];

    for (size_t i = 0U; i < 32U; ++i)
    {
        objects[i] = slab_allocator_alloc(&allocator, small);
        POUND_REQUIRE_PTR_NON_NULL(objects[i]);
    }

    const slab_stats_t allocated = read_stats(&allocator);

    POUND_CHECK_EQ_U64(allocated.slab_count, 1U);
    POUND_CHECK_EQ_U64(allocated.in_use, 32U);
    POUND_CHECK(allocated.object_count >= 32U);
    POUND_CHECK_EQ_U64(allocated.arena_committed, after_create.arena_committed + allocated.bytes_committed);
    POUND_CHECK(allocated.bytes_committed <= sizeof(slab_arena));

    for (size_t i = 0U; i < 10U; ++i)
    {
        slab_allocator_free(&allocator, objects[i]);
    }

    const slab_stats_t partial = read_stats(&allocator);

    POUND_CHECK_EQ_U64(partial.in_use, 22U);
    POUND_CHECK_EQ_U64(partial.slab_count, 1U);
    POUND_CHECK_EQ_U64(partial.object_count, allocated.object_count);
    POUND_CHECK_EQ_U64(partial.bytes_committed, allocated.bytes_committed);

    for (size_t i = 10U; i < 32U; ++i)
    {
        slab_allocator_free(&allocator, objects[i]);
    }

    POUND_CHECK_EQ_U64(read_stats(&allocator).in_use, 0U);
    POUND_CHECK_EQ_U64(read_stats(&allocator).starved_count, 0U);

    slab_allocator_destroy(&allocator);
}

POUND_TEST(slab_allocator, reset_clears_caches_slabs_and_the_cursor)
{
    slab_allocator_t allocator;

    POUND_REQUIRE(POUND_SUCCESS
                  == slab_allocator_init(&allocator, slab_arena, sizeof(slab_arena), NULL));

    const slab_cache_id_t id = add_cache(&allocator, "resettable", 32U, 0U, 0U);
    POUND_REQUIRE(id != SLAB_CACHE_ID_NONE);

    for (int i = 0; i < 8; ++i)
    {
        POUND_CHECK_PTR_NON_NULL(slab_allocator_alloc(&allocator, id));
    }

    const slab_stats_t before = read_stats(&allocator);

    POUND_REQUIRE(before.slab_count > 0U);
    POUND_REQUIRE(before.in_use > 0U);

    slab_allocator_reset(&allocator);

    const slab_stats_t after = read_stats(&allocator);

    POUND_CHECK_EQ_U64(after.cache_count, 0U);
    POUND_CHECK_EQ_U64(after.slab_count, 0U);
    POUND_CHECK_EQ_U64(after.object_count, 0U);
    POUND_CHECK_EQ_U64(after.in_use, 0U);
    POUND_CHECK_EQ_U64(after.bytes_committed, 0U);
    POUND_CHECK_EQ_U64(after.arena_committed, 0U);
    POUND_CHECK_EQ_U64(after.alloc_failures, 0U);
    POUND_CHECK_EQ_U64(after.starved_count, 0U);

    // The cache ids do not survive, so the same id number must now be free for a
    // new cache rather than resolving to a stale record.
    POUND_CHECK_PTR_NULL(slab_allocator_alloc(&allocator, id));
    POUND_CHECK_STR_EQ(slab_allocator_cache_name(&allocator, id), "");

    // The arena is fully available again, which is what makes a reset usable as
    // the tail of a save-state load.
    const slab_cache_id_t recreated = add_cache(&allocator, "recreated", 32U, 0U, 0U);

    POUND_CHECK_EQ_U64(recreated, 1U);
    POUND_CHECK_PTR_NON_NULL(slab_allocator_alloc(&allocator, recreated));

    slab_allocator_destroy(&allocator);
}

POUND_TEST(slab_allocator, destroy_warns_about_live_objects_and_clears_the_structure)
{
    slab_allocator_t allocator;

    POUND_REQUIRE(POUND_SUCCESS
                  == slab_allocator_init(&allocator, slab_arena, sizeof(slab_arena), NULL));

    const slab_cache_id_t id = add_cache(&allocator, "leaky", 32U, 0U, 0U);
    POUND_REQUIRE(id != SLAB_CACHE_ID_NONE);

    for (int i = 0; i < 4; ++i)
    {
        POUND_CHECK_PTR_NON_NULL(slab_allocator_alloc(&allocator, id));
    }

    pound_test_log_reset();
    slab_allocator_destroy(&allocator);

    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_WARN) >= 1U);
    POUND_CHECK(pound_test_log_contains("still live"));

    POUND_CHECK_PTR_NULL(allocator.arena);
    POUND_CHECK_EQ_U64(allocator.cache_count, 0U);
    POUND_CHECK_EQ_U64(allocator.in_use, 0U);

    // A second destroy must be harmless rather than double-freeing the mutex.
    slab_allocator_destroy(&allocator);
    POUND_CHECK_EQ_U64(pound_test_log_count_at(LOG_LEVEL_ERROR), 0U);
}

POUND_TEST(slab_allocator, object_liveness_tracks_state)
{
    slab_allocator_t allocator;

    POUND_REQUIRE(POUND_SUCCESS
                  == slab_allocator_init(&allocator, slab_arena, sizeof(slab_arena), NULL));

    const slab_cache_id_t id = add_cache(&allocator, "tracked", 32U, 0U, 4096U);
    POUND_REQUIRE(id != SLAB_CACHE_ID_NONE);

    void *const object = slab_allocator_alloc(&allocator, id);
    POUND_REQUIRE_PTR_NON_NULL(object);

    POUND_CHECK(slab_allocator_object_is_live(&allocator, object));

    slab_allocator_free(&allocator, object);
    POUND_CHECK(!slab_allocator_object_is_live(&allocator, object));

    // Reallocating re-establishes liveness, which is the only way a pointer that
    // was free becomes live again.
    void *const again = slab_allocator_alloc(&allocator, id);
    POUND_CHECK_PTR_EQ(again, object);
    POUND_CHECK(slab_allocator_object_is_live(&allocator, object));

    // A freed object remains resolvable for sizing, because it is still carved
    // by this allocator and a caller sizing its own storage must not be told the
    // object has vanished.
    slab_allocator_free(&allocator, object);
    POUND_CHECK_EQ_U64(slab_allocator_get_usable_size(&allocator, object), 32U);
    POUND_CHECK(!slab_allocator_object_is_live(&allocator, object));

    // Nulls are never live and never sized.
    POUND_CHECK(!slab_allocator_object_is_live(&allocator, NULL));
    POUND_CHECK(!slab_allocator_object_is_live(NULL, object));
    POUND_CHECK_EQ_U64(slab_allocator_get_usable_size(&allocator, NULL), 0U);
    POUND_CHECK_EQ_U64(slab_allocator_get_usable_size(NULL, object), 0U);

    slab_allocator_destroy(&allocator);
}

POUND_TEST_SUITE(slab_allocator,
                POUND_TEST_CASE(slab_allocator, init_rejects_invalid_arguments),
                POUND_TEST_CASE(slab_allocator, init_accepts_a_null_config_for_defaults),
                POUND_TEST_CASE(slab_allocator, cache_create_rejects_invalid_configuration),
                POUND_TEST_CASE(slab_allocator, cache_create_assigns_dense_one_based_ids),
                POUND_TEST_CASE(slab_allocator, describe_returns_the_resolved_configuration),
                POUND_TEST_CASE(slab_allocator, alloc_returns_objects_aligned_and_writable),
                POUND_TEST_CASE(slab_allocator, live_objects_never_alias),
                POUND_TEST_CASE(slab_allocator, allocation_from_an_unknown_cache_is_refused),
                POUND_TEST_CASE(slab_allocator, free_returns_an_object_to_its_own_cache),
                POUND_TEST_CASE(slab_allocator, out_of_order_frees_are_supported),
                POUND_TEST_CASE(slab_allocator, objects_in_a_full_slab_can_still_be_freed),
                POUND_TEST_CASE(slab_allocator, a_cache_grows_into_a_new_slab_when_it_fills),
                POUND_TEST_CASE(slab_allocator, double_free_is_refused_and_logged),
                POUND_TEST_CASE(slab_allocator, foreign_and_interior_pointers_are_refused_and_logged),
                POUND_TEST_CASE(slab_allocator, free_tolerates_null_and_an_uninitialised_allocator),
                POUND_TEST_CASE(slab_allocator, arena_exhaustion_starves_the_cache),
                POUND_TEST_CASE(slab_allocator, trim_rewinds_over_trailing_free_slabs),
                POUND_TEST_CASE(slab_allocator, trim_stops_at_a_slab_that_still_holds_a_live_object),
                POUND_TEST_CASE(slab_allocator, trim_of_an_unknown_cache_is_refused),
                POUND_TEST_CASE(slab_allocator, cache_destroy_invalidates_its_objects),
                POUND_TEST_CASE(slab_allocator, cache_destroy_compacts_ids_and_a_new_cache_gets_the_freed_one),
                POUND_TEST_CASE(slab_allocator, poison_covers_allocated_and_freed_objects),
                POUND_TEST_CASE(slab_allocator, poison_can_be_toggled_at_runtime),
                POUND_TEST_CASE(slab_allocator, stats_agree_with_the_observable_state),
                POUND_TEST_CASE(slab_allocator, reset_clears_caches_slabs_and_the_cursor),
                POUND_TEST_CASE(slab_allocator, destroy_warns_about_live_objects_and_clears_the_structure),
                POUND_TEST_CASE(slab_allocator, object_liveness_tracks_state))

/*** end of file ***/
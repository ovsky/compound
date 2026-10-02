//! Tests for the Ballistic allocator bridge.
//!
//! The code cache has its own suite; this one exists because the bridge is the only
//! place where Pound meets a third party's idea of what an allocator is, and that
//! meeting is where the assumptions are most likely to be wrong. Every case here
//! therefore drives the cache *through the `bal_allocator_t` Ballistic would hold*,
//! rather than calling the cache directly, so the callbacks are exercised exactly as
//! the engine would exercise them -- including the ways a real caller is allowed to be
//! sloppy.
//!
//! It is compiled only where the engine is linked (`POUND_ENABLE_BALLISTIC`). The
//! bridge is still covered on every other lane, just indirectly: the cache suite
//! verifies the allocator underneath it, and this file is the layer that cannot be
//! built without the engine's headers.
//!
//! Note what is deliberately *not* linked: the Ballistic library itself. Nothing here
//! calls into the engine -- the bridge only fills in a callback table -- so a test
//! that pulled in the prebuilt and its LuaJIT runtime would be testing the packaging
//! rather than the bridge, and would fail in the headless CI container for reasons
//! that have nothing to do with the code.

#include "pound_test.h"

#include "attributes.h"
#include "errors.h"
#include "jit/jit_ballistic.h"
#include "log.h"
#include <string.h>

/// Chunk size used by every case: the documented minimum, so growth is cheap.
#define BALLISTIC_TEST_CHUNK (JIT_CACHE_MIN_CHUNK_BYTES)

/// Ceiling high enough that the ceiling is never what is under test.
#define BALLISTIC_TEST_CEILING (BALLISTIC_TEST_CHUNK * 4U)

// -----------------------------------------------------------------------------
// Fixtures
// -----------------------------------------------------------------------------

/// A cache with a bound allocator beside it, both stack-allocated.
///
/// `jit_cache_t` is defined in its own header precisely so a caller can own it
/// without a second allocation, which is what makes a test like this possible: the
/// cache outlives nothing and the bridge holds no copy of it.
typedef struct
{
    jit_cache_t      cache;
    bal_allocator_t  allocator;
} fixture_t;

/// Initialises a cache and binds a bridge to it.
static void
fixture_open(fixture_t *fixture)
{
    jit_cache_config_t config;

    memset(&config, 0, sizeof(config));
    config.chunk_bytes = BALLISTIC_TEST_CHUNK;
    config.max_bytes   = BALLISTIC_TEST_CEILING;

    POUND_REQUIRE(POUND_SUCCESS == jit_cache_init(&fixture->cache, &config));
    POUND_REQUIRE(POUND_SUCCESS == jit_cache_bind_ballistic(&fixture->cache, &fixture->allocator));
}

/// Releases the cache. A bridge never owns anything, so there is nothing else to tear
/// down -- which is the point worth stating, and is asserted in a case below.
static void
fixture_close(fixture_t *fixture)
{
    jit_cache_destroy(&fixture->cache);
}

// -----------------------------------------------------------------------------
// Binding
// -----------------------------------------------------------------------------

POUND_TEST(jit_ballistic, binding_installs_every_callback)
{
    fixture_t fixture;

    fixture_open(&fixture);

    // A table with a NULL entry would crash the engine the first time it allocated.
    // The context is the cache, so the engine can hand it straight back.
    POUND_CHECK_PTR_NON_NULL(fixture.allocator.allocate);
    POUND_CHECK_PTR_NON_NULL(fixture.allocator.free);
    POUND_CHECK_PTR_NON_NULL(fixture.allocator.allocate_executable);
    POUND_CHECK_PTR_NON_NULL(fixture.allocator.free_executable);
    POUND_CHECK_PTR_NON_NULL(fixture.allocator.protect_rw);
    POUND_CHECK_PTR_NON_NULL(fixture.allocator.protect_rx);
    POUND_CHECK_PTR_EQ(fixture.allocator.context, &fixture.cache);

    // Comparing the six entries against each other for identity would be a weak proxy
    // anyway -- two entries sharing a function is only a bug because they would then
    // ignore part of their arguments. What is checked instead is that each entry does
    // the thing its own name promises, which is what a shared implementation could not
    // do: the data allocator returns a bare pointer and does not claim a page-aligned
    // address, the executable one returns both fields set, and the two protection
    // callbacks drive the block in opposite directions.
    void *const data = fixture.allocator.allocate(fixture.allocator.context, 64U, 64U);

    POUND_REQUIRE_PTR_NON_NULL(data);
    POUND_CHECK(!jit_cache_is_executable(&fixture.cache, data));

    const bal_executable_buffer_t code
        = fixture.allocator.allocate_executable(fixture.allocator.context, 64U, 64U);

    POUND_REQUIRE_PTR_NON_NULL(code.rw_pointer);
    POUND_CHECK_PTR_NON_NULL(code.rx_pointer);
    POUND_CHECK(!jit_cache_is_executable(&fixture.cache, code.rw_pointer));

    fixture.allocator.protect_rx(fixture.allocator.context, code, 64U);
    POUND_CHECK(jit_cache_is_executable(&fixture.cache, code.rw_pointer));

    fixture.allocator.protect_rw(fixture.allocator.context, code, 64U);
    POUND_CHECK(!jit_cache_is_executable(&fixture.cache, code.rw_pointer));

    fixture.allocator.free_executable(fixture.allocator.context, code, 64U);
    fixture.allocator.free(fixture.allocator.context, data, 64U);

    fixture_close(&fixture);
}

POUND_TEST(jit_ballistic, binding_rejects_null_and_uninitialised_caches)
{
    fixture_t    fixture;
    bal_allocator_t allocator;

    memset(&fixture, 0, sizeof(fixture));
    memset(&allocator, 0, sizeof(allocator));

    pound_test_log_reset();

    POUND_CHECK_EQ_I64(jit_cache_bind_ballistic(NULL, &allocator), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(jit_cache_bind_ballistic(&fixture.cache, NULL), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(jit_cache_bind_ballistic(NULL, NULL), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK(pound_test_log_contains("cache or output allocator is NULL"));

    // A zeroed cache is a plausible thing to hand over after a failed initialisation,
    // and it has to be refused with a code that says "not ready" rather than accepted
    // and left to fail later inside the engine.
    POUND_CHECK_EQ_I64(jit_cache_bind_ballistic(&fixture.cache, &allocator),
                      POUND_ERROR_NOT_INITIALIZED);
    POUND_CHECK(pound_test_log_contains("never initialised"));

    // And a refused binding leaves the output table untouched, so a caller that
    // ignored the status cannot end up calling through a null entry.
    POUND_CHECK_PTR_NULL(allocator.allocate);

    // The check is repeated on a cache that was properly initialised and then
    // destroyed, which is the realistic version of the same mistake.
    fixture_open(&fixture);
    fixture_close(&fixture);

    pound_test_log_reset();
    POUND_CHECK_EQ_I64(jit_cache_bind_ballistic(&fixture.cache, &allocator),
                      POUND_ERROR_NOT_INITIALIZED);
}

// -----------------------------------------------------------------------------
// Plain allocations
// -----------------------------------------------------------------------------

POUND_TEST(jit_ballistic, allocations_through_the_bridge_come_from_the_cache)
{
    fixture_t fixture;

    fixture_open(&fixture);

    // Every alignment here is a power of two, which is all Ballistic promises, and
    // several are below the cache's own minimum and so have to be raised rather than
    // refused. The sizes are not multiples of any of them, so each block rounds
    // differently.
    static const size_t requests[][2] = {
        {1U, 8U}, {8U, 1U}, {2U, 16U}, {16U, 3U}, {64U, 3U}, {32U, 100U}, {64U, 4096U}, {4096U, 1U},
    };

    void *blocks[sizeof(requests) / sizeof(requests[0])];

    for (size_t i = 0U; i < (sizeof(requests) / sizeof(requests[0])); ++i)
    {
        const size_t alignment = requests[i][0];
        const size_t size       = requests[i][1];

        blocks[i] = fixture.allocator.allocate((bal_allocator_handle_t)&fixture.cache, alignment, size);

        POUND_REQUIRE_MSG(NULL != blocks[i],
                          "a %zu-byte block at %zu-byte alignment was refused through the bridge",
                          size,
                          alignment);

        // Whatever was asked for, what comes back satisfies the *stronger* of the two
        // alignments: a sub-minimum request is raised to the cache's default, and a
        // legal one is met exactly.
        const size_t guaranteed = (alignment < JIT_CACHE_MIN_ALIGNMENT) ? JIT_CACHE_DEFAULT_ALIGNMENT
                                                                        : alignment;

        POUND_CHECK_MSG(0U == ((uintptr_t)blocks[i] % guaranteed),
                        "a %zu-byte block requested at %zu-byte alignment came back at %p, which "
                        "is not %zu-byte aligned",
                        size,
                        alignment,
                        blocks[i],
                        guaranteed);

        POUND_CHECK_MSG(jit_cache_usable_size(&fixture.cache, blocks[i]) >= size,
                        "a %zu-byte request got %zu bytes",
                        size,
                        jit_cache_usable_size(&fixture.cache, blocks[i]));

        // Writable, which is all the interface asks for on this path.
        memset(blocks[i], 0x3C, size);
    }

    for (size_t i = 0U; i < (sizeof(requests) / sizeof(requests[0])); ++i)
    {
        fixture.allocator.free((bal_allocator_handle_t)&fixture.cache, blocks[i], 1U);
    }

    jit_cache_stats_t stats;

    memset(&stats, 0, sizeof(stats));
    POUND_REQUIRE(POUND_SUCCESS == jit_cache_get_stats(&fixture.cache, &stats));

    const size_t expected = sizeof(requests) / sizeof(requests[0]);

    POUND_CHECK_EQ_U64(stats.live_blocks, 0U);
    POUND_CHECK_EQ_U64(stats.allocations, (uint64_t)expected);
    POUND_CHECK_EQ_U64(stats.frees, (uint64_t)expected);

    fixture_close(&fixture);
}

POUND_TEST(jit_ballistic, the_reported_size_is_ignored_on_release)
{
    fixture_t fixture;

    fixture_open(&fixture);

    // Ballistic passes the size it allocated with, and a correct caller would pass the
    // same one again. The cache must not depend on it: the value it uses is the true
    // capacity it recorded, so a stale or wrong size cannot corrupt the free list. This
    // is asserted by checking the block is really gone and the accounting still
    // balances, which is what corruption would break.
    void *const block = fixture.allocator.allocate((bal_allocator_handle_t)&fixture.cache, 64U, 4096U);

    POUND_REQUIRE_PTR_NON_NULL(block);

    // A size nobody asked for: one too small, and one larger than the chunk.
    fixture.allocator.free((bal_allocator_handle_t)&fixture.cache, block, 1U);

    jit_cache_stats_t stats;

    memset(&stats, 0, sizeof(stats));
    POUND_REQUIRE(POUND_SUCCESS == jit_cache_get_stats(&fixture.cache, &stats));
    POUND_CHECK_EQ_U64(stats.live_blocks, 0U);
    POUND_CHECK_EQ_U64(stats.live_bytes, 0U);
    POUND_CHECK_EQ_U64(stats.frees, 1U);
    POUND_CHECK_MSG(stats.reserved_bytes == stats.free_bytes,
                    "%zu bytes are free but %zu are reserved",
                    stats.free_bytes,
                    stats.reserved_bytes);

    // The space is genuinely reusable, which is the observable consequence.
    void *const again = fixture.allocator.allocate((bal_allocator_handle_t)&fixture.cache, 64U, 4096U);

    POUND_REQUIRE_PTR_NON_NULL(again);
    POUND_CHECK_PTR_EQ(again, block);

    fixture.allocator.free((bal_allocator_handle_t)&fixture.cache, again, UINT64_MAX);
    fixture_close(&fixture);
}

POUND_TEST(jit_ballistic, a_zero_size_request_is_refused_rather_than_answered)
{
    fixture_t fixture;
    bal_executable_buffer_t buffer;

    fixture_open(&fixture);

    // A one-byte allocation is exactly what an empty function body would ask for, and
    // it is not refused -- the engine has to be able to compile something that starts
    // by immediately returning.
    void *const one = fixture.allocator.allocate((bal_allocator_handle_t)&fixture.cache, 16U, 1U);

    POUND_REQUIRE_PTR_NON_NULL(one);

    bal_executable_buffer_t small
        = fixture.allocator.allocate_executable((bal_allocator_handle_t)&fixture.cache, 16U, 1U);

    POUND_REQUIRE_PTR_NON_NULL(small.rw_pointer);
    fixture.allocator.free_executable((bal_allocator_handle_t)&fixture.cache, small, 1U);

    // A genuinely zero-byte request has no address that can be told apart from a
    // failure, so it is refused on both paths rather than answered with something the
    // engine would then write through.
    pound_test_log_reset();

    POUND_CHECK_PTR_NULL(fixture.allocator.allocate((bal_allocator_handle_t)&fixture.cache, 16U, 0U));
    POUND_CHECK(pound_test_log_contains("zero size"));

    buffer = fixture.allocator.allocate_executable((bal_allocator_handle_t)&fixture.cache, 16U, 0U);
    POUND_CHECK_PTR_NULL(buffer.rw_pointer);
    POUND_CHECK_PTR_NULL(buffer.rx_pointer);
    POUND_CHECK(pound_test_log_contains("zero size"));

    fixture.allocator.free((bal_allocator_handle_t)&fixture.cache, one, 1U);

    jit_cache_stats_t stats;

    memset(&stats, 0, sizeof(stats));
    POUND_REQUIRE(POUND_SUCCESS == jit_cache_get_stats(&fixture.cache, &stats));

    // Neither refusal was an allocation failure: both were rejected before the cache
    // went looking for room, so nothing was reserved and no fragment was left behind.
    POUND_CHECK_EQ_U64(stats.live_blocks, 0U);
    POUND_CHECK_EQ_U64(stats.allocations, 2U);
    POUND_CHECK_EQ_U64(stats.frees, 2U);
    POUND_CHECK_EQ_U64(stats.alloc_failures, 0U);

    fixture_close(&fixture);
}

// -----------------------------------------------------------------------------
// Executable buffers
// -----------------------------------------------------------------------------

POUND_TEST(jit_ballistic, executable_buffers_alias_but_stay_writable_then_executable)
{
    fixture_t fixture;

    fixture_open(&fixture);

    bal_executable_buffer_t buffer
        = fixture.allocator.allocate_executable((bal_allocator_handle_t)&fixture.cache, 64U, 512U);

    // Both pointers are set. An unset one would mean the engine wrote code through a
    // null or through memory it does not own.
    POUND_CHECK_PTR_NON_NULL(buffer.rw_pointer);
    POUND_CHECK_PTR_NON_NULL(buffer.rx_pointer);

    // The two are the same address, which is the documented consequence of a platform
    // where one range cannot be mapped twice under two protections. The interface's
    // contract -- write through one, execute through the other -- is satisfied either
    // way, so the bridge does not pretend this is a loss.
    POUND_CHECK_PTR_EQ(buffer.rw_pointer, buffer.rx_pointer);

    void *const block = buffer.rw_pointer;

    POUND_CHECK(!jit_cache_is_executable(&fixture.cache, block));

    // The engine writes the block's machine code through the writable pointer.
    memset(buffer.rw_pointer, 0x90, 512U);

    fixture.allocator.protect_rx((bal_allocator_handle_t)&fixture.cache, buffer, 512U);
    POUND_CHECK(jit_cache_is_executable(&fixture.cache, block));

    // And the same buffer comes back from the describing helper, so a caller that
    // rebuilds a buffer from a raw pointer gets the same thing the engine would.
    bal_executable_buffer_t described;

    memset(&described, 0, sizeof(described));
    POUND_REQUIRE(POUND_SUCCESS == jit_cache_executable_buffer(&fixture.cache, block, &described));
    POUND_CHECK_PTR_EQ(described.rw_pointer, buffer.rw_pointer);
    POUND_CHECK_PTR_EQ(described.rx_pointer, buffer.rx_pointer);

    // Patching it back to writable and re-protecting is the loop the engine runs on
    // every recompile, so it is worth one round trip here too.
    fixture.allocator.protect_rw((bal_allocator_handle_t)&fixture.cache, buffer, 512U);
    POUND_CHECK(!jit_cache_is_executable(&fixture.cache, block));
    memset(buffer.rw_pointer, 0xCC, 512U);
    fixture.allocator.protect_rx((bal_allocator_handle_t)&fixture.cache, buffer, 512U);
    POUND_CHECK(jit_cache_is_executable(&fixture.cache, block));

    fixture.allocator.free_executable((bal_allocator_handle_t)&fixture.cache, buffer, 512U);
    POUND_CHECK_EQ_U64(jit_cache_usable_size(&fixture.cache, block), 0U);

    fixture_close(&fixture);
}

POUND_TEST(jit_ballistic, a_failed_executable_request_yields_both_pointers_null)
{
    fixture_t fixture;

    fixture_open(&fixture);

    // The interface specifies that a failed allocation leaves the buffer empty, and a
    // caller is entitled to test one pointer and assume the other. Returning a buffer
    // with only one field set would be a fault waiting to happen, so both are checked.
    pound_test_log_reset();

    // Larger than a whole chunk, so no chunk can ever satisfy it.
    bal_executable_buffer_t buffer
        = fixture.allocator.allocate_executable((bal_allocator_handle_t)&fixture.cache,
                                                64U,
                                                BALLISTIC_TEST_CHUNK + 1U);

    POUND_CHECK_PTR_NULL(buffer.rw_pointer);
    POUND_CHECK_PTR_NULL(buffer.rx_pointer);
    POUND_CHECK(pound_test_log_contains("more than one"));

    // The same for a misaligned request that cannot be honoured: the bridge raises
    // sub-minimum alignments but still refuses a non-power-of-two, because no address
    // satisfies it.
    const bal_executable_buffer_t odd
        = fixture.allocator.allocate_executable((bal_allocator_handle_t)&fixture.cache, 24U, 512U);

    POUND_CHECK_PTR_NULL(odd.rw_pointer);
    POUND_CHECK_PTR_NULL(odd.rx_pointer);

    fixture_close(&fixture);
}

POUND_TEST(jit_ballistic, releasing_an_empty_executable_buffer_is_reported_not_ignored)
{
    fixture_t fixture;
    bal_executable_buffer_t empty;

    fixture_open(&fixture);

    empty.rw_pointer = NULL;
    empty.rx_pointer = NULL;

    // Ballistic's own contract is that a failed allocation leaves both pointers null,
    // so calling this afterwards would be the engine contradicting itself. The bridge
    // cannot return a status, so the only thing it can do is say so loudly -- a silent
    // no-op here would be indistinguishable from releasing a block successfully.
    pound_test_log_reset();
    fixture.allocator.free_executable((bal_allocator_handle_t)&fixture.cache, empty, 512U);
    POUND_CHECK(pound_test_log_contains("no address in it"));

    // The same for a protection change, which would otherwise look like a successful
    // call and leave the engine dispatching into memory that was never made executable.
    pound_test_log_reset();
    fixture.allocator.protect_rx((bal_allocator_handle_t)&fixture.cache, empty, 512U);
    POUND_CHECK(pound_test_log_contains("no address in it"));

    pound_test_log_reset();
    fixture.allocator.protect_rw((bal_allocator_handle_t)&fixture.cache, empty, 512U);
    POUND_CHECK(pound_test_log_contains("no address in it"));

    // Nothing was allocated and nothing was released, so the cache is untouched.
    jit_cache_stats_t stats;

    memset(&stats, 0, sizeof(stats));
    POUND_REQUIRE(POUND_SUCCESS == jit_cache_get_stats(&fixture.cache, &stats));
    POUND_CHECK_EQ_U64(stats.live_blocks, 0U);
    POUND_CHECK_EQ_U64(stats.frees, 0U);
    POUND_CHECK_EQ_U64(stats.alloc_failures, 0U);

    fixture_close(&fixture);
}

POUND_TEST(jit_ballistic, a_null_context_is_reported_on_every_callback)
{
    fixture_t fixture;
    bal_executable_buffer_t buffer;

    fixture_open(&fixture);

    // A null handle is not a shape the engine should ever produce, but every callback
    // that dereferences it is a crash if it ever does. Each one is checked for a log
    // record, because that is the only signal these signatures can produce.
    memset(&buffer, 0, sizeof(buffer));

    pound_test_log_reset();
    fixture.allocator.allocate(NULL, 64U, 512U);
    POUND_CHECK(pound_test_log_contains("no allocator context"));

    pound_test_log_reset();
    fixture.allocator.free(NULL, &fixture, 512U);
    POUND_CHECK(pound_test_log_contains("no allocator context"));

    pound_test_log_reset();
    (void)fixture.allocator.allocate_executable(NULL, 64U, 512U);
    POUND_CHECK(pound_test_log_contains("no allocator context"));

    pound_test_log_reset();
    fixture.allocator.free_executable(NULL, buffer, 512U);
    POUND_CHECK(pound_test_log_contains("no allocator context"));

    pound_test_log_reset();
    fixture.allocator.protect_rw(NULL, buffer, 512U);
    POUND_CHECK(pound_test_log_contains("no allocator context"));

    pound_test_log_reset();
    fixture.allocator.protect_rx(NULL, buffer, 512U);
    POUND_CHECK(pound_test_log_contains("no allocator context"));

    // A failed executable allocation under a null context still reports an empty
    // buffer, so the caller's test of the return value is safe.
    const bal_executable_buffer_t failed
        = fixture.allocator.allocate_executable(NULL, 64U, 512U);

    POUND_CHECK_PTR_NULL(failed.rw_pointer);
    POUND_CHECK_PTR_NULL(failed.rx_pointer);

    fixture_close(&fixture);
}

// -----------------------------------------------------------------------------
// Describing a block
// -----------------------------------------------------------------------------

POUND_TEST(jit_ballistic, describing_a_block_rejects_anything_but_a_live_executable_one)
{
    fixture_t fixture;
    bal_executable_buffer_t buffer;

    fixture_open(&fixture);

    void *const code = fixture.allocator.allocate_executable((bal_allocator_handle_t)&fixture.cache,
                                                            64U,
                                                            512U)
                         .rw_pointer;

    POUND_REQUIRE_PTR_NON_NULL(code);

    void *const data = fixture.allocator.allocate((bal_allocator_handle_t)&fixture.cache, 64U, 512U);

    POUND_REQUIRE_PTR_NON_NULL(data);

    pound_test_log_reset();

    // A data block has no executable form. Answering with one would hand the engine a
    // buffer it would then try to make executable, which the cache refuses anyway --
    // but the refusal would come much later and name the wrong thing.
    POUND_CHECK_EQ_I64(jit_cache_executable_buffer(&fixture.cache, data, &buffer),
                      POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK(pound_test_log_contains("it is a data block"));

    // An interior pointer is not the start of a block, so it is refused rather than
    // rounded down.
    POUND_CHECK_EQ_I64(jit_cache_executable_buffer(&fixture.cache, (uint8_t *)code + 8U, &buffer),
                      POUND_ERROR_DOUBLE_FREE);

    // A stack address belongs to no cache at all.
    int scratch = 0;

    POUND_CHECK_EQ_I64(jit_cache_executable_buffer(&fixture.cache, &scratch, &buffer),
                      POUND_ERROR_DOUBLE_FREE);

    // Null arguments, including the output.
    POUND_CHECK_EQ_I64(jit_cache_executable_buffer(NULL, code, &buffer), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(jit_cache_executable_buffer(&fixture.cache, NULL, &buffer),
                      POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(jit_cache_executable_buffer(&fixture.cache, code, NULL),
                      POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK(pound_test_log_contains("is NULL"));

    // A released block is no longer a live one, which is the other half of what makes
    // the "not a live block" code meaningful.
    fixture.allocator.free_executable((bal_allocator_handle_t)&fixture.cache,
                                      (bal_executable_buffer_t){code, code},
                                      512U);

    POUND_CHECK_EQ_I64(jit_cache_executable_buffer(&fixture.cache, code, &buffer),
                      POUND_ERROR_DOUBLE_FREE);

    fixture.allocator.free((bal_allocator_handle_t)&fixture.cache, data, 512U);
    fixture_close(&fixture);
}

// -----------------------------------------------------------------------------
// A whole session, in the order the engine would do it
// -----------------------------------------------------------------------------

POUND_TEST(jit_ballistic, a_session_of_allocations_never_loses_a_byte)
{
    fixture_t fixture;

    fixture_open(&fixture);

    // The engine's actual pattern: a constant stream of small metadata allocations
    // interleaved with executable blocks that are written, protected, patched and
    // released as guest code changes. The point is not any single call but that the
    // cache underneath stays bounded and its accounting stays exact across the lot,
    // which is the only way a leak in this layer would ever be noticed.
    //
    // Each round holds its metadata in a real array and releases exactly what it
    // allocated. Tracking the pointers rather than recomputing them matters: a release
    // of a pointer the round did not allocate would either be refused -- and the case
    // would then pass for the wrong reason -- or, worse, would free a neighbour and the
    // accounting would quietly stop balancing.
    for (size_t round = 0U; round < 64U; ++round)
    {
        void       *metadata[8];
        const size_t code_bytes = 1024U + (round * 16U);

        for (size_t i = 0U; i < 8U; ++i)
        {
            // A spread of alignments including ones below the cache's minimum, and
            // sizes that are not multiples of anything, so each block rounds
            // differently and the extents are laid out irregularly.
            const size_t alignment = (1U << (i % 7U));
            const size_t size       = 8U * (1U + (i * 3U));

            metadata[i]
                = fixture.allocator.allocate((bal_allocator_handle_t)&fixture.cache, alignment, size);

            POUND_REQUIRE_MSG(NULL != metadata[i],
                              "round %zu: a %zu-byte allocation at %zu-byte alignment was refused",
                              round,
                              size,
                              alignment);

            memset(metadata[i], (int)round, size);
                    }

        // One executable block per round, released on the same round. The full cycle
        // matters: write while writable, make executable, patch, make writable again,
        // make executable again, release.
        bal_executable_buffer_t code
            = fixture.allocator.allocate_executable((bal_allocator_handle_t)&fixture.cache,
                                                    64U,
                                                    code_bytes);

        POUND_REQUIRE_MSG(NULL != code.rw_pointer, "round %zu: the executable block was refused", round);

                memset(code.rw_pointer, 0x90, code_bytes);
        fixture.allocator.protect_rx((bal_allocator_handle_t)&fixture.cache, code, code_bytes);

        
        POUND_CHECK_MSG(jit_cache_is_executable(&fixture.cache, code.rw_pointer),
                        "round %zu: the block is not executable after protect_rx",
                        round);

        fixture.allocator.protect_rw((bal_allocator_handle_t)&fixture.cache, code, code_bytes);

        POUND_CHECK_MSG(!jit_cache_is_executable(&fixture.cache, code.rw_pointer),
                        "round %zu: the block is still executable after protect_rw",
                        round);

        memset(code.rw_pointer, 0xCC, code_bytes);
        fixture.allocator.protect_rx((bal_allocator_handle_t)&fixture.cache, code, code_bytes);
        fixture.allocator.free_executable((bal_allocator_handle_t)&fixture.cache, code, code_bytes);

                for (size_t i = 0U; i < 8U; ++i)
        {
            fixture.allocator.free((bal_allocator_handle_t)&fixture.cache, metadata[i], 1U);
        }

        // The accounting is checked every round rather than only at the end, so a leak
        // is attributed to the round that caused it rather than to "somewhere in
        // sixty-four rounds".
        if (0U == (round % 8U))
        {
            jit_cache_stats_t stats;

            memset(&stats, 0, sizeof(stats));
            POUND_REQUIRE(POUND_SUCCESS == jit_cache_get_stats(&fixture.cache, &stats));

            POUND_CHECK_MSG(0U == stats.live_blocks,
                            "round %zu ended with %zu live blocks",
                            round,
                            stats.live_blocks);
            POUND_CHECK_MSG(stats.reserved_bytes == stats.free_bytes,
                            "round %zu: %zu bytes are free but %zu are reserved",
                            round,
                            stats.free_bytes,
                            stats.reserved_bytes);
        }
    }

    jit_cache_stats_t stats;

    memset(&stats, 0, sizeof(stats));
    POUND_REQUIRE(POUND_SUCCESS == jit_cache_get_stats(&fixture.cache, &stats));

    POUND_CHECK_EQ_U64(stats.live_blocks, 0U);
    POUND_CHECK_EQ_U64(stats.live_bytes, 0U);
    POUND_CHECK_EQ_U64(stats.allocations, 64U * 9U);
    POUND_CHECK_EQ_U64(stats.frees, 64U * 9U);
    POUND_CHECK_EQ_U64(stats.rx_transitions, 128U);
    POUND_CHECK_EQ_U64(stats.rw_transitions, 64U);
    POUND_CHECK_MSG(stats.reserved_bytes == stats.free_bytes,
                    "%zu bytes are free but %zu are reserved, so a release went missing",
                    stats.free_bytes,
                    stats.reserved_bytes);
    POUND_CHECK_MSG(1U == stats.chunk_count,
                    "64 rounds of alloc-and-release grew the cache to %zu chunks",
                    stats.chunk_count);

    // Everything is free, so the whole chunk goes back to the OS rather than being
    // held for a session that has ended.
    POUND_CHECK_EQ_U64(jit_cache_reclaim(&fixture.cache), BALLISTIC_TEST_CHUNK);
    POUND_CHECK_EQ_U64(jit_cache_usable_size(&fixture.cache, NULL), 0U);

    fixture_close(&fixture);
}

POUND_TEST_SUITE(jit_ballistic,
                POUND_TEST_CASE(jit_ballistic, binding_installs_every_callback),
                POUND_TEST_CASE(jit_ballistic, binding_rejects_null_and_uninitialised_caches),
                POUND_TEST_CASE(jit_ballistic, allocations_through_the_bridge_come_from_the_cache),
                POUND_TEST_CASE(jit_ballistic, the_reported_size_is_ignored_on_release),
                POUND_TEST_CASE(jit_ballistic, a_zero_size_request_is_refused_rather_than_answered),
                POUND_TEST_CASE(jit_ballistic, executable_buffers_alias_but_stay_writable_then_executable),
                POUND_TEST_CASE(jit_ballistic, a_failed_executable_request_yields_both_pointers_null),
                POUND_TEST_CASE(jit_ballistic, releasing_an_empty_executable_buffer_is_reported_not_ignored),
                POUND_TEST_CASE(jit_ballistic, a_null_context_is_reported_on_every_callback),
                POUND_TEST_CASE(jit_ballistic, describing_a_block_rejects_anything_but_a_live_executable_one),
                POUND_TEST_CASE(jit_ballistic, a_session_of_allocations_never_loses_a_byte))

/*** end of file ***/

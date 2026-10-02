//! Tests for the W^X code cache.
//!
//! The cache is the only place in Pound that can hand out memory which is
//! executable, so the suite tests the properties that make that safe rather than
//! only the arithmetic. Three of them are load-bearing:
//!
//!   * **Nothing is ever writable and executable at once.** A block is allocated
//!     writable and not executable, and only `jit_cache_protect_rx` makes it
//!     executable. The state machine is checked from both ends, and a data block is
//!     refused outright, because a data block does not own whole pages and flipping
//!     it would change the protection of its neighbours.
//!   * **Executable blocks own whole pages.** Flipping a sub-page range would change
//!     the protection of whatever shares those pages, so an executable block's
//!     address and size are both checked against the real host page size.
//!   * **The code actually runs.** A JIT cache that allocates, protects and never
//!     executes anything has proved nothing. Two cases emit real machine code for
//!     the host architecture, jump to it, and check the result -- including patching
//!     a block in place through RW, which is the one path where a stale instruction
//!     cache would show up as a wrong answer rather than a crash.
//!
//! The accounting is checked against an exact invariant rather than against
//! hardcoded numbers: `reserved == live + free`, where "free" is every uncarved
//! tail plus every recorded free extent. That is the property that catches a lost
//! byte -- a cursor that fails to advance, a gap that was never recorded, a hole
//! that was dropped instead of coalesced -- none of which any single allocation
//! would reveal.

#include "pound_test.h"

#include "attributes.h"
#include "errors.h"
#include "jit/jit_cache.h"
#include "log.h"
#include "memory.h"
#include "platform.h"
#include <stdio.h>

// -----------------------------------------------------------------------------
// Fixtures and helpers
// -----------------------------------------------------------------------------

/// Chunk size used by most cases.
///
/// The cache's documented minimum. Small, so growing into a second chunk costs
/// kilobytes rather than megabytes and the suite stays fast, and large enough that
/// several page-aligned executable blocks fit in one.
#define JIT_TEST_CHUNK (JIT_CACHE_MIN_CHUNK_BYTES)

/// Ceiling used by most cases, high enough never to be the thing under test.
#define JIT_TEST_CEILING (JIT_TEST_CHUNK * 4U)

/// Initialises `cache` with the given chunk size and ceiling, poisoning off.
static void
init_cache(jit_cache_t *cache, const size_t chunk_bytes, const size_t max_bytes)
{
    jit_cache_config_t config;

    memset(&config, 0, sizeof(config));
    config.chunk_bytes = chunk_bytes;
    config.max_bytes   = max_bytes;

    POUND_REQUIRE(POUND_SUCCESS == jit_cache_init(cache, &config));
}

/// Initialises `cache` with the given chunk size, ceiling and poison setting.
static void
init_cache_poisoned(jit_cache_t *cache, const size_t chunk_bytes, const size_t max_bytes, const bool poison)
{
    jit_cache_config_t config;

    memset(&config, 0, sizeof(config));
    config.chunk_bytes = chunk_bytes;
    config.max_bytes   = max_bytes;
    config.poison      = poison;

    POUND_REQUIRE(POUND_SUCCESS == jit_cache_init(cache, &config));
}

/// Reads a stats snapshot, or a zeroed one if the read failed.
///
/// A failure here would make every later assertion meaningless, so it is reported
/// and answered with zeroes rather than aborting: this is a value-returning helper
/// and `POUND_REQUIRE` cannot be used inside one.
static jit_cache_stats_t
read_stats(const jit_cache_t *cache)
{
    jit_cache_stats_t stats;

    memset(&stats, 0, sizeof(stats));
    POUND_CHECK_MSG(POUND_SUCCESS == jit_cache_get_stats(cache, &stats),
                    "jit_cache_get_stats failed");

    return stats;
}

/// Reads the resolved configuration, or a zeroed one if the read failed.
static jit_cache_resolved_t
read_resolved(const jit_cache_t *cache)
{
    jit_cache_resolved_t resolved;

    memset(&resolved, 0, sizeof(resolved));
    POUND_CHECK_MSG(POUND_SUCCESS == jit_cache_describe(cache, &resolved),
                    "jit_cache_describe failed");

    return resolved;
}

/// Checks the accounting invariant and the counter consistency of a snapshot.
///
/// The invariant is exact: every byte of every reserved chunk is either handed out,
/// recorded as free, or still above the cursor. A byte that is none of those is a
/// byte the cache can never hand out again, and it is the failure mode a cursor bug,
/// an unrecorded alignment gap, or a dropped coalesce all produce.
///
/// It is worth knowing what this check *cannot* see, because the arithmetic it performs
/// is modular and a wrapped quantity can cancel: an extent recorded with a size near
/// SIZE_MAX sums to the correct total again once it is added back, so a free-extent
/// entry corrupted by a subtraction underflow passes this check unchanged. What that
/// corruption breaks is refusal -- an impossibly large request becomes answerable --
/// so the cases that care about it assert that instead.
static void
check_accounting(const jit_cache_t *cache, const char *where)
{
    const jit_cache_stats_t stats = read_stats(cache);

    POUND_CHECK_MSG(stats.reserved_bytes == (stats.live_bytes + stats.free_bytes),
                    "%s: %zu bytes reserved but %zu live + %zu free = %zu; %zu bytes are "
                    "unaccounted for",
                    where,
                    stats.reserved_bytes,
                    stats.live_bytes,
                    stats.free_bytes,
                    stats.live_bytes + stats.free_bytes,
                    (stats.reserved_bytes > (stats.live_bytes + stats.free_bytes))
                        ? (stats.reserved_bytes - (stats.live_bytes + stats.free_bytes))
                        : ((stats.live_bytes + stats.free_bytes) - stats.reserved_bytes));

    POUND_CHECK_MSG(stats.rw_blocks + stats.rx_blocks == stats.live_blocks,
                    "%s: %zu RW + %zu RX but %zu live blocks",
                    where,
                    stats.rw_blocks,
                    stats.rx_blocks,
                    stats.live_blocks);

    POUND_CHECK_MSG(stats.allocations >= (uint64_t)stats.live_blocks + stats.frees,
                    "%s: %llu allocations cannot cover %zu live and %llu freed blocks",
                    where,
                    (unsigned long long)stats.allocations,
                    stats.live_blocks,
                    (unsigned long long)stats.frees);

    // Every chunk is at least the configured size, and the total never passes the ceiling.
    // Both as bounds rather than as the equality this used to be: a chunk carved for a
    // request larger than `chunk_bytes` is deliberately bigger than the configured size,
    // so `reserved == chunk_count * chunk_bytes` no longer holds and cannot be recovered
    // from the statistics alone -- they do not report per-chunk sizes. What the numbers
    // can still say is that every chunk got at least what it was configured for and that
    // nothing pushed past the ceiling, which between them catch a chunk that was reserved
    // but not counted, counted but not reserved, or counted twice.
    const size_t chunk_bytes = read_resolved(cache).chunk_bytes;
    const size_t max_bytes   = read_resolved(cache).max_bytes;

    POUND_CHECK_MSG(stats.chunk_count <= (stats.reserved_bytes / chunk_bytes),
                    "%s: %zu chunk(s) of at least %zu bytes cannot account for %zu reserved bytes",
                    where,
                    stats.chunk_count,
                    chunk_bytes,
                    stats.reserved_bytes);
    POUND_CHECK_MSG(stats.reserved_bytes <= max_bytes,
                    "%s: %zu reserved bytes is past the %zu-byte ceiling",
                    where,
                    stats.reserved_bytes,
                    max_bytes);
}

/// True when `[a, a + a_size)` and `[b, b + b_size)` share a byte.
static bool
ranges_overlap(const void *a, const size_t a_size, const void *b, const size_t b_size)
{
    const uintptr_t a_begin = (uintptr_t)a;
    const uintptr_t b_begin = (uintptr_t)b;

    return (a_begin < (b_begin + b_size)) && (b_begin < (a_begin + a_size));
}

/// Returns a cache that has never been initialised.
///
/// Zeroed rather than left indeterminate, because "never initialised" is precisely the
/// state a caller is in after a failed or skipped `jit_cache_init`: the field the API
/// tests is `initialised`, and a zeroed struct is what that situation really looks
/// like. Handing it indeterminate bytes would be testing the compiler's idea of an
/// untouched local, not the refusal.
static jit_cache_t
unopened_cache(void)
{
    jit_cache_t cache;

    memset(&cache, 0, sizeof(cache));
    return cache;
}

// -----------------------------------------------------------------------------
// A function that emits real host machine code
// -----------------------------------------------------------------------------
//
// The signature is `uint64_t f(void)` so the encoding is a bare constant return and
// does not depend on the host ABI: whether the first argument arrives in `rdi` or
// `rcx` is irrelevant to code that reads no argument, which means one encoding is
// correct on every platform Pound builds and the test does not need a per-ABI
// variant. The value is kept under 2^16 so a single `mov` covers it on both
// architectures.

/// Signature of the functions these tests emit.
typedef uint64_t (*jit_entry_t)(void);

// Casting an object address to a function pointer is not a conversion ISO C
// defines, and the tests do it through `memcpy` precisely because it is not one.
// The assertion is what makes that `memcpy` sound rather than merely quiet.
_Static_assert(sizeof(jit_entry_t) == sizeof(void *), "jit_entry_t must be pointer sized.");
_Static_assert(sizeof(uint32_t) == 4U, "the encodings below assume a 32-bit word.");

/// Writes the encoding of `return value;` into `out`, and returns its length.
///
/// Returns 0 for a value or architecture this file has no encoding for, which the
/// caller treats as "nothing to execute" rather than a failure. An unknown
/// architecture is a gap in the test, not a defect in the code under test, and the
/// rest of the cache's behaviour is still worth checking there.
static size_t
encode_return_constant(uint8_t *out, const size_t capacity, const uint64_t value)
{
#if POUND_ARCHITECTURE_X86

    if (value > 0xFFFFFFFFULL)
    {
        return 0U;
    }

    // mov eax, imm32 ; ret
    if (capacity < 6U)
    {
        return 0U;
    }

    out[0] = 0xB8u;
    out[1] = (uint8_t)(value & 0xFFu);
    out[2] = (uint8_t)((value >> 8) & 0xFFu);
    out[3] = (uint8_t)((value >> 16) & 0xFFu);
    out[4] = (uint8_t)((value >> 24) & 0xFFu);
    out[5] = 0xC3u;
    return 6U;

#elif POUND_ARCHITECTURE_ARM64

    if (value > 0xFFFFULL)
    {
        return 0U;
    }

    // movz w0, #imm16 ; ret
    //
    // movz w0, #imm16 is 0x52800000 with imm16 in bits 20..5; `ret` is the
    // canonical 0xD65F03C0. Both are little-endian, and the guest is little-endian
    // too, so the bytes are written low word first.
    if (capacity < 8U)
    {
        return 0U;
    }

    const uint32_t movz = 0x52800000u | (uint32_t)((value & 0xFFFFu) << 5);
    const uint32_t ret  = 0xD65F03C0u;

    out[0] = (uint8_t)(movz & 0xFFu);
    out[1] = (uint8_t)((movz >> 8) & 0xFFu);
    out[2] = (uint8_t)((movz >> 16) & 0xFFu);
    out[3] = (uint8_t)((movz >> 24) & 0xFFu);
    out[4] = (uint8_t)(ret & 0xFFu);
    out[5] = (uint8_t)((ret >> 8) & 0xFFu);
    out[6] = (uint8_t)((ret >> 16) & 0xFFu);
    out[7] = (uint8_t)((ret >> 24) & 0xFFu);
    return 8U;

#else

    (void)out;
    (void)capacity;
    (void)value;
    return 0U;
#endif
}

/// Turns a block address into something callable.
///
/// The round trip goes through `memcpy` rather than a cast for the reason the static
/// assertion above records: a cast would be a conversion the language does not
/// define, and a `memcpy` of a pointer-sized value into a pointer-sized object is
/// at least something the optimiser is required to preserve.
static jit_entry_t
entry_of(const void *block)
{
    jit_entry_t entry;

    memset(&entry, 0, sizeof(entry));
    memcpy(&entry, &block, sizeof(entry));
    return entry;
}

// -----------------------------------------------------------------------------
// Construction
// -----------------------------------------------------------------------------

POUND_TEST(jit_cache, init_rejects_a_null_cache)
{
    pound_test_log_reset();

    POUND_CHECK_EQ_I64(jit_cache_init(NULL, NULL), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK(pound_test_log_contains("cache context is NULL"));
}

POUND_TEST(jit_cache, init_takes_every_default_from_a_null_config)
{
    jit_cache_t       cache;
    jit_cache_resolved_t resolved;

    POUND_REQUIRE(POUND_SUCCESS == jit_cache_init(&cache, NULL));

    memset(&resolved, 0, sizeof(resolved));
    POUND_REQUIRE(POUND_SUCCESS == jit_cache_describe(&cache, &resolved));

    POUND_CHECK_EQ_U64(resolved.chunk_bytes, JIT_CACHE_DEFAULT_CHUNK_BYTES);
    POUND_CHECK_EQ_U64(resolved.max_bytes, JIT_CACHE_DEFAULT_MAX_BYTES);
    POUND_CHECK_EQ_U64(resolved.default_alignment, JIT_CACHE_DEFAULT_ALIGNMENT);
    POUND_CHECK_MSG(0U != resolved.page_size, "the host page size must be non-zero");
    POUND_CHECK_MSG(!resolved.poison, "poisoning must default to off");

    // A page size is a power of two, and that is what the executable-block alignment
    // guarantee rests on.
    POUND_CHECK_MSG(0U == (resolved.page_size & (resolved.page_size - 1U)),
                    "the host page size %zu is not a power of two",
                    resolved.page_size);

    // Initialisation reserves nothing, so a process that loads a game but never JITs
    // a block pays nothing for the cache.
    const jit_cache_stats_t stats = read_stats(&cache);

    POUND_CHECK_EQ_U64(stats.chunk_count, 0U);
    POUND_CHECK_EQ_U64(stats.reserved_bytes, 0U);

    jit_cache_destroy(&cache);
}

POUND_TEST(jit_cache, init_refuses_a_chunk_that_is_not_a_whole_number_of_pages)
{
    jit_cache_t         cache;
    jit_cache_config_t  config;
    jit_cache_resolved_t probe;

    // The page size is not a constant, so the only way to ask for a chunk that is one
    // byte short of a page multiple is to read it first.
    POUND_REQUIRE(POUND_SUCCESS == jit_cache_init(&cache, NULL));

    memset(&probe, 0, sizeof(probe));
    POUND_REQUIRE(POUND_SUCCESS == jit_cache_describe(&cache, &probe));

    memset(&config, 0, sizeof(config));
    config.chunk_bytes = (probe.page_size > 1U) ? (probe.page_size - 1U) : 3U;
    config.max_bytes   = JIT_TEST_CEILING;

    pound_test_log_reset();

    // A chunk in this shape could not be flipped in a single
    // `mprotect`/`VirtualProtect` call, so it has to be refused at construction rather
    // than failing every executable allocation later.
    POUND_CHECK_EQ_I64(jit_cache_init(&cache, &config), POUND_ERROR_MEMORY_ALIGNMENT);
    POUND_CHECK(pound_test_log_contains("not a multiple of"));

    jit_cache_destroy(&cache);
}

POUND_TEST(jit_cache, init_refuses_a_ceiling_below_one_chunk)
{
    jit_cache_t       cache;
    jit_cache_config_t config;

    memset(&config, 0, sizeof(config));
    config.chunk_bytes = JIT_TEST_CHUNK;
    config.max_bytes   = JIT_TEST_CHUNK - 1U;

    pound_test_log_reset();

    POUND_CHECK_EQ_I64(jit_cache_init(&cache, &config), POUND_ERROR_MEMORY_ALIGNMENT);
    POUND_CHECK(pound_test_log_contains("cannot admit even one"));

    // A refused initialisation must leave nothing usable behind, so a later call that
    // forgets to check the status is refused too rather than operating on half a
    // cache.
    POUND_CHECK_PTR_NULL(jit_cache_alloc(&cache, 64U));
    POUND_CHECK_EQ_I64(jit_cache_describe(&cache, NULL), POUND_ERROR_INVALID_ARGUMENT);
}

POUND_TEST(jit_cache, init_refuses_a_chunk_outside_its_bounds)
{
    jit_cache_t       cache;
    jit_cache_config_t config;

    memset(&config, 0, sizeof(config));
    config.chunk_bytes = JIT_CACHE_MIN_CHUNK_BYTES / 2U;
    config.max_bytes   = JIT_TEST_CEILING;

    pound_test_log_reset();
    POUND_CHECK_EQ_I64(jit_cache_init(&cache, &config), POUND_ERROR_MEMORY_ALIGNMENT);
    POUND_CHECK(pound_test_log_contains("is outside"));

    memset(&config, 0, sizeof(config));
    config.chunk_bytes = JIT_CACHE_MAX_CHUNK_BYTES * 2U;
    config.max_bytes   = JIT_CACHE_MAX_CHUNK_BYTES * 4U;

    pound_test_log_reset();
    POUND_CHECK_EQ_I64(jit_cache_init(&cache, &config), POUND_ERROR_MEMORY_ALIGNMENT);
    POUND_CHECK(pound_test_log_contains("is outside"));
}

POUND_TEST(jit_cache, describe_rejects_null_arguments)
{
    jit_cache_t         cache;
    jit_cache_resolved_t resolved;

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);
    pound_test_log_reset();

    POUND_CHECK_EQ_I64(jit_cache_describe(NULL, NULL), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(jit_cache_describe(&cache, NULL), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(jit_cache_describe(NULL, &resolved), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK(pound_test_log_contains("cache or output is NULL"));

    jit_cache_destroy(&cache);
}

// -----------------------------------------------------------------------------
// Data blocks
// -----------------------------------------------------------------------------

POUND_TEST(jit_cache, data_blocks_are_writable_and_correctly_sized)
{
    jit_cache_t cache;
    const size_t requested = 100U;

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);

    void *const block = jit_cache_alloc(&cache, requested);
    POUND_REQUIRE_PTR_NON_NULL(block);

    // The default alignment, not merely "some alignment".
    POUND_CHECK_MSG(0U == ((uintptr_t)block % JIT_CACHE_DEFAULT_ALIGNMENT),
                    "block %p is not %u-byte aligned",
                    block,
                    (unsigned int)JIT_CACHE_DEFAULT_ALIGNMENT);

    // At least what was asked for. Larger is legitimate: an extent that is too small a
    // remainder to split is handed out whole.
    POUND_CHECK_MSG(jit_cache_usable_size(&cache, block) >= requested,
                    "usable size %zu is below the %zu requested",
                    jit_cache_usable_size(&cache, block),
                    requested);

    // And genuinely writable, across the whole usable size rather than just the
    // requested part, because a caller is entitled to trust the reported capacity.
    const size_t capacity = jit_cache_usable_size(&cache, block);

    memset(block, 0xA5, capacity);

    for (size_t i = 0U; i < capacity; ++i)
    {
        if (((const uint8_t *)block)[i] != 0xA5u)
        {
            POUND_CHECK_MSG(false, "byte %zu of a %zu-byte block did not take the write", i, capacity);
            break;
        }
    }

    check_accounting(&cache, "after one data block");

    jit_cache_free(&cache, block);
    check_accounting(&cache, "after freeing it");
    jit_cache_destroy(&cache);
}

POUND_TEST(jit_cache, data_blocks_honour_every_legal_alignment)
{
    static const size_t alignments[] = {16U, 32U, 64U, 128U, 256U, 1024U, 4096U, 65536U};
    jit_cache_t       cache;

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);

    for (size_t i = 0U; i < (sizeof(alignments) / sizeof(alignments[0])); ++i)
    {
        const size_t alignment = alignments[i];

        void *const block = jit_cache_alloc_aligned(&cache, alignment, 32U);
        POUND_REQUIRE_MSG(NULL != block,
                          "a 32-byte block at %zu-byte alignment was refused",
                          alignment);

        POUND_CHECK_MSG(0U == ((uintptr_t)block % alignment),
                        "block %p is not %zu-byte aligned",
                        block,
                        alignment);

        memset(block, 0x5A, 32U);
        jit_cache_free(&cache, block);
    }

    check_accounting(&cache, "after every alignment");
    jit_cache_destroy(&cache);
}

POUND_TEST(jit_cache, allocation_refuses_impossible_requests)
{
    jit_cache_t cache;

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);
    pound_test_log_reset();

    // A zero-byte block has no address that can be distinguished from a failure, so
    // it is refused rather than answered with something the caller would then write
    // through.
    POUND_CHECK_PTR_NULL(jit_cache_alloc(&cache, 0U));
    POUND_CHECK(pound_test_log_contains("zero size"));

    // Not a power of two: there is no such thing as an address that is a multiple of
    // three.
    POUND_CHECK_PTR_NULL(jit_cache_alloc_aligned(&cache, 3U, 64U));
    POUND_CHECK_PTR_NULL(jit_cache_alloc_aligned(&cache, 24U, 64U));
    POUND_CHECK_PTR_NULL(jit_cache_alloc_aligned(&cache, 0U, 64U));
    POUND_CHECK(pound_test_log_contains("power of two"));

    // Below the minimum, where a block descriptor could not be pointer aligned.
    POUND_CHECK_PTR_NULL(jit_cache_alloc_aligned(&cache, 8U, 64U));
    POUND_CHECK(pound_test_log_contains("is outside"));

    // Above the maximum, which would leave no room for a second block in any chunk.
    POUND_CHECK_PTR_NULL(jit_cache_alloc_aligned(&cache, JIT_CACHE_MAX_ALIGNMENT * 2U, 64U));
    POUND_CHECK(pound_test_log_contains("is outside"));

    // A NULL cache is refused on every allocating entry point.
    POUND_CHECK_PTR_NULL(jit_cache_alloc(NULL, 64U));
    POUND_CHECK_PTR_NULL(jit_cache_alloc_aligned(NULL, 64U, 64U));
    POUND_CHECK_PTR_NULL(jit_cache_alloc_executable(NULL, 64U));
    POUND_CHECK_PTR_NULL(jit_cache_alloc_executable_aligned(NULL, 64U, 64U));

    // Every refusal is a log record too, never a silent NULL.
    POUND_CHECK_MSG(0U != pound_test_log_count_at(LOG_LEVEL_ERROR),
                    "the refusals above produced no error records");

    // None of those was an *allocation* failure: each was rejected before the cache
    // went looking for room, so none reserved, counted, or left a fragment behind.
    POUND_CHECK_MSG(0U == read_stats(&cache).alloc_failures,
                    "%zu malformed requests were counted as allocation failures",
                    read_stats(&cache).alloc_failures);
    check_accounting(&cache, "after every malformed request");

    // A request whose rounded size overflows is a different case: it is well formed
    // enough to reach the allocator, so it is counted, and it must be refused rather
    // than wrapping to a small value and handing back a block that overlaps its
    // neighbours.
    pound_test_log_reset();
    POUND_CHECK_PTR_NULL(jit_cache_alloc(&cache, SIZE_MAX));
    POUND_CHECK(pound_test_log_contains("overflows"));
    POUND_CHECK_EQ_U64(read_stats(&cache).alloc_failures, 1U);
    check_accounting(&cache, "after an overflowing request");

    jit_cache_destroy(&cache);
}

POUND_TEST(jit_cache, a_request_past_the_ceiling_is_refused_without_reserving)
{
    jit_cache_t cache;

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);

    // A request no arrangement of chunks could satisfy under this ceiling. Refusing it
    // *before* reserving matters: reserving first would leave an empty chunk behind that
    // nothing can use and that only `jit_cache_reclaim` would give back.
    pound_test_log_reset();
    POUND_CHECK_PTR_NULL(jit_cache_alloc(&cache, JIT_TEST_CEILING + 1U));
    POUND_CHECK(pound_test_log_contains("Refusing to reserve"));

    POUND_CHECK_EQ_U64(read_stats(&cache).chunk_count, 0U);
    POUND_CHECK_EQ_U64(read_stats(&cache).reserved_bytes, 0U);
    check_accounting(&cache, "after an impossible request");

    // A request that exactly fills a chunk is fine, and is not mistaken for the one
    // above -- nor for one that needs a chunk carved for it.
    void *const exact = jit_cache_alloc(&cache, JIT_TEST_CHUNK);

    POUND_REQUIRE_PTR_NON_NULL(exact);
    POUND_CHECK_EQ_U64(read_stats(&cache).chunk_count, 1U);
    check_accounting(&cache, "after an exactly-fitting request");

    jit_cache_free(&cache, exact);

    // Reclaimed first, so the oversized request below starts from nothing. Leaving the
    // freed chunk in place would be legitimate -- nothing reclaims automatically -- but it
    // would mean the cache held two chunks, and the count is what says whether the carved
    // one was a second chunk or a replacement, which is the thing under test.
    POUND_CHECK_MSG((size_t)JIT_TEST_CHUNK == jit_cache_reclaim(&cache),
                    "Reclaiming the one empty chunk returned the wrong number of bytes.");
    POUND_CHECK_EQ_U64(read_stats(&cache).chunk_count, 0U);

    // A block larger than one chunk, and comfortably under the ceiling, is served from a
    // chunk carved to its size. This is what makes the engine's single 16 MiB code buffer
    // possible against a 4 MiB default, and the carved chunk is verifiably bigger than the
    // configured one -- without that it would be serving an oversized block out of an
    // ordinary chunk, which is the arithmetic this whole path exists to avoid.
    void *const oversized = jit_cache_alloc(&cache, JIT_TEST_CHUNK * 3U);

    POUND_REQUIRE_PTR_NON_NULL(oversized);

    const jit_cache_stats_t after = read_stats(&cache);

    POUND_CHECK_MSG(1U == after.chunk_count,
                    "An oversized block was served with %zu chunks reserved; the first request "
                    "should have carved exactly one.",
                    after.chunk_count);
    POUND_CHECK_MSG(after.reserved_bytes > (size_t)JIT_TEST_CHUNK,
                    "%zu byte(s) are reserved for a block that outgrows a %u-byte chunk, so it "
                    "came out of a chunk that can hold it.",
                    after.reserved_bytes,
                    (unsigned int)JIT_TEST_CHUNK);
    check_accounting(&cache, "after an oversized request");

    // And the carved chunk counts against the ceiling like any other. A third of the
    // ceiling is still free on paper, but committing it would mean reserving past the
    // limit -- so a cache that forgot to count the carved chunk would hand this out and
    // exceed its ceiling in exactly the case that made the carve necessary.
    POUND_CHECK_PTR_NULL(jit_cache_alloc(&cache, JIT_TEST_CHUNK * 2U));
    POUND_CHECK_EQ_U64(read_stats(&cache).chunk_count, 1U);
    check_accounting(&cache, "once the ceiling would be passed");

    jit_cache_free(&cache, oversized);
    jit_cache_destroy(&cache);
}

POUND_TEST(jit_cache, live_blocks_never_alias)
{
    jit_cache_t cache;
    void       *blocks[64];
    size_t      capacities[64];

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);

    for (size_t i = 0U; i < 64U; ++i)
    {
        // Sizes that are not multiples of the alignment, so each block's capacity is
        // rounded differently and the extents are laid out irregularly. Uniform sizes
        // would let a cursor that advanced by the wrong amount still look correct.
        blocks[i] = jit_cache_alloc(&cache, 100U + i);

        if (NULL == blocks[i])
        {
            // The chunk is 256 KiB, so 64 blocks of ~100 bytes cannot exhaust it; a
            // NULL here means the cache failed a request it had room for.
            POUND_CHECK_MSG(false, "block %zu was refused", i);
            break;
        }

        capacities[i] = jit_cache_usable_size(&cache, blocks[i]);

        POUND_CHECK_MSG(capacities[i] >= (100U + i),
                        "block %zu has capacity %zu, below the %zu requested",
                        i,
                        capacities[i],
                        100U + i);
    }

    for (size_t i = 0U; i < 64U; ++i)
    {
        if (NULL == blocks[i])
        {
            break;
        }

        for (size_t j = i + 1U; j < 64U; ++j)
        {
            if (NULL == blocks[j])
            {
                break;
            }

            if (ranges_overlap(blocks[i], capacities[i], blocks[j], capacities[j]))
            {
                POUND_CHECK_MSG(false,
                                "block %zu at %p (%zu bytes) overlaps block %zu at %p (%zu bytes)",
                                i,
                                blocks[i],
                                capacities[i],
                                j,
                                blocks[j],
                                capacities[j]);
                break;
            }
        }
    }

    // Writing through one must not disturb another. A cursor that advanced by the
    // wrong amount would alias, and this is what would notice.
    for (size_t i = 0U; i < 64U; ++i)
    {
        if (NULL == blocks[i])
        {
            break;
        }

        memset(blocks[i], (int)(0x40U + i), capacities[i]);
    }

    for (size_t i = 0U; i < 64U; ++i)
    {
        if (NULL == blocks[i])
        {
            break;
        }

        const uint8_t expected = (uint8_t)(0x40U + i);

        for (size_t k = 0U; k < capacities[i]; ++k)
        {
            if (((const uint8_t *)blocks[i])[k] != expected)
            {
                POUND_CHECK_MSG(false,
                                "block %zu byte %zu was overwritten by a neighbour",
                                i,
                                k);
                break;
            }
        }

        POUND_CHECK_MSG(jit_cache_usable_size(&cache, blocks[i]) == capacities[i],
                        "block %zu changed capacity while it was live",
                        i);
    }

    check_accounting(&cache, "with 64 live blocks");

    for (size_t i = 0U; i < 64U; ++i)
    {
        jit_cache_free(&cache, blocks[i]);
    }

    check_accounting(&cache, "after freeing 64 blocks");
    jit_cache_destroy(&cache);
}

POUND_TEST(jit_cache, block_info_reports_the_truth)
{
    jit_cache_t       cache;
    jit_block_info_t  info;

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);

    void *const block = jit_cache_alloc_aligned(&cache, 256U, 300U);
    POUND_REQUIRE_PTR_NON_NULL(block);

    memset(&info, 0, sizeof(info));
    POUND_REQUIRE(POUND_SUCCESS == jit_cache_get_block_info(&cache, block, &info));

    POUND_CHECK_EQ_U64(info.requested, 300U);
    POUND_CHECK_MSG(info.capacity >= info.requested,
                    "capacity %zu is below the requested %zu",
                    info.capacity,
                    info.requested);
    POUND_CHECK_EQ_U64(info.kind, JIT_BLOCK_KIND_DATA);
    POUND_CHECK_EQ_U64(info.state, JIT_BLOCK_STATE_READ_WRITE);
    POUND_CHECK_MSG(0U == ((uintptr_t)block % 256U), "block %p is not 256-byte aligned", block);

    // An interior pointer resolves to nothing. Rounding one down would free a
    // neighbour, so the cache must refuse rather than guess.
    memset(&info, 0, sizeof(info));
    POUND_CHECK_EQ_I64(jit_cache_get_block_info(&cache, (uint8_t *)block + 8U, &info),
                      POUND_ERROR_DOUBLE_FREE);
    POUND_CHECK_EQ_U64(jit_cache_usable_size(&cache, (uint8_t *)block + 8U), 0U);

    jit_cache_free(&cache, block);

    // And neither does a pointer the cache never produced.
    POUND_CHECK_EQ_I64(jit_cache_get_block_info(&cache, (const void *)&cache, &info),
                      POUND_ERROR_DOUBLE_FREE);
    POUND_CHECK_EQ_U64(jit_cache_usable_size(&cache, (const void *)&cache), 0U);

    jit_cache_destroy(&cache);
}

POUND_TEST(jit_cache, block_info_rejects_null_arguments)
{
    jit_cache_t      cache;
    jit_block_info_t info;
    int              scratch = 0;

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);
    pound_test_log_reset();

    POUND_CHECK_EQ_I64(jit_cache_get_block_info(NULL, &scratch, &info), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(jit_cache_get_block_info(&cache, &scratch, NULL), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(jit_cache_get_block_info(&cache, NULL, &info), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK(pound_test_log_contains("is NULL"));

    // A NULL pointer is a no-op free by allocator convention, so it is not an error and
    // is deliberately not logged.
    const unsigned before = pound_test_log_count_at(LOG_LEVEL_ERROR);

    jit_cache_free(&cache, NULL);
    jit_cache_free_executable(&cache, NULL);
    POUND_CHECK_EQ_U64(pound_test_log_count_at(LOG_LEVEL_ERROR), before);

    jit_cache_destroy(&cache);
}

// -----------------------------------------------------------------------------
// Releasing, reuse and coalescing
// -----------------------------------------------------------------------------

POUND_TEST(jit_cache, freed_space_is_reused_at_the_same_address)
{
    jit_cache_t cache;
    void       *first;

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);

    first = jit_cache_alloc(&cache, 512U);
    POUND_REQUIRE_PTR_NON_NULL(first);

    // Something else live, so the extent is not simply handed back because the chunk
    // grew.
    void *const neighbour = jit_cache_alloc(&cache, 512U);
    POUND_REQUIRE_PTR_NON_NULL(neighbour);

    jit_cache_free(&cache, first);

    // Free space is consulted before the cursor, so this must come back out of the
    // extent just released rather than from the tail.
    void *const again = jit_cache_alloc(&cache, 512U);
    POUND_REQUIRE_PTR_NON_NULL(again);
    POUND_CHECK_PTR_EQ(again, first);
    POUND_CHECK_EQ_U64(read_stats(&cache).chunk_count, 1U);

    jit_cache_free(&cache, again);
    jit_cache_free(&cache, neighbour);
    check_accounting(&cache, "after a reuse cycle");
    jit_cache_destroy(&cache);
}

POUND_TEST(jit_cache, adjacent_extents_coalesce)
{
    jit_cache_t cache;
    void       *blocks[4];

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);

    for (size_t i = 0U; i < 4U; ++i)
    {
        blocks[i] = jit_cache_alloc_aligned(&cache, 64U, 1024U);
        POUND_REQUIRE_MSG(NULL != blocks[i], "block %zu was refused", i);
    }

    // Free the four in a scrambled order, so the coalescing has to merge backwards,
    // forwards, and both at once rather than only in the convenient one.
    jit_cache_free(&cache, blocks[1]);
    check_accounting(&cache, "after freeing the second block");
    jit_cache_free(&cache, blocks[3]);
    check_accounting(&cache, "after freeing the fourth block");
    jit_cache_free(&cache, blocks[0]);
    check_accounting(&cache, "after freeing the first block");
    jit_cache_free(&cache, blocks[2]);
    check_accounting(&cache, "after freeing the third block");

    // If the four 1 KiB extents merged into one 4 KiB extent, this single request is
    // satisfied at the address of the first block. If they did not merge, the largest
    // free extent is 2 KiB and the request either fails or lands elsewhere.
    void *const whole = jit_cache_alloc_aligned(&cache, 64U, 4U * 1024U);

    POUND_REQUIRE_PTR_NON_NULL(whole);
    POUND_CHECK_MSG(whole == blocks[0],
                    "a 4 KiB request over four coalesced 1 KiB extents landed at %p, not %p",
                    whole,
                    blocks[0]);
    POUND_CHECK_EQ_U64(read_stats(&cache).chunk_count, 1U);

    jit_cache_free(&cache, whole);
    check_accounting(&cache, "after releasing the coalesced extent");
    jit_cache_destroy(&cache);
}

POUND_TEST(jit_cache, release_refuses_anything_it_did_not_hand_out)
{
    jit_cache_t cache;
    int         scratch[64];

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);
    pound_test_log_reset();

    // A stack address belongs to no chunk.
    POUND_CHECK_EQ_U64(read_stats(&cache).live_blocks, 0U);

    jit_cache_free(&cache, scratch);
    POUND_CHECK(pound_test_log_contains("not the start of a live block"));
    POUND_CHECK_EQ_U64(read_stats(&cache).frees, 0U);

    void *const block = jit_cache_alloc(&cache, 256U);
    POUND_REQUIRE_PTR_NON_NULL(block);

    // An interior pointer. Rounding it down would release a neighbour, so the cache
    // must refuse instead of guessing which.
    jit_cache_free(&cache, (uint8_t *)block + 16U);
    POUND_CHECK_MSG(1U == read_stats(&cache).live_blocks,
                    "an interior pointer was resolved to a block");

    // The block is still intact and still writable.
    memset(block, 0x11, 256U);
    POUND_CHECK_EQ_U64(jit_cache_usable_size(&cache, block), 256U);

    // A second release of the same address, after a real one, must not corrupt the
    // free list.
    jit_cache_free(&cache, block);
    POUND_CHECK_EQ_U64(read_stats(&cache).live_blocks, 0U);
    check_accounting(&cache, "after a real release");

    pound_test_log_reset();
    jit_cache_free(&cache, block);
    POUND_CHECK(pound_test_log_contains("already released"));
    check_accounting(&cache, "after a double release");

    jit_cache_destroy(&cache);
}

POUND_TEST(jit_cache, release_refuses_a_block_of_the_wrong_kind)
{
    jit_cache_t cache;

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);

    void *const data = jit_cache_alloc(&cache, 256U);
    void *const code = jit_cache_alloc_executable(&cache, 256U);

    POUND_REQUIRE_PTR_NON_NULL(data);
    POUND_REQUIRE_PTR_NON_NULL(code);

    // A data block released through the executable path would leave its recorded
    // state inconsistent with how a later query reads it. Both blocks are still live
    // afterwards, and the refused one is still usable -- which is what tells the two
    // apart from a release that half-succeeded.
    pound_test_log_reset();
    jit_cache_free_executable(&cache, data);
    POUND_CHECK(pound_test_log_contains("is a data block"));
    POUND_CHECK_MSG(2U == read_stats(&cache).live_blocks,
                    "a refused mismatched release took a block with it: %zu live, expected 2",
                    read_stats(&cache).live_blocks);
    POUND_CHECK_MSG(jit_cache_usable_size(&cache, data) > 0U,
                    "the data block was released despite the refusal");

    // And the reverse.
    pound_test_log_reset();
    jit_cache_free(&cache, code);
    POUND_CHECK(pound_test_log_contains("is an executable block"));
    POUND_CHECK_MSG(2U == read_stats(&cache).live_blocks,
                    "a refused mismatched release took a block with it: %zu live, expected 2",
                    read_stats(&cache).live_blocks);
    POUND_CHECK_MSG(jit_cache_usable_size(&cache, code) > 0U,
                    "the executable block was released despite the refusal");

    // Each still frees through its own path.
    jit_cache_free(&cache, data);
    jit_cache_free_executable(&cache, code);
    POUND_CHECK_EQ_U64(read_stats(&cache).live_blocks, 0U);

    check_accounting(&cache, "after both mismatched releases were refused");
    jit_cache_destroy(&cache);
}

POUND_TEST(jit_cache, repeated_cycles_do_not_grow_the_cache)
{
    jit_cache_t cache;

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);

    // Each round allocates, then frees everything it allocated. If freeing did not
    // coalesce and reuse, every round would push the cursor further along and the
    // cache would grow by a chunk every so often. One chunk after two hundred rounds
    // is the whole property: bounded working set, not bounded leak.
    for (size_t round = 0U; round < 200U; ++round)
    {
        void *blocks[16];

        for (size_t i = 0U; i < 16U; ++i)
        {
            blocks[i] = jit_cache_alloc(&cache, 512U);
            POUND_REQUIRE_MSG(NULL != blocks[i], "round %zu block %zu was refused", round, i);
        }

        for (size_t i = 0U; i < 16U; ++i)
        {
            memset(blocks[i], (int)i, 512U);
        }

        for (size_t i = 0U; i < 16U; ++i)
        {
            jit_cache_free(&cache, blocks[i]);
        }

        if (0U == (round % 50U))
        {
            check_accounting(&cache, "mid-cycle");
        }
    }

    const jit_cache_stats_t stats = read_stats(&cache);

    POUND_CHECK_MSG(1U == stats.chunk_count,
                    "200 alloc-and-free rounds grew the cache to %zu chunks; freed space is not "
                    "being reused",
                    stats.chunk_count);
    POUND_CHECK_EQ_U64(stats.live_blocks, 0U);
    POUND_CHECK_EQ_U64(stats.allocations, 200U * 16U);
    POUND_CHECK_EQ_U64(stats.frees, 200U * 16U);

    // Everything is free again, so reclaim can give the whole chunk back.
    POUND_CHECK_EQ_U64(jit_cache_reclaim(&cache), JIT_TEST_CHUNK);
    POUND_CHECK_EQ_U64(read_stats(&cache).chunk_count, 0U);

    jit_cache_destroy(&cache);
}

POUND_TEST(jit_cache, reclaim_returns_only_trailing_empty_chunks)
{
    jit_cache_t cache;
    void       *anchor;
    void       *filler;
    void       *first;
    void       *second;

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);

    // The oldest chunk gets two blocks that together leave a tail, so it stays both
    // live and useful -- the two properties this case is about.
    anchor = jit_cache_alloc(&cache, 2048U);
    POUND_REQUIRE_PTR_NON_NULL(anchor);

    filler = jit_cache_alloc(&cache, 200U * 1024U);
    POUND_REQUIRE_PTR_NON_NULL(filler);
    POUND_CHECK_EQ_U64(read_stats(&cache).chunk_count, 1U);

    // The next request does not fit in what is left of that chunk, so the cache grows.
    first = jit_cache_alloc(&cache, 100U * 1024U);
    POUND_REQUIRE_PTR_NON_NULL(first);
    POUND_CHECK_EQ_U64(read_stats(&cache).chunk_count, 2U);

    // And a small one lands in the new chunk, which now holds two live blocks.
    second = jit_cache_alloc(&cache, 4096U);
    POUND_REQUIRE_PTR_NON_NULL(second);
    POUND_CHECK_EQ_U64(read_stats(&cache).chunk_count, 2U);

    // The newest chunk is not empty, so nothing goes back. This is a legitimate answer
    // and not a failure: the two blocks in it are still in use.
    POUND_CHECK_EQ_U64(jit_cache_reclaim(&cache), 0U);
    POUND_CHECK_EQ_U64(read_stats(&cache).chunk_count, 2U);
    POUND_CHECK_MSG(jit_cache_usable_size(&cache, first) > 0U, "reclaim discarded a live block");

    // Empty the newest chunk. Now it is trailing and fully free, so the whole chunk
    // goes back -- and reclaim stops at the chunk behind it, which still holds two
    // live blocks.
    jit_cache_free(&cache, first);
    jit_cache_free(&cache, second);
    POUND_CHECK_EQ_U64(jit_cache_reclaim(&cache), JIT_TEST_CHUNK);
    POUND_CHECK_EQ_U64(read_stats(&cache).chunk_count, 1U);
    POUND_CHECK_EQ_U64(read_stats(&cache).reserved_bytes, JIT_TEST_CHUNK);

    // The retained chunk still holds the two original blocks, so it is not trailing and
    // stays even though it is nearly empty. Returning it would strand the space above
    // them, which is the reason for keeping it.
    POUND_CHECK_EQ_U64(jit_cache_reclaim(&cache), 0U);
    POUND_CHECK_EQ_U64(read_stats(&cache).chunk_count, 1U);
    POUND_CHECK_MSG(jit_cache_usable_size(&cache, anchor) > 0U, "reclaim discarded a live block");
    POUND_CHECK_MSG(jit_cache_usable_size(&cache, filler) > 0U, "reclaim discarded a live block");

    // And that space above them is still usable, from the retained chunk.
    void *const above = jit_cache_alloc(&cache, 4096U);
    POUND_REQUIRE_PTR_NON_NULL(above);
    POUND_CHECK_MSG(1U == read_stats(&cache).chunk_count,
                    "the retained chunk could not serve a request and the cache grew instead");
    check_accounting(&cache, "after a partial reclaim");

    // With every block gone, both conditions hold and the last chunk goes too.
    jit_cache_free(&cache, above);
    jit_cache_free(&cache, filler);
    jit_cache_free(&cache, anchor);
    POUND_CHECK_EQ_U64(jit_cache_reclaim(&cache), JIT_TEST_CHUNK);
    POUND_CHECK_EQ_U64(read_stats(&cache).chunk_count, 0U);
    POUND_CHECK_EQ_U64(read_stats(&cache).reserved_bytes, 0U);

    jit_cache_destroy(&cache);
}

POUND_TEST(jit_cache, reclaim_rejects_a_null_or_uninitialised_cache)
{
    jit_cache_t cache = unopened_cache();

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(jit_cache_reclaim(NULL), 0U);
    POUND_CHECK(pound_test_log_contains("cache context is NULL"));

    POUND_CHECK_EQ_U64(jit_cache_reclaim(&cache), 0U);
    POUND_CHECK(pound_test_log_contains("never initialised"));
}

// -----------------------------------------------------------------------------
// Write/execute
// -----------------------------------------------------------------------------

POUND_TEST(jit_cache, executable_blocks_own_whole_pages)
{
    jit_cache_t          cache;
    jit_cache_resolved_t resolved;
    void                *blocks[8];
    size_t               page;

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);

    memset(&resolved, 0, sizeof(resolved));
    POUND_REQUIRE(POUND_SUCCESS == jit_cache_describe(&cache, &resolved));
    page = resolved.page_size;

    for (size_t i = 0U; i < 8U; ++i)
    {
        // Sizes that are deliberately not page multiples and not multiples of any
        // alignment the caller asked for, so every block has to round up on its own.
        blocks[i] = jit_cache_alloc_executable_aligned(&cache, 64U, 100U + i);
        POUND_REQUIRE_MSG(NULL != blocks[i], "executable block %zu was refused", i);

        POUND_CHECK_MSG(0U == ((uintptr_t)blocks[i] % page),
                        "executable block %zu at %p is not %zu-byte aligned; flipping its "
                        "protection would change whatever shares those pages",
                        i,
                        blocks[i],
                        page);

        POUND_CHECK_MSG(0U == (jit_cache_usable_size(&cache, blocks[i]) % page),
                        "executable block %zu has capacity %zu, not a multiple of the %zu-byte "
                        "page",
                        i,
                        jit_cache_usable_size(&cache, blocks[i]),
                        page);
    }

    // A page-aligned address and a page-multiple size mean each block's pages are its
    // own, so flipping one leaves its neighbours writable.
    for (size_t i = 0U; i + 1U < 8U; ++i)
    {
        POUND_REQUIRE(POUND_SUCCESS == jit_cache_protect_rx(&cache, blocks[i]));

        // The neighbour is untouched by that flip, so it must still be writable.
        POUND_CHECK_MSG(JIT_BLOCK_STATE_READ_WRITE == jit_cache_block_state(&cache, blocks[i + 1U]),
                        "protecting block %zu to read-execute disturbed block %zu",
                        i,
                        i + 1U);

        POUND_CHECK(JIT_BLOCK_STATE_READ_EXECUTE == jit_cache_block_state(&cache, blocks[i]));
    }

    check_accounting(&cache, "with 8 executable blocks");

    for (size_t i = 0U; i < 8U; ++i)
    {
        jit_cache_free_executable(&cache, blocks[i]);
    }

    check_accounting(&cache, "after releasing 8 executable blocks");
    jit_cache_destroy(&cache);
}

POUND_TEST(jit_cache, an_executable_block_is_writable_before_it_is_executable)
{
    jit_cache_t cache;
    void       *block;

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);

    block = jit_cache_alloc_executable(&cache, 4096U);
    POUND_REQUIRE_PTR_NON_NULL(block);

    // The whole point of the W^X state machine: executable memory is handed out
    // writable and *not* executable, and the transition is explicit. A block that
    // arrived already executable would mean the cache had mapped a page both ways at
    // once, which is the thing this design exists to make impossible.
    POUND_CHECK_MSG(JIT_BLOCK_STATE_READ_WRITE == jit_cache_block_state(&cache, block),
                    "a fresh executable block is in state %u, not read-write",
                    (unsigned int)jit_cache_block_state(&cache, block));
    POUND_CHECK_MSG(!jit_cache_is_executable(&cache, block),
                    "a fresh executable block reports itself executable");
    POUND_CHECK_EQ_U64(read_stats(&cache).rx_blocks, 0U);
    POUND_CHECK_EQ_U64(read_stats(&cache).rw_blocks, 1U);

    // Writable, across the whole capacity.
    const size_t capacity = jit_cache_usable_size(&cache, block);

    memset(block, 0xC3, capacity);

    for (size_t i = 0U; i < capacity; ++i)
    {
        if (((const uint8_t *)block)[i] != 0xC3u)
        {
            POUND_CHECK_MSG(false, "byte %zu of the executable block did not take the write", i);
            break;
        }
    }

    // And only then executable.
    POUND_REQUIRE(POUND_SUCCESS == jit_cache_protect_rx(&cache, block));
    POUND_CHECK(JIT_BLOCK_STATE_READ_EXECUTE == jit_cache_block_state(&cache, block));
    POUND_CHECK(jit_cache_is_executable(&cache, block));

    const jit_cache_stats_t stats = read_stats(&cache);

    POUND_CHECK_EQ_U64(stats.rx_blocks, 1U);
    POUND_CHECK_EQ_U64(stats.rw_blocks, 0U);
    POUND_CHECK_EQ_U64(stats.rx_transitions, 1U);
    POUND_CHECK_EQ_U64(stats.rw_transitions, 0U);

    // And back to writable, for the patch-in-place path.
    POUND_REQUIRE(POUND_SUCCESS == jit_cache_protect_rw(&cache, block));
    POUND_CHECK(JIT_BLOCK_STATE_READ_WRITE == jit_cache_block_state(&cache, block));
    POUND_CHECK(!jit_cache_is_executable(&cache, block));

    const jit_cache_stats_t after = read_stats(&cache);

    POUND_CHECK_EQ_U64(after.rw_transitions, 1U);
    POUND_CHECK_EQ_U64(after.rw_blocks, 1U);
    POUND_CHECK_EQ_U64(after.rx_blocks, 0U);

    jit_cache_free_executable(&cache, block);
    check_accounting(&cache, "after the W^X round trip");
    jit_cache_destroy(&cache);
}

POUND_TEST(jit_cache, the_emitted_code_actually_runs)
{
    jit_cache_t cache;
    uint8_t     code[16];
    const uint64_t expected = 0x1234U;

    const size_t length = encode_return_constant(code, sizeof(code), expected);

    // A host architecture with no encoding here is a gap in the test, not a defect in
    // the cache. Everything else in the suite still covers the allocator there, so the
    // case returns rather than failing.
    if (0U == length)
    {
        return;
    }

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);

    void *const block = jit_cache_alloc_executable(&cache, length);

    POUND_REQUIRE_PTR_NON_NULL(block);
    POUND_CHECK_MSG(jit_cache_usable_size(&cache, block) >= length,
                    "a %zu-byte request got %zu bytes",
                    length,
                    jit_cache_usable_size(&cache, block));

    memcpy(block, code, length);

    POUND_REQUIRE(POUND_SUCCESS == jit_cache_protect_rx(&cache, block));

    // A cache that allocates, protects and never executes anything has proved nothing.
    // This is the case that says the memory really is executable and really does hold
    // what was written to it.
    POUND_CHECK_EQ_U64(entry_of(block)(), expected);

    jit_cache_free_executable(&cache, block);
    check_accounting(&cache, "after running a block");
    jit_cache_destroy(&cache);
}

POUND_TEST(jit_cache, a_block_can_be_patched_in_place)
{
    jit_cache_t cache;
    uint8_t     code[16];

    const size_t length = encode_return_constant(code, sizeof(code), 0x1111U);

    if (0U == length)
    {
        return;
    }

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);

    void *const block = jit_cache_alloc_executable(&cache, length);
    POUND_REQUIRE_PTR_NON_NULL(block);

    memcpy(block, code, length);
    POUND_REQUIRE(POUND_SUCCESS == jit_cache_protect_rx(&cache, block));
    POUND_CHECK_EQ_U64(entry_of(block)(), 0x1111U);

    // Patch it. This is the path where a stale instruction cache shows up as a *wrong
    // answer* rather than a fault, which is why it is worth a case of its own: the
    // rewrite goes through read-write, and the flush on the way back to read-execute
    // has to happen.
    POUND_REQUIRE(POUND_SUCCESS == jit_cache_protect_rw(&cache, block));

    const size_t second = encode_return_constant(code, sizeof(code), 0x2222U);

    POUND_REQUIRE_MSG(second == length,
                      "the two encodings differ in length (%zu then %zu), so the patch would "
                      "not be in place",
                      length,
                      second);

    memcpy(block, code, second);
    POUND_REQUIRE(POUND_SUCCESS == jit_cache_protect_rx(&cache, block));
    POUND_CHECK_EQ_U64(entry_of(block)(), 0x2222U);

    // And back once more, to be sure the transition is repeatable rather than a
    // one-shot.
    POUND_REQUIRE(POUND_SUCCESS == jit_cache_protect_rw(&cache, block));
    const size_t third = encode_return_constant(code, sizeof(code), 0x3333U);

    memcpy(block, code, third);
    POUND_REQUIRE(POUND_SUCCESS == jit_cache_protect_rx(&cache, block));
    POUND_CHECK_EQ_U64(entry_of(block)(), 0x3333U);

    const jit_cache_stats_t stats = read_stats(&cache);

    POUND_CHECK_EQ_U64(stats.rw_transitions, 2U);
    POUND_CHECK_EQ_U64(stats.rx_transitions, 3U);

    jit_cache_free_executable(&cache, block);
    check_accounting(&cache, "after patching a block in place");
    jit_cache_destroy(&cache);
}

POUND_TEST(jit_cache, protection_transitions_are_idempotent)
{
    jit_cache_t cache;

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);

    void *const block = jit_cache_alloc_executable(&cache, 256U);
    POUND_REQUIRE_PTR_NON_NULL(block);

    pound_test_log_reset();

    // Re-protecting a block that is already in the target state has done nothing wrong.
    // Reporting an error would only train callers to ignore the function and check the
    // return value for something else.
    POUND_REQUIRE(POUND_SUCCESS == jit_cache_protect_rx(&cache, block));
    POUND_CHECK_MSG(1U == read_stats(&cache).rx_transitions,
                    "the first transition was not counted");

    POUND_CHECK(POUND_SUCCESS == jit_cache_protect_rx(&cache, block));
    POUND_CHECK_MSG(1U == read_stats(&cache).rx_transitions,
                    "a repeated read-execute transition was counted as a change");
    POUND_CHECK(pound_test_log_contains("was a no-op"));

    POUND_REQUIRE(POUND_SUCCESS == jit_cache_protect_rw(&cache, block));
    POUND_CHECK(POUND_SUCCESS == jit_cache_protect_rw(&cache, block));
    POUND_CHECK_MSG(1U == read_stats(&cache).rw_transitions,
                    "a repeated read-write transition was counted as a change");

    // The state after all four calls is the one the last of them asked for, and the
    // counters agree with it.
    POUND_CHECK(JIT_BLOCK_STATE_READ_WRITE == jit_cache_block_state(&cache, block));

    const jit_cache_stats_t stats = read_stats(&cache);

    POUND_CHECK_EQ_U64(stats.rx_transitions, 1U);
    POUND_CHECK_EQ_U64(stats.rw_transitions, 1U);
    POUND_CHECK_EQ_U64(stats.rw_blocks, 1U);
    POUND_CHECK_EQ_U64(stats.rx_blocks, 0U);

    jit_cache_free_executable(&cache, block);
    jit_cache_destroy(&cache);
}

POUND_TEST(jit_cache, protection_refuses_a_data_block)
{
    jit_cache_t cache;
    int         scratch = 0;

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);

    void *const data = jit_cache_alloc(&cache, 256U);
    POUND_REQUIRE_PTR_NON_NULL(data);
    pound_test_log_reset();

    // A data block does not own whole pages, so flipping it would change the
    // protection of whatever shares those pages -- which, in a chunk carved for other
    // blocks, is other live data. There is no correct answer here but a refusal.
    POUND_CHECK_EQ_I64(jit_cache_protect_rx(&cache, data), POUND_ERROR_MEMORY_FAULT);
    POUND_CHECK_EQ_I64(jit_cache_protect_rw(&cache, data), POUND_ERROR_MEMORY_FAULT);
    POUND_CHECK(pound_test_log_contains("does not own whole pages"));

    // Which leaves the data writable, as it was.
    memset(data, 0x77, 256U);
    POUND_CHECK_EQ_U64(read_stats(&cache).rx_transitions, 0U);
    POUND_CHECK_EQ_U64(read_stats(&cache).rw_transitions, 0U);

    // A foreign pointer is refused with a code that says "not a live block here".
    POUND_CHECK_EQ_I64(jit_cache_protect_rx(&cache, &scratch), POUND_ERROR_DOUBLE_FREE);
    POUND_CHECK_EQ_I64(jit_cache_protect_rw(&cache, &scratch), POUND_ERROR_DOUBLE_FREE);

    // And a NULL block is an argument error, not a "not a live block".
    POUND_CHECK_EQ_I64(jit_cache_protect_rx(&cache, NULL), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(jit_cache_protect_rx(NULL, data), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(jit_cache_protect_rw(NULL, data), POUND_ERROR_INVALID_ARGUMENT);

    jit_cache_free(&cache, data);
    check_accounting(&cache, "after every refused protection change");
    jit_cache_destroy(&cache);
}

POUND_TEST(jit_cache, state_is_free_for_anything_the_cache_does_not_own)
{
    jit_cache_t cache;
    int         scratch = 0;

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);

    void *const block = jit_cache_alloc_executable(&cache, 256U);
    POUND_REQUIRE_PTR_NON_NULL(block);
    POUND_REQUIRE(POUND_SUCCESS == jit_cache_protect_rx(&cache, block));

    // The question "is this executable" is only ever asked about pointers the caller
    // believes the cache owns, and answering "no" for everything else is the safe
    // direction to be wrong in.
    POUND_CHECK(JIT_BLOCK_STATE_FREE == jit_cache_block_state(&cache, &scratch));
    POUND_CHECK(JIT_BLOCK_STATE_FREE == jit_cache_block_state(&cache, NULL));
    POUND_CHECK(JIT_BLOCK_STATE_FREE == jit_cache_block_state(NULL, block));
    POUND_CHECK(!jit_cache_is_executable(&cache, &scratch));
    POUND_CHECK(!jit_cache_is_executable(NULL, block));

    jit_cache_free_executable(&cache, block);

    // A released block reports free again rather than keeping its last state.
    POUND_CHECK(JIT_BLOCK_STATE_FREE == jit_cache_block_state(&cache, block));
    POUND_CHECK(!jit_cache_is_executable(&cache, block));

    jit_cache_destroy(&cache);
}

POUND_TEST(jit_cache, data_and_executable_blocks_stay_independent)
{
    jit_cache_t cache;
    void       *data[8];
    void       *code[8];

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);

    for (size_t i = 0U; i < 8U; ++i)
    {
        // Interleaved, so the two kinds are carved from the same chunk and land at
        // different alignments. This is the case where a data block's presence could
        // push an executable block's offset off a page boundary, and where reusing a
        // data block's freed extent for an executable block would hand out a block
        // whose address is not page aligned.
        data[i] = jit_cache_alloc(&cache, 512U);
        code[i] = jit_cache_alloc_executable(&cache, 512U);

        POUND_REQUIRE_MSG(NULL != data[i], "data block %zu was refused", i);
        POUND_REQUIRE_MSG(NULL != code[i], "executable block %zu was refused", i);
    }

    const size_t page = read_resolved(&cache).page_size;

    for (size_t i = 0U; i < 8U; ++i)
    {
        POUND_CHECK_MSG(0U == ((uintptr_t)code[i] % page),
                        "executable block %zu at %p is not %zu-byte aligned",
                        i,
                        code[i],
                        page);
    }

    // Free every data block, leaving holes scattered through the chunk at arbitrary
    // offsets. Then ask for executable blocks: the cache must not take those holes
    // unless the hole's offset happens to satisfy a page alignment, which is exactly
    // what the extent search has to check.
    for (size_t i = 0U; i < 8U; ++i)
    {
        jit_cache_free(&cache, data[i]);
    }

    void *reused[8];

    for (size_t i = 0U; i < 8U; ++i)
    {
        reused[i] = jit_cache_alloc_executable(&cache, 512U);
        POUND_REQUIRE_MSG(NULL != reused[i], "reused executable block %zu was refused", i);

        POUND_CHECK_MSG(0U == ((uintptr_t)reused[i] % page),
                        "an executable block reused from a data block's freed extent landed at %p, "
                        "which is not %zu-byte aligned",
                        reused[i],
                        page);
    }

    for (size_t i = 0U; i < 8U; ++i)
    {
        POUND_REQUIRE(POUND_SUCCESS == jit_cache_protect_rx(&cache, code[i]));
        POUND_CHECK(JIT_BLOCK_STATE_READ_EXECUTE == jit_cache_block_state(&cache, code[i]));
        POUND_REQUIRE(POUND_SUCCESS == jit_cache_protect_rx(&cache, reused[i]));
        POUND_CHECK(JIT_BLOCK_STATE_READ_EXECUTE == jit_cache_block_state(&cache, reused[i]));
    }

    check_accounting(&cache, "with mixed blocks live");

    for (size_t i = 0U; i < 8U; ++i)
    {
        jit_cache_free_executable(&cache, code[i]);
        jit_cache_free_executable(&cache, reused[i]);
    }

    check_accounting(&cache, "after every mixed block was released");
    jit_cache_destroy(&cache);
}

POUND_TEST(jit_cache, an_extent_too_short_to_reach_an_aligned_address_is_not_used)
{
    jit_cache_t cache;

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);

    const size_t page = read_resolved(&cache).page_size;

    // Two 64-byte blocks side by side, so releasing the second leaves a 64-byte extent
    // at offset 64. That extent is far too short to reach the next page boundary: on a
    // 4 KiB page it is 4032 bytes away from one, a gap larger than the extent itself.
    void *const first  = jit_cache_alloc(&cache, 64U);
    void *const second = jit_cache_alloc(&cache, 64U);

    POUND_REQUIRE_PTR_NON_NULL(first);
    POUND_REQUIRE_PTR_NON_NULL(second);

    jit_cache_free(&cache, second);
    check_accounting(&cache, "with a 64-byte extent at offset 64");

    // An executable block needs a whole page, so that extent cannot serve one. The
    // reason this case exists is the way a free-extent search gets this wrong: judging
    // the extent by `size - (aligned - offset)` subtracts a larger number from a
    // smaller one and wraps to a value near SIZE_MAX, after which the extent looks able
    // to hold anything.
    void *const code = jit_cache_alloc_executable(&cache, 64U);

    POUND_REQUIRE_PTR_NON_NULL(code);
    POUND_CHECK_MSG(0U == ((uintptr_t)code % page),
                    "an executable block landed at %p, which is not %zu-byte aligned",
                    code,
                    page);

    // The wrapped size cannot be caught by the accounting invariant, and the reason is
    // worth stating so nobody trusts the arithmetic check for it: the wrapped extent
    // reports 2^64 - 3968 bytes, and summing that with the other free bytes wraps once
    // more back to the correct total. `reserved == live + free` holds either way.
    //
    // What the wrap does break is refusal. With the extent believed to be effectively
    // infinite, a request the cache cannot honour at all is answered from it -- the cache
    // hands out a block that is not backed by any memory, and the caller faults on its
    // first store. That is the observable difference this case asserts.
    //
    // The request is a byte past the ceiling rather than a byte past a chunk. Being larger
    // than one chunk is no longer unsatisfiable on its own: a request that big gets a chunk
    // carved for it, which is exactly what makes the engine's 16 MiB code buffer possible.
    // Only the ceiling still means "no", so that is what has to be tested -- otherwise this
    // case would be asserting the absence of a feature on the strength of a bug in a
    // neighbouring arithmetic path, and whoever fixed that bug would be told to undo it.
    POUND_CHECK_MSG(NULL == jit_cache_alloc(&cache, JIT_TEST_CEILING + 1U),
                    "a request past the ceiling was served from a 64-byte extent, which means "
                    "the extent is believed to be far longer than it is");
    POUND_CHECK_MSG(NULL == jit_cache_alloc_executable(&cache, JIT_TEST_CEILING + 1U),
                    "an executable request past the ceiling was served from a 64-byte extent");

    // The short extent is untouched rather than consumed or discarded, and still serves
    // what it can serve.
    void *const again = jit_cache_alloc(&cache, 64U);

    POUND_REQUIRE_PTR_NON_NULL(again);
    check_accounting(&cache, "after the short extent served a data block");

    jit_cache_free(&cache, again);
    jit_cache_free_executable(&cache, code);
    jit_cache_free(&cache, first);

    check_accounting(&cache, "with nothing live");
    POUND_CHECK_EQ_U64(jit_cache_reclaim(&cache), JIT_TEST_CHUNK);

    jit_cache_destroy(&cache);
}

POUND_TEST(jit_cache, executable_cycles_do_not_grow_the_cache)
{
    jit_cache_t cache;

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);

    const size_t page = read_resolved(&cache).page_size;

    for (size_t round = 0U; round < 100U; ++round)
    {
        void *blocks[8];

        for (size_t i = 0U; i < 8U; ++i)
        {
            blocks[i] = jit_cache_alloc_executable(&cache, 256U);
            POUND_REQUIRE_MSG(NULL != blocks[i], "round %zu block %zu was refused", round, i);
        }

        for (size_t i = 0U; i < 8U; ++i)
        {
            POUND_REQUIRE(POUND_SUCCESS == jit_cache_protect_rx(&cache, blocks[i]));
            POUND_REQUIRE(POUND_SUCCESS == jit_cache_protect_rw(&cache, blocks[i]));
        }

        for (size_t i = 0U; i < 8U; ++i)
        {
            jit_cache_free_executable(&cache, blocks[i]);
        }

        if (0U == (round % 25U))
        {
            check_accounting(&cache, "mid-executable-cycle");
        }
    }

    // Eight page-sized blocks is 32 KiB on a 4 KiB page, well inside one chunk, so a
    // cache that coalesces and reuses stays at one no matter how many rounds run.
    const jit_cache_stats_t stats = read_stats(&cache);

    POUND_CHECK_MSG(1U == stats.chunk_count,
                    "100 executable alloc-and-free rounds grew the cache to %zu chunks",
                    stats.chunk_count);
    POUND_CHECK_EQ_U64(stats.live_blocks, 0U);
    POUND_CHECK_EQ_U64(stats.rx_transitions, 800U);
    POUND_CHECK_EQ_U64(stats.rw_transitions, 800U);
    POUND_CHECK_EQ_U64(jit_cache_reclaim(&cache), JIT_TEST_CHUNK);
    POUND_CHECK_EQ_U64(read_stats(&cache).chunk_count, 0U);

    (void)page;
    jit_cache_destroy(&cache);
}

// -----------------------------------------------------------------------------
// The ceiling
// -----------------------------------------------------------------------------

POUND_TEST(jit_cache, the_ceiling_is_enforced_and_reported)
{
    jit_cache_t cache;

    // Room for exactly two chunks, and nothing more. The blocks are sized to leave a
    // tail in each chunk, so the "space is still usable" half of this case is a real
    // check rather than an accident of a full chunk.
    const size_t block_bytes = 200U * 1024U;

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CHUNK * 2U);

    void *const first  = jit_cache_alloc(&cache, block_bytes);
    void *const second = jit_cache_alloc(&cache, block_bytes);

    POUND_REQUIRE_PTR_NON_NULL(first);
    POUND_REQUIRE_PTR_NON_NULL(second);

    const jit_cache_stats_t full = read_stats(&cache);

    POUND_CHECK_EQ_U64(full.chunk_count, 2U);
    POUND_CHECK_EQ_U64(full.reserved_bytes, JIT_TEST_CHUNK * 2U);
    POUND_CHECK_EQ_U64(full.alloc_failures, 0U);

    // One more would cross the ceiling. A ceiling the cache quietly ignores is worse
    // than no ceiling, because the caller that set it is bounding the emulator's
    // footprint.
    pound_test_log_reset();
    POUND_CHECK_PTR_NULL(jit_cache_alloc(&cache, block_bytes));
    POUND_CHECK(pound_test_log_contains("ceiling are already committed"));

    const jit_cache_stats_t after = read_stats(&cache);

    POUND_CHECK_MSG(1U == after.alloc_failures,
                    "the refusal was not counted as an allocation failure");
    POUND_CHECK_EQ_U64(after.chunk_count, 2U);
    POUND_CHECK_EQ_U64(after.reserved_bytes, JIT_TEST_CHUNK * 2U);

    // And the tails in those two chunks are still usable, so the ceiling caps the
    // footprint rather than the usefulness.
    void *const small = jit_cache_alloc(&cache, 1024U);
    POUND_REQUIRE_PTR_NON_NULL(small);
    POUND_CHECK_EQ_U64(read_stats(&cache).chunk_count, 2U);

    // Releasing room lets the next growth through again.
    jit_cache_free(&cache, small);
    jit_cache_free(&cache, first);
    POUND_REQUIRE_PTR_NON_NULL(jit_cache_alloc(&cache, block_bytes));
    POUND_CHECK_EQ_U64(read_stats(&cache).chunk_count, 2U);

    check_accounting(&cache, "against the ceiling");
    jit_cache_destroy(&cache);
}

POUND_TEST(jit_cache, a_chunk_grows_the_cache_rather_than_overspilling)
{
    jit_cache_t cache;

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);

    // More than one chunk's worth, in awkward sizes, so several chunks are needed and
    // the tail of each is partly used.
    void *blocks[16];

    for (size_t i = 0U; i < 16U; ++i)
    {
        blocks[i] = jit_cache_alloc(&cache, 40U * 1024U);

        if (NULL == blocks[i])
        {
            break;
        }
    }

    POUND_REQUIRE_MSG(NULL != blocks[15],
                      "640 KiB of 40 KiB blocks did not fit under a 1 MiB ceiling; the cache "
                      "must have refused rather than spanned a chunk boundary");

    const jit_cache_stats_t stats = read_stats(&cache);

    POUND_CHECK_MSG(stats.chunk_count >= 3U,
                    "640 KiB of blocks fitted in %zu chunks of %u bytes",
                    stats.chunk_count,
                    (unsigned int)JIT_TEST_CHUNK);

    check_accounting(&cache, "across several chunks");

    for (size_t i = 0U; i < 16U; ++i)
    {
        jit_cache_free(&cache, blocks[i]);
    }

    check_accounting(&cache, "after freeing across several chunks");

    // Now everything is free, so every chunk is trailing-empty and all of them go back.
    POUND_CHECK_EQ_U64(jit_cache_reclaim(&cache), stats.reserved_bytes);
    POUND_CHECK_EQ_U64(read_stats(&cache).chunk_count, 0U);

    jit_cache_destroy(&cache);
}

POUND_TEST(jit_cache, a_full_chunk_does_not_hand_out_bytes_past_its_end)
{
    // The cache's minimum chunk size plus one page: page-aligned, comfortably inside the
    // documented bounds, and -- the reason for the odd value -- deliberately *not* a
    // multiple of the largest supported alignment. A chunk size that happened to be a
    // multiple of 64 KiB could never produce a cursor that rounds up past the end of its
    // chunk, so on a conventional chunk size the bound this case exists to test could
    // not be reached at all.
    const size_t chunk_bytes = JIT_TEST_CHUNK + 4096U;

    jit_cache_t cache;

    init_cache(&cache, chunk_bytes, chunk_bytes * 2U);

    // Fill the first chunk exactly. This block owns every byte of it, so there is no
    // free extent and no tail left above the cursor, and the cursor sits precisely on
    // the chunk's end -- which is the only state in which rounding it up moves.
    void *const filler = jit_cache_alloc(&cache, chunk_bytes);

    POUND_REQUIRE_PTR_NON_NULL(filler);
    check_accounting(&cache, "with the first chunk full to its last byte");

    // Rounding a cursor of 266240 up to a 64 KiB alignment gives 327680, which is 61440
    // bytes past the end of a 266240-byte chunk. An allocation that subtracts the two
    // without first checking that the subtraction is legal wraps to a number near
    // SIZE_MAX, concludes the chunk has room, and carves a block out of memory the cache
    // never reserved.
    //
    // The result is deceptively plausible: a real address, on a real page boundary, from
    // a real mapping. It is simply not this cache's memory, and the first write to it
    // corrupts an unrelated allocation. So the block is not written to here -- the case
    // asserts the arithmetic instead, which is what actually fails.
    void *const code = jit_cache_alloc_executable_aligned(&cache, JIT_CACHE_MAX_ALIGNMENT, 64U);

    POUND_REQUIRE_PTR_NON_NULL(code);

    POUND_CHECK_MSG(0U == ((uintptr_t)code % JIT_CACHE_MAX_ALIGNMENT),
                    "an executable block landed at %p, which is not %zu-byte aligned",
                    code,
                    JIT_CACHE_MAX_ALIGNMENT);
    POUND_CHECK_MSG(((uintptr_t)code < (uintptr_t)filler)
                        || ((uintptr_t)code >= ((uintptr_t)filler + chunk_bytes)),
                    "an executable block landed at %p, which is inside the full chunk starting "
                    "at %p and %zu bytes long",
                    code,
                    filler,
                    chunk_bytes);

    // With the bound honoured the only remaining option was a second chunk, which the
    // ceiling of two chunks' worth allows exactly.
    POUND_CHECK_MSG(2U == read_stats(&cache).chunk_count,
                    "a full chunk of %zu bytes did not force a second chunk, so the block came "
                    "from somewhere it should not have",
                    chunk_bytes);

    // The cursor of the full chunk is now past its own end, so summing what is left
    // above it underflows and the accounting collapses.
    check_accounting(&cache, "after a block that outgrew the chunk it came from");

    POUND_REQUIRE(POUND_SUCCESS == jit_cache_protect_rx(&cache, code));
    POUND_CHECK(JIT_BLOCK_STATE_READ_EXECUTE == jit_cache_block_state(&cache, code));

    jit_cache_free_executable(&cache, code);
    jit_cache_free(&cache, filler);

    check_accounting(&cache, "with nothing live");
    POUND_CHECK_EQ_U64(jit_cache_reclaim(&cache), chunk_bytes * 2U);

    jit_cache_destroy(&cache);
}

// -----------------------------------------------------------------------------
// Poisoning
// -----------------------------------------------------------------------------

POUND_TEST(jit_cache, poisoning_fills_freshly_allocated_blocks)
{
    jit_cache_t cache;
    void       *first;
    void       *second;

    init_cache_poisoned(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING, true);
    POUND_CHECK(read_resolved(&cache).poison);

    first = jit_cache_alloc(&cache, 128U);
    POUND_REQUIRE_PTR_NON_NULL(first);

    for (size_t i = 0U; i < 128U; ++i)
    {
        if (((const uint8_t *)first)[i] != (uint8_t)JIT_CACHE_POISON_BYTE)
        {
            POUND_CHECK_MSG(false,
                            "byte %zu of a poisoned data block is 0x%02X, not 0x%02X",
                            i,
                            ((const uint8_t *)first)[i],
                            (unsigned int)JIT_CACHE_POISON_BYTE);
            break;
        }
    }

    // Poisoning covers the whole capacity, not just the requested bytes, because a
    // caller is entitled to trust the reported capacity.
    const size_t capacity = jit_cache_usable_size(&cache, first);

    for (size_t i = 128U; i < capacity; ++i)
    {
        if (((const uint8_t *)first)[i] != (uint8_t)JIT_CACHE_POISON_BYTE)
        {
            POUND_CHECK_MSG(false,
                            "byte %zu of a poisoned block's capacity is 0x%02X, not 0x%02X",
                            i,
                            ((const uint8_t *)first)[i],
                            (unsigned int)JIT_CACHE_POISON_BYTE);
            break;
        }
    }

    // Overwrite, release, and take the same space again: the poison must be back. That
    // is the hazard it exists for -- code or data executed before it was written.
    memset(first, 0x5A, capacity);
    jit_cache_free(&cache, first);

    second = jit_cache_alloc(&cache, 128U);
    POUND_REQUIRE_PTR_NON_NULL(second);
    POUND_CHECK_PTR_EQ(second, first);

    for (size_t i = 0U; i < 128U; ++i)
    {
        if (((const uint8_t *)second)[i] != (uint8_t)JIT_CACHE_POISON_BYTE)
        {
            POUND_CHECK_MSG(false,
                            "byte %zu of a reallocated poisoned block is 0x%02X, so the previous "
                            "contents survived",
                            i,
                            ((const uint8_t *)second)[i]);
            break;
        }
    }

    jit_cache_free(&cache, second);
    jit_cache_destroy(&cache);
}

POUND_TEST(jit_cache, an_unpoisoned_cache_leaves_memory_alone)
{
    jit_cache_t cache;

    init_cache_poisoned(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING, false);
    POUND_CHECK(!read_resolved(&cache).poison);

    void *const block = jit_cache_alloc(&cache, 64U);
    POUND_REQUIRE_PTR_NON_NULL(block);

    // Freshly reserved memory from the OS is already zero, and the cache is not
    // supposed to write to it, so the block must arrive as zeroes. Asserting that is
    // the only way to tell "not poisoned" from "poisoned with something that happens
    // to be zero".
    for (size_t i = 0U; i < 64U; ++i)
    {
        if (((const uint8_t *)block)[i] != 0u)
        {
            POUND_CHECK_MSG(false,
                            "byte %zu of an unpoisoned block is 0x%02X; the cache wrote to it",
                            i,
                            ((const uint8_t *)block)[i]);
            break;
        }
    }

    jit_cache_free(&cache, block);
    jit_cache_destroy(&cache);
}

// -----------------------------------------------------------------------------
// Statistics
// -----------------------------------------------------------------------------

POUND_TEST(jit_cache, statistics_track_the_cache)
{
    jit_cache_t       cache;
    jit_cache_stats_t stats;

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);

    memset(&stats, 0, sizeof(stats));
    POUND_REQUIRE(POUND_SUCCESS == jit_cache_get_stats(&cache, &stats));

    // Nothing has happened yet.
    POUND_CHECK_EQ_U64(stats.chunk_count, 0U);
    POUND_CHECK_EQ_U64(stats.reserved_bytes, 0U);
    POUND_CHECK_EQ_U64(stats.live_blocks, 0U);
    POUND_CHECK_EQ_U64(stats.allocations, 0U);
    POUND_CHECK_EQ_U64(stats.frees, 0U);
    POUND_CHECK_EQ_U64(stats.rx_transitions, 0U);

    void *const data = jit_cache_alloc(&cache, 1024U);
    void *const code = jit_cache_alloc_executable(&cache, 1024U);

    POUND_REQUIRE_PTR_NON_NULL(data);
    POUND_REQUIRE_PTR_NON_NULL(code);

    const jit_cache_stats_t live = read_stats(&cache);

    POUND_CHECK_EQ_U64(live.allocations, 2U);
    POUND_CHECK_EQ_U64(live.live_blocks, 2U);
    POUND_CHECK_EQ_U64(live.rw_blocks, 2U);
    POUND_CHECK_EQ_U64(live.rx_blocks, 0U);
    POUND_CHECK_EQ_U64(live.frees, 0U);
    POUND_CHECK_MSG(live.live_bytes >= 2048U, "live bytes %zu is below the two 1 KiB requests", live.live_bytes);
    POUND_CHECK_MSG(live.chunk_count >= 1U, "an allocation reserved no chunk");

    POUND_REQUIRE(POUND_SUCCESS == jit_cache_protect_rx(&cache, code));

    const jit_cache_stats_t flipped = read_stats(&cache);

    POUND_CHECK_EQ_U64(flipped.rw_blocks, 1U);
    POUND_CHECK_EQ_U64(flipped.rx_blocks, 1U);
    POUND_CHECK_EQ_U64(flipped.rx_transitions, 1U);

    jit_cache_free(&cache, data);
    jit_cache_free_executable(&cache, code);

    const jit_cache_stats_t emptied = read_stats(&cache);

    POUND_CHECK_EQ_U64(emptied.live_blocks, 0U);
    POUND_CHECK_EQ_U64(emptied.live_bytes, 0U);
    POUND_CHECK_EQ_U64(emptied.frees, 2U);
    POUND_CHECK_MSG(emptied.free_bytes == emptied.reserved_bytes,
                    "%zu bytes are free but %zu are reserved",
                    emptied.free_bytes,
                    emptied.reserved_bytes);

    jit_cache_destroy(&cache);
}

POUND_TEST(jit_cache, statistics_reject_null_arguments_and_an_uninitialised_cache)
{
    jit_cache_t       cache = unopened_cache();
    jit_cache_stats_t stats;

    pound_test_log_reset();
    POUND_CHECK_EQ_I64(jit_cache_get_stats(NULL, &stats), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(jit_cache_get_stats(&cache, &stats), POUND_ERROR_NOT_INITIALIZED);
    POUND_CHECK(pound_test_log_contains("never initialised"));

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);
    POUND_CHECK_EQ_I64(jit_cache_get_stats(&cache, NULL), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(jit_cache_get_stats(NULL, NULL), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK(pound_test_log_contains("cache or output is NULL"));

    jit_cache_destroy(&cache);
}

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

POUND_TEST(jit_cache, reset_clears_the_cache_and_keeps_its_configuration)
{
    jit_cache_t         cache;
    jit_cache_resolved_t before;
    jit_cache_resolved_t after;

    init_cache_poisoned(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING, true);

    memset(&before, 0, sizeof(before));
    POUND_REQUIRE(POUND_SUCCESS == jit_cache_describe(&cache, &before));

    void *const block = jit_cache_alloc_executable(&cache, 4096U);
    POUND_REQUIRE_PTR_NON_NULL(block);
    POUND_REQUIRE(POUND_SUCCESS == jit_cache_protect_rx(&cache, block));

    // Reset with something still live, which is what a guest changing game does. It
    // must be loud about the leak and must not be blocked by it.
    pound_test_log_reset();
    jit_cache_reset(&cache);
    POUND_CHECK(pound_test_log_contains("still live"));

    const jit_cache_stats_t stats = read_stats(&cache);

    POUND_CHECK_EQ_U64(stats.chunk_count, 0U);
    POUND_CHECK_EQ_U64(stats.reserved_bytes, 0U);
    POUND_CHECK_EQ_U64(stats.live_blocks, 0U);
    POUND_CHECK_EQ_U64(stats.live_bytes, 0U);
    POUND_CHECK_EQ_U64(stats.allocations, 0U);
    POUND_CHECK_EQ_U64(stats.rx_transitions, 0U);

    // The configuration survives, so the cache is immediately usable.
    memset(&after, 0, sizeof(after));
    POUND_REQUIRE(POUND_SUCCESS == jit_cache_describe(&cache, &after));
    POUND_CHECK_EQ_U64(after.chunk_bytes, before.chunk_bytes);
    POUND_CHECK_EQ_U64(after.max_bytes, before.max_bytes);
    POUND_CHECK_EQ_U64(after.page_size, before.page_size);
    POUND_CHECK_EQ_U64(after.poison, before.poison);

    // And it works, which is the difference between a reset and a destroy.
    void *const again = jit_cache_alloc(&cache, 256U);
    POUND_REQUIRE_PTR_NON_NULL(again);
    POUND_CHECK_EQ_U64(read_stats(&cache).chunk_count, 1U);
    check_accounting(&cache, "after a reset and reuse");

    jit_cache_free(&cache, again);
    jit_cache_destroy(&cache);
}

POUND_TEST(jit_cache, reset_rejects_a_null_or_uninitialised_cache)
{
    jit_cache_t cache = unopened_cache();

    pound_test_log_reset();
    jit_cache_reset(NULL);
    POUND_CHECK(pound_test_log_contains("cache context is NULL"));

    jit_cache_reset(&cache);
    POUND_CHECK(pound_test_log_contains("never initialised"));
}

POUND_TEST(jit_cache, destroy_is_safe_with_live_blocks_and_closes_the_cache)
{
    jit_cache_t cache;

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);

    void *const first  = jit_cache_alloc(&cache, 1024U);
    void *const second = jit_cache_alloc_executable(&cache, 1024U);

    POUND_REQUIRE_PTR_NON_NULL(first);
    POUND_REQUIRE_PTR_NON_NULL(second);
    POUND_REQUIRE(POUND_SUCCESS == jit_cache_protect_rx(&cache, second));

    pound_test_log_reset();
    jit_cache_destroy(&cache);
    POUND_CHECK(pound_test_log_contains("still live"));

    // The released pointer is not the cache's any more, and nothing may be done with
    // it. A caller that ignores this and carries on is a bug the cache cannot fix, but
    // it can refuse to pretend to still be usable.
    POUND_CHECK_EQ_U64(jit_cache_usable_size(&cache, first), 0U);
    POUND_CHECK_EQ_U64(jit_cache_usable_size(&cache, second), 0U);
    POUND_CHECK_PTR_NULL(jit_cache_alloc(&cache, 1024U));
    POUND_CHECK_PTR_NULL(jit_cache_alloc_executable(&cache, 1024U));
    POUND_CHECK_EQ_I64(jit_cache_protect_rx(&cache, second), POUND_ERROR_NOT_INITIALIZED);
    POUND_CHECK_EQ_I64(jit_cache_get_stats(&cache, NULL), POUND_ERROR_INVALID_ARGUMENT);

    // A second destroy is harmless and says why.
    pound_test_log_reset();
    jit_cache_destroy(&cache);
    POUND_CHECK(pound_test_log_contains("never initialised"));

    // And so is destroying a null cache.
    pound_test_log_reset();
    jit_cache_destroy(NULL);
    POUND_CHECK(pound_test_log_contains("cache context is NULL"));
}

POUND_TEST(jit_cache, destroy_returns_every_chunk_to_the_system)
{
    jit_cache_t cache;
    void       *blocks[5];

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);

    // Four of these fill a chunk exactly, so the fifth forces a second one. That makes
    // the byte count below exact rather than dependent on how the chunk is carved.
    for (size_t i = 0U; i < 5U; ++i)
    {
        blocks[i] = jit_cache_alloc(&cache, 64U * 1024U);
        POUND_REQUIRE_MSG(NULL != blocks[i], "block %zu was refused", i);
    }

    const jit_cache_stats_t stats = read_stats(&cache);

    POUND_CHECK_MSG(2U == stats.chunk_count, "expected 2 chunks, got %zu", stats.chunk_count);

    for (size_t i = 0U; i < 5U; ++i)
    {
        jit_cache_free(&cache, blocks[i]);
    }

    // With nothing live, the teardown is clean and reports what it handed back, so the
    // accounting is checkable from outside the module. The count is the interesting
    // part: a chunk that leaked, or a report that overstated what was released, would
    // both show up here.
    char expected[64];

    (void)snprintf(expected,
                   sizeof(expected),
                   "returning %zu bytes to the OS",
                   (size_t)stats.chunk_count * JIT_TEST_CHUNK);

    pound_test_log_reset();
    jit_cache_destroy(&cache);

    POUND_CHECK_MSG(pound_test_log_contains(expected), "the teardown did not report \"%s\"", expected);
    POUND_CHECK_MSG(0U == pound_test_log_count_at(LOG_LEVEL_WARN),
                    "a clean teardown logged a warning");
    POUND_CHECK_MSG(0U == pound_test_log_count_at(LOG_LEVEL_ERROR),
                    "a clean teardown logged an error");
}

POUND_TEST(jit_cache, every_byte_the_cache_charges_comes_back_to_the_bucket_it_took)
{
    // A caller with a bucket of its own selected, which is the situation the case is
    // really about. The cache is entitled to charge its own bookkeeping wherever it
    // likes, but a caller that has a bucket active must get it back untouched, and every
    // byte the cache took must be given back to the same ledger it came from.
    (void)memory_subsystem_set_bucket(MEMORY_BUCKET_UI);
    POUND_REQUIRE(MEMORY_BUCKET_UI == memory_subsystem_get_bucket());

    const size_t jit_before = memory_subsystem_get_memory_used_by_bucket((int)MEMORY_BUCKET_JIT_RECOMPILER);
    const size_t ui_before  = memory_subsystem_get_memory_used_by_bucket((int)MEMORY_BUCKET_UI);

    jit_cache_t cache;

    init_cache(&cache, JIT_TEST_CHUNK, JIT_TEST_CEILING);

    // Mix both block kinds, a protection flip, a reuse and a teardown, so every internal
    // allocation the cache makes is exercised at least once: the chunk descriptor, the
    // free-extent array, the block descriptors, and the reallocation of that array as it
    // grows.
    void *const data = jit_cache_alloc(&cache, 4U * 1024U);
    void *const code = jit_cache_alloc_executable(&cache, 4U * 1024U);

    POUND_REQUIRE_PTR_NON_NULL(data);
    POUND_REQUIRE_PTR_NON_NULL(code);

    // A third block forces the free-extent array to grow, which is the one internal
    // allocation whose charge and release happen inside a single call.
    for (size_t i = 0U; i < 8U; ++i)
    {
        void *const scratch = jit_cache_alloc(&cache, 64U);

        POUND_REQUIRE_PTR_NON_NULL(scratch);
        jit_cache_free(&cache, scratch);
    }

    POUND_REQUIRE(POUND_SUCCESS == jit_cache_protect_rx(&cache, code));
    jit_cache_free_executable(&cache, code);
    jit_cache_free(&cache, data);

    jit_cache_destroy(&cache);

    // The point of the case. A descriptor charged to the JIT bucket and released under
    // the caller's bucket leaves the JIT total permanently high and debits a bucket that
    // never took the bytes -- which saturates at zero, so the caller's ledger is simply
    // wrong with no error anywhere. It is a small drift per chunk and per block,
    // invisible in the cache's own statistics because the bytes really are handed back to
    // the allocator, and cumulative over a session's worth of compilation.
    const size_t jit_after = memory_subsystem_get_memory_used_by_bucket((int)MEMORY_BUCKET_JIT_RECOMPILER);
    const size_t ui_after  = memory_subsystem_get_memory_used_by_bucket((int)MEMORY_BUCKET_UI);

    POUND_CHECK_MSG(jit_before == jit_after,
                    "the JIT bucket holds %zu bytes after the cache was destroyed, but it held "
                    "%zu before it existed: %zu bytes of bookkeeping were charged to one bucket "
                    "and released to another",
                    jit_after,
                    jit_before,
                    (jit_after > jit_before) ? (jit_after - jit_before) : (jit_before - jit_after));
    POUND_CHECK_MSG(ui_before == ui_after,
                    "the caller's UI bucket holds %zu bytes after the cache was destroyed, but "
                    "it held %zu before, so the cache released its own bookkeeping against it",
                    ui_after,
                    ui_before);

    // A well-behaved cache leaves the caller's selection alone, and the case has to put
    // the thread back the way it found it either way.
    POUND_CHECK_MSG(MEMORY_BUCKET_UI == memory_subsystem_get_bucket(),
                    "the cache left the caller's bucket as %d",
                    (int)memory_subsystem_get_bucket());

    (void)memory_subsystem_set_bucket(MEMORY_BUCKET_NONE);
}

POUND_TEST_SUITE(jit_cache,
                POUND_TEST_CASE(jit_cache, init_rejects_a_null_cache),
                POUND_TEST_CASE(jit_cache, init_takes_every_default_from_a_null_config),
                POUND_TEST_CASE(jit_cache, init_refuses_a_chunk_that_is_not_a_whole_number_of_pages),
                POUND_TEST_CASE(jit_cache, init_refuses_a_ceiling_below_one_chunk),
                POUND_TEST_CASE(jit_cache, init_refuses_a_chunk_outside_its_bounds),
                POUND_TEST_CASE(jit_cache, describe_rejects_null_arguments),
                POUND_TEST_CASE(jit_cache, data_blocks_are_writable_and_correctly_sized),
                POUND_TEST_CASE(jit_cache, data_blocks_honour_every_legal_alignment),
                POUND_TEST_CASE(jit_cache, allocation_refuses_impossible_requests),
                POUND_TEST_CASE(jit_cache, a_request_past_the_ceiling_is_refused_without_reserving),
                POUND_TEST_CASE(jit_cache, live_blocks_never_alias),
                POUND_TEST_CASE(jit_cache, block_info_reports_the_truth),
                POUND_TEST_CASE(jit_cache, block_info_rejects_null_arguments),
                POUND_TEST_CASE(jit_cache, freed_space_is_reused_at_the_same_address),
                POUND_TEST_CASE(jit_cache, adjacent_extents_coalesce),
                POUND_TEST_CASE(jit_cache, release_refuses_anything_it_did_not_hand_out),
                POUND_TEST_CASE(jit_cache, release_refuses_a_block_of_the_wrong_kind),
                POUND_TEST_CASE(jit_cache, repeated_cycles_do_not_grow_the_cache),
                POUND_TEST_CASE(jit_cache, reclaim_returns_only_trailing_empty_chunks),
                POUND_TEST_CASE(jit_cache, reclaim_rejects_a_null_or_uninitialised_cache),
                POUND_TEST_CASE(jit_cache, executable_blocks_own_whole_pages),
                POUND_TEST_CASE(jit_cache, an_executable_block_is_writable_before_it_is_executable),
                POUND_TEST_CASE(jit_cache, the_emitted_code_actually_runs),
                POUND_TEST_CASE(jit_cache, a_block_can_be_patched_in_place),
                POUND_TEST_CASE(jit_cache, protection_transitions_are_idempotent),
                POUND_TEST_CASE(jit_cache, protection_refuses_a_data_block),
                POUND_TEST_CASE(jit_cache, state_is_free_for_anything_the_cache_does_not_own),
                POUND_TEST_CASE(jit_cache, data_and_executable_blocks_stay_independent),
                POUND_TEST_CASE(jit_cache, an_extent_too_short_to_reach_an_aligned_address_is_not_used),
                POUND_TEST_CASE(jit_cache, executable_cycles_do_not_grow_the_cache),
                POUND_TEST_CASE(jit_cache, the_ceiling_is_enforced_and_reported),
                POUND_TEST_CASE(jit_cache, a_chunk_grows_the_cache_rather_than_overspilling),
                POUND_TEST_CASE(jit_cache, a_full_chunk_does_not_hand_out_bytes_past_its_end),
                POUND_TEST_CASE(jit_cache, poisoning_fills_freshly_allocated_blocks),
                POUND_TEST_CASE(jit_cache, an_unpoisoned_cache_leaves_memory_alone),
                POUND_TEST_CASE(jit_cache, statistics_track_the_cache),
                POUND_TEST_CASE(jit_cache, statistics_reject_null_arguments_and_an_uninitialised_cache),
                POUND_TEST_CASE(jit_cache, reset_clears_the_cache_and_keeps_its_configuration),
                POUND_TEST_CASE(jit_cache, reset_rejects_a_null_or_uninitialised_cache),
                POUND_TEST_CASE(jit_cache, destroy_is_safe_with_live_blocks_and_closes_the_cache),
                POUND_TEST_CASE(jit_cache, destroy_returns_every_chunk_to_the_system),
                POUND_TEST_CASE(jit_cache,
                                every_byte_the_cache_charges_comes_back_to_the_bucket_it_took))

/*** end of file ***/

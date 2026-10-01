//! Unit tests for `memory/memory.h`.
//!
//! The memory subsystem is the emulator's accounting layer: the GUI memory
//! tracker and the JIT code-cache statistics are both driven purely by these
//! counters, so a wrong bucket index silently corrupts every number the debug
//! UI shows.
//!
//! Every assertion here is written as a *delta* against the value observed at
//! the start of the case. `g_host_allocator` is process-global and these cases
//! deliberately leave residue behind (see `cross_bucket_free_saturates`),
//! so absolute totals are not meaningful between cases.

#include "pound_test.h"

#include "memory/memory.h"
#include <stdint.h>

// -----------------------------------------------------------------------------
// Bucket identity
// -----------------------------------------------------------------------------

POUND_TEST(memory, every_named_bucket_is_valid)
{
    POUND_CHECK(memory_bucket_is_valid(MEMORY_BUCKET_NONE));
    POUND_CHECK(memory_bucket_is_valid(MEMORY_BUCKET_UI));
    POUND_CHECK(memory_bucket_is_valid(MEMORY_BUCKET_GUEST_MEMORY));
    POUND_CHECK(memory_bucket_is_valid(MEMORY_BUCKET_JIT_RECOMPILER));
    POUND_CHECK(memory_bucket_is_valid(MEMORY_BUCKET_DEBUG_PROFILING));

    POUND_CHECK(false == memory_bucket_is_valid(MEMORY_BUCKET_COUNT));
    POUND_CHECK(false == memory_bucket_is_valid((memory_bucket_type_t)-1));
    POUND_CHECK(false == memory_bucket_is_valid((memory_bucket_type_t)9999));
}

POUND_TEST(memory, distinct_buckets_are_not_aliased)
{
    // Regression: the original implementation masked the bucket with
    // `bucket & MEMORY_BUCKET_COUNT`, which collapsed GUEST_MEMORY (2) onto
    // NONE (0) and JIT_RECOMPILER (3) onto UI (1), so every per-bucket total
    // except NONE/UI/DEBUG was wrong. Setting each bucket must now be
    // observable.
    static const memory_bucket_type_t k_buckets[] = {
        MEMORY_BUCKET_NONE,
        MEMORY_BUCKET_UI,
        MEMORY_BUCKET_GUEST_MEMORY,
        MEMORY_BUCKET_JIT_RECOMPILER,
        MEMORY_BUCKET_DEBUG_PROFILING,
    };

    memory_subsystem_init();

    for (size_t i = 0; i < (sizeof(k_buckets) / sizeof(k_buckets[0])); ++i)
    {
        (void)memory_subsystem_set_bucket(MEMORY_BUCKET_NONE);
        POUND_CHECK_EQ_I64(memory_subsystem_get_bucket(), k_buckets[i]);

        for (size_t j = 0; j < (sizeof(k_buckets) / sizeof(k_buckets[0])); ++j)
        {
            if (i == j)
            {
                POUND_CHECK_EQ_I64(memory_subsystem_get_bucket(), k_buckets[j]);
            }
            else
            {
                POUND_CHECK(memory_subsystem_get_bucket() != k_buckets[j]);
            }
        }
    }

    (void)memory_subsystem_set_bucket(MEMORY_BUCKET_NONE);
}

POUND_TEST(memory, set_bucket_returns_the_previous_bucket)
{
    memory_subsystem_init();
    (void)memory_subsystem_set_bucket(MEMORY_BUCKET_NONE);

    POUND_CHECK_EQ_I64(memory_subsystem_set_bucket(MEMORY_BUCKET_GUEST_MEMORY),
                       MEMORY_BUCKET_NONE);
    POUND_CHECK_EQ_I64(memory_subsystem_set_bucket(MEMORY_BUCKET_UI),
                       MEMORY_BUCKET_GUEST_MEMORY);
    POUND_CHECK_EQ_I64(memory_subsystem_get_bucket(), MEMORY_BUCKET_UI);

    (void)memory_subsystem_set_bucket(MEMORY_BUCKET_NONE);
}

POUND_TEST(memory, out_of_range_buckets_are_clamped_not_masked)
{
    memory_subsystem_init();

    // Above the range: clamp to the last real bucket, never to COUNT itself
    // (which would index one past the end of the accounting array).
    (void)memory_subsystem_set_bucket((memory_bucket_type_t)9999);
    POUND_CHECK_EQ_I64(memory_subsystem_get_bucket(), MEMORY_BUCKET_COUNT - 1);

    // "Below the range" is indistinguishable from "far above the range":
    // `memory_bucket_type_t` declares no negative enumerator, so its underlying
    // type is unsigned and casting -1 yields UINT_MAX rather than a negative
    // value. Clamping to the last real bucket is the only in-bounds outcome, so
    // it is the one asserted here.
    (void)memory_subsystem_set_bucket((memory_bucket_type_t)-1);
    POUND_CHECK_EQ_I64(memory_subsystem_get_bucket(), MEMORY_BUCKET_COUNT - 1);

    // Each clamp is reported rather than applied silently.
    POUND_CHECK_EQ_U64(pound_test_log_count_at(LOG_LEVEL_WARN), 2U);

    (void)memory_subsystem_set_bucket(MEMORY_BUCKET_NONE);
}

POUND_TEST(memory, querying_an_out_of_range_bucket_returns_zero)
{
    memory_subsystem_init();

    pound_test_log_reset();

    // Regression: the original implementation masked the index here too, so a
    // request for GUEST_MEMORY returned the NONE total.
    POUND_CHECK_EQ_U64(memory_subsystem_get_memory_used_by_bucket(MEMORY_BUCKET_COUNT), 0U);
    POUND_CHECK_EQ_U64(memory_subsystem_get_memory_used_by_bucket(9999), 0U);

    // An out-of-range query is a caller error and is reported, not ignored.
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_WARN) >= 2U);

    pound_test_log_reset();
}

POUND_TEST(memory, total_bucket_sentinel_sums_every_bucket)
{
    memory_subsystem_init();
    (void)memory_subsystem_set_bucket(MEMORY_BUCKET_NONE);

    size_t sum = 0;

    for (int bucket = 0; bucket < (int)MEMORY_BUCKET_COUNT; ++bucket)
    {
        sum += memory_subsystem_get_memory_used_by_bucket(bucket);
    }

    POUND_CHECK_EQ_U64(memory_subsystem_get_memory_used_by_bucket(MEMORY_BUCKET_TOTAL), sum);

    // Any negative value is treated as the total.
    POUND_CHECK_EQ_U64(memory_subsystem_get_memory_used_by_bucket(-7), sum);
}

// -----------------------------------------------------------------------------
// Allocation
// -----------------------------------------------------------------------------

POUND_TEST(memory, allocate_rejects_a_zero_sized_request)
{
    memory_subsystem_init();
    (void)memory_subsystem_set_bucket(MEMORY_BUCKET_NONE);

    pound_test_log_reset();
    POUND_CHECK_PTR_NULL(memory_subsystem_allocate(16U, 0U));
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);
    pound_test_log_reset();
}

POUND_TEST(memory, allocate_rejects_non_power_of_two_alignment)
{
    memory_subsystem_init();
    (void)memory_subsystem_set_bucket(MEMORY_BUCKET_NONE);

    // 0 is rejected as well: it is "not a non-zero power of two".
    static const size_t k_bad_alignments[] = { 0U, 3U, 5U, 6U, 7U, 12U, 24U, 100U };

    pound_test_log_reset();

    for (size_t i = 0; i < (sizeof(k_bad_alignments) / sizeof(k_bad_alignments[0])); ++i)
    {
        POUND_CHECK_PTR_NULL(memory_subsystem_allocate(k_bad_alignments[i], 64U));
    }

    POUND_CHECK_EQ_U64(pound_test_log_count_at(LOG_LEVEL_ERROR),
                       (unsigned)(sizeof(k_bad_alignments) / sizeof(k_bad_alignments[0])));

    pound_test_log_reset();
}

POUND_TEST(memory, allocate_honours_power_of_two_alignment)
{
    memory_subsystem_init();
    (void)memory_subsystem_set_bucket(MEMORY_BUCKET_NONE);

    static const size_t k_alignments[] = { 1U, 2U, 4U, 8U, 16U, 32U, 64U, 256U, 4096U };

    for (size_t i = 0; i < (sizeof(k_alignments) / sizeof(k_alignments[0])); ++i)
    {
        void *const pointer = memory_subsystem_allocate(k_alignments[i], 1024U);

        POUND_REQUIRE(NULL != pointer);
        POUND_CHECK_EQ_U64((uintptr_t)pointer % k_alignments[i], 0U);
        POUND_CHECK(memory_subsystem_get_usable_size(pointer) >= 1024U);

        memory_subsystem_free(pointer);
    }
}

POUND_TEST(memory, allocation_accounting_round_trips)
{
    memory_subsystem_init();

    const memory_bucket_type_t bucket = MEMORY_BUCKET_GUEST_MEMORY;
    (void)memory_subsystem_set_bucket(bucket);

    const size_t before = memory_subsystem_get_memory_used_by_bucket(bucket);
    void        *const pointer = memory_subsystem_allocate(64U, 64U * 1024U);

    POUND_REQUIRE(NULL != pointer);

    const size_t during = memory_subsystem_get_memory_used_by_bucket(bucket);

    // mimalloc reports a usable size that may exceed the request, so the bucket
    // must grow by *at least* what was asked for.
    POUND_CHECK(during >= before + (64U * 1024U));

    memory_subsystem_free(pointer);

    POUND_CHECK_EQ_U64(memory_subsystem_get_memory_used_by_bucket(bucket), before);

    (void)memory_subsystem_set_bucket(MEMORY_BUCKET_NONE);
}

POUND_TEST(memory, allocations_are_charged_to_the_active_bucket)
{
    memory_subsystem_init();
    (void)memory_subsystem_set_bucket(MEMORY_BUCKET_NONE);

    void *ui  = NULL;
    void *jit = NULL;

    (void)memory_subsystem_set_bucket(MEMORY_BUCKET_UI);
    ui = memory_subsystem_allocate(16U, 32U * 1024U);
    POUND_REQUIRE(NULL != ui);

    (void)memory_subsystem_set_bucket(MEMORY_BUCKET_JIT_RECOMPILER);
    jit = memory_subsystem_allocate(64U, 32U * 1024U);
    POUND_REQUIRE(NULL != jit);

    // Both buckets must report growth, which is precisely what the old
    // `bucket & MEMORY_BUCKET_COUNT` mask destroyed.
    POUND_CHECK(memory_subsystem_get_memory_used_by_bucket(MEMORY_BUCKET_UI) >= 32U * 1024U);
    POUND_CHECK(memory_subsystem_get_memory_used_by_bucket(MEMORY_BUCKET_JIT_RECOMPILER)
                >= 32U * 1024U);

    memory_subsystem_free(jit);
    memory_subsystem_free(ui);
    (void)memory_subsystem_set_bucket(MEMORY_BUCKET_NONE);
}

POUND_TEST(memory, free_of_null_is_a_no_op)
{
    memory_subsystem_init();
    (void)memory_subsystem_set_bucket(MEMORY_BUCKET_NONE);

    const size_t before = memory_subsystem_get_memory_used_by_bucket(MEMORY_BUCKET_TOTAL);

    memory_subsystem_free(NULL);

    POUND_CHECK_EQ_U64(memory_subsystem_get_memory_used_by_bucket(MEMORY_BUCKET_TOTAL), before);
    POUND_CHECK_EQ_U64(pound_test_log_count_at(LOG_LEVEL_ERROR), 0U);
}

POUND_TEST(memory, usable_size_of_null_is_zero)
{
    memory_subsystem_init();

    POUND_CHECK_EQ_U64(memory_subsystem_get_usable_size(NULL), 0U);
}

POUND_TEST(memory, heap_size_is_reported)
{
    memory_subsystem_init();
    (void)memory_subsystem_set_bucket(MEMORY_BUCKET_NONE);

    void *const pointer = memory_subsystem_allocate(16U, 1024U);
    POUND_REQUIRE(NULL != pointer);

    // mimalloc reserves lazily, so only assert the heap is addressable at all.
    POUND_CHECK(memory_subsystem_get_heap_size() > 0U);

    memory_subsystem_free(pointer);
}

// -----------------------------------------------------------------------------
// Cross-bucket release
// -----------------------------------------------------------------------------

POUND_TEST(memory, cross_bucket_free_saturates_instead_of_underflowing)
{
    // Documented limitation: a block is credited back to whichever bucket is
    // active at free time, not the one it was charged to. That skews the
    // breakdown, so the debit saturates at zero -- it must never wrap to a
    // value near SIZE_MAX, which the GUI tracker would render as an absurd
    // figure.
    memory_subsystem_init();
    (void)memory_subsystem_set_bucket(MEMORY_BUCKET_JIT_RECOMPILER);

    void *const pointer = memory_subsystem_allocate(16U, 128U * 1024U);
    POUND_REQUIRE(NULL != pointer);

    POUND_CHECK(memory_subsystem_get_memory_used_by_bucket(MEMORY_BUCKET_JIT_RECOMPILER)
                >= 128U * 1024U);

    // Release while a bucket with nothing charged to it is active.
    pound_test_log_reset();
    (void)memory_subsystem_set_bucket(MEMORY_BUCKET_DEBUG_PROFILING);
    memory_subsystem_free(pointer);

    const size_t debited = memory_subsystem_get_memory_used_by_bucket(MEMORY_BUCKET_DEBUG_PROFILING);

    POUND_CHECK_EQ_U64(debited, 0U);
    POUND_CHECK(debited < (size_t)1024U * 1024U * 1024U);

    // The underflow was surfaced rather than silently absorbed.
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_WARN) >= 1U);
    pound_test_log_reset();

    (void)memory_subsystem_set_bucket(MEMORY_BUCKET_NONE);
}

// -----------------------------------------------------------------------------
// Allocator binding and teardown
// -----------------------------------------------------------------------------

POUND_TEST(memory, allocator_can_be_swapped_and_restored)
{
    memory_subsystem_init();

    memory_allocator_t *const host = memory_subsystem_get_allocator();
    POUND_CHECK_PTR_NON_NULL(host);
    POUND_CHECK_PTR_EQ(host, &g_host_allocator);

    memory_subsystem_init();
    POUND_CHECK_PTR_EQ(memory_subsystem_get_allocator(), &g_host_allocator);

    // Detaching the thread must make allocation fail rather than crash.
    memory_subsystem_set_allocator(NULL);
    POUND_CHECK_PTR_NULL(memory_subsystem_get_allocator());
    POUND_CHECK_PTR_NULL(memory_subsystem_allocate(16U, 16U));

    const memory_allocator_t *const previous = memory_subsystem_set_allocator(host);
    POUND_CHECK_PTR_NULL(previous);
    POUND_CHECK_PTR_EQ(memory_subsystem_get_allocator(), &g_host_allocator);

    pound_test_log_reset();
}

POUND_TEST(memory, subsystem_is_safe_to_use_after_destroy)
{
    memory_subsystem_init();
    (void)memory_subsystem_set_bucket(MEMORY_BUCKET_NONE);

    void *const pointer = memory_subsystem_allocate(16U, 128U);
    POUND_REQUIRE(NULL != pointer);

    memory_subsystem_destroy();

    // Every entry point must refuse politely rather than dereference NULL.
    POUND_CHECK_PTR_NULL(memory_subsystem_get_allocator());
    POUND_CHECK_PTR_NULL(memory_subsystem_allocate(16U, 128U));
    POUND_CHECK_EQ_U64(memory_subsystem_get_memory_used_by_bucket(MEMORY_BUCKET_TOTAL), 0U);
    POUND_CHECK_EQ_U64(memory_subsystem_get_heap_size(), 0U);
    POUND_CHECK_EQ_U64(memory_subsystem_get_usable_size(pointer), 0U);

    // Rebind so the outstanding block can actually be released.
    memory_subsystem_init();
    memory_subsystem_free(pointer);

    (void)memory_subsystem_set_bucket(MEMORY_BUCKET_NONE);
    pound_test_log_reset();
}

POUND_TEST(memory, init_resets_the_bucket)
{
    memory_subsystem_init();
    (void)memory_subsystem_set_bucket(MEMORY_BUCKET_UI);

    memory_subsystem_init();
    POUND_CHECK_EQ_I64(memory_subsystem_get_bucket(), MEMORY_BUCKET_NONE);

    (void)memory_subsystem_set_bucket(MEMORY_BUCKET_NONE);
}

POUND_TEST_SUITE(memory,
    POUND_TEST_CASE(memory, every_named_bucket_is_valid),
    POUND_TEST_CASE(memory, distinct_buckets_are_not_aliased),
    POUND_TEST_CASE(memory, set_bucket_returns_the_previous_bucket),
    POUND_TEST_CASE(memory, out_of_range_buckets_are_clamped_not_masked),
    POUND_TEST_CASE(memory, querying_an_out_of_range_bucket_returns_zero),
    POUND_TEST_CASE(memory, total_bucket_sentinel_sums_every_bucket),
    POUND_TEST_CASE(memory, allocate_rejects_a_zero_sized_request),
    POUND_TEST_CASE(memory, allocate_rejects_non_power_of_two_alignment),
    POUND_TEST_CASE(memory, allocate_honours_power_of_two_alignment),
    POUND_TEST_CASE(memory, allocation_accounting_round_trips),
    POUND_TEST_CASE(memory, allocations_are_charged_to_the_active_bucket),
    POUND_TEST_CASE(memory, free_of_null_is_a_no_op),
    POUND_TEST_CASE(memory, usable_size_of_null_is_zero),
    POUND_TEST_CASE(memory, heap_size_is_reported),
    POUND_TEST_CASE(memory, cross_bucket_free_saturates_instead_of_underflowing),
    POUND_TEST_CASE(memory, allocator_can_be_swapped_and_restored),
    POUND_TEST_CASE(memory, subsystem_is_safe_to_use_after_destroy),
    POUND_TEST_CASE(memory, init_resets_the_bucket))
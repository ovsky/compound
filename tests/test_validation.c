//! Unit tests for `validation.h`.
//!
//! These checks are the arithmetic guard every container parser and every
//! allocator relies on, so the cases concentrate on the boundaries a malformed
//! file or a hostile guest will aim at: an addition that wraps to zero, a
//! multiplication that looks small after truncation, an alignment that would
//! round past the end of the address space, and a range whose end is computed
//! from an attacker-controlled length.

#include "pound_test.h"

#include "validation.h"
#include <stdint.h>

POUND_TEST(validation, add_size_accepts_ordinary_values)
{
    size_t out = 0;

    POUND_CHECK_EQ_I64(validation_add_size(2U, 3U, &out), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(out, 5U);
}

POUND_TEST(validation, add_size_rejects_wrap)
{
    size_t out = 1234U;

    POUND_CHECK_EQ_I64(validation_add_size(SIZE_MAX, 1U, &out),
                       POUND_ERROR_GUEST_ADDRESS_OVERFLOW);
    POUND_CHECK_EQ_U64(out, 0U);
}

POUND_TEST(validation, add_size_rejects_null_output)
{
    POUND_CHECK_EQ_I64(validation_add_size(1U, 2U, NULL), POUND_ERROR_INVALID_ARGUMENT);
}

POUND_TEST(validation, mul_size_accepts_ordinary_values)
{
    size_t out = 0;

    POUND_CHECK_EQ_I64(validation_mul_size(7U, 6U, &out), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(out, 42U);
}

POUND_TEST(validation, mul_size_rejects_wrap)
{
    size_t out = 1234U;

    POUND_CHECK_EQ_I64(validation_mul_size(SIZE_MAX, 2U, &out),
                       POUND_ERROR_GUEST_ADDRESS_OVERFLOW);
    POUND_CHECK_EQ_U64(out, 0U);
}

POUND_TEST(validation, mul_size_rejects_null_output)
{
    POUND_CHECK_EQ_I64(validation_mul_size(1U, 2U, NULL), POUND_ERROR_INVALID_ARGUMENT);
}

POUND_TEST(validation, align_up_rounds_and_preserves_already_aligned_values)
{
    size_t out = 0;

    POUND_CHECK_EQ_I64(validation_align_up_size(0U, 16U, &out), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(out, 0U);

    POUND_CHECK_EQ_I64(validation_align_up_size(1U, 16U, &out), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(out, 16U);

    POUND_CHECK_EQ_I64(validation_align_up_size(16U, 16U, &out), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(out, 16U);

    POUND_CHECK_EQ_I64(validation_align_up_size(17U, 16U, &out), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(out, 32U);
}

POUND_TEST(validation, align_up_rejects_invalid_alignment)
{
    size_t out = 0;

    POUND_CHECK_EQ_I64(validation_align_up_size(1U, 0U, &out), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(validation_align_up_size(1U, 3U, &out), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(validation_align_up_size(1U, 16U, NULL), POUND_ERROR_INVALID_ARGUMENT);
}

POUND_TEST(validation, align_up_rejects_wrap_at_the_top_of_size_t)
{
    size_t out = 0;

    POUND_CHECK_EQ_I64(validation_align_up_size(SIZE_MAX, 16U, &out),
                       POUND_ERROR_GUEST_ADDRESS_OVERFLOW);
    POUND_CHECK_EQ_U64(out, 0U);
}

POUND_TEST(validation, power_of_two_recognises_only_non_zero_powers)
{
    POUND_CHECK(validation_is_power_of_two(1U));
    POUND_CHECK(validation_is_power_of_two(2U));
    POUND_CHECK(validation_is_power_of_two(4096U));
    POUND_CHECK(false == validation_is_power_of_two(0U));
    POUND_CHECK(false == validation_is_power_of_two(3U));
    POUND_CHECK(false == validation_is_power_of_two(SIZE_MAX));
}

POUND_TEST(validation, range_size_accepts_ranges_that_end_exactly_at_total)
{
    POUND_CHECK_EQ_I64(validation_range_size(0U, 16U, 16U), POUND_SUCCESS);
    POUND_CHECK_EQ_I64(validation_range_size(16U, 0U, 16U), POUND_SUCCESS);
    POUND_CHECK_EQ_I64(validation_range_size(15U, 1U, 16U), POUND_SUCCESS);
}

POUND_TEST(validation, range_size_rejects_ranges_past_total_without_wrapping)
{
    POUND_CHECK_EQ_I64(validation_range_size(17U, 0U, 16U),
                       POUND_ERROR_GUEST_ADDRESS_OUT_OF_BOUNDS);
    POUND_CHECK_EQ_I64(validation_range_size(1U, SIZE_MAX, 16U),
                       POUND_ERROR_GUEST_ADDRESS_OUT_OF_BOUNDS);
    POUND_CHECK_EQ_I64(validation_range_size(0U, SIZE_MAX, 0U),
                       POUND_ERROR_GUEST_ADDRESS_OUT_OF_BOUNDS);
}

POUND_TEST(validation, range_u64_reports_the_end_and_rejects_overflow)
{
    uint64_t end = 0;

    POUND_CHECK_EQ_I64(validation_range_u64(0x1000U, 0x2000U, 0x3000U, &end), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(end, 0x3000U);

    POUND_CHECK_EQ_I64(validation_range_u64(0x1000U, 0x2001U, 0x3000U, &end),
                       POUND_ERROR_GUEST_ADDRESS_OUT_OF_BOUNDS);

    POUND_CHECK_EQ_I64(validation_range_u64(UINT64_MAX, 1U, UINT64_MAX, &end),
                       POUND_ERROR_GUEST_ADDRESS_OVERFLOW);

    POUND_CHECK_EQ_I64(validation_range_u64(0U, 1U, 1U, NULL), POUND_ERROR_INVALID_ARGUMENT);
}

POUND_TEST(validation, range_contains_handles_edges_and_wrapping_ranges)
{
    POUND_CHECK(validation_range_contains(0x1000U, 0x1000U, 0x1000U, 0x1000U));
    POUND_CHECK(validation_range_contains(0x1000U, 0x1000U, 0x1800U, 0x800U));
    POUND_CHECK(false == validation_range_contains(0x1000U, 0x1000U, 0x0FFFU, 1U));
    POUND_CHECK(false == validation_range_contains(0x1000U, 0x1000U, 0x1FFFU, 2U));

    // A range that wraps the 64-bit address space is never contained, even
    // though its start address looks valid.
    POUND_CHECK(false == validation_range_contains(0U, UINT64_MAX, UINT64_MAX, 2U));
}

POUND_TEST_SUITE(validation,
                 POUND_TEST_CASE(validation, add_size_accepts_ordinary_values),
                 POUND_TEST_CASE(validation, add_size_rejects_wrap),
                 POUND_TEST_CASE(validation, add_size_rejects_null_output),
                 POUND_TEST_CASE(validation, mul_size_accepts_ordinary_values),
                 POUND_TEST_CASE(validation, mul_size_rejects_wrap),
                 POUND_TEST_CASE(validation, mul_size_rejects_null_output),
                 POUND_TEST_CASE(validation, align_up_rounds_and_preserves_already_aligned_values),
                 POUND_TEST_CASE(validation, align_up_rejects_invalid_alignment),
                 POUND_TEST_CASE(validation, align_up_rejects_wrap_at_the_top_of_size_t),
                 POUND_TEST_CASE(validation, power_of_two_recognises_only_non_zero_powers),
                 POUND_TEST_CASE(validation,
                                 range_size_accepts_ranges_that_end_exactly_at_total),
                 POUND_TEST_CASE(validation,
                                 range_size_rejects_ranges_past_total_without_wrapping),
                 POUND_TEST_CASE(validation, range_u64_reports_the_end_and_rejects_overflow),
                 POUND_TEST_CASE(validation, range_contains_handles_edges_and_wrapping_ranges));

/*** end of file ***/

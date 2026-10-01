//! Unit tests for `safe_math.h`.
//!
//! These wrap the compiler's checked-arithmetic builtins. The interesting
//! cases are the overflow directions and the NULL `result` contract, all of
//! which the Ballistic recompiler will hit constantly.

#include "pound_test.h"

#include "safe_math.h"

// -----------------------------------------------------------------------------
// Error strings
// -----------------------------------------------------------------------------

POUND_TEST(safe_math, error_string_covers_every_value)
{
    POUND_CHECK_STR_EQ(safe_math_error_string(POUND_MATH_SUCCESS),
                       "a math operation succeeded");
    POUND_CHECK_STR_EQ(safe_math_error_string(POUND_MATH_ERROR_INVALID_ARGUMENT),
                       "a math function argument was invalid");
    POUND_CHECK_STR_EQ(safe_math_error_string(POUND_MATH_ERROR_SIGNED_POSITIVE_OVERFLOW),
                       "a math operation caused a signed positive overflow");
    POUND_CHECK_STR_EQ(safe_math_error_string(POUND_MATH_ERROR_SIGNED_NEGATIVE_OVERFLOW),
                       "a math operation caused a signed negative overflow");
    POUND_CHECK_STR_EQ(safe_math_error_string(POUND_MATH_ERROR_UNSIGNED_OVERFLOW),
                       "a math operation caused a unsigned overflow");
}

POUND_TEST(safe_math, error_string_falls_back_for_unknown_value)
{
    POUND_CHECK_STR_EQ(safe_math_error_string((safe_math_error_t)9999), "UNKNOWN ERROR");
    POUND_CHECK_STR_EQ(safe_math_error_string((safe_math_error_t)-1), "UNKNOWN ERROR");
}

// -----------------------------------------------------------------------------
// Unsigned addition
// -----------------------------------------------------------------------------

POUND_TEST(safe_math, add_u8_happy_path)
{
    uint8_t result = 0xFF;

    POUND_CHECK_EQ_I64(safe_math_add_u8(1U, 2U, &result), POUND_MATH_SUCCESS);
    POUND_CHECK_EQ_U64(result, 3U);
}

POUND_TEST(safe_math, add_u8_boundary_is_exact)
{
    uint8_t result = 0;

    POUND_CHECK_EQ_I64(safe_math_add_u8(UINT8_MAX, 0U, &result), POUND_MATH_SUCCESS);
    POUND_CHECK_EQ_U64(result, 255U);

    POUND_CHECK_EQ_I64(safe_math_add_u8(0U, UINT8_MAX, &result), POUND_MATH_SUCCESS);
    POUND_CHECK_EQ_U64(result, 255U);
}

POUND_TEST(safe_math, add_u8_detects_unsigned_overflow)
{
    uint8_t result = 0;

    POUND_CHECK_EQ_I64(safe_math_add_u8(UINT8_MAX, 1U, &result),
                       POUND_MATH_ERROR_UNSIGNED_OVERFLOW);

    // The implementation is contractually required to zero the result on
    // failure so a caller that ignores the status cannot use stale data.
    POUND_CHECK_EQ_U64(result, 0U);
}

POUND_TEST(safe_math, add_u16_detects_unsigned_overflow)
{
    uint16_t result = 0;

    // UINT16_MAX is representable, so only adding nothing keeps it in range.
    POUND_CHECK_EQ_I64(safe_math_add_u16(UINT16_MAX, 0U, &result), POUND_MATH_SUCCESS);
    POUND_CHECK_EQ_U64(result, UINT16_MAX);

    POUND_CHECK_EQ_I64(safe_math_add_u16(UINT16_MAX, 2U, &result),
                       POUND_MATH_ERROR_UNSIGNED_OVERFLOW);
    POUND_CHECK_EQ_U64(result, 0U);
}

POUND_TEST(safe_math, add_u32_detects_unsigned_overflow)
{
    uint32_t result = 0;

    POUND_CHECK_EQ_I64(safe_math_add_u32(0xFFFFFFFFU, 0U, &result), POUND_MATH_SUCCESS);
    POUND_CHECK_EQ_U64(result, 0xFFFFFFFFU);

    POUND_CHECK_EQ_I64(safe_math_add_u32(0xFFFFFFFFU, 1U, &result),
                       POUND_MATH_ERROR_UNSIGNED_OVERFLOW);
    POUND_CHECK_EQ_U64(result, 0U);
}

POUND_TEST(safe_math, add_u64_detects_unsigned_overflow)
{
    uint64_t result = 0;

    POUND_CHECK_EQ_I64(safe_math_add_u64(UINT64_MAX, 0U, &result), POUND_MATH_SUCCESS);
    POUND_CHECK_EQ_U64(result, UINT64_MAX);

    POUND_CHECK_EQ_I64(safe_math_add_u64(UINT64_MAX, 1U, &result),
                       POUND_MATH_ERROR_UNSIGNED_OVERFLOW);
    POUND_CHECK_EQ_U64(result, 0U);

    // Also exercise the wrapped case that a naive `a + b` would miss.
    result = 0;
    POUND_CHECK_EQ_I64(safe_math_add_u64(UINT64_C(0x8000000000000000),
                                         UINT64_C(0x8000000000000000),
                                         &result),
                       POUND_MATH_ERROR_UNSIGNED_OVERFLOW);
    POUND_CHECK_EQ_U64(result, 0U);
}

// -----------------------------------------------------------------------------
// Signed addition
// -----------------------------------------------------------------------------

POUND_TEST(safe_math, add_i8_happy_path)
{
    int8_t result = 0;

    POUND_CHECK_EQ_I64(safe_math_add_i8(-5, 3, &result), POUND_MATH_SUCCESS);
    POUND_CHECK_EQ_I64(result, -2);

    POUND_CHECK_EQ_I64(safe_math_add_i8(INT8_MAX, -1, &result), POUND_MATH_SUCCESS);
    POUND_CHECK_EQ_I64(result, INT8_MAX - 1);
}

POUND_TEST(safe_math, add_i8_distinguishes_overflow_direction)
{
    int8_t result = 0;

    POUND_CHECK_EQ_I64(safe_math_add_i8(INT8_MAX, 1, &result),
                       POUND_MATH_ERROR_SIGNED_POSITIVE_OVERFLOW);
    POUND_CHECK_EQ_I64(result, 0);

    POUND_CHECK_EQ_I64(safe_math_add_i8(INT8_MIN, -1, &result),
                       POUND_MATH_ERROR_SIGNED_NEGATIVE_OVERFLOW);
    POUND_CHECK_EQ_I64(result, 0);
}

POUND_TEST(safe_math, add_i16_distinguishes_overflow_direction)
{
    int16_t result = 0;

    POUND_CHECK_EQ_I64(safe_math_add_i16(INT16_MAX, 1, &result),
                       POUND_MATH_ERROR_SIGNED_POSITIVE_OVERFLOW);
    POUND_CHECK_EQ_I64(safe_math_add_i16(INT16_MIN, -1, &result),
                       POUND_MATH_ERROR_SIGNED_NEGATIVE_OVERFLOW);
    POUND_CHECK_EQ_I64(safe_math_add_i16(INT16_MIN, 1, &result), POUND_MATH_SUCCESS);
    POUND_CHECK_EQ_I64(result, INT16_MIN + 1);
}

POUND_TEST(safe_math, add_i32_distinguishes_overflow_direction)
{
    int32_t result = 0;

    POUND_CHECK_EQ_I64(safe_math_add_i32(INT32_MAX, 1, &result),
                       POUND_MATH_ERROR_SIGNED_POSITIVE_OVERFLOW);
    POUND_CHECK_EQ_I64(safe_math_add_i32(INT32_MIN, -1, &result),
                       POUND_MATH_ERROR_SIGNED_NEGATIVE_OVERFLOW);
    POUND_CHECK_EQ_I64(safe_math_add_i32(0x7FFFFFFF, -1, &result), POUND_MATH_SUCCESS);
    POUND_CHECK_EQ_I64(result, 0x7FFFFFFE);
}

POUND_TEST(safe_math, add_i64_distinguishes_overflow_direction)
{
    int64_t result = 0;

    POUND_CHECK_EQ_I64(safe_math_add_i64(INT64_MAX, 1, &result),
                       POUND_MATH_ERROR_SIGNED_POSITIVE_OVERFLOW);
    POUND_CHECK_EQ_I64(result, 0);

    POUND_CHECK_EQ_I64(safe_math_add_i64(INT64_MIN, -1, &result),
                       POUND_MATH_ERROR_SIGNED_NEGATIVE_OVERFLOW);
    POUND_CHECK_EQ_I64(result, 0);

    POUND_CHECK_EQ_I64(safe_math_add_i64(INT64_MIN, 1, &result), POUND_MATH_SUCCESS);
    POUND_CHECK_EQ_I64(result, INT64_MIN + 1);
}

// -----------------------------------------------------------------------------
// NULL result contract
// -----------------------------------------------------------------------------

POUND_TEST(safe_math, every_entry_point_rejects_a_null_result)
{
    // Each call must return INVALID_ARGUMENT rather than dereferencing, and must
    // log the refusal rather than failing silently.
    pound_test_log_reset();

    POUND_CHECK_EQ_I64(safe_math_add_u8(1U, 1U, NULL), POUND_MATH_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(safe_math_add_u16(1U, 1U, NULL), POUND_MATH_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(safe_math_add_u32(1U, 1U, NULL), POUND_MATH_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(safe_math_add_u64(1U, 1U, NULL), POUND_MATH_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(safe_math_add_i8(1, 1, NULL), POUND_MATH_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(safe_math_add_i16(1, 1, NULL), POUND_MATH_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(safe_math_add_i32(1, 1, NULL), POUND_MATH_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(safe_math_add_i64(1, 1, NULL), POUND_MATH_ERROR_INVALID_ARGUMENT);

    POUND_CHECK_EQ_U64(pound_test_log_count_at(LOG_LEVEL_ERROR), 8U);

    pound_test_log_reset();
}

POUND_TEST_SUITE(safe_math,
    POUND_TEST_CASE(safe_math, error_string_covers_every_value),
    POUND_TEST_CASE(safe_math, error_string_falls_back_for_unknown_value),
    POUND_TEST_CASE(safe_math, add_u8_happy_path),
    POUND_TEST_CASE(safe_math, add_u8_boundary_is_exact),
    POUND_TEST_CASE(safe_math, add_u8_detects_unsigned_overflow),
    POUND_TEST_CASE(safe_math, add_u16_detects_unsigned_overflow),
    POUND_TEST_CASE(safe_math, add_u32_detects_unsigned_overflow),
    POUND_TEST_CASE(safe_math, add_u64_detects_unsigned_overflow),
    POUND_TEST_CASE(safe_math, add_i8_happy_path),
    POUND_TEST_CASE(safe_math, add_i8_distinguishes_overflow_direction),
    POUND_TEST_CASE(safe_math, add_i16_distinguishes_overflow_direction),
    POUND_TEST_CASE(safe_math, add_i32_distinguishes_overflow_direction),
    POUND_TEST_CASE(safe_math, add_i64_distinguishes_overflow_direction),
    POUND_TEST_CASE(safe_math, every_entry_point_rejects_a_null_result))
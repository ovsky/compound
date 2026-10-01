#include "safe_math.h"
#include "log.h"
#include <stddef.h>

const char *
safe_math_error_string(const safe_math_error_t error)
{
    switch (error)
    {
        case POUND_MATH_SUCCESS:
            return "a math operation succeeded";
        case POUND_MATH_ERROR_INVALID_ARGUMENT:
            return "a math function argument was invalid";
        case POUND_MATH_ERROR_SIGNED_POSITIVE_OVERFLOW:
            return "a math operation caused a signed positive overflow";
        case POUND_MATH_ERROR_SIGNED_NEGATIVE_OVERFLOW:
            return "a math operation caused a signed negative overflow";
        case POUND_MATH_ERROR_UNSIGNED_OVERFLOW:
            return "a math operation caused a unsigned overflow";
        default:
            return "UNKNOWN ERROR";
    }
}

safe_math_error_t
safe_math_add_u8(const uint8_t a, const uint8_t b, uint8_t *result)
{
    if (POUND_UNLIKELY(NULL == result))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: result pointer is NULL.");
        return POUND_MATH_ERROR_INVALID_ARGUMENT;
    }

    uint8_t out = 0;

    if (POUND_UNLIKELY(__builtin_add_overflow(a, b, &out)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "%u + %u exceeds UINT8_MAX.",
                        (unsigned int)a,
                        (unsigned int)b);
        *result = 0;
        return POUND_MATH_ERROR_UNSIGNED_OVERFLOW;
    }

    *result = out;
    return POUND_MATH_SUCCESS;
}

safe_math_error_t
safe_math_add_u16(const uint16_t a, const uint16_t b, uint16_t *result)
{
    if (POUND_UNLIKELY(NULL == result))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: result pointer is NULL.");
        return POUND_MATH_ERROR_INVALID_ARGUMENT;
    }

    uint16_t out = 0;

    if (POUND_UNLIKELY(__builtin_add_overflow(a, b, &out)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "%u + %u exceeds UINT16_MAX.",
                        (unsigned int)a,
                        (unsigned int)b);
        *result = 0;
        return POUND_MATH_ERROR_UNSIGNED_OVERFLOW;
    }

    *result = out;
    return POUND_MATH_SUCCESS;
}

safe_math_error_t
safe_math_add_u32(const uint32_t a, const uint32_t b, uint32_t *result)
{
    if (POUND_UNLIKELY(NULL == result))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: result pointer is NULL.");
        return POUND_MATH_ERROR_INVALID_ARGUMENT;
    }

    uint32_t out = 0;

    if (POUND_UNLIKELY(__builtin_add_overflow(a, b, &out)))
    {
        POUND_LOG_ERROR(&thread_logger, "%u + %u exceeds UINT32_MAX.", a, b);
        *result = 0;
        return POUND_MATH_ERROR_UNSIGNED_OVERFLOW;
    }

    *result = out;
    return POUND_MATH_SUCCESS;
}

safe_math_error_t
safe_math_add_u64(const uint64_t a, const uint64_t b, uint64_t *result)
{
    if (POUND_UNLIKELY(NULL == result))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: result pointer is NULL.");
        return POUND_MATH_ERROR_INVALID_ARGUMENT;
    }

    uint64_t out = 0;

    if (POUND_UNLIKELY(__builtin_add_overflow(a, b, &out)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "%llu + %llu exceeds UINT64_MAX.",
                        (unsigned long long)a,
                        (unsigned long long)b);
        *result = 0;
        return POUND_MATH_ERROR_UNSIGNED_OVERFLOW;
    }

    *result = out;
    return POUND_MATH_SUCCESS;
}

safe_math_error_t
safe_math_add_i8(const int8_t a, const int8_t b, int8_t *result)
{
    if (POUND_UNLIKELY(NULL == result))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: result pointer is NULL.");
        return POUND_MATH_ERROR_INVALID_ARGUMENT;
    }

    int8_t out = 0;

    if (POUND_UNLIKELY(__builtin_add_overflow(a, b, &out)))
    {
        if ((a > 0 && b > 0))
        {
            POUND_LOG_ERROR(&thread_logger, "%d + %d exceeds INT8_MAX.", a, b);
            *result = 0;
            return POUND_MATH_ERROR_SIGNED_POSITIVE_OVERFLOW;
        }

        POUND_LOG_ERROR(&thread_logger, "%d + %d is below INT8_MIN.", a, b);
        *result = 0;
        return POUND_MATH_ERROR_SIGNED_NEGATIVE_OVERFLOW;
    }

    *result = out;
    return POUND_MATH_SUCCESS;
}

safe_math_error_t
safe_math_add_i16(const int16_t a, const int16_t b, int16_t *result)
{
    if (POUND_UNLIKELY(NULL == result))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: result pointer is NULL.");
        return POUND_MATH_ERROR_INVALID_ARGUMENT;
    }

    int16_t out = 0;

    if (POUND_UNLIKELY(__builtin_add_overflow(a, b, &out)))
    {
        if ((a > 0 && b > 0))
        {
            POUND_LOG_ERROR(&thread_logger, "%d + %d exceeds INT16_MAX.", a, b);
            *result = 0;
            return POUND_MATH_ERROR_SIGNED_POSITIVE_OVERFLOW;
        }

        POUND_LOG_ERROR(&thread_logger, "%d + %d is below INT16_MIN.", a, b);
        *result = 0;
        return POUND_MATH_ERROR_SIGNED_NEGATIVE_OVERFLOW;
    }

    *result = out;
    return POUND_MATH_SUCCESS;
}

safe_math_error_t
safe_math_add_i32(const int32_t a, const int32_t b, int32_t *result)
{
    if (POUND_UNLIKELY(NULL == result))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: result pointer is NULL.");
        return POUND_MATH_ERROR_INVALID_ARGUMENT;
    }

    int32_t out = 0;

    if (POUND_UNLIKELY(__builtin_add_overflow(a, b, &out)))
    {
        if ((a > 0 && b > 0))
        {
            POUND_LOG_ERROR(&thread_logger, "%d + %d exceed INT32_MAX.", a, b);
            *result = 0;
            return POUND_MATH_ERROR_SIGNED_POSITIVE_OVERFLOW;
        }

        POUND_LOG_ERROR(&thread_logger, "%d + %d is below INT32_MIN.", a, b);
        *result = 0;
        return POUND_MATH_ERROR_SIGNED_NEGATIVE_OVERFLOW;
    }

    *result = out;
    return POUND_MATH_SUCCESS;
}

safe_math_error_t
safe_math_add_i64(int64_t a, int64_t b, int64_t *result)
{
    if (POUND_UNLIKELY(NULL == result))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: result pointer is NULL.");
        return POUND_MATH_ERROR_INVALID_ARGUMENT;
    }

    int64_t out = 0;

    if (POUND_UNLIKELY(__builtin_add_overflow(a, b, &out)))
    {
        if ((a > 0 && b > 0))
        {
            POUND_LOG_ERROR(&thread_logger,
                        "%lld + %lld exceed INT64_MAX.",
                        (long long)a,
                        (long long)b);
            *result = 0;
            return POUND_MATH_ERROR_SIGNED_POSITIVE_OVERFLOW;
        }

        POUND_LOG_ERROR(&thread_logger,
                        "%lld + %lld is below INT64_MIN",
                        (long long)a,
                        (long long)b);
        *result = 0;
        return POUND_MATH_ERROR_SIGNED_NEGATIVE_OVERFLOW;
    }

    *result = out;
    return POUND_MATH_SUCCESS;
}

/*** end of file ***/

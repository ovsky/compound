#include "validation.h"

bool
validation_is_power_of_two(const size_t value)
{
    return (value != 0U) && ((value & (value - 1U)) == 0U);
}

error_t
validation_add_size(const size_t a, const size_t b, size_t *POUND_RESTRICT out)
{
    if (NULL == out)
    {
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    size_t result = 0;

    if (__builtin_add_overflow(a, b, &result))
    {
        *out = 0U;
        return POUND_ERROR_GUEST_ADDRESS_OVERFLOW;
    }

    *out = result;
    return POUND_SUCCESS;
}

error_t
validation_mul_size(const size_t a, const size_t b, size_t *POUND_RESTRICT out)
{
    if (NULL == out)
    {
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    size_t result = 0;

    if (__builtin_mul_overflow(a, b, &result))
    {
        *out = 0U;
        return POUND_ERROR_GUEST_ADDRESS_OVERFLOW;
    }

    *out = result;
    return POUND_SUCCESS;
}

error_t
validation_align_up_size(const size_t value, const size_t alignment, size_t *POUND_RESTRICT out)
{
    if ((NULL == out) || (false == validation_is_power_of_two(alignment)))
    {
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const size_t mask    = alignment - 1U;
    const size_t padding = (alignment - (value & mask)) & mask;
    size_t       result  = 0;

    if (__builtin_add_overflow(value, padding, &result))
    {
        *out = 0U;
        return POUND_ERROR_GUEST_ADDRESS_OVERFLOW;
    }

    *out = result;
    return POUND_SUCCESS;
}

error_t
validation_range_size(const size_t offset, const size_t length, const size_t total)
{
    if ((offset > total) || (length > (total - offset)))
    {
        return POUND_ERROR_GUEST_ADDRESS_OUT_OF_BOUNDS;
    }

    return POUND_SUCCESS;
}

error_t
validation_range_u64(const uint64_t base,
                     const uint64_t length,
                     const uint64_t limit,
                     uint64_t *POUND_RESTRICT out_end)
{
    if (NULL == out_end)
    {
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    uint64_t end = 0;

    if (__builtin_add_overflow(base, length, &end))
    {
        *out_end = 0U;
        return POUND_ERROR_GUEST_ADDRESS_OVERFLOW;
    }

    if (end > limit)
    {
        *out_end = end;
        return POUND_ERROR_GUEST_ADDRESS_OUT_OF_BOUNDS;
    }

    *out_end = end;
    return POUND_SUCCESS;
}

bool
validation_range_contains(const uint64_t base,
                          const uint64_t size,
                          const uint64_t address,
                          const uint64_t length)
{
    // `address + length` can wrap while still describing a range that is
    // conceptually inside a window ending at 2^64. Handle that case first so
    // the subtraction below is always on values known not to have wrapped.
    uint64_t address_end = 0U;

    if (__builtin_add_overflow(address, length, &address_end))
    {
        return false;
    }

    const uint64_t base_end = base + size;

    if (base_end < base)
    {
        return false;
    }

    return (address >= base) && (address_end <= base_end);
}

/*** end of file ***/

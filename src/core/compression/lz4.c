//! Raw LZ4 block decompression, with the bounds checks an executable loader needs.

#include "compression/lz4.h"

#include "log.h"
#include <string.h>

/// Adds one LZ4 length extension to `length`, rejecting a value that cannot fit.
///
/// LZ4 encodes a length of 15 or more as 15 followed by 255-bytes and a final
/// byte below 255. Reading past `end` here is the classic truncation bug, so the
/// loop checks the cursor before each byte.
static error_t
read_extended_length(const uint8_t *POUND_RESTRICT input, size_t input_size, size_t *POUND_RESTRICT cursor,
                     size_t *POUND_RESTRICT length)
{
    for (;;)
    {
        if (POUND_UNLIKELY(*cursor >= input_size))
        {
            POUND_LOG_ERROR(&thread_logger, "LZ4 block ends in the middle of a length extension.");
            return POUND_ERROR_CORRUPTED;
        }

        const uint8_t extension = input[(*cursor)++];

        if (POUND_UNLIKELY(*length > (SIZE_MAX - extension)))
        {
            POUND_LOG_ERROR(&thread_logger, "LZ4 length overflows a size_t.");
            return POUND_ERROR_CORRUPTED;
        }

        *length += extension;

        if (255U != extension)
        {
            return POUND_SUCCESS;
        }
    }
}

error_t
pound_lz4_decompress_block(const uint8_t *POUND_RESTRICT input, const size_t input_size,
                           uint8_t *POUND_RESTRICT output, const size_t output_capacity,
                           size_t *POUND_RESTRICT output_size)
{
    if (POUND_UNLIKELY(NULL == input) || POUND_UNLIKELY(NULL == output) || POUND_UNLIKELY(NULL == output_size))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: LZ4 decompression was handed a NULL pointer.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(0U == input_size) || POUND_UNLIKELY(0U == output_capacity))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: LZ4 was given an empty block or destination.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const uintptr_t input_start  = (uintptr_t)input;
    const uintptr_t output_start = (uintptr_t)output;

    if (POUND_UNLIKELY((input_start < (output_start + output_capacity))
                       && (output_start < (input_start + input_size))))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: LZ4 input and output overlap.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    size_t input_cursor  = 0U;
    size_t output_cursor = 0U;

    while (input_cursor < input_size)
    {
        const uint8_t token = input[input_cursor++];

        size_t literal_length = token >> 4U;
        if (15U == literal_length)
        {
            const error_t length_error = read_extended_length(input, input_size, &input_cursor, &literal_length);
            if (POUND_SUCCESS != length_error)
            {
                return length_error;
            }
        }

        if (POUND_UNLIKELY(literal_length > (input_size - input_cursor)))
        {
            POUND_LOG_ERROR(&thread_logger, "LZ4 block claims %zu literal bytes but only %zu remain.",
                            literal_length, input_size - input_cursor);
            return POUND_ERROR_CORRUPTED;
        }

        if (POUND_UNLIKELY(literal_length > (output_capacity - output_cursor)))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "LZ4 literals would write %zu bytes into %zu bytes of remaining output.",
                            literal_length,
                            output_capacity - output_cursor);
            return POUND_ERROR_BUFFER_TOO_SMALL;
        }

        memcpy(output + output_cursor, input + input_cursor, literal_length);
        input_cursor += literal_length;
        output_cursor += literal_length;

        // The last sequence contains only literals. There is no match and no trailer.
        if (input_cursor == input_size)
        {
            break;
        }

        if (POUND_UNLIKELY((input_size - input_cursor) < 2U))
        {
            POUND_LOG_ERROR(&thread_logger, "LZ4 block ends before a match offset.");
            return POUND_ERROR_CORRUPTED;
        }

        const size_t match_offset
            = (size_t)input[input_cursor] | ((size_t)input[input_cursor + 1U] << 8U);
        input_cursor += 2U;

        if (POUND_UNLIKELY(0U == match_offset) || POUND_UNLIKELY(match_offset > output_cursor))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "LZ4 match offset %zu does not point into the %zu bytes already decoded.",
                            match_offset, output_cursor);
            return POUND_ERROR_CORRUPTED;
        }

        size_t match_length = token & 0x0FU;
        if (15U == match_length)
        {
            const error_t length_error = read_extended_length(input, input_size, &input_cursor, &match_length);
            if (POUND_SUCCESS != length_error)
            {
                return length_error;
            }
        }

        if (POUND_UNLIKELY(match_length > (SIZE_MAX - 4U)))
        {
            POUND_LOG_ERROR(&thread_logger, "LZ4 match length overflows a size_t.");
            return POUND_ERROR_CORRUPTED;
        }

        match_length += 4U;

        if (POUND_UNLIKELY(match_length > (output_capacity - output_cursor)))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "LZ4 match would write %zu bytes into %zu bytes of remaining output.",
                            match_length, output_capacity - output_cursor);
            return POUND_ERROR_BUFFER_TOO_SMALL;
        }

        // The source may overlap the destination: LZ4 relies on that for runs, so this
        // is a byte loop rather than memcpy. It is only ever a short run in practice.
        size_t source = output_cursor - match_offset;
        for (size_t i = 0U; i < match_length; ++i)
        {
            output[output_cursor++] = output[source++];
        }
    }

    if (POUND_UNLIKELY(output_cursor != output_capacity))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "LZ4 decoded %zu bytes, but the NSO segment header declares %zu.",
                        output_cursor, output_capacity);
        return POUND_ERROR_CORRUPTED;
    }

    *output_size = output_cursor;
    return POUND_SUCCESS;
}

/*** end of file ***/

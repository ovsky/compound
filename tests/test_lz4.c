//! Tests for the raw LZ4 block decoder used by compressed NSO segments.
//!
//! LZ4 has no checksum and no length field: the destination length comes from the
//! NSO header, and every literal, match offset and match length must be checked
//! against the bytes that actually exist. The error cases below are therefore as
//! important as the happy paths.

#include "pound_test.h"

#include "compression/lz4.h"
#include <string.h>

POUND_TEST(lz4, literal_only_block_is_copied)
{
    static const uint8_t BLOCK[] = { 0x50U, 'h', 'e', 'l', 'l', 'o' };
    uint8_t output[5] = {0};
    size_t  written  = 0U;

    POUND_REQUIRE(POUND_SUCCESS == pound_lz4_decompress_block(BLOCK, sizeof(BLOCK), output, sizeof(output), &written));
    POUND_CHECK_EQ_U64(written, sizeof(output));
    POUND_CHECK(0 == memcmp(output, "hello", 5U));
}

POUND_TEST(lz4, a_long_literal_run_decodes)
{
    uint8_t block[2U + 20U] = {0};
    uint8_t output[20];
    size_t  written = 0U;

    block[0] = 0xF0U; // fifteen literals, then an extension of five
    block[1] = 5U;

    for (size_t i = 0U; i < 20U; ++i)
    {
        block[2U + i] = (uint8_t)('a' + i);
    }

    POUND_REQUIRE(POUND_SUCCESS == pound_lz4_decompress_block(block, sizeof(block), output, sizeof(output), &written));
    POUND_CHECK_EQ_U64(written, sizeof(output));
    POUND_CHECK(0 == memcmp(output, block + 2U, sizeof(output)));
}

POUND_TEST(lz4, an_overlapping_match_repeats_the_last_byte)
{
    // One literal, then a match of 4095 bytes at offset 1. This is the shape a
    // repeated NSO segment produces and it relies on byte-at-a-time copying.
    uint8_t block[20];
    size_t  cursor = 0U;

    block[cursor++] = 0x1FU; // one literal, match length 15 plus extensions
    block[cursor++] = 'A';

    for (uint32_t i = 0U; i < 15U; ++i)
    {
        block[cursor++] = 0xFFU;
    }

    block[cursor++] = 0xFBU; // 0xFFB + 15 + 4 = 0xFFF matched bytes
    block[cursor++] = 0x01U;
    block[cursor++] = 0x00U;

    uint8_t output[4096];
    size_t  written = 0U;

    POUND_REQUIRE(POUND_SUCCESS == pound_lz4_decompress_block(block, cursor, output, sizeof(output), &written));
    POUND_CHECK_EQ_U64(written, sizeof(output));

    for (size_t i = 0U; i < sizeof(output); ++i)
    {
        if ('A' != output[i])
        {
            POUND_CHECK_MSG(false, "byte %zu is 0x%02X, not 'A'", i, output[i]);
            return;
        }
    }
}

POUND_TEST(lz4, malformed_blocks_are_refused)
{
    uint8_t output[8] = {0};
    size_t  written   = 0U;

    POUND_CHECK_EQ_I64(pound_lz4_decompress_block(NULL, 1U, output, sizeof(output), &written),
                       POUND_ERROR_INVALID_ARGUMENT);

    static const uint8_t EMPTY[] = {0};
    POUND_CHECK_EQ_I64(pound_lz4_decompress_block(EMPTY, 0U, output, sizeof(output), &written),
                       POUND_ERROR_INVALID_ARGUMENT);

    // A zero match offset would make the decoder read from before the output.
    static const uint8_t ZERO_OFFSET[] = { 0x10U, 'A', 0x00U, 0x00U };
    POUND_CHECK_EQ_I64(pound_lz4_decompress_block(ZERO_OFFSET, sizeof(ZERO_OFFSET), output, sizeof(output), &written),
                       POUND_ERROR_CORRUPTED);

    // A match that points into bytes that do not exist yet.
    static const uint8_t BAD_OFFSET[] = { 0x10U, 'A', 0x02U, 0x00U };
    POUND_CHECK_EQ_I64(pound_lz4_decompress_block(BAD_OFFSET, sizeof(BAD_OFFSET), output, sizeof(output), &written),
                       POUND_ERROR_CORRUPTED);

    // A truncated literal run.
    static const uint8_t TRUNCATED[] = { 0x50U, 'A' };
    POUND_CHECK_EQ_I64(pound_lz4_decompress_block(TRUNCATED, sizeof(TRUNCATED), output, sizeof(output), &written),
                       POUND_ERROR_CORRUPTED);

    // A destination that is too small for the block.
    static const uint8_t TOO_MUCH[] = { 0x50U, 'A', 'B', 'C', 'D', 'E' };
    uint8_t small[4];
    POUND_CHECK_EQ_I64(pound_lz4_decompress_block(TOO_MUCH, sizeof(TOO_MUCH), small, sizeof(small), &written),
                       POUND_ERROR_BUFFER_TOO_SMALL);

    // A block that decodes fewer bytes than the NSO header promised is not a success:
    // the caller would otherwise run on a partially filled segment.
    static const uint8_t SHORT[] = { 0x30U, 'A', 'B', 'C' };
    POUND_CHECK_EQ_I64(pound_lz4_decompress_block(SHORT, sizeof(SHORT), output, sizeof(output), &written),
                       POUND_ERROR_CORRUPTED);
}

POUND_TEST(lz4, overlapping_input_and_output_is_refused)
{
    uint8_t buffer[32] = { 0x20U, 'A', 'B', 0U };
    size_t  written    = 0U;

    POUND_CHECK_EQ_I64(pound_lz4_decompress_block(buffer, 3U, buffer, 16U, &written),
                       POUND_ERROR_INVALID_ARGUMENT);
}

POUND_TEST_SUITE(lz4,
                 POUND_TEST_CASE(lz4, literal_only_block_is_copied),
                 POUND_TEST_CASE(lz4, a_long_literal_run_decodes),
                 POUND_TEST_CASE(lz4, an_overlapping_match_repeats_the_last_byte),
                 POUND_TEST_CASE(lz4, malformed_blocks_are_refused),
                 POUND_TEST_CASE(lz4, overlapping_input_and_output_is_refused))

/*** end of file ***/

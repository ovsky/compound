//! AES block, mode and XTS primitives.
//!
//! The implementation is deliberately byte-oriented. It is not the fastest AES
//! on a modern CPU, but it is the same code on every host, it has no endianness
//! assumptions, and the package loader calls it a few hundred times per title
//! rather than per frame. Correctness and a readable known-answer test are worth
//! more here than a table-driven last ten percent.

#include "crypto/aes.h"

#include "log.h"
#include <string.h>

static const uint8_t AES_SBOX[256] = {
    0x63, 0x7C, 0x77, 0x7B, 0xF2, 0x6B, 0x6F, 0xC5, 0x30, 0x01, 0x67, 0x2B, 0xFE, 0xD7, 0xAB, 0x76,
    0xCA, 0x82, 0xC9, 0x7D, 0xFA, 0x59, 0x47, 0xF0, 0xAD, 0xD4, 0xA2, 0xAF, 0x9C, 0xA4, 0x72, 0xC0,
    0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC, 0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15,
    0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A, 0x07, 0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75,
    0x09, 0x83, 0x2C, 0x1A, 0x1B, 0x6E, 0x5A, 0xA0, 0x52, 0x3B, 0xD6, 0xB3, 0x29, 0xE3, 0x2F, 0x84,
    0x53, 0xD1, 0x00, 0xED, 0x20, 0xFC, 0xB1, 0x5B, 0x6A, 0xCB, 0xBE, 0x39, 0x4A, 0x4C, 0x58, 0xCF,
    0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85, 0x45, 0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8,
    0x51, 0xA3, 0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5, 0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2,
    0xCD, 0x0C, 0x13, 0xEC, 0x5F, 0x97, 0x44, 0x17, 0xC4, 0xA7, 0x7E, 0x3D, 0x64, 0x5D, 0x19, 0x73,
    0x60, 0x81, 0x4F, 0xDC, 0x22, 0x2A, 0x90, 0x88, 0x46, 0xEE, 0xB8, 0x14, 0xDE, 0x5E, 0x0B, 0xDB,
    0xE0, 0x32, 0x3A, 0x0A, 0x49, 0x06, 0x24, 0x5C, 0xC2, 0xD3, 0xAC, 0x62, 0x91, 0x95, 0xE4, 0x79,
    0xE7, 0xC8, 0x37, 0x6D, 0x8D, 0xD5, 0x4E, 0xA9, 0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A, 0xAE, 0x08,
    0xBA, 0x78, 0x25, 0x2E, 0x1C, 0xA6, 0xB4, 0xC6, 0xE8, 0xDD, 0x74, 0x1F, 0x4B, 0xBD, 0x8B, 0x8A,
    0x70, 0x3E, 0xB5, 0x66, 0x48, 0x03, 0xF6, 0x0E, 0x61, 0x35, 0x57, 0xB9, 0x86, 0xC1, 0x1D, 0x9E,
    0xE1, 0xF8, 0x98, 0x11, 0x69, 0xD9, 0x8E, 0x94, 0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF,
    0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42, 0x68, 0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16,
};

static const uint8_t AES_INV_SBOX[256] = {
    0x52, 0x09, 0x6A, 0xD5, 0x30, 0x36, 0xA5, 0x38, 0xBF, 0x40, 0xA3, 0x9E, 0x81, 0xF3, 0xD7, 0xFB,
    0x7C, 0xE3, 0x39, 0x82, 0x9B, 0x2F, 0xFF, 0x87, 0x34, 0x8E, 0x43, 0x44, 0xC4, 0xDE, 0xE9, 0xCB,
    0x54, 0x7B, 0x94, 0x32, 0xA6, 0xC2, 0x23, 0x3D, 0xEE, 0x4C, 0x95, 0x0B, 0x42, 0xFA, 0xC3, 0x4E,
    0x08, 0x2E, 0xA1, 0x66, 0x28, 0xD9, 0x24, 0xB2, 0x76, 0x5B, 0xA2, 0x49, 0x6D, 0x8B, 0xD1, 0x25,
    0x72, 0xF8, 0xF6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xD4, 0xA4, 0x5C, 0xCC, 0x5D, 0x65, 0xB6, 0x92,
    0x6C, 0x70, 0x48, 0x50, 0xFD, 0xED, 0xB9, 0xDA, 0x5E, 0x15, 0x46, 0x57, 0xA7, 0x8D, 0x9D, 0x84,
    0x90, 0xD8, 0xAB, 0x00, 0x8C, 0xBC, 0xD3, 0x0A, 0xF7, 0xE4, 0x58, 0x05, 0xB8, 0xB3, 0x45, 0x06,
    0xD0, 0x2C, 0x1E, 0x8F, 0xCA, 0x3F, 0x0F, 0x02, 0xC1, 0xAF, 0xBD, 0x03, 0x01, 0x13, 0x8A, 0x6B,
    0x3A, 0x91, 0x11, 0x41, 0x4F, 0x67, 0xDC, 0xEA, 0x97, 0xF2, 0xCF, 0xCE, 0xF0, 0xB4, 0xE6, 0x73,
    0x96, 0xAC, 0x74, 0x22, 0xE7, 0xAD, 0x35, 0x85, 0xE2, 0xF9, 0x37, 0xE8, 0x1C, 0x75, 0xDF, 0x6E,
    0x47, 0xF1, 0x1A, 0x71, 0x1D, 0x29, 0xC5, 0x89, 0x6F, 0xB7, 0x62, 0x0E, 0xAA, 0x18, 0xBE, 0x1B,
    0xFC, 0x56, 0x3E, 0x4B, 0xC6, 0xD2, 0x79, 0x20, 0x9A, 0xDB, 0xC0, 0xFE, 0x78, 0xCD, 0x5A, 0xF4,
    0x1F, 0xDD, 0xA8, 0x33, 0x88, 0x07, 0xC7, 0x31, 0xB1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xEC, 0x5F,
    0x60, 0x51, 0x7F, 0xA9, 0x19, 0xB5, 0x4A, 0x0D, 0x2D, 0xE5, 0x7A, 0x9F, 0x93, 0xC9, 0x9C, 0xEF,
    0xA0, 0xE0, 0x3B, 0x4D, 0xAE, 0x2A, 0xF5, 0xB0, 0xC8, 0xEB, 0xBB, 0x3C, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2B, 0x04, 0x7E, 0xBA, 0x77, 0xD6, 0x26, 0xE1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0C, 0x7D,
};

static const uint8_t AES_RCON[11] = {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36};

static uint8_t
xtime(const uint8_t value)
{
    return (uint8_t)((value << 1) ^ ((value & 0x80U) ? 0x1BU : 0x00U));
}

static uint8_t
gf_multiply(const uint8_t left, const uint8_t right)
{
    uint8_t result = 0U;
    uint8_t a      = left;
    uint8_t b      = right;

    for (unsigned int bit = 0U; bit < 8U; ++bit)
    {
        if (0U != (b & 1U))
        {
            result ^= a;
        }

        a = xtime(a);
        b = (uint8_t)(b >> 1U);
    }

    return result;
}

static void
add_round_key(uint8_t state[POUND_AES_BLOCK_SIZE], const uint8_t *POUND_RESTRICT round_key)
{
    for (size_t i = 0U; i < POUND_AES_BLOCK_SIZE; ++i)
    {
        state[i] ^= round_key[i];
    }
}

static void
sub_bytes(uint8_t state[POUND_AES_BLOCK_SIZE])
{
    for (size_t i = 0U; i < POUND_AES_BLOCK_SIZE; ++i)
    {
        state[i] = AES_SBOX[state[i]];
    }
}

static void
inverse_sub_bytes(uint8_t state[POUND_AES_BLOCK_SIZE])
{
    for (size_t i = 0U; i < POUND_AES_BLOCK_SIZE; ++i)
    {
        state[i] = AES_INV_SBOX[state[i]];
    }
}

static void
shift_rows(uint8_t state[POUND_AES_BLOCK_SIZE])
{
    uint8_t shifted[POUND_AES_BLOCK_SIZE];

    for (size_t column = 0U; column < 4U; ++column)
    {
        for (size_t row = 0U; row < 4U; ++row)
        {
            shifted[(column * 4U) + row] = state[(((column + row) % 4U) * 4U) + row];
        }
    }

    memcpy(state, shifted, sizeof(shifted));
}

static void
inverse_shift_rows(uint8_t state[POUND_AES_BLOCK_SIZE])
{
    uint8_t shifted[POUND_AES_BLOCK_SIZE];

    for (size_t column = 0U; column < 4U; ++column)
    {
        for (size_t row = 0U; row < 4U; ++row)
        {
            shifted[(column * 4U) + row] = state[(((column + 4U - row) % 4U) * 4U) + row];
        }
    }

    memcpy(state, shifted, sizeof(shifted));
}

static void
mix_columns(uint8_t state[POUND_AES_BLOCK_SIZE])
{
    for (size_t column = 0U; column < 4U; ++column)
    {
        uint8_t *POUND_RESTRICT c = state + (column * 4U);

        const uint8_t a = c[0];
        const uint8_t b = c[1];
        const uint8_t d = c[2];
        const uint8_t e = c[3];
        const uint8_t all = (uint8_t)(a ^ b ^ d ^ e);

        c[0] = (uint8_t)(a ^ all ^ xtime((uint8_t)(a ^ b)));
        c[1] = (uint8_t)(b ^ all ^ xtime((uint8_t)(b ^ d)));
        c[2] = (uint8_t)(d ^ all ^ xtime((uint8_t)(d ^ e)));
        c[3] = (uint8_t)(e ^ all ^ xtime((uint8_t)(e ^ a)));
    }
}

static void
inverse_mix_columns(uint8_t state[POUND_AES_BLOCK_SIZE])
{
    for (size_t column = 0U; column < 4U; ++column)
    {
        uint8_t *POUND_RESTRICT c = state + (column * 4U);

        const uint8_t a = c[0];
        const uint8_t b = c[1];
        const uint8_t d = c[2];
        const uint8_t e = c[3];

        c[0] = (uint8_t)(gf_multiply(a, 0x0EU) ^ gf_multiply(b, 0x0BU) ^ gf_multiply(d, 0x0DU)
                         ^ gf_multiply(e, 0x09U));
        c[1] = (uint8_t)(gf_multiply(a, 0x09U) ^ gf_multiply(b, 0x0EU) ^ gf_multiply(d, 0x0BU)
                         ^ gf_multiply(e, 0x0DU));
        c[2] = (uint8_t)(gf_multiply(a, 0x0DU) ^ gf_multiply(b, 0x09U) ^ gf_multiply(d, 0x0EU)
                         ^ gf_multiply(e, 0x0BU));
        c[3] = (uint8_t)(gf_multiply(a, 0x0BU) ^ gf_multiply(b, 0x0DU) ^ gf_multiply(d, 0x09U)
                         ^ gf_multiply(e, 0x0EU));
    }
}

/// Expands `key` into `round_keys`.
static error_t
expand_key(const uint8_t *POUND_RESTRICT key, const size_t key_size, uint8_t *POUND_RESTRICT round_keys,
           uint8_t *POUND_RESTRICT rounds)
{
    if ((POUND_AES_BLOCK_SIZE != key_size) && (24U != key_size) && (POUND_AES_MAX_KEY_SIZE != key_size))
    {
        POUND_LOG_ERROR(&thread_logger, "AES key size %zu is not 16, 24 or 32 bytes.", key_size);
        return POUND_ERROR_UNSUPPORTED_FEATURE;
    }

    const size_t words       = key_size / 4U;
    const size_t round_count = words + 6U;

    memcpy(round_keys, key, key_size);

    for (size_t word = words; word < (4U * (round_count + 1U)); ++word)
    {
        uint8_t temporary[4];

        memcpy(temporary, round_keys + ((word - 1U) * 4U), sizeof(temporary));

        if (0U == (word % words))
        {
            const uint8_t first = temporary[0];

            temporary[0] = (uint8_t)(AES_SBOX[temporary[1]] ^ AES_RCON[word / words]);
            temporary[1] = AES_SBOX[temporary[2]];
            temporary[2] = AES_SBOX[temporary[3]];
            temporary[3] = AES_SBOX[first];
        }
        else if ((words > 6U) && (4U == (word % words)))
        {
            for (size_t i = 0U; i < 4U; ++i)
            {
                temporary[i] = AES_SBOX[temporary[i]];
            }
        }

        for (size_t i = 0U; i < 4U; ++i)
        {
            round_keys[(word * 4U) + i]
                = (uint8_t)(round_keys[((word - words) * 4U) + i] ^ temporary[i]);
        }
    }

    *rounds = (uint8_t)round_count;
    return POUND_SUCCESS;
}

error_t
pound_aes_init(pound_aes_t *POUND_RESTRICT aes, const uint8_t *POUND_RESTRICT key, const size_t key_size)
{
    if (POUND_UNLIKELY(NULL == aes) || POUND_UNLIKELY(NULL == key))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: AES initialization was handed a NULL pointer.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    memset(aes, 0, sizeof(*aes));

    const error_t status = expand_key(key, key_size, aes->round_keys, &aes->rounds);

    if (POUND_SUCCESS != status)
    {
        return status;
    }

    // The inverse cipher walks the encryption schedule backwards with inverse
    // transforms, so no separate key schedule is needed. Keeping a second copy
    // would be a second thing that could drift out of step with the first.
    aes->key_size = (uint8_t)key_size;
    return POUND_SUCCESS;
}

error_t
pound_aes_encrypt_block(const pound_aes_t *POUND_RESTRICT aes, const uint8_t input[POUND_AES_BLOCK_SIZE],
                        uint8_t output[POUND_AES_BLOCK_SIZE])
{
    if (POUND_UNLIKELY(NULL == aes) || POUND_UNLIKELY(NULL == input) || POUND_UNLIKELY(NULL == output)
        || POUND_UNLIKELY(0U == aes->rounds))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: AES encryption was handed an uninitialized context.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    uint8_t state[POUND_AES_BLOCK_SIZE];
    memcpy(state, input, sizeof(state));

    add_round_key(state, aes->round_keys);

    for (uint8_t round = 1U; round < aes->rounds; ++round)
    {
        sub_bytes(state);
        shift_rows(state);
        mix_columns(state);
        add_round_key(state, aes->round_keys + ((size_t)round * POUND_AES_BLOCK_SIZE));
    }

    sub_bytes(state);
    shift_rows(state);
    add_round_key(state, aes->round_keys + ((size_t)aes->rounds * POUND_AES_BLOCK_SIZE));

    memcpy(output, state, sizeof(state));
    return POUND_SUCCESS;
}

error_t
pound_aes_decrypt_block(const pound_aes_t *POUND_RESTRICT aes, const uint8_t input[POUND_AES_BLOCK_SIZE],
                        uint8_t output[POUND_AES_BLOCK_SIZE])
{
    if (POUND_UNLIKELY(NULL == aes) || POUND_UNLIKELY(NULL == input) || POUND_UNLIKELY(NULL == output)
        || POUND_UNLIKELY(0U == aes->rounds))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: AES decryption was handed an uninitialized context.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    uint8_t state[POUND_AES_BLOCK_SIZE];
    memcpy(state, input, sizeof(state));

    add_round_key(state, aes->round_keys + ((size_t)aes->rounds * POUND_AES_BLOCK_SIZE));

    for (uint8_t round = aes->rounds; round > 1U; --round)
    {
        inverse_shift_rows(state);
        inverse_sub_bytes(state);
        add_round_key(state, aes->round_keys + ((size_t)(round - 1U) * POUND_AES_BLOCK_SIZE));
        inverse_mix_columns(state);
    }

    inverse_shift_rows(state);
    inverse_sub_bytes(state);
    add_round_key(state, aes->round_keys);

    memcpy(output, state, sizeof(state));
    return POUND_SUCCESS;
}

static error_t
require_block_multiple(const size_t size)
{
    if ((0U == size) || (0U != (size % POUND_AES_BLOCK_SIZE)))
    {
        POUND_LOG_ERROR(&thread_logger, "AES mode refuses a %zu-byte input; it must be a non-zero multiple of %u.",
                        size, POUND_AES_BLOCK_SIZE);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    return POUND_SUCCESS;
}

static error_t
require_mode_arguments(const pound_aes_t *POUND_RESTRICT aes, const void *POUND_RESTRICT input,
                       const void *POUND_RESTRICT output)
{
    if (POUND_UNLIKELY(NULL == aes) || POUND_UNLIKELY(NULL == input) || POUND_UNLIKELY(NULL == output)
        || POUND_UNLIKELY(0U == aes->rounds))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: an AES mode was handed a NULL or uninitialized context.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    return POUND_SUCCESS;
}

error_t
pound_aes_ecb_encrypt(const pound_aes_t *POUND_RESTRICT aes, const uint8_t *POUND_RESTRICT input,
                      uint8_t *POUND_RESTRICT output, const size_t size)
{
    error_t status = require_mode_arguments(aes, input, output);
    if (POUND_SUCCESS != status)
    {
        return status;
    }

    status = require_block_multiple(size);
    if (POUND_SUCCESS != status)
    {
        return status;
    }

    for (size_t offset = 0U; offset < size; offset += POUND_AES_BLOCK_SIZE)
    {
        (void)pound_aes_encrypt_block(aes, input + offset, output + offset);
    }

    return POUND_SUCCESS;
}

error_t
pound_aes_ecb_decrypt(const pound_aes_t *POUND_RESTRICT aes, const uint8_t *POUND_RESTRICT input,
                      uint8_t *POUND_RESTRICT output, const size_t size)
{
    error_t status = require_mode_arguments(aes, input, output);
    if (POUND_SUCCESS != status)
    {
        return status;
    }

    status = require_block_multiple(size);
    if (POUND_SUCCESS != status)
    {
        return status;
    }

    for (size_t offset = 0U; offset < size; offset += POUND_AES_BLOCK_SIZE)
    {
        (void)pound_aes_decrypt_block(aes, input + offset, output + offset);
    }

    return POUND_SUCCESS;
}

error_t
pound_aes_cbc_encrypt(const pound_aes_t *POUND_RESTRICT aes, const uint8_t iv[POUND_AES_BLOCK_SIZE],
                      const uint8_t *POUND_RESTRICT input, uint8_t *POUND_RESTRICT output, const size_t size)
{
    error_t status = require_mode_arguments(aes, input, output);
    if (POUND_SUCCESS != status)
    {
        return status;
    }

    if (POUND_UNLIKELY(NULL == iv))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: AES-CBC was given a NULL IV.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    status = require_block_multiple(size);
    if (POUND_SUCCESS != status)
    {
        return status;
    }

    uint8_t chain[POUND_AES_BLOCK_SIZE];
    memcpy(chain, iv, sizeof(chain));

    for (size_t offset = 0U; offset < size; offset += POUND_AES_BLOCK_SIZE)
    {
        uint8_t block[POUND_AES_BLOCK_SIZE];

        for (size_t i = 0U; i < POUND_AES_BLOCK_SIZE; ++i)
        {
            block[i] = (uint8_t)(input[offset + i] ^ chain[i]);
        }

        (void)pound_aes_encrypt_block(aes, block, output + offset);
        memcpy(chain, output + offset, sizeof(chain));
    }

    return POUND_SUCCESS;
}

error_t
pound_aes_cbc_decrypt(const pound_aes_t *POUND_RESTRICT aes, const uint8_t iv[POUND_AES_BLOCK_SIZE],
                      const uint8_t *POUND_RESTRICT input, uint8_t *POUND_RESTRICT output, const size_t size)
{
    error_t status = require_mode_arguments(aes, input, output);
    if (POUND_SUCCESS != status)
    {
        return status;
    }

    if (POUND_UNLIKELY(NULL == iv))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: AES-CBC was given a NULL IV.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    status = require_block_multiple(size);
    if (POUND_SUCCESS != status)
    {
        return status;
    }

    uint8_t chain[POUND_AES_BLOCK_SIZE];
    memcpy(chain, iv, sizeof(chain));

    for (size_t offset = 0U; offset < size; offset += POUND_AES_BLOCK_SIZE)
    {
        uint8_t block[POUND_AES_BLOCK_SIZE];
        uint8_t ciphertext[POUND_AES_BLOCK_SIZE];

        memcpy(ciphertext, input + offset, sizeof(ciphertext));
        (void)pound_aes_decrypt_block(aes, ciphertext, block);

        for (size_t i = 0U; i < POUND_AES_BLOCK_SIZE; ++i)
        {
            output[offset + i] = (uint8_t)(block[i] ^ chain[i]);
        }

        memcpy(chain, ciphertext, sizeof(chain));
    }

    return POUND_SUCCESS;
}

static void
increment_counter(uint8_t counter[POUND_AES_BLOCK_SIZE])
{
    for (size_t i = POUND_AES_BLOCK_SIZE; i > 0U; --i)
    {
        ++counter[i - 1U];
        if (0U != counter[i - 1U])
        {
            break;
        }
    }
}

error_t
pound_aes_ctr_xcrypt(const pound_aes_t *POUND_RESTRICT aes, const uint8_t counter[POUND_AES_BLOCK_SIZE],
                     const uint8_t *POUND_RESTRICT input, uint8_t *POUND_RESTRICT output, const size_t size)
{
    error_t status = require_mode_arguments(aes, input, output);
    if (POUND_SUCCESS != status)
    {
        return status;
    }

    if (POUND_UNLIKELY(NULL == counter))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: AES-CTR was given a NULL counter.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (0U == size)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: AES-CTR was given an empty range.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    uint8_t block_counter[POUND_AES_BLOCK_SIZE];
    memcpy(block_counter, counter, sizeof(block_counter));

    size_t offset = 0U;

    while (offset < size)
    {
        uint8_t keystream[POUND_AES_BLOCK_SIZE];

        (void)pound_aes_encrypt_block(aes, block_counter, keystream);
        increment_counter(block_counter);

        const size_t remaining = size - offset;
        const size_t count     = (remaining < POUND_AES_BLOCK_SIZE) ? remaining : POUND_AES_BLOCK_SIZE;

        for (size_t i = 0U; i < count; ++i)
        {
            output[offset + i] = (uint8_t)(input[offset + i] ^ keystream[i]);
        }

        offset += count;
    }

    return POUND_SUCCESS;
}

/// Multiplies a sixteen-byte XTS tweak by x in GF(2^128), little-endian.
///
/// This is IEEE 1619's `gf128mul_x_ble`: the byte at index zero is the least
/// significant polynomial coefficient. It must not be confused with a big-endian
/// integer shift; the reduction polynomial is applied from the high bit into
/// byte zero.
static void
xts_multiply_x(uint8_t tweak[POUND_AES_BLOCK_SIZE])
{
    uint8_t carry = 0U;

    for (size_t i = 0U; i < POUND_AES_BLOCK_SIZE; ++i)
    {
        const uint8_t next_carry = (uint8_t)(tweak[i] >> 7U);
        tweak[i] = (uint8_t)((tweak[i] << 1U) | carry);
        carry    = next_carry;
    }

    if (0U != carry)
    {
        tweak[0] ^= 0x87U;
    }
}

static error_t
xts_crypt(const uint8_t *POUND_RESTRICT key, const size_t key_size,
          const uint8_t data_unit[POUND_AES_BLOCK_SIZE], const uint8_t *POUND_RESTRICT input,
          uint8_t *POUND_RESTRICT output, const size_t size, const size_t sector_size, const bool encrypt)
{
    if (POUND_UNLIKELY(NULL == key) || POUND_UNLIKELY(NULL == data_unit) || POUND_UNLIKELY(NULL == input)
        || POUND_UNLIKELY(NULL == output))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: AES-XTS was handed a NULL pointer.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if ((32U != key_size) && (64U != key_size))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "AES-XTS needs a 32-byte AES-128-XTS or 64-byte AES-256-XTS key, not %zu bytes.",
                        key_size);
        return POUND_ERROR_UNSUPPORTED_FEATURE;
    }

    if ((sector_size < POUND_AES_BLOCK_SIZE) || (0U != (sector_size & (sector_size - 1U)))
        || (0U != (size % sector_size)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "AES-XTS refuses size %zu with sector size %zu; the size must be a whole number of "
                        "power-of-two sectors of at least %u bytes.",
                        size, sector_size, POUND_AES_BLOCK_SIZE);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (0U == size)
    {
        return POUND_SUCCESS;
    }

    // A data unit is one XTS sector. The API is called once per sector so the
    // caller controls the data-unit construction; accepting several sectors here
    // with one tweak would silently reuse a tweak across units.
    if (size != sector_size)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "AES-XTS refuses %zu bytes as one %zu-byte sector; call once per data unit.",
                        size, sector_size);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const size_t half = key_size / 2U;

    pound_aes_t data_aes;
    pound_aes_t tweak_aes;

    error_t status = pound_aes_init(&data_aes, key, half);
    if (POUND_SUCCESS == status)
    {
        status = pound_aes_init(&tweak_aes, key + half, half);
    }

    if (POUND_SUCCESS != status)
    {
        memset(&data_aes, 0, sizeof(data_aes));
        memset(&tweak_aes, 0, sizeof(tweak_aes));
        return status;
    }

    uint8_t tweak[POUND_AES_BLOCK_SIZE];
    (void)pound_aes_encrypt_block(&tweak_aes, data_unit, tweak);

    for (size_t offset = 0U; offset < size; offset += POUND_AES_BLOCK_SIZE)
    {
        uint8_t block[POUND_AES_BLOCK_SIZE];

        for (size_t i = 0U; i < POUND_AES_BLOCK_SIZE; ++i)
        {
            block[i] = (uint8_t)(input[offset + i] ^ tweak[i]);
        }

        if (encrypt)
        {
            (void)pound_aes_encrypt_block(&data_aes, block, block);
        }
        else
        {
            (void)pound_aes_decrypt_block(&data_aes, block, block);
        }

        for (size_t i = 0U; i < POUND_AES_BLOCK_SIZE; ++i)
        {
            output[offset + i] = (uint8_t)(block[i] ^ tweak[i]);
        }

        xts_multiply_x(tweak);
    }

    // The schedules hold key material derived from the caller's key. They are
    // overwritten before returning so a stack frame a crash dump happens to
    // include is not a decryption oracle.
    memset(&data_aes, 0, sizeof(data_aes));
    memset(&tweak_aes, 0, sizeof(tweak_aes));

    return POUND_SUCCESS;
}

error_t
pound_aes_xts_encrypt(const uint8_t *POUND_RESTRICT key, const size_t key_size,
                      const uint8_t tweak[POUND_AES_BLOCK_SIZE], const uint8_t *POUND_RESTRICT input,
                      uint8_t *POUND_RESTRICT output, const size_t size, const size_t sector_size)
{
    return xts_crypt(key, key_size, tweak, input, output, size, sector_size, true);
}

error_t
pound_aes_xts_decrypt(const uint8_t *POUND_RESTRICT key, const size_t key_size,
                      const uint8_t tweak[POUND_AES_BLOCK_SIZE], const uint8_t *POUND_RESTRICT input,
                      uint8_t *POUND_RESTRICT output, const size_t size, const size_t sector_size)
{
    return xts_crypt(key, key_size, tweak, input, output, size, sector_size, false);
}

/*** end of file ***/

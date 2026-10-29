#include "rsa.h"

#include "log.h"
#include "sha256.h"

#include <string.h>

/// Number of 32-bit limbs in a 2048-bit value.
#define RSA_LIMBS 64U

/// Number of bytes in the EMSA-PKCS1-v1_5 encoded message.
#define RSA_EM_BYTES POUND_RSA2048_BYTES

/// ASN.1 DigestInfo prefix for SHA-256 (RFC 8017 section 9.2, note 1).
///
/// The digest itself follows these 19 bytes; together they are 51 bytes, which
/// leaves 202 bytes of 0xFF padding in a 2048-bit block. The prefix is written
/// out rather than DER-encoded because there is exactly one algorithm here and a
/// DER encoder would be more code with more ways to be wrong.
static const uint8_t sha256_digest_info_prefix[19] = {
    0x30, 0x31, 0x30, 0x0D, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
    0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20,
};

static bool
limbs_is_zero(const uint32_t value[RSA_LIMBS])
{
    uint32_t combined = 0U;

    for (size_t i = 0; i < RSA_LIMBS; ++i)
    {
        combined |= value[i];
    }

    return combined == 0U;
}

/// Compares two little-endian limb arrays, returning -1, 0 or 1.
static int
limbs_compare(const uint32_t a[RSA_LIMBS], const uint32_t b[RSA_LIMBS])
{
    for (size_t i = RSA_LIMBS; i-- > 0U;)
    {
        if (a[i] != b[i])
        {
            return (a[i] > b[i]) ? 1 : -1;
        }
    }

    return 0;
}

/// Subtracts `b` from `a` in place, returning the final borrow (0 or 1).
static uint32_t
limbs_subtract(uint32_t a[RSA_LIMBS], const uint32_t b[RSA_LIMBS])
{
    uint64_t borrow = 0U;

    for (size_t i = 0; i < RSA_LIMBS; ++i)
    {
        const uint64_t subtrahend = (uint64_t)b[i] + borrow;
        const uint64_t minuend    = a[i];

        a[i]   = (uint32_t)(minuend - subtrahend);
        borrow = (minuend < subtrahend) ? 1U : 0U;
    }

    return (uint32_t)borrow;
}

/// Doubles `value` modulo `modulus`, both little-endian.
static void
limbs_double_mod(uint32_t value[RSA_LIMBS], const uint32_t modulus[RSA_LIMBS])
{
    uint32_t carry = 0U;

    for (size_t i = 0; i < RSA_LIMBS; ++i)
    {
        const uint64_t doubled = ((uint64_t)value[i] << 1U) | carry;

        value[i] = (uint32_t)doubled;
        carry    = (uint32_t)(doubled >> 32U);
    }

    // The input is reduced, so the doubled value is below 2 * modulus. One
    // subtraction is therefore enough, and a carry out of the top limb is
    // exactly the case where that subtraction must happen.
    if ((carry != 0U) || (limbs_compare(value, modulus) >= 0))
    {
        (void)limbs_subtract(value, modulus);
    }
}

/// Reads a 256-byte big-endian value into 64 little-endian 32-bit limbs.
static void
rsa_load_be(const uint8_t in[POUND_RSA2048_BYTES], uint32_t out[RSA_LIMBS])
{
    for (size_t i = 0; i < RSA_LIMBS; ++i)
    {
        const size_t at = POUND_RSA2048_BYTES - ((i + 1U) * 4U);

        out[i] = ((uint32_t)in[at] << 24U) | ((uint32_t)in[at + 1U] << 16U)
               | ((uint32_t)in[at + 2U] << 8U) | (uint32_t)in[at + 3U];
    }
}

/// Writes 64 little-endian 32-bit limbs as a 256-byte big-endian value.
static void
rsa_store_be(const uint32_t in[RSA_LIMBS], uint8_t out[POUND_RSA2048_BYTES])
{
    for (size_t i = 0; i < RSA_LIMBS; ++i)
    {
        const size_t at = POUND_RSA2048_BYTES - ((i + 1U) * 4U);

        out[at]      = (uint8_t)(in[i] >> 24U);
        out[at + 1U] = (uint8_t)(in[i] >> 16U);
        out[at + 2U] = (uint8_t)(in[i] >> 8U);
        out[at + 3U] = (uint8_t)in[i];
    }
}

/// Computes `-modulus[0]^-1 mod 2^32` by Newton iteration.
static uint32_t
montgomery_factor(const uint32_t modulus[RSA_LIMBS])
{
    uint32_t inverse = 1U;

    // Each iteration doubles the number of correct low bits; five iterations
    // cover all 32 bits of the word.
    for (unsigned i = 0; i < 5U; ++i)
    {
        inverse *= 2U - (modulus[0] * inverse);
    }

    return 0U - inverse;
}

/// Montgomery multiplication for 2048-bit values.
///
/// `out = a * b * R^-1 mod modulus`, where `R = 2^2048`. The intermediate is
/// 128 limbs wide plus one carry limb; every step is a 32x32->64 multiply, so
/// this is portable to hosts without a 128-bit integer type.
static void
montgomery_multiply(const uint32_t a[RSA_LIMBS],
                    const uint32_t b[RSA_LIMBS],
                    const uint32_t modulus[RSA_LIMBS],
                    const uint32_t factor,
                    uint32_t       out[RSA_LIMBS])
{
    uint32_t product[2U * RSA_LIMBS + 1U];
    const size_t product_limbs = 2U * RSA_LIMBS + 1U;

    memset(product, 0, sizeof(product));

    for (size_t i = 0; i < RSA_LIMBS; ++i)
    {
        uint64_t carry = 0U;

        for (size_t j = 0; j < RSA_LIMBS; ++j)
        {
            const uint64_t sum = (uint64_t)a[i] * b[j] + product[i + j] + carry;

            product[i + j] = (uint32_t)sum;
            carry          = sum >> 32U;
        }

        size_t at = i + RSA_LIMBS;

        while ((carry != 0U) && (at < product_limbs))
        {
            const uint64_t sum = (uint64_t)product[at] + carry;

            product[at] = (uint32_t)sum;
            carry       = sum >> 32U;
            at++;
        }

        if (carry != 0U)
        {
            memset(out, 0, RSA_LIMBS * sizeof(out[0]));
            return;
        }
    }

    for (size_t i = 0; i < RSA_LIMBS; ++i)
    {
        const uint32_t m     = product[i] * factor;
        uint64_t       carry = 0U;

        for (size_t j = 0; j < RSA_LIMBS; ++j)
        {
            const uint64_t sum = (uint64_t)m * modulus[j] + product[i + j] + carry;

            product[i + j] = (uint32_t)sum;
            carry          = sum >> 32U;
        }

        size_t at = i + RSA_LIMBS;

        while ((carry != 0U) && (at < product_limbs))
        {
            const uint64_t sum = (uint64_t)product[at] + carry;

            product[at] = (uint32_t)sum;
            carry       = sum >> 32U;
            at++;
        }

        if (carry != 0U)
        {
            memset(out, 0, RSA_LIMBS * sizeof(out[0]));
            return;
        }
    }

    uint32_t result[RSA_LIMBS];

    memcpy(result, &product[RSA_LIMBS], sizeof(result));

    uint32_t high = product[2U * RSA_LIMBS];

    // The REDC result is below 2 * modulus. At most one subtraction is needed,
    // but the loop also handles the carry limb so a future change to the limb
    // width cannot silently keep an out-of-range result.
    while ((high != 0U) || (limbs_compare(result, modulus) >= 0))
    {
        const uint32_t borrow = limbs_subtract(result, modulus);

        if (high < borrow)
        {
            // Mathematically unreachable; refusing to underflow is safer than
            // wrapping into a value that would verify the wrong signature.
            memset(out, 0, RSA_LIMBS * sizeof(out[0]));
            return;
        }

        high -= borrow;
    }

    memcpy(out, result, sizeof(result));
}

/// Reduces `value` from Montgomery form.
static void
montgomery_from(const uint32_t value[RSA_LIMBS],
                const uint32_t modulus[RSA_LIMBS],
                const uint32_t factor,
                uint32_t       out[RSA_LIMBS])
{
    uint32_t one[RSA_LIMBS] = { 0U };

    one[0] = 1U;
    montgomery_multiply(value, one, modulus, factor, out);
}

/// Encodes `signature` with the public exponent and checks the PKCS#1 v1.5
/// envelope around `digest`.
static error_t
verify_encoded(const uint32_t modulus[RSA_LIMBS],
               const uint32_t factor,
               const uint32_t montgomery_one[RSA_LIMBS],
               const uint32_t montgomery_r2[RSA_LIMBS],
               const uint32_t exponent,
               const uint8_t  signature[POUND_RSA2048_BYTES],
               const uint8_t  digest[POUND_RSA_SHA256_DIGEST_BYTES])
{
    uint32_t signature_limbs[RSA_LIMBS];

    rsa_load_be(signature, signature_limbs);

    if (limbs_is_zero(signature_limbs) || (limbs_compare(signature_limbs, modulus) >= 0))
    {
        return POUND_ERROR_SIGNATURE_INVALID;
    }

    uint32_t base[RSA_LIMBS];
    uint32_t accumulator[RSA_LIMBS];

    montgomery_multiply(signature_limbs, montgomery_r2, modulus, factor, base);
    memcpy(accumulator, montgomery_one, sizeof(accumulator));

    for (size_t bit = 32U; bit-- > 0U;)
    {
        montgomery_multiply(accumulator, accumulator, modulus, factor, accumulator);

        if (((exponent >> bit) & 1U) != 0U)
        {
            montgomery_multiply(accumulator, base, modulus, factor, accumulator);
        }
    }

    uint32_t encoded_limbs[RSA_LIMBS];

    montgomery_from(accumulator, modulus, factor, encoded_limbs);

    uint8_t encoded[RSA_EM_BYTES];

    rsa_store_be(encoded_limbs, encoded);

    if ((encoded[0] != 0x00U) || (encoded[1] != 0x01U))
    {
        return POUND_ERROR_SIGNATURE_INVALID;
    }

    size_t padding_end = 2U;

    while ((padding_end < RSA_EM_BYTES) && (encoded[padding_end] == 0xFFU))
    {
        padding_end++;
    }

    // PKCS#1 requires at least eight 0xFF bytes, and the separator must exist
    // before the fixed-size DigestInfo.
    if ((padding_end < 10U) || (padding_end >= RSA_EM_BYTES) || (encoded[padding_end] != 0x00U))
    {
        return POUND_ERROR_SIGNATURE_INVALID;
    }

    const size_t digest_info = padding_end + 1U;

    if (RSA_EM_BYTES - digest_info != sizeof(sha256_digest_info_prefix) + SHA256_DIGEST_SIZE)
    {
        return POUND_ERROR_SIGNATURE_INVALID;
    }

    if (0 != memcmp(&encoded[digest_info], sha256_digest_info_prefix, sizeof(sha256_digest_info_prefix)))
    {
        return POUND_ERROR_SIGNATURE_INVALID;
    }

    if (0
        != memcmp(&encoded[digest_info + sizeof(sha256_digest_info_prefix)],
                  digest,
                  SHA256_DIGEST_SIZE))
    {
        return POUND_ERROR_SIGNATURE_INVALID;
    }

    return POUND_SUCCESS;
}

bool
pound_rsa2048_public_key_is_valid(const pound_rsa2048_public_key_t *POUND_RESTRICT key)
{
    if (NULL == key)
    {
        return false;
    }

    uint32_t modulus[RSA_LIMBS];

    rsa_load_be(key->modulus, modulus);

    // A 2048-bit modulus has its top bit set, and RSA moduli are odd.
    if (((modulus[RSA_LIMBS - 1U] & 0x80000000U) == 0U) || ((modulus[0] & 1U) == 0U)
        || limbs_is_zero(modulus))
    {
        return false;
    }

    const uint32_t exponent = ((uint32_t)key->exponent[0] << 24U)
                            | ((uint32_t)key->exponent[1] << 16U)
                            | ((uint32_t)key->exponent[2] << 8U)
                            | (uint32_t)key->exponent[3];

    return (exponent >= 3U) && ((exponent & 1U) != 0U);
}

error_t
pound_rsa2048_sha256_verify(const pound_rsa2048_public_key_t *POUND_RESTRICT key,
                            const uint8_t signature[POUND_RSA2048_BYTES],
                            const uint8_t digest[POUND_RSA_SHA256_DIGEST_BYTES])
{
    if ((NULL == key) || (NULL == signature) || (NULL == digest))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting RSA verification: a required pointer is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (false == pound_rsa2048_public_key_is_valid(key))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting RSA verification: the public key is not a valid 2048-bit "
                        "RSA key.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    uint32_t modulus[RSA_LIMBS];

    rsa_load_be(key->modulus, modulus);

    const uint32_t factor = montgomery_factor(modulus);

    // R mod n and R^2 mod n are built by repeated doubling. Two thousand and
    // forty-eight iterations is trivial next to one signature verification and
    // avoids a division routine whose overflow cases would need their own tests.
    uint32_t montgomery_one[RSA_LIMBS] = { 0U };
    uint32_t montgomery_r2[RSA_LIMBS];

    montgomery_one[0] = 1U;

    for (size_t i = 0; i < 2048U; ++i)
    {
        limbs_double_mod(montgomery_one, modulus);
    }

    memcpy(montgomery_r2, montgomery_one, sizeof(montgomery_r2));

    for (size_t i = 0; i < 2048U; ++i)
    {
        limbs_double_mod(montgomery_r2, modulus);
    }

    const uint32_t exponent = ((uint32_t)key->exponent[0] << 24U)
                            | ((uint32_t)key->exponent[1] << 16U)
                            | ((uint32_t)key->exponent[2] << 8U)
                            | (uint32_t)key->exponent[3];

    return verify_encoded(modulus,
                          factor,
                          montgomery_one,
                          montgomery_r2,
                          exponent,
                          signature,
                          digest);
}

error_t
pound_rsa2048_sha256_verify_message(const pound_rsa2048_public_key_t *POUND_RESTRICT key,
                                    const uint8_t signature[POUND_RSA2048_BYTES],
                                    const void *POUND_RESTRICT message,
                                    const size_t               length)
{
    if ((NULL == message) && (length != 0U))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting RSA message verification: message is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    uint8_t digest[SHA256_DIGEST_SIZE];

    sha256(message, length, digest);

    const error_t status = pound_rsa2048_sha256_verify(key, signature, digest);

    memset(digest, 0, sizeof(digest));
    return status;
}

/*** end of file ***/

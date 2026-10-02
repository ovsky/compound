//! SHA-256, as specified in FIPS 180-4.
//!
//! Section and clause numbers in the comments below refer to FIPS 180-4, so each step
//! can be checked against the specification rather than against another
//! implementation. The structure follows the specification's own decomposition: an
//! initial value, a message schedule, a compression function, and the padding rule.

#include "crypto/sha256.h"

#include <string.h>

/// FIPS 180-4 section 4.2.2: the SHA-256 initial hash value H(0).
///
/// These are the first thirty-two bits of the fractional parts of the square roots of
/// the first eight primes. The specification prints them, so they are transcribed
/// rather than computed; a computed table would be a second thing that can be wrong.
static const uint32_t SHA256_INITIAL_STATE[8] = {
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
};

/// FIPS 180-4 section 4.2.2: the round constants K, the first thirty-two bits of the
/// fractional parts of the cube roots of the first sixty-four primes.
static const uint32_t SHA256_ROUND_CONSTANTS[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

/// Rotates a 32-bit word right by `bits`, which is never zero and never 32.
///
/// FIPS 180-4 uses the right rotations 2, 6, 7, 11, 13, 17, 18, 19, 22 and 25. The
/// shift is written as a plain `uint32_t` shift with a hard-coded 32 so that it stays
/// well defined on a host where `int` is wider than 32 bits; a shift by a `size_t`
/// that happens to be wider than the promoted type is not.
static uint32_t
rotate_right(const uint32_t value, const uint32_t bits)
{
    return (value >> bits) | (value << (32U - bits));
}

/// Reads four bytes of a block as one big-endian word.
///
/// Written as shifts rather than `memcpy` into a `uint32_t` followed by a byte swap, so
/// that the result does not depend on the host's byte order and does not depend on a
/// strict-aliasing assumption. FIPS 180-4 section 6.1.1 defines the input to the
/// compression function as a big-endian sequence of 32-bit words regardless of the
/// machine.
static uint32_t
load_big_endian_32(const uint8_t *POUND_RESTRICT bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

/// FIPS 180-4 section 6.2.2: the compression function.
///
/// `state` is updated in place with the eight chaining words for the next block, and
/// `block` is exactly `SHA256_BLOCK_SIZE` bytes.
static void
sha256_compress(uint32_t *POUND_RESTRICT state, const uint8_t *POUND_RESTRICT block)
{
    // Section 4.2.2 step 1: the message schedule. The first sixteen words are the block
    // itself; the remaining forty-eight are built from them.
    uint32_t schedule[64];

    for (size_t i = 0U; i < 16U; ++i)
    {
        schedule[i] = load_big_endian_32(block + (i * 4U));
    }

    for (size_t i = 16U; i < 64U; ++i)
    {
        // sigma0 and sigma1 as defined in section 4.1.2.
        const uint32_t previous15 = schedule[i - 15U];
        const uint32_t previous2  = schedule[i - 2U];

        const uint32_t sigma0 = rotate_right(previous15, 7U)
                                ^ rotate_right(previous15, 18U)
                                ^ (previous15 >> 3);
        const uint32_t sigma1 = rotate_right(previous2, 17U) ^ rotate_right(previous2, 19U) ^ (previous2 >> 10);

        schedule[i] = schedule[i - 16U] + sigma0 + schedule[i - 7U] + sigma1;
    }

    // Section 4.2.2 step 2: the eight working variables, seeded from the chaining value.
    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t f = state[5];
    uint32_t g = state[6];
    uint32_t h = state[7];

    // Section 4.2.2 steps 3 and 4: the sixty-four rounds.
    for (size_t i = 0U; i < 64U; ++i)
    {
        const uint32_t sum1     = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
        const uint32_t choose   = (e & f) ^ ((~e) & g);
        const uint32_t temp1    = h + sum1 + choose + SHA256_ROUND_CONSTANTS[i] + schedule[i];
        const uint32_t sum0     = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2    = sum0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    // Section 4.2.2 step 5: add the working variables back into the chaining value.
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void
sha256_init(sha256_t *POUND_RESTRICT context)
{
    memcpy(context->state, SHA256_INITIAL_STATE, sizeof(SHA256_INITIAL_STATE));
    context->total_bytes = 0U;
    context->buffered    = 0U;
    memset(context->buffer, 0, sizeof(context->buffer));
}

void
sha256_update(sha256_t *POUND_RESTRICT context, const void *POUND_RESTRICT data, size_t size)
{
    if (0U == size)
    {
        return;
    }

    const uint8_t *bytes = (const uint8_t *)data;

    context->total_bytes += (uint64_t)size;

    // Top up a partial block first. This is the common case for a stream of small
    // reads, such as hashing a partition one buffer at a time, and it keeps the
    // compression calls aligned to the block boundary the specification requires.
    if (0U != context->buffered)
    {
        const size_t wanted = SHA256_BLOCK_SIZE - context->buffered;
        const size_t take   = (size < wanted) ? size : wanted;

        memcpy(context->buffer + context->buffered, bytes, take);

        context->buffered += take;
        bytes += take;
        size -= take;

        if (context->buffered < SHA256_BLOCK_SIZE)
        {
            return;
        }

        sha256_compress(context->state, context->buffer);
        context->buffered = 0U;
    }

    // Compress every whole block still in the input. Advancing the pointer by a
    // `SHA256_BLOCK_SIZE` stride keeps it inside the caller's buffer, so this is
    // pointer arithmetic within one object rather than an out-of-bounds computation.
    while (size >= SHA256_BLOCK_SIZE)
    {
        sha256_compress(context->state, bytes);
        bytes += SHA256_BLOCK_SIZE;
        size -= SHA256_BLOCK_SIZE;
    }

    // Hold back whatever is left for the next call, or for the padding in `final`.
    if (0U != size)
    {
        memcpy(context->buffer, bytes, size);
        context->buffered = size;
    }
}

void
sha256_final(sha256_t *POUND_RESTRICT context, uint8_t out[SHA256_DIGEST_SIZE])
{
    // FIPS 180-4 section 5.1.1: append a single '1' bit, then enough '0' bits for the
    // message to occupy a whole number of blocks minus eight bytes, then the message
    // length in bits as a big-endian 64-bit integer.
    //
    // The length is taken before any padding byte is added, which is the whole reason
    // `total_bytes` is maintained across every call rather than derived from
    // `buffered`.
    const uint64_t bit_length = context->total_bytes * 8U;

    context->buffer[context->buffered] = 0x80U;
    context->buffered++;

    // The length field occupies the last eight bytes of a block, so if the '1' bit
    // landed in the last eight bytes there is no room for it and this block must be
    // finished off now.
    if (context->buffered > (SHA256_BLOCK_SIZE - 8U))
    {
        memset(context->buffer + context->buffered, 0, SHA256_BLOCK_SIZE - context->buffered);
        sha256_compress(context->state, context->buffer);
        context->buffered = 0U;
    }

    memset(context->buffer + context->buffered, 0, (SHA256_BLOCK_SIZE - 8U) - context->buffered);

    for (size_t i = 0U; i < 8U; ++i)
    {
        context->buffer[(SHA256_BLOCK_SIZE - 8U) + i] = (uint8_t)(bit_length >> (56U - (i * 8U)));
    }

    sha256_compress(context->state, context->buffer);

    // FIPS 180-4 section 6.1: the digest is the chaining value, serialised big-endian.
    for (size_t i = 0U; i < 8U; ++i)
    {
        out[(i * 4U) + 0U] = (uint8_t)(context->state[i] >> 24);
        out[(i * 4U) + 1U] = (uint8_t)(context->state[i] >> 16);
        out[(i * 4U) + 2U] = (uint8_t)(context->state[i] >> 8);
        out[(i * 4U) + 3U] = (uint8_t)(context->state[i]);
    }

    // Left initialised so a caller hashing a run of partitions in one context does not
    // have to remember to reinitialise between them.
    sha256_init(context);
}

void
sha256(const void *POUND_RESTRICT data, const size_t size, uint8_t out[SHA256_DIGEST_SIZE])
{
    sha256_t context;

    sha256_init(&context);
    sha256_update(&context, data, size);
    sha256_final(&context, out);
}

void
sha256_to_hex(const uint8_t digest[SHA256_DIGEST_SIZE], char out[SHA256_HEX_SIZE + 1U])
{
    static const char HEX_DIGITS[] = "0123456789abcdef";

    for (size_t i = 0U; i < SHA256_DIGEST_SIZE; ++i)
    {
        out[(i * 2U) + 0U] = HEX_DIGITS[(digest[i] >> 4) & 0x0FU];
        out[(i * 2U) + 1U] = HEX_DIGITS[digest[i] & 0x0FU];
    }

    out[SHA256_HEX_SIZE] = '\0';
}

bool
sha256_equal(const uint8_t *POUND_RESTRICT a, const uint8_t *POUND_RESTRICT b)
{
    uint8_t difference = 0U;

    for (size_t i = 0U; i < SHA256_DIGEST_SIZE; ++i)
    {
        difference |= (uint8_t)(a[i] ^ b[i]);
    }

    return 0U == difference;
}

/*** end of file ***/

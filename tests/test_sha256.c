//! Tests for SHA-256.
//!
//! The vectors are the ones published in FIPS 180-4 and in the NIST example programs.
//! They are transcribed rather than generated, and they are the only reason to believe
//! the implementation is correct at all -- a hash that is merely self-consistent would
//! pass every chunking and reuse case below and still disagree with every other SHA-256
//! on earth, which for this file means every container header in every game.
//!
//! Five lengths are checked against the specification, chosen so that between them they
//! exercise every branch of the padding rule:
//!
//!   * 0 bytes    -- the message is entirely padding, so the length field is the only
//!                   content and lands in the first block.
//!   * 3 bytes    -- a single partial block, padded within it.
//!   * 56 bytes   -- exactly the largest message whose '1' bit and length field still
//!                   fit in the same block. This is the case a padding rule gets wrong.
//!   * 112 bytes  -- two full blocks, so the message ends on a block boundary and the
//!                   padding must open a third.
//!   * 1000000    -- long enough that the 64-bit length field and the multi-block loop
//!                   are both genuinely exercised at scale.
//!
//! Everything else in the suite is self-consistency: the streaming interface against
//! the one-shot one, a context reused after finalisation, and the padding boundaries at
//! every length up to several blocks. Those cannot make the hash correct, but they do
//! catch the mistakes that are easy to make while refactoring a working implementation.

#include "pound_test.h"

#include "crypto/sha256.h"
#include <string.h>

/// FIPS 180-4 examples, as lowercase hex.
#define DIGEST_EMPTY   "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
#define DIGEST_ABC     "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
#define DIGEST_448_BIT "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
#define DIGEST_896_BIT "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"
#define DIGEST_MILLION "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"

/// FIPS 180-4's two multi-block examples, verbatim.
static const char MESSAGE_448[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
static const char MESSAGE_896[] = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                                  "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";

/// Hashes `data` and compares against `expected_hex`, reporting both.
///
/// A raw byte comparison would print thirty-two unreadable numbers on failure, so the
/// digests are rendered and the message names what was being hashed. This is the whole
/// output of a failed conformance case, so it has to be legible.
static void
check_digest(const void *data, const size_t size, const char *expected_hex, const char *where)
{
    uint8_t digest[SHA256_DIGEST_SIZE];
    char    actual_hex[SHA256_HEX_SIZE + 1U];

    sha256(data, size, digest);
    sha256_to_hex(digest, actual_hex);

    POUND_CHECK_MSG(0 == strcmp(actual_hex, expected_hex),
                    "%s: hashing %zu bytes gave %s, but FIPS 180-4 specifies %s",
                    where,
                    size,
                    actual_hex,
                    expected_hex);
}

POUND_TEST(sha256, the_empty_message_matches_the_specification)
{
    // The degenerate case, and the one most likely to be wrong: there is no message to
    // pad around, so the whole block is the '1' bit followed by zeros and the length.
    check_digest("", 0U, DIGEST_EMPTY, "the empty message");
}

POUND_TEST(sha256, a_single_block_message_matches_the_specification)
{
    check_digest("abc", 3U, DIGEST_ABC, "\"abc\"");
}

POUND_TEST(sha256, a_message_that_exactly_fits_one_block_matches_the_specification)
{
    // Fifty-six bytes is the boundary the padding rule turns on. Below it, the '1' bit
    // and the eight-byte length field fit in the same block as the message. Above it,
    // the message needs a second block, and the number of compressions changes from one
    // to two. Sixty-four bytes of input is exactly one full block, so the padding opens
    // the next one.
    POUND_REQUIRE(sizeof(MESSAGE_448) - 1U == 56U);

    check_digest(MESSAGE_448, sizeof(MESSAGE_448) - 1U, DIGEST_448_BIT, "the 56-byte example");
}

POUND_TEST(sha256, a_multi_block_message_matches_the_specification)
{
    POUND_REQUIRE(sizeof(MESSAGE_896) - 1U == 112U);

    check_digest(MESSAGE_896, sizeof(MESSAGE_896) - 1U, DIGEST_896_BIT, "the 112-byte example");
}

POUND_TEST(sha256, a_million_repeated_bytes_match_the_specification)
{
    // Fed a thousand bytes at a time rather than allocated in one piece, so this also
    // drives the streaming path a thousand times. The result must not depend on where
    // the caller's buffer boundaries fall, which is the property being tested as much
    // as the digest.
    uint8_t chunk[1000];
    uint8_t digest[SHA256_DIGEST_SIZE];
    char    actual_hex[SHA256_HEX_SIZE + 1U];

    memset(chunk, 'a', sizeof(chunk));

    sha256_t context;

    sha256_init(&context);

    for (size_t i = 0U; i < 1000U; ++i)
    {
        sha256_update(&context, chunk, sizeof(chunk));
    }

    sha256_final(&context, digest);
    sha256_to_hex(digest, actual_hex);

    POUND_CHECK_MSG(0 == strcmp(actual_hex, DIGEST_MILLION),
                    "one million 'a' characters gave %s, but the specification gives %s",
                    actual_hex,
                    DIGEST_MILLION);
}

POUND_TEST(sha256, chunk_boundaries_do_not_change_the_digest)
{
    // Every length from nothing to several blocks past the one-block and two-block
    // padding boundaries, hashed one byte at a time and compared against the one-shot
    // answer.
    //
    // One byte at a time is the pathological case for the buffered path: every call
    // either tops up a partial block or has to trigger a compression on its own. Any
    // mistake in the "how much is left" arithmetic shows up as a mismatch here at
    // exactly one length, which is a far easier failure to read than a digest that is
    // wrong for a million bytes.
    uint8_t message[300];
    uint8_t one_shot[SHA256_DIGEST_SIZE];
    uint8_t streamed[SHA256_DIGEST_SIZE];

    for (size_t i = 0U; i < sizeof(message); ++i)
    {
        message[i] = (uint8_t)(i * 7U);
    }

    for (size_t length = 0U; length <= sizeof(message); ++length)
    {
        sha256(message, length, one_shot);

        sha256_t context;

        sha256_init(&context);

        for (size_t i = 0U; i < length; ++i)
        {
            sha256_update(&context, message + i, 1U);
        }

        sha256_final(&context, streamed);

        if (!sha256_equal(one_shot, streamed))
        {
            char expected_hex[SHA256_HEX_SIZE + 1U];
            char actual_hex[SHA256_HEX_SIZE + 1U];

            sha256_to_hex(one_shot, expected_hex);
            sha256_to_hex(streamed, actual_hex);

            POUND_CHECK_MSG(false,
                            "hashing %zu bytes one byte at a time gave %s, but hashing it in "
                            "one call gave %s",
                            length,
                            actual_hex,
                            expected_hex);
        }
    }
}

POUND_TEST(sha256, awkward_chunk_sizes_do_not_change_the_digest)
{
    // Chunk sizes chosen to straddle every internal boundary: one byte short of a block,
    // one byte over, exactly a block, and a size that is not a factor of anything in
    // the algorithm. A bug that only appears for a chunk size that divides evenly would
    // survive the one-byte-at-a-time case.
    static const size_t CHUNK_SIZES[] = {1U, 3U, 7U, 31U, 32U, 55U, 56U, 63U, 64U, 65U, 127U, 128U, 1000U};
    uint8_t             message[1000];
    uint8_t             one_shot[SHA256_DIGEST_SIZE];
    uint8_t             streamed[SHA256_DIGEST_SIZE];

    for (size_t i = 0U; i < sizeof(message); ++i)
    {
        message[i] = (uint8_t)(i * 31U);
    }

    sha256(message, sizeof(message), one_shot);

    for (size_t c = 0U; c < (sizeof(CHUNK_SIZES) / sizeof(CHUNK_SIZES[0])); ++c)
    {
        const size_t chunk = CHUNK_SIZES[c];

        sha256_t context;

        sha256_init(&context);

        for (size_t offset = 0U; offset < sizeof(message); offset += chunk)
        {
            const size_t remaining = sizeof(message) - offset;
            const size_t take      = (remaining < chunk) ? remaining : chunk;

            sha256_update(&context, message + offset, take);
        }

        sha256_final(&context, streamed);

        POUND_CHECK_MSG(sha256_equal(one_shot, streamed),
                        "hashing 1000 bytes in %zu-byte pieces disagreed with hashing them "
                        "in one call",
                        chunk);
    }
}

POUND_TEST(sha256, an_empty_update_is_a_no_op)
{
    // A caller walking a partition table will legitimately have nothing to contribute
    // for some entries, and that must not perturb the state.
    uint8_t digest[SHA256_DIGEST_SIZE];
    char    hex[SHA256_HEX_SIZE + 1U];

    sha256_t context;

    sha256_init(&context);
    sha256_update(&context, NULL, 0U);
    sha256_update(&context, "abc", 3U);
    sha256_update(&context, NULL, 0U);
    sha256_final(&context, digest);

    sha256_to_hex(digest, hex);

    POUND_CHECK_MSG(0 == strcmp(hex, DIGEST_ABC),
                    "a zero-length update changed the digest: got %s, expected %s",
                    hex,
                    DIGEST_ABC);
}

POUND_TEST(sha256, a_finalised_context_is_ready_to_hash_again)
{
    // `sha256_final` resets rather than poisons. A long-lived context -- one held by the
    // loader, reused for every partition of every image -- depends on that, and a
    // context that had to be reinitialised by hand would be a silent-corruption bug the
    // first time somebody forgot.
    uint8_t first[SHA256_DIGEST_SIZE];
    uint8_t second[SHA256_DIGEST_SIZE];
    uint8_t fresh[SHA256_DIGEST_SIZE];
    char    hex[SHA256_HEX_SIZE + 1U];

    sha256_t context;

    sha256_init(&context);
    sha256_update(&context, "abc", 3U);
    sha256_final(&context, first);

    sha256_update(&context, "abc", 3U);
    sha256_final(&context, second);

    sha256("abc", 3U, fresh);

    POUND_CHECK(sha256_equal(first, second));
    POUND_CHECK(sha256_equal(first, fresh));

    sha256_to_hex(first, hex);
    POUND_CHECK_MSG(0 == strcmp(hex, DIGEST_ABC), "the reused context gave %s, expected %s", hex, DIGEST_ABC);
}

POUND_TEST(sha256, every_byte_of_a_message_affects_its_digest)
{
    // A hash that ignored part of its input would still pass every vector above, since
    // the vectors happen to be sensitive. Flipping one bit at each of the thirty-two
    // digest bytes' worth of positions proves the whole message is covered: the digest
    // of a message must change if any byte of it changes.
    uint8_t message[128];
    uint8_t baseline[SHA256_DIGEST_SIZE];
    uint8_t modified[SHA256_DIGEST_SIZE];

    for (size_t i = 0U; i < sizeof(message); ++i)
    {
        message[i] = (uint8_t)i;
    }

    sha256(message, sizeof(message), baseline);

    for (size_t i = 0U; i < sizeof(message); ++i)
    {
        const uint8_t original = message[i];

        // Both the low bit and the high bit, so a byte-order or shift error that only
        // shows in part of a byte is caught too.
        for (int bit = 0; bit < 8; bit += 7)
        {
            message[i] = (uint8_t)(original ^ (1U << bit));

            sha256(message, sizeof(message), modified);

            POUND_CHECK_MSG(!sha256_equal(baseline, modified),
                            "flipping bit %d of byte %zu did not change the digest",
                            bit,
                            i);
        }

        message[i] = original;
    }
}

POUND_TEST(sha256, hex_output_is_lowercase_and_nul_terminated)
{
    // The rendering is what appears in log records and in a loader's error message, so
    // it has to be exactly sixty-four lowercase hex characters and a NUL. A buffer that
    // was one byte short would be a nasty overflow in a diagnostic path.
    uint8_t digest[SHA256_DIGEST_SIZE];
    char    hex[SHA256_HEX_SIZE + 1U];

    for (size_t i = 0U; i < SHA256_DIGEST_SIZE; ++i)
    {
        // Every possible nibble value, so the digit table is fully covered.
        digest[i] = (uint8_t)(i * 8U);
    }

    sha256_to_hex(digest, hex);

    POUND_REQUIRE('\0' == hex[SHA256_HEX_SIZE]);

    for (size_t i = 0U; i < SHA256_HEX_SIZE; ++i)
    {
        const char c = hex[i];

        POUND_CHECK_MSG(('0' <= c) && (c <= '9') || ('a' <= c) && (c <= 'f'),
                        "character %zu of the hex digest is '%c', which is not a lowercase "
                        "hex digit",
                        i,
                        c);
    }

    // The full expected rendering, high nibble first. Compared as a string rather than
    // nibble by nibble because a nibble check would pass even if two bytes were
    // transposed, and byte order is precisely what a broken implementation gets wrong.
    // The input was chosen so the expected text exercises every hex digit 0-9 and a-f.
    POUND_CHECK_MSG(0 == strcmp(hex, "0008101820283038404850586068707880889098a0a8b0b8c0c8d0d8e0e8f0f8"),
                    "the digest of 0x00 0x08 0x10 ... 0xf8 rendered as %s",
                    hex);
}

POUND_TEST(sha256, digests_compare_by_value)
{
    uint8_t a[SHA256_DIGEST_SIZE];
    uint8_t b[SHA256_DIGEST_SIZE];

    memset(a, 0xA5, sizeof(a));
    memset(b, 0xA5, sizeof(b));

    POUND_CHECK(sha256_equal(a, b));

    // Each byte position in turn, so a comparison that only looked at part of the array
    // is caught.
    for (size_t i = 0U; i < SHA256_DIGEST_SIZE; ++i)
    {
        b[i] ^= 0x01U;
        POUND_CHECK_MSG(!sha256_equal(a, b), "digests differing only in byte %zu compared equal", i);
        b[i] ^= 0x01U;
    }

    // A digest that is all zeroes must not be confused with an absent one, which is why
    // `sha256_equal` takes two pointers and never a null-tolerant third argument.
    memset(b, 0x00, sizeof(b));
    POUND_CHECK(!sha256_equal(a, b));
}

POUND_TEST_SUITE(sha256,
                POUND_TEST_CASE(sha256, the_empty_message_matches_the_specification),
                POUND_TEST_CASE(sha256, a_single_block_message_matches_the_specification),
                POUND_TEST_CASE(sha256, a_message_that_exactly_fits_one_block_matches_the_specification),
                POUND_TEST_CASE(sha256, a_multi_block_message_matches_the_specification),
                POUND_TEST_CASE(sha256, a_million_repeated_bytes_match_the_specification),
                POUND_TEST_CASE(sha256, chunk_boundaries_do_not_change_the_digest),
                POUND_TEST_CASE(sha256, awkward_chunk_sizes_do_not_change_the_digest),
                POUND_TEST_CASE(sha256, an_empty_update_is_a_no_op),
                POUND_TEST_CASE(sha256, a_finalised_context_is_ready_to_hash_again),
                POUND_TEST_CASE(sha256, every_byte_of_a_message_affects_its_digest),
                POUND_TEST_CASE(sha256, hex_output_is_lowercase_and_nul_terminated),
                POUND_TEST_CASE(sha256, digests_compare_by_value))

/*** end of file ***/

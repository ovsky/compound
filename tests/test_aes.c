//! Tests for AES block, CBC, CTR and XTS.
//!
//! The block vectors are FIPS 197's own examples. CBC and CTR are the NIST
//! SP 800-38A examples. XTS is checked against the IEEE 1619 sector-0 vector and
//! against independently computed sectors 1 and 5; the latter matter because the
//! NCA header is six sectors and a sector-number tweak that only works for zero
//! would decrypt exactly one sector of it.

#include "pound_test.h"

#include "crypto/aes.h"
#include <string.h>

/// Decodes a lowercase or uppercase hex string into `out`.
static void
from_hex(const char *POUND_RESTRICT text, uint8_t *POUND_RESTRICT out, const size_t out_size)
{
    for (size_t i = 0U; i < out_size; ++i)
    {
        const char high = text[i * 2U];
        const char low  = text[(i * 2U) + 1U];

        uint8_t high_value = (uint8_t)((high >= 'a') ? (high - 'a' + 10) : (high - 'A' + 10));
        uint8_t low_value  = (uint8_t)((low >= 'a') ? (low - 'a' + 10) : (low - 'A' + 10));

        if ((high >= '0') && (high <= '9'))
        {
            high_value = (uint8_t)(high - '0');
        }

        if ((low >= '0') && (low <= '9'))
        {
            low_value = (uint8_t)(low - '0');
        }

        out[i] = (uint8_t)((high_value << 4U) | low_value);
    }
}

static void
check_block(const char *key_hex, const size_t key_size, const char *plain_hex, const char *cipher_hex)
{
    uint8_t key[POUND_AES_MAX_KEY_SIZE];
    uint8_t plain[POUND_AES_BLOCK_SIZE];
    uint8_t expected[POUND_AES_BLOCK_SIZE];
    uint8_t actual[POUND_AES_BLOCK_SIZE];

    from_hex(key_hex, key, key_size);
    from_hex(plain_hex, plain, sizeof(plain));
    from_hex(cipher_hex, expected, sizeof(expected));

    pound_aes_t aes;
    POUND_REQUIRE(POUND_SUCCESS == pound_aes_init(&aes, key, key_size));

    POUND_REQUIRE(POUND_SUCCESS == pound_aes_encrypt_block(&aes, plain, actual));
    POUND_CHECK(0 == memcmp(actual, expected, sizeof(actual)));

    POUND_REQUIRE(POUND_SUCCESS == pound_aes_decrypt_block(&aes, expected, actual));
    POUND_CHECK(0 == memcmp(actual, plain, sizeof(actual)));
}

POUND_TEST(aes, fips_197_block_vectors_match)
{
    check_block("000102030405060708090a0b0c0d0e0f",
                16U,
                "00112233445566778899aabbccddeeff",
                "69c4e0d86a7b0430d8cdb78070b4c55a");
    check_block("000102030405060708090a0b0c0d0e0f1011121314151617",
                24U,
                "00112233445566778899aabbccddeeff",
                "dda97ca4864cdfe06eaf70a0ec0d7191");
    check_block("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
                32U,
                "00112233445566778899aabbccddeeff",
                "8ea2b7ca516745bfeafc49904b496089");
}

POUND_TEST(aes, cbc_matches_nist_sp_800_38a)
{
    static const char KEY[]  = "2b7e151628aed2a6abf7158809cf4f3c";
    static const char IV[]   = "000102030405060708090a0b0c0d0e0f";
    static const char PLAIN[] = "6bc1bee22e409f96e93d7e117393172a"
                                "ae2d8a571e03ac9c9eb76fac45af8e51"
                                "30c81c46a35ce411e5fbc1191a0a52ef"
                                "f69f2445df4f9b17ad2b417be66c3710";
    static const char CIPHER[] = "7649abac8119b246cee98e9b12e9197d"
                                 "5086cb9b507219ee95db113a917678b2"
                                 "73bed6b8e3c1743b7116e69e22229516"
                                 "3ff1caa1681fac09120eca307586e1a7";

    uint8_t key[16];
    uint8_t iv[POUND_AES_BLOCK_SIZE];
    uint8_t plain[64];
    uint8_t expected[64];
    uint8_t actual[64];

    from_hex(KEY, key, sizeof(key));
    from_hex(IV, iv, sizeof(iv));
    from_hex(PLAIN, plain, sizeof(plain));
    from_hex(CIPHER, expected, sizeof(expected));

    pound_aes_t aes;
    POUND_REQUIRE(POUND_SUCCESS == pound_aes_init(&aes, key, sizeof(key)));
    POUND_REQUIRE(POUND_SUCCESS == pound_aes_cbc_encrypt(&aes, iv, plain, actual, sizeof(actual)));
    POUND_CHECK(0 == memcmp(actual, expected, sizeof(actual)));
    POUND_REQUIRE(POUND_SUCCESS == pound_aes_cbc_decrypt(&aes, iv, expected, actual, sizeof(actual)));
    POUND_CHECK(0 == memcmp(actual, plain, sizeof(actual)));
}

POUND_TEST(aes, ctr_matches_nist_sp_800_38a)
{
    static const char KEY[] = "2b7e151628aed2a6abf7158809cf4f3c";
    static const char COUNTER[] = "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff";
    static const char PLAIN[] = "6bc1bee22e409f96e93d7e117393172a"
                                "ae2d8a571e03ac9c9eb76fac45af8e51"
                                "30c81c46a35ce411e5fbc1191a0a52ef"
                                "f69f2445df4f9b17ad2b417be66c3710";
    static const char CIPHER[] = "874d6191b620e3261bef6864990db6ce"
                                 "9806f66b7970fdff8617187bb9fffdff"
                                 "5ae4df3edbd5d35e5b4f09020db03eab"
                                 "1e031dda2fbe03d1792170a0f3009cee";

    uint8_t key[16];
    uint8_t counter[POUND_AES_BLOCK_SIZE];
    uint8_t plain[64];
    uint8_t expected[64];
    uint8_t actual[64];

    from_hex(KEY, key, sizeof(key));
    from_hex(COUNTER, counter, sizeof(counter));
    from_hex(PLAIN, plain, sizeof(plain));
    from_hex(CIPHER, expected, sizeof(expected));

    pound_aes_t aes;
    POUND_REQUIRE(POUND_SUCCESS == pound_aes_init(&aes, key, sizeof(key)));
    POUND_REQUIRE(POUND_SUCCESS == pound_aes_ctr_xcrypt(&aes, counter, plain, actual, sizeof(actual)));
    POUND_CHECK(0 == memcmp(actual, expected, sizeof(actual)));
    POUND_REQUIRE(POUND_SUCCESS == pound_aes_ctr_xcrypt(&aes, counter, expected, actual, sizeof(actual)));
    POUND_CHECK(0 == memcmp(actual, plain, sizeof(actual)));
}

/// Encrypts the IEEE 1619 plaintext with the data unit `sector` and checks the
/// first and last blocks. The complete 512-byte ciphertext is deliberately not
/// copied into the test: two blocks at each end catch a wrong data unit, a wrong
/// tweak direction and a wrong block chaining, without a wall of hex.
static void
check_xts_sector(const uint8_t sector, const char *first_hex, const char *last_hex)
{
    uint8_t key[32];
    uint8_t tweak[POUND_AES_BLOCK_SIZE] = {0};
    uint8_t plain[512];
    uint8_t cipher[512];
    uint8_t expected_first[POUND_AES_BLOCK_SIZE];
    uint8_t expected_last[POUND_AES_BLOCK_SIZE];

    from_hex("2718281828459045235360287471352631415926535897932384626433832795", key, sizeof(key));
    from_hex(first_hex, expected_first, sizeof(expected_first));
    from_hex(last_hex, expected_last, sizeof(expected_last));

    for (size_t i = 0U; i < sizeof(plain); ++i)
    {
        plain[i] = (uint8_t)i;
    }

    tweak[15] = sector;

    POUND_REQUIRE(POUND_SUCCESS == pound_aes_xts_encrypt(key,
                                                          sizeof(key),
                                                          tweak,
                                                          plain,
                                                          cipher,
                                                          sizeof(cipher),
                                                          sizeof(cipher)));
    POUND_CHECK(0 == memcmp(cipher, expected_first, sizeof(expected_first)));
    POUND_CHECK(0 == memcmp(cipher + sizeof(cipher) - sizeof(expected_last),
                            expected_last,
                            sizeof(expected_last)));

    uint8_t recovered[512];
    POUND_REQUIRE(POUND_SUCCESS == pound_aes_xts_decrypt(key,
                                                          sizeof(key),
                                                          tweak,
                                                          cipher,
                                                          recovered,
                                                          sizeof(recovered),
                                                          sizeof(recovered)));
    POUND_CHECK(0 == memcmp(recovered, plain, sizeof(plain)));
}

POUND_TEST(aes, xts_matches_the_ieee_1619_vectors)
{
    check_xts_sector(0U,
                     "27a7479befa1d476489f308cd4cfa6e2a96e4bbe3208ff25287dd3819616e89c",
                     "eb4a427d1923ce3ff262735779a418f20a282df920147beabe421ee5319d0568");
    check_xts_sector(1U,
                     "9379af57714c3d90e0c280923d5147dadc97e13ca72d6fd26a787183bba3bd0d",
                     "ff45fa58cbfbd29da73281904102890a8dc496f9bf469a98c3a40dd8a9e01402");
    check_xts_sector(5U,
                     "a2677776c563ed375d42f5cd7dba3811bac1d47e7a9a0867f41a48f84a6c9d80",
                     "de999f6fc47e69a7aa2c7b6274a84d50375e54e18676309cd78042685674f994");
}

POUND_TEST(aes, malformed_lengths_are_refused)
{
    uint8_t key[POUND_AES_MAX_KEY_SIZE] = {0};
    uint8_t block[POUND_AES_BLOCK_SIZE] = {0};
    uint8_t output[POUND_AES_BLOCK_SIZE] = {0};
    uint8_t counter[POUND_AES_BLOCK_SIZE] = {0};
    pound_aes_t aes;

    POUND_CHECK_EQ_I64(pound_aes_init(&aes, key, 15U), POUND_ERROR_UNSUPPORTED_FEATURE);
    POUND_CHECK_EQ_I64(pound_aes_init(&aes, key, 64U), POUND_ERROR_UNSUPPORTED_FEATURE);

    POUND_REQUIRE(POUND_SUCCESS == pound_aes_init(&aes, key, 16U));
    POUND_CHECK_EQ_I64(pound_aes_cbc_encrypt(&aes, block, block, output, 17U),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(pound_aes_ctr_xcrypt(&aes, counter, block, output, 0U),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(pound_aes_xts_encrypt(key, 16U, block, block, output, 16U, 16U),
                       POUND_ERROR_UNSUPPORTED_FEATURE);
    POUND_CHECK_EQ_I64(pound_aes_xts_encrypt(key, 32U, block, block, output, 16U, 512U),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(pound_aes_xts_encrypt(key, 32U, block, block, output, 32U, 16U),
                       POUND_ERROR_INVALID_ARGUMENT);
}

POUND_TEST_SUITE(aes,
                 POUND_TEST_CASE(aes, fips_197_block_vectors_match),
                 POUND_TEST_CASE(aes, cbc_matches_nist_sp_800_38a),
                 POUND_TEST_CASE(aes, ctr_matches_nist_sp_800_38a),
                 POUND_TEST_CASE(aes, xts_matches_the_ieee_1619_vectors),
                 POUND_TEST_CASE(aes, malformed_lengths_are_refused))

/*** end of file ***/

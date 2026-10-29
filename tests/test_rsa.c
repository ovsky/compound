//! Unit tests for RSA-2048 PKCS#1 v1.5 SHA-256 verification.
//!
//! The vector in `rsa_test_vector.h` is a throwaway key generated with the host
//! platform's RSA implementation. The private half was discarded, so the test
//! proves the verifier against an independent signer rather than against a
//! round-trip through Compound's own arithmetic.

#include "pound_test.h"

#include "crypto/rsa.h"
#include "crypto/sha256.h"
#include "rsa_test_vector.h"

#include <string.h>

static const char test_message[] = "compound-signature-test";

static void
make_test_key(pound_rsa2048_public_key_t *key)
{
    memcpy(key->modulus, rsa_test_modulus, sizeof(key->modulus));
    key->exponent[0] = 0x00U;
    key->exponent[1] = 0x01U;
    key->exponent[2] = 0x00U;
    key->exponent[3] = 0x01U;
}

POUND_TEST(rsa, verifies_a_known_pkcs1_v1_5_sha256_signature)
{
    pound_rsa2048_public_key_t key;

    make_test_key(&key);

    POUND_CHECK_EQ_I64(pound_rsa2048_sha256_verify_message(&key,
                                                           rsa_test_signature,
                                                           test_message,
                                                           sizeof(test_message) - 1U),
                       POUND_SUCCESS);
}

POUND_TEST(rsa, verifies_a_precomputed_digest)
{
    pound_rsa2048_public_key_t key;
    uint8_t                    digest[SHA256_DIGEST_SIZE];

    make_test_key(&key);
    sha256(test_message, sizeof(test_message) - 1U, digest);

    POUND_CHECK_EQ_I64(pound_rsa2048_sha256_verify(&key, rsa_test_signature, digest),
                       POUND_SUCCESS);
}

POUND_TEST(rsa, rejects_a_message_that_was_not_signed)
{
    pound_rsa2048_public_key_t key;

    make_test_key(&key);

    POUND_CHECK_EQ_I64(pound_rsa2048_sha256_verify_message(&key,
                                                           rsa_test_signature,
                                                           "compound-signature-tesu",
                                                           sizeof(test_message) - 1U),
                       POUND_ERROR_SIGNATURE_INVALID);
}

POUND_TEST(rsa, rejects_a_tampered_signature)
{
    pound_rsa2048_public_key_t key;
    uint8_t                    signature[POUND_RSA2048_BYTES];

    make_test_key(&key);
    memcpy(signature, rsa_test_signature, sizeof(signature));
    signature[POUND_RSA2048_BYTES / 2U] ^= 0x01U;

    POUND_CHECK_EQ_I64(pound_rsa2048_sha256_verify_message(&key,
                                                           signature,
                                                           test_message,
                                                           sizeof(test_message) - 1U),
                       POUND_ERROR_SIGNATURE_INVALID);
}

POUND_TEST(rsa, rejects_a_signature_equal_to_the_modulus)
{
    pound_rsa2048_public_key_t key;

    make_test_key(&key);

    POUND_CHECK_EQ_I64(pound_rsa2048_sha256_verify_message(&key,
                                                           key.modulus,
                                                           test_message,
                                                           sizeof(test_message) - 1U),
                       POUND_ERROR_SIGNATURE_INVALID);
}

POUND_TEST(rsa, rejects_an_all_zero_signature)
{
    pound_rsa2048_public_key_t key;
    uint8_t                    signature[POUND_RSA2048_BYTES] = { 0U };

    make_test_key(&key);

    POUND_CHECK_EQ_I64(pound_rsa2048_sha256_verify_message(&key,
                                                           signature,
                                                           test_message,
                                                           sizeof(test_message) - 1U),
                       POUND_ERROR_SIGNATURE_INVALID);
}

POUND_TEST(rsa, rejects_a_null_key_signature_or_digest)
{
    pound_rsa2048_public_key_t key;
    uint8_t                    digest[SHA256_DIGEST_SIZE] = { 0U };

    make_test_key(&key);

    POUND_CHECK_EQ_I64(pound_rsa2048_sha256_verify(NULL, rsa_test_signature, digest),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(pound_rsa2048_sha256_verify(&key, NULL, digest),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(pound_rsa2048_sha256_verify(&key, rsa_test_signature, NULL),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(pound_rsa2048_sha256_verify_message(&key,
                                                           rsa_test_signature,
                                                           NULL,
                                                           1U),
                       POUND_ERROR_INVALID_ARGUMENT);
}

POUND_TEST(rsa, rejects_invalid_public_keys_as_key_errors)
{
    pound_rsa2048_public_key_t key;
    uint8_t                    digest[SHA256_DIGEST_SIZE] = { 0U };

    make_test_key(&key);

    // Even modulus: not a valid RSA modulus.
    key.modulus[POUND_RSA2048_BYTES - 1U] &= 0xFEU;
    POUND_CHECK(false == pound_rsa2048_public_key_is_valid(&key));
    POUND_CHECK_EQ_I64(pound_rsa2048_sha256_verify(&key, rsa_test_signature, digest),
                       POUND_ERROR_INVALID_ARGUMENT);

    // 2047-bit modulus: the top bit must be set for a 2048-bit key.
    make_test_key(&key);
    key.modulus[0] &= 0x7FU;
    POUND_CHECK(false == pound_rsa2048_public_key_is_valid(&key));

    // Zero exponent.
    make_test_key(&key);
    memset(key.exponent, 0, sizeof(key.exponent));
    POUND_CHECK(false == pound_rsa2048_public_key_is_valid(&key));

    // Even exponent: RSA public exponents are odd.
    make_test_key(&key);
    key.exponent[3] = 0x02U;
    POUND_CHECK(false == pound_rsa2048_public_key_is_valid(&key));

    POUND_CHECK(false == pound_rsa2048_public_key_is_valid(NULL));
}

POUND_TEST(rsa, accepts_a_well_formed_key)
{
    pound_rsa2048_public_key_t key;

    make_test_key(&key);
    POUND_CHECK(pound_rsa2048_public_key_is_valid(&key));
}

POUND_TEST_SUITE(rsa,
                 POUND_TEST_CASE(rsa, verifies_a_known_pkcs1_v1_5_sha256_signature),
                 POUND_TEST_CASE(rsa, verifies_a_precomputed_digest),
                 POUND_TEST_CASE(rsa, rejects_a_message_that_was_not_signed),
                 POUND_TEST_CASE(rsa, rejects_a_tampered_signature),
                 POUND_TEST_CASE(rsa, rejects_a_signature_equal_to_the_modulus),
                 POUND_TEST_CASE(rsa, rejects_an_all_zero_signature),
                 POUND_TEST_CASE(rsa, rejects_a_null_key_signature_or_digest),
                 POUND_TEST_CASE(rsa, rejects_invalid_public_keys_as_key_errors),
                 POUND_TEST_CASE(rsa, accepts_a_well_formed_key));

/*** end of file ***/

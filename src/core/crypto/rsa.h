#ifndef POUND_CRYPTO_RSA_H
#define POUND_CRYPTO_RSA_H

//! RSA-2048 signature verification, PKCS#1 v1.5, SHA-256.
//!
//! ## Why this exists
//!
//! The Switch family authenticates its containers with RSA-2048. An XCI, NCA,
//! NSP and ticket all carry a 256-byte signature over a header, and the console
//! verifies that signature with a public key baked into firmware before it will
//! use the contents. Compound cannot load a commercial title honestly without
//! the same primitive: a parser that accepts any 256 bytes because "the hash was
//! checked elsewhere" is authenticating nothing.
//!
//! This module implements the verification direction only. It takes a public
//! key, a signature, and either a digest or a message, and answers whether the
//! signature is a valid PKCS#1 v1.5 encoding of that digest. It never signs,
//! never generates keys, and never handles private material. That asymmetry is
//! deliberate: verification is what a loader needs, and a signing routine in an
//! emulator would be a liability with no legitimate caller.
//!
//! ## What it does not do
//!
//! - It does not provide, ship, or guess Nintendo's public keys. The caller
//!   supplies a key; if none is supplied, there is nothing to verify and the
//!   loader refuses rather than pretending. See `fs/keys.h` for the same rule on
//!   the decryption side.
//! - It does not parse certificates or key blobs. A caller that has a DER
//!   SubjectPublicKeyInfo must extract the modulus and exponent itself; the
//!   conversion is format-specific and belongs with the format parser.
//! - It does not support RSA-PSS. The formats Compound targets use PKCS#1 v1.5,
//!   and implementing PSS as well would double the surface for no current
//!   caller.
//! - It does not attempt to be constant-time. The operation only involves public
//!   values, so timing leaks the key that the caller already has.
//!
//! ## The arithmetic
//!
//! The modulus is 2048 bits, which is 64 32-bit limbs. The implementation uses
//! Montgomery multiplication in `R = 2^2048`, with 32-bit limbs so that every
//! intermediate fits in a 64-bit `uint64_t` on all supported hosts. That avoids
//! a compiler-specific `__int128` dependency and keeps the code reviewable
//! against the textbook algorithm. The public exponent is accepted as a 32-bit
//! big-endian field; the usual value is 65537.

#include "attributes.h"
#include "errors.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/// Size of an RSA-2048 modulus and signature, in bytes.
#define POUND_RSA2048_BYTES 256U

/// Size of the SHA-256 digest that PKCS#1 v1.5 signs, in bytes.
#define POUND_RSA_SHA256_DIGEST_BYTES 32U

/// A raw RSA-2048 public key in big-endian wire form.
///
/// This is exactly the representation a container header or a key file already
/// uses, so a caller can copy the modulus and exponent into it without a
/// byte-order conversion. `exponent` is four bytes because that is the width of
/// the usual 65537 and of every exponent the formats use; a shorter value is
/// left-padded with zeroes.
typedef struct
{
    uint8_t modulus[POUND_RSA2048_BYTES];
    uint8_t exponent[4];
} pound_rsa2048_public_key_t;

/// Verifies `signature` over a precomputed SHA-256 `digest`.
///
/// The caller hashes the signed region itself, which is what a container parser
/// needs: it may have streamed the region through `sha256_update` rather than
/// holding it contiguously.
///
/// A legitimate PKCS#1 v1.5 signature is always accepted. A malformed key
/// (even modulus, wrong high bit, zero exponent) is refused as
/// `POUND_ERROR_INVALID_ARGUMENT` rather than treated as a bad signature,
/// because retrying with the same key cannot help. A signature that is
/// numerically out of range, has the wrong padding, or encodes a different
/// digest is `POUND_ERROR_SIGNATURE_INVALID`.
error_t pound_rsa2048_sha256_verify(const pound_rsa2048_public_key_t *POUND_RESTRICT key,
                                    const uint8_t signature[POUND_RSA2048_BYTES],
                                    const uint8_t digest[POUND_RSA_SHA256_DIGEST_BYTES]);

/// Verifies `signature` over `message` after hashing it with SHA-256.
///
/// Equivalent to hashing `message` and calling `pound_rsa2048_sha256_verify`.
/// `message` may be NULL only when `length` is zero.
error_t pound_rsa2048_sha256_verify_message(
    const pound_rsa2048_public_key_t *POUND_RESTRICT key,
    const uint8_t signature[POUND_RSA2048_BYTES],
    const void *POUND_RESTRICT message,
    size_t                     length);

/// Reports whether `key` describes a usable RSA-2048 public key.
///
/// Checks that the modulus is exactly 2048 bits and odd, and that the exponent
/// is odd and at least three. The exponent is not required to be 65537; a caller
/// with a valid but unusual key should be able to use it.
bool pound_rsa2048_public_key_is_valid(const pound_rsa2048_public_key_t *POUND_RESTRICT key);

#ifdef __cplusplus
}
#endif

#endif // POUND_CRYPTO_RSA_H

/*** end of file ***/

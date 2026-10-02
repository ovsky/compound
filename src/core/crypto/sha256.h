#ifndef POUND_CRYPTO_SHA256_H
#define POUND_CRYPTO_SHA256_H

//! SHA-256, as specified in FIPS 180-4.
//!
//! This exists because every container format Pound reads authenticates itself with
//! SHA-256: an XCI records the SHA-256 of its header, an NCA header records the
//! SHA-256 of the PFS0 it wraps, and a PFS0 file entry records the SHA-256 of the file
//! it points at. Verifying those hashes is the only structural integrity check
//! available for a user-supplied image, so this is load-bearing for loading rather
//! than a diagnostic nicety.
//!
//! It is *not* a general-purpose crypto library and does not pretend to be one. There
//! is no SHA-1, no SHA-3, no HMAC, and no key handling here. Decryption lives behind
//! the key provider in `fs/keys.h`, which never contains key material of its own.
//!
//! The implementation is deliberately plain. `sha256_update` is called a handful of
//! times per image, not once per byte of guest memory, so clarity and obvious
//! conformance to the spec beat a hand-optimised inner loop, and the code is written
//! to be checked against the specification line by line. In particular the input is
//! assembled with explicit byte shifts rather than by punning a host word, so the
//! result is identical on a little-endian and a big-endian host and cannot be broken
//! by a strict-aliasing assumption.
//!
//! ## A note on what this is not
//!
//! SHA-256 is a hash, not a MAC and not a cipher. It provides integrity, not
//! authenticity, and it provides neither confidentiality nor a way to detect a file
//! that was deliberately modified and then re-signed by someone who knew the format.
//! Pound uses it because the formats specify it, not because it makes an image
//! trustworthy.

#include "attributes.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Size of a SHA-256 digest, in bytes.
#define SHA256_DIGEST_SIZE 32U

/// SHA-256's internal block size, in bytes. The compression function cannot be
/// called with any other amount of input, and the padding rule is expressed in terms
/// of it, so it is part of the interface rather than an implementation detail.
#define SHA256_BLOCK_SIZE 64U

/// Number of characters in the lowercase hex form of a digest, excluding the
/// terminating NUL.
#define SHA256_HEX_SIZE 64U

/// A streaming SHA-256 context.
///
/// Safe to reuse after `sha256_final`, which resets it to the initial state, so a
/// context that has been stacked or pooled does not need reinitialising between
/// images. The context holds no pointers to its input, so a digest may be taken over
/// data that was freed the moment `sha256_update` returned.
typedef struct
{
    /// The eight chaining words, updated in place by every compression.
    uint32_t state[8];

    /// Total message length in bytes, counted across every `sha256_update` call.
    ///
    /// FIPS 180-4 encodes the padded message's length in *bits*, modulo 2^64. That
    /// means a message of 2^61 bytes or more would hash as a shorter one. No
    /// filesystem that can hold a game image comes close, so the modular behaviour is
    /// left as the specification defines it rather than being special-cased.
    uint64_t total_bytes;

    /// Bytes of a partial block held back from the last compression, always less
    /// than `SHA256_BLOCK_SIZE`.
    size_t buffered;

    /// The partial block. Sized to a full block so that padding, which needs up to a
    /// block and a half of contiguity, never has to allocate.
    uint8_t buffer[SHA256_BLOCK_SIZE];
} sha256_t;

/// Puts `context` into the initial state, ready to accept a message.
void sha256_init(sha256_t *POUND_RESTRICT context);

/// Absorbs `size` bytes of `data` into `context`.
///
/// `data` may be NULL only when `size` is zero, which is permitted and does nothing.
/// Passing NULL with a non-zero size is a caller bug and is rejected by the standard
/// library's behaviour inside `memcpy`; Pound does not silently accept it.
void sha256_update(sha256_t *POUND_RESTRICT context, const void *POUND_RESTRICT data, size_t size);

/// Finishes the message and writes the digest to `out`.
///
/// The length padding is applied here, so a message must not be padded by the
/// caller. `context` is left in the initial state, so it can be reused immediately.
void sha256_final(sha256_t *POUND_RESTRICT context, uint8_t out[SHA256_DIGEST_SIZE]);

/// Computes the digest of a single buffer, without exposing the streaming interface.
///
/// Equivalent to `init`, one `update` and `final`, and provided because the
/// overwhelming majority of call sites hash one contiguous range: the contents of a
/// container header, or one file's worth of a partition.
void sha256(const void *POUND_RESTRICT data, size_t size, uint8_t out[SHA256_DIGEST_SIZE]);

/// Writes the lowercase hex form of `digest` to `out`, with a terminating NUL.
///
/// Requires `out` to hold at least `SHA256_HEX_SIZE + 1` bytes. Exists so that
/// hashes can be logged and compared as text without every call site open-coding the
/// same loop and getting the byte order subtly different.
void sha256_to_hex(const uint8_t digest[SHA256_DIGEST_SIZE], char out[SHA256_HEX_SIZE + 1U]);

/// Compares two digests without an early exit.
///
/// A `memcmp` that returns as soon as it finds a difference leaks, through timing,
/// how much of a hash a caller guessed correctly. That is not a threat here, but the
/// constant-time form costs three lines and removes the question, and a hash
/// comparison is exactly the place where a future reader would expect it.
bool sha256_equal(const uint8_t *POUND_RESTRICT a, const uint8_t *POUND_RESTRICT b);

#endif // POUND_CRYPTO_SHA256_H

/*** end of file ***/

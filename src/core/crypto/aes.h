#ifndef POUND_CRYPTO_AES_H
#define POUND_CRYPTO_AES_H

//! AES block, mode and XTS primitives.
//!
//! This is the cryptography the package parser needs and nothing beyond it. It
//! implements AES-128, AES-192 and AES-256 from FIPS-197, the three NIST modes a
//! Switch container uses (ECB for tickets and key areas, CBC for the older
//! header scheme, CTR for NCA sections), and IEEE 1619 XTS for NCA headers.
//!
//! No key is stored here, no key is generated here, and no fallback key exists.
//! Every entry point takes a caller-owned key and every length is explicit. A
//! malformed length is refused before a block is touched rather than being
//! rounded, padded or processed partially: a half-decrypted NCA header is not a
//! header, and treating it as one is how a loader starts reading noise.
//!
//! ## XTS and the Nintendo tweak
//!
//! IEEE 1619 XTS is defined over little-endian 128-bit data units, and the
//! multiplication by x is the little-endian `gf128mul_x_ble`. Nintendo's NCA
//! code hands hactool a data unit whose low eight bytes are a big-endian sector
//! number. That is not the IEEE convention, but it is the one the console uses,
//! so `pound_aes_xts_crypt` treats its `data_unit` argument as an opaque
//! sixteen-byte value and callers construct it in the format they need. The NCA
//! parser constructs the Nintendo form and says so where it does.

#include "attributes.h"
#include "errors.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// AES block size in bytes. Every mode here is defined over these blocks.
#define POUND_AES_BLOCK_SIZE 16U

/// Largest AES key, in bytes (AES-256).
#define POUND_AES_MAX_KEY_SIZE 32U

/// Size of the expanded round-key schedule for AES-256, in bytes.
#define POUND_AES_MAX_EXPANDED_KEY_SIZE 240U

/// An initialized AES key schedule.
///
/// Both directions are expanded once, at initialization, so the hot loop of CTR
/// and XTS decryption does not replay the key schedule per block. The struct is
/// plain data with no allocation and can be copied, but a copy is only useful to
/// a single thread; the schedule is read-only after initialization.
typedef struct
{
    uint8_t round_keys[POUND_AES_MAX_EXPANDED_KEY_SIZE];
    uint8_t inverse_round_keys[POUND_AES_MAX_EXPANDED_KEY_SIZE];
    uint8_t rounds;
    uint8_t key_size;

    /// Explicit tail padding, so the struct's size does not change when a
    /// compiler chooses a different default for adjacent members.
    uint8_t pad[2];
} pound_aes_t;

/// Initializes `aes` from `key`.
///
/// `key_size` must be 16, 24 or 32. Returns `POUND_ERROR_INVALID_ARGUMENT` for a
/// NULL pointer, or `POUND_ERROR_UNSUPPORTED_FEATURE` for a key size AES does
/// not define.
error_t pound_aes_init(pound_aes_t *POUND_RESTRICT aes, const uint8_t *POUND_RESTRICT key,
                       const size_t key_size);

/// Encrypts or decrypts one 16-byte block in place-safe fashion.
///
/// `input` and `output` may be the same buffer. The context must have been
/// initialized by `pound_aes_init`.
error_t pound_aes_encrypt_block(const pound_aes_t *POUND_RESTRICT aes, const uint8_t input[POUND_AES_BLOCK_SIZE],
                                uint8_t output[POUND_AES_BLOCK_SIZE]);
error_t pound_aes_decrypt_block(const pound_aes_t *POUND_RESTRICT aes, const uint8_t input[POUND_AES_BLOCK_SIZE],
                                uint8_t output[POUND_AES_BLOCK_SIZE]);

/// Applies AES-ECB to `size` bytes in place.
///
/// `size` must be a non-zero multiple of 16. ECB is used only where the format
/// defines independent key blocks; it is never used here to encrypt a message of
/// more than one block except for the small fixed-size ticket and key-area
/// records.
error_t pound_aes_ecb_encrypt(const pound_aes_t *POUND_RESTRICT aes, const uint8_t *POUND_RESTRICT input,
                              uint8_t *POUND_RESTRICT output, const size_t size);
error_t pound_aes_ecb_decrypt(const pound_aes_t *POUND_RESTRICT aes, const uint8_t *POUND_RESTRICT input,
                              uint8_t *POUND_RESTRICT output, const size_t size);

/// Applies AES-CBC with the supplied 16-byte IV.
///
/// `size` must be a non-zero multiple of 16. The IV is copied, not advanced in
/// the caller's buffer; a caller that chains calls updates its own IV from the
/// last ciphertext block, as CBC defines.
error_t pound_aes_cbc_encrypt(const pound_aes_t *POUND_RESTRICT aes, const uint8_t iv[POUND_AES_BLOCK_SIZE],
                              const uint8_t *POUND_RESTRICT input, uint8_t *POUND_RESTRICT output, const size_t size);
error_t pound_aes_cbc_decrypt(const pound_aes_t *POUND_RESTRICT aes, const uint8_t iv[POUND_AES_BLOCK_SIZE],
                              const uint8_t *POUND_RESTRICT input, uint8_t *POUND_RESTRICT output, const size_t size);

/// Applies AES-CTR with a 16-byte initial counter block.
///
/// The counter is incremented as a 128-bit big-endian integer after every block.
/// `input` and `output` may be the same buffer. `size` may be any non-zero value;
/// a trailing partial block is handled exactly, as CTR is a stream mode.
error_t pound_aes_ctr_xcrypt(const pound_aes_t *POUND_RESTRICT aes, const uint8_t counter[POUND_AES_BLOCK_SIZE],
                             const uint8_t *POUND_RESTRICT input, uint8_t *POUND_RESTRICT output, const size_t size);

/// Applies AES-XTS to `size` bytes with an explicit 16-byte data unit.
///
/// `key_size` is the total key length: 32 for AES-128-XTS or 64 for AES-256-XTS.
/// The first half is the data key and the second half the tweak key. `tweak` is
/// the data unit exactly as the format defines it; see the file comment about
/// Nintendo's big-endian sector construction.
///
/// `sector_size` must be a power of two and at least 16, and `size` must be a
/// non-zero multiple of it. IEEE 1619 permits ciphertext stealing for a partial
/// final block; no Nintendo structure this build reads uses it, so it is refused
/// rather than implemented incompletely. `input` and `output` may alias.
error_t pound_aes_xts_encrypt(const uint8_t *POUND_RESTRICT key, const size_t key_size,
                              const uint8_t tweak[POUND_AES_BLOCK_SIZE], const uint8_t *POUND_RESTRICT input,
                              uint8_t *POUND_RESTRICT output, const size_t size, const size_t sector_size);
error_t pound_aes_xts_decrypt(const uint8_t *POUND_RESTRICT key, const size_t key_size,
                              const uint8_t tweak[POUND_AES_BLOCK_SIZE], const uint8_t *POUND_RESTRICT input,
                              uint8_t *POUND_RESTRICT output, const size_t size, const size_t sector_size);

#ifdef __cplusplus
}
#endif

#endif // POUND_CRYPTO_AES_H

/*** end of file ***/

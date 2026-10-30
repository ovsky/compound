#ifndef POUND_COMPRESSION_LZ4_H
#define POUND_COMPRESSION_LZ4_H

//! The raw LZ4 block format, which is what an NSO segment stores.
//!
//! NSO does not contain an LZ4 *frame*. It contains the block payload with no
//! length prefix, no checksum and no end mark: the segment's stored size in the
//! NSO header is the only boundary, and the uncompressed size is the only
//! expected output length. That makes bounds checking the whole job. A truncated
//! block, an offset that points before the output, or a match that would write
//! past the segment is a malformed image, not something to clamp or pad.
//!
//! The decoder is deliberately a single-shot function. It allocates nothing,
//! keeps no state, and refuses overlapping input and output rather than trying
//! to make an in-place LZ4 decode work on buffers that a caller might alias.

#include "attributes.h"
#include "errors.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Decompresses one raw LZ4 block.
///
/// Writes exactly `output_capacity` bytes on success and stores that same value in
/// `*output_size`. The NSO header gives the exact uncompressed length, so the
/// caller should pass it as `output_capacity` rather than a guess: a block that
/// decodes to fewer bytes is refused, because an NSO segment shorter than its
/// header claims is malformed and must not be mapped with uninitialised tail bytes.
///
/// Returns:
/// - `POUND_SUCCESS` when the block decoded exactly to `output_capacity` bytes.
/// - `POUND_ERROR_INVALID_ARGUMENT` for a NULL pointer, a zero length, or
///   overlapping input and output.
/// - `POUND_ERROR_CORRUPTED` for a malformed block or an output length mismatch.
/// - `POUND_ERROR_BUFFER_TOO_SMALL` when the block would write past the output.
error_t pound_lz4_decompress_block(const uint8_t *POUND_RESTRICT input, size_t input_size,
                                   uint8_t *POUND_RESTRICT output, size_t output_capacity,
                                   size_t *POUND_RESTRICT output_size);

#ifdef __cplusplus
}
#endif

#endif // POUND_COMPRESSION_LZ4_H

/*** end of file ***/

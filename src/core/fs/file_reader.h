#ifndef POUND_FS_FILE_READER_H
#define POUND_FS_FILE_READER_H

//! A file-backed `fs_reader_t`.
//!
//! The parsers are written against `fs_reader_t` so they can be tested on a
//! buffer and then used on a real image without a second code path. This module
//! is the bridge to the other direction: it opens a host file, reports its size,
//! and installs a seek-and-read callback that obeys the same random-access
//! contract as `fs_reader_from_buffer`.
//!
//! The file handle is owned by the caller and stays open for as long as the
//! reader is used. `fs_file_close` is the only function that closes it; a
//! reader made by `fs_file_reader` never closes or frees the file, exactly as a
//! buffer reader never frees its buffer.
//!
//! 64-bit offsets are used on every platform. Windows' `long` is 32 bits, so a
//! plain `fseek` would silently truncate an offset past 2 GiB -- the point at
//! which a real game image starts having interesting data.

#include "attributes.h"
#include "errors.h"
#include "fs/fs_reader.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    /// Opaque `FILE *`; NULL means closed.
    void *handle;

    /// Size captured when the file was opened, in bytes.
    uint64_t size;

    uint8_t pad[8];
} fs_file_t;

/// Opens `path` for binary reading and records its size.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` for a NULL argument,
/// `POUND_ERROR_NOT_FOUND` when the file does not exist, and
/// `POUND_ERROR_IO` when it exists but cannot be sized or read.
error_t fs_file_open(fs_file_t *POUND_RESTRICT file, const char *POUND_RESTRICT path);

/// Closes a file opened by `fs_file_open` and zeroes `file`.
///
/// Closing a closed or zeroed file is harmless.
void fs_file_close(fs_file_t *POUND_RESTRICT file);

/// Installs a reader over `file`.
///
/// The reader borrows the file; `file` must stay open and must outlive every
/// read through `reader`. Returns `POUND_ERROR_NOT_INITIALIZED` for a closed
/// file, and `POUND_ERROR_INVALID_ARGUMENT` for a NULL argument.
error_t fs_file_reader(fs_file_t *POUND_RESTRICT file, fs_reader_t *POUND_RESTRICT reader);

/// Whether `file` currently owns an open handle.
bool fs_file_is_open(const fs_file_t *POUND_RESTRICT file);

#ifdef __cplusplus
}
#endif

#endif // POUND_FS_FILE_READER_H

/*** end of file ***/

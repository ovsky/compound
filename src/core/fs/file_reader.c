// Must be defined before the first system header so `off_t` is 64-bit on hosts
// whose default ABI still spells it as a 32-bit type.
#if !defined(_WIN32) && !defined(_FILE_OFFSET_BITS)
#define _FILE_OFFSET_BITS 64
#endif

#include "fs/file_reader.h"

#include "log.h"
#include "platform.h"

#include <stdio.h>
#include <string.h>

/// Windows spells the 64-bit seek and tell operations with a leading underscore;
/// POSIX spells them `fseeko`/`ftello` once large-file offsets are enabled.
#if POUND_PLATFORM_WINDOWS
#define POUND_FSEEK64(stream, offset, origin) _fseeki64((stream), (offset), (origin))
#define POUND_FTELL64(stream)                 _ftelli64((stream))
#else
#define POUND_FSEEK64(stream, offset, origin) fseeko((stream), (off_t)(offset), (origin))
#define POUND_FTELL64(stream)                 ftello((stream))
#endif

/// The `fs_reader_t::read_at` callback for a file-backed reader.
///
/// `context` is the `fs_file_t` installed by `fs_file_reader`. The reader has
/// already checked the range against `reader->size`, but the check is repeated
/// here because this callback can also be reached through a copied reader whose
/// size was changed by its owner.
static error_t
file_read_at(void *POUND_RESTRICT context, const uint64_t offset, void *POUND_RESTRICT destination,
             const size_t size)
{
    fs_file_t *POUND_RESTRICT file = (fs_file_t *)context;

    if (POUND_UNLIKELY(NULL == file) || POUND_UNLIKELY(NULL == file->handle))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting read: the file reader is closed.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    if (0U == size)
    {
        return POUND_SUCCESS;
    }

    if (POUND_UNLIKELY(NULL == destination))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting read: a %zu-byte read was given no destination.", size);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY((offset > file->size) || ((uint64_t)size > (file->size - offset))))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting read: [%llu, %llu) is outside a %llu-byte file.",
                        (unsigned long long)offset,
                        (unsigned long long)(offset + (uint64_t)size),
                        (unsigned long long)file->size);
        return POUND_ERROR_IO;
    }

    FILE *stream = (FILE *)file->handle;

    if (0 != POUND_FSEEK64(stream, (int64_t)offset, SEEK_SET))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting read: seeking to %llu failed.", (unsigned long long)offset);
        return POUND_ERROR_IO;
    }

    const size_t read = fread(destination, 1U, size, stream);
    if (read != size)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting read: wanted %zu bytes at %llu, read %zu.",
                        size,
                        (unsigned long long)offset,
                        read);
        return POUND_ERROR_IO;
    }

    return POUND_SUCCESS;
}

error_t
fs_file_open(fs_file_t *POUND_RESTRICT file, const char *POUND_RESTRICT path)
{
    if (POUND_UNLIKELY(NULL == file) || POUND_UNLIKELY(NULL == path) || POUND_UNLIKELY('\0' == path[0]))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting file open: file=%p, path=%p.", (const void *)file,
                        (const void *)path);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    memset(file, 0, sizeof(*file));

    FILE *stream = fopen(path, "rb");
    if (NULL == stream)
    {
        POUND_LOG_ERROR(&thread_logger, "Opening %s failed.", path);
        return POUND_ERROR_NOT_FOUND;
    }

    if (0 != POUND_FSEEK64(stream, 0, SEEK_END))
    {
        POUND_LOG_ERROR(&thread_logger, "Sizing %s failed: the end could not be located.", path);
        (void)fclose(stream);
        return POUND_ERROR_IO;
    }

    const int64_t end = POUND_FTELL64(stream);
    if (end < 0)
    {
        POUND_LOG_ERROR(&thread_logger, "Sizing %s failed: the length was not reported.", path);
        (void)fclose(stream);
        return POUND_ERROR_IO;
    }

    if (0 != POUND_FSEEK64(stream, 0, SEEK_SET))
    {
        POUND_LOG_ERROR(&thread_logger, "Rewinding %s failed.", path);
        (void)fclose(stream);
        return POUND_ERROR_IO;
    }

    file->handle = stream;
    file->size   = (uint64_t)end;
    return POUND_SUCCESS;
}

void
fs_file_close(fs_file_t *POUND_RESTRICT file)
{
    if (NULL == file)
    {
        return;
    }

    if (NULL != file->handle)
    {
        (void)fclose((FILE *)file->handle);
    }

    memset(file, 0, sizeof(*file));
}

error_t
fs_file_reader(fs_file_t *POUND_RESTRICT file, fs_reader_t *POUND_RESTRICT reader)
{
    if (POUND_UNLIKELY(NULL == file) || POUND_UNLIKELY(NULL == reader))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting file reader: file=%p, reader=%p.", (const void *)file,
                        (const void *)reader);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    // Zero first so a rejected call yields a reader that refuses every read.
    memset(reader, 0, sizeof(*reader));

    if (NULL == file->handle)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting file reader: the file is closed.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    reader->context = file;
    reader->size    = file->size;
    reader->read_at = file_read_at;
    return POUND_SUCCESS;
}

bool
fs_file_is_open(const fs_file_t *POUND_RESTRICT file)
{
    return (NULL != file) && (NULL != file->handle);
}

/*** end of file ***/

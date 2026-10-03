//! The random-access byte source that every container parser is written against.

#include "fs/fs_reader.h"

#include "log.h"
#include <string.h>

/// `read_at` for a caller-owned buffer.
///
/// `context` is the `fs_reader_t` itself, set by `fs_reader_from_buffer`, so the
/// buffer's location and length are read from the same struct that owns them and cannot
/// disagree with `reader->size`.
static error_t
buffer_read_at(void *POUND_RESTRICT context, const uint64_t offset, void *POUND_RESTRICT destination,
               const size_t size)
{
    const fs_reader_t *POUND_RESTRICT reader = (const fs_reader_t *)context;

    if (POUND_UNLIKELY(NULL == reader))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the buffer reader has no context.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (0U == size)
    {
        return POUND_SUCCESS;
    }

    if (POUND_UNLIKELY(NULL == destination))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: a %zu-byte read at offset %llu was given no "
                        "destination.",
                        size,
                        (unsigned long long)offset);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    // The offset was validated against the source size by `fs_reader_read_at` before
    // this was reached, so it is in range and narrowing it to `size_t` is exact: it is
    // at most `reader->buffer_size`, which is itself a `size_t`.
    const uint8_t *POUND_RESTRICT source = reader->buffer_data + (size_t)offset;

    memcpy(destination, source, size);
    return POUND_SUCCESS;
}

void
fs_reader_from_buffer(fs_reader_t *POUND_RESTRICT reader, void *POUND_RESTRICT data, const size_t size)
{
    if (POUND_UNLIKELY(NULL == reader))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: the reader is NULL.");
        return;
    }

    // Zeroed first so that a rejected call leaves a reader that refuses every read
    // rather than one pointing at a buffer that was never accepted.
    reader->context     = NULL;
    reader->size        = 0U;
    reader->read_at     = NULL;
    reader->buffer_data = NULL;
    reader->buffer_size = 0U;

    if (POUND_UNLIKELY((NULL == data) && (0U != size)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: a %zu-byte buffer was described with a NULL pointer.",
                        size);
        return;
    }

    reader->buffer_data = (const uint8_t *)data;
    reader->buffer_size = size;
    reader->size        = (uint64_t)size;
    reader->read_at     = buffer_read_at;

    // `read_at` receives the reader, so the bounds it copies from and the bounds
    // `fs_reader_read_at` checked against are the same two fields.
    reader->context = reader;
}

bool
fs_reader_range_is_valid(const fs_reader_t *POUND_RESTRICT reader, const uint64_t offset, const size_t size)
{
    if (POUND_UNLIKELY(NULL == reader))
    {
        return false;
    }

    // Written as a subtraction from the source size rather than as `offset + size <=
    // reader->size`, because that addition can wrap. An offset near 2^64 and a modest
    // size would sum to a small number and pass the addition form, handing the caller
    // a range that starts past the end of the source and reads whatever the subtraction
    // form correctly rejects. The subtraction form cannot wrap, since both operands are
    // bounded and `offset` is compared against the size before being subtracted.
    if (offset > reader->size)
    {
        return false;
    }

    return (size <= (size_t)(reader->size - offset));
}

void
fs_reader_copy(fs_reader_t *POUND_RESTRICT copy, const fs_reader_t *POUND_RESTRICT source)
{
    if (POUND_UNLIKELY(NULL == copy))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: the destination reader is NULL.");
        return;
    }

    if (POUND_UNLIKELY(NULL == source))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the source reader is NULL.");

        // Zeroed rather than left alone. A destination that still held a previous
        // reader's pointer would keep that reader's bytes reachable, which is the exact
        // failure this function exists to make impossible.
        memset(copy, 0, sizeof(*copy));
        return;
    }

    *copy = *source;

    // The self-reference test, and the whole of the fix. `fs_reader_from_buffer` sets
    // `context` to the reader it was handed so that `buffer_read_at` can find the buffer;
    // a struct assignment copies that pointer across unchanged, leaving the destination
    // addressing the source. Rebinding it here is what makes the copy independent.
    //
    // Nothing else is touched. A reader whose context is an allocation or a file handle
    // keeps it, because that memory belongs to whoever created it and the copy is meant
    // to refer to the same bytes.
    if (copy->context == (const void *)source)
    {
        copy->context = copy;
    }
}

error_t
fs_reader_read_at(const fs_reader_t *POUND_RESTRICT reader, const uint64_t offset,
                  void *POUND_RESTRICT destination, const size_t size)
{
    if (POUND_UNLIKELY(NULL == reader))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the reader is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(NULL == reader->read_at))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the reader at %p has no read function.",
                        (const void *)reader);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    // A zero-byte read is always satisfiable and says nothing about the range, so it is
    // answered before the bounds check. A caller probing a header at the very end of a
    // file with a zero length should get success, not an out-of-range failure.
    if (0U == size)
    {
        return POUND_SUCCESS;
    }

    if (POUND_UNLIKELY(NULL == destination))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: a %zu-byte read at offset %llu was given no "
                        "destination.",
                        size,
                        (unsigned long long)offset);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(!fs_reader_range_is_valid(reader, offset, size)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "A %zu-byte read at offset %llu runs past the end of a %llu-byte "
                        "source.",
                        size,
                        (unsigned long long)offset,
                        (unsigned long long)reader->size);
        return POUND_ERROR_IO;
    }

    return reader->read_at(reader->context, offset, destination, size);
}

void
fs_reader_copy(fs_reader_t *POUND_RESTRICT copy, const fs_reader_t *POUND_RESTRICT source)
{
    if (POUND_UNLIKELY(NULL == copy))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: the destination reader is NULL.");
        return;
    }

    if (POUND_UNLIKELY(NULL == source))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the source reader is NULL.");
        memset(copy, 0, sizeof(*copy));
        return;
    }

    *copy = *source;

    if (copy->context == (const void *)source)
    {
        copy->context = copy;
    }
}

/*** end of file ***/

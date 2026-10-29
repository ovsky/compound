//! Tests for the file-backed `fs_reader_t`.
//!
//! The interesting property is not "fread works". It is that a reader over a
//! host file enforces the same range rule as the buffer reader, reports a
//! truncated read as an I/O failure rather than returning a partial segment,
//! and refuses to read after the file has been closed. Boot tests rely on all
//! three: a loader that accepted a short read would map uninitialised bytes as
//! code.

#include "pound_test.h"

#include "fs/file_reader.h"

#include <stdio.h>
#include <string.h>

static unsigned g_file_counter = 0U;

static void
make_path(char *POUND_RESTRICT path, const size_t capacity)
{
    (void)snprintf(path, capacity, "compound_file_reader_%u.bin", g_file_counter++);
}

static error_t
write_file(const char *POUND_RESTRICT path, const void *POUND_RESTRICT data, const size_t size)
{
    FILE *stream = fopen(path, "wb");
    if (NULL == stream)
    {
        return POUND_ERROR_IO;
    }

    const size_t written = fwrite(data, 1U, size, stream);
    const int    closed  = fclose(stream);

    if ((written != size) || (0 != closed))
    {
        (void)remove(path);
        return POUND_ERROR_IO;
    }

    return POUND_SUCCESS;
}

POUND_TEST(file_reader, open_reports_size_and_reads_exact_bytes)
{
    char path[128];
    make_path(path, sizeof(path));

    const uint8_t bytes[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    POUND_REQUIRE(POUND_SUCCESS == write_file(path, bytes, sizeof(bytes)));

    fs_file_t file;
    POUND_REQUIRE(POUND_SUCCESS == fs_file_open(&file, path));
    POUND_CHECK(fs_file_is_open(&file));
    POUND_CHECK_EQ_U64(file.size, sizeof(bytes));

    fs_reader_t reader;
    POUND_REQUIRE(POUND_SUCCESS == fs_file_reader(&file, &reader));
    POUND_CHECK_EQ_U64(reader.size, sizeof(bytes));

    uint8_t out[4] = {0};
    POUND_CHECK_EQ_I64(fs_reader_read_at(&reader, 4U, out, sizeof(out)), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(out[0], 4U);
    POUND_CHECK_EQ_U64(out[3], 7U);

    // A read that starts inside the file but ends past it must fail, not return
    // the short prefix.
    memset(out, 0xFF, sizeof(out));
    POUND_CHECK_EQ_I64(fs_reader_read_at(&reader, 14U, out, sizeof(out)), POUND_ERROR_IO);
    POUND_CHECK_EQ_U64(out[0], 0xFFU);

    fs_file_close(&file);
    POUND_CHECK(!fs_file_is_open(&file));
    POUND_CHECK_EQ_U64(file.size, 0U);

    // The reader still points at the file object; a read through it must notice
    // that the underlying handle is gone.
    POUND_CHECK_EQ_I64(fs_reader_read_at(&reader, 0U, out, sizeof(out)), POUND_ERROR_NOT_INITIALIZED);

    (void)remove(path);
}

POUND_TEST(file_reader, opening_a_missing_file_is_not_found)
{
    char path[128];
    (void)snprintf(path, sizeof(path), "compound_file_reader_missing_%u.bin", g_file_counter++);
    (void)remove(path);

    fs_file_t file;
    POUND_CHECK_EQ_I64(fs_file_open(&file, path), POUND_ERROR_NOT_FOUND);
    POUND_CHECK(!fs_file_is_open(&file));
}

POUND_TEST(file_reader, reader_refuses_a_closed_file)
{
    fs_file_t  file;
    fs_reader_t reader;

    memset(&file, 0, sizeof(file));
    memset(&reader, 0xAA, sizeof(reader));

    POUND_CHECK_EQ_I64(fs_file_reader(&file, &reader), POUND_ERROR_NOT_INITIALIZED);
    POUND_CHECK_PTR_NULL(reader.read_at);
    POUND_CHECK_EQ_U64(reader.size, 0U);
}

POUND_TEST(file_reader, null_arguments_are_rejected)
{
    POUND_CHECK_EQ_I64(fs_file_open(NULL, "x"), POUND_ERROR_INVALID_ARGUMENT);

    fs_file_t file;
    POUND_CHECK_EQ_I64(fs_file_open(&file, NULL), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(fs_file_open(&file, ""), POUND_ERROR_INVALID_ARGUMENT);
}

POUND_TEST_SUITE(file_reader,
                 POUND_TEST_CASE(file_reader, open_reports_size_and_reads_exact_bytes),
                 POUND_TEST_CASE(file_reader, opening_a_missing_file_is_not_found),
                 POUND_TEST_CASE(file_reader, reader_refuses_a_closed_file),
                 POUND_TEST_CASE(file_reader, null_arguments_are_rejected))

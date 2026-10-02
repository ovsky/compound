//! PFS0 parsing. The format is documented at length in `pfs0.h`; this file is the
//! implementation of that document, and the comments here are about the decisions and
//! the arithmetic rather than a second description of the layout.

#include "fs/pfs0.h"

#include "log.h"
#include "memory/memory.h"
#include <string.h>

/// Alignment requested for the file table.
///
/// A power of two, as `memory_subsystem_allocate` requires. Each entry begins with two
/// 64-bit fields, so the natural alignment is eight; sixteen is requested so an entry
/// never straddles a cacheline boundary, which costs nothing on a table of at most a
/// few hundred kilobytes and keeps a scan from doing two fetches per entry.
#define PFS0_TABLE_ALIGNMENT 16U

/// Alignment requested for the string table, which is read as bytes and scanned as a
/// sequence of NUL-terminated strings.
#define PFS0_STRING_ALIGNMENT 16U

/// Size of the buffer `pfs0_hash` streams a file's contents through.
///
/// Large enough that a multi-megabyte file takes a few hundred reads rather than a few
/// thousand, and small enough to allocate on any platform the loader runs on without
/// thinking about it. Sixty-four kilobytes is a compromise between the two, and the
/// number has no correctness attached to it: any size produces the same digest.
#define PFS0_HASH_CHUNK_SIZE (64U * 1024U)

/// Reads a little-endian 32-bit field.
///
/// Written as shifts rather than by loading a `uint32_t` and swapping, for the same
/// reason `sha256.c` does it that way: the result must not depend on the host's byte
/// order, and must not depend on a strict-aliasing assumption about a packed struct.
/// The fields here are read one at a time out of a buffer that came from a file, so
/// there is no guarantee of alignment and no guarantee of the host's order.
static uint32_t
load_le32(const uint8_t *POUND_RESTRICT bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/// Reads a little-endian 64-bit field.
static uint64_t
load_le64(const uint8_t *POUND_RESTRICT bytes)
{
    return (uint64_t)load_le32(bytes) | ((uint64_t)load_le32(bytes + 4) << 32);
}

/// Renders a magic that failed to match, as four characters, so a log can show what was
/// actually found rather than only that it was wrong.
///
/// The bytes are masked and printed through a printable range deliberately: a wrong
/// magic is attacker- or corruption-controlled, and putting its raw bytes into a log
/// record as a `%c` would let a truncated image inject escape sequences into the log or
/// a terminal. Bytes outside printable ASCII are shown as `?`.
static void
describe_magic(const uint32_t magic, char out[5])
{
    for (size_t i = 0U; i < 4U; ++i)
    {
        const uint8_t byte = (uint8_t)((magic >> (i * 8U)) & 0xFFU);

        out[i] = ((byte >= 0x20U) && (byte < 0x7FU)) ? (char)byte : '?';
    }

    out[4] = '\0';
}

/// Checks that `[offset, offset + size)` lies inside `[base, base + length)`.
///
/// The subtraction form throughout, because `offset + size` cannot then wrap and appear
/// to be inside the range when it is not. An offset near the top of the address space
/// with a modest size is the case that catches this out, and in a format whose offsets
/// come straight out of a file it is the case that matters.
///
/// The intermediate `relative > length` test is not redundant. Without it,
/// `length - relative` is itself an unsigned subtraction that wraps for any offset
/// beyond the end of the range, producing a huge allowance that the size check then
/// passes. Both halves of the subtraction have to be proved in range before either
/// subtraction is performed.
static bool
range_within(const uint64_t offset, const uint64_t size, const uint64_t base, const uint64_t length)
{
    if (offset < base)
    {
        return false;
    }

    const uint64_t relative = offset - base;

    if (relative > length)
    {
        return false;
    }

    return (size <= (length - relative));
}

error_t
pfs0_open(pfs0_t *POUND_RESTRICT pfs0, const fs_reader_t *POUND_RESTRICT reader)
{
    if (POUND_UNLIKELY(NULL == pfs0))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the partition is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(NULL == reader))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the source reader is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(NULL == reader->read_at))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the source reader at %p has no read function.",
                        (const void *)reader);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    // Left closed on every failure path below, so a caller may call `pfs0_close`
    // unconditionally, a second close is a no-op, and a caller that ignores the result of
    // this call and asks the partition a question anyway is told it is not open. The
    // reader is deliberately *not* copied in here; it is published on the success paths,
    // because `pfs0_is_open` defines openness as having a working reader and a failed
    // open has none.
    pfs0->entries             = NULL;
    pfs0->count               = 0U;
    pfs0->data_offset         = 0U;
    pfs0->data_size           = 0U;
    pfs0->string_table_offset = 0U;
    pfs0->string_table_size   = 0U;
    memset(&pfs0->reader, 0, sizeof(pfs0->reader));

    uint8_t header[PFS0_HEADER_SIZE];

    error_t status = fs_reader_read_at(reader, 0U, header, sizeof(header));

    if (POUND_UNLIKELY(POUND_SUCCESS != status))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Could not read the %zu-byte partition header: %s.",
                        sizeof(header),
                        pound_error_to_string(status));
        return status;
    }

    const uint32_t magic            = load_le32(&header[0]);
    const uint32_t file_count       = load_le32(&header[4]);
    const uint32_t string_size      = load_le32(&header[8]);
    const uint32_t reserved         = load_le32(&header[12]);

    if (POUND_UNLIKELY(PFS0_MAGIC != magic))
    {
        char found[5];

        describe_magic(magic, found);

        POUND_LOG_ERROR(&thread_logger,
                        "This is not a %s partition: the magic is '%s' (0x%08X), not '%s' "
                        "(0x%08X).",
                        PFS0_MAGIC_TEXT,
                        found,
                        magic,
                        PFS0_MAGIC_TEXT,
                        PFS0_MAGIC);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    // Read but deliberately not enforced. The field is documented as reserved and is
    // zero in every file Nintendo's tooling produces, but nothing in the format gives
    // it a meaning, and rejecting an image over a field whose meaning is unknown would
    // make Pound stricter than the format requires for no benefit. Noted at debug level
    // so a user investigating a rejected image can see whether it is zero.
    if (0U != reserved)
    {
        POUND_LOG_DEBUG(&thread_logger,
                        "The partition's reserved header field is 0x%08X rather than zero. "
                        "It has no defined meaning and is being ignored.",
                        reserved);
    }

    if (POUND_UNLIKELY(file_count > PFS0_MAX_FILES))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "The partition claims %u files, but no real partition has more than "
                        "%u; refusing to size a table from it.",
                        file_count,
                        PFS0_MAX_FILES);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    if (POUND_UNLIKELY(string_size > PFS0_MAX_STRING_TABLE_SIZE))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "The partition claims a %u-byte string table, but the largest one "
                        "this parser accepts is %u bytes.",
                        string_size,
                        PFS0_MAX_STRING_TABLE_SIZE);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    if (POUND_UNLIKELY(string_size < PFS0_STRING_TABLE_MAGIC_SIZE))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "The partition's string table is %u bytes, which is too small to "
                        "contain even its own magic.",
                        string_size);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    // The file table immediately follows the header, and its size is bounded because
    // `file_count` is, so the multiply cannot wrap. The two lengths are then added
    // only after both have been checked against the source, which is what keeps a
    // header claiming a huge table from producing a small sum that passes the range
    // test.
    const uint64_t table_bytes = (uint64_t)file_count * PFS0_ENTRY_SIZE;

    if (POUND_UNLIKELY(!fs_reader_range_is_valid(reader, PFS0_HEADER_SIZE, (size_t)table_bytes)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "The partition's %u-entry file table needs %llu bytes at offset %u, "
                        "which runs past the end of a %llu-byte source.",
                        file_count,
                        (unsigned long long)table_bytes,
                        PFS0_HEADER_SIZE,
                        (unsigned long long)reader->size);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    const uint64_t string_table_offset = PFS0_HEADER_SIZE + table_bytes;

    if (POUND_UNLIKELY(!fs_reader_range_is_valid(reader, string_table_offset, string_size)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "The partition's %u-byte string table at offset %llu runs past the end "
                        "of a %llu-byte source.",
                        string_size,
                        (unsigned long long)string_table_offset,
                        (unsigned long long)reader->size);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    // The data region starts after the string table. `reader->size` is at least
    // `string_table_offset + string_size` because that range was just validated, so
    // this subtraction cannot go negative.
    const uint64_t data_offset = string_table_offset + string_size;
    const uint64_t data_size   = reader->size - data_offset;

    if (0U == file_count)
    {
        // An empty partition is legal and does appear: an NCA section 0 for a title
        // with no loose content, and the NSO inside a delta fragment that has been
        // fully applied. There is nothing to allocate and nothing to validate, so this
        // succeeds with an empty table rather than being treated as a degenerate error.
        pfs0->data_offset         = data_offset;
        pfs0->data_size           = data_size;
        pfs0->string_table_offset = string_table_offset;
        pfs0->string_table_size   = string_size;
        pfs0->reader              = *reader;

        return POUND_SUCCESS;
    }

    // The entry array holds parsed `pfs0_entry_t` values, which are much larger than the
    // 24 raw bytes each one came from: a parsed entry carries a fixed-size name buffer
    // where the raw entry carries only an offset into the string table. Sizing this
    // allocation from the raw table length rather than from the parsed entry size writes
    // several hundred bytes past the end of the block for every file in the partition.
    // The two lengths are deliberately different variables so that they cannot be
    // confused at the call site.
    const size_t entry_bytes = (size_t)file_count * sizeof(pfs0_entry_t);

    pfs0_entry_t *POUND_RESTRICT entries = (pfs0_entry_t *)memory_subsystem_allocate(PFS0_TABLE_ALIGNMENT,
                                                                                     entry_bytes);

    if (POUND_UNLIKELY(NULL == entries))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Could not allocate room for the partition's %u file entries.",
                        file_count);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    pfs0->entries = entries;
    pfs0->count   = file_count;

    pfs0->data_offset         = data_offset;
    pfs0->data_size           = data_size;
    pfs0->string_table_offset = string_table_offset;
    pfs0->string_table_size   = string_size;

    // The whole file table is read in one go rather than entry by entry: it is one
    // contiguous run of bytes, and a single read of a few hundred kilobytes costs the
    // source one operation instead of one per entry.
    uint8_t *POUND_RESTRICT table = (uint8_t *)memory_subsystem_allocate(PFS0_TABLE_ALIGNMENT, (size_t)table_bytes);

    if (POUND_UNLIKELY(NULL == table))
    {
        POUND_LOG_ERROR(&thread_logger, "Could not allocate room to read the partition's file table.");

        // `entries` is already published, so the same teardown as the failure paths
        // below is what releases it. Going through `pfs0_close` rather than freeing by
        // hand means there is one place that knows the state a half-open partition is
        // in.
        pfs0_close(pfs0);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    status = fs_reader_read_at(reader, PFS0_HEADER_SIZE, table, (size_t)table_bytes);

    if (POUND_UNLIKELY(POUND_SUCCESS != status))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Could not read the partition's file table: %s.",
                        pound_error_to_string(status));
        memory_subsystem_free(table);
        pfs0_close(pfs0);
        return status;
    }

    uint8_t *POUND_RESTRICT strings = (uint8_t *)memory_subsystem_allocate(PFS0_STRING_ALIGNMENT, string_size);

    if (POUND_UNLIKELY(NULL == strings))
    {
        POUND_LOG_ERROR(&thread_logger, "Could not allocate room to read the partition's string table.");
        memory_subsystem_free(table);
        pfs0_close(pfs0);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    status = fs_reader_read_at(reader, string_table_offset, strings, string_size);

    if (POUND_UNLIKELY(POUND_SUCCESS != status))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Could not read the partition's string table: %s.",
                        pound_error_to_string(status));
        memory_subsystem_free(strings);
        memory_subsystem_free(table);
        pfs0_close(pfs0);
        return status;
    }

    const uint32_t string_magic = load_le32(strings);

    if (POUND_UNLIKELY(PFS0_MAGIC != string_magic))
    {
        char found[5];

        describe_magic(string_magic, found);

        POUND_LOG_ERROR(&thread_logger,
                        "The partition's header is valid but its string table begins with '%s' "
                        "(0x%08X) rather than '%s'.",
                        found,
                        string_magic,
                        PFS0_MAGIC_TEXT);
        memory_subsystem_free(strings);
        memory_subsystem_free(table);
        pfs0_close(pfs0);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    // Names live after the string table's own magic, and an entry's `string_offset` is
    // measured from there.
    const uint32_t names_size = string_size - PFS0_STRING_TABLE_MAGIC_SIZE;

    for (uint32_t i = 0U; i < file_count; ++i)
    {
        const uint8_t *POUND_RESTRICT raw     = &table[(size_t)i * PFS0_ENTRY_SIZE];
        pfs0_entry_t *POUND_RESTRICT   entry   = &entries[i];

        // The format stores this relative to the start of the data region. It is kept
        // relative through the range checks below and rebased afterwards, because
        // rebasing first would let a huge stored offset wrap when the base is added and
        // then pass a check that was written for an absolute offset.
        const uint64_t relative = load_le64(&raw[0]);

        entry->size = load_le64(&raw[8]);

        const uint32_t string_offset = load_le32(&raw[16]);
        const uint32_t hashed_size   = load_le32(&raw[20]);

        if (POUND_UNLIKELY(string_offset >= names_size))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "File %u of the partition points at name offset %u, but the "
                            "string table holds only %u bytes of names.",
                            i,
                            string_offset,
                            names_size);
            memory_subsystem_free(strings);
            memory_subsystem_free(table);
            pfs0_close(pfs0);
            return POUND_ERROR_MALFORMED_HEADER;
        }

        // The name must be terminated before the end of the name region. Searching the
        // remaining region rather than a fixed window is what turns "this name runs off
        // the end of the string table" into a diagnosed header error instead of a read
        // past the allocation.
        const uint8_t *POUND_RESTRICT name_start = &strings[PFS0_STRING_TABLE_MAGIC_SIZE + string_offset];
        const size_t             available     = names_size - string_offset;

        size_t name_length = 0U;

        while ((name_length < available) && ('\0' != name_start[name_length]))
        {
            ++name_length;
        }

        if (POUND_UNLIKELY(name_length >= available))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "The name of file %u of the partition is not terminated before the "
                            "end of the string table.",
                            i);
            memory_subsystem_free(strings);
            memory_subsystem_free(table);
            pfs0_close(pfs0);
            return POUND_ERROR_MALFORMED_HEADER;
        }

        if (POUND_UNLIKELY(name_length >= PFS0_NAME_MAX))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "The name of file %u of the partition is %zu characters, which does "
                            "not fit in the %u this parser stores.",
                            i,
                            name_length,
                            PFS0_NAME_MAX);
            memory_subsystem_free(strings);
            memory_subsystem_free(table);
            pfs0_close(pfs0);
            return POUND_ERROR_MALFORMED_HEADER;
        }

        if (POUND_UNLIKELY(0U == name_length))
        {
            POUND_LOG_ERROR(&thread_logger, "File %u of the partition has an empty name.", i);
            memory_subsystem_free(strings);
            memory_subsystem_free(table);
            pfs0_close(pfs0);
            return POUND_ERROR_MALFORMED_HEADER;
        }

        memcpy(entry->name, name_start, name_length);
        entry->name[name_length] = '\0';

        if (POUND_UNLIKELY(!range_within(relative, entry->size, 0U, data_size)))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "File %u ('%s') of the partition occupies %llu bytes at data offset "
                            "%llu, which leaves the %llu-byte data region.",
                            i,
                            entry->name,
                            (unsigned long long)entry->size,
                            (unsigned long long)relative,
                            (unsigned long long)data_size);
            memory_subsystem_free(strings);
            memory_subsystem_free(table);
            pfs0_close(pfs0);
            return POUND_ERROR_MALFORMED_HEADER;
        }

        // Safe to add now: the check above has established that `relative` is at most
        // `data_size`, which is at most the reader's size, so the sum cannot wrap and
        // lands inside the source by construction.
        entry->offset = data_offset + relative;

        // The format defines this field as always zero and no tool has ever set it. A
        // nonzero value means the file was produced by something that does not follow
        // the format, which is worth knowing, but it changes no offset and no size, so
        // it is reported and not treated as a rejection.
        if (0U != hashed_size)
        {
            POUND_LOG_DEBUG(&thread_logger,
                            "File %u ('%s') of the partition has a nonzero hashed-size field "
                            "(0x%08X). The format defines it as always zero and it is ignored.",
                            i,
                            entry->name,
                            hashed_size);
        }
    }

    // Duplicate names and overlapping byte ranges are both impossible in a file from
    // Nintendo's tooling, so either one means the image is not what it claims to be.
    // The checks are quadratic in the entry count, which is a few thousand at most and
    // takes a few million string comparisons -- fast enough that a cheaper algorithm
    // would be premature, and simple enough that there is nothing to get wrong.
    for (uint32_t i = 0U; i < file_count; ++i)
    {
        for (uint32_t j = (i + 1U); j < file_count; ++j)
        {
            if (0 == strcmp(entries[i].name, entries[j].name))
            {
                POUND_LOG_ERROR(&thread_logger,
                                "Files %u and %u of the partition are both called '%s'. A name "
                                "must be unique, or a lookup by name would be ambiguous.",
                                i,
                                j,
                                entries[i].name);
                memory_subsystem_free(strings);
                memory_subsystem_free(table);
                pfs0_close(pfs0);
                return POUND_ERROR_MALFORMED_HEADER;
            }

            // A zero-length file occupies no bytes, so it can never overlap anything
            // and is skipped. Two entries claiming the same bytes is the case worth
            // catching: it means the partition is describing its data twice, and which
            // copy a reader gets would depend on iteration order.
            if ((0U == entries[i].size) || (0U == entries[j].size))
            {
                continue;
            }

            const bool i_in_j = range_within(entries[i].offset, entries[i].size, entries[j].offset, entries[j].size);
            const bool j_in_i = range_within(entries[j].offset, entries[j].size, entries[i].offset, entries[i].size);

            if (POUND_UNLIKELY(i_in_j || j_in_i))
            {
                POUND_LOG_ERROR(&thread_logger,
                                "Files %u ('%s') and %u ('%s') of the partition claim "
                                "overlapping bytes of its data region.",
                                i,
                                entries[i].name,
                                j,
                                entries[j].name);
                memory_subsystem_free(strings);
                memory_subsystem_free(table);
                pfs0_close(pfs0);
                return POUND_ERROR_MALFORMED_HEADER;
            }
        }
    }

    memory_subsystem_free(strings);
    memory_subsystem_free(table);

    // Published last, once every entry is known good. A caller cannot observe a
    // half-built table.
    pfs0->reader = *reader;

    return POUND_SUCCESS;
}

void
pfs0_close(pfs0_t *POUND_RESTRICT pfs0)
{
    if (POUND_UNLIKELY(NULL == pfs0))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: the partition is NULL.");
        return;
    }

    if (NULL != pfs0->entries)
    {
        memory_subsystem_free(pfs0->entries);
    }

    // Returned to the closed state rather than merely emptied, so a second close is a
    // no-op instead of a double free, and so a caller that ignores the result of
    // `pfs0_open` and calls close anyway cannot free an unowned pointer.
    pfs0->entries             = NULL;
    pfs0->count               = 0U;
    pfs0->data_offset         = 0U;
    pfs0->data_size           = 0U;
    pfs0->string_table_offset = 0U;
    pfs0->string_table_size   = 0U;

    memset(&pfs0->reader, 0, sizeof(pfs0->reader));
}

bool
pfs0_is_open(const pfs0_t *POUND_RESTRICT pfs0)
{
    if (POUND_UNLIKELY(NULL == pfs0))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the partition is NULL.");
        return false;
    }

    // An open partition with no files has a non-NULL, zero-length entry array... except
    // that `pfs0_open` does not allocate one in that case. So presence of an entry
    // array is not the test; the source being readable is. A partition whose reader has
    // no read function cannot be open, because opening one requires having read from it.
    return (NULL != pfs0->reader.read_at);
}

uint32_t
pfs0_count(const pfs0_t *POUND_RESTRICT pfs0)
{
    if (POUND_UNLIKELY(NULL == pfs0))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the partition is NULL.");
        return 0U;
    }

    return pfs0->count;
}

const pfs0_entry_t *
pfs0_entry_at(const pfs0_t *POUND_RESTRICT pfs0, const uint32_t index)
{
    if (POUND_UNLIKELY(NULL == pfs0))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the partition is NULL.");
        return NULL;
    }

    if (POUND_UNLIKELY(index >= pfs0->count))
    {
        // Not logged. This is called in a loop by anything that walks a partition, and
        // the loop's own bound is the thing that went wrong; a log per iteration would
        // bury the caller's own message.
        return NULL;
    }

    return &pfs0->entries[index];
}

error_t
pfs0_index_of(const pfs0_t *POUND_RESTRICT pfs0, const char *POUND_RESTRICT name,
              uint32_t *POUND_RESTRICT out_index)
{
    if (POUND_UNLIKELY(NULL == pfs0))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the partition is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY((NULL == name) || ('\0' == name[0])))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: a file lookup was given an empty name.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(NULL == out_index))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the file '%s' was looked up with no destination for its index.",
                        name);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    for (uint32_t i = 0U; i < pfs0->count; ++i)
    {
        if (0 == strcmp(pfs0->entries[i].name, name))
        {
            *out_index = i;
            return POUND_SUCCESS;
        }
    }

    // A miss is a question with an answer of "no", not a failure. The loader probes for
    // files that only some titles ship, so logging here would report a normal outcome
    // as a problem every time a title lacks an optional file.
    //
    // `out_index` is left alone rather than set to a sentinel, so a caller that ignores
    // the return value cannot mistake an untouched local for a valid index. Every
    // caller in Pound checks the status.
    return POUND_ERROR_NOT_FOUND;
}

bool
pfs0_contains(const pfs0_t *POUND_RESTRICT pfs0, const char *POUND_RESTRICT name)
{
    if ((NULL == pfs0) || (NULL == name) || ('\0' == name[0]))
    {
        return false;
    }

    for (uint32_t i = 0U; i < pfs0->count; ++i)
    {
        if (0 == strcmp(pfs0->entries[i].name, name))
        {
            return true;
        }
    }

    return false;
}

error_t
pfs0_read(const pfs0_t *POUND_RESTRICT pfs0, const uint32_t index, const uint64_t offset_within_file,
          void *POUND_RESTRICT destination, const size_t size)
{
    if (POUND_UNLIKELY(NULL == pfs0))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the partition is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(!pfs0_is_open(pfs0)))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the partition is not open.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    // Resolved before the entry is dereferenced. An index is a value, so an out-of-range
    // one is rejected rather than being used to index off the end of the table, and a
    // stale one from a partition that has since been closed can only name an index that
    // this table either has or does not.
    const pfs0_entry_t *POUND_RESTRICT entry = pfs0_entry_at(pfs0, index);

    if (POUND_UNLIKELY(NULL == entry))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: a %zu-byte read was asked for file %u, but the "
                        "partition holds %u file(s).",
                        size,
                        index,
                        pfs0->count);
        return POUND_ERROR_NOT_FOUND;
    }

    if (0U == size)
    {
        return POUND_SUCCESS;
    }

    if (POUND_UNLIKELY(NULL == destination))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: a %zu-byte read of '%s' was given no destination.",
                        size,
                        entry->name);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    // The same overflow-safe form `pfs0_open` used, so a caller that adds its own
    // arithmetic to a file's offset and size is caught here rather than producing a
    // wrapped sum that passes a naive check and reads outside the file.
    if (POUND_UNLIKELY(!range_within(offset_within_file, (uint64_t)size, 0U, entry->size)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "A %zu-byte read at offset %llu would run past the end of '%s', which "
                        "is %llu bytes.",
                        size,
                        (unsigned long long)offset_within_file,
                        entry->name,
                        (unsigned long long)entry->size);
        return POUND_ERROR_IO;
    }

    return fs_reader_read_at(&pfs0->reader, entry->offset + offset_within_file, destination, size);
}

error_t
pfs0_hash(const pfs0_t *POUND_RESTRICT pfs0, const uint32_t index, uint8_t out[SHA256_DIGEST_SIZE])
{
    if (POUND_UNLIKELY(NULL == out))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the hash destination is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(NULL == pfs0))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the partition is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(!pfs0_is_open(pfs0)))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the partition is not open.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    const pfs0_entry_t *POUND_RESTRICT entry = pfs0_entry_at(pfs0, index);

    if (POUND_UNLIKELY(NULL == entry))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: '%s' was hashed, but the partition holds only %u "
                        "file(s).",
                        (index < pfs0->count) ? pfs0->entries[index].name : "(out of range)",
                        pfs0->count);
        return POUND_ERROR_NOT_FOUND;
    }

    uint8_t *POUND_RESTRICT chunk = (uint8_t *)memory_subsystem_allocate(1U, PFS0_HASH_CHUNK_SIZE);

    if (POUND_UNLIKELY(NULL == chunk))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Could not allocate the %zu-byte buffer needed to hash '%s'.",
                        PFS0_HASH_CHUNK_SIZE,
                        entry->name);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    sha256_t context;

    sha256_init(&context);

    uint64_t consumed = 0U;

    error_t status = POUND_SUCCESS;

    while (consumed < entry->size)
    {
        const uint64_t remaining = entry->size - consumed;

        // The narrowing is exact: `remaining` is at least one here, and is clamped to the
        // chunk size, which is a `size_t`.
        size_t take = PFS0_HASH_CHUNK_SIZE;

        if (remaining < (uint64_t)take)
        {
            take = (size_t)remaining;
        }

        status = pfs0_read(pfs0, index, consumed, chunk, take);

        if (POUND_UNLIKELY(POUND_SUCCESS != status))
        {
            // Released here rather than left to the end of the function, so a failed
            // read cannot fall through to a finalisation of a context that never
            // finished and then write a digest of half a file.
            memory_subsystem_free(chunk);
            return status;
        }

        sha256_update(&context, chunk, take);

        consumed += (uint64_t)take;
    }

    // A zero-length file is hashed as the empty message, which `sha256_final` handles
    // without a preceding update, so the loop above correctly doing nothing is right
    // rather than an omission.
    sha256_final(&context, out);

    memory_subsystem_free(chunk);

    return POUND_SUCCESS;
}

error_t
pfs0_verify_hash(const pfs0_t *POUND_RESTRICT pfs0, const uint32_t index,
                 const uint8_t expected[SHA256_DIGEST_SIZE])
{
    if (POUND_UNLIKELY(NULL == expected))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the expected digest for a file check is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    // The same argument and openness checks `pfs0_hash` makes, and in the same order, so
    // that a caller cannot get a different code for the same mistake depending on which
    // of the two it happened to call.
    if (POUND_UNLIKELY(NULL == pfs0))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the partition is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(!pfs0_is_open(pfs0)))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the partition is not open.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    const pfs0_entry_t *POUND_RESTRICT entry = pfs0_entry_at(pfs0, index);

    if (POUND_UNLIKELY(NULL == entry))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: a digest was checked for file %u, but the partition "
                        "holds only %u file(s).",
                        index,
                        pfs0->count);
        return POUND_ERROR_NOT_FOUND;
    }

    uint8_t actual[SHA256_DIGEST_SIZE];

    const error_t status = pfs0_hash(pfs0, index, actual);

    if (POUND_UNLIKELY(POUND_SUCCESS != status))
    {
        return status;
    }

    if (POUND_UNLIKELY(!sha256_equal(actual, expected)))
    {
        char actual_hex[SHA256_HEX_SIZE + 1U];
        char expected_hex[SHA256_HEX_SIZE + 1U];

        sha256_to_hex(actual, actual_hex);
        sha256_to_hex(expected, expected_hex);

        // Both digests, because "the hash is wrong" has two very different causes and
        // the user can usually tell them apart from these: a file of the right length
        // with the wrong content was modified, and a short file was truncated, and the
        // fix for the second is a re-download rather than a hunt for tampering.
        POUND_LOG_ERROR(&thread_logger,
                        "'%s' does not match its recorded SHA-256. The file is %llu bytes and "
                        "hashes to %s, but %s was expected. A file that is shorter than "
                        "expected is usually an incomplete copy.",
                        entry->name,
                        (unsigned long long)entry->size,
                        actual_hex,
                        expected_hex);
        return POUND_ERROR_HASH_MISMATCH;
    }

    return POUND_SUCCESS;
}

void
pfs0_log_summary(const pfs0_t *POUND_RESTRICT pfs0)
{
    if (POUND_UNLIKELY(NULL == pfs0))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: the partition is NULL.");
        return;
    }

    if (POUND_UNLIKELY(!pfs0_is_open(pfs0)))
    {
        POUND_LOG_WARN(&thread_logger, "Ignoring call: the partition is not open.");
        return;
    }

    POUND_LOG_INFO(&thread_logger,
                   "The partition holds %u file(s) in %llu bytes of data, with its file table "
                   "at offset %llu.",
                   pfs0->count,
                   (unsigned long long)pfs0->data_size,
                   (unsigned long long)pfs0->string_table_offset);

    // A bounded number of names. A partition with thousands of files would otherwise
    // produce a log nobody reads, and a count plus a sample is what answers the question
    // a user actually has when they are looking at a log, which is whether the files
    // they expected are there.
    const uint32_t SHOWN = 16U;
    const uint32_t shown = (pfs0->count < SHOWN) ? pfs0->count : SHOWN;

    for (uint32_t i = 0U; i < shown; ++i)
    {
        POUND_LOG_INFO(&thread_logger, "  file %u: %s", i, pfs0->entries[i].name);
    }

    if (shown < pfs0->count)
    {
        POUND_LOG_INFO(&thread_logger,
                       "  ... and %u more. Open the partition to list the rest.",
                       pfs0->count - shown);
    }
}

/*** end of file ***/

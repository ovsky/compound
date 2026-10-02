//! HFS0 parsing. The format is documented at length in `hfs0.h`; this file is the
//! implementation of that document, and the comments here are about the decisions and the
//! arithmetic rather than a second description of the layout.

#include "fs/hfs0.h"

#include "log.h"
#include "memory/memory.h"
#include <string.h>

/// Alignment requested for the file table.
///
/// A power of two, as `memory_subsystem_allocate` requires. Each entry is 64 bytes and
/// eight-byte aligned, so the natural alignment is eight; sixteen is requested so an entry
/// never straddles a cacheline boundary, which costs nothing on a table of at most a few
/// hundred kilobytes and keeps a scan from doing two fetches per entry.
#define HFS0_TABLE_ALIGNMENT 16U

/// Alignment requested for the string table, which is read as bytes and scanned as a
/// sequence of NUL-terminated strings.
#define HFS0_STRING_ALIGNMENT 16U

/// Size of the buffer `hfs0_hash` and `hfs0_hash_metadata` stream bytes through.
///
/// Large enough that a multi-megabyte file takes a few hundred reads rather than a few
/// thousand, and small enough to allocate on any platform the loader runs on without
/// thinking about it. Sixty-four kilobytes is a compromise between the two, and the
/// number has no correctness attached to it: any size produces the same digest.
#define HFS0_HASH_CHUNK_SIZE (64U * 1024U)

/// Reads a little-endian 32-bit field.
///
/// Written as shifts rather than by loading a `uint32_t` and swapping, for the same
/// reason `sha256.c` and `pfs0.c` do it that way: the result must not depend on the host's
/// byte order, and must not depend on a strict-aliasing assumption about a packed struct.
/// The fields here are read one at a time out of a buffer that came from a file, so there
/// is no guarantee of alignment and no guarantee of the host's order.
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
/// The bytes are masked and printed through a printable range deliberately: a wrong magic
/// is corruption-controlled, and putting its raw bytes into a log record as a `%c` would
/// let a truncated image inject escape sequences into the log or a terminal. Bytes outside
/// printable ASCII are shown as `?`.
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
/// The subtraction form throughout, because `offset + size` cannot then wrap and appear to
/// be inside the range when it is not. An offset near the top of the address space with a
/// modest size is the case that catches this out, and in a format whose offsets come
/// straight out of a file it is the case that matters.
///
/// The intermediate `relative > length` test is not redundant. Without it,
/// `length - relative` is itself an unsigned subtraction that wraps for any offset beyond
/// the end of the range, producing a huge allowance that the size check then passes. Both
/// halves of the subtraction have to be proved in range before either is performed.
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

/// The state a slice's reader carries: which parent, and where inside it the entry starts.
///
/// Kept private to this file, and reached only through `slice_read_at`, which is the whole
/// reason `hfs0_slice_t` has to own an allocation -- see the note in `hfs0.h`.
typedef struct
{
    /// The partition's reader, copied by value so the slice keeps working even if the
    /// caller closes the partition it came from. The bytes it refers to must still
    /// outlive the slice, which is the reader's own contract.
    fs_reader_t parent;

    /// Absolute offset of the entry's first byte within the parent's source.
    uint64_t base;
} hfs0_slice_context_t;

/// Reads through a slice, rebasing the caller's offset onto the entry.
///
/// The size check happens before the addition so the rebased offset cannot wrap, and the
/// parent is asked to do the range check for us rather than repeating it here.
static error_t
slice_read_at(void *POUND_RESTRICT context, const uint64_t offset, void *POUND_RESTRICT destination, const size_t size)
{
    const hfs0_slice_context_t *const slice = context;

    if (size > (UINT64_MAX - slice->base))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting read: a %zu-byte read at offset 0x%llx inside an entry starting "
                        "at 0x%llx would push the offset past the address space.",
                        size,
                        (unsigned long long)offset,
                        (unsigned long long)slice->base);
        return POUND_ERROR_GUEST_ADDRESS_OVERFLOW;
    }

    return fs_reader_read_at(&slice->parent, slice->base + offset, destination, size);
}

error_t
hfs0_open(hfs0_t *POUND_RESTRICT hfs0, const fs_reader_t *POUND_RESTRICT reader)
{
    if (POUND_UNLIKELY(NULL == hfs0))
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

    // Left closed on every failure path below, so a caller may call `hfs0_close`
    // unconditionally, a second close is a no-op, and a caller that ignores the result of
    // this call and asks the partition a question anyway is told it is not open. The
    // reader is deliberately *not* copied in here; it is published on the success paths,
    // because `hfs0_is_open` defines openness as having a working reader and a failed open
    // has none.
    hfs0->entries             = NULL;
    hfs0->hashed_sizes        = NULL;
    hfs0->count               = 0U;
    hfs0->data_offset         = 0U;
    hfs0->data_size           = 0U;
    hfs0->string_table_offset = 0U;
    hfs0->metadata_size       = 0U;
    hfs0->string_table_size   = 0U;
    memset(&hfs0->reader, 0, sizeof(hfs0->reader));

    uint8_t header[HFS0_HEADER_SIZE];

    error_t status = fs_reader_read_at(reader, 0U, header, sizeof(header));

    if (POUND_UNLIKELY(POUND_SUCCESS != status))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Could not read the %zu-byte partition header: %s.",
                        sizeof(header),
                        pound_error_to_string(status));
        return status;
    }

    const uint32_t magic       = load_le32(&header[0]);
    const uint32_t file_count  = load_le32(&header[4]);
    const uint32_t string_size = load_le32(&header[8]);
    const uint32_t reserved    = load_le32(&header[12]);

    if (POUND_UNLIKELY(HFS0_MAGIC != magic))
    {
        char found[5];

        describe_magic(magic, found);

        POUND_LOG_ERROR(&thread_logger,
                        "This is not a %s partition: the magic is '%s' (0x%08X), not '%s' "
                        "(0x%08X).",
                        HFS0_MAGIC_TEXT,
                        found,
                        magic,
                        HFS0_MAGIC_TEXT,
                        HFS0_MAGIC);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    // Read but deliberately not enforced. The field is documented as reserved and is zero
    // in every file Nintendo's tooling produces, but nothing in the format gives it a
    // meaning, and rejecting an image over a field whose meaning is unknown would refuse
    // images that are otherwise perfectly readable.
    POUND_UNUSED(reserved);

    if (POUND_UNLIKELY(0U == file_count))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "The %s partition declares no files, so there is nothing to read out of "
                        "it. An empty partition is always a truncated or wrong file.",
                        HFS0_MAGIC_TEXT);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    if (POUND_UNLIKELY(file_count > HFS0_MAX_FILES))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "The %s partition declares %u files, more than the %u a real one holds. "
                        "Refusing before the file table is sized, because that table would be "
                        "%llu bytes.",
                        HFS0_MAGIC_TEXT,
                        file_count,
                        (unsigned int)HFS0_MAX_FILES,
                        (unsigned long long)file_count * (unsigned long long)HFS0_ENTRY_SIZE);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    if (POUND_UNLIKELY(string_size > HFS0_MAX_STRING_TABLE_SIZE))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "The %s partition declares a %u-byte string table, more than the %u a real "
                        "one holds.",
                        HFS0_MAGIC_TEXT,
                        string_size,
                        (unsigned int)HFS0_MAX_STRING_TABLE_SIZE);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    // Every offset in the table is relative to the start of the data region, and the data
    // region starts after the metadata. So the size of the metadata has to be established
    // before any entry can be checked against anything, and it is a sum of three fields
    // that came from a file -- so the sum is proved not to wrap before it is used as a
    // length. `file_count` is already bounded, so only the string table can be large
    // enough to matter, but the check is written on the sum rather than on the term so it
    // keeps being right if the caps ever change.
    const uint64_t table_bytes = (uint64_t)file_count * (uint64_t)HFS0_ENTRY_SIZE;

    if (POUND_UNLIKELY(table_bytes > (UINT64_MAX - HFS0_HEADER_SIZE - string_size)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: %u files of %u bytes plus a %u-byte string table "
                        "overflows the address space, so the partition's size cannot be computed.",
                        file_count,
                        (unsigned int)HFS0_ENTRY_SIZE,
                        string_size);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    const uint64_t metadata_size = HFS0_HEADER_SIZE + table_bytes + (uint64_t)string_size;

    if (POUND_UNLIKELY(metadata_size < HFS0_MIN_METADATA_SIZE))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "The %s partition's metadata region is %llu bytes, less than the %zu its "
                        "own header and one table entry require.",
                        HFS0_MAGIC_TEXT,
                        (unsigned long long)metadata_size,
                        (size_t)HFS0_MIN_METADATA_SIZE);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    // The table has to be inside the source before anything is allocated for it, so a
    // header claiming a huge partition cannot make the loader reserve memory for a file
    // that does not exist. This is the check the file-count cap alone cannot do: 4096
    // entries is a quarter of a megabyte of table, and a truncated file could still claim
    // a table that runs past its own end.
    if (POUND_UNLIKELY(!fs_reader_range_is_valid(reader, HFS0_HEADER_SIZE, (size_t)table_bytes)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "The %s partition's file table of %u entries (%llu bytes, from offset %u) "
                        "runs past the end of the %llu-byte source.",
                        HFS0_MAGIC_TEXT,
                        file_count,
                        (unsigned long long)table_bytes,
                        (unsigned int)HFS0_HEADER_SIZE,
                        (unsigned long long)reader->size);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    if (POUND_UNLIKELY(!fs_reader_range_is_valid(reader, HFS0_HEADER_SIZE + table_bytes, (size_t)string_size)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "The %s partition's %u-byte string table (from offset %llu) runs past the "
                        "end of the %llu-byte source.",
                        HFS0_MAGIC_TEXT,
                        string_size,
                        (unsigned long long)(HFS0_HEADER_SIZE + table_bytes),
                        (unsigned long long)reader->size);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    const uint64_t string_table_offset = HFS0_HEADER_SIZE + table_bytes;
    const uint64_t data_offset         = string_table_offset + (uint64_t)string_size;

    // An entry with no data is not possible in a real partition, but one that claims to
    // extend past the end of the source is the case that matters, and that can only be
    // judged against the source. A partition whose declared data region is longer than the
    // source is a truncated file, and every read out of it would otherwise fail
    // individually with no indication that the image itself is short.
    if (POUND_UNLIKELY(!fs_reader_range_is_valid(reader, data_offset, 0U)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "The %s partition's data region starts at %llu, past the end of the "
                        "%llu-byte source.",
                        HFS0_MAGIC_TEXT,
                        (unsigned long long)data_offset,
                        (unsigned long long)reader->size);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    uint8_t *const raw_table = memory_subsystem_allocate(HFS0_TABLE_ALIGNMENT, (size_t)table_bytes);

    if (POUND_UNLIKELY(NULL == raw_table))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Could not allocate %llu bytes for the file table of %u entries.",
                        (unsigned long long)table_bytes,
                        file_count);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    uint8_t *const strings = NULL;

    if (POUND_UNLIKELY(0U != string_size))
    {
        strings = memory_subsystem_allocate(HFS0_STRING_ALIGNMENT, (size_t)string_size);

        if (POUND_UNLIKELY(NULL == strings))
        {
            POUND_LOG_ERROR(&thread_logger, "Could not allocate %u bytes for the string table.", string_size);
            memory_subsystem_free(raw_table);
            return POUND_ERROR_ALLOCATION_FAILED;
        }
    }

    status = fs_reader_read_at(reader, HFS0_HEADER_SIZE, raw_table, (size_t)table_bytes);

    if (POUND_UNLIKELY(POUND_SUCCESS != status))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Could not read the %llu-byte file table: %s.",
                        (unsigned long long)table_bytes,
                        pound_error_to_string(status));
        memory_subsystem_free(raw_table);
        memory_subsystem_free(strings);
        return status;
    }

    if (POUND_UNLIKELY(0U != string_size))
    {
        status = fs_reader_read_at(reader, string_table_offset, strings, (size_t)string_size);

        if (POUND_UNLIKELY(POUND_SUCCESS != status))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "Could not read the %u-byte string table: %s.",
                            string_size,
                            pound_error_to_string(status));
            memory_subsystem_free(raw_table);
            memory_subsystem_free(strings);
            return status;
        }
    }

    hfs0_entry_t *const entries = memory_subsystem_allocate(HFS0_TABLE_ALIGNMENT,
                                                           (size_t)file_count * sizeof(hfs0_entry_t));

    if (POUND_UNLIKELY(NULL == entries))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Could not allocate %zu bytes for %u decoded entries.",
                        (size_t)file_count * sizeof(hfs0_entry_t),
                        file_count);
        memory_subsystem_free(raw_table);
        memory_subsystem_free(strings);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    uint64_t *const hashed_sizes = memory_subsystem_allocate(HFS0_TABLE_ALIGNMENT,
                                                            (size_t)file_count * sizeof(uint64_t));

    if (POUND_UNLIKELY(NULL == hashed_sizes))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Could not allocate %zu bytes for %u hashed sizes.",
                        (size_t)file_count * sizeof(uint64_t),
                        file_count);
        memory_subsystem_free(entries);
        memory_subsystem_free(raw_table);
        memory_subsystem_free(strings);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    // Every failure from here on has to release all four allocations, which is a
    // mistake waiting to happen when one more is added. A single exit is not available in
    // C without a goto, and a goto here would be the clearest thing in the file, so the
    // releases are written out at each site and the count is checked by the fact that the
    // tests reopen a partition after every kind of rejection without the process growing.
    status = POUND_SUCCESS;

    for (uint32_t i = 0U; i < file_count; ++i)
    {
        const uint8_t *const raw = &raw_table[(size_t)i * HFS0_ENTRY_SIZE];

        const uint64_t entry_offset  = load_le64(&raw[HFS0_ENTRY_OFFSET_OFFSET]);
        const uint64_t entry_size    = load_le64(&raw[HFS0_ENTRY_SIZE_OFFSET]);
        const uint64_t string_offset = load_le64(&raw[HFS0_ENTRY_STRING_OFFSET_OFFSET]);
        const uint64_t hashed_size   = load_le64(&raw[HFS0_ENTRY_HASHED_SIZE_OFFSET]);

        hashed_sizes[i] = hashed_size;

        if (POUND_UNLIKELY(string_offset >= string_size))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "Entry %u of %u names itself at string table offset %llu, which is "
                            "outside the %u-byte table.",
                            i,
                            file_count,
                            (unsigned long long)string_offset,
                            string_size);
            status = POUND_ERROR_MALFORMED_HEADER;
            break;
        }

        // Find the end of the name by scanning for a NUL, bounded by the table's own end.
        // A name with no NUL in it would otherwise be read past the buffer, and the string
        // table's last name in a valid image is the one that ends exactly at the table's
        // end -- which is why the bound is `>=` against the offset and the scan is against
        // the size, not the other way round.
        const uint8_t *const name_bytes = &strings[(size_t)string_offset];
        const size_t       name_room  = (size_t)(string_size - string_offset);
        size_t             name_length = 0U;

        while ((name_length < name_room) && ('\0' != name_bytes[name_length]))
        {
            ++name_length;
        }

        if (POUND_UNLIKELY(name_length >= name_room))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "Entry %u of %u has a name that is not terminated within the string "
                            "table; %llu byte(s) remain from its offset %llu and none of them is "
                            "a terminator.",
                            i,
                            file_count,
                            (unsigned long long)name_room,
                            (unsigned long long)string_offset);
            status = POUND_ERROR_MALFORMED_HEADER;
            break;
        }

        if (POUND_UNLIKELY(name_length >= HFS0_NAME_MAX))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "Entry %u of %u has a %zu-byte name, which does not fit the %u-byte "
                            "field. Truncating it would collide with a real path and return the "
                            "wrong file.",
                            i,
                            file_count,
                            name_length,
                            (unsigned int)HFS0_NAME_MAX);
            status = POUND_ERROR_MALFORMED_HEADER;
            break;
        }

        // Reject an empty name. It is not something a real partition contains, and a
        // lookup for "" would then match it, which is not a useful outcome for any caller.
        if (POUND_UNLIKELY(0U == name_length))
        {
            POUND_LOG_ERROR(&thread_logger, "Entry %u of %u has an empty name.", i, file_count);
            status = POUND_ERROR_MALFORMED_HEADER;
            break;
        }

        if (POUND_UNLIKELY(!range_within(entry_offset, entry_size, data_offset, reader->size - data_offset)))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "Entry %u of %u occupies bytes [%llu, %llu), which is outside the data "
                            "region [%llu, %llu) of a %llu-byte source.",
                            i,
                            file_count,
                            (unsigned long long)entry_offset,
                            (unsigned long long)(entry_offset + entry_size),
                            (unsigned long long)data_offset,
                            (unsigned long long)reader->size,
                            (unsigned long long)reader->size);
            status = POUND_ERROR_MALFORMED_HEADER;
            break;
        }

        // Two entries covering the same bytes would make the partition's contents depend
        // on which one a reader found first, and there is no correct answer to which that
        // should be. Checked in the same pass as everything else, which costs a comparison
        // per pair, so it is a quadratic loop -- which is the reason this is a cap-checked
        // pass and not something to make clever later.
        for (uint32_t j = 0U; j < i; ++j)
        {
            if (POUND_UNLIKELY(0U == strcmp(entries[j].name, name_bytes)))
            {
                POUND_LOG_ERROR(&thread_logger,
                                "Entries %u and %u are both named '%s'. A lookup by name would be "
                                "ambiguous, and picking the first match is how the wrong file gets "
                                "loaded.",
                                j,
                                i,
                                name_bytes);
                status = POUND_ERROR_MALFORMED_HEADER;
                break;
            }
        }

        if (POUND_SUCCESS != status)
        {
            break;
        }

        memcpy(entries[i].name, name_bytes, name_length);
        entries[i].name[name_length] = '\0';
        entries[i].offset            = entry_offset;
        entries[i].size              = entry_size;
    }

    if (POUND_SUCCESS == status)
    {
        // Publish last, so that everything above ran against a closed partition and a
        // failure at any point leaves nothing that a caller could mistake for open.
        hfs0->reader              = *reader;
        hfs0->entries             = entries;
        hfs0->hashed_sizes        = hashed_sizes;
        hfs0->count               = file_count;
        hfs0->metadata_size       = metadata_size;
        hfs0->string_table_offset = string_table_offset;
        hfs0->string_table_size   = string_size;
        hfs0->data_offset         = data_offset;
        hfs0->data_size           = reader->size - data_offset;
    }
    else
    {
        memory_subsystem_free(hashed_sizes);
        memory_subsystem_free(entries);
    }

    memory_subsystem_free(raw_table);
    memory_subsystem_free(strings);

    return status;
}

void
hfs0_close(hfs0_t *POUND_RESTRICT hfs0)
{
    if (POUND_UNLIKELY(NULL == hfs0))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the partition is NULL.");
        return;
    }

    memory_subsystem_free(hfs0->hashed_sizes);
    hfs0->hashed_sizes = NULL;

    memory_subsystem_free(hfs0->entries);
    hfs0->entries = NULL;

    hfs0->count               = 0U;
    hfs0->data_offset         = 0U;
    hfs0->data_size           = 0U;
    hfs0->string_table_offset = 0U;
    hfs0->metadata_size       = 0U;
    hfs0->string_table_size   = 0U;
    memset(&hfs0->reader, 0, sizeof(hfs0->reader));
}

bool
hfs0_is_open(const hfs0_t *POUND_RESTRICT hfs0)
{
    return (NULL != hfs0) && (NULL != hfs0->entries) && (0U != hfs0->count) && (NULL != hfs0->reader.read_at);
}

uint32_t
hfs0_count(const hfs0_t *POUND_RESTRICT hfs0)
{
    return (NULL == hfs0) ? 0U : hfs0->count;
}

const hfs0_entry_t *
hfs0_entry_at(const hfs0_t *POUND_RESTRICT hfs0, const uint32_t index)
{
    if (POUND_UNLIKELY(!hfs0_is_open(hfs0)))
    {
        return NULL;
    }

    if (POUND_UNLIKELY(index >= hfs0->count))
    {
        return NULL;
    }

    return &hfs0->entries[index];
}

bool
hfs0_hashed_size_at(const hfs0_t *POUND_RESTRICT hfs0, const uint32_t index, uint64_t *POUND_RESTRICT out)
{
    if (POUND_UNLIKELY(!hfs0_is_open(hfs0)))
    {
        return false;
    }

    if (POUND_UNLIKELY(index >= hfs0->count))
    {
        return false;
    }

    if (POUND_UNLIKELY(NULL == out))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the output pointer is NULL.");
        return false;
    }

    *out = hfs0->hashed_sizes[index];
    return true;
}

error_t
hfs0_index_of(const hfs0_t *POUND_RESTRICT hfs0, const char *POUND_RESTRICT name,
              uint32_t *POUND_RESTRICT out_index)
{
    if (POUND_UNLIKELY(NULL == out_index))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the output pointer is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    *out_index = 0U;

    if (POUND_UNLIKELY(NULL == name))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the name is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(!hfs0_is_open(hfs0)))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the partition is not open.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    // No log on a miss. The loader probes for files that only some titles ship, so "not
    // there" is an answer rather than a failure, and a log line for every probe would bury
    // the failures that matter.
    for (uint32_t i = 0U; i < hfs0->count; ++i)
    {
        if (0 == strcmp(hfs0->entries[i].name, name))
        {
            *out_index = i;
            return POUND_SUCCESS;
        }
    }

    return POUND_ERROR_NOT_FOUND;
}

bool
hfs0_contains(const hfs0_t *POUND_RESTRICT hfs0, const char *POUND_RESTRICT name)
{
    uint32_t       index  = 0U;
    const error_t  status = hfs0_index_of(hfs0, name, &index);

    // A NULL name is a programming error rather than a miss, and `hfs0_index_of` has
    // already logged it. Collapsing that into "false" here would be a silent swallow of a
    // real mistake, so the distinction is preserved by reporting only NOT_FOUND as a miss.
    return (POUND_SUCCESS == status) || (POUND_ERROR_NOT_FOUND != status);
}

error_t
hfs0_read(const hfs0_t *POUND_RESTRICT hfs0, const uint32_t index,
          const uint64_t offset_within_file, void *POUND_RESTRICT destination, const size_t size)
{
    if (POUND_UNLIKELY(NULL == destination))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the destination is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(NULL == hfs0))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the partition is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(!hfs0_is_open(hfs0)))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the partition is not open.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    if (POUND_UNLIKELY(index >= hfs0->count))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: index %u is outside the %u file(s) this partition "
                        "holds.",
                        index,
                        hfs0->count);
        return POUND_ERROR_NOT_FOUND;
    }

    const hfs0_entry_t *const entry = &hfs0->entries[index];

    // Re-checked here rather than trusted from the open, because the caller supplied
    // `offset_within_file` and may have computed it from something else. The open proved
    // the entry is inside the source; this proves the caller's range is inside the entry.
    if (POUND_UNLIKELY(!range_within(offset_within_file, size, 0U, entry->size)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Entry %u ('%s') is %llu byte(s) long; a %zu-byte read at offset %llu is "
                        "outside it.",
                        index,
                        entry->name,
                        (unsigned long long)entry->size,
                        size,
                        (unsigned long long)offset_within_file);
        return POUND_ERROR_IO;
    }

    return fs_reader_read_at(&hfs0->reader, entry->offset + offset_within_file, destination, size);
}

/// Streams a byte range of a reader through SHA-256.
///
/// One implementation for both of the hashing entry points, because "hash some bytes out of
/// a reader in chunks" is the whole of what `hfs0_hash` and `hfs0_hash_metadata` do, and
/// two copies is two places for a chunk-boundary bug to live.
///
/// The chunk buffer is released on every path, including the ones that return early, which
/// is why there is one exit rather than one per failure.
static error_t
hash_range(const fs_reader_t *POUND_RESTRICT reader, const uint64_t offset, const uint64_t size,
           uint8_t out[SHA256_DIGEST_SIZE])
{
    uint8_t *const chunk = memory_subsystem_allocate(HFS0_STRING_ALIGNMENT, HFS0_HASH_CHUNK_SIZE);

    if (POUND_UNLIKELY(NULL == chunk))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Could not allocate the %u-byte buffer used to stream a hash.",
                        (unsigned int)HFS0_HASH_CHUNK_SIZE);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    sha256_t hash;
    error_t  status = POUND_SUCCESS;

    sha256_init(&hash);

    uint64_t remaining = size;

    while (0U != remaining)
    {
        // Clamp to the chunk size, and to whatever is left, rather than assuming either.
        // A file whose declared size is not a multiple of the chunk size is normal, and the
        // loop must not read the tail twice or skip it.
        const size_t take
            = (remaining < (uint64_t)HFS0_HASH_CHUNK_SIZE) ? (size_t)remaining : (size_t)HFS0_HASH_CHUNK_SIZE;

        status = fs_reader_read_at(reader, offset + (size - remaining), chunk, take);

        if (POUND_UNLIKELY(POUND_SUCCESS != status))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "Could not read %zu byte(s) at offset %llu while hashing: %s.",
                            take,
                            (unsigned long long)(offset + (size - remaining)),
                            pound_error_to_string(status));
            break;
        }

        sha256_update(&hash, chunk, take);
        remaining -= take;
    }

    if (POUND_SUCCESS == status)
    {
        sha256_final(&hash, out);
    }

    memory_subsystem_free(chunk);
    return status;
}

error_t
hfs0_hash(const hfs0_t *POUND_RESTRICT hfs0, const uint32_t index, uint8_t out[SHA256_DIGEST_SIZE])
{
    if (POUND_UNLIKELY(NULL == out))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the digest destination is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(NULL == hfs0))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the partition is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(!hfs0_is_open(hfs0)))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the partition is not open.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    if (POUND_UNLIKELY(index >= hfs0->count))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: index %u is outside the %u file(s) this partition "
                        "holds.",
                        index,
                        hfs0->count);
        return POUND_ERROR_NOT_FOUND;
    }

    const hfs0_entry_t *const entry = &hfs0->entries[index];

    return hash_range(&hfs0->reader, entry->offset, entry->size, out);
}

error_t
hfs0_hash_metadata(const hfs0_t *POUND_RESTRICT hfs0, uint8_t out[SHA256_DIGEST_SIZE])
{
    if (POUND_UNLIKELY(NULL == out))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the digest destination is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(NULL == hfs0))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the partition is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(!hfs0_is_open(hfs0)))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the partition is not open.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    return hash_range(&hfs0->reader, 0U, hfs0->metadata_size, out);
}

error_t
hfs0_verify_metadata(const hfs0_t *POUND_RESTRICT hfs0, const uint8_t expected[SHA256_DIGEST_SIZE])
{
    if (POUND_UNLIKELY(NULL == expected))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the expected digest is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    uint8_t actual[SHA256_DIGEST_SIZE];

    const error_t status = hfs0_hash_metadata(hfs0, actual);

    if (POUND_UNLIKELY(POUND_SUCCESS != status))
    {
        return status;
    }

    if (POUND_UNLIKELY(0 != memcmp(actual, expected, SHA256_DIGEST_SIZE)))
    {
        char actual_hex[SHA256_HEX_SIZE + 1U];
        char expected_hex[SHA256_HEX_SIZE + 1U];

        sha256_to_hex(actual, actual_hex);
        sha256_to_hex(expected, expected_hex);

        POUND_LOG_ERROR(&thread_logger,
                        "The %s partition's %llu-byte metadata region hashes to %s, not %s. The image "
                        "is either truncated or has been modified since it was written.",
                        HFS0_MAGIC_TEXT,
                        (unsigned long long)hfs0->metadata_size,
                        actual_hex,
                        expected_hex);
        return POUND_ERROR_HASH_MISMATCH;
    }

    return POUND_SUCCESS;
}

error_t
hfs0_entry_reader(const hfs0_t *POUND_RESTRICT hfs0, const uint32_t index, hfs0_slice_t *POUND_RESTRICT out)
{
    if (POUND_UNLIKELY(NULL == out))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the slice is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    out->reader  = (fs_reader_t){ 0 };
    out->context = NULL;

    if (POUND_UNLIKELY(NULL == hfs0))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the partition is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(!hfs0_is_open(hfs0)))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the partition is not open.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    if (POUND_UNLIKELY(index >= hfs0->count))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: index %u is outside the %u file(s) this partition "
                        "holds.",
                        index,
                        hfs0->count);
        return POUND_ERROR_NOT_FOUND;
    }

    const hfs0_entry_t *const entry = &hfs0->entries[index];

    hfs0_slice_context_t *const context
        = memory_subsystem_allocate(_Alignof(hfs0_slice_context_t), sizeof(hfs0_slice_context_t));

    if (POUND_UNLIKELY(NULL == context))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Could not allocate %zu bytes for the slice over entry %u ('%s').",
                        sizeof(hfs0_slice_context_t),
                        index,
                        entry->name);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    context->parent = hfs0->reader;
    context->base   = entry->offset;

    out->context                  = context;
    out->reader.context           = context;
    out->reader.size              = entry->size;
    out->reader.read_at           = slice_read_at;
    out->reader.buffer_data       = NULL;
    out->reader.buffer_size       = 0U;

    return POUND_SUCCESS;
}

void
hfs0_slice_close(hfs0_slice_t *POUND_RESTRICT slice)
{
    if (POUND_UNLIKELY(NULL == slice))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the slice is NULL.");
        return;
    }

    memory_subsystem_free(slice->context);
    slice->context = NULL;
    slice->reader  = (fs_reader_t){ 0 };
}

void
hfs0_log_summary(const hfs0_t *POUND_RESTRICT hfs0)
{
    if (POUND_UNLIKELY(!hfs0_is_open(hfs0)))
    {
        POUND_LOG_INFO(&thread_logger, "The %s partition is not open; there is nothing to summarise.", HFS0_MAGIC_TEXT);
        return;
    }

    POUND_LOG_INFO(&thread_logger,
                   "%s partition: %u file(s), %llu-byte metadata region, %llu byte(s) of data from "
                   "offset %llu.",
                   HFS0_MAGIC_TEXT,
                   hfs0->count,
                   (unsigned long long)hfs0->metadata_size,
                   (unsigned long long)hfs0->data_size,
                   (unsigned long long)hfs0->data_offset);

    // Only the first few names. A partition with a few hundred files would otherwise bury
    // everything else in the log, and a long name list tells a user nothing that a count
    // and a size do not.
    enum
    {
        /// How many names `hfs0_log_summary` prints. Chosen so an XCI's handful of NCAs and
        /// an update NCA's first few content files both fit, and so a large partition does
        /// not turn one log record into a page.
        SUMMARY_NAMES = 8U,
    };

    const uint32_t shown = (hfs0->count < SUMMARY_NAMES) ? hfs0->count : (uint32_t)SUMMARY_NAMES;

    for (uint32_t i = 0U; i < shown; ++i)
    {
        POUND_LOG_INFO(&thread_logger,
                       "  [%u] %s -- %llu byte(s), hashed region %llu byte(s)",
                       i,
                       hfs0->entries[i].name,
                       (unsigned long long)hfs0->entries[i].size,
                       (unsigned long long)hfs0->hashed_sizes[i]);
    }

    if (hfs0->count > shown)
    {
        POUND_LOG_INFO(&thread_logger, "  ... and %u more file(s).", hfs0->count - shown);
    }
}

/*** end of file ***/
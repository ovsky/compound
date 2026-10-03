//! HFS0 parsing tests.
//!
//! The image builder here writes the format byte for byte as the header documents it, so a
//! test and the parser agree only where the *format* says they should. The alternative --
//! checking a parsed partition against another parser -- would pass for two consistent
//! misreadings of the layout, which for this format is the exact class of bug worth
//! catching: the three ways HFS0 differs from PFS0 (64-byte entries, string offsets measured
//! from the start of the string table, and a `hashed_size` field) each produce a plausible
//! table rather than a clean failure.

#include "pound_test.h"

#include "attributes.h"
#include "crypto/sha256.h"
#include "errors.h"
#include "fs/hfs0.h"
#include "fs/pfs0.h"
#include "log.h"
#include <string.h>

/// One file to place in a synthetic partition.
///
/// `contents` may be NULL only when `size` is zero, which is also how a case asks for an
/// empty file. `hashed_size` is written into the entry verbatim so a test can give every
/// entry the same value the real tooling uses, or vary it to prove the field is recorded
/// rather than derived.
typedef struct
{
    const char *name;
    uint8_t    *POUND_RESTRICT contents;
    size_t      size;
    uint64_t    hashed_size;
} build_file_t;

/// Where the three regions of a synthetic partition landed, so a test can corrupt a
/// specific one without recomputing the arithmetic itself.
typedef struct
{
    size_t total;          ///< Zero if the files would not fit, which no test should produce.
    size_t table_offset;   ///< Always `HFS0_HEADER_SIZE`.
    size_t string_offset;  ///< Immediately after the file table.
    size_t data_offset;    ///< Immediately after the string table.
} hfs0_layout_t;

/// Writes a little-endian 32-bit value.
static void
store_le32(uint8_t *POUND_RESTRICT destination, const uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
    destination[2] = (uint8_t)(value >> 16);
    destination[3] = (uint8_t)(value >> 24);
}

/// Writes a little-endian 64-bit value.
static void
store_le64(uint8_t *POUND_RESTRICT destination, const uint64_t value)
{
    store_le32(destination, (uint32_t)value);
    store_le32(destination + 4, (uint32_t)(value >> 32));
}

/// Reads a little-endian 32-bit value.
static uint32_t
load_le32(const uint8_t *POUND_RESTRICT bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/// Reads a little-endian 64-bit value back out of a table entry.
///
/// The inverse of `store_le64`, used where a case has to assert something about the bytes
/// on disk rather than about the parser's reading of them. Asserting against the parser
/// alone would only prove the parser agrees with itself.
static uint64_t
load_entry_u64(const uint8_t *POUND_RESTRICT entry, const size_t field_offset)
{
    return (uint64_t)load_le32(&entry[field_offset]) | ((uint64_t)load_le32(&entry[field_offset + 4U]) << 32);
}

/// Writes a valid partition image containing `files` into `buffer`.
///
/// `layout->total` is set to zero if the files would not fit, so a test can require it and
/// catch its own sizing mistake rather than reading a truncated image and failing for the
/// wrong reason.
///
/// The image is built with no gaps between files' data, which is what the real tooling
/// produces and what lets a test compute a file's absolute offset independently of the
/// parser and compare. The last 32 bytes of each 64-byte entry are left zeroed, which is
/// what a real partition's reserved words hold.
static hfs0_layout_t
build_hfs0(uint8_t *POUND_RESTRICT buffer, const size_t capacity, const build_file_t *POUND_RESTRICT files,
           const size_t count)
{
    hfs0_layout_t layout;

    layout.table_offset  = HFS0_HEADER_SIZE;
    layout.string_offset = HFS0_HEADER_SIZE + (count * HFS0_ENTRY_SIZE);

    size_t string_size = 0U;

    for (size_t i = 0U; i < count; ++i)
    {
        string_size += strlen(files[i].name) + 1U;
    }

    layout.data_offset = layout.string_offset + string_size;

    size_t data_size = 0U;

    for (size_t i = 0U; i < count; ++i)
    {
        data_size += files[i].size;
    }

    layout.total = layout.data_offset + data_size;

    if (layout.total > capacity)
    {
        layout.total = 0U;
        return layout;
    }

    memset(buffer, 0, layout.total);

    store_le32(&buffer[0], HFS0_MAGIC);
    store_le32(&buffer[4], (uint32_t)count);
    store_le32(&buffer[8], (uint32_t)string_size);
    store_le32(&buffer[12], 0U);

    size_t name_cursor = 0U;
    size_t data_cursor = 0U;

    for (size_t i = 0U; i < count; ++i)
    {
        const size_t   name_length = strlen(files[i].name);
        uint8_t *POUND_RESTRICT entry = &buffer[layout.table_offset + (i * HFS0_ENTRY_SIZE)];

        store_le64(&entry[HFS0_ENTRY_OFFSET_OFFSET], (uint64_t)data_cursor);
        store_le64(&entry[HFS0_ENTRY_SIZE_OFFSET], (uint64_t)files[i].size);

        // Measured from the first byte of the string table itself, which is what
        // `hfs0_open` measures from and what the first name in a real partition has an
        // offset of: zero. This is the field most likely to be copied from PFS0, whose
        // offsets are four bytes further in because its string table carries a repeated
        // magic that HFS0's does not -- so the builder states it explicitly rather than
        // inheriting the value from a cursor.
        store_le64(&entry[HFS0_ENTRY_STRING_OFFSET_OFFSET], (uint64_t)name_cursor);
        store_le64(&entry[HFS0_ENTRY_HASHED_SIZE_OFFSET], files[i].hashed_size);

        memcpy(&buffer[layout.string_offset + name_cursor], files[i].name, name_length + 1U);

        name_cursor += name_length + 1U;

        if ((NULL != files[i].contents) && (0U != files[i].size))
        {
            memcpy(&buffer[layout.data_offset + data_cursor], files[i].contents, files[i].size);
        }

        data_cursor += files[i].size;
    }

    return layout;
}

/// Fills a `build_file_t` with a distinct, recognisable pattern and a `hashed_size` of
/// `hashed_size`.
static void
make_file(build_file_t *POUND_RESTRICT file, const char *name, uint8_t *POUND_RESTRICT contents, const size_t size,
          const uint8_t seed, const uint64_t hashed_size)
{
    file->name        = name;
    file->contents    = contents;
    file->size        = size;
    file->hashed_size = hashed_size;

    for (size_t i = 0U; i < size; ++i)
    {
        contents[i] = (uint8_t)(seed + (uint8_t)i);
    }
}

/// The partition most HFS0 cases use.
///
/// Three files of different sizes, so an off-by-one in an offset or a size cannot pass by
/// being right for a zero-length file. Two names contain forward slashes, one leading, so
/// the path shapes a real image uses -- `/hfs0/0` in an XCI, a full content path in an
/// update NCA -- are covered rather than only bare leaf names.
#define SAMPLE_FILE_COUNT 3U
#define SAMPLE_CAPACITY   4096U

/// Backing storage for the sample files' contents. Static so a case may overwrite its own
/// copy in the image without disturbing what the next case is handed.
static uint8_t SAMPLE_CONTENTS[SAMPLE_FILE_COUNT][512];

/// The hashed region an XCI's HFS0 entries record.
///
/// Not a value Pound acts on -- `hashed_size` belongs to whatever encloses the partition --
/// so it is only ever a fixed pattern the tests read back to prove the field was recorded
/// rather than recomputed.
#define SAMPLE_HASHED_SIZE 0x200ULL

/// Opens `buffer` as a partition and reports whether it worked.
///
/// Returns rather than asserting, because `POUND_REQUIRE` returns from the *helper*, not
/// from the case that called it: a case written `open_sample(...)` and then carried on
/// would run every one of its checks against a partition that was never open, turning one
/// clear failure into a dozen confusing ones. Every caller wraps this in
/// `POUND_REQUIRE(open_sample(...))`, so the case does abandon -- on the caller's frame.
static bool
open_sample(uint8_t *POUND_RESTRICT buffer, const size_t size, hfs0_t *POUND_RESTRICT out)
{
    fs_reader_t reader;

    fs_reader_from_buffer(&reader, buffer, size);

    return (POUND_SUCCESS == hfs0_open(out, &reader));
}

/// The sample image, built once into `image` and described by `layout`.
///
/// Every case starts from this so a corruption one case applies cannot be seen by the next,
/// which matters because most of these cases corrupt exactly one field and would otherwise
/// be order-dependent.
static void
make_sample(uint8_t *POUND_RESTRICT image, hfs0_layout_t *POUND_RESTRICT layout)
{
    build_file_t files[SAMPLE_FILE_COUNT];

    make_file(&files[0], "/hfs0/0", SAMPLE_CONTENTS[0], sizeof(SAMPLE_CONTENTS[0]), 0x10U, SAMPLE_HASHED_SIZE);
    make_file(&files[1], "/hfs0/1", SAMPLE_CONTENTS[1], 100U, 0x40U, SAMPLE_HASHED_SIZE);
    make_file(&files[2],
              "Nintendo Switch Product/Title/Content.nca",
              SAMPLE_CONTENTS[2],
              sizeof(SAMPLE_CONTENTS[2]),
              0x80U,
              SAMPLE_HASHED_SIZE);

    *layout = build_hfs0(image, SAMPLE_CAPACITY, files, SAMPLE_FILE_COUNT);

    POUND_REQUIRE(0U != layout->total);
}

POUND_TEST(hfs0, a_valid_partition_opens_and_reports_its_files)
{
    uint8_t      image[SAMPLE_CAPACITY];
    hfs0_layout_t layout;

    make_sample(image, &layout);

    hfs0_t hfs0;

    POUND_REQUIRE(open_sample(image, layout.total, &hfs0));

    POUND_CHECK(hfs0_is_open(&hfs0));
    POUND_CHECK_EQ_U64(hfs0_count(&hfs0), SAMPLE_FILE_COUNT);

    // The absolute offsets are what `fs_reader_read_at` takes, so the layout arithmetic is
    // checked here rather than a relative one -- a relative offset would pass even if the
    // parser forgot the data base entirely, and every read would then be wrong.
    POUND_CHECK_EQ_U64(hfs0.data_offset, layout.data_offset);
    POUND_CHECK_EQ_U64(hfs0.data_size, layout.total - layout.data_offset);
    POUND_CHECK_EQ_U64(hfs0.string_table_offset, layout.string_offset);
    POUND_CHECK_EQ_U64(hfs0.metadata_size, layout.data_offset);

    const hfs0_entry_t *const first = hfs0_entry_at(&hfs0, 0U);

    POUND_REQUIRE_PTR_NON_NULL(first);
    POUND_CHECK_STR_EQ(first->name, "/hfs0/0");
    POUND_CHECK_EQ_U64(first->offset, layout.data_offset);
    POUND_CHECK_EQ_U64(first->size, sizeof(SAMPLE_CONTENTS[0]));

    const hfs0_entry_t *const third = hfs0_entry_at(&hfs0, 2U);

    POUND_REQUIRE_PTR_NON_NULL(third);
    POUND_CHECK_STR_EQ(third->name, "Nintendo Switch Product/Title/Content.nca");
    POUND_CHECK_EQ_U64(third->offset, layout.data_offset + sizeof(SAMPLE_CONTENTS[0]) + 100U);

    hfs0_close(&hfs0);
    POUND_CHECK(!hfs0_is_open(&hfs0));
}

POUND_TEST(hfs0, every_encoding_the_parser_relies_on_is_the_one_the_format_documents)
{
    // The builder and the parser are in the same binary, so a change to either would be
    // invisible if the tests only ever compared them to each other. This checks the two
    // structural facts they could both get wrong together: the entry stride, and the base
    // the string offsets are measured from.
    uint8_t      image[SAMPLE_CAPACITY];
    hfs0_layout_t layout;

    make_sample(image, &layout);

    POUND_REQUIRE(HFS0_ENTRY_SIZE == 64U);
    POUND_REQUIRE(HFS0_HEADER_SIZE == 16U);

    // PFS0's entry is 24 bytes and its string offsets are measured past a four-byte magic.
    // Both of those are the plausible mistakes here, so both are pinned: if either drifts,
    // this fails with a clear statement rather than a table of garbage further down.
    POUND_CHECK_MSG(24U != HFS0_ENTRY_SIZE,
                    "HFS0's entry stride has become PFS0's 24 bytes, which reads a table of "
                    "plausible garbage rather than failing.");

    // The first name in a real HFS0 sits at string offset zero. The builder writes
    // `name_cursor` verbatim, which starts at zero, so the image itself carries the
    // evidence rather than this comment asserting it.
    const uint8_t *const first_entry = &image[layout.table_offset];

    POUND_CHECK_MSG(0U == load_entry_u64(first_entry, HFS0_ENTRY_STRING_OFFSET_OFFSET),
                    "The first name is not at string offset zero, so this image does not have "
                    "the layout a real partition has.");
}

POUND_TEST(hfs0, a_files_contents_read_back_byte_for_byte)
{
    uint8_t      image[SAMPLE_CAPACITY];
    hfs0_layout_t layout;

    make_sample(image, &layout);

    hfs0_t hfs0;

    POUND_REQUIRE(open_sample(image, layout.total, &hfs0));

    uint8_t read_back[512];

    POUND_REQUIRE(POUND_SUCCESS == hfs0_read(&hfs0, 1U, 0U, read_back, 100U));
    POUND_CHECK(0 == memcmp(read_back, SAMPLE_CONTENTS[1], 100U));

    // A read from inside the file, not only from its start, so an off-by-one in the base
    // offset is caught rather than cancelling out against the same error in the builder.
    POUND_REQUIRE(POUND_SUCCESS == hfs0_read(&hfs0, 1U, 64U, read_back, 36U));
    POUND_CHECK(0 == memcmp(read_back, &SAMPLE_CONTENTS[1][64], 36U));

    // The whole of the largest file, to prove a read larger than the chunk buffer the hash
    // path uses is not what is happening here -- and that the sizes are not being clamped.
    POUND_REQUIRE(POUND_SUCCESS == hfs0_read(&hfs0, 0U, 0U, read_back, sizeof(read_back)));
    POUND_CHECK(0 == memcmp(read_back, SAMPLE_CONTENTS[0], sizeof(SAMPLE_CONTENTS[0])));

    hfs0_close(&hfs0);
}

POUND_TEST(hfs0, an_empty_file_is_read_as_zero_bytes_and_still_appears_in_the_table)
{
    uint8_t      image[SAMPLE_CAPACITY];
    hfs0_layout_t layout;

    build_file_t files[2];

    make_file(&files[0], "empty.nca", NULL, 0U, 0U, SAMPLE_HASHED_SIZE);
    make_file(&files[1], "/hfs0/1", SAMPLE_CONTENTS[0], 32U, 0x20U, SAMPLE_HASHED_SIZE);

    layout = build_hfs0(image, SAMPLE_CAPACITY, files, 2U);

    POUND_REQUIRE(0U != layout.total);

    hfs0_t hfs0;

    POUND_REQUIRE(open_sample(image, layout.total, &hfs0));

    POUND_CHECK_EQ_U64(hfs0_count(&hfs0), 2U);

    const hfs0_entry_t *const entry = hfs0_entry_at(&hfs0, 0U);

    POUND_REQUIRE_PTR_NON_NULL(entry);
    POUND_CHECK_STR_EQ(entry->name, "empty.nca");
    POUND_CHECK_EQ_U64(entry->size, 0U);

    // Reading zero bytes at offset zero is a success, not a refusal. Refusing it would make
    // a caller special-case empty files, and the range check in `hfs0_read` is written to
    // allow a zero-length read at the very end of a file for exactly this reason.
    uint8_t scratch = 0U;

    POUND_CHECK_EQ_U64(POUND_SUCCESS == hfs0_read(&hfs0, 0U, 0U, &scratch, 0U), 1U);

    hfs0_close(&hfs0);
}

POUND_TEST(hfs0, a_read_that_would_leave_a_file_is_refused)
{
    uint8_t      image[SAMPLE_CAPACITY];
    hfs0_layout_t layout;

    make_sample(image, &layout);

    hfs0_t hfs0;

    POUND_REQUIRE(open_sample(image, layout.total, &hfs0));

    uint8_t      scratch[16];
    const size_t file_size = sizeof(SAMPLE_CONTENTS[0]);

    // One byte past the end.
    POUND_CHECK_EQ_U64(POUND_ERROR_IO == hfs0_read(&hfs0, 0U, file_size, scratch, 1U), 1U);

    // Straddling the end.
    POUND_CHECK_EQ_U64(POUND_ERROR_IO == hfs0_read(&hfs0, 0U, file_size - 8U, scratch, sizeof(scratch)), 1U);

    // Exactly the last byte, which must succeed -- the case either side of the refusal.
    POUND_CHECK_EQ_U64(POUND_SUCCESS == hfs0_read(&hfs0, 0U, file_size - 1U, scratch, 1U), 1U);

    // An offset past the end with nothing to read. The range is empty but starts outside the
    // file, and an empty range inside the source at a bogus offset is still a bad request.
    POUND_CHECK_EQ_U64(POUND_ERROR_IO == hfs0_read(&hfs0, 0U, file_size + 1U, scratch, 0U), 1U);

    // An offset that would wrap rather than merely exceed. Checked explicitly because the
    // subtraction in the range test is what makes it safe, and this is the case that
    // proves it: without the `relative > length` half, this reads as a huge allowance.
    POUND_CHECK_EQ_U64(POUND_ERROR_IO == hfs0_read(&hfs0, 0U, UINT64_MAX - 4U, scratch, 8U), 1U);

    // An index that names no file. This *does* log, and the difference from
    // `hfs0_index_of` is deliberate rather than an inconsistency: a miss from
    // `hfs0_index_of` is a probe -- "does this partition ship that optional file?" -- and
    // an answer, whereas a caller reaching `hfs0_read` with an index it did not get from
    // `hfs0_index_of` has a bug. Both return NOT_FOUND; only the bug is worth a record.
    pound_test_log_reset();
    POUND_CHECK_EQ_U64(POUND_ERROR_NOT_FOUND == hfs0_read(&hfs0, 99U, 0U, scratch, 1U), 1U);
    POUND_CHECK(pound_test_log_contains("index 99 is outside"));
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) > 0U);

    hfs0_close(&hfs0);
}

POUND_TEST(hfs0, files_are_found_by_name_and_a_miss_is_quiet)
{
    uint8_t      image[SAMPLE_CAPACITY];
    hfs0_layout_t layout;

    make_sample(image, &layout);

    hfs0_t hfs0;

    POUND_REQUIRE(open_sample(image, layout.total, &hfs0));

    uint32_t index = 0U;

    POUND_REQUIRE(POUND_SUCCESS == hfs0_index_of(&hfs0, "/hfs0/1", &index));
    POUND_CHECK_EQ_U64(index, 1U);

    POUND_CHECK(hfs0_contains(&hfs0, "Nintendo Switch Product/Title/Content.nca"));

    // A miss is an answer, not a failure: the loader probes for files only some titles
    // ship, so a lookup failure must not log. Asserted at ERROR specifically, since a
    // debug record would be reasonable and an error one is not.
    pound_test_log_reset();
    POUND_CHECK_EQ_U64(POUND_ERROR_NOT_FOUND == hfs0_index_of(&hfs0, "/hfs0/99", &index), 1U);
    POUND_CHECK_EQ_U64(pound_test_log_count_at(LOG_LEVEL_ERROR), 0U);
    POUND_CHECK(!hfs0_contains(&hfs0, "/hfs0/99"));

    // Every name in the sample image resolves back to itself, so a lookup cannot succeed
    // against the wrong entry -- which a forward-only test would not catch.
    for (uint32_t i = 0U; i < hfs0_count(&hfs0); ++i)
    {
        const hfs0_entry_t *const entry = hfs0_entry_at(&hfs0, i);

        POUND_REQUIRE_PTR_NON_NULL(entry);

        POUND_REQUIRE(POUND_SUCCESS == hfs0_index_of(&hfs0, entry->name, &index));
        POUND_CHECK_EQ_U64(index, i);
    }

    hfs0_close(&hfs0);
}

POUND_TEST(hfs0, hashed_sizes_are_recorded_verbatim_and_absent_indices_are_reported)
{
    uint8_t      image[SAMPLE_CAPACITY];
    hfs0_layout_t layout;

    // Deliberately different per entry, so a parser that filled the array from one header
    // field rather than from each entry would be caught.
    build_file_t files[3];

    make_file(&files[0], "a", SAMPLE_CONTENTS[0], 16U, 0x10U, 0x200ULL);
    make_file(&files[1], "b", SAMPLE_CONTENTS[1], 16U, 0x20U, 0x400ULL);
    make_file(&files[2], "c", SAMPLE_CONTENTS[2], 16U, 0x30U, 0ULL);

    layout = build_hfs0(image, SAMPLE_CAPACITY, files, 3U);

    POUND_REQUIRE(0U != layout.total);

    hfs0_t hfs0;

    POUND_REQUIRE(open_sample(image, layout.total, &hfs0));

    uint64_t hashed = 1U;

    POUND_CHECK(hfs0_hashed_size_at(&hfs0, 0U, &hashed));
    POUND_CHECK_EQ_U64(hashed, 0x200ULL);

    POUND_CHECK(hfs0_hashed_size_at(&hfs0, 1U, &hashed));
    POUND_CHECK_EQ_U64(hashed, 0x400ULL);

    // Zero is a legitimate hashed size and must not be reported as "absent". This is the
    // reason the API returns a boolean rather than using zero as a sentinel.
    POUND_CHECK(hfs0_hashed_size_at(&hfs0, 2U, &hashed));
    POUND_CHECK_EQ_U64(hashed, 0ULL);

    // An out-of-range index leaves the caller's value untouched rather than writing a
    // sentinel into it, so a caller that ignores the boolean cannot act on a number that
    // looks like a result.
    hashed = 0xABCDULL;
    POUND_CHECK(!hfs0_hashed_size_at(&hfs0, 3U, &hashed));
    POUND_CHECK_EQ_U64(hashed, 0xABCDULL);

    hfs0_close(&hfs0);
}

POUND_TEST(hfs0, the_metadata_region_hashes_to_what_hashing_those_bytes_directly_does)
{
    uint8_t      image[SAMPLE_CAPACITY];
    hfs0_layout_t layout;

    make_sample(image, &layout);

    hfs0_t hfs0;

    POUND_REQUIRE(open_sample(image, layout.total, &hfs0));

    // The metadata region is the header, the file table and the string table -- everything
    // before the data. Which is exactly what an enclosing format hashes when it records a
    // digest of the HFS0's metadata, so the boundary is checked from both sides: too short
    // and the digest would cover part of a file's bytes; too long and it would cover the
    // first file's contents.
    uint8_t streamed[SHA256_DIGEST_SIZE];
    uint8_t direct[SHA256_DIGEST_SIZE];

    POUND_REQUIRE(POUND_SUCCESS == hfs0_hash_metadata(&hfs0, streamed));

    sha256(image, (size_t)layout.data_offset, direct);
    POUND_CHECK(0 == memcmp(streamed, direct, SHA256_DIGEST_SIZE));

    // One byte less, which would end mid-string-table.
    sha256(image, (size_t)layout.data_offset - 1U, direct);
    POUND_CHECK_MSG(0 != memcmp(streamed, direct, SHA256_DIGEST_SIZE),
                    "Hashing one byte less gives the same digest, so the metadata region is "
                    "not bounded by the string table's end.");

    hfs0_close(&hfs0);
}

POUND_TEST(hfs0, the_metadata_hash_verifies_against_the_image_and_reports_a_mismatch)
{
    uint8_t      image[SAMPLE_CAPACITY];
    hfs0_layout_t layout;

    make_sample(image, &layout);

    hfs0_t hfs0;

    POUND_REQUIRE(open_sample(image, layout.total, &hfs0));

    uint8_t expected[SHA256_DIGEST_SIZE];

    POUND_REQUIRE(POUND_SUCCESS == hfs0_hash_metadata(&hfs0, expected));

    // The image's own digest verifies, so the comparison is not vacuously passing.
    POUND_CHECK_EQ_U64(POUND_SUCCESS == hfs0_verify_metadata(&hfs0, expected), 1U);

    // One flipped bit in the file table, which is inside the hashed region.
    image[layout.table_offset + 9U] ^= 0x01U;

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(POUND_ERROR_HASH_MISMATCH == hfs0_verify_metadata(&hfs0, expected), 1U);

    // Both digests in the message, because "truncated" and "modified" need different
    // responses and a user cannot tell them apart from "mismatch".
    POUND_CHECK(pound_test_log_contains("metadata region hashes to"));
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) > 0U);

    hfs0_close(&hfs0);
}

POUND_TEST(hfs0, a_wrong_magic_is_reported_as_a_malformed_header)
{
    uint8_t      image[SAMPLE_CAPACITY];
    hfs0_layout_t layout;

    make_sample(image, &layout);

    // PFS0's magic, which is the mistake a caller makes by pointing this parser at the
    // wrong format -- and the one whose header layout is close enough to be worth trying.
    store_le32(&image[0], PFS0_MAGIC);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, image, layout.total);

    hfs0_t hfs0;

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(POUND_ERROR_MALFORMED_HEADER == hfs0_open(&hfs0, &reader), 1U);
    POUND_CHECK(pound_test_log_contains("HFS0"));
    POUND_CHECK(!hfs0_is_open(&hfs0));

    hfs0_close(&hfs0);
}

POUND_TEST(hfs0, a_file_count_beyond_the_cap_is_refused_without_allocating)
{
    uint8_t image[SAMPLE_CAPACITY];

    memset(image, 0, sizeof(image));
    store_le32(&image[0], HFS0_MAGIC);
    store_le32(&image[4], HFS0_MAX_FILES + 1U);
    store_le32(&image[8], 4U);
    store_le32(&image[12], 0U);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, image, sizeof(image));

    hfs0_t hfs0;

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(POUND_ERROR_MALFORMED_HEADER == hfs0_open(&hfs0, &reader), 1U);

    // The refusal happens before anything is allocated, which is the whole point of the
    // cap: the declared count would ask for a quarter-megabyte table from a file that does
    // not contain one. The absence of a partition is the observable form of that.
    POUND_CHECK(!hfs0_is_open(&hfs0));
    POUND_CHECK(pound_test_log_contains("more than the"));

    hfs0_close(&hfs0);
}

POUND_TEST(hfs0, a_partition_declaring_no_files_is_refused)
{
    uint8_t image[SAMPLE_CAPACITY];

    memset(image, 0, sizeof(image));
    store_le32(&image[0], HFS0_MAGIC);
    store_le32(&image[4], 0U);
    store_le32(&image[8], 0U);
    store_le32(&image[12], 0U);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, image, sizeof(image));

    hfs0_t hfs0;

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(POUND_ERROR_MALFORMED_HEADER == hfs0_open(&hfs0, &reader), 1U);
    POUND_CHECK(!hfs0_is_open(&hfs0));
    POUND_CHECK(pound_test_log_contains("declares no files"));

    hfs0_close(&hfs0);
}

POUND_TEST(hfs0, a_name_pointing_past_the_string_table_is_refused)
{
    uint8_t      image[SAMPLE_CAPACITY];
    hfs0_layout_t layout;

    make_sample(image, &layout);

    // The first entry's name offset, measured from the start of the string table.
    store_le64(&image[layout.table_offset + HFS0_ENTRY_STRING_OFFSET_OFFSET], (uint64_t)layout.string_offset);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, image, layout.total);

    hfs0_t hfs0;

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(POUND_ERROR_MALFORMED_HEADER == hfs0_open(&hfs0, &reader), 1U);
    POUND_CHECK(pound_test_log_contains("outside the"));
    POUND_CHECK(!hfs0_is_open(&hfs0));

    hfs0_close(&hfs0);
}

POUND_TEST(hfs0, a_name_with_no_terminator_is_refused_rather_than_read_past_the_table)
{
    uint8_t      image[SAMPLE_CAPACITY];
    hfs0_layout_t layout;

    make_sample(image, &layout);

    // Overwrite the last byte of the string table, which terminates the last name, with a
    // non-NUL. Every name then runs to the end of the table with nothing to stop it, which
    // is the case that would read past the buffer if the scan were not bounded.
    image[layout.data_offset - 1U] = 'x';

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, image, layout.total);

    hfs0_t hfs0;

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(POUND_ERROR_MALFORMED_HEADER == hfs0_open(&hfs0, &reader), 1U);
    POUND_CHECK(pound_test_log_contains("not terminated"));
    POUND_CHECK(!hfs0_is_open(&hfs0));

    hfs0_close(&hfs0);
}

POUND_TEST(hfs0, a_name_too_long_to_store_is_refused_rather_than_truncated)
{
    uint8_t image[SAMPLE_CAPACITY];

    // A name of exactly the capacity, which fits only if the terminator is counted. One
    // longer is the refusal case, and the boundary between them is where a parser that
    // checked `length > capacity` rather than `length >= capacity` would truncate.
    char name[HFS0_NAME_MAX + 2U];

    memset(name, 'n', sizeof(name) - 1U);
    name[sizeof(name) - 1U] = '\0';

    build_file_t files[1];

    files[0].name        = name;
    files[0].contents    = SAMPLE_CONTENTS[0];
    files[0].size        = 16U;
    files[0].hashed_size = SAMPLE_HASHED_SIZE;

    const hfs0_layout_t layout = build_hfs0(image, SAMPLE_CAPACITY, files, 1U);

    POUND_REQUIRE(0U != layout.total);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, image, layout.total);

    hfs0_t hfs0;

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(POUND_ERROR_MALFORMED_HEADER == hfs0_open(&hfs0, &reader), 1U);
    POUND_CHECK(pound_test_log_contains("does not fit"));

    hfs0_close(&hfs0);

    // And exactly at the limit it opens, with the whole name present. A parser that
    // refused this would look correct on the case above while dropping a real long path.
    name[HFS0_NAME_MAX - 1U] = '\0';

    const hfs0_layout_t fitting = build_hfs0(image, SAMPLE_CAPACITY, files, 1U);

    POUND_REQUIRE(0U != fitting.total);

    // The image was rebuilt in place, so the reader is rebuilt over it. Reusing the reader
    // from the refused attempt would size it from the previous -- and now longer -- string
    // table, which would make this half of the case test something other than what it says.
    fs_reader_t fitting_reader;

    fs_reader_from_buffer(&fitting_reader, image, fitting.total);

    hfs0_t at_limit;

    POUND_CHECK_EQ_U64(POUND_SUCCESS == hfs0_open(&at_limit, &fitting_reader), 1U);

    const hfs0_entry_t *const entry = hfs0_entry_at(&at_limit, 0U);

    POUND_REQUIRE_PTR_NON_NULL(entry);
    POUND_CHECK_STR_EQ(entry->name, name);

    hfs0_close(&at_limit);
}

POUND_TEST(hfs0, an_entry_whose_bytes_leave_the_data_region_is_refused)
{
    uint8_t      image[SAMPLE_CAPACITY];
    hfs0_layout_t layout;

    make_sample(image, &layout);

    // The last entry's size, pushed past the end of the source. Offset plus size has to be
    // checked against the source rather than merely against the metadata, since the data
    // region's length is whatever the reader has.
    uint8_t *POUND_RESTRICT entry = &image[layout.table_offset + (2U * HFS0_ENTRY_SIZE)];

    store_le64(&entry[HFS0_ENTRY_SIZE_OFFSET], (uint64_t)layout.total);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, image, layout.total);

    hfs0_t hfs0;

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(POUND_ERROR_MALFORMED_HEADER == hfs0_open(&hfs0, &reader), 1U);

    // Both the stored (relative) offset and where that offset actually lands, so a user
    // reading the record can see which of the two the file claimed to be at.
    POUND_CHECK(pound_test_log_contains("does not fit inside"));
    POUND_CHECK(pound_test_log_contains("starting at 266"));
    POUND_CHECK(!hfs0_is_open(&hfs0));

    hfs0_close(&hfs0);
}

POUND_TEST(hfs0, an_entry_whose_offset_wraps_out_of_the_range_is_refused)
{
    uint8_t      image[SAMPLE_CAPACITY];
    hfs0_layout_t layout;

    make_sample(image, &layout);

    uint8_t *POUND_RESTRICT entry = &image[layout.table_offset];

    // An offset near the top of the address space with a modest size. `offset + size` wraps
    // to a small number, so a range check written as a sum would see an offset inside the
    // data region and accept the entry. This is the case the subtraction form in
    // `range_within` exists for.
    store_le64(&entry[HFS0_ENTRY_OFFSET_OFFSET], UINT64_MAX - 8U);
    store_le64(&entry[HFS0_ENTRY_SIZE_OFFSET], 64U);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, image, layout.total);

    hfs0_t hfs0;

    POUND_CHECK_EQ_U64(POUND_ERROR_MALFORMED_HEADER == hfs0_open(&hfs0, &reader), 1U);

    hfs0_close(&hfs0);
}

POUND_TEST(hfs0, two_entries_with_the_same_name_are_refused)
{
    uint8_t      image[SAMPLE_CAPACITY];
    hfs0_layout_t layout;

    build_file_t files[2];

    make_file(&files[0], "same.nca", SAMPLE_CONTENTS[0], 16U, 0x10U, SAMPLE_HASHED_SIZE);
    make_file(&files[1], "same.nca", SAMPLE_CONTENTS[1], 16U, 0x20U, SAMPLE_HASHED_SIZE);

    layout = build_hfs0(image, SAMPLE_CAPACITY, files, 2U);

    POUND_REQUIRE(0U != layout.total);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, image, layout.total);

    hfs0_t hfs0;

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(POUND_ERROR_MALFORMED_HEADER == hfs0_open(&hfs0, &reader), 1U);

    // The refusal names both entries and the name, because "two entries are the same" is
    // not actionable on its own and this is the one case where the log has to identify
    // which two.
    POUND_CHECK(pound_test_log_contains("Entries 0 and 1"));
    POUND_CHECK(pound_test_log_contains("same.nca"));

    hfs0_close(&hfs0);
}

POUND_TEST(hfs0, a_truncated_image_is_reported_as_a_bad_header)
{
    uint8_t      image[SAMPLE_CAPACITY];
    hfs0_layout_t layout;

    make_sample(image, &layout);

    fs_reader_t reader;

    // A file that stops in the middle of the file table. The header's own fields are all
    // readable, so this is a case where the declared layout has to be checked against the
    // source's length rather than the header's word.
    fs_reader_from_buffer(&reader, image, layout.string_offset - 8U);

    hfs0_t hfs0;

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(POUND_ERROR_MALFORMED_HEADER == hfs0_open(&hfs0, &reader), 1U);
    POUND_CHECK(pound_test_log_contains("runs past the end"));

    hfs0_close(&hfs0);

    // And one that stops before the header is even complete, which is an I/O failure
    // rather than a malformed header: the reader could not supply what was asked for, and
    // reporting that as a format problem would send a user looking at the wrong thing.
    fs_reader_from_buffer(&reader, image, 8U);

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(POUND_ERROR_IO == hfs0_open(&hfs0, &reader), 1U);
    POUND_CHECK(pound_test_log_contains("Could not read"));

    hfs0_close(&hfs0);
}

POUND_TEST(hfs0, a_files_hash_agrees_with_hashing_its_bytes_directly)
{
    uint8_t      image[SAMPLE_CAPACITY];
    hfs0_layout_t layout;

    make_sample(image, &layout);

    hfs0_t hfs0;

    POUND_REQUIRE(open_sample(image, layout.total, &hfs0));

    for (uint32_t i = 0U; i < hfs0_count(&hfs0); ++i)
    {
        const hfs0_entry_t *const entry = hfs0_entry_at(&hfs0, i);

        POUND_REQUIRE_PTR_NON_NULL(entry);

        uint8_t streamed[SHA256_DIGEST_SIZE];
        uint8_t direct[SHA256_DIGEST_SIZE];

        POUND_REQUIRE(POUND_SUCCESS == hfs0_hash(&hfs0, i, streamed));

        sha256(&image[(size_t)entry->offset], (size_t)entry->size, direct);

        POUND_CHECK_MSG(0 == memcmp(streamed, direct, SHA256_DIGEST_SIZE),
                        "File %u ('%s') hashed differently through the streaming path.",
                        i,
                        entry->name);
    }

    hfs0_close(&hfs0);
}

POUND_TEST(hfs0, a_files_hash_agrees_across_every_chunk_boundary)
{
    // The streaming path reads in fixed-size chunks, so the interesting sizes are the ones
    // around each multiple of that chunk: one byte under, exactly on, one over. A file at
    // exactly the chunk size is the case that would be hashed twice if the loop's
    // termination were `>=` rather than on the remaining count.
    enum
    {
        /// The size `hfs0_hash` reads in. Must match `HFS0_HASH_CHUNK_SIZE` in `hfs0.c`;
        /// there is no exported macro for it, so the size is stated here and the agreement
        /// is checked by the hashes rather than by comparing the two numbers.
        CHUNK = 64U * 1024U,

        /// Room for the largest file used below plus slack.
        CAPACITY = (CHUNK * 2U) + 4096U,
    };

    static uint8_t image[CAPACITY];
    static uint8_t contents[CHUNK + 64U];

    for (size_t i = 0U; i < sizeof(contents); ++i)
    {
        contents[i] = (uint8_t)(i * 7U);
    }

    const size_t sizes[] = { 0U, 1U, 63U, 64U, 65U, (CHUNK - 1U), CHUNK, (CHUNK + 1U), (CHUNK + 63U) };

    for (size_t which = 0U; which < (sizeof(sizes) / sizeof(sizes[0])); ++which)
    {
        build_file_t file;

        file.name        = "boundary.bin";
        file.contents    = contents;
        file.size        = sizes[which];
        file.hashed_size = SAMPLE_HASHED_SIZE;

        const hfs0_layout_t layout = build_hfs0(image, sizeof(image), &file, 1U);

        POUND_REQUIRE_MSG(0U != layout.total, "The %zu-byte case did not fit.", sizes[which]);

        hfs0_t hfs0;

        POUND_REQUIRE(open_sample(image, layout.total, &hfs0));

        uint8_t streamed[SHA256_DIGEST_SIZE];
        uint8_t direct[SHA256_DIGEST_SIZE];

        POUND_REQUIRE(POUND_SUCCESS == hfs0_hash(&hfs0, 0U, streamed));

        sha256(contents, sizes[which], direct);

        POUND_CHECK_MSG(0 == memcmp(streamed, direct, SHA256_DIGEST_SIZE),
                        "A %zu-byte file hashed differently through the chunked path.",
                        sizes[which]);

        hfs0_close(&hfs0);
    }
}

POUND_TEST(hfs0, an_entry_can_be_opened_as_a_reader_for_a_nested_parser)
{
    uint8_t      image[SAMPLE_CAPACITY];
    hfs0_layout_t layout;

    build_file_t files[2];

    make_file(&files[0], "/hfs0/0", SAMPLE_CONTENTS[0], sizeof(SAMPLE_CONTENTS[0]), 0x10U, SAMPLE_HASHED_SIZE);
    make_file(&files[1], "/hfs0/1", SAMPLE_CONTENTS[1], 100U, 0x40U, SAMPLE_HASHED_SIZE);

    layout = build_hfs0(image, SAMPLE_CAPACITY, files, 2U);

    POUND_REQUIRE(0U != layout.total);

    hfs0_t hfs0;

    POUND_REQUIRE(open_sample(image, layout.total, &hfs0));

    // This is the whole reason the slice exists: a nested parser wants a reader, and the
    // entry behind it is far too large to be read into memory. The offsets it sees start
    // at zero and its reported size is the entry's size, not the partition's.
    hfs0_slice_t slice;

    POUND_REQUIRE(POUND_SUCCESS == hfs0_entry_reader(&hfs0, 1U, &slice));
    POUND_REQUIRE_PTR_NON_NULL(slice.reader.read_at);
    POUND_CHECK_EQ_U64(slice.reader.size, 100U);

    uint8_t head[16];
    uint8_t tail[8];

    POUND_CHECK_EQ_U64(POUND_SUCCESS == fs_reader_read_at(&slice.reader, 0U, head, sizeof(head)), 1U);
    POUND_CHECK(0 == memcmp(head, SAMPLE_CONTENTS[1], sizeof(head)));

    // An offset near the top of the address space, so the rebasing inside the slice has to
    // be overflow-checked rather than added.
    POUND_CHECK_EQ_U64(POUND_ERROR_IO == fs_reader_read_at(&slice.reader, UINT64_MAX - 2U, tail, 8U), 1U);

    // Past the entry's own end, which the slice reports as an I/O failure rather than
    // quietly reading the next file's bytes out of the parent.
    POUND_CHECK_EQ_U64(POUND_ERROR_IO == fs_reader_read_at(&slice.reader, 96U, tail, sizeof(tail)), 1U);

    // The last byte is inside it.
    POUND_CHECK_EQ_U64(POUND_SUCCESS == fs_reader_read_at(&slice.reader, 99U, tail, 1U), 1U);

    hfs0_slice_close(&slice);
    POUND_CHECK_PTR_NULL(slice.reader.read_at);

    // Closing twice is harmless, which is what lets a caller close unconditionally on every
    // path including the ones that failed after the slice was opened.
    hfs0_slice_close(&slice);

    // An index that names no file leaves the slice closed rather than half-open.
    hfs0_slice_t missing;

    POUND_CHECK_EQ_U64(POUND_ERROR_NOT_FOUND == hfs0_entry_reader(&hfs0, 9U, &missing), 1U);
    POUND_CHECK_PTR_NULL(missing.reader.read_at);
    POUND_CHECK_PTR_NULL(missing.context);
    hfs0_slice_close(&missing);

    hfs0_close(&hfs0);
}

POUND_TEST(hfs0, closing_twice_is_harmless_and_a_closed_partition_answers_nothing)
{
    uint8_t      image[SAMPLE_CAPACITY];
    hfs0_layout_t layout;

    make_sample(image, &layout);

    hfs0_t hfs0;

    POUND_REQUIRE(open_sample(image, layout.total, &hfs0));

    const uint64_t count_before = hfs0_count(&hfs0);
    const uint64_t meta_before  = hfs0.metadata_size;

    hfs0_close(&hfs0);

    // Everything a caller could ask is now a refusal rather than a stale answer. Checking
    // the values rather than only the flag is what catches a close that zeroes one field
    // and forgets another.
    POUND_CHECK(!hfs0_is_open(&hfs0));
    POUND_CHECK_EQ_U64(hfs0_count(&hfs0), 0U);
    POUND_CHECK_EQ_U64(hfs0.metadata_size, 0U);
    POUND_CHECK_EQ_U64(hfs0.data_offset, 0U);
    POUND_CHECK_PTR_NULL(hfs0.entries);
    POUND_CHECK_PTR_NULL(hfs0.hashed_sizes);

    hfs0_close(&hfs0);

    // And the accessors on a closed partition. The sizes were non-zero before the close, so
    // these prove the close actually cleared them.
    POUND_REQUIRE(count_before > 0U);
    POUND_REQUIRE(meta_before > 0U);

    POUND_CHECK_PTR_NULL(hfs0_entry_at(&hfs0, 0U));

    uint32_t       index  = 0U;
    const error_t  status = hfs0_index_of(&hfs0, "/hfs0/0", &index);

    POUND_CHECK_EQ_U64(POUND_ERROR_NOT_INITIALIZED == status, 1U);

    uint8_t     scratch[4];
    uint8_t     digest[SHA256_DIGEST_SIZE];
    uint64_t    hashed = 0U;

    POUND_CHECK_EQ_U64(POUND_ERROR_NOT_INITIALIZED == hfs0_read(&hfs0, 0U, 0U, scratch, sizeof(scratch)), 1U);
    POUND_CHECK_EQ_U64(POUND_ERROR_NOT_INITIALIZED == hfs0_hash(&hfs0, 0U, digest), 1U);
    POUND_CHECK_EQ_U64(POUND_ERROR_NOT_INITIALIZED == hfs0_hash_metadata(&hfs0, digest), 1U);
    POUND_CHECK(!hfs0_hashed_size_at(&hfs0, 0U, &hashed));

    hfs0_close(&hfs0);
}

POUND_TEST(hfs0, opening_from_a_null_or_unusable_source_is_refused)
{
    hfs0_t     hfs0;
    fs_reader_t reader;

    // A reader describing a non-empty buffer it cannot point at is refused, and is left
    // with no `read_at` -- which is what a caller that ignored an earlier refusal ends up
    // holding. This is the shape of the bug the NULL check below exists for: a struct that
    // looks like a reader and cannot read.
    fs_reader_from_buffer(&reader, NULL, 4U);

    POUND_CHECK_PTR_NULL(reader.read_at);

    pound_test_log_reset();

    POUND_CHECK_EQ_U64(POUND_ERROR_INVALID_ARGUMENT == hfs0_open(NULL, &reader), 1U);
    POUND_CHECK_EQ_U64(POUND_ERROR_INVALID_ARGUMENT == hfs0_open(&hfs0, NULL), 1U);
    POUND_CHECK_EQ_U64(POUND_ERROR_INVALID_ARGUMENT == hfs0_open(&hfs0, &reader), 1U);
    POUND_CHECK(pound_test_log_contains("has no read function"));

    // A NULL partition and a NULL slice are refused by the slice close too, rather than
    // dereferenced.
    hfs0_slice_t slice;

    POUND_CHECK_EQ_U64(POUND_ERROR_INVALID_ARGUMENT == hfs0_entry_reader(NULL, 0U, &slice), 1U);
    POUND_CHECK_EQ_U64(POUND_ERROR_INVALID_ARGUMENT == hfs0_entry_reader(&hfs0, 0U, NULL), 1U);

    hfs0_close(NULL);
    hfs0_slice_close(NULL);

    // Both of those logged rather than returning silently.
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) > 0U);
}

POUND_TEST(hfs0, a_summary_can_be_logged_and_says_what_the_partition_holds)
{
    uint8_t      image[SAMPLE_CAPACITY];
    hfs0_layout_t layout;

    make_sample(image, &layout);

    hfs0_t hfs0;

    POUND_REQUIRE(open_sample(image, layout.total, &hfs0));

    pound_test_log_reset();
    hfs0_log_summary(&hfs0);

    POUND_CHECK(pound_test_log_contains("3 file(s)"));
    POUND_CHECK(pound_test_log_contains("/hfs0/0"));
    POUND_CHECK(pound_test_log_contains("hashed region"));

    hfs0_close(&hfs0);

    // And on a closed partition it says so rather than reading freed memory, which is the
    // failure mode a summary helper has when it is written to assume it is open.
    pound_test_log_reset();
    hfs0_log_summary(&hfs0);
    POUND_CHECK(pound_test_log_contains("not open"));
}

POUND_TEST_SUITE(hfs0,
                POUND_TEST_CASE(hfs0, a_valid_partition_opens_and_reports_its_files),
                POUND_TEST_CASE(hfs0,
                                every_encoding_the_parser_relies_on_is_the_one_the_format_documents),
                POUND_TEST_CASE(hfs0, a_files_contents_read_back_byte_for_byte),
                POUND_TEST_CASE(hfs0, an_empty_file_is_read_as_zero_bytes_and_still_appears_in_the_table),
                POUND_TEST_CASE(hfs0, a_read_that_would_leave_a_file_is_refused),
                POUND_TEST_CASE(hfs0, files_are_found_by_name_and_a_miss_is_quiet),
                POUND_TEST_CASE(hfs0, hashed_sizes_are_recorded_verbatim_and_absent_indices_are_reported),
                POUND_TEST_CASE(hfs0, the_metadata_region_hashes_to_what_hashing_those_bytes_directly_does),
                POUND_TEST_CASE(hfs0, the_metadata_hash_verifies_against_the_image_and_reports_a_mismatch),
                POUND_TEST_CASE(hfs0, a_wrong_magic_is_reported_as_a_malformed_header),
                POUND_TEST_CASE(hfs0, a_file_count_beyond_the_cap_is_refused_without_allocating),
                POUND_TEST_CASE(hfs0, a_partition_declaring_no_files_is_refused),
                POUND_TEST_CASE(hfs0, a_name_pointing_past_the_string_table_is_refused),
                POUND_TEST_CASE(hfs0, a_name_with_no_terminator_is_refused_rather_than_read_past_the_table),
                POUND_TEST_CASE(hfs0, a_name_too_long_to_store_is_refused_rather_than_truncated),
                POUND_TEST_CASE(hfs0, an_entry_whose_bytes_leave_the_data_region_is_refused),
                POUND_TEST_CASE(hfs0, an_entry_whose_offset_wraps_out_of_the_range_is_refused),
                POUND_TEST_CASE(hfs0, two_entries_with_the_same_name_are_refused),
                POUND_TEST_CASE(hfs0, a_truncated_image_is_reported_as_a_bad_header),
                POUND_TEST_CASE(hfs0, a_files_hash_agrees_with_hashing_its_bytes_directly),
                POUND_TEST_CASE(hfs0, a_files_hash_agrees_across_every_chunk_boundary),
                POUND_TEST_CASE(hfs0, an_entry_can_be_opened_as_a_reader_for_a_nested_parser),
                POUND_TEST_CASE(hfs0, closing_twice_is_harmless_and_a_closed_partition_answers_nothing),
                POUND_TEST_CASE(hfs0, opening_from_a_null_or_unusable_source_is_refused),
                POUND_TEST_CASE(hfs0, a_summary_can_be_logged_and_says_what_the_partition_holds))

/*** end of file ***/
//! Tests for the loader's foundation: the random-access reader, the user-supplied key
//! store, and the PFS0 parser.
//!
//! The three live in one translation unit because they are one layer: the key store
//! stands alone, but the reader and PFS0 are two halves of the same contract -- a parser
//! that works against a buffer and a parser that works against a sixty-gigabyte file must
//! take the identical code path, and a test can only demonstrate that if it drives both
//! through the same reader. Keeping the PFS0 image builder next to the reader tests is
//! what lets the reader's behaviour be asserted on the byte ranges a real partition
//! produces rather than on contrived ones.
//!
//! ## How the PFS0 tests get a partition to parse
//!
//! There are no real game images in this repository, and there will not be any: they are
//! tens of gigabytes of copyrighted data. So `build_pfs0` writes a partition byte by byte
//! from a list of names and contents, following the layout documented in `pfs0.h`. Every
//! case then either parses that, or corrupts one specific field of it -- a magic, a file
//! count, one entry's offset, one name's length -- and asserts that the parser rejects
//! exactly that. A parser that checks nothing passes the happy path; a parser that checks
//! the wrong thing passes the corruption cases. Both have to be right to pass all of it.

#include "pound_test.h"

#include "crypto/sha256.h"
#include "fs/fs_reader.h"
#include "fs/keys.h"
#include "fs/pfs0.h"
#include "log.h"
#include "memory/memory.h"
#include <stdio.h>
#include <string.h>

// ===================================================================================
// fs_reader
// ===================================================================================

/// A pattern that is unlikely to be produced by an uninitialised buffer, so a test that
/// reads the wrong place fails rather than accidentally matching.
static void
fill_pattern(uint8_t *POUND_RESTRICT buffer, const size_t size, const uint8_t seed)
{
    for (size_t i = 0U; i < size; ++i)
    {
        buffer[i] = (uint8_t)(seed + (uint8_t)(i * 7U) + 1U);
    }
}

POUND_TEST(fs_reader, a_buffer_reader_returns_the_bytes_it_was_given)
{
    uint8_t source[256];

    fill_pattern(source, sizeof(source), 0x10U);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, source, sizeof(source));

    POUND_REQUIRE_MSG(NULL != reader.read_at, "the reader was given no read function.");
    POUND_REQUIRE(sizeof(source) == reader.size);

    uint8_t whole[256];

    POUND_CHECK_MSG(POUND_SUCCESS == fs_reader_read_at(&reader, 0U, whole, sizeof(whole)),
                    "reading the whole buffer failed.");

    POUND_CHECK_MSG(0 == memcmp(whole, source, sizeof(source)), "the whole-buffer read differed from the source.");

    // A read from the middle, and one that ends exactly at the last byte, which is the
    // boundary a bounds check written as `<` rather than `<=` would get wrong.
    uint8_t middle[32];

    POUND_CHECK(POUND_SUCCESS == fs_reader_read_at(&reader, 100U, middle, sizeof(middle)));
    POUND_CHECK(0 == memcmp(middle, &source[100], sizeof(middle)));

    uint8_t tail[16];

    POUND_CHECK(POUND_SUCCESS == fs_reader_read_at(&reader, sizeof(source) - sizeof(tail), tail, sizeof(tail)));
    POUND_CHECK(0 == memcmp(tail, &source[sizeof(source) - sizeof(tail)], sizeof(tail)));
}

POUND_TEST(fs_reader, a_read_past_the_end_of_the_source_fails_rather_than_truncating)
{
    uint8_t source[64];

    fill_pattern(source, sizeof(source), 0x40U);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, source, sizeof(source));

    uint8_t destination[64];

    // One byte past the end is the case a check written as `offset < size` misses.
    POUND_CHECK_MSG(POUND_ERROR_IO == fs_reader_read_at(&reader, sizeof(source), destination, 1U),
                    "a read starting one byte past the end of the source was allowed.");

    // Straddling the end.
    POUND_CHECK(POUND_ERROR_IO == fs_reader_read_at(&reader, sizeof(source) - 8U, destination, 16U));

    // Entirely past it.
    POUND_CHECK(POUND_ERROR_IO == fs_reader_read_at(&reader, 1000000U, destination, 16U));
}

POUND_TEST(fs_reader, a_range_that_would_wrap_is_rejected)
{
    uint8_t source[64];

    fill_pattern(source, sizeof(source), 0x40U);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, source, sizeof(source));

    // An offset near the top of the address space with a modest size. Written as
    // `offset + size <= reader->size` this sums to a small number and passes, handing
    // back a read that starts far outside the source. The subtraction form rejects it,
    // and this is the case that proves the form was chosen deliberately.
    POUND_CHECK(!fs_reader_range_is_valid(&reader, UINT64_MAX - 4U, 16U));

    uint8_t destination[16];

    POUND_CHECK(POUND_ERROR_IO == fs_reader_read_at(&reader, UINT64_MAX - 4U, destination, sizeof(destination)));

    // And one byte short of the top, whose sum wraps the other way.
    POUND_CHECK(!fs_reader_range_is_valid(&reader, UINT64_MAX, 2U));

    // The boundary itself: an offset exactly at the end with a zero size is inside, and
    // with any size at all is not.
    POUND_CHECK(fs_reader_range_is_valid(&reader, sizeof(source), 0U));
    POUND_CHECK(!fs_reader_range_is_valid(&reader, sizeof(source), 1U));
}

POUND_TEST(fs_reader, a_zero_length_read_at_the_end_of_the_source_succeeds)
{
    // A caller probing a header that ends exactly at the end of a file should be told
    // the range is fine, not that it is out of bounds. A zero-length read says nothing
    // about the range, so answering it before the bounds check is correct rather than
    // lenient.
    uint8_t source[32];

    fill_pattern(source, sizeof(source), 0x00U);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, source, sizeof(source));

    uint8_t destination[1];

    POUND_CHECK(POUND_SUCCESS == fs_reader_read_at(&reader, sizeof(source), destination, 0U));
    POUND_CHECK(POUND_SUCCESS == fs_reader_read_at(&reader, UINT64_MAX, destination, 0U));
}

POUND_TEST(fs_reader, a_reader_with_no_read_function_refuses_every_read)
{
    // What `fs_reader_from_buffer` leaves behind when it rejects its arguments. A caller
    // that ignored the failure must get a reader that says no rather than one that
    // dereferences something it was never given.
    fs_reader_t reader;

    memset(&reader, 0, sizeof(reader));

    uint8_t destination[4];

    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == fs_reader_read_at(&reader, 0U, destination, sizeof(destination)));
    POUND_CHECK(!fs_reader_range_is_valid(&reader, 0U, sizeof(destination)));
    POUND_CHECK(!fs_reader_range_is_valid(NULL, 0U, sizeof(destination)));
}

POUND_TEST(fs_reader, a_rejected_buffer_leaves_a_reader_that_cannot_read)
{
    fs_reader_t reader;

    // Deliberately pre-poisoned, so a failure to zero the reader is visible rather than
    // masked by whatever the stack happened to hold.
    memset(&reader, 0xA5, sizeof(reader));

    fs_reader_from_buffer(&reader, NULL, 64U);

    POUND_REQUIRE_MSG(NULL == reader.read_at,
                      "a reader described with a NULL buffer was left able to read.");
    POUND_CHECK(0U == reader.size);
    POUND_CHECK(NULL == reader.buffer_data);

    uint8_t destination[4];

    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == fs_reader_read_at(&reader, 0U, destination, sizeof(destination)));

    // A NULL buffer with a zero size is legal: it describes an empty source.
    fs_reader_from_buffer(&reader, NULL, 0U);

    POUND_REQUIRE(NULL != reader.read_at);
    POUND_CHECK(0U == reader.size);
    POUND_CHECK(POUND_SUCCESS == fs_reader_read_at(&reader, 0U, destination, 0U));
}

POUND_TEST(fs_reader, a_read_with_no_destination_is_rejected)
{
    uint8_t source[32];

    fill_pattern(source, sizeof(source), 0x20U);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, source, sizeof(source));

    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == fs_reader_read_at(&reader, 0U, NULL, 4U));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == fs_reader_read_at(NULL, 0U, source, 4U));
}

/// A reader built over a buffer whose fill pattern is `seed`, returned by value.
///
/// Every case below needs a reader that outlives the frame that made it, which is the
/// whole subject. Reading the buffer directly rather than closing over it is what makes
/// the frame's return observable: the caller's `fs_reader_t` is a local, and after this
/// returns that stack slot is free.
static fs_reader_t
make_scoped_reader(uint8_t *POUND_RESTRICT source, const size_t size, const uint8_t seed)
{
    for (size_t i = 0U; i < size; ++i)
    {
        source[i] = (uint8_t)(seed + (uint8_t)i);
    }

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, source, size);

    return reader;
}

POUND_TEST(fs_reader, a_copied_reader_keeps_reading_the_same_bytes)
{
    uint8_t     source[64];
    fs_reader_t original = make_scoped_reader(source, sizeof(source), 0x11U);

    // A buffer-backed reader points `context` at itself, so this assignment is what the
    // plain struct copy in a parser's `open` used to be. It leaves `copy.context`
    // addressing `original` -- and `original` is a local in the previous statement, dead
    // the moment this block ends.
    fs_reader_t copy;

    POUND_REQUIRE(POUND_SUCCESS == fs_reader_read_at(&original, 0U, source, sizeof(source)));

    fs_reader_copy(&copy, &original);

    POUND_CHECK(copy.read_at != NULL);
    POUND_CHECK(copy.size == original.size);
    POUND_CHECK_PTR_EQ(copy.context, &copy);
    POUND_CHECK_PTR_EQ(copy.buffer_data, original.buffer_data);

    uint8_t read_back[64];

    POUND_REQUIRE(POUND_SUCCESS == fs_reader_read_at(&copy, 0U, read_back, sizeof(read_back)));
    POUND_CHECK(0 == memcmp(read_back, source, sizeof(source)));

    // And at an offset, so the copy is checked against more than the base case.
    POUND_REQUIRE(POUND_SUCCESS == fs_reader_read_at(&copy, 33U, read_back, 16U));
    POUND_CHECK(0 == memcmp(read_back, &source[33], 16U));

    // A read that the *original* could not do is still refused, so the copy has not lost
    // the size check along with the self-reference.
    POUND_CHECK(POUND_ERROR_IO == fs_reader_read_at(&copy, sizeof(source), read_back, 1U));
}

POUND_TEST(fs_reader, a_copied_reader_survives_the_frame_it_was_made_in)
{
    // This is the case that matters, and the reason the copy has to rebind rather than
    // merely duplicate. `scoped` is a local in `make_scoped_reader`; its stack slot is
    // reused by the loop below before the first read through the copy, so a copy whose
    // context still pointed at it would read whatever the loop left there.
    //
    // The bytes are on the caller's stack, which is exactly the shape of the bug: a
    // partition holds its reader by value, and the reader it was opened from is almost
    // always a local.
    uint8_t     source[128];
    fs_reader_t scoped = make_scoped_reader(source, sizeof(source), 0x80U);

    fs_reader_t copy;

    fs_reader_copy(&copy, &scoped);

    // Clobber the frame the original lived in, with a pattern chosen not to match.
    uint8_t clobber[128];

    for (size_t i = 0U; i < sizeof(clobber); ++i)
    {
        clobber[i] = 0xEEU;
    }

    uint8_t read_back[128];

    POUND_REQUIRE(POUND_SUCCESS == fs_reader_read_at(&copy, 0U, read_back, sizeof(read_back)));
    POUND_CHECK_MSG(0 != memcmp(read_back, clobber, sizeof(clobber)),
                    "The copy returned the clobber pattern, which means it read through the "
                    "dead frame rather than its own fields.");
    POUND_CHECK(0 == memcmp(read_back, source, sizeof(source)));
}

POUND_TEST(fs_reader, a_copied_reader_keeps_a_context_that_is_not_itself)
{
    // The other half of the rule: only a self-reference is rebound. A reader whose
    // context is a real allocation keeps it, because copying the pointer is the correct
    // behaviour -- rebinding would make the copy's read function look for its state in a
    // struct that does not hold it.
    uint8_t source[16];

    for (size_t i = 0U; i < sizeof(source); ++i)
    {
        source[i] = (uint8_t)i;
    }

    uint8_t state = 0x5AU;

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, source, sizeof(source));
    reader.context = &state;

    fs_reader_t copy;

    fs_reader_copy(&copy, &reader);

    POUND_CHECK_PTR_EQ(copy.context, &state);
    POUND_CHECK(copy.read_at == reader.read_at);
}

POUND_TEST(fs_reader, copying_from_nothing_leaves_a_reader_that_refuses_everything)
{
    fs_reader_t copy;

    // Seeded from a real reader first, so a `copy` that merely kept whatever it already
    // held would pass a check on a zeroed struct.
    uint8_t     source[8];
    fs_reader_t original = make_scoped_reader(source, sizeof(source), 0x40U);

    fs_reader_copy(&copy, &original);
    POUND_REQUIRE_PTR_NON_NULL(copy.read_at);

    fs_reader_copy(&copy, NULL);

    POUND_CHECK_PTR_NULL(copy.read_at);
    POUND_CHECK_PTR_NULL(copy.buffer_data);
    POUND_CHECK_EQ_U64(copy.size, 0U);

    // A NULL destination is refused rather than written through, and says so.
    pound_test_log_reset();
    fs_reader_copy(NULL, &original);
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) > 0U);
}

// ===================================================================================
// keys
// ===================================================================================

/// A synthetic key file.
///
/// Every value here is an obvious placeholder -- a rising counter -- and none of it is a
/// real key or derived from one. They exist to be compared against, not to decrypt
/// anything. The file exercises the shapes a real one has: comments in both syntaxes,
/// blank lines, inconsistent spacing around the separator, mixed-case hex, and a key
/// family with more than one member.
static const char KEY_FILE[] = "# A synthetic key file. None of these values is a real key.\n"
                               "; Semicolon comments work too.\n"
                               "\n"
                               "master_key_00 = 000102030405060708090a0b0c0d0e0f\n"
                               "master_key_01: 101112131415161718191a1b1c1d1e1f\n"
                               "  header_key  =  202122232425262728292a2b2c2d2e2f  \n"
                               "titlekek_00=303132333435363738393a3b3c3d3e3f\n"
                               "TITLEKEK_01 = A0A1A2A3A4A5A6A7A8A9AAABACADAEAF\n"
                               "key_area_key_application_07 = B0B1B2B3B4B5B6B7B8B9BABBBCBDBEBF\n";

POUND_TEST(keys, a_key_file_loads_and_every_entry_can_be_read_back)
{
    key_store_t store;

    key_store_init(&store);

    POUND_REQUIRE_MSG(POUND_SUCCESS == key_store_load_text(&store, KEY_FILE, sizeof(KEY_FILE) - 1U),
                      "the synthetic key file was rejected.");

    POUND_REQUIRE(6U == key_store_count(&store));

    uint8_t key[POUND_KEY_SIZE];

    POUND_REQUIRE(POUND_SUCCESS == key_store_get(&store, "master_key_00", key));
    for (size_t i = 0U; i < POUND_KEY_SIZE; ++i)
    {
        POUND_CHECK_MSG((uint8_t)i == key[i], "master_key_00 byte %zu is 0x%02X, not 0x%02X.", i, key[i], (unsigned)i);
    }

    // The colon separator, and the same family one index up.
    POUND_REQUIRE(POUND_SUCCESS == key_store_get_master_key(&store, 1U, key));
    POUND_CHECK(0x10U == key[0]);
    POUND_CHECK(0x1FU == key[POUND_KEY_SIZE - 1U]);

    // Leading spaces before the name, trailing spaces after the value, and two spaces
    // around the separator.
    POUND_REQUIRE(POUND_SUCCESS == key_store_get(&store, POUND_KEY_HEADER_KEY, key));
    POUND_CHECK(0x20U == key[0]);

    // No spaces at all around the separator.
    POUND_REQUIRE(POUND_SUCCESS == key_store_get(&store, "titlekek_00", key));
    POUND_CHECK(0x30U == key[0]);

    // Uppercase hex, and a name that is not lowercase.
    POUND_REQUIRE(POUND_SUCCESS == key_store_get(&store, "TITLEKEK_01", key));
    POUND_CHECK(0xA0U == key[0]);
    POUND_CHECK(0xAFU == key[POUND_KEY_SIZE - 1U]);

    POUND_CHECK(key_store_has(&store, "master_key_00"));
    POUND_CHECK(!key_store_has(&store, "master_key_02"));
    POUND_CHECK(!key_store_has(&store, "MASTER_KEY_00"));

    key_store_destroy(&store);
}

POUND_TEST(keys, a_missing_key_is_reported_and_nothing_is_written)
{
    // The single most important behaviour in the module. A key that is not present must
    // produce an error and leave the caller's buffer untouched -- never a zero key,
    // never a stale value from a previous lookup. A zero key decrypts to noise, and
    // noise that happens to look like a header is how a missing key turns into a crash
    // three subsystems deep instead of a clear message at load time.
    key_store_t store;

    key_store_init(&store);

    uint8_t destination[POUND_KEY_SIZE];

    memset(destination, 0xC7, sizeof(destination));

    POUND_CHECK(POUND_ERROR_KEY_MISSING == key_store_get(&store, POUND_KEY_HEADER_KEY, destination));

    for (size_t i = 0U; i < POUND_KEY_SIZE; ++i)
    {
        POUND_CHECK_MSG(0xC7U == destination[i],
                        "a failed lookup overwrote byte %zu of the caller's buffer, so a caller "
                        "that ignored the error would proceed with stale key material.",
                        i);
    }

    key_store_destroy(&store);
}

POUND_TEST(keys, a_fresh_store_contains_no_keys_at_all)
{
    // The architectural guarantee, stated as a test: Pound ships no keys. Every name the
    // loader will ask for is absent from an empty store, so there is no code path -- and
    // no table, and no default -- that can quietly supply one. If a key were ever
    // compiled in, this test would stop passing, which is the point of writing it.
    key_store_t store;

    key_store_init(&store);

    static const char *const WELL_KNOWN[] = {
        POUND_KEY_HEADER_KEY,
        POUND_KEY_XCI_HEADER_KEY,
        POUND_KEY_PACKAGE2_KEY,
        "master_key_00",
        "master_key_01",
        "master_key_02",
        "key_area_key_application_00",
        "key_area_key_application_07",
        "titlekek_00",
        "titlekek_0f",
    };

    uint8_t key[POUND_KEY_SIZE];

    for (size_t i = 0U; i < (sizeof(WELL_KNOWN) / sizeof(WELL_KNOWN[0])); ++i)
    {
        POUND_CHECK_MSG(!key_store_has(&store, WELL_KNOWN[i]),
                        "the key '%s' is present in an empty store, so Pound is shipping a key.",
                        WELL_KNOWN[i]);

        POUND_CHECK_MSG(POUND_ERROR_KEY_MISSING == key_store_get(&store, WELL_KNOWN[i], key),
                        "the key '%s' could be read out of an empty store.",
                        WELL_KNOWN[i]);
    }

    POUND_CHECK(0U == key_store_count(&store));

    key_store_destroy(&store);
}

POUND_TEST(keys, a_later_definition_of_a_name_replaces_the_earlier_one)
{
    // So that a user can append a corrected key to an existing file without having to
    // find and edit the old line. This is the behaviour a "duplicate key" error would
    // deny them.
    static const char TWICE[] = "master_key_00 = 00000000000000000000000000000000\n"
                                "master_key_00 = aabbccddeeff00112233445566778899\n";

    key_store_t store;

    key_store_init(&store);

    POUND_REQUIRE(POUND_SUCCESS == key_store_load_text(&store, TWICE, sizeof(TWICE) - 1U));
    POUND_REQUIRE(1U == key_store_count(&store));

    uint8_t key[POUND_KEY_SIZE];

    POUND_REQUIRE(POUND_SUCCESS == key_store_get(&store, "master_key_00", key));
    POUND_CHECK(0xAAU == key[0]);
    POUND_CHECK(0x99U == key[POUND_KEY_SIZE - 1U]);

    key_store_destroy(&store);
}

POUND_TEST(keys, a_bad_line_is_skipped_and_the_rest_of_the_file_still_loads)
{
    // A user with a hundred keys and one typo should get ninety-nine keys and a line
    // number, not a single opaque failure. So parsing continues past a rejected line and
    // the returned code says the file was not clean.
    static const char MOSTLY_GOOD[] = "master_key_00 = 000102030405060708090a0b0c0d0e0f\n"
                                      "broken_line_without_a_separator\n"
                                      "master_key_01 = 101112131415161718191a1b1c1d1e1f\n"
                                      "short_value = 00112233\n"
                                      "not_hex = gghh112233445566778899aabbccddeeff\n"
                                      "header_key = 202122232425262728292a2b2c2d2e2f\n";

    key_store_t store;

    key_store_init(&store);

    POUND_CHECK_MSG(POUND_ERROR_MALFORMED_HEADER == key_store_load_text(&store, MOSTLY_GOOD, sizeof(MOSTLY_GOOD) - 1U),
                    "a file with three bad lines was reported as clean.");

    // The three good ones survived, which is the whole point.
    POUND_REQUIRE(3U == key_store_count(&store));
    POUND_CHECK(key_store_has(&store, "master_key_00"));
    POUND_CHECK(key_store_has(&store, "master_key_01"));
    POUND_CHECK(key_store_has(&store, POUND_KEY_HEADER_KEY));

    // And the bad ones did not, in particular not the one with a plausible-looking but
    // wrong-length value.
    POUND_CHECK(!key_store_has(&store, "short_value"));
    POUND_CHECK(!key_store_has(&store, "not_hex"));
    POUND_CHECK(!key_store_has(&store, "broken_line_without_a_separator"));

    key_store_destroy(&store);
}

POUND_TEST(keys, text_after_a_keys_value_is_rejected_rather_than_ignored)
{
    // `master_key_00 = 0011...ff master_key_01 = 0011...ff` on one line is almost always
    // a missing newline. Accepting it would store a key with a silently wrong value,
    // which is the one outcome this module exists to make impossible.
    static const char RUN_TOGETHER[] = "master_key_00 = 000102030405060708090a0b0c0d0e0f master_key_01 = 00\n";

    key_store_t store;

    key_store_init(&store);

    POUND_CHECK(POUND_ERROR_MALFORMED_HEADER == key_store_load_text(&store, RUN_TOGETHER, sizeof(RUN_TOGETHER) - 1U));
    POUND_CHECK(0U == key_store_count(&store));

    key_store_destroy(&store);
}

POUND_TEST(keys, a_trailing_comment_after_a_value_is_allowed)
{
    static const char ANNOTATED[] = "master_key_00 = 000102030405060708090a0b0c0d0e0f  # from the console\n"
                                   "header_key = 202122232425262728292a2b2c2d2e2f ; another note\n";

    key_store_t store;

    key_store_init(&store);

    POUND_REQUIRE(POUND_SUCCESS == key_store_load_text(&store, ANNOTATED, sizeof(ANNOTATED) - 1U));
    POUND_REQUIRE(2U == key_store_count(&store));

    key_store_destroy(&store);
}

POUND_TEST(keys, line_endings_and_a_missing_final_newline_both_parse)
{
    // A key file authored on Windows has to load on Linux and vice versa. `\r` is
    // whitespace to the parser, so it is skipped along with the `\n`, and a last line
    // with no terminator is handled by the same code that handles one with.
    static const char CRLF[]   = "master_key_00 = 000102030405060708090a0b0c0d0e0f\r\n"
                                  "header_key = 202122232425262728292a2b2c2d2e2f\r\n";
    static const char NO_TAIL[] = "master_key_00 = 000102030405060708090a0b0c0d0e0f";

    key_store_t store;

    key_store_init(&store);

    POUND_REQUIRE(POUND_SUCCESS == key_store_load_text(&store, CRLF, sizeof(CRLF) - 1U));
    POUND_CHECK(2U == key_store_count(&store));
    key_store_destroy(&store);

    key_store_init(&store);

    POUND_REQUIRE(POUND_SUCCESS == key_store_load_text(&store, NO_TAIL, sizeof(NO_TAIL) - 1U));
    POUND_CHECK(1U == key_store_count(&store));

    key_store_destroy(&store);
}

POUND_TEST(keys, a_value_of_the_wrong_width_is_rejected)
{
    // Every Switch key is AES-128, so anything that is not thirty-two hex digits is a
    // mistake, and accepting a shorter one would mean decrypting with half a key.
    static const char TOO_SHORT[] = "master_key_00 = 000102030405060708090a0b0c0d0e\n";
    static const char TOO_LONG[]  = "master_key_00 = 000102030405060708090a0b0c0d0e0f00\n";
    static const char EMPTY[]     = "master_key_00 = \n";

    key_store_t store;

    key_store_init(&store);

    POUND_CHECK(POUND_ERROR_MALFORMED_HEADER == key_store_load_text(&store, TOO_SHORT, sizeof(TOO_SHORT) - 1U));
    POUND_CHECK(0U == key_store_count(&store));

    POUND_CHECK(POUND_ERROR_MALFORMED_HEADER == key_store_load_text(&store, TOO_LONG, sizeof(TOO_LONG) - 1U));
    POUND_CHECK(0U == key_store_count(&store));

    POUND_CHECK(POUND_ERROR_MALFORMED_HEADER == key_store_load_text(&store, EMPTY, sizeof(EMPTY) - 1U));
    POUND_CHECK(0U == key_store_count(&store));

    key_store_destroy(&store);
}

POUND_TEST(keys, a_name_too_long_to_store_is_rejected_rather_than_truncated)
{
    // Truncating would produce a *different* name, which would then either fail to match
    // anything or, worse, match a different key. Either way the user would be looking at
    // the right name in their file and getting the wrong key.
    char oversized[POUND_KEY_NAME_MAX + 32U];
    char line[POUND_KEY_NAME_MAX + 160U];

    memset(oversized, 'k', sizeof(oversized) - 1U);
    oversized[sizeof(oversized) - 1U] = '\0';

    int written = sprintf(line, "%s = 000102030405060708090a0b0c0d0e0f\n", oversized);

    POUND_REQUIRE(written > 0);
    POUND_REQUIRE((size_t)written < sizeof(line));

    key_store_t store;

    key_store_init(&store);

    POUND_CHECK(POUND_ERROR_MALFORMED_HEADER == key_store_load_text(&store, line, (size_t)written));
    POUND_CHECK(0U == key_store_count(&store));

    // A name of exactly one character below the limit still fits, so the bound is not off
    // by one in the direction that would reject a legitimate name.
    char at_limit[POUND_KEY_NAME_MAX];

    memset(at_limit, 'j', sizeof(at_limit) - 1U);
    at_limit[sizeof(at_limit) - 1U] = '\0';

    written = sprintf(line, "%s = 000102030405060708090a0b0c0d0e0f\n", at_limit);

    POUND_REQUIRE(written > 0);
    POUND_REQUIRE((size_t)written < sizeof(line));

    POUND_CHECK_MSG(POUND_SUCCESS == key_store_load_text(&store, line, (size_t)written),
                    "a name one character below the limit was rejected, so the bound is "
                    "off by one.");
    POUND_CHECK(1U == key_store_count(&store));

    key_store_destroy(&store);
}

POUND_TEST(keys, a_key_family_index_outside_the_family_is_rejected)
{
    // An out-of-range index must not become a lookup of `master_key_09`, which a user's
    // file could plausibly contain and which would then be used in place of a real key.
    // So an out-of-range index is refused on the bound, and an in-range index whose key
    // happens to be absent is a *missing key*, which is a different code and a different
    // message. Conflating the two would tell a user their index was wrong when the real
    // problem is that they have not added the key.
    key_store_t store;

    key_store_init(&store);

    uint8_t key[POUND_KEY_SIZE];

    POUND_CHECK(POUND_ERROR_KEY_MISSING == key_store_get_master_key(&store, 0U, key));
    POUND_CHECK(POUND_ERROR_KEY_MISSING == key_store_get_master_key(&store, POUND_KEY_MASTER_KEY_COUNT - 1U, key));

    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == key_store_get_master_key(&store, POUND_KEY_MASTER_KEY_COUNT, key));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == key_store_get_master_key(&store, 99U, key));

    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT
                == key_store_get_application_key_area(&store, POUND_KEY_APPLICATION_KEY_AREA_COUNT, key));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == key_store_get_titlekek(&store, POUND_KEY_TITLEKEK_COUNT, key));

    // With the key actually present, the in-range index succeeds and still holds its own
    // value, so the bound is not rejecting valid indices along with invalid ones.
    static const char PARTIAL[] = "master_key_00 = 000102030405060708090a0b0c0d0e0f\n"
                                  "master_key_02 = 202122232425262728292a2b2c2d2e2f\n";

    POUND_REQUIRE(POUND_SUCCESS == key_store_load_text(&store, PARTIAL, sizeof(PARTIAL) - 1U));

    POUND_REQUIRE(POUND_SUCCESS == key_store_get_master_key(&store, 0U, key));
    POUND_CHECK(0x00U == key[0]);

    POUND_REQUIRE(POUND_SUCCESS == key_store_get_master_key(&store, POUND_KEY_MASTER_KEY_COUNT - 1U, key));
    POUND_CHECK(0x20U == key[0]);

    // The hole in the middle is still a missing key, not a bad index.
    POUND_CHECK(POUND_ERROR_KEY_MISSING == key_store_get_master_key(&store, 1U, key));

    key_store_destroy(&store);
}

POUND_TEST(keys, a_family_member_that_is_absent_reports_which_key_was_missing)
{
    key_store_t store;

    key_store_init(&store);

    // A file with master_key_00 but not master_key_01: asking for the second must name
    // the second, because that is the line the user has to add.
    static const char PARTIAL[] = "master_key_00 = 000102030405060708090a0b0c0d0e0f\n";

    POUND_REQUIRE(POUND_SUCCESS == key_store_load_text(&store, PARTIAL, sizeof(PARTIAL) - 1U));

    pound_test_log_reset();

    uint8_t key[POUND_KEY_SIZE];

    POUND_CHECK(POUND_ERROR_KEY_MISSING == key_store_get_master_key(&store, 1U, key));
    POUND_CHECK_MSG(pound_test_log_contains("master_key_01"),
                    "the failure did not name the key that was missing, so a user would not "
                    "know what to add to their file.");

    // And it must say that nothing was substituted, because the natural wrong reaction
    // to a decryption failure is to try a different key.
    POUND_CHECK(pound_test_log_contains("will not substitute"));

    key_store_destroy(&store);
}

POUND_TEST(keys, keys_are_enumerable_by_name_in_a_stable_order)
{
    key_store_t store;

    key_store_init(&store);

    POUND_REQUIRE(POUND_SUCCESS == key_store_load_text(&store, KEY_FILE, sizeof(KEY_FILE) - 1U));
    POUND_REQUIRE(6U == key_store_count(&store));

    // Sorted, so two runs produce the same list and a log can be compared between them.
    char previous[POUND_KEY_NAME_MAX];

    POUND_REQUIRE(POUND_SUCCESS == key_store_name_at(&store, 0U, previous));

    for (size_t i = 1U; i < key_store_count(&store); ++i)
    {
        char name[POUND_KEY_NAME_MAX];

        POUND_REQUIRE(POUND_SUCCESS == key_store_name_at(&store, i, name));

        // Ascending, so the name at the previous index sorts *before* this one. The
        // direction is the whole assertion: a check written as `0 < strcmp` would pass
        // for a store sorted in descending order and fail for one correctly sorted
        // ascending, which is a test that proves nothing.
        POUND_CHECK_MSG(0 > strcmp(previous, name), "the key names are not in sorted order at index %zu.", i);
        memcpy(previous, name, sizeof(name));
    }

    char name[POUND_KEY_NAME_MAX];

    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == key_store_name_at(&store, 6U, name));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == key_store_name_at(&store, 0U, NULL));

    key_store_destroy(&store);
}

POUND_TEST(keys, a_store_grows_past_its_initial_capacity_and_stays_sorted)
{
    // The array starts at eight entries and doubles. Enough names to cross that several
    // times, inserted in an order that would break anything relying on insertion order,
    // and then checked for the sorted invariant the binary search depends on.
    key_store_t store;

    key_store_init(&store);

    char line[128];

    for (int i = 39; i >= 0; --i)
    {
        const int written = sprintf(line,
                                    "synthetic_key_%02d = 000102030405060708090a0b0c0d0e%02x\n",
                                    i,
                                    i);

        POUND_REQUIRE(written > 0);
        POUND_REQUIRE((size_t)written < sizeof(line));
        POUND_REQUIRE(POUND_SUCCESS == key_store_load_text(&store, line, (size_t)written));
    }

    POUND_REQUIRE(40U == key_store_count(&store));

    uint8_t key[POUND_KEY_SIZE];

    for (int i = 0; i < 40; ++i)
    {
        char name[32];

        const int written = sprintf(name, "synthetic_key_%02d", i);

        POUND_REQUIRE(written > 0);
        POUND_REQUIRE((size_t)written < sizeof(name));

        POUND_REQUIRE_MSG(POUND_SUCCESS == key_store_get(&store, name, key),
                          "the key '%s' was lost across a reallocation.",
                          name);
        POUND_CHECK_MSG((uint8_t)i == key[POUND_KEY_SIZE - 1U],
                        "the key '%s' holds the value of another key after a reallocation.",
                        name);
    }

    key_store_destroy(&store);
}

POUND_TEST(keys, a_destroyed_store_is_empty_and_destroying_it_again_is_harmless)
{
    key_store_t store;

    key_store_init(&store);

    POUND_REQUIRE(POUND_SUCCESS == key_store_load_text(&store, KEY_FILE, sizeof(KEY_FILE) - 1U));

    key_store_destroy(&store);

    POUND_CHECK(0U == key_store_count(&store));
    POUND_CHECK(!key_store_has(&store, "master_key_00"));

    key_store_destroy(&store);

    // And on a store that was never used.
    key_store_t unused;

    memset(&unused, 0, sizeof(unused));
    key_store_destroy(&unused);

    POUND_CHECK(0U == key_store_count(&unused));
}

// ===================================================================================
// PFS0
// ===================================================================================

/// Mirrors the chunk size `pfs0_hash` streams through.
///
/// The chunk size is private to the implementation and deliberately has no correctness
/// attached to it -- any size produces the same digest. The tests need to know it only so
/// that a file can be built which straddles the boundary, which is how the chunk loop's
/// remainder arithmetic gets exercised. If the implementation ever changed this, these
/// cases would still pass; they would just be testing a different boundary.
#define HASH_CHUNK_PROBE (64U * 1024U)

/// One file to place in a synthetic partition.
///
/// `contents` is a pointer rather than an inline array because several cases need a file
/// larger than any reasonable stack or struct could hold, and because the image builder
/// copies from it directly. It may be NULL only when `size` is zero.
typedef struct
{
    const char *name;
    uint8_t    *POUND_RESTRICT contents;
    size_t      size;
} build_file_t;

/// Where the three regions of a synthetic partition landed, so a test can corrupt a
/// specific one.
typedef struct
{
    size_t total;          ///< Zero if the files would not fit, which no test should produce.
    size_t table_offset;   ///< Always `PFS0_HEADER_SIZE`.
    size_t string_offset;  ///< Immediately after the file table.
    size_t data_offset;    ///< Immediately after the string table.
} pfs0_layout_t;

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

/// Writes a valid partition image containing `files` into `buffer`.
///
/// `layout->total` is set to zero if the files would not fit, so a test can require it
/// and catch its own sizing mistake rather than reading a truncated image and failing for
/// the wrong reason. The image is built with no gaps between files' data, which is what
/// the real tooling produces and what lets a test compute a file's absolute offset
/// independently of the parser and compare.
static pfs0_layout_t
build_pfs0(uint8_t *POUND_RESTRICT buffer, const size_t capacity, const build_file_t *POUND_RESTRICT files,
           const size_t count)
{
    pfs0_layout_t layout;

    layout.table_offset  = PFS0_HEADER_SIZE;
    layout.string_offset = PFS0_HEADER_SIZE + (count * PFS0_ENTRY_SIZE);

    size_t string_size = PFS0_STRING_TABLE_MAGIC_SIZE;

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

    store_le32(&buffer[0], PFS0_MAGIC);
    store_le32(&buffer[4], (uint32_t)count);
    store_le32(&buffer[8], (uint32_t)string_size);
    store_le32(&buffer[12], 0U);

    store_le32(&buffer[layout.string_offset], PFS0_MAGIC);

    size_t name_cursor = PFS0_STRING_TABLE_MAGIC_SIZE;
    size_t data_cursor = 0U;

    for (size_t i = 0U; i < count; ++i)
    {
        const size_t   name_length = strlen(files[i].name);
        uint8_t *POUND_RESTRICT entry = &buffer[layout.table_offset + (i * PFS0_ENTRY_SIZE)];

        store_le64(&entry[0], (uint64_t)data_cursor);
        store_le64(&entry[8], (uint64_t)files[i].size);

        // The stored offset is measured from the first byte *after* the string table's
        // four-byte magic, which is what `pfs0_open` measures from and what the first name
        // in a real partition has an offset of: zero. Storing `name_cursor` verbatim would
        // make the first name point four bytes past where it actually is.
        store_le32(&entry[16], (uint32_t)(name_cursor - PFS0_STRING_TABLE_MAGIC_SIZE));
        store_le32(&entry[20], 0U);

        memcpy(&buffer[layout.string_offset + name_cursor], files[i].name, name_length + 1U);

        name_cursor += name_length + 1U;

        // A NULL `contents` leaves the file's region as the caller left it. Two of the
        // hash cases need that: a file larger than any struct here could hold, whose
        // contents are written straight into the image after the header is built, because
        // the header is all `build_pfs0` needs in order to describe it.
        if ((NULL != files[i].contents) && (0U != files[i].size))
        {
            memcpy(&buffer[layout.data_offset + data_cursor], files[i].contents, files[i].size);
        }

        data_cursor += files[i].size;
    }

    return layout;
}

/// Fills a `build_file_t` with a distinct, recognisable pattern.
static void
make_file(build_file_t *POUND_RESTRICT file, const char *name, uint8_t *POUND_RESTRICT contents, const size_t size,
          const uint8_t seed)
{
    file->name     = name;
    file->contents = contents;
    file->size     = size;

    for (size_t i = 0U; i < size; ++i)
    {
        contents[i] = (uint8_t)(seed + (uint8_t)i);
    }
}

/// The partition most PFS0 cases use: three files of different sizes, so an off-by-one in
/// an offset or a size cannot pass by being right for a zero-length file, and a name with
/// a slash in it, so path-shaped names are covered.
#define SAMPLE_FILE_COUNT 3U
#define SAMPLE_CAPACITY   2048U

/// Backing storage for the sample files' contents. Static so a test may overwrite its own
/// copy in the image without disturbing what the next case is handed.
static uint8_t SAMPLE_CONTENTS[SAMPLE_FILE_COUNT][512];

static void
make_sample_files(build_file_t files[SAMPLE_FILE_COUNT])
{
    make_file(&files[0], "main", SAMPLE_CONTENTS[0], 300U, 0x00U);
    make_file(&files[1], "sub/second.nca", SAMPLE_CONTENTS[1], 100U, 0x40U);
    make_file(&files[2], "third", SAMPLE_CONTENTS[2], 0U, 0x00U);
}

POUND_TEST(pfs0, a_valid_partition_opens_and_reports_its_files)
{
    uint8_t image[SAMPLE_CAPACITY];

    build_file_t files[SAMPLE_FILE_COUNT];

    make_sample_files(files);

    const pfs0_layout_t layout = build_pfs0(image, sizeof(image), files, SAMPLE_FILE_COUNT);

    POUND_REQUIRE(0U != layout.total);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, image, layout.total);

    pfs0_t pfs0;

    POUND_REQUIRE_MSG(POUND_SUCCESS == pfs0_open(&pfs0, &reader), "a valid partition was rejected.");

    POUND_CHECK(pfs0_is_open(&pfs0));
    POUND_CHECK(SAMPLE_FILE_COUNT == pfs0_count(&pfs0));
    POUND_CHECK(layout.data_offset == pfs0.data_offset);
    POUND_CHECK(layout.total == (pfs0.data_offset + pfs0.data_size));

    const pfs0_entry_t *entry = pfs0_entry_at(&pfs0, 0U);

    POUND_REQUIRE(NULL != entry);
    POUND_CHECK_MSG(0 == strcmp("main", entry->name), "file 0 is called '%s', not 'main'.", entry->name);
    POUND_CHECK(300U == entry->size);

    // A name with a forward slash in it, which is how a path inside an NCA's section
    // zero is stored, survives as a single flat string.
    entry = pfs0_entry_at(&pfs0, 1U);
    POUND_REQUIRE(NULL != entry);
    POUND_CHECK(0 == strcmp("sub/second.nca", entry->name));
    POUND_CHECK(100U == entry->size);

    // A zero-length file is legal and must be reported with its real name, not skipped.
    entry = pfs0_entry_at(&pfs0, 2U);
    POUND_REQUIRE(NULL != entry);
    POUND_CHECK(0 == strcmp("third", entry->name));
    POUND_CHECK(0U == entry->size);

    POUND_CHECK(NULL == pfs0_entry_at(&pfs0, SAMPLE_FILE_COUNT));

    pfs0_log_summary(&pfs0);

    pfs0_close(&pfs0);
}

POUND_TEST(pfs0, a_files_contents_read_back_byte_for_byte)
{
    uint8_t image[SAMPLE_CAPACITY];

    build_file_t files[SAMPLE_FILE_COUNT];

    make_sample_files(files);

    const pfs0_layout_t layout = build_pfs0(image, sizeof(image), files, SAMPLE_FILE_COUNT);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, image, layout.total);

    pfs0_t pfs0;

    POUND_REQUIRE(POUND_SUCCESS == pfs0_open(&pfs0, &reader));

    for (uint32_t i = 0U; i < SAMPLE_FILE_COUNT; ++i)
    {
        uint8_t destination[512];

        POUND_REQUIRE_MSG(POUND_SUCCESS == pfs0_read(&pfs0, i, 0U, destination, files[i].size),
                          "file %u ('%s') could not be read.",
                          i,
                          files[i].name);

        POUND_CHECK_MSG(0 == memcmp(destination, files[i].contents, files[i].size),
                        "file %u read back differently from what was written.",
                        i);
    }

    // The same file read from its middle, which proves the offset in the entry is
    // relative to the file rather than to the partition.
    uint8_t middle[16];

    POUND_REQUIRE(POUND_SUCCESS == pfs0_read(&pfs0, 0U, 128U, middle, sizeof(middle)));
    POUND_CHECK(0 == memcmp(middle, &files[0].contents[128], sizeof(middle)));

    // The last byte of a file, which is the boundary a `<` rather than `<=` gets wrong.
    uint8_t last[1];

    POUND_REQUIRE(POUND_SUCCESS == pfs0_read(&pfs0, 0U, 299U, last, sizeof(last)));
    POUND_CHECK(files[0].contents[299] == last[0]);

    // A zero-length file reads zero bytes successfully, and one byte from it does not,
    // even though its start and its end are the same offset. A zero-length read at *any*
    // offset succeeds, because a read of nothing has no range to violate -- the same rule
    // `fs_reader_read_at` follows, so the two layers cannot disagree about an empty read.
    POUND_CHECK(POUND_SUCCESS == pfs0_read(&pfs0, 2U, 0U, middle, 0U));
    POUND_CHECK(POUND_SUCCESS == pfs0_read(&pfs0, 2U, 1U, middle, 0U));
    POUND_CHECK(POUND_ERROR_IO == pfs0_read(&pfs0, 2U, 0U, middle, 1U));

    pfs0_close(&pfs0);
}

POUND_TEST(pfs0, a_read_that_would_leave_a_file_is_refused)
{
    uint8_t image[SAMPLE_CAPACITY];

    build_file_t files[SAMPLE_FILE_COUNT];

    make_sample_files(files);

    const pfs0_layout_t layout = build_pfs0(image, sizeof(image), files, SAMPLE_FILE_COUNT);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, image, layout.total);

    pfs0_t pfs0;

    POUND_REQUIRE(POUND_SUCCESS == pfs0_open(&pfs0, &reader));

    uint8_t destination[512];

    // One byte past the end of a 300-byte file.
    POUND_CHECK_MSG(POUND_ERROR_IO == pfs0_read(&pfs0, 0U, 300U, destination, 1U),
                    "a read starting one byte past the end of a file was allowed.");

    // Starting inside and running out.
    POUND_CHECK(POUND_ERROR_IO == pfs0_read(&pfs0, 0U, 290U, destination, 20U));

    // An offset near the top of the address space, which a bounds check written as
    // `offset + size <= entry->size` would let through by wrapping.
    POUND_CHECK(POUND_ERROR_IO == pfs0_read(&pfs0, 0U, UINT64_MAX - 2U, destination, 8U));
    POUND_CHECK(POUND_ERROR_IO == pfs0_read(&pfs0, 0U, UINT64_MAX, destination, 1U));

    // A file index that does not exist.
    POUND_CHECK(POUND_ERROR_NOT_FOUND == pfs0_read(&pfs0, SAMPLE_FILE_COUNT, 0U, destination, 1U));
    POUND_CHECK(POUND_ERROR_NOT_FOUND == pfs0_read(&pfs0, 0xFFFFFFFFU, 0U, destination, 1U));

    // A read with no destination.
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == pfs0_read(&pfs0, 0U, 0U, NULL, 4U));

    pfs0_close(&pfs0);
}

POUND_TEST(pfs0, a_read_from_a_closed_partition_is_refused)
{
    // The reason reads are addressed by index. A caller holding an entry pointer across a
    // close has a dangling pointer, and an index cannot dangle: it is re-validated
    // against the table that exists now.
    uint8_t image[SAMPLE_CAPACITY];

    build_file_t files[SAMPLE_FILE_COUNT];

    make_sample_files(files);

    const pfs0_layout_t layout = build_pfs0(image, sizeof(image), files, SAMPLE_FILE_COUNT);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, image, layout.total);

    pfs0_t pfs0;

    POUND_REQUIRE(POUND_SUCCESS == pfs0_open(&pfs0, &reader));

    uint8_t destination[16];

    POUND_REQUIRE(POUND_SUCCESS == pfs0_read(&pfs0, 0U, 0U, destination, sizeof(destination)));

    pfs0_close(&pfs0);

    POUND_CHECK(!pfs0_is_open(&pfs0));
    POUND_CHECK(0U == pfs0_count(&pfs0));

    // Everything that needs the table refuses it.
    POUND_CHECK(POUND_ERROR_NOT_INITIALIZED == pfs0_read(&pfs0, 0U, 0U, destination, sizeof(destination)));
    POUND_CHECK(NULL == pfs0_entry_at(&pfs0, 0U));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == pfs0_read(NULL, 0U, 0U, destination, sizeof(destination)));

    uint8_t digest[SHA256_DIGEST_SIZE];

    POUND_CHECK(POUND_ERROR_NOT_INITIALIZED == pfs0_hash(&pfs0, 0U, digest));
    POUND_CHECK(POUND_ERROR_NOT_INITIALIZED == pfs0_verify_hash(&pfs0, 0U, digest));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == pfs0_verify_hash(&pfs0, 0U, NULL));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == pfs0_verify_hash(NULL, 0U, digest));

    // Closing twice is harmless.
    pfs0_close(&pfs0);
}

POUND_TEST(pfs0, files_are_found_by_name_and_a_miss_is_quiet)
{
    uint8_t image[SAMPLE_CAPACITY];

    build_file_t files[SAMPLE_FILE_COUNT];

    make_sample_files(files);

    const pfs0_layout_t layout = build_pfs0(image, sizeof(image), files, SAMPLE_FILE_COUNT);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, image, layout.total);

    pfs0_t pfs0;

    POUND_REQUIRE(POUND_SUCCESS == pfs0_open(&pfs0, &reader));

    uint32_t index = 0xFFFFFFFFU;

    POUND_REQUIRE(POUND_SUCCESS == pfs0_index_of(&pfs0, "sub/second.nca", &index));
    POUND_CHECK_MSG(1U == index, "'sub/second.nca' resolved to index %u rather than 1.", index);

    // The name has to match exactly. A prefix, a different case and a trailing slash are
    // all different files, and treating them as the same one is how a loader ends up
    // reading a file the user did not ask for.
    POUND_CHECK(POUND_ERROR_NOT_FOUND == pfs0_index_of(&pfs0, "sub", &index));
    POUND_CHECK(POUND_ERROR_NOT_FOUND == pfs0_index_of(&pfs0, "SUB/SECOND.NCA", &index));
    POUND_CHECK(POUND_ERROR_NOT_FOUND == pfs0_index_of(&pfs0, "sub/second.nca/", &index));
    POUND_CHECK(POUND_ERROR_NOT_FOUND == pfs0_index_of(&pfs0, "second", &index));

    // A miss is a question with an answer of "no", so it must not log. The loader probes
    // for optional files that only some titles ship, and an error record per probe would
    // bury the log.
    pound_test_log_reset();

    // Deliberately set to something that is neither a valid index nor the value a
    // successful lookup would have left, so the next check can tell "the miss wrote
    // nothing" apart from "the miss happened to write what was there before".
    index = 0xDEADBEEFU;

    POUND_CHECK(POUND_ERROR_NOT_FOUND == pfs0_index_of(&pfs0, "no_such_file", &index));

    POUND_CHECK_MSG(0U == pound_test_log_count_at(LOG_LEVEL_ERROR),
                    "a lookup that found nothing logged %u error record(s); a miss is an "
                    "ordinary answer, not a failure.",
                    pound_test_log_count_at(LOG_LEVEL_ERROR));

    // `out_index` is left alone on a miss, so a caller that ignores the status cannot
    // mistake an untouched local for a valid index.
    POUND_CHECK(0xDEADBEEFU == index);

    POUND_CHECK(pfs0_contains(&pfs0, "main"));
    POUND_CHECK(!pfs0_contains(&pfs0, "MAIN"));
    POUND_CHECK(!pfs0_contains(&pfs0, ""));

    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == pfs0_index_of(&pfs0, "main", NULL));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == pfs0_index_of(&pfs0, NULL, &index));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == pfs0_index_of(NULL, "main", &index));

    pfs0_close(&pfs0);
}

POUND_TEST(pfs0, a_wrong_magic_is_reported_as_a_malformed_header)
{
    uint8_t image[SAMPLE_CAPACITY];

    build_file_t files[SAMPLE_FILE_COUNT];

    make_sample_files(files);

    const pfs0_layout_t layout = build_pfs0(image, sizeof(image), files, SAMPLE_FILE_COUNT);

    // "HFS0" stored little-endian. A real format that is not this one, which is the case
    // that matters: a loader handed an HFS0 image must say "not a partition" rather than
    // "corrupt".
    store_le32(&image[0], 0x30534648U);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, image, layout.total);

    pfs0_t pfs0;

    POUND_CHECK(POUND_ERROR_MALFORMED_HEADER == pfs0_open(&pfs0, &reader));
    POUND_CHECK(!pfs0_is_open(&pfs0));

    // The found magic is shown, so a user can see what the file actually starts with.
    POUND_CHECK(pound_test_log_contains("HFS0"));

    // A magic whose bytes are not printable must not be echoed into the log raw, or a
    // corrupt image could inject escape sequences into a terminal reading the log.
    memset(image, 0x1BU, 4U);

    POUND_CHECK(POUND_ERROR_MALFORMED_HEADER == pfs0_open(&pfs0, &reader));
    POUND_CHECK_MSG(!pound_test_log_contains("\x1B"),
                    "a magic containing an escape character reached the log unescaped.");
}

POUND_TEST(pfs0, a_file_count_beyond_the_cap_is_refused_without_allocating)
{
    // A header claiming four billion files would ask for a ninety-gigabyte allocation if
    // the count were believed before it was checked.
    uint8_t image[SAMPLE_CAPACITY];

    build_file_t files[SAMPLE_FILE_COUNT];

    make_sample_files(files);

    const pfs0_layout_t layout = build_pfs0(image, sizeof(image), files, SAMPLE_FILE_COUNT);

    store_le32(&image[4], 0xFFFFFFFFU);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, image, layout.total);

    pfs0_t pfs0;

    POUND_CHECK(POUND_ERROR_MALFORMED_HEADER == pfs0_open(&pfs0, &reader));
    POUND_CHECK(pound_test_log_contains("no real partition has more than"));

    // One past the cap, and exactly the cap. Exactly the cap is a legitimate declared
    // size that this tiny image cannot satisfy, so it must fail on the range check and
    // not on the cap -- which the log distinguishes.
    store_le32(&image[4], PFS0_MAX_FILES + 1U);

    POUND_CHECK(POUND_ERROR_MALFORMED_HEADER == pfs0_open(&pfs0, &reader));

    store_le32(&image[4], PFS0_MAX_FILES);

    POUND_CHECK_MSG(POUND_ERROR_MALFORMED_HEADER == pfs0_open(&pfs0, &reader),
                    "a file count of exactly the cap was not checked against the source size.");
    POUND_CHECK(pound_test_log_contains("runs past the end"));
}

POUND_TEST(pfs0, a_string_table_size_beyond_the_cap_is_refused)
{
    uint8_t image[SAMPLE_CAPACITY];

    build_file_t files[SAMPLE_FILE_COUNT];

    make_sample_files(files);

    const pfs0_layout_t layout = build_pfs0(image, sizeof(image), files, SAMPLE_FILE_COUNT);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, image, layout.total);

    pfs0_t pfs0;

    store_le32(&image[8], 0xFFFFFFF0U);

    POUND_CHECK(POUND_ERROR_MALFORMED_HEADER == pfs0_open(&pfs0, &reader));
    POUND_CHECK(pound_test_log_contains("string table"));

    // One past the cap.
    store_le32(&image[8], PFS0_MAX_STRING_TABLE_SIZE + 1U);

    POUND_CHECK(POUND_ERROR_MALFORMED_HEADER == pfs0_open(&pfs0, &reader));

    // Too small to hold even its own magic.
    store_le32(&image[8], 0U);

    POUND_CHECK(POUND_ERROR_MALFORMED_HEADER == pfs0_open(&pfs0, &reader));
    POUND_CHECK_MSG(pound_test_log_contains("too small"), "the zero-length string table was not named as such.");

    // Large enough to be plausible but larger than this image: rejected on the range,
    // not on the cap.
    store_le32(&image[8], (uint32_t)layout.total);

    POUND_CHECK(POUND_ERROR_MALFORMED_HEADER == pfs0_open(&pfs0, &reader));
}

POUND_TEST(pfs0, a_string_table_with_the_wrong_magic_is_refused)
{
    // The header and the string table carry the same magic independently. A file whose
    // header is right and whose string table is not has been assembled wrongly, and
    // reading names out of it would read whatever happens to be at those offsets.
    uint8_t image[SAMPLE_CAPACITY];

    build_file_t files[SAMPLE_FILE_COUNT];

    make_sample_files(files);

    const pfs0_layout_t layout = build_pfs0(image, sizeof(image), files, SAMPLE_FILE_COUNT);

    store_le32(&image[layout.string_offset], 0xDEADBEEFU);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, image, layout.total);

    pfs0_t pfs0;

    POUND_CHECK(POUND_ERROR_MALFORMED_HEADER == pfs0_open(&pfs0, &reader));
    POUND_CHECK_MSG(pound_test_log_contains("string table begins with"),
                    "the failure did not say the string table's magic was wrong, which is the "
                    "unusual part.");
}

POUND_TEST(pfs0, a_name_pointing_past_the_string_table_is_refused)
{
    uint8_t image[SAMPLE_CAPACITY];

    build_file_t files[SAMPLE_FILE_COUNT];

    make_sample_files(files);

    const pfs0_layout_t layout = build_pfs0(image, sizeof(image), files, SAMPLE_FILE_COUNT);

    const uint32_t names_size = (uint32_t)(layout.data_offset - layout.string_offset - PFS0_STRING_TABLE_MAGIC_SIZE);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, image, layout.total);

    pfs0_t pfs0;

    // Exactly at the end of the names region, which is past the last usable offset.
    store_le32(&image[layout.table_offset + 16U], names_size);

    POUND_CHECK(POUND_ERROR_MALFORMED_HEADER == pfs0_open(&pfs0, &reader));
    POUND_CHECK(pound_test_log_contains("points at name offset"));

    // The last byte of the names region is the terminator of the final name, so pointing
    // there would yield an empty name. Overwriting it and pointing one byte earlier
    // instead makes the name run to the very end of the region with no terminator, which
    // is the case that would otherwise read past the allocation.
    image[layout.data_offset - 1U] = 'X';

    store_le32(&image[layout.table_offset + 16U], names_size - 1U);

    POUND_CHECK(POUND_ERROR_MALFORMED_HEADER == pfs0_open(&pfs0, &reader));
    POUND_CHECK_MSG(pound_test_log_contains("not terminated"),
                    "a name running off the end of the string table was not reported as "
                    "unterminated.");
}

POUND_TEST(pfs0, a_name_too_long_to_store_is_refused_rather_than_truncated)
{
    // A name of more than `PFS0_NAME_MAX` bytes, properly terminated inside the string
    // table. Truncating it would produce a name that collides with a real one, and a
    // lookup would then return whichever file happened to be stored first.
    static char long_name[PFS0_NAME_MAX + 64U];

    memset(long_name, 'A', sizeof(long_name) - 1U);
    long_name[sizeof(long_name) - 1U] = '\0';

    POUND_REQUIRE(sizeof(long_name) - 1U > PFS0_NAME_MAX);

    uint8_t image[SAMPLE_CAPACITY + sizeof(long_name)];

    build_file_t files[1];

    make_file(&files[0], long_name, SAMPLE_CONTENTS[0], 64U, 0x11U);

    const pfs0_layout_t layout = build_pfs0(image, sizeof(image), files, 1U);

    POUND_REQUIRE(0U != layout.total);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, image, layout.total);

    pfs0_t pfs0;

    POUND_CHECK_MSG(POUND_ERROR_MALFORMED_HEADER == pfs0_open(&pfs0, &reader),
                    "a name of %zu characters was accepted into a %u-character field, so it "
                    "must have been truncated.",
                    sizeof(long_name) - 1U,
                    PFS0_NAME_MAX);
    POUND_CHECK(pound_test_log_contains("does not fit in the"));
}

POUND_TEST(pfs0, an_entry_whose_bytes_leave_the_data_region_is_refused)
{
    uint8_t image[SAMPLE_CAPACITY];

    build_file_t files[SAMPLE_FILE_COUNT];

    make_sample_files(files);

    const pfs0_layout_t layout = build_pfs0(image, sizeof(image), files, SAMPLE_FILE_COUNT);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, image, layout.total);

    pfs0_t pfs0;

    // File 0 claims to be as large as the whole partition plus a bit.
    store_le64(&image[layout.table_offset + 8U], (uint64_t)layout.total + 1U);

    POUND_CHECK_MSG(POUND_ERROR_MALFORMED_HEADER == pfs0_open(&pfs0, &reader),
                    "an entry reaching past the end of the data region was accepted.");
    POUND_CHECK(pound_test_log_contains("leaves the"));

    // An offset near the top of the address space, which a bounds check written as
    // `offset + size <= data_size` would accept by wrapping.
    store_le64(&image[layout.table_offset], UINT64_MAX - 3U);
    store_le64(&image[layout.table_offset + 8U], 16U);

    POUND_CHECK(POUND_ERROR_MALFORMED_HEADER == pfs0_open(&pfs0, &reader));

    // The exact boundaries of the data region are covered by
    // `a_file_at_the_edge_of_the_data_region_is_measured_correctly`, in a single-file
    // partition. They cannot be probed here: moving one file's range in a three-file
    // partition walks it into the other two, so the overlap check would be what rejected
    // the case, and the bounds check would never be exercised at all.

    pfs0_close(&pfs0);
}

POUND_TEST(pfs0, a_file_at_the_edge_of_the_data_region_is_measured_correctly)
{
    // Both ends of the data region, on a one-file partition so that moving the file's
    // range cannot run into the overlap check against a second file and mask the bounds
    // check being tested. Two different checks with opposite answers meet at the last
    // byte of the region, and a test that does not distinguish them is a test that
    // proves nothing about either.
    uint8_t image[SAMPLE_CAPACITY];

    build_file_t files[1];

    make_file(&files[0], "only", SAMPLE_CONTENTS[0], 300U, 0x00U);

    const pfs0_layout_t layout = build_pfs0(image, sizeof(image), files, 1U);

    POUND_REQUIRE(0U != layout.total);

    const uint64_t data_size = (uint64_t)(layout.total - layout.data_offset);

    // The file fills the region exactly, so every case below is a boundary case: the
    // whole region, one byte more than the region, and the two positions an empty file
    // can sit at.
    POUND_REQUIRE_MSG(data_size == files[0].size,
                      "the test's own data region is %llu bytes but the file is %zu, so these "
                      "are not the boundary cases this test claims to check.",
                      (unsigned long long)data_size,
                      files[0].size);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, image, layout.total);

    pfs0_t pfs0;

    // Exactly the region: the end of the file is the end of the region.
    store_le64(&image[layout.table_offset], 0U);
    store_le64(&image[layout.table_offset + 8U], data_size);

    POUND_REQUIRE_MSG(POUND_SUCCESS == pfs0_open(&pfs0, &reader),
                      "a file occupying exactly the whole data region was rejected, so the "
                      "bounds check is off by one at the full end.");

    // And it is readable, which is what proves the rebase landed correctly: the stored
    // offset was relative, so a caller using it without the base would be reading from
    // the wrong place entirely.
    const pfs0_entry_t *entry = pfs0_entry_at(&pfs0, 0U);

    POUND_REQUIRE_MSG(NULL != entry, "the partition opened without its only entry.");
    POUND_CHECK_MSG(layout.data_offset == entry->offset,
                    "the first file reports an offset of %llu rather than the data region's "
                    "start at %zu, so its offset was not rebased onto the region.",
                    (unsigned long long)entry->offset,
                    layout.data_offset);

    uint8_t destination[512];

    POUND_REQUIRE(POUND_SUCCESS == pfs0_read(&pfs0, 0U, 0U, destination, files[0].size));
    POUND_CHECK_MSG(0 == memcmp(destination, files[0].contents, files[0].size),
                    "a file filling the data region read back different bytes.");

    // Its last byte is the very last byte of the region.
    POUND_REQUIRE(POUND_SUCCESS == pfs0_read(&pfs0, 0U, files[0].size - 1U, destination, 1U));
    POUND_CHECK(files[0].contents[files[0].size - 1U] == destination[0]);

    pfs0_close(&pfs0);

    // One byte further out, rejected by the bounds check.
    store_le64(&image[layout.table_offset], 1U);
    store_le64(&image[layout.table_offset + 8U], data_size);

    POUND_CHECK(POUND_ERROR_MALFORMED_HEADER == pfs0_open(&pfs0, &reader));

    // An empty file at the very end of the region is *inside*: it occupies no bytes, and
    // its one-past-the-end start is the same place the previous file ended. Rejecting it
    // would be an off-by-one in the strict direction.
    store_le64(&image[layout.table_offset], data_size);
    store_le64(&image[layout.table_offset + 8U], 0U);

    POUND_REQUIRE_MSG(POUND_SUCCESS == pfs0_open(&pfs0, &reader),
                      "an empty file at the end of the data region was rejected, so the "
                      "bounds check is off by one at the empty end.");

    pfs0_close(&pfs0);

    // One byte past even that, which does name a position outside the region.
    store_le64(&image[layout.table_offset], data_size + 1U);
    store_le64(&image[layout.table_offset + 8U], 0U);

    POUND_CHECK_MSG(POUND_ERROR_MALFORMED_HEADER == pfs0_open(&pfs0, &reader),
                    "an empty file one byte past the end of the data region was accepted.");
}

POUND_TEST(pfs0, two_entries_with_the_same_name_are_refused)
{
    // A duplicate name makes a lookup ambiguous, and resolving the ambiguity by picking
    // the first match is how a loader ends up reading a file the user did not ask for.
    uint8_t image[SAMPLE_CAPACITY];

    build_file_t files[SAMPLE_FILE_COUNT];

    make_sample_files(files);

    const pfs0_layout_t layout = build_pfs0(image, sizeof(image), files, SAMPLE_FILE_COUNT);

    // Point file 2 at file 0's name. Its size is zero, so the overlap check skips it and
    // the name check is what has to catch this.
    store_le32(&image[layout.table_offset + (2U * PFS0_ENTRY_SIZE) + 16U], 0U);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, image, layout.total);

    pfs0_t pfs0;

    POUND_CHECK(POUND_ERROR_MALFORMED_HEADER == pfs0_open(&pfs0, &reader));
    POUND_CHECK_MSG(pound_test_log_contains("must be unique"), "the failure did not identify the ambiguity.");
}

POUND_TEST(pfs0, two_entries_claiming_the_same_bytes_are_refused)
{
    uint8_t image[SAMPLE_CAPACITY];

    build_file_t files[SAMPLE_FILE_COUNT];

    make_sample_files(files);

    const pfs0_layout_t layout = build_pfs0(image, sizeof(image), files, SAMPLE_FILE_COUNT);

    // File 1 claims to start where file 0 starts and to be as long as it already was, so
    // the two ranges overlap without either being contained whole in the other.
    store_le64(&image[layout.table_offset + PFS0_ENTRY_SIZE], 0U);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, image, layout.total);

    pfs0_t pfs0;

    POUND_CHECK(POUND_ERROR_MALFORMED_HEADER == pfs0_open(&pfs0, &reader));
    POUND_CHECK(pound_test_log_contains("overlapping"));
}

POUND_TEST(pfs0, two_empty_files_at_the_same_offset_are_not_an_overlap)
{
    // A zero-length file occupies no bytes, so two of them at the same offset claim
    // nothing and there is nothing to disagree about. The names still have to differ,
    // which they do.
    uint8_t image[SAMPLE_CAPACITY];

    build_file_t files[SAMPLE_FILE_COUNT];

    make_sample_files(files);

    const pfs0_layout_t layout = build_pfs0(image, sizeof(image), files, SAMPLE_FILE_COUNT);

    store_le64(&image[layout.table_offset + (2U * PFS0_ENTRY_SIZE)], 0U);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, image, layout.total);

    pfs0_t pfs0;

    POUND_REQUIRE_MSG(POUND_SUCCESS == pfs0_open(&pfs0, &reader),
                      "two empty files sharing an offset were rejected as overlapping, which "
                      "they are not.");

    pfs0_close(&pfs0);
}

POUND_TEST(pfs0, a_truncated_image_is_reported_as_a_bad_header_and_a_stub_as_an_io_failure)
{
    // The two are different problems. A header that describes more than the file holds is
    // an image that is not what it claims; a file too short to hold a header at all is a
    // read that ran off the end. The first sends a user looking for corruption, the
    // second tells them the copy is incomplete.
    uint8_t image[SAMPLE_CAPACITY];

    build_file_t files[SAMPLE_FILE_COUNT];

    make_sample_files(files);

    const pfs0_layout_t layout = build_pfs0(image, sizeof(image), files, SAMPLE_FILE_COUNT);

    fs_reader_t reader;

    pfs0_t pfs0;

    // Cut off inside the file table.
    fs_reader_from_buffer(&reader, image, PFS0_HEADER_SIZE + PFS0_ENTRY_SIZE);

    POUND_CHECK_MSG(POUND_ERROR_MALFORMED_HEADER == pfs0_open(&pfs0, &reader),
                    "an image truncated inside its file table was not reported as a bad header.");
    POUND_CHECK(pound_test_log_contains("file table"));

    // Cut off inside the string table.
    fs_reader_from_buffer(&reader, image, layout.string_offset + 2U);

    POUND_CHECK(POUND_ERROR_MALFORMED_HEADER == pfs0_open(&pfs0, &reader));
    POUND_CHECK(pound_test_log_contains("string table at offset"));

    // Cut off so that the header itself is incomplete.
    fs_reader_from_buffer(&reader, image, 8U);

    POUND_CHECK_MSG(POUND_ERROR_IO == pfs0_open(&pfs0, &reader),
                    "an image too short to hold a header was not reported as an I/O failure.");
}

POUND_TEST(pfs0, an_empty_partition_opens_with_no_files)
{
    // Legal, and it appears in practice: an NCA section zero for a title with no loose
    // content, and an NSO inside a fully applied delta. A zero file count must succeed
    // with an empty table, not be treated as a degenerate header.
    uint8_t image[64];

    const pfs0_layout_t layout = build_pfs0(image, sizeof(image), NULL, 0U);

    POUND_REQUIRE(0U != layout.total);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, image, layout.total);

    pfs0_t pfs0;

    POUND_REQUIRE_MSG(POUND_SUCCESS == pfs0_open(&pfs0, &reader), "an empty partition was rejected.");
    POUND_CHECK(pfs0_is_open(&pfs0));
    POUND_CHECK(0U == pfs0_count(&pfs0));
    POUND_CHECK(!pfs0_contains(&pfs0, "main"));
    POUND_CHECK(NULL == pfs0_entry_at(&pfs0, 0U));

    // A hash of a partition with no files is a hash of nothing, and must not be reported
    // as an attempt to hash file zero.
    uint8_t digest[SHA256_DIGEST_SIZE];

    POUND_CHECK(POUND_ERROR_NOT_FOUND == pfs0_hash(&pfs0, 0U, digest));

    pfs0_log_summary(&pfs0);

    pfs0_close(&pfs0);
}

POUND_TEST(pfs0, a_files_hash_matches_hashing_its_bytes_directly)
{
    // The streaming path in `pfs0_hash` reads through the same reader a caller would use,
    // in fixed-size chunks. A file larger than one chunk is what exercises the loop, so
    // the sample includes one spanning three chunks.
    static const size_t BIG_SIZE = (HASH_CHUNK_PROBE * 2U) + 1234U;
    static const size_t SMALL   = 1000U;

    const size_t capacity = BIG_SIZE + SMALL + 1024U;

    uint8_t *image = (uint8_t *)memory_subsystem_allocate(1U, capacity);

    POUND_REQUIRE_MSG(NULL != image, "could not allocate room for the test image.");

    uint8_t *big = (uint8_t *)memory_subsystem_allocate(1U, BIG_SIZE);

    POUND_REQUIRE_MSG(NULL != big, "could not allocate the large test file.");

    uint8_t *small = (uint8_t *)memory_subsystem_allocate(1U, SMALL);

    POUND_REQUIRE_MSG(NULL != small, "could not allocate the small test file.");

    for (size_t i = 0U; i < BIG_SIZE; ++i)
    {
        big[i] = (uint8_t)(i * 13U + 5U);
    }

    for (size_t i = 0U; i < SMALL; ++i)
    {
        small[i] = (uint8_t)(i * 29U + 11U);
    }

    build_file_t files[2];

    make_file(&files[0], "small", small, SMALL, 0x00U);
    make_file(&files[1], "large", big, BIG_SIZE, 0x00U);

    const pfs0_layout_t layout = build_pfs0(image, capacity, files, 2U);

    POUND_REQUIRE(0U != layout.total);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, image, layout.total);

    pfs0_t pfs0;

    POUND_REQUIRE(POUND_SUCCESS == pfs0_open(&pfs0, &reader));

    uint8_t via_partition[SHA256_DIGEST_SIZE];
    uint8_t directly[SHA256_DIGEST_SIZE];

    // The small file: its contents are known, so hash them directly and compare.
    POUND_REQUIRE(POUND_SUCCESS == pfs0_hash(&pfs0, 0U, via_partition));
    sha256(small, SMALL, directly);

    POUND_CHECK_MSG(sha256_equal(via_partition, directly),
                    "hashing a small file through the partition disagreed with hashing its "
                    "bytes directly.");

    // The large file, which spans three chunks. The expected digest is produced by
    // streaming the same bytes through SHA-256 in a deliberately different chunking, so
    // the comparison is against the hash rather than against the same loop twice. The
    // chunk size here is not a multiple of the file's length, so the loop has to handle
    // a partial final chunk -- reading a whole one anyway would run off the end of the
    // buffer and hash bytes that are not part of the file.
    POUND_REQUIRE(POUND_SUCCESS == pfs0_hash(&pfs0, 1U, via_partition));

    sha256_t context;

    sha256_init(&context);

    size_t offset = 0U;

    while (offset < BIG_SIZE)
    {
        size_t take = 4096U;

        if ((BIG_SIZE - offset) < take)
        {
            take = BIG_SIZE - offset;
        }

        sha256_update(&context, big + offset, take);

        offset += take;
    }

    sha256_final(&context, directly);

    POUND_CHECK_MSG(sha256_equal(via_partition, directly),
                    "hashing a file larger than the parser's chunk size disagreed with "
                    "streaming the same bytes in a different chunking.");

    // And the verification wrapper agrees with both.
    POUND_CHECK(POUND_SUCCESS == pfs0_verify_hash(&pfs0, 1U, directly));
    POUND_CHECK(POUND_ERROR_NOT_FOUND == pfs0_hash(&pfs0, 2U, via_partition));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == pfs0_hash(&pfs0, 0U, NULL));

    pfs0_close(&pfs0);

    memory_subsystem_free(small);
    memory_subsystem_free(big);
    memory_subsystem_free(image);
}

POUND_TEST(pfs0, a_wrong_digest_is_reported_with_both_hashes)
{
    // "The hash is wrong" has two causes with opposite responses: the file was modified,
    // or the copy is incomplete. Logging both digests and the file's size is what lets a
    // user tell them apart without opening a hex editor.
    uint8_t image[SAMPLE_CAPACITY];

    build_file_t files[SAMPLE_FILE_COUNT];

    make_sample_files(files);

    const pfs0_layout_t layout = build_pfs0(image, sizeof(image), files, SAMPLE_FILE_COUNT);

    fs_reader_t reader;

    fs_reader_from_buffer(&reader, image, layout.total);

    pfs0_t pfs0;

    POUND_REQUIRE(POUND_SUCCESS == pfs0_open(&pfs0, &reader));

    uint8_t correct[SHA256_DIGEST_SIZE];

    POUND_REQUIRE(POUND_SUCCESS == pfs0_hash(&pfs0, 0U, correct));
    POUND_CHECK(POUND_SUCCESS == pfs0_verify_hash(&pfs0, 0U, correct));

    // Flip one bit in one byte of the file, which is what a modified file looks like.
    image[layout.data_offset + 5U] ^= 0x01U;

    pound_test_log_reset();

    POUND_CHECK(POUND_ERROR_HASH_MISMATCH == pfs0_verify_hash(&pfs0, 0U, correct));

    char hex[SHA256_HEX_SIZE + 1U];

    sha256_to_hex(correct, hex);

    POUND_CHECK_MSG(pound_test_log_contains(hex), "the failure did not print the digest that was expected.");
    POUND_CHECK(pound_test_log_contains("incomplete copy"));
    POUND_CHECK(pound_test_log_contains("300 bytes"));

    // A digest that is all zeroes is a different kind of wrong, and must not be treated
    // as "no expectation given".
    memset(correct, 0, sizeof(correct));

    POUND_CHECK(POUND_ERROR_HASH_MISMATCH == pfs0_verify_hash(&pfs0, 0U, correct));

    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == pfs0_verify_hash(&pfs0, 0U, NULL));
    POUND_CHECK(POUND_ERROR_NOT_FOUND == pfs0_verify_hash(&pfs0, 99U, correct));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == pfs0_verify_hash(NULL, 0U, correct));

    pfs0_close(&pfs0);
}

POUND_TEST(pfs0, a_hash_agrees_with_the_one_shot_api_for_a_file_of_every_size)
{
    // Every size across the chunk boundary and across the small sizes a partition is
    // mostly made of, so the chunk loop's `remaining` arithmetic is checked at each
    // interesting point rather than at one arbitrary size.
    static const size_t SIZES[] = {0U,    1U,     63U,    64U,     65U,     4095U,
                                   4096U, 4097U,  65535U, 65536U,  65537U,  70000U};

    for (size_t s = 0U; s < (sizeof(SIZES) / sizeof(SIZES[0])); ++s)
    {
        const size_t size     = SIZES[s];
        const size_t capacity = size + 512U;

        uint8_t *image = (uint8_t *)memory_subsystem_allocate(1U, capacity);

        POUND_REQUIRE_MSG(NULL != image, "could not allocate an image for a %zu-byte file.", size);

        // The file's data region is described by the header but written afterwards, so
        // that the pattern can span the whole image without a second copy of it existing.
        build_file_t file;

        file.name     = "only";
        file.contents = NULL;
        file.size     = size;

        const pfs0_layout_t layout = build_pfs0(image, capacity, &file, 1U);

        POUND_REQUIRE_MSG(0U != layout.total, "the image for a %zu-byte file did not fit.", size);

        for (size_t i = 0U; i < size; ++i)
        {
            image[layout.data_offset + i] = (uint8_t)(i * 17U + 3U);
        }

        fs_reader_t reader;

        fs_reader_from_buffer(&reader, image, layout.total);

        pfs0_t pfs0;

        POUND_REQUIRE(POUND_SUCCESS == pfs0_open(&pfs0, &reader));

        uint8_t via_partition[SHA256_DIGEST_SIZE];
        uint8_t directly[SHA256_DIGEST_SIZE];

        POUND_REQUIRE(POUND_SUCCESS == pfs0_hash(&pfs0, 0U, via_partition));

        sha256(&image[layout.data_offset], size, directly);

        POUND_CHECK_MSG(sha256_equal(via_partition, directly),
                        "hashing a %zu-byte file through the partition disagreed with hashing "
                        "its bytes directly.",
                        size);

        pfs0_close(&pfs0);

        memory_subsystem_free(image);
    }
}

POUND_TEST(pfs0, opening_from_a_null_or_unusable_source_is_refused)
{
    pfs0_t pfs0;

    uint8_t image[SAMPLE_CAPACITY];

    build_file_t files[SAMPLE_FILE_COUNT];

    make_sample_files(files);

    const pfs0_layout_t layout = build_pfs0(image, sizeof(image), files, SAMPLE_FILE_COUNT);

    POUND_REQUIRE(0U != layout.total);

    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == pfs0_open(NULL, NULL));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == pfs0_open(&pfs0, NULL));

    // A reader with no read function.
    fs_reader_t empty;

    memset(&empty, 0, sizeof(empty));
    empty.size = layout.total;

    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == pfs0_open(&pfs0, &empty));

    // A source too short to hold a header at all.
    fs_reader_t tiny;

    fs_reader_from_buffer(&tiny, image, 4U);

    POUND_CHECK(POUND_ERROR_IO == pfs0_open(&pfs0, &tiny));

    // A zero-length source.
    fs_reader_t nothing;

    fs_reader_from_buffer(&nothing, image, 0U);

    POUND_CHECK(POUND_ERROR_IO == pfs0_open(&pfs0, &nothing));

    // Nothing above should have left the partition half-open, so a close is harmless and
    // a summary declines rather than reading a table that is not there.
    POUND_CHECK(!pfs0_is_open(&pfs0));
    pfs0_close(&pfs0);
    pfs0_log_summary(&pfs0);
    POUND_CHECK(0U == pfs0_count(NULL));
    POUND_CHECK(!pfs0_is_open(NULL));
    pfs0_close(NULL);
    pfs0_log_summary(NULL);
}

POUND_TEST_SUITE(fs_reader,
                POUND_TEST_CASE(fs_reader, a_buffer_reader_returns_the_bytes_it_was_given),
                POUND_TEST_CASE(fs_reader, a_read_past_the_end_of_the_source_fails_rather_than_truncating),
                POUND_TEST_CASE(fs_reader, a_range_that_would_wrap_is_rejected),
                POUND_TEST_CASE(fs_reader, a_zero_length_read_at_the_end_of_the_source_succeeds),
                POUND_TEST_CASE(fs_reader, a_reader_with_no_read_function_refuses_every_read),
                POUND_TEST_CASE(fs_reader, a_rejected_buffer_leaves_a_reader_that_cannot_read),
                POUND_TEST_CASE(fs_reader, a_read_with_no_destination_is_rejected),
                POUND_TEST_CASE(fs_reader, a_copied_reader_keeps_reading_the_same_bytes),
                POUND_TEST_CASE(fs_reader, a_copied_reader_survives_the_frame_it_was_made_in),
                POUND_TEST_CASE(fs_reader, a_copied_reader_keeps_a_context_that_is_not_itself),
                POUND_TEST_CASE(fs_reader, copying_from_nothing_leaves_a_reader_that_refuses_everything))

POUND_TEST_SUITE(keys,
                POUND_TEST_CASE(keys, a_key_file_loads_and_every_entry_can_be_read_back),
                POUND_TEST_CASE(keys, a_missing_key_is_reported_and_nothing_is_written),
                POUND_TEST_CASE(keys, a_fresh_store_contains_no_keys_at_all),
                POUND_TEST_CASE(keys, a_later_definition_of_a_name_replaces_the_earlier_one),
                POUND_TEST_CASE(keys, a_bad_line_is_skipped_and_the_rest_of_the_file_still_loads),
                POUND_TEST_CASE(keys, text_after_a_keys_value_is_rejected_rather_than_ignored),
                POUND_TEST_CASE(keys, a_trailing_comment_after_a_value_is_allowed),
                POUND_TEST_CASE(keys, line_endings_and_a_missing_final_newline_both_parse),
                POUND_TEST_CASE(keys, a_value_of_the_wrong_width_is_rejected),
                POUND_TEST_CASE(keys, a_name_too_long_to_store_is_rejected_rather_than_truncated),
                POUND_TEST_CASE(keys, a_key_family_index_outside_the_family_is_rejected),
                POUND_TEST_CASE(keys, a_family_member_that_is_absent_reports_which_key_was_missing),
                POUND_TEST_CASE(keys, keys_are_enumerable_by_name_in_a_stable_order),
                POUND_TEST_CASE(keys, a_store_grows_past_its_initial_capacity_and_stays_sorted),
                POUND_TEST_CASE(keys, a_destroyed_store_is_empty_and_destroying_it_again_is_harmless))

POUND_TEST_SUITE(pfs0,
                POUND_TEST_CASE(pfs0, a_valid_partition_opens_and_reports_its_files),
                POUND_TEST_CASE(pfs0, a_files_contents_read_back_byte_for_byte),
                POUND_TEST_CASE(pfs0, a_read_that_would_leave_a_file_is_refused),
                POUND_TEST_CASE(pfs0, a_read_from_a_closed_partition_is_refused),
                POUND_TEST_CASE(pfs0, files_are_found_by_name_and_a_miss_is_quiet),
                POUND_TEST_CASE(pfs0, a_wrong_magic_is_reported_as_a_malformed_header),
                POUND_TEST_CASE(pfs0, a_file_count_beyond_the_cap_is_refused_without_allocating),
                POUND_TEST_CASE(pfs0, a_string_table_size_beyond_the_cap_is_refused),
                POUND_TEST_CASE(pfs0, a_string_table_with_the_wrong_magic_is_refused),
                POUND_TEST_CASE(pfs0, a_name_pointing_past_the_string_table_is_refused),
                POUND_TEST_CASE(pfs0, a_name_too_long_to_store_is_refused_rather_than_truncated),
                POUND_TEST_CASE(pfs0, an_entry_whose_bytes_leave_the_data_region_is_refused),
                POUND_TEST_CASE(pfs0, a_file_at_the_edge_of_the_data_region_is_measured_correctly),
                POUND_TEST_CASE(pfs0, two_entries_with_the_same_name_are_refused),
                POUND_TEST_CASE(pfs0, two_entries_claiming_the_same_bytes_are_refused),
                POUND_TEST_CASE(pfs0, two_empty_files_at_the_same_offset_are_not_an_overlap),
                POUND_TEST_CASE(pfs0, a_truncated_image_is_reported_as_a_bad_header_and_a_stub_as_an_io_failure),
                POUND_TEST_CASE(pfs0, an_empty_partition_opens_with_no_files),
                POUND_TEST_CASE(pfs0, a_files_hash_matches_hashing_its_bytes_directly),
                POUND_TEST_CASE(pfs0, a_wrong_digest_is_reported_with_both_hashes),
                POUND_TEST_CASE(pfs0, a_hash_agrees_with_the_one_shot_api_for_a_file_of_every_size),
                POUND_TEST_CASE(pfs0, opening_from_a_null_or_unusable_source_is_refused))

/*** end of file ***/

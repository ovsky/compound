//! Tests for NCA3 header, key-area, section and ExeFS handling.
//!
//! The images here are synthetic: a real NCA is gigabytes and its keys are Nintendo's.
//! What a synthetic image can prove is that the offsets, the XTS header sectors, the
//! key-area decryption, the section counter construction and the ExeFS lookup agree
//! with the documented layout. The independent AES vectors in `test_aes.c` are what
//! make those checks meaningful rather than self-consistent.

#include "pound_test.h"

#include "crypto/aes.h"
#include "fs/keys.h"
#include "loader/package/nca.h"
#include <stdlib.h>
#include <string.h>

#define NCA_TEST_SIZE         0x3000U
#define NCA_TEST_SECTION_START 0xE00U
#define NCA_TEST_SECTION_END   NCA_TEST_SIZE
#define NCA_TEST_SECTION_SIZE (NCA_TEST_SECTION_END - NCA_TEST_SECTION_START)
#define NCA_TEST_NSO_SIZE     0x100U
#define NCA_TEST_PFS0_OFFSET  0x31U // 16-byte header + 24-byte entry + 9-byte string table

static void
store_le32(uint8_t *POUND_RESTRICT bytes, const uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
    bytes[2] = (uint8_t)(value >> 16U);
    bytes[3] = (uint8_t)(value >> 24U);
}

static void
store_le64(uint8_t *POUND_RESTRICT bytes, const uint64_t value)
{
    store_le32(bytes, (uint32_t)value);
    store_le32(bytes + 4U, (uint32_t)(value >> 32U));
}

/// Builds the PFS0 ExeFS section: one file, `main`, holding a fake NSO.
static size_t
build_exefs(uint8_t *POUND_RESTRICT section)
{
    memset(section, 0, NCA_TEST_SECTION_SIZE);

    memcpy(section, "PFS0", 4U);
    store_le32(section + 4U, 1U);
    store_le32(section + 8U, 9U); // string-table magic plus "main\0"
    store_le32(section + 12U, 0U);

    store_le64(section + 16U, 0U);
    store_le64(section + 24U, NCA_TEST_NSO_SIZE);
    store_le32(section + 32U, 0U);
    store_le32(section + 36U, 0U);

    memcpy(section + 40U, "PFS0", 4U);
    memcpy(section + 44U, "main", 5U);

    uint8_t *nso = section + NCA_TEST_PFS0_OFFSET;
    memcpy(nso, "NSO0", 4U);

    for (size_t i = 4U; i < NCA_TEST_NSO_SIZE; ++i)
    {
        nso[i] = (uint8_t)(0x40U + i);
    }

    return NCA_TEST_PFS0_OFFSET + NCA_TEST_NSO_SIZE;
}

/// Writes a plaintext NCA3 header and encrypted key area.
///
/// `crypt_type` is the section encryption type. `header_key` and `key_area_key` are
/// always supplied here; the caller chooses whether the header itself is encrypted.
static void
build_nca(uint8_t *POUND_RESTRICT image, const uint8_t crypt_type, const uint8_t *POUND_RESTRICT header_key,
          const uint8_t *POUND_RESTRICT key_area_key, const bool encrypt_header,
          const bool rights_id_present)
{
    memset(image, 0, NCA_TEST_SIZE);

    uint8_t *header = image;

    memcpy(header + 0x200U, "NCA3", 4U);
    header[0x204U] = 0U; // download
    header[0x205U] = 0U; // program
    header[0x206U] = 0U; // key generation
    header[0x207U] = 0U; // key-area index

    store_le64(header + 0x208U, NCA_TEST_SIZE);
    store_le64(header + 0x210U, UINT64_C(0x0100000000001234));
    header[0x220U] = 0U;

    if (rights_id_present)
    {
        for (size_t i = 0U; i < 16U; ++i)
        {
            header[0x230U + i] = (uint8_t)(0x10U + i);
        }
    }

    store_le32(header + 0x240U, NCA_TEST_SECTION_START / POUND_NCA_SECTOR_SIZE);
    store_le32(header + 0x244U, NCA_TEST_SECTION_END / POUND_NCA_SECTOR_SIZE);

    uint8_t *fs_header = header + 0x400U;
    fs_header[0x2U] = 0U; // PFS0 partition
    fs_header[0x3U] = 0U; // PFS0 filesystem
    fs_header[0x4U] = crypt_type;

    store_le64(fs_header + 0x40U, 0U);
    store_le64(fs_header + 0x48U, NCA_TEST_SECTION_SIZE);

    for (size_t i = 0U; i < 8U; ++i)
    {
        fs_header[0x140U + i] = (uint8_t)i;
    }

    uint8_t plain_keys[0x40];
    for (size_t i = 0U; i < sizeof(plain_keys); ++i)
    {
        plain_keys[i] = (uint8_t)i;
    }

    pound_aes_t aes;
    POUND_REQUIRE(POUND_SUCCESS == pound_aes_init(&aes, key_area_key, 16U));
    POUND_REQUIRE(POUND_SUCCESS == pound_aes_ecb_encrypt(&aes, plain_keys, header + 0x300U, sizeof(plain_keys)));

    if (encrypt_header)
    {
        uint8_t raw[POUND_NCA_HEADER_SIZE];
        memcpy(raw, header, sizeof(raw));

        for (uint64_t sector = 0U; sector < (POUND_NCA_HEADER_SIZE / POUND_NCA_SECTOR_SIZE); ++sector)
        {
            uint8_t tweak[POUND_AES_BLOCK_SIZE] = {0};
            tweak[15] = (uint8_t)sector;

            POUND_REQUIRE(POUND_SUCCESS == pound_aes_xts_encrypt(header_key,
                                                                 32U,
                                                                 tweak,
                                                                 raw + (sector * POUND_NCA_SECTOR_SIZE),
                                                                 header + (sector * POUND_NCA_SECTOR_SIZE),
                                                                 POUND_NCA_SECTOR_SIZE,
                                                                 POUND_NCA_SECTOR_SIZE));
        }
    }
}

/// Builds the section counter exactly as the format documents it, independently of
/// the private helper in `nca.c`.
static void
build_section_counter(uint8_t counter[POUND_AES_BLOCK_SIZE], const uint8_t section_ctr[8], uint64_t offset)
{
    offset >>= 4U;

    for (uint32_t i = 0U; i < 8U; ++i)
    {
        counter[i]        = section_ctr[7U - i];
        counter[15U - i] = (uint8_t)(offset & 0xFFU);
        offset >>= 8U;
    }
}

/// Encrypts the section in place with the key-area CTR key or XTS key pair.
static void
encrypt_section(uint8_t *POUND_RESTRICT image, const uint8_t *POUND_RESTRICT plain_keys, const uint8_t crypt_type)
{
    uint8_t *section = image + NCA_TEST_SECTION_START;
    const uint8_t *fs_header = image + 0x400U;

    if (POUND_NCA_CRYPT_CTR == crypt_type)
    {
        uint8_t counter[POUND_AES_BLOCK_SIZE];
        build_section_counter(counter, fs_header + 0x140U, NCA_TEST_SECTION_START);

        pound_aes_t aes;
        POUND_REQUIRE(POUND_SUCCESS == pound_aes_init(&aes, plain_keys + 0x20U, 16U));
        POUND_REQUIRE(POUND_SUCCESS == pound_aes_ctr_xcrypt(&aes, counter, section, section, NCA_TEST_SECTION_SIZE));
    }
    else if (POUND_NCA_CRYPT_XTS == crypt_type)
    {
        for (size_t offset = 0U; offset < NCA_TEST_SECTION_SIZE; offset += POUND_NCA_SECTOR_SIZE)
        {
            uint8_t tweak[POUND_AES_BLOCK_SIZE] = {0};
            tweak[15] = (uint8_t)(offset / POUND_NCA_SECTOR_SIZE);

            POUND_REQUIRE(POUND_SUCCESS == pound_aes_xts_encrypt(plain_keys,
                                                                 32U,
                                                                 tweak,
                                                                 section + offset,
                                                                 section + offset,
                                                                 POUND_NCA_SECTOR_SIZE,
                                                                 POUND_NCA_SECTOR_SIZE));
        }
    }
}

static void
fill_test_keys(key_store_t *POUND_RESTRICT keys, const uint8_t *POUND_RESTRICT header_key,
               const uint8_t *POUND_RESTRICT key_area_key)
{
    key_store_init(keys);
    POUND_REQUIRE(POUND_SUCCESS == key_store_add_sized(keys, "header_key", header_key, 32U));
    POUND_REQUIRE(POUND_SUCCESS == key_store_add(keys, "key_area_key_application_00", key_area_key));
}

/// Opens the image and checks that the ExeFS main survives the whole path.
static void
open_and_check_main(const uint8_t *POUND_RESTRICT image, key_store_t *POUND_RESTRICT keys,
                    const uint8_t *POUND_RESTRICT title_key)
{
    fs_reader_t reader;
    fs_reader_from_buffer(&reader, image, NCA_TEST_SIZE);

    pound_nca_info_t info;
    POUND_REQUIRE(POUND_SUCCESS == pound_nca_inspect(&reader, 0U, NCA_TEST_SIZE, keys, &info));
    POUND_CHECK_EQ_U64(info.program_id, UINT64_C(0x0100000000001234));
    POUND_CHECK_EQ_U64(info.content_type, 0U);

    pound_nca_t nca;
    POUND_REQUIRE(POUND_SUCCESS == pound_nca_open(&nca, &reader, 0U, NCA_TEST_SIZE, keys, title_key));

    uint8_t *main_data = NULL;
    size_t   main_size = 0U;
    POUND_REQUIRE(POUND_SUCCESS == pound_nca_extract_main(&nca, &main_data, &main_size));
    POUND_CHECK_EQ_U64(main_size, NCA_TEST_NSO_SIZE);
    POUND_CHECK(0 == memcmp(main_data, "NSO0", 4U));

    memset(main_data, 0, main_size);
    free(main_data);
    pound_nca_close(&nca);
}

POUND_TEST(nca, a_plaintext_none_section_extracts_main)
{
    static const uint8_t HEADER_KEY[32] = {0};
    static const uint8_t KEY_AREA_KEY[16] = {0};

    uint8_t image[NCA_TEST_SIZE];
    build_nca(image, POUND_NCA_CRYPT_NONE, HEADER_KEY, KEY_AREA_KEY, false, false);
    build_exefs(image + NCA_TEST_SECTION_START);

    key_store_t keys;
    fill_test_keys(&keys, HEADER_KEY, KEY_AREA_KEY);

    open_and_check_main(image, &keys, NULL);

    key_store_destroy(&keys);
}

POUND_TEST(nca, a_ctr_section_uses_the_documented_counter)
{
    static const uint8_t HEADER_KEY[32] = {1U, 2U, 3U, 4U};
    static const uint8_t KEY_AREA_KEY[16] = {5U, 6U, 7U, 8U};

    uint8_t plain_keys[0x40];
    for (size_t i = 0U; i < sizeof(plain_keys); ++i)
    {
        plain_keys[i] = (uint8_t)(0x80U + i);
    }

    uint8_t image[NCA_TEST_SIZE];
    build_nca(image, POUND_NCA_CRYPT_CTR, HEADER_KEY, KEY_AREA_KEY, true, false);
    build_exefs(image + NCA_TEST_SECTION_START);

    // `build_nca` wrote the key area from the fixed 0..0x3F pattern. Use the same
    // pattern here so the section key matches the one the parser recovers.
    for (size_t i = 0U; i < sizeof(plain_keys); ++i)
    {
        plain_keys[i] = (uint8_t)i;
    }

    encrypt_section(image, plain_keys, POUND_NCA_CRYPT_CTR);

    key_store_t keys;
    fill_test_keys(&keys, HEADER_KEY, KEY_AREA_KEY);

    open_and_check_main(image, &keys, NULL);

    key_store_destroy(&keys);
}

POUND_TEST(nca, an_xts_section_and_encrypted_header_both_decrypt)
{
    static const uint8_t HEADER_KEY[32] = {0x30U, 0x31U, 0x32U, 0x33U};
    static const uint8_t KEY_AREA_KEY[16] = {0x40U, 0x41U, 0x42U, 0x43U};

    uint8_t plain_keys[0x40];
    for (size_t i = 0U; i < sizeof(plain_keys); ++i)
    {
        plain_keys[i] = (uint8_t)i;
    }

    uint8_t image[NCA_TEST_SIZE];
    build_nca(image, POUND_NCA_CRYPT_XTS, HEADER_KEY, KEY_AREA_KEY, true, false);
    build_exefs(image + NCA_TEST_SECTION_START);
    encrypt_section(image, plain_keys, POUND_NCA_CRYPT_XTS);

    key_store_t keys;
    fill_test_keys(&keys, HEADER_KEY, KEY_AREA_KEY);

    open_and_check_main(image, &keys, NULL);

    key_store_destroy(&keys);
}

POUND_TEST(nca, missing_keys_are_refused_before_reading_sections)
{
    static const uint8_t HEADER_KEY[32] = {0};
    static const uint8_t KEY_AREA_KEY[16] = {0};

    uint8_t image[NCA_TEST_SIZE];
    build_nca(image, POUND_NCA_CRYPT_NONE, HEADER_KEY, KEY_AREA_KEY, false, false);
    build_exefs(image + NCA_TEST_SECTION_START);

    fs_reader_t reader;
    fs_reader_from_buffer(&reader, image, NCA_TEST_SIZE);

    key_store_t empty;
    key_store_init(&empty);

    pound_nca_t nca;
    POUND_CHECK_EQ_I64(pound_nca_open(&nca, &reader, 0U, NCA_TEST_SIZE, &empty, NULL),
                       POUND_ERROR_KEY_MISSING);

    // A 32-byte key-area key where the format has a 16-byte one must not be
    // truncated into a plausible-looking key.
    key_store_t wrong_size;
    key_store_init(&wrong_size);
    static const uint8_t TOO_WIDE[32] = {0};
    POUND_REQUIRE(POUND_SUCCESS == key_store_add_sized(&wrong_size, "key_area_key_application_00", TOO_WIDE, 32U));
    POUND_CHECK_EQ_I64(pound_nca_open(&nca, &reader, 0U, NCA_TEST_SIZE, &wrong_size, NULL),
                       POUND_ERROR_KEY_SIZE);

    key_store_destroy(&wrong_size);
    key_store_destroy(&empty);
}

POUND_TEST(nca, a_rights_id_nca_requires_a_title_key)
{
    static const uint8_t HEADER_KEY[32] = {0};
    static const uint8_t KEY_AREA_KEY[16] = {0};

    uint8_t image[NCA_TEST_SIZE];
    build_nca(image, POUND_NCA_CRYPT_CTR, HEADER_KEY, KEY_AREA_KEY, true, true);
    build_exefs(image + NCA_TEST_SECTION_START);

    fs_reader_t reader;
    fs_reader_from_buffer(&reader, image, NCA_TEST_SIZE);

    key_store_t keys;
    fill_test_keys(&keys, HEADER_KEY, KEY_AREA_KEY);

    pound_nca_t nca;
    POUND_CHECK_EQ_I64(pound_nca_open(&nca, &reader, 0U, NCA_TEST_SIZE, &keys, NULL),
                       POUND_ERROR_KEY_MISSING);

    key_store_destroy(&keys);
}

POUND_TEST(nca, unsupported_layouts_are_named)
{
    static const uint8_t HEADER_KEY[32] = {0};
    static const uint8_t KEY_AREA_KEY[16] = {0};

    uint8_t image[NCA_TEST_SIZE];
    build_nca(image, POUND_NCA_CRYPT_BKTR, HEADER_KEY, KEY_AREA_KEY, false, false);
    build_exefs(image + NCA_TEST_SECTION_START);

    fs_reader_t reader;
    fs_reader_from_buffer(&reader, image, NCA_TEST_SIZE);

    key_store_t keys;
    fill_test_keys(&keys, HEADER_KEY, KEY_AREA_KEY);

    pound_nca_t nca;
    POUND_CHECK_EQ_I64(pound_nca_open(&nca, &reader, 0U, NCA_TEST_SIZE, &keys, NULL),
                       POUND_ERROR_UNSUPPORTED_FEATURE);

    // NCA2 is recognisable but deliberately not approximated with the NCA3 path.
    memcpy(image + 0x200U, "NCA2", 4U);
    POUND_CHECK_EQ_I64(pound_nca_open(&nca, &reader, 0U, NCA_TEST_SIZE, &keys, NULL),
                       POUND_ERROR_UNSUPPORTED_FEATURE);

    key_store_destroy(&keys);
}

POUND_TEST(nca, malformed_headers_are_refused)
{
    uint8_t image[NCA_TEST_SIZE];
    memset(image, 0, sizeof(image));

    fs_reader_t reader;
    fs_reader_from_buffer(&reader, image, NCA_TEST_SIZE);

    key_store_t keys;
    key_store_init(&keys);

    pound_nca_t nca;
    POUND_CHECK_EQ_I64(pound_nca_open(&nca, &reader, 0U, 0x100U, &keys, NULL),
                       POUND_ERROR_MALFORMED_HEADER);

    memcpy(image + 0x200U, "NCA3", 4U);
    store_le64(image + 0x208U, 0x2000U);
    store_le32(image + 0x240U, 0x10U);
    store_le32(image + 0x244U, 0x08U); // end before start
    POUND_CHECK_EQ_I64(pound_nca_open(&nca, &reader, 0U, NCA_TEST_SIZE, &keys, NULL),
                       POUND_ERROR_MALFORMED_HEADER);

    key_store_destroy(&keys);
}

POUND_TEST_SUITE(nca,
                 POUND_TEST_CASE(nca, a_plaintext_none_section_extracts_main),
                 POUND_TEST_CASE(nca, a_ctr_section_uses_the_documented_counter),
                 POUND_TEST_CASE(nca, an_xts_section_and_encrypted_header_both_decrypt),
                 POUND_TEST_CASE(nca, missing_keys_are_refused_before_reading_sections),
                 POUND_TEST_CASE(nca, a_rights_id_nca_requires_a_title_key),
                 POUND_TEST_CASE(nca, unsupported_layouts_are_named),
                 POUND_TEST_CASE(nca, malformed_headers_are_refused))

/*** end of file ***/

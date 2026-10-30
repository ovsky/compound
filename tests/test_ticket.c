//! Tests for ticket parsing and common-title-key decryption.
//!
//! A ticket is a signed structure, but Compound cannot verify the signature without
//! Nintendo's certificate chain and does not claim to. These tests therefore cover
//! exactly what the loader relies on: the signature-type-dependent offset, the
//! rights id, the common/personalized distinction, and decryption with a
//! caller-supplied `titlekek`.

#include "pound_test.h"

#include "fs/keys.h"
#include "loader/package/ticket.h"
#include <string.h>

#define TICKET_SIGNATURE_SIZE 0x40U
#define TICKET_SIZE (TICKET_SIGNATURE_SIZE + 0x2C0U)

static void
store_be32(uint8_t *POUND_RESTRICT bytes, const uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24U);
    bytes[1] = (uint8_t)(value >> 16U);
    bytes[2] = (uint8_t)(value >> 8U);
    bytes[3] = (uint8_t)value;
}

/// Builds a minimal ticket with an RSA-2048 signature block.
static void
build_ticket(uint8_t *POUND_RESTRICT ticket, const uint8_t title_key_type)
{
    memset(ticket, 0, TICKET_SIZE);
    store_be32(ticket, 0x010003U);

    uint8_t *data = ticket + TICKET_SIGNATURE_SIZE;

    // The encrypted title key is placed at ticket-data offset 0x40.
    for (size_t i = 0U; i < POUND_TICKET_TITLE_KEY_SIZE; ++i)
    {
        data[0x40U + i] = (uint8_t)(0xA0U + i);
    }

    data[0x141U] = title_key_type;

    for (size_t i = 0U; i < POUND_TICKET_RIGHTS_ID_SIZE; ++i)
    {
        data[0x160U + i] = (uint8_t)(0x10U + i);
    }
}

POUND_TEST(ticket, parses_the_rights_id_and_title_key)
{
    uint8_t raw[TICKET_SIZE];
    build_ticket(raw, 0U);

    pound_ticket_t ticket;
    POUND_REQUIRE(POUND_SUCCESS == pound_ticket_parse(&ticket, raw, sizeof(raw)));

    for (size_t i = 0U; i < POUND_TICKET_RIGHTS_ID_SIZE; ++i)
    {
        POUND_CHECK_EQ_U64(ticket.rights_id[i], (uint8_t)(0x10U + i));
    }

    for (size_t i = 0U; i < POUND_TICKET_TITLE_KEY_SIZE; ++i)
    {
        POUND_CHECK_EQ_U64(ticket.encrypted_title_key[i], (uint8_t)(0xA0U + i));
    }

    POUND_CHECK_EQ_U64(ticket.title_key_type, 0U);
}

POUND_TEST(ticket, a_personalized_ticket_is_refused)
{
    uint8_t raw[TICKET_SIZE];
    build_ticket(raw, 1U);

    pound_ticket_t ticket;
    POUND_CHECK_EQ_I64(pound_ticket_parse(&ticket, raw, sizeof(raw)), POUND_ERROR_UNSUPPORTED_FEATURE);
}

POUND_TEST(ticket, malformed_tickets_are_refused)
{
    uint8_t raw[TICKET_SIZE];
    build_ticket(raw, 0U);

    pound_ticket_t ticket;

    POUND_CHECK_EQ_I64(pound_ticket_parse(NULL, raw, sizeof(raw)), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(pound_ticket_parse(&ticket, NULL, sizeof(raw)), POUND_ERROR_INVALID_ARGUMENT);

    uint8_t short_raw[3] = {0};
    POUND_CHECK_EQ_I64(pound_ticket_parse(&ticket, short_raw, sizeof(short_raw)),
                       POUND_ERROR_MALFORMED_HEADER);

    store_be32(raw, 0x010099U);
    POUND_CHECK_EQ_I64(pound_ticket_parse(&ticket, raw, sizeof(raw)), POUND_ERROR_UNSUPPORTED_FEATURE);

    build_ticket(raw, 0U);
    POUND_CHECK_EQ_I64(pound_ticket_parse(&ticket, raw, TICKET_SIZE - 1U), POUND_ERROR_MALFORMED_HEADER);
}

POUND_TEST(ticket, the_common_title_key_decrypts_with_titlekek_zero)
{
    uint8_t raw[TICKET_SIZE];
    build_ticket(raw, 0U);

    // The FIPS 197 AES-128 example, read backwards: the titlekek is the key, the
    // stored ciphertext is the known ciphertext, and the recovered title key is the
    // known plaintext. This makes the ticket test depend on AES being right rather
    // than on the same ticket code producing the same bytes.
    static const uint8_t TITLEKEK[16]
        = { 0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U,
            0x08U, 0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU };
    static const uint8_t CIPHERTEXT[16]
        = { 0x69U, 0xC4U, 0xE0U, 0xD8U, 0x6AU, 0x7BU, 0x04U, 0x30U,
            0xD8U, 0xCDU, 0xB7U, 0x80U, 0x70U, 0xB4U, 0xC5U, 0x5AU };
    static const uint8_t EXPECTED[16]
        = { 0x00U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U,
            0x88U, 0x99U, 0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0xFFU };

    uint8_t *data = raw + TICKET_SIGNATURE_SIZE;
    memcpy(data + 0x40U, CIPHERTEXT, sizeof(CIPHERTEXT));

    pound_ticket_t ticket;
    POUND_REQUIRE(POUND_SUCCESS == pound_ticket_parse(&ticket, raw, sizeof(raw)));

    key_store_t keys;
    key_store_init(&keys);
    POUND_REQUIRE(POUND_SUCCESS == key_store_add(&keys, "titlekek_00", TITLEKEK));

    uint8_t title_key[POUND_TICKET_TITLE_KEY_SIZE] = {0};
    POUND_REQUIRE(POUND_SUCCESS == pound_ticket_decrypt_title_key(&ticket, &keys, 0U, title_key));
    POUND_CHECK(0 == memcmp(title_key, EXPECTED, sizeof(EXPECTED)));

    key_store_destroy(&keys);
}

POUND_TEST(ticket, a_missing_or_wrong_sized_titlekek_is_not_substituted)
{
    uint8_t raw[TICKET_SIZE];
    build_ticket(raw, 0U);

    pound_ticket_t ticket;
    POUND_REQUIRE(POUND_SUCCESS == pound_ticket_parse(&ticket, raw, sizeof(raw)));

    key_store_t keys;
    key_store_init(&keys);

    uint8_t title_key[POUND_TICKET_TITLE_KEY_SIZE] = {0};
    POUND_CHECK_EQ_I64(pound_ticket_decrypt_title_key(&ticket, &keys, 0U, title_key),
                       POUND_ERROR_KEY_MISSING);

    uint8_t thirty_two[32] = {0};
    POUND_REQUIRE(POUND_SUCCESS == key_store_add_sized(&keys, "titlekek_00", thirty_two, sizeof(thirty_two)));
    POUND_CHECK_EQ_I64(pound_ticket_decrypt_title_key(&ticket, &keys, 0U, title_key),
                       POUND_ERROR_KEY_SIZE);

    key_store_destroy(&keys);
}

POUND_TEST(ticket, all_signature_sizes_locate_the_same_data)
{
    static const struct
    {
        uint32_t type;
        size_t   signature_size;
    } CASES[] = {
        { 0x010000U, 0x240U },
        { 0x010001U, 0x140U },
        { 0x010002U, 0x80U },
        { 0x010003U, 0x240U },
        { 0x010004U, 0x140U },
        { 0x010005U, 0x80U },
        { 0x010006U, 0x40U },
    };

    for (size_t index = 0U; index < (sizeof(CASES) / sizeof(CASES[0])); ++index)
    {
        size_t size = CASES[index].signature_size + 0x2C0U;
        uint8_t raw[0x240U + 0x2C0U];
        memset(raw, 0, sizeof(raw));
        store_be32(raw, CASES[index].type);

        uint8_t *data = raw + CASES[index].signature_size;
        memcpy(data + 0x160U, "RIGHTSID01234567", 16U);

        pound_ticket_t ticket;
        POUND_REQUIRE(POUND_SUCCESS == pound_ticket_parse(&ticket, raw, size));
        POUND_CHECK(0 == memcmp(ticket.rights_id, "RIGHTSID01234567", 16U));
    }
}

POUND_TEST_SUITE(ticket,
                 POUND_TEST_CASE(ticket, parses_the_rights_id_and_title_key),
                 POUND_TEST_CASE(ticket, a_personalized_ticket_is_refused),
                 POUND_TEST_CASE(ticket, malformed_tickets_are_refused),
                 POUND_TEST_CASE(ticket, the_common_title_key_decrypts_with_titlekek_zero),
                 POUND_TEST_CASE(ticket, a_missing_or_wrong_sized_titlekek_is_not_substituted),
                 POUND_TEST_CASE(ticket, all_signature_sizes_locate_the_same_data))

/*** end of file ***/

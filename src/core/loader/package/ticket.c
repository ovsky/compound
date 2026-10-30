//! Ticket parsing and common-title-key decryption.

#include "loader/package/ticket.h"

#include "crypto/aes.h"
#include "log.h"
#include <string.h>

/// Ticket data starts after the signature data.
#define TICKET_DATA_SIZE 0x2C0U

/// Offsets inside ticket data.
#define TICKET_TITLE_KEY_OFFSET       0x40U
#define TICKET_TITLE_KEY_TYPE_OFFSET  0x141U
#define TICKET_MASTER_KEY_REV_OFFSET  0x145U
#define TICKET_RIGHTS_ID_OFFSET       0x160U

static uint32_t
load_be32(const uint8_t *POUND_RESTRICT bytes)
{
    return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) | ((uint32_t)bytes[2] << 8U)
         | (uint32_t)bytes[3];
}

/// Returns the total signature-data length for a ticket signature type.
///
/// The table is the one on Switchbrew's Ticket page: four bytes of type, the
/// signature, then padding to the next 0x40-byte boundary.
static bool
signature_data_size(const uint32_t type, size_t *POUND_RESTRICT out_size)
{
    switch (type)
    {
        case 0x010000U:
        case 0x010003U:
            *out_size = 0x240U;
            return true;
        case 0x010001U:
        case 0x010004U:
            *out_size = 0x140U;
            return true;
        case 0x010002U:
        case 0x010005U:
            *out_size = 0x80U;
            return true;
        case 0x010006U:
            *out_size = 0x40U;
            return true;
        default:
            return false;
    }
}

error_t
pound_ticket_parse(pound_ticket_t *POUND_RESTRICT ticket, const uint8_t *POUND_RESTRICT data,
                   const size_t size)
{
    if (POUND_UNLIKELY(NULL == ticket) || POUND_UNLIKELY(NULL == data))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: ticket parsing was handed a NULL pointer.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    memset(ticket, 0, sizeof(*ticket));

    if (size < 4U)
    {
        POUND_LOG_ERROR(&thread_logger, "A %zu-byte ticket is too short to hold a signature type.", size);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    size_t data_offset = 0U;
    if (!signature_data_size(load_be32(data), &data_offset))
    {
        POUND_LOG_ERROR(&thread_logger, "Ticket signature type 0x%08X is not one Compound recognises.",
                        load_be32(data));
        return POUND_ERROR_UNSUPPORTED_FEATURE;
    }

    if ((size < data_offset) || ((size - data_offset) < TICKET_DATA_SIZE))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "A %zu-byte ticket does not contain the %u bytes of ticket data after its "
                        "%zu-byte signature block.",
                        size,
                        TICKET_DATA_SIZE,
                        data_offset);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    const uint8_t *POUND_RESTRICT ticket_data = data + data_offset;

    memcpy(ticket->rights_id, ticket_data + TICKET_RIGHTS_ID_OFFSET, sizeof(ticket->rights_id));
    memcpy(ticket->encrypted_title_key, ticket_data + TICKET_TITLE_KEY_OFFSET,
           sizeof(ticket->encrypted_title_key));

    ticket->title_key_type      = ticket_data[TICKET_TITLE_KEY_TYPE_OFFSET];
    ticket->master_key_revision = ticket_data[TICKET_MASTER_KEY_REV_OFFSET];

    if (0U != ticket->title_key_type)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Ticket title-key type %u is personalized and needs a console-unique RSA key; "
                        "only common tickets (type 0) can be opened.",
                        ticket->title_key_type);
        return POUND_ERROR_UNSUPPORTED_FEATURE;
    }

    return POUND_SUCCESS;
}

error_t
pound_ticket_decrypt_title_key(const pound_ticket_t *POUND_RESTRICT ticket,
                               const key_store_t *POUND_RESTRICT keys,
                               const unsigned int titlekek_index,
                               uint8_t out[POUND_TICKET_TITLE_KEY_SIZE])
{
    if (POUND_UNLIKELY(NULL == ticket) || POUND_UNLIKELY(NULL == keys) || POUND_UNLIKELY(NULL == out))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: title-key decryption was handed a NULL pointer.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    uint8_t titlekek[POUND_KEY_SIZE];
    const error_t key_error = key_store_get_titlekek(keys, titlekek_index, titlekek);
    if (POUND_SUCCESS != key_error)
    {
        return key_error;
    }

    pound_aes_t aes;
    error_t     status = pound_aes_init(&aes, titlekek, sizeof(titlekek));
    if (POUND_SUCCESS == status)
    {
        status = pound_aes_ecb_decrypt(&aes, ticket->encrypted_title_key, out, POUND_TICKET_TITLE_KEY_SIZE);
    }

    memset(&aes, 0, sizeof(aes));
    memset(titlekek, 0, sizeof(titlekek));
    return status;
}

/*** end of file ***/

#ifndef POUND_LOADER_PACKAGE_TICKET_H
#define POUND_LOADER_PACKAGE_TICKET_H

//! The small part of a Switch ticket that the content loader needs: the rights
//! identifier and the AES-128 title key.
//!
//! A ticket is signed, but Compound cannot verify that signature without
//! Nintendo's certificate chain, and it deliberately does not pretend otherwise.
//! What it can do is decrypt the title key with the user's `titlekek_NN`, which is
//! all a loader needs in order to open an NCA that names the same rights id. The
//! signature is therefore *not* an authentication check here and must not be
//! described as one.
//!
//! Personalized tickets (title-key type 1) store an RSA-encrypted key block that
//! requires a console-unique private key. This build refuses them rather than
//! treating the ciphertext as a title key.

#include "attributes.h"
#include "errors.h"
#include "fs/keys.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Size of a common ticket's title-key block.
#define POUND_TICKET_TITLE_KEY_SIZE 16U

/// The rights id is sixteen bytes.
#define POUND_TICKET_RIGHTS_ID_SIZE 16U

/// The fields extracted from a ticket.
typedef struct
{
    uint8_t rights_id[POUND_TICKET_RIGHTS_ID_SIZE];

    /// The encrypted common title key exactly as stored.
    uint8_t encrypted_title_key[POUND_TICKET_TITLE_KEY_SIZE];

    /// Master-key generation used to choose `titlekek_NN`.
    uint8_t master_key_revision;

    /// Ticket title-key type. Only type 0 (common) is supported.
    uint8_t title_key_type;
} pound_ticket_t;

/// Parses one ticket from a byte buffer.
///
/// The signature-data length is derived from the signature type, so a ticket with a
/// 2048-bit or 4096-bit signature parses at the right ticket-data offset instead of
/// assuming one fixed layout. Unknown signature types are refused.
error_t pound_ticket_parse(pound_ticket_t *POUND_RESTRICT ticket, const uint8_t *POUND_RESTRICT data,
                           size_t size);

/// Decrypts `ticket->encrypted_title_key` into `out` using `titlekek_NN`.
///
/// `titlekek_index` is the NCA's key generation, because that is the index the
/// content uses to encrypt the common title key. It is deliberately not read from
/// the ticket: the ticket's own master-key revision describes its signature
/// generation, which is not necessarily the generation that encrypted the NCA.
///
/// The output is written only on success. A missing or wrong-sized titlekek is
/// reported as the key store reports it; nothing is substituted.
error_t pound_ticket_decrypt_title_key(const pound_ticket_t *POUND_RESTRICT ticket,
                                       const key_store_t *POUND_RESTRICT keys,
                                       unsigned int titlekek_index,
                                       uint8_t out[POUND_TICKET_TITLE_KEY_SIZE]);

#ifdef __cplusplus
}
#endif

#endif // POUND_LOADER_PACKAGE_TICKET_H

/*** end of file ***/

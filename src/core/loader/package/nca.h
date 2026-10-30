#ifndef POUND_LOADER_PACKAGE_NCA_H
#define POUND_LOADER_PACKAGE_NCA_H

//! NCA3 header, key-area and section decryption.
//!
//! This is the first half of loading a real package: an NCA stores its own
//! metadata in an XTS-encrypted header the size of six sectors, encrypts its key
//! area with a key from the user's key file, and then encrypts each section with
//! a key from that area. Nothing here guesses a key. A missing key is reported by
//! the key store before any ciphertext is treated as plaintext, and a header whose
//! decrypted magic is not `NCA3` is refused rather than searched for another
//! structure inside the noise.
//!
//! ## What is deliberately not here
//!
//! There is no signature verification and no claim that a successfully decrypted
//! NCA is authentic. Verifying Nintendo's signature chain needs Nintendo's public
//! certificates, which this project does not ship. Decryption proves only that the
//! supplied key matches, exactly like opening an encrypted archive.
//!
//! NCA2 is not handled. Its header is decrypted differently and this build has no
//! test image that could prove an implementation correct; it is refused as an
//! unsupported feature instead of being approximated with the NCA3 path.
//!
//! Section decryption is bounded. A section larger than
//! `POUND_NCA_MAX_SECTION_SIZE` is refused before allocation. That keeps a
//! corrupted length from asking the host for an arbitrary amount of memory. It is
//! a loader limit, not a statement that larger content does not exist.

#include "attributes.h"
#include "errors.h"
#include "fs/fs_reader.h"
#include "fs/keys.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// NCA header size: six XTS sectors.
#define POUND_NCA_HEADER_SIZE 0xC00U

/// NCA sector size, in bytes.
#define POUND_NCA_SECTOR_SIZE 0x200U

/// Largest section this loader will decrypt into one host buffer (512 MiB).
#define POUND_NCA_MAX_SECTION_SIZE (UINT64_C(512) * 1024U * 1024U)

/// Number of sections an NCA declares.
#define POUND_NCA_SECTION_COUNT 4U

/// Partition and filesystem types from an NCA fs header.
#define POUND_NCA_PARTITION_PFS0 0U
#define POUND_NCA_FS_TYPE_PFS0    0U

/// Section encryption types.
#define POUND_NCA_CRYPT_NONE 1U
#define POUND_NCA_CRYPT_XTS  2U
#define POUND_NCA_CRYPT_CTR  3U
#define POUND_NCA_CRYPT_BKTR 4U

/// One section's byte range, relative to the start of the NCA.
typedef struct
{
    uint64_t offset;
    uint64_t size;
} pound_nca_section_t;

/// The identifying fields of an NCA header, without opening its key area.
///
/// A package has to choose which NCA is the program before it can match that NCA's
/// rights id to a ticket, so it needs to read the header without already having a
/// title key. This is that view. It proves only that the header decrypted and is
/// structurally plausible; `pound_nca_open` still performs the full validation.
typedef struct
{
    uint8_t  content_type;
    uint8_t  distribution_type;
    uint8_t  crypto_type;
    uint8_t  crypto_type2;
    uint8_t  kaek_ind;
    uint8_t  rights_id[16];
    uint64_t program_id;
    uint64_t nca_size;
    bool     is_plaintext;
} pound_nca_info_t;

/// Reads and decrypts an NCA header, returning only its identifying fields.
error_t pound_nca_inspect(const fs_reader_t *POUND_RESTRICT source, uint64_t base, uint64_t size,
                          const key_store_t *POUND_RESTRICT keys,
                          pound_nca_info_t *POUND_RESTRICT info);

/// A parsed NCA header and decrypted key area.
///
/// The source reader is borrowed and must outlive this struct. `base` is the
/// NCA's offset in that reader, so an NCA inside an NSP can be addressed without
/// copying it out of the package.
typedef struct
{
    /// Borrowed parent reader. Never freed by this module.
    const fs_reader_t *source;

    /// Offset of the first byte of this NCA in `source`.
    uint64_t base;

    /// Declared NCA length, in bytes.
    uint64_t size;

    /// Decrypted header, kept for callers that need the program id or rights id.
    uint8_t header[POUND_NCA_HEADER_SIZE];

    /// Content type: 0 program, 1 meta, 2 control, 3 manual, 4 data, 5 public data.
    uint8_t content_type;

    /// Key generation field from the header.
    uint8_t crypto_type;

    /// Key-area encryption key index.
    uint8_t kaek_ind;

    /// Distribution type.
    uint8_t distribution_type;

    /// Program/title id from the header.
    uint64_t program_id;

    /// Rights id, or all zero for a gamecard-style NCA with no ticket.
    uint8_t rights_id[16];

    /// The four section ranges from the header.
    pound_nca_section_t sections[POUND_NCA_SECTION_COUNT];

    /// Raw fs headers, one per section, from the decrypted header.
    uint8_t fs_headers[POUND_NCA_SECTION_COUNT][POUND_NCA_SECTOR_SIZE];

    /// Decrypted key area: [0x00..0x1f] is the XTS key pair, [0x20..0x2f] the CTR key.
    uint8_t decrypted_keys[0x40];

    /// The common title key supplied for a rights-id NCA, or zero.
    uint8_t title_key[16];

    /// True when the header was already plaintext and no `header_key` was used.
    bool is_plaintext;

    /// True when a rights id was present and a title key was supplied.
    bool has_rights_id;

    /// True after a successful open.
    bool initialized;
} pound_nca_t;

/// Opens the NCA at `[base, base + size)` in `source`.
///
/// `title_key` must be the already-decrypted common title key for an NCA with a
/// nonzero rights id, and is ignored otherwise. It may be NULL for a gamecard-style
/// NCA with a zero rights id. A rights-id NCA without a title key is refused with
/// `POUND_ERROR_KEY_MISSING`, not opened and left to fail later.
///
/// Returns `POUND_ERROR_KEY_MISSING` when a needed key is absent,
/// `POUND_ERROR_KEY_SIZE` when the key is present but the wrong width,
/// `POUND_ERROR_MALFORMED_HEADER` for a bad or truncated NCA,
/// `POUND_ERROR_UNSUPPORTED_FEATURE` for NCA2, BKTR or another unsupported layout,
/// and `POUND_ERROR_IO` when the source cannot supply the bytes.
error_t pound_nca_open(pound_nca_t *POUND_RESTRICT nca, const fs_reader_t *POUND_RESTRICT source,
                       uint64_t base, uint64_t size, const key_store_t *POUND_RESTRICT keys,
                       const uint8_t title_key[16]);

/// Clears an NCA object. Safe on a zeroed object and safe to call twice.
void pound_nca_close(pound_nca_t *POUND_RESTRICT nca);

/// Reads and decrypts one section into a freshly allocated buffer.
///
/// `*out` is allocated with `malloc` and owned by the caller, who must `free` it.
/// On failure `*out` is NULL and `*out_size` is zero. Section `index` must be less
/// than `POUND_NCA_SECTION_COUNT`; an empty section is refused rather than returned
/// as a zero-byte success, because every caller here expects content.
error_t pound_nca_read_section(const pound_nca_t *POUND_RESTRICT nca, uint32_t index,
                               uint8_t **POUND_RESTRICT out, size_t *POUND_RESTRICT out_size);

/// Extracts the `main` NSO from the NCA's ExeFS section.
///
/// The ExeFS section is the PFS0 section whose fs header says partition type PFS0
/// and fs type PFS0. The returned buffer is a copy of the `main` file and is owned
/// by the caller. A program NCA with no ExeFS, no `main`, or a PFS0 that does not
/// fit its section is refused; this is not a RomFS extractor and does not pretend
/// to be one.
error_t pound_nca_extract_main(const pound_nca_t *POUND_RESTRICT nca, uint8_t **POUND_RESTRICT out,
                               size_t *POUND_RESTRICT out_size);

/// Logs the parts of a parsed header that identify the content.
void pound_nca_log_summary(const pound_nca_t *POUND_RESTRICT nca);

#ifdef __cplusplus
}
#endif

#endif // POUND_LOADER_PACKAGE_NCA_H

/*** end of file ***/

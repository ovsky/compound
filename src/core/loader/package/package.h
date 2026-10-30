#ifndef POUND_LOADER_PACKAGE_PACKAGE_H
#define POUND_LOADER_PACKAGE_PACKAGE_H

//! Loading the executable out of an NSP, XCI or standalone NCA.
//!
//! The result is one decrypted ExeFS `main` NSO in memory. That is deliberately a
//! narrow result: Compound maps and interprets executables, and this build does not
//! implement RomFS, control data, metadata or a title filesystem. A package that
//! has a program NCA and an ExeFS `main` can be loaded; everything else is refused
//! at the point where it would have to be approximated.
//!
//! ## Tickets
//!
//! An eShop NCA names a rights id. Its section key is the common title key from the
//! matching ticket, encrypted under `titlekek_NN`. The ticket is read from the same
//! NSP. If the ticket or the titlekek is absent, the package is refused with the
//! key store's error. Compound never supplies a title key, and does not fall back to
//! the key area of a rights-id NCA.
//!
//! ## Switch 2
//!
//! A package that decrypts with the user's keys and yields a standard NSO can be
//! loaded. That is not a claim that Switch 2 titles run: the NCA format, the key
//! generations and the executable container for Switch 2 are not all documented,
//! and this parser does not guess them. `is_switch2` reports only whether the
//! content identified itself as Switch 2 through a field this parser understands;
//! it is never set from a file extension.

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

/// Largest ticket this loader will read. A ticket is a few hundred bytes; the cap
/// keeps a corrupt PFS0 entry from asking for an arbitrary allocation.
#define POUND_PACKAGE_MAX_TICKET_SIZE 0x1000U

/// Largest number of tickets considered from one NSP.
#define POUND_PACKAGE_MAX_TICKETS 64U

/// The executable extracted from a package.
typedef struct
{
    /// Decrypted ExeFS `main` NSO, owned by this struct.
    uint8_t *main_data;

    /// Length of `main_data`, in bytes.
    size_t main_size;

    /// Program id from the chosen NCA.
    uint64_t program_id;

    /// True when the content came from an XCI's secure partition.
    bool from_gamecard;

    /// True when the content declared a Switch 2 marker this build recognises.
    bool is_switch2;
} pound_package_t;

/// Extracts the program `main` NSO from `reader`.
///
/// The reader must contain a PFS0 (NSP), an XCI, or one NCA. The source bytes must
/// outlive the call; the returned buffer does not refer to them.
///
/// Returns the key store's errors for missing or malformed keys, and
/// `POUND_ERROR_UNSUPPORTED_FORMAT` for a container this loader does not understand.
/// No output is returned on failure.
error_t pound_package_load(const fs_reader_t *POUND_RESTRICT reader, const key_store_t *POUND_RESTRICT keys,
                           pound_package_t *POUND_RESTRICT package);

/// Releases the extracted executable. Safe on a zeroed struct and safe twice.
void pound_package_close(pound_package_t *POUND_RESTRICT package);

/// Logs which container was loaded and what executable it produced.
void pound_package_log_summary(const pound_package_t *POUND_RESTRICT package);

#ifdef __cplusplus
}
#endif

#endif // POUND_LOADER_PACKAGE_PACKAGE_H

/*** end of file ***/

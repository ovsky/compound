#ifndef POUND_FS_KEYS_H
#define POUND_FS_KEYS_H

//! The key provider: the only place in Compound that is allowed to hold decryption key
//! material, and it holds nothing until the user supplies it.
//!
//! ## Why this file exists and is shaped this way
//!
//! Nintendo's console keys are secrets. They are not published by the vendor, they are
//! not derivable from anything in a game's public data, and no amount of work produces
//! them. They are also, technically, part of the DRM that a homebrew emulator has to
//! be careful about: the accepted practice in the emulator community -- and what every
//! reputable emulator does -- is that the *user* brings their own keys, extracted with
//! their own hardware and their own tools, and the emulator never distributes them.
//!
//! So there is no fallback, no default, and no hardcoded table anywhere below. If a
//! key is not in the user's file, `key_store_get` returns `POUND_ERROR_KEY_MISSING` and
//! the loader stops with a message saying which key is missing and how to provide it.
//! A zero-filled key would be worse than a refusal: it decrypts to noise, and noise
//! that looks like a header is how you get a mysterious crash three subsystems deep
//! instead of a clear message at load time.
//!
//! ## The file format
//!
//! Compound reads the same plain-text format the rest of the homebrew ecosystem uses, so a
//! user who already has a key file for another emulator does not have to convert it:
//!
//! ```text
//! # Lines beginning with a hash are comments.
//! master_key_00 = 00000000000000000000000000000000
//! header_key    = 00000000000000000000000000000000
//! ```
//!
//! One `name = hex` pair per line. Whitespace around the name, the separator and the
//! value is ignored, `#` and `;` start a comment, and blank lines are skipped. A
//! duplicate name replaces the earlier value rather than being an error, because a user
//! appending a corrected key to an existing file should not have to edit the old line.
//!
//! ## What is deliberately not here
//!
//! No AES, no key derivation, no keyblob parsing, no RSA, no certificate chain. Those
//! belong to the format parsers that consume a `uint8_t[POUND_KEY_SIZE]`, and a key
//! provider that also did the crypto would be one large module with one set of
//! permissions.

#include "attributes.h"
#include "errors.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Length of the AES-128 keys that make up most of a key file.
///
/// This remains the size accepted by the original fixed-width API. A small number of
/// keys -- most importantly `header_key`, which is two AES-128 keys used as one
/// AES-256 XTS key -- are 32 bytes, so the store carries a per-entry length rather
/// than assuming every key is one width.
#define POUND_KEY_SIZE 16U

/// Largest supported key, in bytes. An entry is never truncated to this size: a value
/// longer than the store can represent is refused.
#define POUND_KEY_MAX_SIZE 32U

/// Capacity of a key's name, including the terminating NUL.
///
/// Sized to the longest name in use rather than to something round, so that a
/// hardcoded name can never be silently truncated into a different key. `strlen` of the
/// longest defined name below is 38, so this has comfortable headroom while still
/// rejecting a pathological name at load time instead of storing it.
#define POUND_KEY_NAME_MAX 64U

/// Number of characters the longest key's value occupies in the text file: two hex
/// digits per byte. A shorter value is accepted only when its length names a supported
/// key size.
#define POUND_KEY_HEX_MAX (POUND_KEY_MAX_SIZE * 2U)

// --- Names of the keys Compound itself asks for by name -----------------------------
//
// Defined here so a caller cannot typo a string literal and silently get a lookup that
// is always missing, which would surface much later as an unrelated decryption failure.

/// AES-CTR key for an XCI or NSP header, derived by the console from a keyblob.
#define POUND_KEY_HEADER_KEY "header_key"

/// The same key under the name older key files use. Preferred over `header_key` when
/// both are present, because it is the name associated with keyblob-derived keys on
/// the most recent hardware.
#define POUND_KEY_XCI_HEADER_KEY "xci_header_key"

/// AES-CTR key for the second-level package inside a boot image.
#define POUND_KEY_PACKAGE2_KEY "package2_key_00"

/// Number of entries in the `master_key_NN` family.
///
/// A key file may contain more generations than Compound knows how to use. The bound
/// says which indices this build will ask for; an out-of-range request is a programming
/// error, not a cue to read an arbitrary name a user happened to define.
#define POUND_KEY_MASTER_KEY_COUNT 7U

/// Number of entries in the `key_area_key_application_NN` family.
#define POUND_KEY_APPLICATION_KEY_AREA_COUNT 32U

/// Number of entries in the `titlekek_NN` family, which is what titles on the newer
/// hardware use in place of the application key area keys.
#define POUND_KEY_TITLEKEK_COUNT 32U

/// One stored key.
typedef struct
{
    /// Name, NUL-terminated. Never empty.
    char name[POUND_KEY_NAME_MAX];

    /// The key itself, in the first `size` bytes. Bytes past `size` are always zero.
    uint8_t value[POUND_KEY_MAX_SIZE];

    /// Number of valid bytes in `value`: 16 or 32 for every key format this build reads.
    uint8_t size;

    /// Explicit tail padding. `key_entry_t` is copied as raw storage when the array
    /// grows, so every byte must have a defined value.
    uint8_t pad[7];
} key_entry_t;

/// A set of keys supplied by the user.
///
/// Entries are kept sorted by name so a lookup is a binary search, which also makes a
/// duplicate name detectable in a single comparison during insertion. The set is
/// deliberately small -- the well-known keys number a few dozen -- so a sorted array
/// beats a hash table on every axis that matters here: no allocation per entry, no
/// hash function to get subtly wrong, iteration order that is stable and therefore
/// reproducible in a log, and a structure that is trivial to free.
typedef struct
{
    key_entry_t *POUND_RESTRICT entries;
    size_t       count;
    size_t       capacity;
} key_store_t;

/// Puts `store` into the empty state. Does not allocate, so a store is usable
/// immediately and a `key_store_destroy` on a never-used store is safe.
void key_store_init(key_store_t *POUND_RESTRICT store);

/// Releases `store`'s array and returns it to the empty state.
///
/// Safe on a zeroed struct and safe to call twice. Does not clear the key bytes before
/// releasing them: the array is handed back to the allocator, which is outside Compound's
/// control, so a zeroing pass would be security theatre. A caller that genuinely needs
/// the memory gone -- and nothing in Compound does -- should let the store be destroyed
/// at exit and rely on the OS reclaiming it.
void key_store_destroy(key_store_t *POUND_RESTRICT store);

/// Inserts or replaces the key called `name`.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` if `store` is NULL, if `name` is NULL or
/// empty, or if it does not fit in `POUND_KEY_NAME_MAX` including the NUL -- a name that
/// does not fit is rejected rather than truncated, because a truncated name is a
/// different key and would look up successfully against the wrong entry.
/// `POUND_ERROR_ALLOCATION_FAILED` if the array cannot grow. On failure the store is
/// left exactly as it was.
error_t key_store_add(key_store_t *POUND_RESTRICT store, const char *POUND_RESTRICT name,
                      const uint8_t *POUND_RESTRICT value);

/// Inserts or replaces a key of an explicit length.
///
/// `size` must be 16 or 32. Those are the two widths the formats Compound reads use:
/// one AES-128 key, or a pair of AES-128 keys consumed as one AES-256 XTS key. Any
/// other length is refused with `POUND_ERROR_KEY_SIZE`; it is not rounded up, truncated,
/// or padded.
error_t key_store_add_sized(key_store_t *POUND_RESTRICT store, const char *POUND_RESTRICT name,
                            const uint8_t *POUND_RESTRICT value, size_t size);

/// Parses a key file's contents into `store`.
///
/// `text` need not be NUL-terminated, so a file read through an `fs_reader_t` can be
/// handed over directly with its length. Line endings are not special-cased: `\r` is
/// whitespace and is skipped along with `\n`, so a file authored on Windows and one
/// authored on Linux parse identically.
///
/// Entries already in `store` are replaced, and parsing continues past a malformed
/// line, collecting the first error and finishing the file. That is deliberate: a user
/// with a hundred keys and one typo should be told the line number of the typo and get
/// the other ninety-nine keys, not be presented with a single opaque failure. The
/// returned code is `POUND_ERROR_MALFORMED_HEADER` if any line was rejected, and
/// `POUND_SUCCESS` only if every non-blank, non-comment line parsed.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` if `store` is NULL, or if `text` is NULL with
/// a non-zero `size`.
error_t key_store_load_text(key_store_t *POUND_RESTRICT store, const char *POUND_RESTRICT text, const size_t size);

/// Looks up the key called `name` and writes it to `out`.
///
/// Returns `POUND_ERROR_KEY_MISSING` if the store does not contain `name`, which is the
/// single most important behaviour in this file: a missing key is reported, never
/// substituted. Returns `POUND_ERROR_INVALID_ARGUMENT` if `store` or `out` is NULL or
/// `name` is NULL or empty.
///
/// `out` is only written on success, so a caller that checks the return value and
/// ignores `out` on failure cannot accidentally proceed with stale bytes from a
/// previous lookup.
error_t key_store_get(const key_store_t *POUND_RESTRICT store, const char *POUND_RESTRICT name,
                      uint8_t out[POUND_KEY_SIZE]);

/// Looks up a key of an explicit length.
///
/// Missing and wrong-sized are different errors. The key store is not allowed to make
/// a 16-byte key satisfy a 32-byte request or to drop the second half of a 32-byte key:
/// either would produce a plausible-looking decryption failure later, far from the file
/// that caused it. A present key of the wrong length therefore reports
/// `POUND_ERROR_KEY_SIZE`, and a missing one reports `POUND_ERROR_KEY_MISSING`.
error_t key_store_get_sized(const key_store_t *POUND_RESTRICT store, const char *POUND_RESTRICT name,
                            uint8_t *POUND_RESTRICT out, size_t size);

/// Returns the stored length of `name` without copying its value.
error_t key_store_get_size(const key_store_t *POUND_RESTRICT store, const char *POUND_RESTRICT name, size_t *out_size);

/// Whether `name` is present, without copying the value out.
bool key_store_has(const key_store_t *POUND_RESTRICT store, const char *POUND_RESTRICT name);

/// Number of keys in the store.
size_t key_store_count(const key_store_t *POUND_RESTRICT store);

/// Writes the name of the key at `index` into `out`, for reporting which keys loaded.
///
/// `index` is an index into the sorted array, so the order is alphabetical and stable.
/// This exists so a caller can list the store without reaching into its layout, and
/// specifically so it can *count and name* keys without ever reading a value -- see
/// `key_store_log_summary`.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` if `out` is NULL, if `index` is past the
/// end, or if `out` is smaller than `POUND_KEY_NAME_MAX`.
error_t key_store_name_at(const key_store_t *POUND_RESTRICT store, const size_t index,
                          char out[POUND_KEY_NAME_MAX]);

/// Returns the `master_key_NN` key for `index`.
///
/// The name is assembled into a stack buffer rather than being stored in a static
/// table, so the set of families is described once in code instead of being duplicated
/// as both a macro list and a lookup. `index` must be less than
/// `POUND_KEY_MASTER_KEY_COUNT`; the bound is checked here and reported, because an
/// out-of-range index is a programming error and must not be turned into a lookup of
/// `master_key_09`, which a user's file could plausibly contain and which would then
/// silently be used in place of a real key.
error_t key_store_get_master_key(const key_store_t *POUND_RESTRICT store, const unsigned int index,
                                 uint8_t out[POUND_KEY_SIZE]);

/// Returns the `key_area_key_application_NN` key for `index`, which decrypts an NCA's
/// key area for title-locked application content.
///
/// `index` must be less than `POUND_KEY_APPLICATION_KEY_AREA_COUNT`.
error_t key_store_get_application_key_area(const key_store_t *POUND_RESTRICT store, const unsigned int index,
                                           uint8_t out[POUND_KEY_SIZE]);

/// Returns the `titlekek_NN` key for `index`.
///
/// Titles on the newer hardware derive their key-area key from one of these instead of
/// using the application key area keys directly, so a loader has to consult both and try
/// the one that matches. `index` must be less than `POUND_KEY_TITLEKEK_COUNT`.
error_t key_store_get_titlekek(const key_store_t *POUND_RESTRICT store, const unsigned int index,
                               uint8_t out[POUND_KEY_SIZE]);

/// Reports which keys are present, by name, and how many were loaded.
///
/// Logs names and counts only, never values. A key is a shared secret; a log file gets
/// attached to bug reports and pasted into forums, and a sixteen-byte key in a
/// screenshot is a key in a screenshot. Nothing in this file writes a `value` to a log,
/// and this is the one function whose entire job is to produce a log, so it is where
/// that rule is most likely to be broken by accident.
void key_store_log_summary(const key_store_t *POUND_RESTRICT store);

#endif // POUND_FS_KEYS_H

/*** end of file ***/

#ifndef POUND_FS_PFS0_H
#define POUND_FS_PFS0_H

//! PFS0, the partition file system that Switch and Switch 2 packages are built out of.
//!
//! ## Where this format appears
//!
//! PFS0 is not a container in its own right so much as the common internal currency of
//! the family's formats. An XCI holds one. An NSP is one. An NCA's first section is
//! one, holding the game's content files under their full paths. An NSO executable
//! inside a delta fragment is one. Every one of those is a flat, headerless file
//! system, so anything that needs to enumerate a game's files -- the loader, the
//! installer, the patcher -- goes through this parser first.
//!
//! ## The layout
//!
//! ```text
//!   offset 0                   Header, 16 bytes
//!                               magic          u32  "PFS0"
//!                               file_count     u32
//!                               string_table_size u32
//!                               reserved       u32
//!
//!   offset 16                  File table, file_count entries of 24 bytes
//!                               offset        u64  relative to the start of the data
//!                               size          u64
//!                               string_offset u32  relative to the start of the string
//!                                                     table, after that table's own
//!                                                     4-byte magic
//!                               hashed_size   u32  zero in every file ever produced
//!
//!   offset 16 + count * 24     String table, string_table_size bytes
//!                               magic         u32  "PFS0"
//!                               names         NUL-terminated, unaligned, concatenated
//!
//!   ...after the string table  File data. Entry offsets are relative to this point.
//! ```
//!
//! ## The design decisions that matter
//!
//! **It does not read the file.** A PFS0 can be sixty gigabytes and a few hundred bytes
//! long, and the entries say where the data is without it being anywhere near. Opening
//! one reads the header, the file table and the string table -- a few hundred kilobytes
//! at the very worst -- and leaves every file's contents on disk. Reading an entry's
//! bytes is a separate, explicit call, and a caller that wants one file does not pay
//! for the other fifteen thousand. Anything else would mean a sixty-gigabyte allocation
//! before the loader could look at a single filename.
//!
//! **It validates the whole table at open, not lazily.** A lazy parser that checked an
//! entry only when it was used would hand out an entry whose name ran off the end of
//! the string table, and the fault would then appear in whatever subsystem was unlucky
//! enough to read that name. Validating up front means `pfs0_open` either returns a
//! table in which every offset is known good, or returns an error naming what was
//! wrong. The cost is one pass over the file table, which for the largest plausible
//! partition is a few hundred kilobytes of sequential reads.
//!
//! **Entries are checked for overlap and for duplicate names.** Neither is possible in
//! a file produced by Nintendo's tools, so both indicate that the image is not what it
//! claims to be. A duplicate name in particular would make a lookup by name ambiguous,
//! and resolving that ambiguity by picking the first match is exactly the kind of silent
//! choice that loads the wrong file and fails much later.
//!
//! ## What this parser does not do
//!
//! It does not decrypt. A PFS0 inside an NCA is encrypted with a key the user supplies
//! and the NCA parser is responsible for arranging; what reaches this parser is
//! plaintext. It also does not know about hierarchies: a PFS0 has no directories, and
//! the paths inside an NCA's section 0 are stored as single flat names containing
//! forward slashes. Splitting them into a tree is the loader's job, not the format's.

#include "attributes.h"
#include "crypto/sha256.h"
#include "errors.h"
#include "fs/fs_reader.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// The format's magic, as it appears in the header and again at the start of the string
/// table. Stored as a number so it can be compared without a string call on a possibly
/// unaligned four bytes, and as the characters so error messages can print what was
/// found.
#define PFS0_MAGIC          0x30534650U
#define PFS0_MAGIC_TEXT     "PFS0"

/// Size of the fixed header, in bytes.
#define PFS0_HEADER_SIZE 16U

/// Size of one file table entry, in bytes.
#define PFS0_ENTRY_SIZE 24U

/// Size of the string table's own magic, which every name's offset is measured from.
#define PFS0_STRING_TABLE_MAGIC_SIZE 4U

/// Capacity of an entry's name, including the terminating NUL.
///
/// A name inside an NCA is a full path such as
/// `Nintendo Switch Product/Some Title/Some Content.nca`, which is comfortably inside
/// this. A name that does not fit is rejected at open with a log naming the entry,
/// because truncating it would collide with a real path and silently return the wrong
/// file's contents.
#define PFS0_NAME_MAX 256U

/// Largest number of files a partition may declare.
///
/// A cap as well as a bound, and a generous one: the largest PFS0 in normal use holds
/// a few thousand entries. Without a cap, a corrupt or hostile header claiming four
/// billion files would ask for a ninety-six-gigabyte allocation before the reader
/// discovered the file is not that big. Checking the count against the reader's size as
/// well as against this constant means the allocation is only attempted once the file
/// has been shown to actually contain the bytes being claimed.
#define PFS0_MAX_FILES 0x4000U

/// Largest string table a partition may declare, in bytes.
///
/// The same reasoning as `PFS0_MAX_FILES`, for the table of names. Real string tables
/// are a few kilobytes; a megabyte is already thousands of times that.
#define PFS0_MAX_STRING_TABLE_SIZE (1U << 20)

/// One file inside a partition.
typedef struct
{
    /// Where the file's bytes start, as an absolute offset into the reader the
    /// partition was opened from. The format stores this relative to the start of the
    /// data region; the base has already been folded in, so a caller can hand this
    /// straight to `fs_reader_read_at` without knowing anything about the layout.
    uint64_t offset;

    /// Length of the file in bytes.
    uint64_t size;

    /// The file's name, as stored: a flat string that may contain forward slashes.
    ///
    /// NUL-terminated by `pfs0_open`, which guarantees it is at least that long and was
    /// terminated within the string table in the first place.
    char name[PFS0_NAME_MAX];
} pfs0_entry_t;

/// An opened partition.
typedef struct
{
    /// A copy of the reader the partition was opened from.
    ///
    /// Stored by value so a caller cannot destroy the reader it opened the partition
    /// from and leave the partition holding a dangling pointer, and so that
    /// `pfs0_read` needs nothing but the partition. The copy refers to the same bytes;
    /// those must outlive the partition, which is the reader's own contract.
    fs_reader_t reader;

    /// The file table, in the order the file stored it.
    ///
    /// Allocated by `pfs0_open` and released by `pfs0_close`. Sorted by name would make
    /// lookups cheaper, but the stored order is the order the entries' offsets
    /// generally follow, so keeping it makes enumeration read the partition the way it
    /// was written. With a few thousand entries and a handful of lookups per load, the
    /// linear scan in `pfs0_find` is not the thing worth optimising here.
    pfs0_entry_t *POUND_RESTRICT entries;

    /// Absolute offset of the first byte of file data, for callers that need to slice
    /// the partition themselves.
    uint64_t data_offset;

    /// Total number of bytes of file data. Every entry is known to lie within
    /// `[data_offset, data_offset + data_size)`, and this is what that interval's
    /// length is -- the reader may extend past it with padding, so this is not
    /// necessarily the reader's size.
    uint64_t data_size;

    /// Absolute offset of the string table's magic, for diagnostics.
    uint64_t string_table_offset;

    /// Number of entries in `entries`.
    uint32_t count;

    /// Size of the string table as declared, including its own four-byte magic.
    uint32_t string_table_size;
} pfs0_t;

/// Opens the partition that starts at the beginning of `reader`.
///
/// On success every entry in the resulting table has a NUL-terminated name that fits
/// `PFS0_NAME_MAX`, and a byte range that lies entirely inside the partition's data
/// region, and no two entries share a name. A caller can therefore use the table
/// without re-validating it.
///
/// Returns:
///   * `POUND_ERROR_INVALID_ARGUMENT` if either argument is NULL.
///   * `POUND_ERROR_MALFORMED_HEADER` if the magic is wrong, a declared table runs
///     past the end of the source, the file count or string table size exceeds what a
///     real partition could hold, a name is not terminated within the string table or
///     is too long, an entry's byte range leaves the data region, or two entries share
///     a name. Every one of these logs which entry and what was wrong.
///   * `POUND_ERROR_IO` if the source could not supply a range the header declared.
///   * `POUND_ERROR_ALLOCATION_FAILED` if the tables could not be allocated.
///
/// On failure the partition is left closed, so `pfs0_close` is safe to call on it
/// whether or not this succeeded.
error_t pfs0_open(pfs0_t *POUND_RESTRICT pfs0, const fs_reader_t *POUND_RESTRICT reader);

/// Releases the partition's tables and returns it to the closed state.
///
/// Safe on a zeroed struct and safe to call twice. The underlying reader is not owned
/// and is not touched.
void pfs0_close(pfs0_t *POUND_RESTRICT pfs0);

/// Whether the partition is open and holds at least one file.
bool pfs0_is_open(const pfs0_t *POUND_RESTRICT pfs0);

/// Number of files in the partition, or zero if it is not open.
uint32_t pfs0_count(const pfs0_t *POUND_RESTRICT pfs0);

/// Returns a pointer to entry `index`, or NULL if the partition is closed or `index` is
/// out of range.
///
/// The pointer is owned by the partition and is invalidated by `pfs0_close`. Returning
/// a pointer rather than copying a 272-byte entry out by value keeps enumeration free,
/// and is valid only until `pfs0_close`. It is for inspection -- a caller reading
/// `name`, `size` and `offset` out of it -- and deliberately cannot be handed back to
/// `pfs0_read` or `pfs0_hash`, which take an index. See the note on indexing below.
const pfs0_entry_t *pfs0_entry_at(const pfs0_t *POUND_RESTRICT pfs0, const uint32_t index);

/// Returns the index of the file called `name`.
///
/// This is the only way to name a file for a later read, and it exists so that reads are
/// addressed by index rather than by pointer.
///
/// The reason is a lifetime problem a pointer-based interface cannot solve. A caller can
/// hold a `pfs0_entry_t *` across a `pfs0_close`, and the allocator is then free to hand
/// the same address back for the next partition. Passing that pointer to a read would be
/// either a use-after-free or, worse, a read from the wrong file that returns plausible
/// data. An index cannot go stale that way: it is validated against the current table on
/// every use, so a stale one is either out of range, which is an error, or it now names a
/// different file, which is visible rather than silent.
///
/// Returns `POUND_ERROR_NOT_FOUND` if no entry has that name, and
/// `POUND_ERROR_INVALID_ARGUMENT` if an argument is NULL. A miss is an expected
/// outcome -- the loader probes for optional files -- so it does not log, while the open
/// path does log its failures.
error_t pfs0_index_of(const pfs0_t *POUND_RESTRICT pfs0, const char *POUND_RESTRICT name,
                      uint32_t *POUND_RESTRICT out_index);

/// Whether a file called `name` is present.
bool pfs0_contains(const pfs0_t *POUND_RESTRICT pfs0, const char *POUND_RESTRICT name);

/// Reads `size` bytes from `offset_within_file` inside the file at `index`.
///
/// The range is re-checked here against the file's own bounds rather than trusted from
/// the open, so a caller that adds its own arithmetic to an offset gets a logged error
/// rather than a read outside the file.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` if an argument is NULL, `POUND_ERROR_NOT_FOUND`
/// if `index` does not name a file in this partition, and `POUND_ERROR_IO` if the range
/// would read past the end of the file or of the underlying source.
error_t pfs0_read(const pfs0_t *POUND_RESTRICT pfs0, const uint32_t index,
                  const uint64_t offset_within_file, void *POUND_RESTRICT destination, const size_t size);

/// Streams the file at `index` through SHA-256 and writes the digest to `out`.
///
/// This is the format's only integrity check, and it is worth doing before handing a
/// file to a parser that will act on it. A file may legitimately be tens of megabytes,
/// so the bytes are read in chunks through a temporary buffer rather than being loaded
/// whole; the buffer is released before returning on every path.
///
/// Returns the errors `pfs0_read` returns, plus `POUND_ERROR_ALLOCATION_FAILED` if the
/// chunk buffer could not be allocated.
error_t pfs0_hash(const pfs0_t *POUND_RESTRICT pfs0, const uint32_t index, uint8_t out[SHA256_DIGEST_SIZE]);

/// Computes the file at `index`'s SHA-256 and compares it to `expected`.
///
/// Returns `POUND_ERROR_HASH_MISMATCH` if they differ, having logged both digests so a
/// user can see how far off the file is. A truncated or partially written image is the
/// usual cause, and "the file is truncated" and "the file has been modified" need
/// different responses, so the two hex forms are both in the message.
error_t pfs0_verify_hash(const pfs0_t *POUND_RESTRICT pfs0, const uint32_t index,
                         const uint8_t expected[SHA256_DIGEST_SIZE]);

/// Logs a summary of the partition: how many files, how many bytes of data, and the
/// first few file names.
///
/// Only the first few names, because a partition with fifteen thousand files would
/// otherwise bury everything else in the log, and because a name list is long enough to
/// make a log file unwieldy without telling a user anything a count would not.
void pfs0_log_summary(const pfs0_t *POUND_RESTRICT pfs0);

#endif // POUND_FS_PFS0_H

/*** end of file ***/

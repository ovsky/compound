#ifndef POUND_FS_HFS0_H
#define POUND_FS_HFS0_H

//! HFS0, the hashed partition file system that sits at the top of an XCI and inside every
//! update NCA.
//!
//! ## Where this format appears
//!
//! HFS0 is one layer in from the outside. An XCI's payload is one HFS0 whose entries are
//! the encrypted NCAs. An update NCA's section 0 is one HFS0 whose entries are the
//! content files and the meta files. Nothing else in the family uses it, which is the
//! opposite of PFS0 -- see the note on the relationship between the two below.
//!
//! ## The layout
//!
//! ```text
//!   offset 0                   Header, 16 bytes
//!                               magic            u32  "HFS0"
//!                               file_count       u32
//!                               string_table_size u32
//!                               reserved         u32
//!
//!   offset 16                  File table, file_count entries of 64 bytes
//!                               offset         u64  relative to the start of the data
//!                               size           u64
//!                               string_offset  u64  relative to the start of the string
//!                                                       table itself
//!                               hashed_size    u64  see "The hashed region" below
//!                               reserved[4]    u64
//!
//!   offset 16 + count * 64     String table, string_table_size bytes
//!                               names           NUL-terminated, concatenated
//!
//!   ...after the string table  File data. Entry offsets are relative to this point.
//! ```
//!
//! The metadata region is everything before the data: `16 + file_count * 64 +
//! string_table_size` bytes. That is the region the format's integrity check covers, and
//! it is the only part of a partition that is ever read into memory.
//!
//! ## The relationship to PFS0
//!
//! HFS0 and PFS0 are close relatives and the resemblance is misleading in three specific
//! ways, all of which are places a parser written for one and pointed at the other will
//! read nonsense without erroring:
//!
//! 1. **HFS0's string offsets are relative to the start of the string table. PFS0's are
//!    relative to four bytes past it**, past a repeated magic that PFS0's string table
//!    carries and HFS0's does not. Feeding an HFS0's offsets to PFS0's rule skips four
//!    bytes into every name.
//! 2. **HFS0's entries are 64 bytes, PFS0's 24.** Reading one with the other's stride
//!    produces a table of plausible-looking garbage.
//! 3. **HFS0 records a hash; PFS0 does not.** The `hashed_size` field has no counterpart
//!    in PFS0, and in an update NCA it is the region the NCA's own per-file hash covers
//!    -- which is not the metadata region, and not the file's data either. See below.
//!
//! The two are therefore separate modules rather than one parameterised parser. The
//! overlap is real but bounded: both read a table, both check it fully at open, and the
//! checks are the same checks. A shared implementation would have to carry a switch
//! through every bounds check to express a difference that only shows up in three places,
//! and the cost of a shared bug in that arithmetic is a wrong file read rather than a
//! clean failure.
//!
//! ## The design decisions that matter
//!
//! **It does not read the file.** Same reasoning as PFS0: an update NCA's section 0 can
//! be tens of gigabytes and a few hundred bytes of table. Opening one reads the header,
//! the file table and the string table, and leaves the contents on disk. `hfs0_read` is a
//! separate, explicit call.
//!
//! **It validates the whole table at open, not lazily.** Every entry's byte range is
//! checked against the data region, every name is checked to be terminated inside the
//! string table and to fit `HFS0_NAME_MAX`, and no two entries may share a name -- all
//! before the table is handed out. A lazy parser would hand out an entry whose name runs
//! off the end of the string table, and the fault would surface in whatever subsystem was
//! unlucky enough to read it. The cost is one pass over a table of at most a few hundred
//! kilobytes.
//!
//! **The metadata hash is verified, but the per-file `hashed_size` is only recorded.**
//! These are different things and conflating them is a real error. The metadata region's
//! hash is this format's own integrity check, and `hfs0_verify_metadata` performs it.
//! An entry's `hashed_size` is the size of the region an *enclosing* format hashes when
//! it hashes that file -- for an NCA's content files it is the NCA header plus the section
//! header, not this partition's metadata and not the file's contents. NCA's verifier
//! needs it, so it is exposed; nothing here uses it, because a PFS0 with a `hashed_size`
//! field would have exactly the same field and no use for it either.
//!
//! ## What this parser does not do
//!
//! It does not decrypt. Every HFS0 Pound will meet is inside encrypted storage -- an XCI's
//! entries are encrypted NCAs, and an update NCA's section 0 is a section whose
//! encryption was arranged by the NCA parser. What reaches this parser is plaintext. It
//! also does not know about hierarchies: names are flat strings that may contain forward
//! slashes, and splitting them into a tree is the loader's job.

#include "attributes.h"
#include "crypto/sha256.h"
#include "errors.h"
#include "fs/fs_reader.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// The format's magic, as it appears in the header. Stored as a number so it can be
/// compared without a string call on four bytes that came from a file and may not be
/// aligned, and as the characters so error messages can name both what was expected and
/// what was found.
#define HFS0_MAGIC          0x30534648U
#define HFS0_MAGIC_TEXT     "HFS0"

/// Size of the fixed header, in bytes.
#define HFS0_HEADER_SIZE 16U

/// Size of one file table entry, in bytes.
///
/// Sixty-four, against PFS0's twenty-four. Not a scaling of the same thing: the extra
/// space is `hashed_size` plus reserved words, and an entry read at the wrong stride is a
/// table of plausible garbage rather than a clean failure.
#define HFS0_ENTRY_SIZE 64U

/// Offset of the metadata region's size, within a file's data, when hashed by an
/// enclosing format. Not a field of this format; see `hashed_size` below.
#define HFS0_ENTRY_HASHED_SIZE_OFFSET 0x18U

/// Offset of a file's name offset, within its table entry.
#define HFS0_ENTRY_STRING_OFFSET_OFFSET 0x10U

/// Offset of a file's data offset, within its table entry.
#define HFS0_ENTRY_OFFSET_OFFSET 0x00U

/// Offset of a file's size, within its table entry.
#define HFS0_ENTRY_SIZE_OFFSET 0x08U

/// Capacity of an entry's name, including the terminating NUL.
///
/// A name in an XCI's HFS0 is a path such as `/hfs0/0`; a name in an update NCA's section
/// 0 is a full content path such as
/// `Nintendo Switch Product/Some Title/Some Content.nca`. Both are well inside this. A
/// longer name is rejected at open with a log naming the entry, because truncating it
/// would collide with a real path and silently return the wrong file's bytes.
#define HFS0_NAME_MAX 256U

/// Largest number of files a partition may declare.
///
/// A cap as well as a bound, and a generous one: the largest HFS0 in normal use -- an
/// update NCA's section 0 -- holds a few hundred entries. Without a cap, a corrupt or
/// hostile header claiming four billion files would ask for a 256-gigabyte allocation
/// before the reader had established that the file is that big.
#define HFS0_MAX_FILES 0x1000U

/// Largest string table a partition may declare, in bytes.
///
/// The same reasoning as `HFS0_MAX_FILES`, for the table of names. Real string tables are
/// a few kilobytes; a megabyte is already thousands of times that.
#define HFS0_MAX_STRING_TABLE_SIZE (1U << 20)

/// Smallest possible metadata region, in bytes: the header and a string table's worth of
/// nothing. A partition declaring fewer bytes than this cannot contain even its own
/// header, and refusing it before the file table is sized turns a wrap into an error.
#define HFS0_MIN_METADATA_SIZE (HFS0_HEADER_SIZE + HFS0_ENTRY_SIZE)

/// One file inside a partition.
typedef struct
{
    /// Where the file's bytes start, as an absolute offset into the reader the partition
    /// was opened from. The format stores this relative to the start of the data region;
    /// the base has already been folded in, so a caller can hand this straight to
    /// `fs_reader_read_at` without knowing anything about the layout.
    uint64_t offset;

    /// Length of the file in bytes.
    uint64_t size;

    /// The file's name, as stored: a flat string that may contain forward slashes, and
    /// for an XCI's HFS0 usually begins with one.
    ///
    /// NUL-terminated by `hfs0_open`, which guarantees it is at least that long and was
    /// terminated within the string table in the first place.
    char name[HFS0_NAME_MAX];
} hfs0_entry_t;

/// An opened partition.
typedef struct
{
    /// A copy of the reader the partition was opened from.
    ///
    /// Stored by value so a caller cannot destroy the reader it opened the partition from
    /// and leave the partition holding a dangling pointer. The copy refers to the same
    /// bytes; those must outlive the partition, which is the reader's own contract.
    fs_reader_t reader;

    /// The file table, in the order the file stored it.
    ///
    /// Allocated by `hfs0_open` and released by `hfs0_close`. The stored order is kept
    /// because the entries' offsets generally follow it, so enumeration reads the
    /// partition the way it was written. With a few hundred entries and a handful of
    /// lookups per load, the linear scan in `hfs0_index_of` is not the thing worth
    /// optimising.
    hfs0_entry_t *POUND_RESTRICT entries;

    /// Per-file `hashed_size` values, parallel to `entries`, in the order the file stored
    /// them.
    ///
    /// A separate array rather than a field in `hfs0_entry_t` because the value belongs
    /// to the enclosing format's hashing scheme rather than to anything this parser does
    /// with the file, and the note in the module comment says so at length. Keeping it
    /// out of the entry also keeps `hfs0_entry_t` the shape a reader of the module
    /// documentation would expect: where the file is, how long it is, and what it is
    /// called.
    uint64_t *POUND_RESTRICT hashed_sizes;

    /// Length of the metadata region: the header, the file table and the string table.
    ///
    /// This is what `hfs0_hash_metadata` covers, and the region whose length an enclosing
    /// format that stores a hash of the HFS0 will have recorded separately.
    uint64_t metadata_size;

    /// Absolute offset of the first byte of the string table.
    uint64_t string_table_offset;

    /// Absolute offset of the first byte of file data, for callers that need to slice the
    /// partition themselves.
    uint64_t data_offset;

    /// Total number of bytes of file data. Every entry is known to lie within
    /// `[data_offset, data_offset + data_size)`, and this is what that interval's length
    /// is -- the reader may extend past it with padding, so this is not necessarily the
    /// reader's size.
    uint64_t data_size;

    /// Number of entries in `entries`.
    uint32_t count;

    /// Size of the string table as declared.
    uint32_t string_table_size;
} hfs0_t;

/// Opens the partition that starts at the beginning of `reader`.
///
/// On success every entry in the resulting table has a NUL-terminated name that fits
/// `HFS0_NAME_MAX`, a byte range that lies entirely inside the partition's data region,
/// and a name no other entry shares. A caller can therefore use the table without
/// re-validating it.
///
/// Returns:
///   * `POUND_ERROR_INVALID_ARGUMENT` if either argument is NULL, or the reader has no
///     read function.
///   * `POUND_ERROR_MALFORMED_HEADER` if the magic is wrong, a declared table runs past
///     the end of the source, the file count or string table size exceeds what a real
///     partition could hold, the metadata region's size would wrap, a name is not
///     terminated within the string table or is too long, an entry's byte range leaves
///     the data region, or two entries share a name. Every one of these logs which entry
///     and what was wrong.
///   * `POUND_ERROR_IO` if the source could not supply a range the header declared.
///   * `POUND_ERROR_ALLOCATION_FAILED` if the tables could not be allocated.
///
/// On failure the partition is left closed, so `hfs0_close` is safe to call on it whether
/// or not this succeeded.
error_t hfs0_open(hfs0_t *POUND_RESTRICT hfs0, const fs_reader_t *POUND_RESTRICT reader);

/// Releases the partition's tables and returns it to the closed state.
///
/// Safe on a zeroed struct and safe to call twice. The underlying reader is not owned and
/// is not touched.
void hfs0_close(hfs0_t *POUND_RESTRICT hfs0);

/// Whether the partition is open and holds at least one file.
bool hfs0_is_open(const hfs0_t *POUND_RESTRICT hfs0);

/// Number of files in the partition, or zero if it is not open.
uint32_t hfs0_count(const hfs0_t *POUND_RESTRICT hfs0);

/// Returns a pointer to entry `index`, or NULL if the partition is closed or `index` is
/// out of range.
///
/// The pointer is owned by the partition and is invalidated by `hfs0_close`. It is for
/// inspection -- reading `name`, `size` and `offset` out of it -- and deliberately cannot
/// be handed back to `hfs0_read`, which takes an index. See the note on indexing in
/// `hfs0_index_of`.
const hfs0_entry_t *hfs0_entry_at(const hfs0_t *POUND_RESTRICT hfs0, const uint32_t index);

/// Returns the `hashed_size` recorded for entry `index`, and whether it was found.
///
/// The result is a separate output rather than a sentinel in the value, because zero is a
/// legitimate `hashed_size` and a partition can legitimately contain no files, so neither
/// can stand in for "not there".
bool hfs0_hashed_size_at(const hfs0_t *POUND_RESTRICT hfs0, const uint32_t index, uint64_t *POUND_RESTRICT out);

/// Returns the index of the file called `name`.
///
/// This is the only way to name a file for a later read, and it exists so that reads are
/// addressed by index rather than by pointer.
///
/// The reason is a lifetime problem a pointer-based interface cannot solve. A caller can
/// hold an `hfs0_entry_t *` across an `hfs0_close`, and the allocator is then free to
/// hand the same address back for the next partition. Passing that pointer to a read would
/// be either a use-after-free or, worse, a read from the wrong file that returns plausible
/// data. An index cannot go stale that way: it is validated against the current table on
/// every use, so a stale one is either out of range, which is an error, or it now names a
/// different file, which is visible rather than silent.
///
/// Returns `POUND_ERROR_NOT_FOUND` if no entry has that name, and
/// `POUND_ERROR_INVALID_ARGUMENT` if an argument is NULL. A miss is an expected outcome --
/// the loader probes for optional files -- so it does not log, while the open path does
/// log its failures.
error_t hfs0_index_of(const hfs0_t *POUND_RESTRICT hfs0, const char *POUND_RESTRICT name,
                      uint32_t *POUND_RESTRICT out_index);

/// Whether a file called `name` is present.
bool hfs0_contains(const hfs0_t *POUND_RESTRICT hfs0, const char *POUND_RESTRICT name);

/// Reads `size` bytes from `offset_within_file` inside the file at `index`.
///
/// The range is re-checked here against the file's own bounds rather than trusted from the
/// open, so a caller that adds its own arithmetic to an offset gets a logged error rather
/// than a read outside the file.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` if an argument is NULL, `POUND_ERROR_NOT_FOUND`
/// if `index` does not name a file in this partition, and `POUND_ERROR_IO` if the range
/// would read past the end of the file or of the underlying source.
error_t hfs0_read(const hfs0_t *POUND_RESTRICT hfs0, const uint32_t index,
                  const uint64_t offset_within_file, void *POUND_RESTRICT destination, const size_t size);

/// Streams the file at `index` through SHA-256 and writes the digest to `out`.
///
/// A file may legitimately be tens of megabytes, so the bytes are read in chunks through a
/// temporary buffer rather than being loaded whole; the buffer is released before
/// returning on every path.
///
/// Returns the errors `hfs0_read` returns, plus `POUND_ERROR_ALLOCATION_FAILED` if the
/// chunk buffer could not be allocated.
error_t hfs0_hash(const hfs0_t *POUND_RESTRICT hfs0, const uint32_t index, uint8_t out[SHA256_DIGEST_SIZE]);

/// Computes SHA-256 over the partition's metadata region and writes the digest to `out`.
///
/// The region is `[0, metadata_size)` -- the header, the file table and the string table
/// -- and is the format's own integrity check. An XCI records the expected digest of its
/// HFS0's metadata region in its own header, which is what `xci_verify_hfs0` compares
/// against this.
///
/// Returns `POUND_ERROR_NOT_INITIALIZED` if the partition is not open, and
/// `POUND_ERROR_ALLOCATION_FAILED` if the chunk buffer could not be allocated.
error_t hfs0_hash_metadata(const hfs0_t *POUND_RESTRICT hfs0, uint8_t out[SHA256_DIGEST_SIZE]);

/// Computes the metadata region's SHA-256 and compares it to `expected`.
///
/// Returns `POUND_ERROR_HASH_MISMATCH` if they differ, having logged both digests so a
/// user can see how far off the image is. "The image is truncated" and "the image has been
/// modified" need different responses, so both hex forms are in the message.
error_t hfs0_verify_metadata(const hfs0_t *POUND_RESTRICT hfs0, const uint8_t expected[SHA256_DIGEST_SIZE]);

/// A reader over one entry's bytes, sharing the partition's underlying source.
///
/// This is what lets a nested parser be handed an HFS0's entry without that entry being
/// read into memory: an XCI's HFS0 holds NCAs that are tens of gigabytes, and the NCA
/// parser wants a reader rather than a buffer.
///
/// ## Why the context is allocated and owned here rather than borrowed
///
/// `fs_reader_t` is a plain struct with no destructor, and a reader over a sub-range needs
/// somewhere to remember the parent reader and the base offset. The obvious move -- point
/// `context` at the parent partition and stash the offset somewhere -- does not work,
/// because `fs_reader_t` has no field to put the offset in, and adding one would change
/// the type every parser in the project depends on.
///
/// The alternative that does work is to hide the offset inside a closure, which means
/// allocating. So the ownership is made explicit instead of implicit: the caller owns the
/// `hfs0_slice_t`, must call `hfs0_slice_close`, and passes `&slice->reader` to the next
/// parser. A slice opened and never closed leaks exactly one small block; one that is
/// closed and then used is caught by the use-after-free that follows, rather than by a
/// silent read from a recycled allocation returning plausible bytes.
///
/// The allocation is one block per nesting level, and the chain is at most four deep
/// (XCI -> HFS0 -> NCA -> PFS0), so this is not a performance concern.
typedef struct
{
    /// The reader to hand to the next parser. Treat every other field as owned by the
    /// slice and read through this.
    fs_reader_t reader;

    /// The state `reader.context` points at, or NULL if the slice was never opened.
    ///
    /// Kept here so `hfs0_slice_close` has something to free that is not the caller's own
    /// struct, and so a caller that memsets the slice cannot lose track of an allocation.
    void *context;
} hfs0_slice_t;

/// Opens entry `index` as a reader over the partition's source, without reading it.
///
/// The returned reader's `size` is the entry's size and its offsets are relative to the
/// entry's first byte, so a nested parser sees its file starting at zero.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` if an argument is NULL, `POUND_ERROR_NOT_FOUND`
/// if `index` does not name a file in this partition, and `POUND_ERROR_ALLOCATION_FAILED`
/// if the slice's context could not be allocated.
///
/// On failure the slice is left closed, so `hfs0_slice_close` is safe to call on it
/// whether or not this succeeded.
error_t hfs0_entry_reader(const hfs0_t *POUND_RESTRICT hfs0, const uint32_t index,
                          hfs0_slice_t *POUND_RESTRICT out);

/// Releases the slice's context and returns it to the closed state.
///
/// Safe on a zeroed struct and safe to call twice. Does nothing to the bytes the reader
/// referred to -- it never owned them.
void hfs0_slice_close(hfs0_slice_t *POUND_RESTRICT slice);

/// Logs a summary of the partition: how many files, how many bytes of data, how large the
/// metadata region, and the first few file names.
///
/// Only the first few names, because a partition with a few hundred files would otherwise
/// bury everything else in the log, and because a name list is long enough to make a log
/// file unwieldy without telling a user anything a count would not.
void hfs0_log_summary(const hfs0_t *POUND_RESTRICT hfs0);

#endif // POUND_FS_HFS0_H

/*** end of file ***/

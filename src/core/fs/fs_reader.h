#ifndef POUND_FS_FS_READER_H
#define POUND_FS_FS_READER_H

//! A random-access byte source, and the only way container parsers see file data.
//!
//! Every format Pound reads -- XCI, NCA, PFS0, HFS0 -- is a header followed by large
//! opaque regions that must *not* be read into memory. A Switch 2 game is tens of
//! gigabytes; the executable is tens of megabytes; the PFS0 string table inside an NCA
//! can sit forty gigabytes into the image. A parser that took a `uint8_t *` covering
//! the whole file would be unusable on a real game, so the parsers here take a
//! `fs_reader_t` and ask for the few hundred bytes they actually need.
//!
//! The indirection also keeps the parsers testable. A test builds a `fs_reader_t` over
//! a stack buffer and gets byte-for-byte the same code path a real file takes, so a
//! parser cannot pass its tests against a hand-rolled mock and then fail on disk.
//!
//! ## Offset arithmetic
//!
//! Offsets are `uint64_t` throughout and are *not* validated by the reader beyond
//! checking they lie inside the source. A parser is responsible for its own bounds:
//! adding an entry's offset to its size, or walking a partition chain, is arithmetic
//! the parser must do against values it has already range-checked, because only it
//! knows what a legitimate layout looks like. The one thing the reader does guarantee
//! is that a read which runs past the end of the source fails with
//! `POUND_ERROR_IO` rather than returning short.

#include "attributes.h"
#include "errors.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// A random-access source of bytes.
///
/// A plain struct rather than an opaque handle so a caller can put one on the stack and
/// so the buffer-backed implementation below can be a literal with no allocation. The
/// cost is that the layout is part of the interface, which is acceptable for a type
/// this small and this central.
typedef struct
{
    /// Opaque state handed back to `read_at`. The buffer-backed reader ignores it in
    /// favour of the two `buffer_` fields below.
    void *context;

    /// Total number of bytes the source can produce. Every offset a parser derives
    /// must be checked against this before it is used.
    uint64_t size;

    /// Reads exactly `size` bytes from `offset` into `destination`.
    ///
    /// A short read is an error, not a partial success. Every caller in Pound wants a
    /// whole structure, and a parser handed a half-filled one would go on to interpret
    /// whatever happened to be in the untouched bytes. So a read that cannot be
    /// completed in full returns `POUND_ERROR_IO` and leaves the caller's buffer in an
    /// unspecified state -- which every caller must treat as unusable rather than read.
    ///
    /// `read_at` returns `POUND_SUCCESS` on a complete read, or a specific non-success
    /// code. A parser that cannot proceed propagates it unchanged; that is what keeps a
    /// truncated image from being reported as a format error.
    error_t (*read_at)(void *POUND_RESTRICT context, const uint64_t offset, void *POUND_RESTRICT destination,
                       const size_t size);

    /// The caller's buffer, for a reader built by `fs_reader_from_buffer`.
    ///
    /// These live in the reader rather than in a separately allocated context because a
    /// heap-allocated context would need a matching destructor, and a reader is the sort
    /// of thing a parser stores in a stack frame or a union and forgets about. Making
    /// the common case allocation-free is worth two fields; a source with a lifetime
    /// that genuinely needs a context of its own supplies one through `context`.
    const uint8_t *POUND_RESTRICT buffer_data;

    /// Length of `buffer_data`.
    size_t buffer_size;
} fs_reader_t;

/// Wraps a caller-owned buffer in an `fs_reader_t`.
///
/// The buffer must outlive the reader; the reader stores the pointer and does not copy
/// or free it. This is how a test feeds a parser, and how a caller feeds one buffer of
/// an image that is already in memory.
///
/// `data` may be NULL only when `size` is zero. On failure the reader is left zeroed,
/// so a caller that ignores the return value gets a reader whose `read_at` is NULL and
/// therefore refuses every read, rather than a reader pointing at freed memory.
void fs_reader_from_buffer(fs_reader_t *POUND_RESTRICT reader, void *POUND_RESTRICT data, const size_t size);

/// Reads `size` bytes at `offset` through `reader`.
///
/// A convenience over `reader->read_at` for the common case, and the function every
/// parser actually calls so there is one place where the "whole range must be inside
/// the source" rule is enforced rather than re-implemented per parser.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` if `reader` or `destination` is NULL, or if
/// `reader->read_at` is NULL; `POUND_ERROR_IO` if the range would extend past the end
/// of the source, checked with a comparison written so that `offset + size` cannot
/// wrap; otherwise whatever the underlying reader returned.
error_t fs_reader_read_at(const fs_reader_t *POUND_RESTRICT reader, const uint64_t offset,
                          void *POUND_RESTRICT destination, const size_t size);

/// Returns whether `[offset, offset + size)` lies entirely inside `reader`'s source.
///
/// The same overflow-safe comparison `fs_reader_read_at` uses, exposed so a parser can
/// validate a range it is about to hand to something other than the reader -- handing
/// a guest-visible offset to a decompressor, or a partition chain to another
/// `fs_reader_t`. Returns false for a NULL reader.
///
/// Not logging on failure is deliberate: this is a predicate that gets called in a
/// loop over a table of entries, and the caller logs once with the specific entry that
/// failed rather than once per rejected range.
bool fs_reader_range_is_valid(const fs_reader_t *POUND_RESTRICT reader, const uint64_t offset, const size_t size);

/// Copies `source` into `copy` such that `copy` remains usable after `source` is gone.
///
/// ## Why a struct assignment is not enough
///
/// A reader built by `fs_reader_from_buffer` points its own `context` at itself: the read
/// function needs the reader to find `buffer_data` and `buffer_size`, and passing the
/// reader as the context is how it gets them without an allocation. So
///
/// ```c
/// copy = *source;
/// ```
///
/// leaves `copy.context` addressing **`source`**. Every read through `copy` then
/// dereferences the original. When `source` was a local in the caller's frame, that is a
/// use-after-free the moment the call returns -- and it does not fail loudly. The stack
/// slot usually still holds the reader, so reads succeed and return the right bytes right
/// up until the frame is reused, at which point a partition starts returning whatever else
/// has since been written there.
///
/// This is the only correct form of "copy a reader", so it exists here rather than being
/// open-coded at each parser: a parser that stores a reader has to call this, and the
/// comment above is what tells the next one.
///
/// ## What it deliberately does not touch
///
/// Only a `context` that points at `source` is rebound. A reader whose `context` is a
/// real allocation or a file handle keeps it: that memory is owned by whoever made it and
/// copying the pointer is correct. Detecting the difference is exactly the self-reference
/// test, so there is nothing else to decide.
///
/// Logs and does nothing on a NULL `copy`. A NULL `source` leaves `copy` zeroed, which
/// refuses every read rather than leaving a previous reader's bytes reachable.
void fs_reader_copy(fs_reader_t *POUND_RESTRICT copy, const fs_reader_t *POUND_RESTRICT source);
void fs_reader_copy(fs_reader_t *POUND_RESTRICT copy, const fs_reader_t *POUND_RESTRICT source);

#endif // POUND_FS_FS_READER_H

/*** end of file ***/

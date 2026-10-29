#ifndef POUND_MEMORY_ARENA_H
#define POUND_MEMORY_ARENA_H

//! A bounded bump allocator with an explicit owner and rewindable scopes.
//!
//! Compound already has a fixed-block pool (`pool_allocator.h`) and a size-class
//! slab (`slab_allocator.h`). Both answer "give me one object" and pay for a free
//! list so individual objects can be returned. A loader has a different shape:
//! it carves a title's code, read-only data, writable data, a stack, and a
//! handful of small metadata blocks once, hands them to the guest, and only
//! releases them together when the title is unloaded. For that workload a free
//! list is pure overhead and a failed allocation halfway through is the common
//! case, not the exceptional one.
//!
//! An arena is that workload's allocator: one contiguous host buffer, a cursor,
//! and a checkpoint/rewind pair. It cannot hand a single block back, and it does
//! not pretend to. What it does provide is the part a loader actually needs to
//! be safe:
//!
//! - every allocation is aligned, overflow-checked, and zeroed before it is
//!   returned, so an uninitialised read cannot leak the previous title's data;
//! - every arena carries an owner tag, so memory accounting can attribute a
//!   title's heap to that title rather than to "somewhere in the loader";
//! - a rewind is checked against the current cursor, so a stale checkpoint
//!   cannot move the cursor forwards or outside the backing buffer; and
//! - the high-water mark survives a reset, which is what a loader uses to size
//!   the next title's arena without guessing.
//!
//! The arena owns no host memory. The caller owns the buffer and must keep it
//! alive for as long as the arena is in use. This is deliberate: `boot.c`
//! allocates its guest window through `memory_subsystem_allocate`, and the arena
//! is only the view the loader uses to carve that window.

#include "attributes.h"
#include "errors.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/// Identifies a `pound_arena_t` that `pound_arena_init` has prepared.
#define POUND_ARENA_MAGIC 0x4152454EU

/// Marker returned by `pound_arena_checkpoint` before any allocation.
///
/// A checkpoint is just the cursor, so zero is also the cursor of a freshly
/// initialised arena. It is named separately because a caller writing a
/// checkpoint to disk or into a guest structure should not have to know that.
#define POUND_ARENA_CHECKPOINT_START ((size_t)0)

/// Bookkeeping for one arena.
///
/// The struct is intentionally plain data. A caller may keep it in a boot state
/// struct and copy that struct as a whole, as long as it does not use the copy
/// while the original is still allocating from the same buffer.
typedef struct
{
    /// Start of the caller-owned backing buffer. Never written by the arena
    /// itself except through `pound_arena_allocate`.
    uint8_t *base;

    /// Number of bytes in the backing buffer.
    size_t capacity;

    /// Bytes handed out so far. Also the value `pound_arena_checkpoint` returns.
    size_t used;

    /// Largest value `used` has ever reached. Retained across resets so a
    /// loader can size the next arena from observed demand.
    size_t high_water;

    /// Allocation tag. The arena never interprets it; it is returned verbatim so
    /// memory accounting can charge the allocation to a title or subsystem.
    uint64_t owner;

    /// Alignment applied when a caller passes an alignment of zero.
    size_t default_alignment;

    /// Number of successful `pound_arena_allocate` calls since init.
    uint32_t allocation_count;

    /// Number of refused allocations since init. A non-zero value with a
    /// non-zero `used` is the normal "the last title did not fit" signal.
    uint32_t failed_allocations;

    /// Bumped by every reset. A checkpoint recorded before a reset carries the
    /// old value and is refused, rather than being mistaken for a checkpoint at
    /// the start of the new scope.
    uint32_t generation;

    /// `POUND_ARENA_MAGIC` once initialised. Guards every entry point against a
    /// zeroed or uninitialised arena.
    uint32_t magic;
} pound_arena_t;

/// One rewind point. Opaque to callers, and deliberately not just the cursor:
/// the allocation count and generation move with it so a stale scope is
/// distinguishable from the current one.
typedef struct
{
    size_t   used;
    uint32_t allocation_count;
    uint32_t generation;
} pound_arena_checkpoint_t;

/// Prepares `arena` to hand out bytes from `buffer`.
///
/// `alignment` is the default alignment for allocations that do not request a
/// stricter one. It must be a power of two, and `buffer` must already satisfy
/// it; the arena refuses rather than silently shifting the base and losing the
/// caller's ownership of the first bytes.
///
/// Returns:
/// - `POUND_SUCCESS`.
/// - `POUND_ERROR_INVALID_ARGUMENT` -- a NULL arena or buffer, a zero size, or
///   a default alignment that is not a power of two.
/// - `POUND_ERROR_MEMORY_ALIGNMENT` -- `buffer` is not aligned to `alignment`.
error_t pound_arena_init(pound_arena_t *POUND_RESTRICT  arena,
                         void *POUND_RESTRICT             buffer,
                         size_t                           size,
                         size_t                           alignment,
                         uint64_t                         owner);

/// Returns the arena to its initial state and zeroes every byte it has handed
/// out. The owner tag, capacity, default alignment and high-water mark survive.
///
/// A reset of an already-empty arena is a no-op, so a caller can reset at the
/// start of a load without first tracking whether the previous load ran.
error_t pound_arena_reset(pound_arena_t *POUND_RESTRICT arena);

/// Allocates `size` zeroed bytes aligned to at least `alignment`.
///
/// A zero `alignment` means "use the arena's default". A non-zero alignment
/// must be a power of two and a multiple of the default alignment, so a caller
/// cannot ask for a weaker guarantee than the backing buffer provides.
///
/// On success `*out` receives the pointer and the bytes are zeroed. On failure
/// `*out` is set to NULL; a caller can therefore use the same cleanup path for
/// "did not fit" and "already failed" without inspecting the arena.
///
/// Returns:
/// - `POUND_SUCCESS`.
/// - `POUND_ERROR_INVALID_ARGUMENT` -- a NULL arena or out pointer, a zero
///   size, or an alignment that is not a power of two.
/// - `POUND_ERROR_MEMORY_ALIGNMENT` -- a requested alignment weaker than the
///   arena's default, or not a multiple of it.
/// - `POUND_ERROR_NOT_INITIALIZED` -- the arena was never initialised.
/// - `POUND_ERROR_ALLOCATION_FAILED` -- the aligned allocation would exceed the
///   backing buffer. `failed_allocations` is incremented.
error_t pound_arena_allocate(pound_arena_t *POUND_RESTRICT arena,
                             size_t                        size,
                             size_t                        alignment,
                             void **POUND_RESTRICT         out);

/// Records the current cursor for a later `pound_arena_rewind`.
pound_arena_checkpoint_t pound_arena_checkpoint(const pound_arena_t *POUND_RESTRICT arena);

/// Releases every allocation made after `checkpoint` and zeroes those bytes.
///
/// Rewinding to the current cursor is a no-op. Rewinding to a checkpoint that
/// was never produced by this arena, or was produced before a reset, is refused
/// rather than clamped: a stale checkpoint means the caller's scope bookkeeping
/// is wrong, and moving the cursor to a plausible-but-wrong place would hide it.
///
/// Returns:
/// - `POUND_SUCCESS`.
/// - `POUND_ERROR_INVALID_ARGUMENT` -- a NULL arena.
/// - `POUND_ERROR_NOT_INITIALIZED` -- the arena was never initialised.
/// - `POUND_ERROR_CORRUPTED` -- the checkpoint is ahead of the cursor or its
///   allocation count is inconsistent with the live scope.
error_t pound_arena_rewind(pound_arena_t *POUND_RESTRICT               arena,
                           const pound_arena_checkpoint_t checkpoint);

/// Bytes still available without growing the backing buffer.
size_t pound_arena_remaining(const pound_arena_t *POUND_RESTRICT arena);

/// Bytes handed out since the last reset.
size_t pound_arena_used(const pound_arena_t *POUND_RESTRICT arena);

/// Largest number of bytes ever handed out by this arena.
size_t pound_arena_high_water(const pound_arena_t *POUND_RESTRICT arena);

/// Owner tag supplied to `pound_arena_init`.
uint64_t pound_arena_owner(const pound_arena_t *POUND_RESTRICT arena);

/// Reports whether `arena` has been initialised and not subsequently corrupted.
bool pound_arena_is_initialized(const pound_arena_t *POUND_RESTRICT arena);

#ifdef __cplusplus
}
#endif

#endif // POUND_MEMORY_ARENA_H

/*** end of file ***/

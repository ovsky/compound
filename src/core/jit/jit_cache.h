//! Write/execute ("W^X") code cache for host-native compiled guest code.
//!
//! A JIT must emit host machine code into memory, then make that memory
//! executable, and it must be able to make it writable again to patch a block in
//! place. Doing that with a general-purpose heap means a `mprotect`/`VirtualProtect`
//! per block per transition, and it means no way to prove that no page was ever
//! writable *and* executable at the same time. This module is the cache that
//! makes both properties cheap and checkable.
//!
//! # W^X is enforced, not requested
//!
//! No call in this API can produce a page that is both writable and executable.
//! Every page the cache owns is in exactly one of three states, and the
//! transitions are one-way in each direction:
//!
//! ```text
//!   RW  --protect_rx-->  RX  --protect_rw-->  RW
//! ```
//!
//! The *only* way to get executable memory is `jit_cache_alloc_executable`, which
//! hands out RW memory and requires an explicit `jit_cache_protect_rx` before the
//! block may be executed. The cache tracks which state each block is in and
//! reports a mismatch rather than silently succeeding, so a caller that forgets
//! the transition gets a typed error and a log record instead of a fault two
//! million instructions later.
//!
//! # Executable blocks own whole pages; data blocks do not
//!
//! `mprotect` and `VirtualProtect` both take a page-aligned range and round any
//! interior address outward, so flipping a sub-page block would change the
//! protection of whatever shares those pages. An executable block is therefore
//! carved at a page-aligned offset with a page-multiple capacity. Data blocks are
//! aligned only as asked, because nothing ever flips their protection.
//!
//! # Windows cannot alias two protections over one range
//!
//! `bal_executable_buffer_t` carries both a `rw_pointer` and an `rx_pointer`
//! because on POSIX a range can be `mmap`ed twice with different protections and
//! the pair never changes. A user-mode Windows process cannot do that: `NtMapViewOfSection`
//! with two protection attributes is not reachable from Win32, and `VirtualAlloc`
//! has a single `flProtect` for the whole reservation. On Windows the two pointers
//! therefore *alias the same address*, and the transition is a `VirtualProtect`.
//!
//! What the Ballistic contract actually requires is "write through `rw_pointer`,
//! execute through `rx_pointer`". That holds identically either way, so aliasing
//! costs nothing and pretending otherwise would be the dishonest option.
//!
//! Apple platforms are a third case: `MAP_JIT` plus
//! `pthread_jit_write_protect_np`. The path is implemented per the documented
//! API, but the project has no macOS CI lane, so it is unverified here.
//!
//! # Reuse, coalescing and why the arena is chunked
//!
//! Memory comes from chunks of `jit_cache_config_t::chunk_bytes`, reserved and
//! committed together as needed rather than in one large up-front reservation.
//! That is deliberate: a large `MEM_RESERVE` still has to be committed before it
//! is touched, and committing 256 MiB at start-up would charge the whole thing
//! against the Windows commit limit on a machine that may only ever run one
//! game. A JIT working set is bursty, so the cache grows into it.
//!
//! Within a chunk, allocation is first-fit over a sorted, coalesced list of free
//! extents, with the cursor used only when no extent is large enough. Freed space
//! is reused rather than stranded, and adjacent extents merge on free, so a churn
//! of same-sized blocks converges to one extent instead of fragmenting the chunk.
//!
//! A chunk is the reclaim unit, and only a *trailing* chunk that holds no live
//! block can be unmapped -- the same rule the slab allocator follows, for the same
//! reason: an extent below a live one cannot be returned to the OS without
//! fragmenting the address space into pieces no later chunk would fit. A chunk
//! that has free space but is not trailing is retained; `jit_cache_reclaim`
//! returns how many bytes it actually gave back, and zero is a legitimate answer
//! for a fragmented cache.
//!
//! # Instruction cache coherence
//!
//! After the guest's machine code is written, the host's instruction cache may
//! hold the previous contents of those addresses. Every transition to RX flushes
//! the range. The flush happens *before* the protection change, while the range is
//! still writable: libgcc's `__clear_cache` is a kernel call on the platforms
//! Pound targets, but it is not contractually a non-writing function, and calling
//! it on a range that has just become read-only would turn a cache-coherency bug
//! into a fault.
//!
//! # Thread safety
//!
//! Every entry point takes the cache's mutex, including the read-only queries, so
//! a stats snapshot is self-consistent. Protection transitions are serialised
//! with allocation, which means a block cannot be flipped while it is being freed
//! on another thread.

#ifndef POUND_JIT_CACHE_H
#define POUND_JIT_CACHE_H

#include "attributes.h"
#include "errors.h"
#include "sync/mutex.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Default alignment for a data block.
#define JIT_CACHE_DEFAULT_ALIGNMENT 64

/// Smallest alignment a caller may request.
///
/// 16 rather than 1 because every block descriptor the cache keeps is pointer
/// aligned, and because a JIT block is a code fragment, not a byte array.
#define JIT_CACHE_MIN_ALIGNMENT 16

/// Largest alignment a caller may request.
///
/// Bounded so that an aligned block still leaves room for at least one other in
/// any chunk the cache will accept.
#define JIT_CACHE_MAX_ALIGNMENT (64U * 1024U)

/// Default bytes per chunk.
#define JIT_CACHE_DEFAULT_CHUNK_BYTES (4U * 1024U * 1024U)

/// Smallest chunk a cache may be configured with.
///
/// 256 KiB is a floor rather than an arbitrary round number: a chunk must be able
/// to hold a page-aligned executable block plus the remainder that alignment
/// wastes, and the default 64 KiB request plus a 64 KiB alignment would leave
/// nothing for a second block.
#define JIT_CACHE_MIN_CHUNK_BYTES (256U * 1024U)

/// Largest chunk a cache may be configured with.
#define JIT_CACHE_MAX_CHUNK_BYTES (256U * 1024U * 1024U)

/// Default ceiling on bytes committed to the OS.
#define JIT_CACHE_DEFAULT_MAX_BYTES (512U * 1024U * 1024U)

/// Byte written over a block when poisoning is enabled.
///
/// Matches `POOL_ALLOCATOR_POISON_BYTE` so "poisoned" means one thing across the
/// memory subsystem.
#define JIT_CACHE_POISON_BYTE 0xDD

/// Smallest remainder worth recording as a free extent after a split.
///
/// Below this the two pieces would be separated by bookkeeping costs more than
/// the space is worth, so the whole extent is handed out instead. Sized to hold a
/// handful of minimum-sized blocks.
#define JIT_CACHE_MIN_SPLIT_REMAINDER 256U

/// Protection state of a live block.
typedef enum
{
    /// Not handed out. Never observable through the public API.
    JIT_BLOCK_STATE_FREE = 0,

    /// Readable and writable. Not executable.
    JIT_BLOCK_STATE_READ_WRITE,

    /// Readable and executable. Not writable.
    JIT_BLOCK_STATE_READ_EXECUTE,
} jit_block_state_t;

/// Whether a block is ever made executable.
typedef enum
{
    /// Plain host memory. Never flipped, so it may be sub-page.
    JIT_BLOCK_KIND_DATA = 0,

    /// Guest-derived machine code. Page aligned, page sized, and flippable.
    JIT_BLOCK_KIND_EXECUTABLE,
} jit_block_kind_t;

/// Construction parameters for the cache.
///
/// Distinct from every per-block request so that a block's size cannot be passed
/// where the cache's were meant.
typedef struct
{
    /// Bytes per chunk. Zero takes `JIT_CACHE_DEFAULT_CHUNK_BYTES`. Must be a
    /// multiple of the host page size.
    size_t chunk_bytes;

    /// Ceiling on bytes committed to the OS. Zero takes
    /// `JIT_CACHE_DEFAULT_MAX_BYTES`. Exceeding it makes allocation fail with
    /// `POUND_ERROR_ALLOCATION_FAILED`; it is never exceeded silently.
    size_t max_bytes;

    /// Fill each block with `JIT_CACHE_POISON_BYTE` when it is allocated.
    ///
    /// Applied at allocation only, never at free. Poisoning on free would mean a
    /// protection flip on every release, which is the single most expensive thing
    /// this cache exists to avoid -- and poisoning is meant to catch code that is
    /// *executed before being written*, which is decided at allocation.
    bool poison;

    /// Explicit so the struct's size does not depend on `bool`'s width.
    char pad[7];
} jit_cache_config_t;

/// The cache's resolved configuration, as reported by `jit_cache_describe`.
typedef struct
{
    /// Bytes per chunk, after defaults.
    size_t chunk_bytes;

    /// Byte ceiling, after defaults.
    size_t max_bytes;

    /// The host page size the cache detected. Executable blocks are aligned and
    /// sized to it.
    size_t page_size;

    /// Alignment given to blocks that do not ask for one.
    size_t default_alignment;

    /// Whether poisoning is on.
    bool poison;

    /// Explicit padding.
    char pad[7];
} jit_cache_resolved_t;

/// Everything known about one live block.
typedef struct
{
    /// Bytes the caller actually asked for. Never exceeds `capacity`.
    size_t requested;

    /// Bytes actually available from the block's address. Greater than
    /// `requested` when the block absorbed an alignment remainder or a split
    /// remainder.
    size_t capacity;

    /// Offset of the block within its chunk. Exposed for diagnostics; not a
    /// stable identifier.
    size_t offset;

    /// Index of the owning chunk in the cache's chunk list.
    size_t chunk_index;

    /// Whether the block is ever made executable.
    uint32_t kind;

    /// Current protection state.
    uint32_t state;
} jit_block_info_t;

/// Snapshot of cache activity, sampled under the cache lock.
typedef struct
{
    /// Chunks currently reserved and committed.
    size_t chunk_count;

    /// Bytes committed to the OS across every chunk.
    size_t reserved_bytes;

    /// Bytes handed out across every live block.
    size_t live_bytes;

    /// Bytes not handed out: free extents plus each chunk's uncarved tail.
    size_t free_bytes;

    /// Live blocks.
    size_t live_blocks;

    /// Live blocks currently writable. A non-zero count is normal between
    /// allocation and `jit_cache_protect_rx`, and means the guest's code is being
    /// edited.
    size_t rw_blocks;

    /// Live blocks currently executable.
    size_t rx_blocks;

    /// Successful allocations since initialisation.
    uint64_t allocations;

    /// Successful frees since initialisation.
    uint64_t frees;

    /// Allocations refused because no chunk could satisfy them.
    uint64_t alloc_failures;

    /// Transitions from RX back to RW.
    uint64_t rw_transitions;

    /// Transitions from RW to RX, each of which flushed the instruction cache.
    uint64_t rx_transitions;

    /// Chunks unmapped by `jit_cache_reclaim` and `jit_cache_reset`.
    uint64_t reclaims;
} jit_cache_stats_t;

typedef struct jit_chunk jit_chunk_t;

/// The cache.
///
/// Defined here rather than in the translation unit so a caller can put one on
/// the stack -- which is what lets the test suite exercise the cache without a
/// host allocator of its own, and what keeps the struct's single ownership
/// obvious.
struct jit_cache
{
    /// Guards everything below. Every entry point takes it, including the
    /// read-only queries, so a snapshot is self-consistent.
    mutex_t lock;

    /// Chunks, in creation order. Only the newest can be unmapped by
    /// `jit_cache_reclaim`; see the note on fragmentation in the module comment.
    jit_chunk_t *chunks;

    /// Number of chunks in `chunks`.
    size_t chunk_count;

    /// Host page size, detected at init. Executable blocks are aligned and sized
    /// to it so that flipping their protection cannot touch a neighbour.
    size_t page_size;

    /// Bytes per chunk, after defaults.
    size_t chunk_bytes;

    /// Byte ceiling on committed memory, after defaults.
    size_t max_bytes;

    /// Alignment given to blocks that do not request one.
    size_t default_alignment;

    /// Bytes committed to the OS across every chunk.
    size_t reserved_bytes;

    /// Live blocks across every chunk.
    size_t live_blocks;

    /// Bytes handed out across every live block.
    size_t live_bytes;

    /// Live blocks currently writable.
    size_t rw_blocks;

    /// Live blocks currently executable.
    size_t rx_blocks;

    /// Successful allocations.
    uint64_t allocations;

    /// Successful frees.
    uint64_t frees;

    /// Allocations refused for want of space.
    uint64_t alloc_failures;

    /// RX-to-RW transitions.
    uint64_t rw_transitions;

    /// RW-to-RX transitions.
    uint64_t rx_transitions;

    /// Chunks unmapped.
    uint64_t reclaims;

    /// Whether newly allocated blocks are poisoned.
    bool poison;

    /// Whether `lock` holds a usable mutex.
    bool initialised;

    /// Explicit padding so the struct's size does not depend on `bool` width.
    bool pad[6];
};

typedef struct jit_cache jit_cache_t;

/// Initialises `cache`.
///
/// `config` may be NULL, which takes every default. Returns `POUND_SUCCESS`, or a
/// typed error with a log record: `POUND_ERROR_INVALID_ARGUMENT` for a NULL
/// cache, `POUND_ERROR_MEMORY_ALIGNMENT` for an alignment or chunk size that is
/// not a power of two / not a multiple of the host page size, and
/// `POUND_ERROR_ALLOCATION_FAILED` when no configuration could ever yield a
/// block.
///
/// Initialisation reserves nothing. The first allocation creates the first chunk,
/// so an emulator that loads a game but never JITs a block costs nothing.
error_t jit_cache_init(jit_cache_t *POUND_RESTRICT cache,
                       const jit_cache_config_t *POUND_RESTRICT config);

/// Releases every chunk and the cache's mutex.
///
/// Does not require that every block has been freed, but logs a warning naming the
/// live count when one remains. Logs and ignores a NULL or uninitialised cache.
void jit_cache_destroy(jit_cache_t *POUND_RESTRICT cache);

/// Drops every block and every chunk, keeping the configuration.
///
/// Unlike `destroy` this keeps the mutex, so the cache can be used again
/// immediately. This is the reset a guest does when it changes game or reloads a
/// save state.
void jit_cache_reset(jit_cache_t *POUND_RESTRICT cache);

/// Writes the resolved configuration to `out`.
error_t jit_cache_describe(const jit_cache_t *POUND_RESTRICT cache,
                           jit_cache_resolved_t *POUND_RESTRICT out);

/// Allocates a writable, non-executable block of at least `size` bytes.
void *jit_cache_alloc(jit_cache_t *POUND_RESTRICT cache, size_t size);

/// Allocates a writable, non-executable block aligned to `alignment`.
void *jit_cache_alloc_aligned(jit_cache_t *POUND_RESTRICT cache, size_t alignment, size_t size);

/// Releases a block previously returned by `jit_cache_alloc*`.
///
/// A NULL pointer is a no-op. A pointer the cache did not produce, or one that
/// has already been released, is refused and logged rather than corrupting the
/// free-extent list.
void jit_cache_free(jit_cache_t *POUND_RESTRICT cache, void *POUND_RESTRICT pointer);

/// Allocates a page-aligned block of at least `size` bytes for machine code.
///
/// The block is returned *writable and not executable*. It must be passed to
/// `jit_cache_protect_rx` before anything jumps to it.
void *jit_cache_alloc_executable(jit_cache_t *POUND_RESTRICT cache, size_t size);

/// As `jit_cache_alloc_executable`, with an explicit alignment.
///
/// The effective alignment is `max(alignment, page_size)`, because a block whose
/// protection is ever flipped has to own whole pages.
void *jit_cache_alloc_executable_aligned(jit_cache_t *POUND_RESTRICT cache,
                                         size_t                               alignment,
                                         size_t                               size);

/// Releases a block previously returned by `jit_cache_alloc_executable*`.
void jit_cache_free_executable(jit_cache_t *POUND_RESTRICT cache, void *POUND_RESTRICT pointer);

/// Makes a block read-execute and flushes the host instruction cache over it.
///
/// Idempotent: a block that is already RX is accepted and counted as a transition
/// only if it actually changed. Requires the block to be one obtained from
/// `jit_cache_alloc_executable*`; a data block has no whole-page ownership and is
/// refused with `POUND_ERROR_MEMORY_FAULT`.
error_t jit_cache_protect_rx(jit_cache_t *POUND_RESTRICT cache, void *POUND_RESTRICT pointer);

/// Makes a block read-write again, so it can be patched in place.
error_t jit_cache_protect_rw(jit_cache_t *POUND_RESTRICT cache, void *POUND_RESTRICT pointer);

/// Returns a block's current protection state.
///
/// A pointer the cache does not own reports `JIT_BLOCK_STATE_FREE`, which is
/// indistinguishable from an uninitialised cache by design: the question "is this
/// executable" is only ever asked about pointers the caller believes the cache
/// owns, and answering "no" for everything else is the safe direction to be wrong
/// in.
jit_block_state_t jit_cache_block_state(const jit_cache_t *POUND_RESTRICT cache,
                                        const void *POUND_RESTRICT       pointer);

/// Returns true when `pointer` is a live block currently in the RX state.
bool jit_cache_is_executable(const jit_cache_t *POUND_RESTRICT cache, const void *POUND_RESTRICT pointer);

/// Writes everything known about `pointer` to `out`.
error_t jit_cache_get_block_info(const jit_cache_t *POUND_RESTRICT cache,
                                 const void *POUND_RESTRICT           pointer,
                                 jit_block_info_t *POUND_RESTRICT     out);

/// Returns a block's usable size, or `0` for a pointer the cache does not own.
size_t jit_cache_usable_size(const jit_cache_t *POUND_RESTRICT cache, const void *POUND_RESTRICT pointer);

/// Writes a snapshot of cache activity to `out`.
error_t jit_cache_get_stats(const jit_cache_t *POUND_RESTRICT cache,
                            jit_cache_stats_t *POUND_RESTRICT        out);

/// Unmaps every trailing chunk that holds no live block.
///
/// Returns the number of bytes actually returned to the OS, which is legitimately
/// zero for a fragmented cache whose newest chunk still holds a live block.
size_t jit_cache_reclaim(jit_cache_t *POUND_RESTRICT cache);

#endif // POUND_JIT_CACHE_H

/*** end of file ***/
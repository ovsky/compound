//! Fixed-size-block pool allocator modelling Horizon OS kernel memory pools.
//!
//! Horizon OS carves guest heap objects out of a small number of fixed-size
//! pools rather than from a general-purpose heap, so an allocation, a free and
//! a stats query are all lock-and-tread operations with no coalescing and no
//! size negotiation. That is exactly what this module is: an arena that is
//! carved once into per-size-class block runs, each with its own LIFO free list.
//!
//! # Why not just call mimalloc
//!
//! Because the guest allocator has different requirements from the host one.
//! Guest object lifetimes must be reproducible for save states, the set of live
//! sizes must be knowable ahead of time so a save file can record the pool
//! state, and a poisoned block must be identifiable so a guest use-after-free
//! is caught inside the guest rather than as an undefined host crash. mimalloc
//! optimises for host throughput and deliberately poisons nothing.
//!
//! # Size classes
//!
//! Class `i` holds `min_payload + ((min_payload * (growth - 1)) * i) / 1024`
//! usable bytes, rounded up to `alignment`, for growth expressed in
//! thousandths. A growth of 1250 is the default: large enough that a typical
//! allocation does not waste half its block, small enough that the largest
//! covered size stays within `max_payload` over a manageable number of classes.
//!
//! # Alignment
//!
//! Every payload in a pool is aligned to the pool's configured `alignment`, not
//! to whatever a caller happened to ask for. This mirrors Linux's
//! `ARCH_KMALLOC_MINALIGN` model: a fixed alignment is what allows a block
//! header to sit immediately before every payload with no per-request
//! arithmetic. A request for stricter alignment than the pool was configured
//! with is refused with `POUND_ERROR_MEMORY_ALIGNMENT` and logged, never
//! silently rounded down to a misaligned address.
//!
//! # Thread safety
//!
//! Every public entry point takes the pool's mutex, including the read-only
//! queries. `get_usable_size` and `get_stats` therefore serialise against
//! concurrent allocation, which is what makes their results self-consistent
//! rather than a torn snapshot of a counter mid-update.

#ifndef POUND_POOL_ALLOCATOR_H
#define POUND_POOL_ALLOCATOR_H

#include "attributes.h"
#include "errors.h"
#include "sync/mutex.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Fills freed and freshly allocated blocks so that use-after-free and
/// read-before-write are both detectable. Matches the value produced by
/// `memory_fill_poisoned`.
#define POOL_ALLOCATOR_POISON_BYTE 0xDD

/// Upper bound on size classes.
///
/// A class is 16 bytes of metadata, and every class is walked linearly by
/// `pool_allocator_class_for_size`. The real bound is set by `max_payload` and
/// `growth`; this is only a defensive cap against a nonsensical configuration.
#define POOL_ALLOCATOR_MAX_CLASSES 64

/// Default alignment when `pool_config_t::alignment` is zero.
#define POOL_ALLOCATOR_DEFAULT_ALIGNMENT 16

/// Smallest alignment a pool may be configured with.
///
/// 8 is the floor because the block header packs a 32-bit magic, a 32-bit class
/// index and a 64-bit free-list link; an alignment below 8 could not place the
/// link without misalignment.
#define POUND_POOL_MIN_ALIGNMENT 8

/// Largest alignment a pool may be configured with.
///
/// Bounded so the header stride can never exceed a page, which keeps every
/// payload inside the page its block starts in and therefore keeps
/// `get_usable_size` valid for a multi-page payload.
#define POUND_POOL_MAX_ALIGNMENT 4096

typedef struct pool_block pool_block_t;

/// Bookkeeping stored immediately before every payload.
///
/// Laid out to exactly 16 bytes so that with the default alignment the header
/// consumes no padding at all. The field order is chosen for that: two 32-bit
/// fields then a naturally-aligned pointer.
struct pool_block
{
    /// Identifies this block as belonging to a pool, and whether it is live.
    ///
    /// Two distinct values rather than one plus a separate flag, because the
    /// discrimination has to work in both directions: `free` must reject a
    /// block that is already free, while `get_usable_size` must resolve a block
    /// that is currently live. A single "zero means live" convention can only
    /// express one of those.
    uint32_t magic;

    /// Index into `pool_allocator_t::classes`. Validated on free.
    uint32_t class_index;

    /// Free-list link. Only meaningful while the block is free; the bytes are
    /// reused for payload while the block is live.
    pool_block_t *next;
};

/// Magic held by a block that is on a free list and available to hand out.
#define POOL_BLOCK_MAGIC_FREE 0x46554F50u /* 'POUF' little-endian */

/// Magic held by a block that is currently handed out to a caller.
#define POOL_BLOCK_MAGIC_LIVE 0x49564F50u /* 'POVI' little-endian */

/// Per-size-class state.
typedef struct
{
    /// Usable bytes at `block + header_stride`. This is what
    /// `pool_allocator_get_usable_size` reports.
    size_t payload_size;

    /// Distance between consecutive blocks in this class: header plus payload,
    /// rounded up to the pool alignment so every payload stays aligned.
    size_t stride;

    /// Blocks carved out of the arena for this class.
    size_t capacity;

    /// Blocks currently handed out.
    size_t used;

    /// High-water mark of `used`, for leak diagnosis.
    size_t peak_used;

    /// Most recently freed block; `NULL` when the class is exhausted.
    pool_block_t *free_head;

    /// Cumulative successful allocations for this class.
    uint64_t allocations;

    /// Cumulative frees for this class.
    uint64_t frees;
} pool_class_t;

/// Construction parameters. Zeroed fields take the documented default.
///
/// Field order is chosen so the struct has no implicit padding: the two small
/// fields share the tail of the last eight-byte slot.
typedef struct
{
    /// Payload size of class 0. Default `alignment`.
    size_t min_payload;

    /// Payload size of the largest class. Default 4096.
    size_t max_payload;

    /// Payload alignment for every block. Default
    /// `POOL_ALLOCATOR_DEFAULT_ALIGNMENT`. Must be a power of two in
    /// `[POUND_POOL_MIN_ALIGNMENT, POUND_POOL_MAX_ALIGNMENT]`.
    size_t alignment;

    /// Size-class growth in thousandths, so 1250 means "25% larger each class".
    /// Must be greater than 1000. Default 1250.
    uint32_t growth_permille;

    /// Poison freed and freshly allocated blocks. Default `false`, because the
    /// cost is a full block write on every allocation and it is only wanted in
    /// debug runs and when reproducing guest bugs.
    bool poison;

    /// Absorbs the tail padding of `growth_permille` and `poison` explicitly,
    /// following the convention already used by `gui_plugin_t`.
    char pad[3];
} pool_config_t;

/// Snapshot of pool activity.
///
/// All fields are consistent with each other because `get_stats` samples under
/// the pool lock rather than reading the live counters.
typedef struct
{
    /// Bytes of arena handed to the pool.
    size_t arena_size;

    /// Bytes of arena consumed by block runs, including headers.
    size_t arena_committed;

    /// Sum of `payload_size` over every live block.
    size_t bytes_in_use;

    /// High-water mark of `bytes_in_use`.
    size_t peak_bytes_in_use;

    /// Number of size classes.
    size_t class_count;

    /// Allocation requests, including ones that failed.
    uint64_t total_requests;

    /// Requests refused because no class could serve them.
    uint64_t total_failures;

    /// Requests refused because the arena was full.
    uint64_t arena_exhaustions;

    /// Successful allocations and frees. Equal in a leak-free program.
    uint64_t total_allocations;
    uint64_t total_frees;
} pool_stats_t;

typedef struct
{
    /// Arena base. Owned by the caller; the pool never frees it.
    uint8_t *arena;

    /// Bytes available at `arena`.
    size_t arena_size;

    /// Offset of the next uncarved byte in the arena.
    size_t bump;

    pool_class_t classes[POOL_ALLOCATOR_MAX_CLASSES];
    size_t      class_count;

    /// Bytes between a payload and its header. Aligned to `alignment` so that
    /// the payload of any block is aligned.
    size_t header_stride;

    /// Payload alignment, and the bound on request alignment.
    size_t alignment;

    size_t   bytes_in_use;
    size_t   peak_bytes_in_use;
    uint64_t total_requests;
    uint64_t total_failures;
    uint64_t arena_exhaustions;
    uint64_t total_allocations;
    uint64_t total_frees;

    bool    poison_enabled;
    uint8_t poison_byte;

    /// Aligns the mutex on the next eight-byte boundary without relying on the
    /// compiler inserting a hole.
    char pad[6];

    /// Guards every field above.
    mutex_t lock;
} pool_allocator_t;

/// Initialises `pool` over `arena`.
///
/// The arena is carved into all size classes up front, so a pool never grows
/// and never moves a live block; `arena` must therefore outlive every payload
/// returned from it.
///
/// `config` may be NULL for the documented defaults.
///
/// `arena` must be non-NULL, `arena_size` non-zero, and the address of
/// `arena` a multiple of the configured alignment. Returns `POUND_SUCCESS`, or
/// `POUND_ERROR_INVALID_ARGUMENT` / `POUND_ERROR_MEMORY_ALIGNMENT` for the
/// above, or `POUND_ERROR_ALLOCATION_FAILED` when the arena cannot fit a
/// single block of the smallest class.
error_t pool_allocator_init(pool_allocator_t *POUND_RESTRICT pool,
                            void *POUND_RESTRICT           arena,
                            size_t                         arena_size,
                            const pool_config_t *POUND_RESTRICT config);

/// Returns `pool` to its freshly-initialised state, invalidating all payloads.
///
/// Every outstanding pointer becomes dangling. Intended for restoring a saved
/// guest state, where the guest's whole heap is discarded as a unit. Logs and
/// ignores a NULL pool rather than aborting.
void pool_allocator_reset(pool_allocator_t *POUND_RESTRICT pool);

/// Releases the pool's mutex and zeroes the structure.
///
/// Does not free the arena, which the caller owns, and does not require that
/// every payload has been freed; use `pool_allocator_leak_bytes` first if that
/// matters. Logs and ignores a NULL pool.
void pool_allocator_destroy(pool_allocator_t *POUND_RESTRICT pool);

/// Allocates a block able to hold `bytes` usable bytes.
///
/// The returned payload is aligned to the pool's `alignment`; requesting a
/// stricter alignment than that fails rather than returning a misaligned
/// pointer. `bytes` of zero selects the smallest class rather than succeeding
/// with a degenerate zero-length block, matching `malloc(0)` being a distinct
/// case at the call site.
///
/// Returns NULL, and logs, on invalid arguments, on an alignment the pool was
/// not configured for, or when the arena is exhausted.
void *pool_allocator_alloc(pool_allocator_t *POUND_RESTRICT pool, size_t bytes, size_t alignment);

/// Returns a block to its class free list.
///
/// Validates the block's magic, class index and arena membership before
/// touching it, so a double free, a free of a pointer from another allocator, or
/// a free of wild memory is reported and ignored rather than corrupting the
/// free list. Such a failure is logged at `LOG_LEVEL_ERROR`; the function
/// returns without modifying the pool.
void pool_allocator_free(pool_allocator_t *POUND_RESTRICT pool, void *POUND_RESTRICT payload);

/// Returns the usable bytes at `payload`, or 0 if `payload` was not produced by
/// this pool.
size_t pool_allocator_get_usable_size(const pool_allocator_t *POUND_RESTRICT pool,
                                      const void *POUND_RESTRICT       payload);

/// Copies a consistent snapshot of the counters into `out`.
error_t pool_allocator_get_stats(const pool_allocator_t *POUND_RESTRICT pool,
                                 pool_stats_t *POUND_RESTRICT          out);

/// Returns live payload bytes that were never freed, for leak reporting.
size_t pool_allocator_leak_bytes(const pool_allocator_t *POUND_RESTRICT pool);

/// Enables or disables poisoning of allocated and freed blocks.
///
/// Takes effect on the next allocation or free; blocks already live keep
/// whatever bytes they had. Intended to be enabled at start-up in debug builds.
void pool_allocator_set_poison(pool_allocator_t *POUND_RESTRICT pool, bool enabled);

/// Returns the index of the smallest class whose payload can hold `bytes`, or
/// `class_count` when no class can.
///
/// Requires `pool->class_count` to already be populated, so it is only
/// meaningful between `init` and `destroy`; exposed for tests and for the GUI
/// panel that renders the class table.
size_t pool_allocator_class_for_size(const pool_allocator_t *POUND_RESTRICT pool, size_t bytes);

#endif // POUND_POOL_ALLOCATOR_H

/*** end of file ***/
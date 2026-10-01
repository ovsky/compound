//! Slab allocator for guest object caches, modelled on Horizon OS and SLUB.
//!
//! Where `pool_allocator` hands out a fixed number of interchangeable blocks per
//! size class, a slab allocator serves *object caches*: a named type with a
//! known size and a known lifetime, such as a process handle, a page descriptor
//! or a file object. That changes three things.
//!
//! - **Objects may be freed out of order.** A guest allocating and releasing
//!   interleaved objects cannot be served by a bump-then-never-reuse arena, so
//!   each slab keeps a free list threaded through its own objects.
//! - **Slabs must be reclaimable.** A slab whose objects are *all* free can be
//!   returned, because a long-running guest that allocates a burst of a given
//!   type must not pin that memory forever.
//! - **Slabs are the unit of host allocation.** One carve per slab keeps the
//!   guest's per-object cost to a header read, and lets a slab be released
//!   without disturbing its neighbours.
//!
//! # Why not just use `pool_allocator` for this
//!
//! The pool allocator is a fine fixed-block allocator and is what Horizon-style
//! *size-class* pools want. It cannot release partially-used slabs, because it
//! carved the arena once and never moves a block, and it carries no notion of a
//! named type. Both matter for object caches, so this is a separate structure
//! rather than a configuration flag.
//!
//! # Per-object metadata
//!
//! Every object is preceded by a 16-byte header carrying its owning slab, a
//! live/free magic, and the index of the next free object in its slab.
//!
//! SLUB avoids this by resolving a pointer to its slab through the kernel's page
//! tables and keeping liveness implicit in the free-list transition. Neither half
//! of that is available to a userspace emulator: Pound has no guest page tables
//! to consult, and an implicit liveness bit can only be recovered by walking the
//! free list, which is linear.
//!
//! A header costs 16 bytes per object -- zero slack at 16-byte alignment, which
//! is the common case -- and buys three things that matter more: `free` resolves
//! its slab from the header instead of scanning, liveness is a single comparison
//! rather than a free-list walk, and a pointer this allocator never produced is
//! rejected on a magic check instead of being followed.
//!
//! The free list is threaded by 32-bit *index* rather than by pointer, so it
//! needs no side array. A slab's object count is bounded by
//! `SLAB_ALLOCATOR_MAX_SLAB_BYTES / SLAB_ALLOCATOR_MIN_OBJECT_SIZE`, which is far
//! below `UINT32_MAX`, so an index cannot overflow.
//!
//! # Releasing slabs
//!
//! The arena is a stack: each carve takes the bytes at the cursor and pushes the
//! cursor forward, so slab address ranges tile the arena with no gaps.
//!
//! Releasing a slab means rewinding the cursor over it, and that is only sound for
//! slabs at the *very end* -- a slab with live neighbours behind it cannot be
//! reclaimed without fragmenting the arena into pieces no later carve would fit.
//! `slab_allocator_trim` therefore releases the longest run of fully-free trailing
//! slabs and stops at the first slab that still has a live object. That is what
//! makes a burst of allocations followed by a quiet period give its memory back.
//!
//! The arena must be sized for the peak *simultaneous* slab count, and the cursor
//! only ever moves forward except through `trim` and `cache_destroy`.
//!
//! # Thread safety
//!
//! Every entry point takes the allocator's mutex, including the read-only
//! queries, so a snapshot is self-consistent. One lock for the whole allocator
//! rather than one per cache keeps the structure simple, and a guest has a
//! handful of caches.

#ifndef POUND_SLAB_ALLOCATOR_H
#define POUND_SLAB_ALLOCATOR_H

#include "attributes.h"
#include "errors.h"
#include "sync/mutex.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Fills freed and freshly allocated objects so use-after-free is detectable.
/// Matches `POOL_ALLOCATOR_POISON_BYTE` so one notion of "poisoned" covers both
/// allocators.
#define SLAB_ALLOCATOR_POISON_BYTE 0xDD

/// Upper bound on concurrently registered object caches.
///
/// A guest has a bounded, enumerable set of object types, so a fixed table keeps
/// allocation out of the slab allocator itself and makes lookup a scan of a
/// small array. The real bound is the cache count; this is only a defensive cap.
#define SLAB_ALLOCATOR_MAX_CACHES 64

/// Smallest object size a cache may declare.
///
/// One byte is not usable: the header is pointer-aligned, so anything smaller
/// could not be carved without overlapping the next object's header.
#define SLAB_ALLOCATOR_MIN_OBJECT_SIZE 24U

/// Default bytes per slab when `slab_config_t::slab_bytes` is zero.
#define SLAB_ALLOCATOR_DEFAULT_SLAB_BYTES (64U * 1024U)

/// Largest slab size a cache may request.
///
/// Bounded so that the largest possible object count, and therefore every 32-bit
/// free-list index, stays comfortably inside `UINT32_MAX`.
#define SLAB_ALLOCATOR_MAX_SLAB_BYTES (16U * 1024U * 1024U)

/// Default alignment for objects when `slab_config_t::alignment` is zero.
#define SLAB_ALLOCATOR_DEFAULT_ALIGNMENT 16

typedef struct slab_cache slab_cache_t;
typedef struct slab       slab_t;

/// Construction parameters for the allocator itself.
///
/// Distinct from `slab_config_t`, which describes one object cache. The two are
/// kept apart because an allocator's construction has nothing to do with any
/// particular object type, and reusing one type for both would invite passing a
/// cache's parameters where the allocator's were meant.
typedef struct
{
    /// Poison freed and freshly allocated objects. Default `false`, because the
    /// cost is a full object write on every allocation and it is only wanted in
    /// debug runs and when reproducing guest bugs.
    bool poison;

    /// Absorbs the struct's tail padding explicitly, following the convention
    /// already used by `pool_config_t`.
    char pad[7];
} slab_allocator_config_t;

/// Construction parameters for one object cache.
typedef struct
{
    /// Caller-facing name, for logs and the GUI panel. Need not be unique; two
    /// caches may share a name if the caller prefers.
    const char *name;

    /// Object payload size, at least `SLAB_ALLOCATOR_MIN_OBJECT_SIZE`.
    size_t object_size;

    /// Payload alignment. Must be a power of two. Zero takes
    /// `SLAB_ALLOCATOR_DEFAULT_ALIGNMENT`.
    size_t alignment;

    /// Bytes per slab. Zero takes `SLAB_ALLOCATOR_DEFAULT_SLAB_BYTES`. Must not
    /// exceed `SLAB_ALLOCATOR_MAX_SLAB_BYTES`.
    ///
    /// A slab is carved to hold as many objects as fit in the arena remaining at
    /// the moment of the carve, capped by this value, so the last slab of a cache
    /// is typically smaller than the rest. That is deliberate: refusing to carve
    /// because the remainder is short would strand the arena.
    size_t slab_bytes;

    /// Slabs to reserve on the *first* allocation of this cache. Later growth
    /// takes one slab at a time, so a long-running guest does not over-reserve.
    /// Zero takes 1.
    size_t slab_count;
} slab_config_t;

/// Identity of a registered cache, as returned by `slab_allocator_cache_create`.
///
/// A plain integer rather than a pointer, so it survives
/// `slab_allocator_reset` and can be stored in a save state or compared against a
/// cached value without dereferencing anything.
typedef uint32_t slab_cache_id_t;

/// Value returned for "no such cache". Never a valid id, because ids are handed
/// out one-based so a zeroed table means "empty".
#define SLAB_CACHE_ID_NONE ((slab_cache_id_t)0)

/// Snapshot of allocator activity. Every field is mutually consistent because
/// `slab_allocator_get_stats` samples under the allocator lock.
typedef struct
{
    /// Registered caches.
    size_t cache_count;

    /// Slabs currently held across every cache, in use or partially used.
    size_t slab_count;

    /// Objects carved across every cache, live and free.
    size_t object_count;

    /// Objects currently handed out.
    size_t in_use;

    /// Host bytes committed to slabs, including headers and the alignment slack
    /// before each object region. Falls when `slab_allocator_trim` rewinds the
    /// arena over fully-free trailing slabs.
    size_t bytes_committed;

    /// Bytes of the arena consumed so far, committed or not.
    size_t arena_committed;

    /// Allocation requests refused because the arena could not hold another slab.
    uint64_t alloc_failures;

    /// Caches whose object type has failed to grow at least once. See
    /// `slab_allocator_cache_is_starved`.
    size_t starved_count;
} slab_stats_t;

typedef struct slab_allocator
{
    /// Host arena. Owned by the caller; the slab allocator never frees it, only
    /// carves slabs out of it.
    uint8_t *arena;

    /// Bytes available at `arena`.
    size_t arena_size;

    /// Offset of the next uncarved byte.
    size_t bump;

    slab_cache_t *caches[SLAB_ALLOCATOR_MAX_CACHES];
    size_t        cache_count;

    size_t   slab_count;
    size_t   object_count;
    size_t   in_use;
    size_t   bytes_committed;
    uint64_t alloc_failures;

    /// Objects poisoned on allocation and free.
    bool    poison_enabled;
    uint8_t poison_byte;

    /// Aligns the mutex on the next eight-byte boundary without relying on the
    /// compiler inserting a hole.
    char pad[6];

    /// Guards every field above.
    mutex_t lock;
} slab_allocator_t;

/// Initialises `allocator` over `arena`.
///
/// `config` may be NULL, in which case poisoning is off.
///
/// The arena is carved bump-style, so `arena` must be sized for the peak
/// *simultaneous* slab count rather than the peak live object count.
///
/// Returns `POUND_SUCCESS`, `POUND_ERROR_INVALID_ARGUMENT` for a NULL allocator
/// or arena or a zero arena size, or `POUND_ERROR_MEMORY_ALIGNMENT` when the
/// arena's address is not a multiple of `SLAB_ALLOCATOR_DEFAULT_ALIGNMENT`.
error_t slab_allocator_init(slab_allocator_t *POUND_RESTRICT         allocator,
                            void *POUND_RESTRICT                   arena,
                            size_t                                 arena_size,
                            const slab_allocator_config_t *POUND_RESTRICT config);

/// Drops every slab and every cache, returning the allocator to its
/// freshly-initialised state.
///
/// Every outstanding object pointer becomes dangling. Logs and ignores a NULL or
/// uninitialised allocator.
void slab_allocator_reset(slab_allocator_t *POUND_RESTRICT allocator);

/// Releases the allocator's mutex and zeroes the structure.
///
/// Does not free the arena, which the caller owns, and does not require that
/// every object has been freed. Logs a warning naming the live count when one
/// remains. Logs and ignores a NULL allocator.
void slab_allocator_destroy(slab_allocator_t *POUND_RESTRICT allocator);

/// Registers an object cache of the type described by `config`.
///
/// `config->name` must be non-NULL and non-empty, `config->object_size` at least
/// `SLAB_ALLOCATOR_MIN_OBJECT_SIZE`, `config->alignment` a power of two, and
/// `config->slab_bytes` within range. Zeroed `alignment`, `slab_bytes` and
/// `slab_count` take their defaults.
///
/// No slabs are reserved here; the first `slab_allocator_alloc` grows the cache.
/// That keeps registration cheap enough to call while loading a guest, and makes
/// a cache that is created and never used cost no arena.
///
/// Returns the new cache's id, or `SLAB_CACHE_ID_NONE` after logging the reason:
/// invalid arguments, an unsupported alignment or slab size, or the cache table
/// being full.
slab_cache_id_t slab_allocator_cache_create(slab_allocator_t *POUND_RESTRICT allocator,
                                            const slab_config_t *POUND_RESTRICT config);

/// Releases a cache and every slab backing it.
///
/// Every object still live is invalidated. Slabs that sit at the end of the arena
/// rewind it, so destroying a cache that grew last gives its memory back;
/// slabs with live neighbours behind them cannot be rewound and their bytes stay
/// committed until the trailing ones are released.
///
/// Ids are renumbered: the cache table is compacted, so a cached id greater than
/// `cache_id` now refers to a different cache. A caller holding ids across a
/// `cache_destroy` must re-read them, which is why the id is a plain integer
/// rather than something that survives renumbering by construction.
///
/// Logs and ignores `SLAB_CACHE_ID_NONE` and unknown ids rather than releasing
/// something twice.
void slab_allocator_cache_destroy(slab_allocator_t *POUND_RESTRICT allocator,
                                  slab_cache_id_t cache_id);

/// Allocates one object of type `cache_id`.
///
/// The returned pointer is aligned to the cache's alignment and its
/// `object_size` bytes are poisoned when poisoning is enabled. Growing the cache
/// reserves `slab_count` slabs on the first allocation and one per allocation
/// thereafter, so a burst of objects does not request a slab per object.
///
/// Returns NULL, and logs, when `cache_id` is unknown or the arena cannot hold
/// another slab. Out-of-order frees are supported.
void *slab_allocator_alloc(slab_allocator_t *POUND_RESTRICT allocator, slab_cache_id_t cache_id);

/// Returns an object to its slab's free list.
///
/// Validates that `object` was carved by `allocator` before touching anything, so
/// a double free, a foreign pointer, or a stack address is reported and ignored
/// rather than corrupting a free list. Validation walks the allocator's own slab
/// list rather than following the header's own slab pointer, so a garbage pointer
/// is never dereferenced. Such a failure is logged at `LOG_LEVEL_ERROR` and the
/// allocator is left unchanged.
///
/// A NULL `object` is a no-op, matching `free(NULL)`.
void slab_allocator_free(slab_allocator_t *POUND_RESTRICT allocator, void *POUND_RESTRICT object);

/// Returns true when `object` is a live object carved by `allocator`.
///
/// The intended use is a debug assertion on a guest pointer: it resolves the
/// owning slab from the allocator's own list, bounds-checks the object against
/// it, and then reads the object's own magic. Any step failing means the pointer
/// is not a live object of this allocator, and returns false rather than
/// reporting why.
bool slab_allocator_object_is_live(const slab_allocator_t *POUND_RESTRICT allocator,
                                   const void *POUND_RESTRICT       object);

/// Returns the usable bytes at `object`, or 0 if `object` was not carved by
/// `allocator`.
///
/// Usable bytes are the cache's `object_size`, not the slack up to the stride;
/// the slack exists to keep the next object aligned and is not usable.
size_t slab_allocator_get_usable_size(const slab_allocator_t *POUND_RESTRICT allocator,
                                      const void *POUND_RESTRICT       object);

/// Rewinds the arena over trailing slabs whose objects are all free.
///
/// Only trailing slabs are released, because the arena is a stack and a slab with
/// live neighbours behind it cannot be reclaimed without fragmenting it. The walk
/// stops at the first trailing slab that still holds a live object.
///
/// The allocator does not trim automatically, because a caller that frees and
/// immediately reallocates would otherwise thrash. A burst followed by a quiet
/// period wants this called between them.
///
/// Pass `SLAB_CACHE_ID_NONE` to consider every cache. Returns the number of slabs
/// released, or 0 when there is nothing trailing to release. Logs and returns 0 on
/// a NULL or uninitialised allocator.
size_t slab_allocator_trim(slab_allocator_t *POUND_RESTRICT allocator, slab_cache_id_t cache_id);

/// Writes a consistent snapshot of the counters into `out`.
error_t slab_allocator_get_stats(const slab_allocator_t *POUND_RESTRICT allocator,
                                 slab_stats_t *POUND_RESTRICT          out);

/// Copies a cache's immutable configuration into `out`.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` for a NULL argument and
/// `POUND_ERROR_NOT_INITIALIZED` for an unknown id. Lets the GUI memory panel
/// render the cache table without reaching into the allocator's internals.
error_t slab_allocator_cache_describe(const slab_allocator_t *POUND_RESTRICT allocator,
                                      slab_cache_id_t                     cache_id,
                                      slab_config_t *POUND_RESTRICT        out);

/// Returns the number of live objects in `cache_id`, or 0 for an unknown id.
size_t slab_allocator_cache_in_use(const slab_allocator_t *POUND_RESTRICT allocator,
                                   slab_cache_id_t                     cache_id);

/// Enables or disables poisoning of allocated and freed objects.
///
/// Takes effect on the next allocation or free; objects already live keep
/// whatever bytes they had.
void slab_allocator_set_poison(slab_allocator_t *POUND_RESTRICT allocator, bool enabled);

/// Returns the cache's name, or `""` for an unknown id. Never NULL.
const char *slab_allocator_cache_name(const slab_allocator_t *POUND_RESTRICT allocator,
                                      slab_cache_id_t cache_id);

/// Reports whether `cache_id`'s object type has failed to grow.
///
/// A cache is marked starved when the last growth attempt could not fit a slab.
/// Distinct from "out of objects right now", which is normal for any full cache.
/// Returns false for an unknown id.
bool slab_allocator_cache_is_starved(const slab_allocator_t *POUND_RESTRICT allocator,
                                     slab_cache_id_t cache_id);

#endif // POUND_SLAB_ALLOCATOR_H

/*** end of file ***/
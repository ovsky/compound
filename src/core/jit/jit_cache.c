#include "jit_cache.h"

#include "log.h"
#include "memory/memory.h"
#include "platform.h"
#include <string.h>

#if POUND_PLATFORM_WINDOWS
#include <windows.h>
#elif POUND_PLATFORM_APPLE
#include <pthread.h>
#include <sys/mman.h>
#elif POUND_PLATFORM_POSIX
#include <sys/mman.h>
#include <unistd.h>
#endif

/// Magic held by a live block descriptor.
///
/// Non-zero so that a zeroed descriptor is detectable, and distinct from any
/// pointer value a caller could plausibly produce.
#define JIT_BLOCK_MAGIC 0x4A544250u /* 'PB'TJ: 'PBTJ' little-endian */

// -----------------------------------------------------------------------------
// Platform memory primitives
// -----------------------------------------------------------------------------
//
// Kept in one place so the allocator above them never sees a platform call, and
// so the Windows aliasing constraint documented in the header has exactly one
// implementation to be true of.

/// Returns the host page size, or `0` if the platform would not say.
static size_t
detect_page_size(void)
{
#if POUND_PLATFORM_WINDOWS
    SYSTEM_INFO info;

    ZeroMemory(&info, sizeof(info));
    GetSystemInfo(&info);

    if (0U == info.dwPageSize)
    {
        return 0U;
    }

    return (size_t)info.dwPageSize;
#elif POUND_PLATFORM_POSIX
    const long page_size = sysconf(_SC_PAGESIZE);

    if (page_size <= 0)
    {
        return 0U;
    }

    return (size_t)page_size;
#else
    return 0U;
#endif
}

/// Reserves and commits `bytes` of writable host memory.
///
/// Returns NULL on failure. A partially satisfied request is never returned: the
/// whole range is either committed or not touched at all.
static void *
chunk_reserve(const size_t bytes)
{
#if POUND_PLATFORM_WINDOWS
    // MEM_RESERVE|MEM_COMMIT in one call, rather than a large reservation that is
    // committed lazily. The reason is in the header: a reservation is charged
    // against the commit limit the moment it is committed, so reserving the
    // cache's whole ceiling up front would commit it too whether or not a game
    // ever JIT-compiled anything.
    void *const base = VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

    return base;
#elif POUND_PLATFORM_APPLE
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;

#    if defined(MAP_JIT)
    flags |= MAP_JIT;
#    endif

    void *const base = mmap(NULL, bytes, PROT_READ | PROT_WRITE, flags, -1, 0);

    return (MAP_FAILED == base) ? NULL : base;
#elif POUND_PLATFORM_POSIX
    void *const base = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    return (MAP_FAILED == base) ? NULL : base;
#else
    POUND_UNUSED(bytes);
    return NULL;
#endif
}

/// Releases a chunk obtained from `chunk_reserve`.
static void
chunk_release(void *POUND_RESTRICT base, const size_t bytes)
{
    if (NULL == base)
    {
        return;
    }

#if POUND_PLATFORM_WINDOWS
    // MEM_RELEASE requires a zero size and gives back the whole reservation.
    POUND_UNUSED(bytes);
    (void)VirtualFree(base, 0, MEM_RELEASE);
#elif POUND_PLATFORM_POSIX
    (void)munmap(base, bytes);
#else
    POUND_UNUSED(bytes);
#endif
}

/// Flushes the host instruction cache over `base`, which must still be writable.
///
/// Deliberately called before the protection change rather than after, for two
/// reasons. The flush has to happen after the guest's machine code is written and
/// before the range stops being writable, and libgcc's `__clear_cache` is a
/// kernel call on the platforms Pound targets but is not contractually a
/// non-writing function -- calling it on a range that had just become read-only
/// would turn a cache-coherency bug into a fault.
static void
chunk_flush_icache(void *POUND_RESTRICT base, const size_t bytes)
{
#if POUND_PLATFORM_WINDOWS
    // Required on every architecture Windows runs Pound on, ARM64 included,
    // where the data and instruction caches are not coherent.
    if (0U == FlushInstructionCache(GetCurrentProcess(), base, bytes))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "FlushInstructionCache(%p, %zu) failed with %lu.",
                        base,
                        bytes,
                        (unsigned long)GetLastError());
    }
#elif POUND_PLATFORM_POSIX
    __builtin___clear_cache((char *)base, (char *)base + bytes);
#else
    POUND_UNUSED(base);
    POUND_UNUSED(bytes);
#endif
}

/// Flips a page-aligned range from read-write to read-execute.
///
/// Does not flush: the caller owns the ordering, because only the caller knows
/// when the guest's code has finished being written.
static error_t
chunk_make_rx(void *POUND_RESTRICT base, const size_t bytes)
{
#if POUND_PLATFORM_WINDOWS
    DWORD previous = 0U;

    if (0U == VirtualProtect(base, bytes, PAGE_EXECUTE_READ, &previous))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "VirtualProtect(%p, %zu, PAGE_EXECUTE_READ) failed with %lu.",
                        base,
                        bytes,
                        (unsigned long)GetLastError());
        return POUND_ERROR_MEMORY_FAULT;
    }

    return POUND_SUCCESS;
#elif POUND_PLATFORM_APPLE
#    if defined(MAP_JIT)
    if (0 != pthread_jit_write_protect_np(true))
    {
        POUND_LOG_ERROR(&thread_logger, "pthread_jit_write_protect_np(true) failed for %p.", base);
        return POUND_ERROR_MEMORY_FAULT;
    }

    return POUND_SUCCESS;
#    else
    // Without MAP_JIT the platform exposes no write-protection toggle, so mprotect
    // is the only option. This is the pre-10.15 path, and it is documented as
    // unverified because the project has no macOS CI lane.
    if (0 != mprotect(base, bytes, PROT_READ | PROT_EXEC))
    {
        POUND_LOG_ERROR(&thread_logger, "mprotect(%p, %zu, PROT_READ|PROT_EXEC) failed.", base, bytes);
        return POUND_ERROR_MEMORY_FAULT;
    }

    return POUND_SUCCESS;
#    endif
#elif POUND_PLATFORM_POSIX
    if (0 != mprotect(base, bytes, PROT_READ | PROT_EXEC))
    {
        POUND_LOG_ERROR(&thread_logger, "mprotect(%p, %zu, PROT_READ|PROT_EXEC) failed.", base, bytes);
        return POUND_ERROR_MEMORY_FAULT;
    }

    return POUND_SUCCESS;
#else
    POUND_UNUSED(base);
    POUND_UNUSED(bytes);
    return POUND_ERROR_MEMORY_FAULT;
#endif
}

/// Flips a page-aligned range from read-execute back to read-write.
static error_t
chunk_make_rw(void *POUND_RESTRICT base, const size_t bytes)
{
#if POUND_PLATFORM_WINDOWS
    DWORD previous = 0U;

    if (0U == VirtualProtect(base, bytes, PAGE_READWRITE, &previous))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "VirtualProtect(%p, %zu, PAGE_READWRITE) failed with %lu.",
                        base,
                        bytes,
                        (unsigned long)GetLastError());
        return POUND_ERROR_MEMORY_FAULT;
    }

    return POUND_SUCCESS;
#elif POUND_PLATFORM_APPLE
#    if defined(MAP_JIT)
    if (0 != pthread_jit_write_protect_np(false))
    {
        POUND_LOG_ERROR(&thread_logger, "pthread_jit_write_protect_np(false) failed for %p.", base);
        return POUND_ERROR_MEMORY_FAULT;
    }

    return POUND_SUCCESS;
#    else
    if (0 != mprotect(base, bytes, PROT_READ | PROT_WRITE))
    {
        POUND_LOG_ERROR(&thread_logger, "mprotect(%p, %zu, PROT_READ|PROT_WRITE) failed.", base, bytes);
        return POUND_ERROR_MEMORY_FAULT;
    }

    return POUND_SUCCESS;
#    endif
#elif POUND_PLATFORM_POSIX
    if (0 != mprotect(base, bytes, PROT_READ | PROT_WRITE))
    {
        POUND_LOG_ERROR(&thread_logger, "mprotect(%p, %zu, PROT_READ|PROT_WRITE) failed.", base, bytes);
        return POUND_ERROR_MEMORY_FAULT;
    }

    return POUND_SUCCESS;
#else
    POUND_UNUSED(base);
    POUND_UNUSED(bytes);
    return POUND_ERROR_MEMORY_FAULT;
#endif
}

// -----------------------------------------------------------------------------
// Internals
// -----------------------------------------------------------------------------

/// A free extent within a chunk, in chunk-relative offsets.
///
/// Offsets rather than pointers, so a chunk's free list stays meaningful
/// independently of where the chunk itself was placed.
typedef struct jit_hole
{
    /// Offset of the first free byte.
    size_t offset;

    /// Number of contiguous free bytes.
    size_t size;
} jit_hole_t;

/// One live allocation.
///
/// The descriptor lives in host memory rather than immediately before the
/// payload, which is what the pool and slab allocators do instead. Here that
/// would be actively wrong: the payload is executable half the time, and the
/// descriptor would have to be flipped along with it or become unreadable exactly
/// when the JIT needs to patch a block in place.
typedef struct jit_block
{
    /// `JIT_BLOCK_MAGIC` while this descriptor is live.
    uint32_t magic;

    /// Index of the owning chunk in `jit_cache::chunks`.
    uint32_t chunk_index;

    /// `jit_block_kind_t`.
    uint32_t kind;

    /// `jit_block_state_t`.
    uint32_t state;

    /// Offset of the block within its chunk.
    size_t offset;

    /// Bytes available from the block's address.
    size_t capacity;

    /// Bytes the caller asked for.
    size_t requested;

    /// Intrusive list of the chunk's live blocks.
    struct jit_block *next_in_chunk;
} jit_block_t;

/// A reserved, committed region the cache carves blocks out of.
struct jit_chunk
{
    struct jit_chunk *next;

    /// First byte of the reserved region.
    uint8_t *base;

    /// Bytes reserved.
    size_t bytes;

    /// High-water mark of the bump cursor. Every byte below this is either live or
    /// in the free list; every byte above it has never been handed out.
    ///
    /// Advanced for *every* tail carve, not just executable ones. Advancing it
    /// selectively would let two data blocks carved from a tail resolve to the
    /// same offset, and neither would appear in the free list to notice.
    size_t used;

    /// Free extents, sorted by `offset` ascending, non-overlapping and coalesced.
    /// Reuse comes from here.
    jit_hole_t *holes;

    /// Number of entries in `holes`.
    size_t hole_count;

    /// Entries `holes` can hold.
    size_t hole_capacity;

    /// Live blocks carved from this chunk.
    jit_block_t *live;

    /// Number of live blocks. A chunk with none is a candidate for unmapping, if
    /// it is also trailing.
    size_t live_blocks;

    /// Bytes handed out from this chunk.
    size_t live_bytes;
};

/// Returns `true` when `alignment` is a non-zero power of two.
static bool
is_power_of_two(const size_t alignment)
{
    return (0U != alignment) && (0U == (alignment & (alignment - 1U)));
}

/// Rounds `value` up to the next multiple of `alignment`.
///
/// Returns false when the rounding would overflow, rather than wrapping to a small
/// value and handing back a block that overlaps its neighbours.
static bool
align_up_checked(const size_t value, const size_t alignment, size_t *POUND_RESTRICT out)
{
    const size_t mask = alignment - 1U;

    if (POUND_UNLIKELY(value > (SIZE_MAX - mask)))
    {
        return false;
    }

    *out = (value + mask) & ~mask;
    return true;
}

/// Returns `chunk`'s index in `cache->chunks`.
///
/// Free, because block descriptors record it for diagnostics and a chunk's own
/// position never changes except by being unmapped, which invalidates its blocks
/// anyway.
static size_t
chunk_index_of(const jit_cache_t *POUND_RESTRICT cache, const jit_chunk_t *POUND_RESTRICT chunk)
{
    size_t index = 0U;

    for (const jit_chunk_t *walk = cache->chunks; NULL != walk; walk = walk->next, index++)
    {
        if (walk == chunk)
        {
            return index;
        }
    }

    return 0U;
}

/// Fills a freshly reserved block with the poison pattern.
///
/// Applied at allocation only. Poisoning on free would mean a protection flip on
/// every release, which is the single most expensive thing this cache exists to
/// avoid, and the hazard poisoning is meant to catch is code executed before it was
/// written -- which is decided here.
static void
poison_range(const jit_cache_t *POUND_RESTRICT cache, void *POUND_RESTRICT block, const size_t bytes)
{
    if (cache->poison)
    {
        memset(block, (int)JIT_CACHE_POISON_BYTE, bytes);
    }
}

/// Charges host allocations made below to the code cache's own bucket.
///
/// The memory subsystem attributes every host allocation to whichever bucket the
/// calling thread has selected, and the selected bucket is thread state that outlives
/// any single call. So a bracket is only correct if *both* ends of it are inside: an
/// allocation made here and released later, from a different call, would otherwise be
/// credited to `MEMORY_BUCKET_JIT_RECOMPILER` and debited from whatever the caller
/// happened to have active. The per-bucket totals would then drift upward for the life
/// of the process and a reporting UI would show the JIT bucket growing without bound
/// after a cache that had released everything.
///
/// That is also why these two are used as a pair rather than a single scoped helper:
/// the release sites are in three different functions, and a scope that could not cross
/// a call boundary would be a scope that could not be used to fix the problem.
static memory_bucket_type_t
bucket_enter(void)
{
    return memory_subsystem_set_bucket(MEMORY_BUCKET_JIT_RECOMPILER);
}

/// Restores the bucket that `bucket_enter` displaced.
static void
bucket_leave(const memory_bucket_type_t previous)
{
    (void)memory_subsystem_set_bucket(previous);
}

/// Resizes a chunk's free-extent array to hold `capacity` entries.
///
/// Returns `POUND_SUCCESS`, or a typed error with a log record. On failure the
/// existing array is left intact and usable.
static error_t
holes_reserve(jit_chunk_t *POUND_RESTRICT chunk, const size_t capacity)
{
    if (capacity <= chunk->hole_capacity)
    {
        return POUND_SUCCESS;
    }

    if (POUND_UNLIKELY(capacity > (SIZE_MAX / sizeof(jit_hole_t))))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Chunk %p cannot hold a %zu-entry free-extent array.",
                        (const void *)chunk,
                        capacity);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    // Charged to the cache's own bucket rather than to whatever the calling thread
    // happens to have active, so a guest UI allocation cannot make the code cache look
    // larger or smaller than it is. The release of the old array is inside the same
    // bracket, which is what keeps the two ends in the same bucket.
    const memory_bucket_type_t previous_bucket = bucket_enter();

    jit_hole_t *const grown = memory_subsystem_allocate(_Alignof(jit_hole_t),
                                                        sizeof(*grown) * capacity);

    if (NULL != grown)
    {
        if (chunk->hole_count > 0U)
        {
            memcpy(grown, chunk->holes, sizeof(*grown) * chunk->hole_count);
        }

        if (NULL != chunk->holes)
        {
            memory_subsystem_free(chunk->holes);
        }

        chunk->holes         = grown;
        chunk->hole_capacity = capacity;
    }

    bucket_leave(previous_bucket);

    if (POUND_UNLIKELY(NULL == grown))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Chunk %p cannot grow its free-extent array to %zu entries.",
                        (const void *)chunk,
                        capacity);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    return POUND_SUCCESS;
}

/// Reserves and registers a new chunk at the head of `cache`.
///
/// `bytes` is the chunk's own size rather than `cache->chunk_bytes`, because a request
/// larger than one ordinary chunk is given a chunk sized to it. See the oversized
/// carve in `reserve_block`. Every chunk is a multiple of the page size, so an
/// oversized chunk reserves the same way an ordinary one does.
///
/// Returns `POUND_SUCCESS`, or a typed error with a log record. The chunk list is
/// left untouched on failure.
static error_t
chunk_create(jit_cache_t *POUND_RESTRICT   cache,
             const size_t                 bytes,
             jit_chunk_t **POUND_RESTRICT out)
{
    // Growing past the ceiling is refused rather than honoured. A ceiling the
    // cache quietly ignores is worse than no ceiling, because the caller that set
    // it is bounding the emulator's footprint. Checked with a subtraction rather than
    // a sum, so an oversized request cannot wrap the sum into a small number and
    // reserve past the ceiling it was just tested against.
    if (POUND_UNLIKELY(bytes > (cache->max_bytes - cache->reserved_bytes)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing to reserve another %zu bytes: %zu of a %zu-byte ceiling are "
                        "already committed.",
                        bytes,
                        cache->reserved_bytes,
                        cache->max_bytes);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    uint8_t *const base = chunk_reserve(bytes);

    if (POUND_UNLIKELY(NULL == base))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Reserving a %zu-byte code cache chunk failed.",
                        bytes);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    // Bracketed so the descriptor is charged to, and later debited from, the JIT
    // bucket. See `bucket_enter`.
    const memory_bucket_type_t previous_bucket = bucket_enter();

    jit_chunk_t *const chunk = memory_subsystem_allocate(_Alignof(jit_chunk_t), sizeof(*chunk));

    if (POUND_UNLIKELY(NULL == chunk))
    {
        chunk_release(base, bytes);
        POUND_LOG_ERROR(&thread_logger, "Allocating a chunk descriptor failed.");
        bucket_leave(previous_bucket);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    bucket_leave(previous_bucket);

    memset(chunk, 0, sizeof(*chunk));

    chunk->base  = base;
    chunk->bytes = bytes;
    chunk->next  = cache->chunks;

    cache->chunks = chunk;
    cache->chunk_count++;
    cache->reserved_bytes += bytes;

    POUND_LOG_DEBUG(&thread_logger,
                    "Reserved a %zu-byte code cache chunk at %p (%zu chunks, %zu of %zu bytes).",
                    bytes,
                    (const void *)base,
                    cache->chunk_count,
                    cache->reserved_bytes,
                    cache->max_bytes);

    *out = chunk;
    return POUND_SUCCESS;
}

/// Inserts a free extent into `chunk`, coalescing it with both neighbours.
///
/// The caller must hold `lock`.
static error_t
holes_insert(jit_chunk_t *POUND_RESTRICT chunk, const size_t offset, const size_t size)
{
    // The list is sorted by offset, so the insertion point is the first hole that
    // starts above `offset`.
    size_t index = 0U;

    while ((index < chunk->hole_count) && (chunk->holes[index].offset < offset))
    {
        index++;
    }

    // Doubling rather than incrementing because a chunk whose blocks are all freed
    // has one entry per block, and growing by one would make releasing a full
    // chunk quadratic in its block count.
    const size_t needed = chunk->hole_count + 1U;
    const error_t reserved = holes_reserve(chunk,
                                            (needed > chunk->hole_capacity) ? (needed * 2U)
                                                                         : chunk->hole_capacity);

    if (POUND_SUCCESS != reserved)
    {
        return reserved;
    }

    if ((index > 0U)
        && ((chunk->holes[index - 1U].offset + chunk->holes[index - 1U].size) == offset))
    {
        // The new extent abuts the one before it, so absorb it instead of adding a
        // second entry. `index` then names the merged entry.
        chunk->holes[index - 1U].size += size;
        index--;
    }
    else
    {
        if (chunk->hole_count > index)
        {
            memmove(&chunk->holes[index + 1U],
                    &chunk->holes[index],
                    sizeof(chunk->holes[0]) * (chunk->hole_count - index));
        }

        chunk->hole_count++;
        chunk->holes[index].offset = offset;
        chunk->holes[index].size   = size;
    }

    // And now absorb the one after it, if the two now touch. This has to come
    // second: merging backward can bring the entry up against its successor, which
    // merging backward alone would never notice.
    if (((index + 1U) < chunk->hole_count)
        && ((chunk->holes[index].offset + chunk->holes[index].size)
            == chunk->holes[index + 1U].offset))
    {
        chunk->holes[index].size += chunk->holes[index + 1U].size;

        memmove(&chunk->holes[index + 1U],
                &chunk->holes[index + 2U],
                sizeof(chunk->holes[0]) * (chunk->hole_count - index - 2U));
        chunk->hole_count--;
    }

    return POUND_SUCCESS;
}

/// Returns the offset of the first address at or above `offset` that satisfies
/// `alignment`.
///
/// The caller must have established that `offset` is inside a chunk and that
/// `alignment` is a power of two no larger than the chunk size, so the addition
/// cannot overflow.
static size_t
hole_aligned_offset(size_t offset, const size_t alignment)
{
    const size_t mask = alignment - 1U;

    if (0U == (offset & mask))
    {
        return offset;
    }

    return (offset + mask) & ~mask;
}

/// Returns the index of the first free extent in `chunk` that can hold `needed`
/// bytes *at `alignment`*, or `chunk->hole_count` if none can.
///
/// The alignment is not optional. An extent starting at an arbitrary offset cannot
/// be assumed to yield an aligned block, and for an executable block an unaligned
/// one is unusable, because flipping its protection would change the protection of
/// whatever shares those pages. So the leading bytes an extent would have to give up
/// to reach the first aligned address count against it.
///
/// The caller must hold `lock`.
static size_t
holes_find(const jit_chunk_t *POUND_RESTRICT chunk, const size_t needed, const size_t alignment)
{
    for (size_t i = 0U; i < chunk->hole_count; ++i)
    {
        const size_t offset  = chunk->holes[i].offset;
        const size_t aligned = hole_aligned_offset(offset, alignment);

        // The leading bytes an extent has to give up to reach an aligned address can
        // exceed the extent's own length -- a 64-byte extent at offset 64 is more than
        // a page away from the next aligned address. Subtracting then would wrap to a
        // value near SIZE_MAX and the extent would be judged able to hold anything, so
        // the comparison is made against zero instead of against a subtraction that is
        // only meaningful when the two are in range.
        if (aligned >= (offset + chunk->holes[i].size))
        {
            continue;
        }

        if ((chunk->holes[i].size - (aligned - offset)) >= needed)
        {
            return i;
        }
    }

    return chunk->hole_count;
}

/// Removes `needed` bytes from the extent at `index`, at `alignment`, splitting it
/// if either remainder is worth keeping.
///
/// `out_offset` and `out_capacity` receive where the block goes and how much it can
/// hold. Any leading bytes skipped to reach an aligned address are recorded as their
/// own extent, so the gap is still reusable and still counted.
///
/// The caller must hold `lock` and must have reserved room for one more extent entry
/// beyond `chunk->hole_count`, which is the most this can need.
static void
holes_take(jit_chunk_t *POUND_RESTRICT chunk,
           const size_t       index,
           const size_t       needed,
           const size_t       alignment,
           size_t *POUND_RESTRICT out_offset,
           size_t *POUND_RESTRICT out_capacity)
{
    const size_t start      = chunk->holes[index].offset;
    const size_t offset     = hole_aligned_offset(start, alignment);
    const size_t leading    = offset - start;
    const size_t usable     = chunk->holes[index].size - leading;
    const size_t trailing   = usable - needed;

    *out_offset = offset;

    if (trailing < JIT_CACHE_MIN_SPLIT_REMAINDER)
    {
        // Hand out the whole usable run rather than leave a tail too small to earn
        // its own bookkeeping entry. The caller learns the real size through
        // `jit_cache_usable_size`, so nothing is lost but rounding.
        *out_capacity = usable;

        memmove(&chunk->holes[index],
                &chunk->holes[index + 1U],
                sizeof(chunk->holes[0]) * (chunk->hole_count - index - 1U));
        chunk->hole_count--;
    }
    else
    {
        chunk->holes[index].offset = offset + needed;
        chunk->holes[index].size   = trailing;
        *out_capacity              = needed;
    }

    if (leading > 0U)
    {
        // Best effort: the array has room by the caller's contract, and a failure
        // here can only be a host out-of-memory. The bytes stay reserved either way.
        const error_t recorded = holes_insert(chunk, start, leading);

        if (POUND_SUCCESS != recorded)
        {
            POUND_LOG_ERROR(&thread_logger,
                            "The %zu leading bytes of a %zu-byte extent at offset %zu of chunk %p "
                            "could not be recorded, so they will not be reused.",
                            leading,
                            usable,
                            start,
                            (const void *)chunk);
        }
    }
}

/// Locates the chunk containing `address`.
///
/// Returns NULL if the address belongs to no chunk. The caller must hold `lock`.
static jit_chunk_t *
chunk_find(const jit_cache_t *POUND_RESTRICT cache, const uint8_t *POUND_RESTRICT address)
{
    // Newest first, because that is where an allocation would land anyway, and it
    // keeps the hot chunk's range checks ahead of the cold ones.
    jit_chunk_t *chunk = cache->chunks;

    while (NULL != chunk)
    {
        if ((address >= chunk->base) && (address < (chunk->base + chunk->bytes)))
        {
            return chunk;
        }

        chunk = chunk->next;
    }

    return NULL;
}

/// Locates the live block whose payload starts exactly at `address`.
///
/// An interior pointer deliberately resolves to nothing: the cache hands out block
/// bases, and rounding one down would release a neighbour instead. Neither output
/// pointer has to be non-NULL.
///
/// The caller must hold `lock`.
static jit_block_t *
block_find(const jit_cache_t *POUND_RESTRICT cache,
           const void *POUND_RESTRICT           address,
           jit_chunk_t **POUND_RESTRICT          out_chunk,
           jit_block_t **POUND_RESTRICT         out_previous)
{
    const uint8_t *const raw   = (const uint8_t *)(uintptr_t)address;
    jit_chunk_t  *const  chunk = chunk_find(cache, raw);

    if (NULL == chunk)
    {
        return NULL;
    }

    jit_block_t *previous = NULL;

    for (jit_block_t *block = chunk->live; NULL != block; block = block->next_in_chunk)
    {
        if ((raw == (chunk->base + block->offset)) && (JIT_BLOCK_MAGIC == block->magic))
        {
            if (NULL != out_chunk)
            {
                *out_chunk = chunk;
            }

            if (NULL != out_previous)
            {
                *out_previous = previous;
            }

            return block;
        }

        previous = block;
    }

    return NULL;
}

/// Computes the effective alignment for a block of `kind`.
///
/// An executable block owns whole pages, because flipping a sub-page range would
/// change the protection of whatever shares those pages. So its alignment is at
/// least the page size whatever the caller asked for.
static size_t
effective_alignment(const jit_cache_t *POUND_RESTRICT cache, const size_t alignment, const uint32_t kind)
{
    const size_t requested = (0U == alignment) ? cache->default_alignment : alignment;

    if (JIT_BLOCK_KIND_EXECUTABLE == (jit_block_kind_t)kind)
    {
        return (requested > cache->page_size) ? requested : cache->page_size;
    }

    return requested;
}

/// Reserves `size` bytes for one block and writes where it goes.
///
/// Three passes, in order: reuse cached free space, then extend the newest chunk's
/// tail, then create a chunk. Free space is consulted before the cursor because a
/// cache that grew first and reused second would pin every byte it ever allocated
/// until a whole chunk emptied.
///
/// The caller must hold `lock`.
static error_t
reserve_block(jit_cache_t *POUND_RESTRICT   cache,
              const uint32_t                 kind,
              const size_t                   size,
              const size_t                   alignment,
              jit_chunk_t **POUND_RESTRICT   out_chunk,
              size_t *POUND_RESTRICT         out_offset,
              size_t *POUND_RESTRICT         out_capacity)
{
    const size_t effective = effective_alignment(cache, alignment, kind);
    size_t       need      = 0U;

    // Assigned after `try_new_chunk`, which a `goto` reaches, and so it cannot be
    // initialised at the point of declaration. See the oversized carve there.
    size_t chunk_bytes = 0U;

    if (POUND_UNLIKELY(!align_up_checked(size, effective, &need)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing a %zu-byte block at %zu-byte alignment: the rounded size "
                        "overflows.",
                        size,
                        effective);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    for (jit_chunk_t *chunk = cache->chunks; NULL != chunk; chunk = chunk->next)
    {
        const size_t index = holes_find(chunk, need, effective);

        if (index >= chunk->hole_count)
        {
            continue;
        }

        // `holes_take` can record the bytes it skips to reach an aligned address, so
        // it needs room for one entry more than the list holds now. If that cannot be
        // reserved the extent is simply left alone -- the space is still there, the
        // cache just does not use *this* one and falls through to growing.
        if (POUND_SUCCESS != holes_reserve(chunk, chunk->hole_count + 1U))
        {
            continue;
        }

        holes_take(chunk, index, need, effective, out_offset, out_capacity);
        *out_chunk = chunk;
        return POUND_SUCCESS;
    }

    if (NULL != cache->chunks)
    {
        jit_chunk_t *const chunk  = cache->chunks;
        const size_t  aligned     = hole_aligned_offset(chunk->used, effective);

        // The aligned offset is tested against the chunk's size *before* the remaining
        // bytes are computed. Rounding up can leave the cursor within one alignment of
        // the end, in which case the rounded address is past the end of the chunk, and
        // `chunk->bytes - aligned` would wrap to a value near SIZE_MAX and make a
        // carve that runs off the chunk look like it fits.
        if ((aligned <= chunk->bytes) && ((chunk->bytes - aligned) >= need))
        {
            // Reaching an aligned address can leave a gap between the cursor and the
            // carve. It is recorded rather than abandoned, so a later request can use
            // it and the accounting stays exact: every byte of every chunk is either
            // live, free, or still uncarved.
            if (aligned > chunk->used)
            {
                if (POUND_SUCCESS != holes_reserve(chunk, chunk->hole_count + 1U))
                {
                    goto try_new_chunk;
                }

                if (POUND_SUCCESS != holes_insert(chunk, chunk->used, aligned - chunk->used))
                {
                    POUND_LOG_ERROR(&thread_logger,
                                    "The %zu-byte alignment gap at offset %zu of chunk %p could "
                                    "not be recorded, so it will not be reused.",
                                    aligned - chunk->used,
                                    chunk->used,
                                    (const void *)chunk);
                }
            }

            // Advancing the cursor here is what makes the carve exclusive. Failing to
            // do so would hand the same bytes to the next caller.
            chunk->used    = aligned + need;
            *out_offset    = aligned;
            *out_chunk     = chunk;
            *out_capacity  = need;
            return POUND_SUCCESS;
        }
    }

try_new_chunk:
    // A request bigger than one ordinary chunk is given a chunk of its own rather than
    // refused. The size of the request belongs to the caller, and a cache that can only
    // satisfy sizes it chose in advance is one the caller has to guess before it can run
    // at all.
    //
    // That is not hypothetical. Ballistic asks for a single 16 MiB executable buffer
    // when its engine starts, which is four times the default 4 MiB chunk, so with the
    // refusal in place `bal_engine_init` fails outright and the emulator has no JIT at
    // all -- a defect that only appears once something is running the real engine,
    // because nothing in the cache's own tests ever asks for more than a chunk.
    //
    // The oversized chunk is rounded up to the page size, because the reserve is
    // page-granular and a chunk ending mid-page would leave its last bytes unusable by
    // anything. The rounding is checked rather than assumed: `need` came from the
    // caller, and a wrapped size would reserve a chunk *smaller* than the block carved
    // out of it, which is a heap overflow rather than a refusal.
    chunk_bytes = cache->chunk_bytes;

    if (POUND_UNLIKELY(need > cache->chunk_bytes))
    {
        const size_t remainder = (need % cache->page_size);

        if (POUND_UNLIKELY(0U == remainder))
        {
            chunk_bytes = need;
        }
        else
        {
            const size_t padding = (cache->page_size - remainder);

            if (POUND_UNLIKELY(need > (SIZE_MAX - padding)))
            {
                POUND_LOG_ERROR(&thread_logger,
                                "Refusing a %zu-byte block at %zu-byte alignment: rounding it up "
                                "to a whole %zu-byte page would overflow the size type.",
                                size,
                                effective,
                                cache->page_size);
                return POUND_ERROR_ALLOCATION_FAILED;
            }

            chunk_bytes = (need + padding);
        }

        POUND_LOG_DEBUG(&thread_logger,
                        "A %zu-byte block needs more than one %zu-byte chunk, so it is getting a "
                        "%zu-byte chunk of its own.",
                        size,
                        cache->chunk_bytes,
                        chunk_bytes);
    }

    {
        jit_chunk_t *created = NULL;
        const error_t result = chunk_create(cache, chunk_bytes, &created);

        if (POUND_SUCCESS != result)
        {
            return result;
        }

        // A fresh chunk's tail begins at zero, which is aligned to any power of two
        // no larger than the chunk, so there is no leading gap to record.
        created->used   = need;
        *out_offset     = 0U;
        *out_chunk      = created;
        *out_capacity   = need;
        return POUND_SUCCESS;
    }
}

/// Records a live block for the bytes `reserve_block` just handed out.
///
/// Returns `POUND_SUCCESS`, or a typed error with a log record. On failure the
/// bytes are returned to the chunk's free list, because a descriptor allocation
/// that failed must not cost the extent forever.
static error_t
block_record(jit_cache_t *POUND_RESTRICT cache,
             jit_chunk_t *POUND_RESTRICT chunk,
             const uint32_t                   kind,
             const size_t                     offset,
             const size_t                     capacity,
             const size_t                     requested)
{
    const memory_bucket_type_t previous_bucket = bucket_enter();

    jit_block_t *const block = memory_subsystem_allocate(_Alignof(jit_block_t), sizeof(*block));

    bucket_leave(previous_bucket);

    if (POUND_UNLIKELY(NULL == block))
    {
        cache->alloc_failures++;

        const error_t returned = holes_insert(chunk, offset, capacity);

        if (POUND_SUCCESS != returned)
        {
            POUND_LOG_ERROR(&thread_logger,
                            "Allocating a block descriptor failed, and the %zu bytes at offset "
                            "%zu of chunk %p could not be returned to the free list. They stay "
                            "reserved but unreachable, so this chunk is %zu bytes smaller than "
                            "its size suggests.",
                            capacity,
                            offset,
                            (const void *)chunk,
                            capacity);
        }

        return POUND_ERROR_ALLOCATION_FAILED;
    }

    block->magic         = JIT_BLOCK_MAGIC;
    block->chunk_index   = (uint32_t)chunk_index_of(cache, chunk);
    block->kind          = kind;
    block->state         = (uint32_t)JIT_BLOCK_STATE_READ_WRITE;
    block->offset        = offset;
    block->capacity      = capacity;
    block->requested     = requested;
    block->next_in_chunk = chunk->live;

    chunk->live        = block;
    chunk->live_blocks++;
    chunk->live_bytes += capacity;

    cache->live_blocks++;
    cache->live_bytes += capacity;
    cache->rw_blocks++;
    cache->allocations++;

    return POUND_SUCCESS;
}

/// Unmaps and frees every chunk, leaving `cache`'s counters to the caller.
static void
chunk_list_drain(jit_cache_t *POUND_RESTRICT cache)
{
    jit_chunk_t *chunk = cache->chunks;

    // One bracket for the whole drain, because every release in it belongs to the
    // bucket its counterpart allocation was charged to.
    const memory_bucket_type_t previous_bucket = bucket_enter();

    while (NULL != chunk)
    {
        jit_chunk_t *const next = chunk->next;
        jit_block_t *block     = chunk->live;

        while (NULL != block)
        {
            jit_block_t *const next_block = block->next_in_chunk;

            memory_subsystem_free(block);
            block = next_block;
        }

        memory_subsystem_free(chunk->holes);
        chunk_release(chunk->base, chunk->bytes);
        memory_subsystem_free(chunk);

        cache->reclaims++;
        chunk = next;
    }

    bucket_leave(previous_bucket);
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

error_t
jit_cache_init(jit_cache_t *POUND_RESTRICT cache, const jit_cache_config_t *POUND_RESTRICT config)
{
    if (POUND_UNLIKELY(NULL == cache))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: cache context is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    memset(cache, 0, sizeof(*cache));

    jit_cache_config_t resolved;

    memset(&resolved, 0, sizeof(resolved));

    if (NULL != config)
    {
        resolved = *config;
    }

    resolved.chunk_bytes = (0U == resolved.chunk_bytes) ? JIT_CACHE_DEFAULT_CHUNK_BYTES
                                                        : resolved.chunk_bytes;
    resolved.max_bytes   = (0U == resolved.max_bytes) ? JIT_CACHE_DEFAULT_MAX_BYTES : resolved.max_bytes;

    cache->page_size = detect_page_size();

    if (POUND_UNLIKELY(0U == cache->page_size))
    {
        POUND_LOG_ERROR(&thread_logger, "The host page size could not be determined.");
        return POUND_ERROR_MEMORY_ALIGNMENT;
    }

    // A chunk that is not a whole number of pages could not be flipped in one
    // `mprotect`/`VirtualProtect` call, so it is refused at construction rather
    // than failing every executable allocation later.
    if (POUND_UNLIKELY(0U != (resolved.chunk_bytes % cache->page_size)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Chunk size %zu is not a multiple of the %zu-byte host page size.",
                        resolved.chunk_bytes,
                        cache->page_size);
        return POUND_ERROR_MEMORY_ALIGNMENT;
    }

    if (POUND_UNLIKELY((resolved.chunk_bytes < JIT_CACHE_MIN_CHUNK_BYTES)
                       || (resolved.chunk_bytes > JIT_CACHE_MAX_CHUNK_BYTES)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Chunk size %zu is outside [%u, %u].",
                        resolved.chunk_bytes,
                        (unsigned int)JIT_CACHE_MIN_CHUNK_BYTES,
                        (unsigned int)JIT_CACHE_MAX_CHUNK_BYTES);
        return POUND_ERROR_MEMORY_ALIGNMENT;
    }

    if (POUND_UNLIKELY(resolved.max_bytes < resolved.chunk_bytes))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "A %zu-byte ceiling cannot admit even one %zu-byte chunk.",
                        resolved.max_bytes,
                        resolved.chunk_bytes);
        return POUND_ERROR_MEMORY_ALIGNMENT;
    }

    cache->chunk_bytes       = resolved.chunk_bytes;
    cache->max_bytes         = resolved.max_bytes;
    cache->default_alignment = JIT_CACHE_DEFAULT_ALIGNMENT;
    cache->poison            = resolved.poison;

    if (POUND_UNLIKELY(0 != mutex_init(&cache->lock)))
    {
        POUND_LOG_ERROR(&thread_logger, "Initialising the code cache mutex failed.");
        memset(cache, 0, sizeof(*cache));
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    cache->initialised = true;

    POUND_LOG_INFO(&thread_logger,
                   "Code cache ready: %zu-byte chunks, %zu-byte ceiling, %zu-byte pages, poison "
                   "%s.",
                   cache->chunk_bytes,
                   cache->max_bytes,
                   cache->page_size,
                   cache->poison ? "on" : "off");

    return POUND_SUCCESS;
}

void
jit_cache_destroy(jit_cache_t *POUND_RESTRICT cache)
{
    if (NULL == cache)
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: cache context is NULL.");
        return;
    }

    if (!cache->initialised)
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: code cache was never initialised.");
        return;
    }

    mutex_lock(&cache->lock);

    if (POUND_UNLIKELY(0U != cache->live_blocks))
    {
        POUND_LOG_WARN(&thread_logger,
                       "Destroying a code cache with %zu blocks (%zu bytes) still live.",
                       cache->live_blocks,
                       cache->live_bytes);
    }

    const size_t released = cache->reserved_bytes;

    chunk_list_drain(cache);

    cache->chunks         = NULL;
    cache->chunk_count    = 0U;
    cache->reserved_bytes = 0U;
    cache->live_blocks    = 0U;
    cache->live_bytes     = 0U;
    cache->rw_blocks      = 0U;
    cache->rx_blocks      = 0U;

    mutex_unlock(&cache->lock);
    mutex_destroy(&cache->lock);

    memset(cache, 0, sizeof(*cache));

    POUND_LOG_INFO(&thread_logger,
                    "Code cache destroyed, returning %zu bytes to the OS.",
                    released);
}

void
jit_cache_reset(jit_cache_t *POUND_RESTRICT cache)
{
    if (NULL == cache)
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: cache context is NULL.");
        return;
    }

    if (!cache->initialised)
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: code cache was never initialised.");
        return;
    }

    mutex_lock(&cache->lock);

    if (POUND_UNLIKELY(0U != cache->live_blocks))
    {
        POUND_LOG_WARN(&thread_logger,
                       "Resetting a code cache with %zu blocks (%zu bytes) still live.",
                       cache->live_blocks,
                       cache->live_bytes);
    }

    chunk_list_drain(cache);

    cache->chunks         = NULL;
    cache->chunk_count    = 0U;
    cache->reserved_bytes = 0U;
    cache->live_blocks    = 0U;
    cache->live_bytes     = 0U;
    cache->rw_blocks      = 0U;
    cache->rx_blocks      = 0U;
    cache->allocations    = 0U;
    cache->frees          = 0U;
    cache->alloc_failures = 0U;
    cache->rw_transitions = 0U;
    cache->rx_transitions = 0U;

    mutex_unlock(&cache->lock);
}

error_t
jit_cache_describe(const jit_cache_t *POUND_RESTRICT cache, jit_cache_resolved_t *POUND_RESTRICT out)
{
    if (POUND_UNLIKELY((NULL == cache) || (NULL == out)))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: cache or output is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!cache->initialised)
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: code cache was never initialised.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    out->chunk_bytes       = cache->chunk_bytes;
    out->max_bytes         = cache->max_bytes;
    out->page_size         = cache->page_size;
    out->default_alignment = cache->default_alignment;
    out->poison            = cache->poison;

    return POUND_SUCCESS;
}

/// Validates the arguments shared by both allocating entry points.
///
/// Returns true when the request may proceed. Every rejection is logged, so the
/// caller does not have to.
static bool
check_alloc_request(const jit_cache_t *POUND_RESTRICT cache,
                    const size_t               size,
                    const size_t               alignment,
                    const char *const          path)
{
    if (!cache->initialised)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing a %zu-byte block from %s: the code cache is not initialised.",
                        size,
                        path);
        return false;
    }

    if (POUND_UNLIKELY(0U == size))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: %s was given a zero size.", path);
        return false;
    }

    if (POUND_UNLIKELY(!is_power_of_two(alignment)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing a %zu-byte block from %s: %zu is not a non-zero power of two "
                        "alignment.",
                        size,
                        path,
                        alignment);
        return false;
    }

    if (POUND_UNLIKELY((alignment < JIT_CACHE_MIN_ALIGNMENT) || (alignment > JIT_CACHE_MAX_ALIGNMENT)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing a %zu-byte block from %s: alignment %zu is outside [%u, %u].",
                        size,
                        path,
                        alignment,
                        (unsigned int)JIT_CACHE_MIN_ALIGNMENT,
                        (unsigned int)JIT_CACHE_MAX_ALIGNMENT);
        return false;
    }

    return true;
}

/// Shared body of both data-block allocating entry points.
static void *
allocate_data(jit_cache_t *POUND_RESTRICT cache, const size_t alignment, const size_t size)
{
    if (POUND_UNLIKELY(NULL == cache))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: cache context is NULL.");
        return NULL;
    }

    if (!check_alloc_request(cache, size, alignment, "jit_cache_alloc"))
    {
        return NULL;
    }

    mutex_lock(&cache->lock);

    jit_chunk_t *chunk    = NULL;
    size_t       offset   = 0U;
    size_t       capacity = 0U;

    error_t reserved
        = reserve_block(cache, (uint32_t)JIT_BLOCK_KIND_DATA, size, alignment, &chunk, &offset, &capacity);

    if (POUND_SUCCESS == reserved)
    {
        reserved = block_record(cache, chunk, (uint32_t)JIT_BLOCK_KIND_DATA, offset, capacity, size);
    }

    if (POUND_UNLIKELY(POUND_SUCCESS != reserved))
    {
        cache->alloc_failures++;
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing a %zu-byte data block at %zu-byte alignment: no chunk could "
                        "satisfy it.",
                        size,
                        alignment);
        mutex_unlock(&cache->lock);
        return NULL;
    }

    void *const block = (void *)(chunk->base + offset);

    poison_range(cache, block, capacity);

    mutex_unlock(&cache->lock);
    return block;
}

/// Shared body of both executable-block allocating entry points.
static void *
allocate_executable(jit_cache_t *POUND_RESTRICT cache, const size_t alignment, const size_t size)
{
    if (POUND_UNLIKELY(NULL == cache))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: cache context is NULL.");
        return NULL;
    }

    if (!check_alloc_request(cache, size, alignment, "jit_cache_alloc_executable"))
    {
        return NULL;
    }

    mutex_lock(&cache->lock);

    jit_chunk_t *chunk    = NULL;
    size_t       offset   = 0U;
    size_t       capacity = 0U;

    error_t reserved = reserve_block(cache,
                                     (uint32_t)JIT_BLOCK_KIND_EXECUTABLE,
                                     size,
                                     alignment,
                                     &chunk,
                                     &offset,
                                     &capacity);

    if (POUND_SUCCESS == reserved)
    {
        reserved = block_record(cache,
                                chunk,
                                (uint32_t)JIT_BLOCK_KIND_EXECUTABLE,
                                offset,
                                capacity,
                                size);
    }

    if (POUND_UNLIKELY(POUND_SUCCESS != reserved))
    {
        cache->alloc_failures++;
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing a %zu-byte executable block at %zu-byte alignment: no chunk "
                        "could satisfy it.",
                        size,
                        alignment);
        mutex_unlock(&cache->lock);
        return NULL;
    }

    void *const block = (void *)(chunk->base + offset);

    poison_range(cache, block, capacity);

    mutex_unlock(&cache->lock);
    return block;
}

void *
jit_cache_alloc(jit_cache_t *POUND_RESTRICT cache, const size_t size)
{
    if (POUND_UNLIKELY((NULL == cache) || (!cache->initialised)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing a %zu-byte block: the code cache is not initialised.",
                        size);
        return NULL;
    }

    return allocate_data(cache, cache->default_alignment, size);
}

void *
jit_cache_alloc_aligned(jit_cache_t *POUND_RESTRICT cache, const size_t alignment, const size_t size)
{
    return allocate_data(cache, alignment, size);
}

void *
jit_cache_alloc_executable(jit_cache_t *POUND_RESTRICT cache, const size_t size)
{
    if (POUND_UNLIKELY((NULL == cache) || (!cache->initialised)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing a %zu-byte executable block: the code cache is not initialised.",
                        size);
        return NULL;
    }

    return allocate_executable(cache, cache->default_alignment, size);
}

void *
jit_cache_alloc_executable_aligned(jit_cache_t *POUND_RESTRICT cache,
                                   const size_t                               alignment,
                                   const size_t                               size)
{
    return allocate_executable(cache, alignment, size);
}

/// Shared body of both releasing entry points.
///
/// The block's kind is checked against the entry point that was called, so a data
/// block cannot be released through the executable path and leave its recorded
/// state inconsistent with how a later query reads it.
static void
release_block(jit_cache_t *POUND_RESTRICT cache,
              void *POUND_RESTRICT         pointer,
              const uint32_t              expected_kind,
              const char *const           path)
{
    if (POUND_UNLIKELY(NULL == cache))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: cache context is NULL.");
        return;
    }

    if (POUND_UNLIKELY(NULL == pointer))
    {
        // A NULL release is a no-op by long-standing allocator convention, so it
        // is not an error.
        return;
    }

    if (!cache->initialised)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Ignoring call: %p cannot be released from a code cache that is not "
                        "initialised.",
                        pointer);
        return;
    }

    mutex_lock(&cache->lock);

    jit_chunk_t *chunk    = NULL;
    jit_block_t *previous = NULL;

    jit_block_t *const block = block_find(cache, pointer, &chunk, &previous);

    if (POUND_UNLIKELY(NULL == block))
    {
        mutex_unlock(&cache->lock);
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing to release %p through %s: it is not the start of a live block "
                        "in this cache. It may be an interior pointer, a pointer from another "
                        "allocator, or a block that was already released.",
                        pointer,
                        path);
        return;
    }

    if (POUND_UNLIKELY(block->kind != expected_kind))
    {
        const bool is_executable = (JIT_BLOCK_KIND_EXECUTABLE == (jit_block_kind_t)block->kind);

        mutex_unlock(&cache->lock);
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing to release %p through %s: the block is %s. Use the matching "
                        "release function so its recorded state stays consistent.",
                        pointer,
                        path,
                        is_executable ? "an executable block" : "a data block");
        return;
    }

    // Unlink using the predecessor found in the same walk that located the block.
    if (NULL != previous)
    {
        previous->next_in_chunk = block->next_in_chunk;
    }
    else
    {
        chunk->live = block->next_in_chunk;
    }

    const size_t   capacity = block->capacity;
    const size_t   offset   = block->offset;
    const uint32_t state    = block->state;

    chunk->live_blocks--;
    chunk->live_bytes -= capacity;

    cache->live_blocks--;
    cache->live_bytes -= capacity;

    if (JIT_BLOCK_STATE_READ_WRITE == (jit_block_state_t)state)
    {
        cache->rw_blocks--;
    }
    else if (JIT_BLOCK_STATE_READ_EXECUTE == (jit_block_state_t)state)
    {
        cache->rx_blocks--;
    }
    else
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Block %p was in state %u, which is not a live state; its counters were "
                        "not decremented.",
                        pointer,
                        state);
    }

    cache->frees++;

    // An executable block that is still in its read-execute state has *page* protection
    // that forbids writing, and both Windows and POSIX map protection for a whole page
    // range rather than for the block. Publishing those bytes to the free list without
    // making them writable again would hand the next allocation memory it cannot
    // write: a data block carved out of them faults on its first store, and an
    // executable block faults on the store that precedes its own first `protect_rx`.
    // Both are the engine's ordinary allocate-and-release cycle rather than anything
    // exotic, so the flip happens before the space is published.
    //
    // Only a block that was actually left executable pays for it. A JIT that patches a
    // block and releases it while writable -- which is the normal sequence -- costs
    // nothing here, which is why this is not a per-release protection call.
    const bool left_executable = ((JIT_BLOCK_KIND_EXECUTABLE == (jit_block_kind_t)expected_kind)
                                   && (JIT_BLOCK_STATE_READ_EXECUTE == (jit_block_state_t)state));

    error_t recycled = POUND_SUCCESS;

    if (left_executable)
    {
        recycled = chunk_make_rw(chunk->base + offset, capacity);
    }

    if (POUND_SUCCESS == recycled)
    {
        recycled = holes_insert(chunk, offset, capacity);
    }

    // Bracketed for the same reason as the allocation in `block_record`: the
    // descriptor is debited from the bucket it was charged to, whatever bucket the
    // caller has selected in the meantime.
    {
        const memory_bucket_type_t previous_bucket = bucket_enter();

        memory_subsystem_free(block);
        bucket_leave(previous_bucket);
    }

    mutex_unlock(&cache->lock);

    if (POUND_SUCCESS != recycled)
    {
        // The bytes stay reserved but unreachable, which is the safe direction: an
        // unreachable extent costs address space, a published-but-unwritable one costs
        // the process. The protection flip already logged its own failure above.
        if (left_executable)
        {
            POUND_LOG_ERROR(&thread_logger,
                            "Released %zu bytes at offset %zu of chunk %p, but they could not be "
                            "made writable again and will not be reused.",
                            capacity,
                            offset,
                            (const void *)chunk);
        }
        else
        {
            POUND_LOG_ERROR(&thread_logger,
                            "Released %zu bytes at offset %zu of chunk %p, but they could not be "
                            "returned to the free list and will not be reused.",
                            capacity,
                            offset,
                            (const void *)chunk);
        }
    }
}

void
jit_cache_free(jit_cache_t *POUND_RESTRICT cache, void *POUND_RESTRICT pointer)
{
    release_block(cache, pointer, (uint32_t)JIT_BLOCK_KIND_DATA, "jit_cache_free");
}

void
jit_cache_free_executable(jit_cache_t *POUND_RESTRICT cache, void *POUND_RESTRICT pointer)
{
    release_block(cache, pointer, (uint32_t)JIT_BLOCK_KIND_EXECUTABLE, "jit_cache_free_executable");
}

/// Shared body of both protection transitions.
static error_t
transition_block(jit_cache_t *POUND_RESTRICT cache,
                 void *POUND_RESTRICT         pointer,
                 const uint32_t              target,
                 const char *const           path)
{
    if (POUND_UNLIKELY(NULL == cache))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: cache context is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(NULL == pointer))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: %s was given a NULL block.", path);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!cache->initialised)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Ignoring call: %s was given a block from a code cache that is not "
                        "initialised.",
                        path);
        return POUND_ERROR_NOT_INITIALIZED;
    }

    mutex_lock(&cache->lock);

    jit_chunk_t *chunk    = NULL;
    jit_block_t *previous = NULL;

    jit_block_t *const block = block_find(cache, pointer, &chunk, &previous);

    if (POUND_UNLIKELY(NULL == block))
    {
        mutex_unlock(&cache->lock);
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing %s on %p: it is not the start of a live block in this cache.",
                        path,
                        pointer);
        return POUND_ERROR_DOUBLE_FREE;
    }

    if (POUND_UNLIKELY(JIT_BLOCK_KIND_EXECUTABLE != (jit_block_kind_t)block->kind))
    {
        mutex_unlock(&cache->lock);
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing %s on %p: the block is a data block. Its protection is never "
                        "flipped, because it does not own whole pages.",
                        path,
                        pointer);
        return POUND_ERROR_MEMORY_FAULT;
    }

    if ((uint32_t)block->state == target)
    {
        // Idempotent by design. A JIT that re-protects a block it already protected
        // has done nothing wrong, and reporting an error would only train callers to
        // ignore this function.
mutex_unlock(&cache->lock);
    POUND_LOG_INFO(&thread_logger, "%s on %p was a no-op; already in that state.", path, pointer);
    return POUND_SUCCESS;
    }

    void *const range    = (void *)(chunk->base + block->offset);
    const size_t bytes  = block->capacity;

    if (JIT_BLOCK_STATE_READ_EXECUTE == (jit_block_state_t)target)
    {
        // Flush while the range is still writable; see `chunk_flush_icache` for why
        // the order matters.
        chunk_flush_icache(range, bytes);

        const error_t flipped = chunk_make_rx(range, bytes);

        if (POUND_UNLIKELY(POUND_SUCCESS != flipped))
        {
            mutex_unlock(&cache->lock);
            return flipped;
        }

        cache->rx_transitions++;
        cache->rw_blocks--;
        cache->rx_blocks++;
    }
    else
    {
        const error_t flipped = chunk_make_rw(range, bytes);

        if (POUND_UNLIKELY(POUND_SUCCESS != flipped))
        {
            mutex_unlock(&cache->lock);
            return flipped;
        }

        cache->rw_transitions++;
        cache->rx_blocks--;
        cache->rw_blocks++;
    }

    block->state = target;

    mutex_unlock(&cache->lock);
    return POUND_SUCCESS;
}

error_t
jit_cache_protect_rx(jit_cache_t *POUND_RESTRICT cache, void *POUND_RESTRICT pointer)
{
    return transition_block(cache, pointer, (uint32_t)JIT_BLOCK_STATE_READ_EXECUTE, "jit_cache_protect_rx");
}

error_t
jit_cache_protect_rw(jit_cache_t *POUND_RESTRICT cache, void *POUND_RESTRICT pointer)
{
    return transition_block(cache, pointer, (uint32_t)JIT_BLOCK_STATE_READ_WRITE, "jit_cache_protect_rw");
}

jit_block_state_t
jit_cache_block_state(const jit_cache_t *POUND_RESTRICT cache, const void *POUND_RESTRICT pointer)
{
    if ((NULL == cache) || (!cache->initialised) || (NULL == pointer))
    {
        return JIT_BLOCK_STATE_FREE;
    }

    // The same const cast the slab allocator uses: the query takes the lock so its
    // answer is self-consistent, and the lock is not logically mutable state.
    mutex_t *const lock = (mutex_t *)(uintptr_t)(void *)&cache->lock;

    mutex_lock(lock);

    jit_block_state_t state = JIT_BLOCK_STATE_FREE;

    jit_block_t *const block = block_find(cache, pointer, NULL, NULL);

    if (NULL != block)
    {
        state = (jit_block_state_t)block->state;
    }

    mutex_unlock(lock);
    return state;
}

bool
jit_cache_is_executable(const jit_cache_t *POUND_RESTRICT cache, const void *POUND_RESTRICT pointer)
{
    return JIT_BLOCK_STATE_READ_EXECUTE == jit_cache_block_state(cache, pointer);
}

error_t
jit_cache_get_block_info(const jit_cache_t *POUND_RESTRICT cache,
                         const void *POUND_RESTRICT           pointer,
                         jit_block_info_t *POUND_RESTRICT     out)
{
    if (POUND_UNLIKELY((NULL == cache) || (NULL == pointer) || (NULL == out)))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: cache, pointer or output is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!cache->initialised)
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: code cache was never initialised.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    mutex_t *const lock = (mutex_t *)(uintptr_t)(void *)&cache->lock;

    mutex_lock(lock);

    error_t result = POUND_ERROR_DOUBLE_FREE;

    jit_chunk_t *chunk    = NULL;
    jit_block_t *previous = NULL;

    jit_block_t *const block = block_find(cache, pointer, &chunk, &previous);

    if (POUND_UNLIKELY(NULL == block))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Ignoring call: %p is not the start of a live block in this cache.",
                        pointer);
    }
    else
    {
        out->requested   = block->requested;
        out->capacity    = block->capacity;
        out->offset      = block->offset;
        out->chunk_index = block->chunk_index;
        out->kind        = block->kind;
        out->state       = block->state;
        result           = POUND_SUCCESS;
    }

    mutex_unlock(lock);
    return result;
}

size_t
jit_cache_usable_size(const jit_cache_t *POUND_RESTRICT cache, const void *POUND_RESTRICT pointer)
{
    jit_block_info_t info;

    memset(&info, 0, sizeof(info));

    if (POUND_SUCCESS != jit_cache_get_block_info(cache, pointer, &info))
    {
        return 0U;
    }

    return info.capacity;
}

error_t
jit_cache_get_stats(const jit_cache_t *POUND_RESTRICT cache, jit_cache_stats_t *POUND_RESTRICT out)
{
    if (POUND_UNLIKELY((NULL == cache) || (NULL == out)))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: cache or output is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!cache->initialised)
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: code cache was never initialised.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    mutex_t *const lock = (mutex_t *)(uintptr_t)(void *)&cache->lock;

    mutex_lock(lock);

    size_t free_bytes = 0U;

    for (const jit_chunk_t *chunk = cache->chunks; NULL != chunk; chunk = chunk->next)
    {
        free_bytes += (chunk->bytes - chunk->used);

        for (size_t i = 0U; i < chunk->hole_count; ++i)
        {
            free_bytes += chunk->holes[i].size;
        }
    }

    out->chunk_count    = cache->chunk_count;
    out->reserved_bytes = cache->reserved_bytes;
    out->live_bytes     = cache->live_bytes;
    out->free_bytes     = free_bytes;
    out->live_blocks    = cache->live_blocks;
    out->rw_blocks      = cache->rw_blocks;
    out->rx_blocks      = cache->rx_blocks;
    out->allocations    = cache->allocations;
    out->frees          = cache->frees;
    out->alloc_failures = cache->alloc_failures;
    out->rw_transitions = cache->rw_transitions;
    out->rx_transitions = cache->rx_transitions;
    out->reclaims       = cache->reclaims;

    mutex_unlock(lock);
    return POUND_SUCCESS;
}

size_t
jit_cache_reclaim(jit_cache_t *POUND_RESTRICT cache)
{
    if (POUND_UNLIKELY(NULL == cache))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: cache context is NULL.");
        return 0U;
    }

    if (!cache->initialised)
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: code cache was never initialised.");
        return 0U;
    }

    mutex_lock(&cache->lock);

    jit_chunk_t **link    = &cache->chunks;
    size_t        released = 0U;

    // Only the tail can go. An extent below a live block cannot be returned to the
    // OS without fragmenting the address space into pieces no later chunk would
    // fit, which is the same rule the slab allocator follows for the same reason.
    //
    // A chunk with no live blocks can still own a free-extent array, so the array is
    // released here too -- inside the same bucket bracket as the descriptor, and as
    // the one `holes_reserve` charged the array to.
    const memory_bucket_type_t previous_bucket = bucket_enter();

    while (NULL != *link)
    {
        jit_chunk_t *const chunk = *link;

        if (0U != chunk->live_blocks)
        {
            break;
        }

        // Read before the descriptor goes back to the allocator. The allocator recycles
        // the block immediately, so `chunk->bytes` read after `memory_subsystem_free(chunk)`
        // is whatever the next allocation has already put there -- which is how this
        // corrupted `reserved_bytes` and the return value in the first place. The value
        // matters: `reserved_bytes` is what the ceiling test subtracts from, so garbage in
        // here is either a cache that silently exceeds its ceiling or one that refuses
        // every request for the rest of the session.
        const size_t chunk_bytes = chunk->bytes;

        *link = chunk->next;

        memory_subsystem_free(chunk->holes);
        chunk_release(chunk->base, chunk_bytes);
        memory_subsystem_free(chunk);

        cache->chunk_count--;
        cache->reserved_bytes -= chunk_bytes;
        cache->reclaims++;
        released += chunk_bytes;
    }

    bucket_leave(previous_bucket);

    const size_t remaining = cache->chunk_count;

    mutex_unlock(&cache->lock);

    if (0U != released)
    {
        POUND_LOG_DEBUG(&thread_logger,
                        "Returned %zu bytes of code cache to the OS (%zu chunks left).",
                        released,
                        remaining);
    }

    return released;
}

/*** end of file ***/
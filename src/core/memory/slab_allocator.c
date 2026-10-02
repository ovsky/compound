#include "slab_allocator.h"
#include "log.h"
#include <string.h>

/// Marks an object as available to hand out.
#define SLAB_OBJECT_MAGIC_FREE 0x45524653u /* 'SFRE' little-endian */

/// Marks an object as currently handed out.
#define SLAB_OBJECT_MAGIC_LIVE 0x45564C53u /* 'SLVE' little-endian */

/// Sentinel for `slab_object_t::next_free` meaning "end of list".
///
/// 32-bit because it is an object index, and the largest possible slab holds
/// `SLAB_ALLOCATOR_MAX_SLAB_BYTES / stride` objects, which is bounded well below
/// `UINT32_MAX` by the slab-size cap.
#define SLAB_OBJECT_FREE_NONE UINT32_MAX

/// Header preceding every object payload.
///
/// Laid out to exactly 16 bytes so that at the default alignment a header
/// consumes no slack at all. The field order is chosen for that: a pointer, then
/// the two 32-bit fields that fill the remaining eight-byte slot.
typedef struct slab_object slab_object_t;

struct slab_object
{
    /// Owning slab. Never dereferenced during validation; see `find_slab`.
    slab_t *slab;

    /// Next free object in the owning slab, as an index into the slab's object
    /// array, or `SLAB_OBJECT_FREE_NONE` when live. An index rather than a
    /// pointer so no side array is needed to thread the list.
    uint32_t next_free;

    /// `SLAB_OBJECT_MAGIC_FREE` or `SLAB_OBJECT_MAGIC_LIVE`. Doubles as the
    /// live/free discriminator, so a double free is rejected rather than
    /// duplicating the object on a free list.
    uint32_t magic;
};

/// One slab: a single carve out of the arena, divided into equal objects.
///
/// Field order keeps the struct free of implicit padding: seven pointers-sized
/// fields, all naturally aligned.
struct slab
{
    /// First payload. Aligned to the owning cache's alignment.
    uint8_t *objects;

    /// Host address the slab was carved at, so the region can be bounds-checked
    /// without consulting any pointer the caller supplied.
    uint8_t *host_base;

    /// Bytes of `objects`.
    size_t objects_size;

    /// Objects carved, live and free.
    size_t object_count;

    /// Objects currently handed out.
    size_t in_use;

    /// Owning cache, so a free can update the right counters without a lookup.
    slab_cache_t *cache;

    /// Bytes this slab occupies in the arena, header and alignment slack
    /// included. Recorded because the arena is a stack: releasing a slab is only
    /// possible when it sits at the very end, and that has to be decided from the
    /// address range rather than recomputed.
    size_t used_bytes;

    struct slab *next;

    /// Index of the head of the free list, or `SLAB_FREE_HEAD_NONE` when full.
    ///
    /// An object index rather than a `size_t`, to match `slab_object_t::next_free`
    /// so the two can be copied between each other without a narrowing cast on
    /// every free. `object_count` is validated to fit in 32 bits in `plan_slab`,
    /// which the slab-size cap makes trivially true.
    uint32_t free_head;

    /// Absorbs the struct's tail padding explicitly, following the convention
    /// already used by `pool_allocator_t`.
    uint32_t pad;
};

/// Sentinel for `slab_t::free_head` meaning "no free object". Deliberately the
/// same value as `SLAB_OBJECT_FREE_NONE`, so the two fields are interchangeable.
#define SLAB_FREE_HEAD_NONE UINT32_MAX

// The object header is 16 bytes by design, so that at the default 16-byte
// alignment it consumes no slack. A change to its field list that added padding
// would silently cost 8 bytes on every object in every slab, and the tests would
// still pass, so the layout is pinned here rather than left to review.
_Static_assert(sizeof(slab_object_t) == 16U,
               "the object header must stay 16 bytes to avoid per-object slack");

// The slab header's size is load-bearing in a subtler way: it offsets the object
// region, so a change to it shifts every payload in every slab. Pinned for the
// same reason.
_Static_assert(sizeof(slab_t) == 72U, "the slab header layout is part of the arena geometry");

/// One object type.
struct slab_cache
{
    slab_config_t config;

    /// Distance between consecutive payloads: header plus payload, rounded up to
    /// the cache alignment.
    size_t stride;

    /// Distance from a payload back to its header.
    size_t header_stride;

    /// Slabs with at least one free object.
    slab_t *partial;

    /// Slabs with no free object.
    slab_t *full;

    size_t   in_use;
    size_t   object_count;
    size_t   slab_count;
    uint64_t allocations;
    uint64_t frees;

    /// Set once growth has failed, so the condition is reportable through
    /// `slab_allocator_cache_is_starved` and in stats, rather than being visible
    /// only as a log line.
    bool starved;

    /// Absorbs the struct's tail padding explicitly, following the convention
    /// already used by `pool_allocator_t`.
    char pad[7];
};

/// Rounds `value` up to the next multiple of `alignment`.
///
/// `alignment` is always a power of two, validated at cache creation, so this is
/// a mask rather than a division. Returns false on overflow.
static bool
align_up_checked(size_t value, size_t alignment, size_t *POUND_RESTRICT out)
{
    const size_t mask = alignment - 1U;

    if (value > (SIZE_MAX - mask))
    {
        return false;
    }

    *out = (value + mask) & ~mask;
    return true;
}

/// Rounds `value` up to the next multiple of the compile-time constant
/// `alignment`, without logging on overflow.
///
/// Used where the alignment is a literal from this file rather than caller input,
/// so overflow is a property of the constants and cannot be triggered at runtime.
/// `align_up_checked` remains the entry point for anything caller-supplied.
static size_t
align_up_unlogged(size_t value, size_t alignment)
{
    const size_t mask = alignment - 1U;

    return (value + mask) & ~mask;
}

/// Returns the cache for `cache_id`, or NULL if it does not exist.
///
/// One-based ids are what make this safe: a zeroed table contains no caches at
/// all, so `SLAB_CACHE_ID_NONE` cannot collide with a real id. The caller must
/// hold `lock`.
static slab_cache_t *
find_cache(const slab_allocator_t *POUND_RESTRICT allocator, slab_cache_id_t cache_id)
{
    if ((SLAB_CACHE_ID_NONE == cache_id) || (cache_id > allocator->cache_count))
    {
        return NULL;
    }

    // Ids are handed out densely from one, so the id indexes the table directly.
    return allocator->caches[cache_id - 1U];
}

/// Returns the header preceding `payload`.
static slab_object_t *
object_header(const slab_cache_t *POUND_RESTRICT cache, const void *POUND_RESTRICT payload)
{
    return (slab_object_t *)(void *)((uint8_t *)(uintptr_t)payload - cache->header_stride);
}

/// Returns the payload following `header`.
static void *
object_payload(const slab_cache_t *POUND_RESTRICT cache, const slab_object_t *POUND_RESTRICT header)
{
    return (void *)(uint8_t *)(uintptr_t)header + cache->header_stride;
}

/// Fills `bytes` at `object` with the poison pattern.
static void
poison_range(const slab_allocator_t *POUND_RESTRICT allocator,
             void *POUND_RESTRICT         object,
             size_t                       bytes)
{
    if (allocator->poison_enabled)
    {
        memset(object, (int)allocator->poison_byte, bytes);
    }
}

/// Searches one cache's slab lists for the slab containing `object`.
///
/// Both lists are searched: an object in a *full* slab is still live, and a
/// search that looked only at the partial list would refuse every free once the
/// cache filled up. The caller must hold `lock`.
static slab_t *
search_cache_slabs(const slab_cache_t *POUND_RESTRICT cache,
                   const uint8_t *POUND_RESTRICT       address,
                   size_t *POUND_RESTRICT               out_index)
{
    for (int pass = 0; pass < 2; ++pass)
    {
        // Pass 0 walks the partial list, pass 1 the full list. Walking the
        // partial list first matches the common case, where the object was freed
        // recently or the cache has spare capacity.
        const slab_t *slab = (0 == pass) ? cache->partial : cache->full;

        for (; NULL != slab; slab = slab->next)
        {
            if ((address < slab->objects)
                || ((size_t)(address - slab->objects) >= slab->objects_size))
            {
                continue;
            }

            const size_t offset = (size_t)(address - slab->objects);

            if ((offset % cache->stride) != 0U)
            {
                // Inside the slab but not on an object boundary: an interior
                // pointer, which this allocator does not hand out. Returning
                // NULL rather than continuing is deliberate -- continuing would
                // let a mid-object pointer resolve to whichever later slab
                // happened to contain it.
                return NULL;
            }

            *out_index = offset / cache->stride;
            return (slab_t *)(uintptr_t)slab;
        }
    }

    return NULL;
}

/// Locates the slab whose object region contains `object`.
///
/// Walks the allocator's own lists and compares addresses arithmetically,
/// rather than following the slab pointer stored in the object's header. That
/// ordering is the point: a pointer the caller invented has a header full of
/// garbage, and reading `header->slab` to learn whether the pointer is ours would
/// already have dereferenced it.
///
/// Returns the slab and writes the object's index within it to `out_index`, or
/// returns NULL if the pointer belongs to no slab. The caller must hold `lock`.
static slab_t *
find_slab(const slab_allocator_t *POUND_RESTRICT allocator,
          const void *POUND_RESTRICT       object,
          size_t *POUND_RESTRICT            out_index)
{
    const uint8_t *const address = (const uint8_t *)(uintptr_t)object;

    // A released slab is detached from every cache's lists, so it holds no live
    // objects and is never searched. A pointer into one is a use-after-free that
    // `cache_destroy` or `trim` already invalidated.
    for (size_t c = 0U; c < allocator->cache_count; ++c)
    {
        slab_t *const slab = search_cache_slabs(allocator->caches[c], address, out_index);

        if (NULL != slab)
        {
            return slab;
        }
    }

    return NULL;
}

/// Validates that the object at `index` of `slab` is live, and resolves its
/// header.
///
/// `slab` must have come from `find_slab`, so the bounds are already known good
/// and the header is inside a region this allocator owns. Returns
/// `POUND_SUCCESS`, or a typed error with a log record naming which check failed.
static error_t
resolve_live_object(const slab_cache_t *POUND_RESTRICT cache,
                    const slab_t *POUND_RESTRICT        slab,
                    size_t                              index,
                    const void *POUND_RESTRICT          object,
                    slab_object_t **POUND_RESTRICT      out_header)
{
    if (index >= slab->object_count)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing %p: object index %zu is outside cache \"%s\"'s slab of "
                        "%zu objects.",
                        object,
                        index,
                        cache->config.name,
                        slab->object_count);
        return POUND_ERROR_CORRUPTED;
    }

    const uint8_t *const payload = slab->objects + (index * cache->stride);

    slab_object_t *const header = object_header(cache, payload);

    if (SLAB_OBJECT_MAGIC_LIVE != header->magic)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "free refused: %p is not a live object of cache \"%s\" (magic 0x%08X); "
                        "it was never allocated here or has already been freed.",
                        object,
                        cache->config.name,
                        (unsigned int)header->magic);
        return POUND_ERROR_DOUBLE_FREE;
    }

    if (header->slab != slab)
    {
        // Reachable only if the arena was overwritten. Reported rather than
        // trusted, because carrying on would credit the wrong slab's counters.
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing %p: its header names slab %p but the containing slab is %p.",
                        object,
                        (const void *)header->slab,
                        (const void *)slab);
        return POUND_ERROR_CORRUPTED;
    }

    *out_header = header;
    return POUND_SUCCESS;
}

/// Unlinks `slab` from whichever of `cache`'s two lists currently holds it.
///
/// Reports and does nothing if the slab is on neither list, which can only mean
/// the list bookkeeping has diverged. Reporting rather than asserting keeps a
/// corrupted allocator inspectable instead of terminating the guest.
static void
unlink_slab(slab_cache_t *POUND_RESTRICT cache, slab_t *POUND_RESTRICT slab)
{
    for (slab_t **link = &cache->partial; NULL != *link; link = &(*link)->next)
    {
        if (*link == slab)
        {
            *link = slab->next;
            return;
        }
    }

    for (slab_t **link = &cache->full; NULL != *link; link = &(*link)->next)
    {
        if (*link == slab)
        {
            *link = slab->next;
            return;
        }
    }

    POUND_LOG_ERROR(&thread_logger,
                    "Slab %p is on neither list of cache \"%s\"; list bookkeeping has diverged.",
                    (const void *)slab,
                    cache->config.name);
}

/// Computes the slab geometry for `config` against the arena's current cursor.
///
/// The result depends on where the slab will land, because the object region
/// starts at the next aligned address after the header rather than immediately
/// after it, so the usable object bytes vary by up to `alignment - 1`. Both the
/// dry run at cache creation and the real carve therefore go through this, and
/// both are called with an unchanged cursor, which is what makes their answers
/// agree.
///
/// Any of the five outputs may be NULL when the caller does not need it; the dry
/// run at cache creation asks only for the stride and header stride, for
/// instance. Returns `POUND_SUCCESS` or a typed error with a log record.
static error_t
plan_slab(const slab_allocator_t *POUND_RESTRICT allocator,
          const slab_config_t *POUND_RESTRICT config,
          size_t *POUND_RESTRICT                   out_stride,
          size_t *POUND_RESTRICT                   out_header_stride,
          size_t *POUND_RESTRICT                   out_payload_offset,
          size_t *POUND_RESTRICT                   out_object_count,
          size_t *POUND_RESTRICT                   out_used_bytes)
{
    // The header for object `i` is stored at `objects + i * stride - header_stride`,
    // which is to say in the slack *below* object `i`'s payload -- the gap between
    // the end of object `i - 1`'s payload and object `i`'s payload start.
    //
    // So `stride` and `header_stride` cannot be computed independently. If they
    // were, `header_stride` rounding up to the alignment while `stride` rounded
    // only `sizeof(slab_object_t)` would make the slack `stride - header_stride`
    // smaller than `object_size`, and every header would be written on top of the
    // previous object's payload. That is silent: allocations succeed, the
    // alignment checks pass, and only a later free notices the header now holds
    // guest data. Hence `stride` is derived from `header_stride`.
    size_t header_stride = 0U;

    if (POUND_UNLIKELY(!align_up_checked(sizeof(slab_object_t), config->alignment, &header_stride)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Cache \"%s\" cannot be created: header alignment overflowed.",
                        config->name);
        return POUND_ERROR_MEMORY_ALIGNMENT;
    }

    size_t stride = 0U;

    if (POUND_UNLIKELY(!align_up_checked(header_stride + config->object_size,
                                         config->alignment,
                                         &stride)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Cache \"%s\" cannot be created: a %zu-byte object at %zu-byte alignment "
                        "overflows the stride computation.",
                        config->name,
                        config->object_size,
                        config->alignment);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    // Only arena beyond the cursor can hold this slab; anything before it belongs
    // to slabs already carved.
    if (allocator->bump > allocator->arena_size)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Cache \"%s\" cannot be created: the arena cursor at %zu is past the "
                        "%zu-byte arena.",
                        config->name,
                        allocator->bump,
                        allocator->arena_size);
        return POUND_ERROR_CORRUPTED;
    }

    const size_t remaining = allocator->arena_size - allocator->bump;

    if (remaining <= sizeof(slab_t))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Cache \"%s\" cannot be created: only %zu arena bytes remain, too few "
                        "for a %zu-byte slab header.",
                        config->name,
                        remaining,
                        sizeof(slab_t));
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    const uintptr_t base = (uintptr_t)allocator->arena + allocator->bump;

    // The lead-in must clear two structures: the slab header, and the *first object's*
    // header, which sits *below* the payload region at `objects - header_stride`.
    //
    // The second is easy to forget, and forgetting it is silent until the
    // alignment is wide enough for `header_stride` to exceed `sizeof(slab_t)`. At
    // that point object zero's header lands on top of the slab header and
    // overwrites its cache pointer, after which every free resolves to the wrong
    // slab and every stats query reports nonsense. So the requirement is their
    // sum, not their maximum.
    const size_t minimum_lead = sizeof(slab_t) + header_stride;

    if (POUND_UNLIKELY(base > (UINTPTR_MAX - (uintptr_t)minimum_lead)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Cache \"%s\" cannot be created: the slab address overflows.",
                        config->name);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    // Align against the slab's *real address*, not against the header size.
    //
    // This distinction is the whole correctness of the alignment guarantee. The
    // arena is aligned to 16, but a slab's own address is not: a preceding cache
    // record or an earlier slab can leave the cursor at any multiple of eight. So
    // `align_up(minimum_lead, alignment)` -- the offset that would be correct if
    // every slab started on an alignment boundary -- is wrong here, and would hand
    // out every payload with the same residual misalignment.
    size_t payload_start = 0U;

    if (POUND_UNLIKELY(!align_up_checked((size_t)(base + minimum_lead),
                                         config->alignment,
                                         &payload_start)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Cache \"%s\" cannot be created: payload alignment overflowed.",
                        config->name);
        return POUND_ERROR_MEMORY_ALIGNMENT;
    }

    const size_t payload_offset = payload_start - (size_t)base;

    // Guard the subtraction before making it. A wide alignment can push the
    // payload start past the end of what is left, and `remaining - payload_offset`
    // would then wrap to a value near SIZE_MAX, producing an object count in the
    // billions and a carve that scribbles over the whole address space.
    if (POUND_UNLIKELY((payload_offset > remaining) || ((remaining - payload_offset) < stride)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Cache \"%s\" cannot be created: %zu arena bytes remain, which cannot hold "
                        "a %zu-byte header, %zu bytes of alignment padding, and one %zu-byte object.",
                        config->name,
                        remaining,
                        sizeof(slab_t),
                        payload_offset,
                        stride);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    const size_t objects_size = remaining - payload_offset;

    size_t object_count = objects_size / stride;

    // Honour the cache's slab size by capping how much of the remaining arena the
    // slab may claim. The remainder is stranded rather than reused, which is the
    // documented cost of a fixed slab size.
    const size_t cap_payload = (config->slab_bytes > payload_offset)
                                   ? (config->slab_bytes - payload_offset)
                                   : 0U;

    if ((object_count * stride) > cap_payload)
    {
        object_count = cap_payload / stride;
    }

    if (0U == object_count)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Cache \"%s\" cannot be created: its %zu-byte slab size is too small for "
                        "one %zu-byte object.",
                        config->name,
                        config->slab_bytes,
                        stride);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    // Both free-list links are object indices held in 32 bits, so a slab holding
    // more objects than a 32-bit index can address is not representable. The
    // slab-size cap already bounds this at roughly 700k objects, so this is an
    // assertion about the constants rather than a runtime condition.
    if (object_count > (size_t)UINT32_MAX)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Cache \"%s\" cannot be created: %zu objects per slab exceeds the "
                        "32-bit free-list index limit.",
                        config->name,
                        object_count);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    // Round the slab's footprint up to the cache's alignment, not merely to the
    // header's. That way the *next* slab also starts on an alignment boundary, so
    // `payload_offset` is the same for every slab of a cache and a slab's object
    // count does not vary with where in the arena it happened to land.
    //
    // The rounding can push the footprint past what is left even though the
    // objects themselves fit, in which case the last object is dropped rather
    // than the carve being refused: an object fewer is a smaller slab, and a
    // smaller slab that fits is worth far more than the configured one that
    // cannot.
    const size_t granule = (config->alignment > _Alignof(slab_t)) ? config->alignment
                                                                 : _Alignof(slab_t);
    size_t       used_bytes = 0U;

    for (;;)
    {
        if (POUND_UNLIKELY(!align_up_checked(payload_offset + (object_count * stride),
                                             granule,
                                             &used_bytes)))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "Cache \"%s\" cannot be created: the slab size overflows.",
                            config->name);
            return POUND_ERROR_ALLOCATION_FAILED;
        }

        if (used_bytes <= remaining)
        {
            break;
        }

        if (0U == object_count)
        {
            POUND_LOG_ERROR(&thread_logger,
                            "Cache \"%s\" cannot be created: a %zu-byte header, %zu bytes of "
                            "alignment padding and %zu bytes of rounding do not fit in the %zu "
                            "bytes remaining.",
                            config->name,
                            sizeof(slab_t),
                            payload_offset,
                            granule,
                            remaining);
            return POUND_ERROR_ALLOCATION_FAILED;
        }

        object_count--;
    }

    if (NULL != out_stride)
    {
        *out_stride = stride;
    }

    if (NULL != out_header_stride)
    {
        *out_header_stride = header_stride;
    }

    if (NULL != out_payload_offset)
    {
        *out_payload_offset = payload_offset;
    }

    if (NULL != out_object_count)
    {
        *out_object_count = object_count;
    }

    if (NULL != out_used_bytes)
    {
        *out_used_bytes = used_bytes;
    }

    return POUND_SUCCESS;
}

/// Carves one new slab for `cache` at the arena cursor and links it as partial.
///
/// Returns `POUND_SUCCESS` with the slab in `out_slab`, or a typed error with a
/// log record. On failure the arena cursor is left untouched and the cache is
/// marked starved.
static error_t
grow_cache(slab_allocator_t *POUND_RESTRICT allocator,
           slab_cache_t *POUND_RESTRICT      cache,
           slab_t **POUND_RESTRICT           out_slab)
{
    size_t        stride         = 0U;
    size_t        header_stride  = 0U;
    size_t        payload_offset = 0U;
    size_t        object_count   = 0U;
    size_t        used_bytes     = 0U;
    const error_t planned        = plan_slab(allocator,
                                     &cache->config,
                                     &stride,
                                     &header_stride,
                                     &payload_offset,
                                     &object_count,
                                     &used_bytes);

    if (POUND_SUCCESS != planned)
    {
        cache->starved = true;
        return planned;
    }

    slab_t *const slab = (slab_t *)(void *)(allocator->arena + allocator->bump);

    slab->host_base    = (uint8_t *)(void *)slab;
    slab->objects      = slab->host_base + payload_offset;
    slab->objects_size = object_count * stride;
    slab->object_count = object_count;
    slab->in_use       = 0U;
    slab->cache        = cache;
    slab->used_bytes   = used_bytes;
    slab->free_head    = SLAB_FREE_HEAD_NONE;
    slab->next         = cache->partial;

    // Thread the free list through the objects themselves in index order, so the
    // first allocation of a slab hands out its lowest index and the sequence is
    // reproducible across runs. Reproducibility matters because a save state
    // records guest object addresses.
    for (size_t i = 0U; i < object_count; ++i)
    {
        const uint8_t *const payload = slab->objects + (i * stride);

        slab_object_t *const header = object_header(cache, payload);

        header->slab      = slab;
        header->magic     = SLAB_OBJECT_MAGIC_FREE;
        header->next_free = (i + 1U < object_count) ? (uint32_t)(i + 1U)
                                                    : SLAB_OBJECT_FREE_NONE;
    }

    slab->free_head = 0U;

    cache->partial      = slab;
    cache->object_count += object_count;
    cache->slab_count++;

    allocator->slab_count++;
    allocator->object_count += object_count;
    allocator->bytes_committed += used_bytes;
    allocator->bump += used_bytes;

    *out_slab = slab;
    return POUND_SUCCESS;
}

/// Pops one live object from `cache`, growing the cache when none is free.
///
/// Returns NULL only after logging, having already counted the failure in
/// `alloc_failures` and marked the cache starved.
///
/// Recursion is bounded to one level: the only path that recurses is the one
/// immediately after a successful carve, and a freshly carved slab always has a
/// free object, so the recursive call returns without growing again.
static void *
take_object(slab_allocator_t *POUND_RESTRICT allocator, slab_cache_t *POUND_RESTRICT cache)
{
    while (NULL != cache->partial)
    {
        slab_t *const slab = cache->partial;

        if (SLAB_FREE_HEAD_NONE == slab->free_head)
        {
            // A slab only returns to the partial list when an object is freed
            // into it, so this is unreachable unless the lists have diverged.
            // Treat the slab as full rather than handing out a slot with no free
            // index.
            POUND_LOG_ERROR(&thread_logger,
                            "Partial slab %p of cache \"%s\" has an empty free list; list "
                            "bookkeeping has diverged.",
                            (const void *)slab,
                            cache->config.name);
            unlink_slab(cache, slab);
            slab->next = cache->full;
            cache->full = slab;
            continue;
        }

        const size_t index = (size_t)slab->free_head;

        slab_object_t *const header
            = (slab_object_t *)(void *)(slab->objects + (index * cache->stride)
                                       - cache->header_stride);

        // Advance the head before mutating the header, so a corrupted
        // `next_free` cannot make the list lose its remaining entries.
        slab->free_head = header->next_free;

        header->next_free = SLAB_OBJECT_FREE_NONE;
        header->magic     = SLAB_OBJECT_MAGIC_LIVE;

        slab->in_use++;
        cache->in_use++;
        cache->allocations++;
        allocator->in_use++;

        if (SLAB_FREE_HEAD_NONE == slab->free_head)
        {
            unlink_slab(cache, slab);
            slab->next = cache->full;
            cache->full = slab;
        }

        void *const object = object_payload(cache, header);

        poison_range(allocator, object, cache->config.object_size);
        return object;
    }

    // No partial slab has room. The first allocation reserves a whole group so
    // that a cache used in bursts does not carve a slab per object; later growth
    // takes one slab at a time so a long-running guest does not over-reserve.
    const size_t want = (0U == cache->slab_count) ? cache->config.slab_count : 1U;

    bool grew_any = false;

    for (size_t i = 0U; i < want; ++i)
    {
        slab_t *slab = NULL;

        if (POUND_SUCCESS != grow_cache(allocator, cache, &slab))
        {
            // Partial success is still usable: the caller may have a slab to draw
            // from, since a later carve in the group can fail once the arena runs
            // out. Only fail outright when nothing at all was obtained.
            break;
        }

        grew_any = true;
    }

    if (!grew_any)
    {
        allocator->alloc_failures++;
        return NULL;
    }

    return take_object(allocator, cache);
}

/// Rewinds the arena over trailing slabs whose objects are all free.
///
/// Releases the longest run of fully-free slabs ending exactly at the arena
/// cursor, and stops at the first trailing slab that still holds a live object.
/// `only` restricts the search to one cache; NULL considers every cache.
///
/// Requires `lock`.
static size_t
trim_trailing(slab_allocator_t *POUND_RESTRICT allocator, const slab_cache_t *POUND_RESTRICT only)
{
    size_t released = 0U;

    for (;;)
    {
        const uint8_t *const top = allocator->arena + allocator->bump;

        slab_t        *found      = NULL;
        slab_cache_t *found_cache = NULL;

        for (size_t c = 0U; c < allocator->cache_count; ++c)
        {
            slab_cache_t *const cache = allocator->caches[c];

            if ((NULL != only) && (cache != only))
            {
                continue;
            }

            for (int pass = 0; pass < 2; ++pass)
            {
                for (slab_t *slab = (0 == pass) ? cache->partial : cache->full;
                     NULL != slab;
                     slab = slab->next)
                {
                    if ((slab->host_base + slab->used_bytes) != top)
                    {
                        continue;
                    }

                    if (0U != slab->in_use)
                    {
                        // Trailing but still live: the rewind must stop here,
                        // because every slab below this one is only reachable
                        // while this one stays allocated.
                        return released;
                    }

                    found       = slab;
                    found_cache = cache;
                }
            }
        }

        if (NULL == found)
        {
            return released;
        }

        unlink_slab(found_cache, found);

        allocator->bump -= found->used_bytes;
        allocator->slab_count--;
        allocator->object_count -= found->object_count;
        allocator->bytes_committed -= found->used_bytes;
        found_cache->object_count -= found->object_count;
        found_cache->slab_count--;

        found->cache = NULL;
        released++;
    }
}

error_t
slab_allocator_init(slab_allocator_t *POUND_RESTRICT         allocator,
                    void *POUND_RESTRICT                   arena,
                    size_t                                 arena_size,
                    const slab_allocator_config_t *POUND_RESTRICT config)
{
    if (POUND_UNLIKELY(NULL == allocator))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: allocator context is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(NULL == arena))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: arena is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(0U == arena_size))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: arena_size is zero.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(0U != ((uintptr_t)arena % (uintptr_t)SLAB_ALLOCATOR_DEFAULT_ALIGNMENT)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: arena %p is not a multiple of the %d-byte alignment.",
                        arena,
                        (int)SLAB_ALLOCATOR_DEFAULT_ALIGNMENT);
        return POUND_ERROR_MEMORY_ALIGNMENT;
    }

    // Start from a zeroed structure so a failed initialisation never leaves the
    // caller with counters that disagree with its arena.
    memset(allocator, 0, sizeof(*allocator));

    allocator->arena          = (uint8_t *)arena;
    allocator->arena_size     = arena_size;
    allocator->bump           = 0U;
    allocator->poison_byte    = (uint8_t)SLAB_ALLOCATOR_POISON_BYTE;
    allocator->poison_enabled = (NULL != config) ? config->poison : false;

    if (POUND_UNLIKELY(POUND_SUCCESS != mutex_init(&allocator->lock)))
    {
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    POUND_LOG_INFO(&thread_logger,
                   "Initialised a slab allocator over a %zu-byte arena.",
                   arena_size);
    return POUND_SUCCESS;
}

void
slab_allocator_reset(slab_allocator_t *POUND_RESTRICT allocator)
{
    if (POUND_UNLIKELY(NULL == allocator))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: allocator context is NULL.");
        return;
    }

    if (NULL == allocator->arena)
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: allocator was never initialised.");
        return;
    }

    mutex_lock(&allocator->lock);

    // The arena is not scrubbed. Its contents are about to be overwritten by the
    // next carve, and a scrub proportional to the arena would make a reset, which
    // a save-state load performs on every restore, cost more than the load itself.
    for (size_t i = 0U; i < allocator->cache_count; ++i)
    {
        allocator->caches[i] = NULL;
    }

    allocator->caches[allocator->cache_count] = NULL;

    allocator->cache_count     = 0U;
    allocator->slab_count      = 0U;
    allocator->object_count    = 0U;
    allocator->in_use          = 0U;
    allocator->bytes_committed = 0U;
    allocator->bump            = 0U;
    allocator->alloc_failures  = 0U;

    mutex_unlock(&allocator->lock);
}

void
slab_allocator_destroy(slab_allocator_t *POUND_RESTRICT allocator)
{
    if (POUND_UNLIKELY(NULL == allocator))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: allocator context is NULL.");
        return;
    }

    if (NULL == allocator->arena)
    {
        return;
    }

    if (0U != allocator->in_use)
    {
        POUND_LOG_WARN(&thread_logger,
                       "Destroying a slab allocator with %zu objects still live across %zu caches.",
                       allocator->in_use,
                       allocator->cache_count);
    }

    mutex_destroy(&allocator->lock);
    memset(allocator, 0, sizeof(*allocator));
}

slab_cache_id_t
slab_allocator_cache_create(slab_allocator_t *POUND_RESTRICT allocator,
                            const slab_config_t *POUND_RESTRICT config)
{
    if (POUND_UNLIKELY(NULL == allocator) || (POUND_UNLIKELY(NULL == config)))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: allocator or config is NULL.");
        return SLAB_CACHE_ID_NONE;
    }

    if (NULL == allocator->arena)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: allocator was never initialised.");
        return SLAB_CACHE_ID_NONE;
    }

    if (POUND_UNLIKELY(NULL == config->name) || ('\0' == config->name[0]))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: cache name is NULL or empty.");
        return SLAB_CACHE_ID_NONE;
    }

    if (POUND_UNLIKELY(config->object_size < SLAB_ALLOCATOR_MIN_OBJECT_SIZE))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing to create cache \"%s\": object_size %zu is below the %u-byte "
                        "minimum a header requires.",
                        config->name,
                        config->object_size,
                        (unsigned int)SLAB_ALLOCATOR_MIN_OBJECT_SIZE);
        return SLAB_CACHE_ID_NONE;
    }

    slab_config_t resolved = *config;

    if (0U == resolved.alignment)
    {
        resolved.alignment = SLAB_ALLOCATOR_DEFAULT_ALIGNMENT;
    }

    if (0U == resolved.slab_bytes)
    {
        resolved.slab_bytes = SLAB_ALLOCATOR_DEFAULT_SLAB_BYTES;
    }

    if (0U == resolved.slab_count)
    {
        resolved.slab_count = 1U;
    }

    if (POUND_UNLIKELY(0U != (resolved.alignment & (resolved.alignment - 1U))))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing to create cache \"%s\": alignment %zu is not a power of two.",
                        resolved.name,
                        resolved.alignment);
        return SLAB_CACHE_ID_NONE;
    }

    if (resolved.slab_bytes > SLAB_ALLOCATOR_MAX_SLAB_BYTES)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing to create cache \"%s\": slab_bytes %zu exceeds the %u-byte "
                        "maximum.",
                        resolved.name,
                        resolved.slab_bytes,
                        (unsigned int)SLAB_ALLOCATOR_MAX_SLAB_BYTES);
        return SLAB_CACHE_ID_NONE;
    }

    mutex_lock(&allocator->lock);

    if (POUND_UNLIKELY(allocator->cache_count >= SLAB_ALLOCATOR_MAX_CACHES))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing to create cache \"%s\": all %d cache slots are in use.",
                        resolved.name,
                        (int)SLAB_ALLOCATOR_MAX_CACHES);
        mutex_unlock(&allocator->lock);
        return SLAB_CACHE_ID_NONE;
    }

    // The cache record itself is carved from the arena, so the slab allocator
    // needs no host allocator of its own and every structure it hands out has one
    // owner and one lifetime. It is placed after any slabs already carved, which
    // is why the geometry below is computed against the current cursor.
    //
    // The reserve is rounded up to the default alignment so every slab that
    // follows starts on an alignment boundary rather than on whatever offset the
    // record's size happened to leave behind.
    const size_t reserve = align_up_unlogged(sizeof(slab_cache_t),
                                             SLAB_ALLOCATOR_DEFAULT_ALIGNMENT);

    if (POUND_UNLIKELY((allocator->arena_size - allocator->bump) < reserve))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing to create cache \"%s\": %zu arena bytes remain, too few for its "
                        "%zu-byte record.",
                        resolved.name,
                        allocator->arena_size - allocator->bump,
                        reserve);
        mutex_unlock(&allocator->lock);
        return SLAB_CACHE_ID_NONE;
    }

    slab_cache_t *const cache = (slab_cache_t *)(void *)(allocator->arena + allocator->bump);

    memset(cache, 0, sizeof(*cache));
    allocator->bump += reserve;

    cache->config = resolved;

    // Dry-run the geometry so a cache that could never be satisfied is rejected
    // now, rather than at the first guest allocation. No slab is carved here, so
    // a cache that is created and never used costs only its record. Only the
    // stride and header stride are wanted; the rest is what `grow_cache` will
    // recompute against a cursor that has not moved.
    size_t        stride        = 0U;
    size_t        header_stride = 0U;
    size_t        object_count  = 0U;
    const error_t planned
        = plan_slab(allocator, &cache->config, &stride, &header_stride, NULL, &object_count, NULL);

    if (POUND_SUCCESS != planned)
    {
        // Give the record back: nothing references it, and a caller that retries
        // with a smaller object should not pay for the abandoned one.
        allocator->bump -= reserve;
        mutex_unlock(&allocator->lock);
        return SLAB_CACHE_ID_NONE;
    }

    cache->stride        = stride;
    cache->header_stride = header_stride;

    allocator->caches[allocator->cache_count] = cache;
    allocator->cache_count++;

    const slab_cache_id_t id = (slab_cache_id_t)allocator->cache_count;

    mutex_unlock(&allocator->lock);

    POUND_LOG_INFO(&thread_logger,
                   "Created slab cache \"%s\" as id %u: %zu-byte objects, %zu-byte stride, "
                   "%zu per slab.",
                   resolved.name,
                   (unsigned int)id,
                   resolved.object_size,
                   stride,
                   object_count);
    return id;
}

void
slab_allocator_cache_destroy(slab_allocator_t *POUND_RESTRICT allocator, slab_cache_id_t cache_id)
{
    if (POUND_UNLIKELY(NULL == allocator))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: allocator context is NULL.");
        return;
    }

    if (NULL == allocator->arena)
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: allocator was never initialised.");
        return;
    }

    mutex_lock(&allocator->lock);

    slab_cache_t *const cache = find_cache(allocator, cache_id);

    if (POUND_UNLIKELY(NULL == cache))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Ignoring call: cache %u does not exist.",
                        (unsigned int)cache_id);
        mutex_unlock(&allocator->lock);
        return;
    }

    if (0U != cache->in_use)
    {
        POUND_LOG_WARN(&thread_logger,
                       "Destroying cache \"%s\" with %zu objects still live.",
                       cache->config.name,
                       cache->in_use);
    }

    // Rewind whatever this cache left at the end of the arena. A slab buried
    // behind another cache's slabs cannot be reclaimed, and stays committed
    // until the trailing ones are released.
    trim_trailing(allocator, cache);

    // Detach whatever remains, which is necessarily the slabs the rewind could
    // not reach.
    for (int pass = 0; pass < 2; ++pass)
    {
        slab_t *slab = (0 == pass) ? cache->partial : cache->full;

        while (NULL != slab)
        {
            slab_t *const next = slab->next;

            allocator->object_count -= slab->object_count;
            allocator->slab_count--;
            allocator->in_use -= slab->in_use;
            allocator->bytes_committed -= slab->used_bytes;

            slab->cache = NULL;
            slab        = next;
        }

        if (0 == pass)
        {
            cache->partial = NULL;
        }
        else
        {
            cache->full = NULL;
        }
    }

    cache->in_use       = 0U;
    cache->object_count = 0U;
    cache->slab_count   = 0U;
    cache->starved      = false;

    // Compact the table so ids stay dense and repeated create/destroy cycles do
    // not grow without bound. This renumbers ids above `cache_id`; the header
    // documents that callers must not cache them across a destroy.
    const size_t index = (size_t)(cache_id - 1U);

    for (size_t i = index; (i + 1U) < allocator->cache_count; ++i)
    {
        allocator->caches[i] = allocator->caches[i + 1U];
    }

    allocator->cache_count--;
    allocator->caches[allocator->cache_count] = NULL;

    mutex_unlock(&allocator->lock);
}

void *
slab_allocator_alloc(slab_allocator_t *POUND_RESTRICT allocator, slab_cache_id_t cache_id)
{
    if (POUND_UNLIKELY(NULL == allocator))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: allocator context is NULL.");
        return NULL;
    }

    if (NULL == allocator->arena)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: allocator was never initialised.");
        return NULL;
    }

    mutex_lock(&allocator->lock);

    slab_cache_t *const cache = find_cache(allocator, cache_id);

    if (POUND_UNLIKELY(NULL == cache))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Allocation refused: cache %u does not exist.",
                        (unsigned int)cache_id);
        mutex_unlock(&allocator->lock);
        return NULL;
    }

    void *const object = take_object(allocator, cache);

    if (NULL == object)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Allocation of a %zu-byte object from cache \"%s\" failed: the arena "
                        "cannot hold another slab.",
                        cache->config.object_size,
                        cache->config.name);
    }

    mutex_unlock(&allocator->lock);
    return object;
}

void
slab_allocator_free(slab_allocator_t *POUND_RESTRICT allocator, void *POUND_RESTRICT object)
{
    if (POUND_UNLIKELY(NULL == allocator))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring function: allocator context is NULL.");
        return;
    }

    if (POUND_UNLIKELY(NULL == object))
    {
        // Matches free(NULL), which every allocator is required to treat as a
        // no-op rather than an error.
        return;
    }

    if (NULL == allocator->arena)
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring function: allocator was never initialised.");
        return;
    }

    mutex_lock(&allocator->lock);

    size_t index = 0U;
    slab_t *const slab = find_slab(allocator, object, &index);

    if (POUND_UNLIKELY(NULL == slab))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "free refused: %p belongs to no slab of this allocator; it is a foreign "
                        "pointer, an interior pointer, or an object whose slab was released.",
                        object);
        mutex_unlock(&allocator->lock);
        return;
    }

    slab_cache_t *const cache = slab->cache;

    // A released slab is off every cache's lists, so `find_slab` cannot return one.
    // Assert the invariant rather than trusting it, because dereferencing a NULL
    // cache below would be a crash in an emulator.
    if (POUND_UNLIKELY(NULL == cache))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "free refused: %p names slab %p, which is no longer owned by a cache.",
                        object,
                        (const void *)slab);
        mutex_unlock(&allocator->lock);
        return;
    }

    slab_object_t *header = NULL;

    if (POUND_UNLIKELY(POUND_SUCCESS != resolve_live_object(cache, slab, index, object, &header)))
    {
        mutex_unlock(&allocator->lock);
        return;
    }

    if (POUND_UNLIKELY(0U == slab->in_use))
    {
        // The magic and the bounds both check out but the slab claims nothing is
        // live, which means the counters and the magic have diverged.
        POUND_LOG_ERROR(&thread_logger,
                        "free refused: %p validates but slab %p reports zero live objects.",
                        object,
                        (const void *)slab);
        mutex_unlock(&allocator->lock);
        return;
    }

    // LIFO within a slab: the freed object becomes the head of the free list. That
    // is the cheapest possible path, and the resulting addresses depend only on
    // the allocation sequence, not on timing, which is what lets a save state
    // record guest object addresses and replay them.
    header->next_free = slab->free_head;
    header->magic     = SLAB_OBJECT_MAGIC_FREE;

    const bool was_full = (SLAB_FREE_HEAD_NONE == slab->free_head);

    slab->free_head = (uint32_t)index;
    slab->in_use--;
    cache->in_use--;
    cache->frees++;
    allocator->in_use--;

    if (was_full)
    {
        unlink_slab(cache, slab);
        slab->next     = cache->partial;
        cache->partial = slab;
    }

    poison_range(allocator, object, cache->config.object_size);

    mutex_unlock(&allocator->lock);
}

bool
slab_allocator_object_is_live(const slab_allocator_t *POUND_RESTRICT allocator,
                              const void *POUND_RESTRICT       object)
{
    if (POUND_UNLIKELY(NULL == allocator) || (POUND_UNLIKELY(NULL == object)))
    {
        return false;
    }

    if (NULL == allocator->arena)
    {
        return false;
    }

    mutex_t *const lock = (mutex_t *)(uintptr_t)(void *)&allocator->lock;

    mutex_lock(lock);

    size_t index = 0U;
    slab_t *const slab = find_slab(allocator, object, &index);

    bool live = false;

    if ((NULL != slab) && (NULL != slab->cache) && (index < slab->object_count))
    {
        const slab_cache_t *const cache = slab->cache;
        const slab_object_t *const header = object_header(cache, object);

        live = (SLAB_OBJECT_MAGIC_LIVE == header->magic) && (header->slab == slab);
    }

    mutex_unlock(lock);
    return live;
}

size_t
slab_allocator_get_usable_size(const slab_allocator_t *POUND_RESTRICT allocator,
                               const void *POUND_RESTRICT       object)
{
    if (POUND_UNLIKELY(NULL == allocator) || (POUND_UNLIKELY(NULL == object)))
    {
        return 0U;
    }

    if (NULL == allocator->arena)
    {
        return 0U;
    }

    mutex_t *const lock = (mutex_t *)(uintptr_t)(void *)&allocator->lock;

    mutex_lock(lock);

    size_t index = 0U;
    slab_t *const slab = find_slab(allocator, object, &index);

    // Resolvable for a live *and* a freed object: both are still carved by this
    // allocator, which is what a caller sizing a buffer out of its own storage
    // needs. It reports the cache's object size, not the slack up to the stride;
    // the slack exists only to keep the next object aligned.
    const size_t usable
        = ((NULL != slab) && (NULL != slab->cache) && (index < slab->object_count))
              ? slab->cache->config.object_size
              : 0U;

    mutex_unlock(lock);
    return usable;
}

size_t
slab_allocator_trim(slab_allocator_t *POUND_RESTRICT allocator, slab_cache_id_t cache_id)
{
    if (POUND_UNLIKELY(NULL == allocator))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: allocator context is NULL.");
        return 0U;
    }

    if (NULL == allocator->arena)
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: allocator was never initialised.");
        return 0U;
    }

    mutex_lock(&allocator->lock);

    slab_cache_t *const only = (SLAB_CACHE_ID_NONE == cache_id)
                                   ? NULL
                                   : find_cache(allocator, cache_id);

    if ((SLAB_CACHE_ID_NONE != cache_id) && (NULL == only))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Ignoring call: cache %u does not exist.",
                        (unsigned int)cache_id);
        mutex_unlock(&allocator->lock);
        return 0U;
    }

    const size_t released = trim_trailing(allocator, only);

    mutex_unlock(&allocator->lock);

    if (0U != released)
    {
        POUND_LOG_DEBUG(&thread_logger,
                        "Rewound the arena over %zu empty trailing slab(s).",
                        released);
    }

    return released;
}

error_t
slab_allocator_get_stats(const slab_allocator_t *POUND_RESTRICT allocator,
                         slab_stats_t *POUND_RESTRICT          out)
{
    if (POUND_UNLIKELY(NULL == allocator))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: allocator context is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(NULL == out))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: out is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (NULL == allocator->arena)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: allocator was never initialised.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    mutex_t *const lock = (mutex_t *)(uintptr_t)(void *)&allocator->lock;

    mutex_lock(lock);

    size_t starved = 0U;

    for (size_t i = 0U; i < allocator->cache_count; ++i)
    {
        if (allocator->caches[i]->starved)
        {
            starved++;
        }
    }

    out->cache_count      = allocator->cache_count;
    out->slab_count      = allocator->slab_count;
    out->object_count    = allocator->object_count;
    out->in_use          = allocator->in_use;
    out->bytes_committed = allocator->bytes_committed;
    out->arena_committed = allocator->bump;
    out->alloc_failures  = allocator->alloc_failures;
    out->starved_count   = starved;

    mutex_unlock(lock);
    return POUND_SUCCESS;
}

error_t
slab_allocator_cache_describe(const slab_allocator_t *POUND_RESTRICT allocator,
                              slab_cache_id_t                     cache_id,
                              slab_config_t *POUND_RESTRICT        out)
{
    if (POUND_UNLIKELY(NULL == allocator) || (POUND_UNLIKELY(NULL == out)))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: allocator or out is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (NULL == allocator->arena)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: allocator was never initialised.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    mutex_t *const lock = (mutex_t *)(uintptr_t)(void *)&allocator->lock;

    mutex_lock(lock);

    const slab_cache_t *const cache = find_cache(allocator, cache_id);

    if (POUND_UNLIKELY(NULL == cache))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: cache %u does not exist.",
                        (unsigned int)cache_id);
        mutex_unlock(lock);
        return POUND_ERROR_NOT_INITIALIZED;
    }

    *out = cache->config;

    mutex_unlock(lock);
    return POUND_SUCCESS;
}

size_t
slab_allocator_cache_in_use(const slab_allocator_t *POUND_RESTRICT allocator, slab_cache_id_t cache_id)
{
    if (POUND_UNLIKELY(NULL == allocator) || (POUND_UNLIKELY(NULL == allocator->arena)))
    {
        return 0U;
    }

    mutex_t *const lock = (mutex_t *)(uintptr_t)(void *)&allocator->lock;

    mutex_lock(lock);

    const slab_cache_t *const cache = find_cache(allocator, cache_id);
    const size_t              live  = (NULL != cache) ? cache->in_use : 0U;

    mutex_unlock(lock);
    return live;
}

bool
slab_allocator_cache_is_starved(const slab_allocator_t *POUND_RESTRICT allocator,
                                slab_cache_id_t cache_id)
{
    if (POUND_UNLIKELY(NULL == allocator) || (POUND_UNLIKELY(NULL == allocator->arena)))
    {
        return false;
    }

    mutex_t *const lock = (mutex_t *)(uintptr_t)(void *)&allocator->lock;

    mutex_lock(lock);

    const slab_cache_t *const cache = find_cache(allocator, cache_id);
    const bool               starved = (NULL != cache) && cache->starved;

    mutex_unlock(lock);
    return starved;
}

void
slab_allocator_set_poison(slab_allocator_t *POUND_RESTRICT allocator, bool enabled)
{
    if (POUND_UNLIKELY(NULL == allocator))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: allocator context is NULL.");
        return;
    }

    mutex_lock(&allocator->lock);
    allocator->poison_enabled = enabled;
    mutex_unlock(&allocator->lock);
}

const char *
slab_allocator_cache_name(const slab_allocator_t *POUND_RESTRICT allocator, slab_cache_id_t cache_id)
{
    if (POUND_UNLIKELY(NULL == allocator) || (POUND_UNLIKELY(NULL == allocator->arena)))
    {
        return "";
    }

    mutex_t *const lock = (mutex_t *)(uintptr_t)(void *)&allocator->lock;

    mutex_lock(lock);

    const slab_cache_t *const cache = find_cache(allocator, cache_id);
    const char *const      name   = (NULL != cache) ? cache->config.name : "";

    mutex_unlock(lock);
    return name;
}

/*** end of file ***/
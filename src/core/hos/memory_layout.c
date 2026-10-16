#include "memory_layout.h"
#include "log.h"
#include <string.h>

/// Distinguishes an initialised layout from zeroed memory. Not a security
/// measure: it catches a caller using a layout before `hos_memory_layout_init`,
/// which would otherwise look like an address space with no regions rather than
/// like a bug.
#define HOS_MEMORY_LAYOUT_MAGIC 0x484F534CU

/// Rounds `value` up to the next multiple of `alignment`, which must be a power
/// of two. Callers keep `value` inside a window, so the add cannot overflow.
static uint64_t
align_up(const uint64_t value, const uint64_t alignment)
{
    return (value + (alignment - 1U)) & ~(alignment - 1U);
}

/// Index of the region containing `address`, or -1.
///
/// Linear because the table is at most 32 entries and a process has a handful
/// of regions: a binary search would need the sorted order that a linear scan
/// gets for free. Regions are page-aligned and half-open, so an address exactly
/// on a boundary belongs to the region that starts there.
static int
layout_find_index(const hos_memory_layout_t *POUND_RESTRICT layout, const uint64_t address)
{
    for (uint32_t index = 0U; index < layout->region_count; index++)
    {
        const guest_memory_region_t *const region = &layout->regions[index];

        if ((address >= region->base) && ((address - region->base) < region->size))
        {
            return (int)index;
        }
    }

    return -1;
}

/// Writes the region count back into the window whose gate reads it.
///
/// Every mutation of the table has to go through here: the layout's count is
/// the truth, but the gate in `guest_memory.c` reads the copy in
/// `guest_memory_t`, and a missed copy would leave the gate looking at a
/// suffix of the table.
static void
layout_publish_region_count(hos_memory_layout_t *POUND_RESTRICT layout)
{
    if (layout->bound_memory != NULL)
    {
        layout->bound_memory->region_count = layout->region_count;
    }
}

/// Answers the index of the first and last region covering `[base, end)`, or a
/// refusal when the range is not inside the address space.
///
/// `base < end` and both are page-aligned, which the callers guarantee, so
/// `end - 1` is the range's last byte and lands in its last region. The table
/// tiles the address space without gaps, so a range inside it always covers at
/// least one region.
static error_t
layout_covered_range(const hos_memory_layout_t *POUND_RESTRICT layout,
                     const uint64_t                      base,
                     const uint64_t                      end,
                     uint32_t *POUND_RESTRICT            out_first,
                     uint32_t *POUND_RESTRICT            out_last)
{
    if ((base < layout->address_space_start) || (end > layout->address_space_end))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory range [0x%llx, 0x%llx): outside the address space [0x%llx, 0x%llx).",
                        (unsigned long long)base,
                        (unsigned long long)end,
                        (unsigned long long)layout->address_space_start,
                        (unsigned long long)layout->address_space_end);
        return POUND_ERROR_GUEST_ADDRESS_OUT_OF_BOUNDS;
    }

    const int first = layout_find_index(layout, base);
    const int last  = layout_find_index(layout, end - 1U);

    if ((first < 0) || (last < 0))
    {
        // Unreachable: a range inside the address space is tiled, so both ends
        // land in a region. Reported rather than hidden, because a table that is
        // not tiling is a bug in whatever wrote it.
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory range [0x%llx, 0x%llx): the address space is not fully tiled.",
                        (unsigned long long)base,
                        (unsigned long long)end);
        return POUND_ERROR_CORRUPTED;
    }

    *out_first = (uint32_t)first;
    *out_last  = (uint32_t)last;
    return POUND_SUCCESS;
}

/// Reports whether every region in `[first, last]` agrees with the first about
/// state, permission and attribute.
///
/// This is Horizon's `CheckMemoryState` rule. The kernel walks the whole range
/// and demands one configuration before it changes any of it, so reprotecting
/// across a boundary between two differently-configured regions is refused
/// rather than half-done.
static bool
layout_range_is_homogeneous(const hos_memory_layout_t *POUND_RESTRICT layout,
                            const uint32_t                      first,
                            const uint32_t                      last)
{
    const guest_memory_region_t *const head = &layout->regions[first];

    for (uint32_t index = first + 1U; index <= last; index++)
    {
        const guest_memory_region_t *const region = &layout->regions[index];

        if ((region->state != head->state) || (region->permission != head->permission) ||
            (region->attribute != head->attribute))
        {
            return false;
        }
    }

    return true;
}

/// Splits the region containing `address` at `address`, so `address` becomes a
/// region boundary, and answers the index of the region starting there.
///
/// A boundary that is already present is left alone. Both halves keep the
/// original's state, permission, attribute and counts: a split changes the shape
/// of the table, never what it says about any byte.
static error_t
layout_split_at(hos_memory_layout_t *POUND_RESTRICT layout,
                const uint64_t                      address,
                uint32_t *POUND_RESTRICT            out_index)
{
    const int index = layout_find_index(layout, address);

    if (index < 0)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout split at 0x%llx: the address space is not fully tiled.",
                        (unsigned long long)address);
        return POUND_ERROR_CORRUPTED;
    }

    const uint32_t at = (uint32_t)index;

    if (layout->regions[at].base == address)
    {
        *out_index = at;
        return POUND_SUCCESS;
    }

    if (layout->region_count >= HOS_MEMORY_LAYOUT_MAX_REGIONS)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout split at 0x%llx: the region table is full (%u entries).",
                        (unsigned long long)address,
                        (unsigned int)layout->region_count);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    const guest_memory_region_t head = layout->regions[at];
    const uint32_t              tail = layout->region_count - (at + 1U);

    if (tail > 0U)
    {
        memmove(&layout->regions[at + 2U],
                &layout->regions[at + 1U],
                (size_t)tail * sizeof(layout->regions[0]));
    }

    layout->regions[at]         = head;
    layout->regions[at].size    = address - head.base;
    layout->regions[at + 1U]    = head;
    layout->regions[at + 1U].base = address;
    layout->regions[at + 1U].size = (head.base + head.size) - address;

    layout->region_count++;
    layout_publish_region_count(layout);

    *out_index = at + 1U;
    return POUND_SUCCESS;
}

/// Ensures region boundaries at `base` and `end`, so `[base, end)` is a whole
/// number of regions, and answers the index of the region starting at `base`.
///
/// `first` and `last` are the indices covering the range before any split, from
/// `layout_covered_range`. The table is checked for room for both splits before
/// either is made, so a range that does not fit is refused without being half
/// split. `end` at the end of the address space needs no boundary: the last
/// region already ends there.
static error_t
layout_split_range(hos_memory_layout_t *POUND_RESTRICT layout,
                   const uint64_t                      base,
                   const uint64_t                      end,
                   const uint32_t                      first,
                   const uint32_t                      last,
                   uint32_t *POUND_RESTRICT            out_at)
{
    const uint32_t splits = (uint32_t)(layout->regions[first].base != base) +
                            (uint32_t)((layout->regions[last].base + layout->regions[last].size) != end);

    if ((layout->region_count + splits) > HOS_MEMORY_LAYOUT_MAX_REGIONS)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory range [0x%llx, 0x%llx): the region table is full (%u entries).",
                        (unsigned long long)base,
                        (unsigned long long)end,
                        (unsigned int)layout->region_count);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    error_t error = layout_split_at(layout, base, out_at);

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    if (end < layout->address_space_end)
    {
        uint32_t unused = 0U;

        error = layout_split_at(layout, end, &unused);

        if (error != POUND_SUCCESS)
        {
            return error;
        }
    }

    return POUND_SUCCESS;
}

error_t
hos_memory_layout_init(hos_memory_layout_t *POUND_RESTRICT layout,
                       guest_memory_t *POUND_RESTRICT      guest_memory,
                       const uint64_t                      guest_base,
                       const uint64_t                      guest_end)
{
    if ((layout == NULL) || (guest_memory == NULL))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout init: layout=%p, guest_memory=%p.",
                        (const void *)layout,
                        (const void *)guest_memory);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (layout->magic == HOS_MEMORY_LAYOUT_MAGIC)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout init: the layout is already initialised.");
        return POUND_ERROR_ALREADY_INITIALIZED;
    }

    if (((guest_base % HOS_MEMORY_PAGE_SIZE) != 0U) || ((guest_end % HOS_MEMORY_PAGE_SIZE) != 0U))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout init: [0x%llx, 0x%llx) is not page-aligned.",
                        (unsigned long long)guest_base,
                        (unsigned long long)guest_end);
        return POUND_ERROR_MEMORY_ALIGNMENT;
    }

    // Two non-empty halves: a window under two pages could not be split into a
    // map region and a heap region, and silently producing an empty heap region
    // would turn every `svcSetHeapSize` into a refusal with no explanation.
    if ((guest_end <= guest_base) || ((guest_end - guest_base) < (2U * HOS_MEMORY_PAGE_SIZE)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout init: [0x%llx, 0x%llx) is not at least two pages.",
                        (unsigned long long)guest_base,
                        (unsigned long long)guest_end);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const uint64_t window_base = (uint64_t)guest_memory->guest_base;
    const uint64_t window_end  = (uint64_t)guest_memory->guest_end;

    if ((guest_base < window_base) || (guest_end > window_end))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout init: [0x%llx, 0x%llx) is not inside the guest window "
                        "[0x%llx, 0x%llx).",
                        (unsigned long long)guest_base,
                        (unsigned long long)guest_end,
                        (unsigned long long)window_base,
                        (unsigned long long)window_end);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    // Horizon's type-2 address space puts the map region in the lower half and
    // the heap region in the upper half. Reproduce that split, rounding the
    // boundary up so the heap region never starts mid-page.
    const uint64_t midpoint  = guest_base + ((guest_end - guest_base) / 2U);
    const uint64_t heap_base = align_up(midpoint, HOS_MEMORY_PAGE_SIZE);
    const uint64_t map_size  = heap_base - guest_base;
    const uint64_t heap_size = guest_end - heap_base;

    if ((map_size < HOS_MEMORY_PAGE_SIZE) || (heap_size < HOS_MEMORY_PAGE_SIZE))
    {
        // Defensive: the two-page minimum above makes this unreachable, and it
        // is here so that a future relaxation of that check cannot produce a
        // zero-sized region.
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout init: the midpoint 0x%llx does not split [0x%llx, 0x%llx) "
                        "into two non-empty halves.",
                        (unsigned long long)heap_base,
                        (unsigned long long)guest_base,
                        (unsigned long long)guest_end);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    memset(layout, 0, sizeof(*layout));

    layout->magic              = HOS_MEMORY_LAYOUT_MAGIC;
    layout->address_space_start = guest_base;
    layout->address_space_end   = guest_end;
    layout->heap_base           = heap_base;
    layout->heap_region_size    = heap_size;
    layout->heap_size           = 0U;
    layout->region_count        = 2U;
    layout->bound_memory        = guest_memory;

    // Both halves start `Free` with no permission: nothing may touch a layout
    // that has only been initialised, exactly as on hardware, until a loader
    // maps something into the map region.
    layout->regions[0] = (guest_memory_region_t){ .base          = guest_base,
                                                  .size          = map_size,
                                                  .permission    = 0U,
                                                  .state         = (uint32_t)HOS_MEMORY_STATE_FREE,
                                                  .attribute     = 0U,
                                                  .ipc_count     = 0U,
                                                  .device_count  = 0U };

    layout->regions[1] = (guest_memory_region_t){ .base          = heap_base,
                                                  .size          = heap_size,
                                                  .permission    = 0U,
                                                  .state         = (uint32_t)HOS_MEMORY_STATE_FREE,
                                                  .attribute     = 0U,
                                                  .ipc_count     = 0U,
                                                  .device_count  = 0U };

    // Turn the gate on: from here on every access through this window is checked
    // against the table above.
    guest_memory->regions      = layout->regions;
    guest_memory->region_count = layout->region_count;
    return POUND_SUCCESS;
}

error_t
hos_memory_layout_map(hos_memory_layout_t *POUND_RESTRICT layout,
                      const uint64_t                      base,
                      const uint64_t                      size,
                      const uint32_t                      state,
                      const uint32_t                      permission,
                      const uint32_t                      attribute)
{
    if (layout == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout map: layout is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (layout->magic != HOS_MEMORY_LAYOUT_MAGIC)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout map: the layout is not initialised.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    if (size == 0U)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout map at 0x%llx: zero bytes.", (unsigned long long)base);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (((base % HOS_MEMORY_PAGE_SIZE) != 0U) || ((size % HOS_MEMORY_PAGE_SIZE) != 0U))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout map: [0x%llx, 0x%llx) is not page-aligned.",
                        (unsigned long long)base,
                        (unsigned long long)(base + size));
        return POUND_ERROR_MEMORY_ALIGNMENT;
    }

    if (base > (UINT64_MAX - size))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout map at 0x%llx: size 0x%llx overflows the address space.",
                        (unsigned long long)base,
                        (unsigned long long)size);
        return POUND_ERROR_GUEST_ADDRESS_OVERFLOW;
    }

    const uint64_t end = base + size;

    if ((base < layout->address_space_start) || (end > layout->address_space_end))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout map: [0x%llx, 0x%llx) is outside the address space "
                        "[0x%llx, 0x%llx).",
                        (unsigned long long)base,
                        (unsigned long long)end,
                        (unsigned long long)layout->address_space_start,
                        (unsigned long long)layout->address_space_end);
        return POUND_ERROR_GUEST_ADDRESS_OUT_OF_BOUNDS;
    }

    // The heap region belongs to `svcSetHeapSize` and nothing else. Horizon's
    // `CanContain` refuses a mapping that overlaps it, which is why its
    // `SetHeapSize` documents `0xD401` for a heap region that has been mapped
    // into; refusing here is what keeps that result unreachable rather than
    // letting a loader silently corrupt the heap.
    const uint64_t heap_end = layout->heap_base + layout->heap_region_size;

    if ((base < heap_end) && (end > layout->heap_base))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout map: [0x%llx, 0x%llx) overlaps the heap region "
                        "[0x%llx, 0x%llx), which only svcSetHeapSize may commit.",
                        (unsigned long long)base,
                        (unsigned long long)end,
                        (unsigned long long)layout->heap_base,
                        (unsigned long long)heap_end);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const int index = layout_find_index(layout, base);

    if (index < 0)
    {
        // The table tiles the address space without gaps, so an address inside
        // it always lands in a region; landing in none means the table has been
        // corrupted, not that the caller asked for something wrong.
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout map at 0x%llx: the address space is not fully tiled.",
                        (unsigned long long)base);
        return POUND_ERROR_CORRUPTED;
    }

    const guest_memory_region_t target = layout->regions[index];

    if (target.state != (uint32_t)HOS_MEMORY_STATE_FREE)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout map at 0x%llx: the region [0x%llx, 0x%llx) is state 0x%X, "
                        "not Free.",
                        (unsigned long long)base,
                        (unsigned long long)target.base,
                        (unsigned long long)(target.base + target.size),
                        (unsigned int)target.state);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const uint64_t target_end = target.base + target.size;

    if (end > target_end)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout map: [0x%llx, 0x%llx) spans the end of the Free region "
                        "[0x%llx, 0x%llx).",
                        (unsigned long long)base,
                        (unsigned long long)end,
                        (unsigned long long)target.base,
                        (unsigned long long)target_end);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const uint64_t left_size  = base - target.base;
    const uint64_t right_size = target_end - end;
    const uint32_t growth     = (uint32_t)(left_size > 0U ? 1U : 0U) + (uint32_t)(right_size > 0U ? 1U : 0U);

    if ((layout->region_count + growth) > HOS_MEMORY_LAYOUT_MAX_REGIONS)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout map at 0x%llx: the region table is full (%u entries).",
                        (unsigned long long)base,
                        (unsigned int)layout->region_count);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    const uint32_t index_u32 = (uint32_t)index;
    const uint32_t tail      = layout->region_count - (index_u32 + 1U);

    if (growth > 0U)
    {
        memmove(&layout->regions[index_u32 + 1U + growth],
                &layout->regions[index_u32 + 1U],
                (size_t)tail * sizeof(layout->regions[0]));
    }

    // `target` is a copy: the writes below overwrite the entry it came from.
    uint32_t at = index_u32;

    if (left_size > 0U)
    {
        layout->regions[at] = (guest_memory_region_t){ .base         = target.base,
                                                       .size         = left_size,
                                                       .permission   = 0U,
                                                       .state        = (uint32_t)HOS_MEMORY_STATE_FREE,
                                                       .attribute    = 0U,
                                                       .ipc_count    = 0U,
                                                       .device_count = 0U };
        at++;
    }

    layout->regions[at] = (guest_memory_region_t){ .base         = base,
                                                   .size         = size,
                                                   .permission   = permission,
                                                   .state        = state,
                                                   .attribute    = attribute,
                                                   .ipc_count    = 0U,
                                                   .device_count = 0U };
    at++;

    if (right_size > 0U)
    {
        layout->regions[at] = (guest_memory_region_t){ .base         = end,
                                                       .size         = right_size,
                                                       .permission   = 0U,
                                                       .state        = (uint32_t)HOS_MEMORY_STATE_FREE,
                                                       .attribute    = 0U,
                                                       .ipc_count    = 0U,
                                                       .device_count = 0U };
    }

    layout->region_count += growth;
    layout_publish_region_count(layout);
    return POUND_SUCCESS;
}

error_t
hos_memory_layout_set_heap_size(hos_memory_layout_t *POUND_RESTRICT layout, const uint64_t size)
{
    if (layout == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout heap resize: layout is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (layout->magic != HOS_MEMORY_LAYOUT_MAGIC)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout heap resize: the layout is not initialised.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    if ((size % HOS_MEMORY_PAGE_SIZE) != 0U)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout heap resize to 0x%llx: not page-aligned.",
                        (unsigned long long)size);
        return POUND_ERROR_MEMORY_ALIGNMENT;
    }

    if (size > layout->heap_region_size)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout heap resize to 0x%llx: the heap region is only 0x%llx.",
                        (unsigned long long)size,
                        (unsigned long long)layout->heap_region_size);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const uint64_t span_start = layout->heap_base;
    const uint64_t span_end   = layout->heap_base + layout->heap_region_size;

    // The heap region is non-empty (`hos_memory_layout_init` guarantees it) and
    // the table tiles the address space, so both ends of the span land in a
    // region.
    const int first = layout_find_index(layout, span_start);
    const int last  = layout_find_index(layout, span_end - 1U);

    if ((first < 0) || (last < 0) || (layout->regions[first].base != span_start))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout heap resize: the heap region [0x%llx, 0x%llx) is not a "
                        "region boundary.",
                        (unsigned long long)span_start,
                        (unsigned long long)span_end);
        return POUND_ERROR_CORRUPTED;
    }

    const uint32_t replaced    = (uint32_t)(last - first + 1);
    const uint32_t new_entries = (uint32_t)(size > 0U ? 1U : 0U) + (uint32_t)(size < layout->heap_region_size ? 1U : 0U);
    const uint32_t new_count   = layout->region_count - replaced + new_entries;

    if (new_count > HOS_MEMORY_LAYOUT_MAX_REGIONS)
    {
        // Reachable only with the table nearly full and the heap split across
        // two entries; the system call turns this into `0xCE01`.
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout heap resize to 0x%llx: the region table is full (%u entries).",
                        (unsigned long long)size,
                        (unsigned int)layout->region_count);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    const uint32_t tail = layout->region_count - (uint32_t)(last + 1);

    if (tail > 0U)
    {
        memmove(&layout->regions[(uint32_t)first + new_entries],
                &layout->regions[(uint32_t)last + 1U],
                (size_t)tail * sizeof(layout->regions[0]));
    }

    uint32_t at = (uint32_t)first;

    if (size > 0U)
    {
        layout->regions[at] = (guest_memory_region_t){ .base         = span_start,
                                                       .size         = size,
                                                       .permission   = GUEST_MEMORY_PERM_READ | GUEST_MEMORY_PERM_WRITE,
                                                       .state        = (uint32_t)HOS_MEMORY_STATE_NORMAL,
                                                       .attribute    = 0U,
                                                       .ipc_count    = 0U,
                                                       .device_count = 0U };
        at++;
    }

    if (size < layout->heap_region_size)
    {
        layout->regions[at] = (guest_memory_region_t){ .base         = span_start + size,
                                                       .size         = layout->heap_region_size - size,
                                                       .permission   = 0U,
                                                       .state        = (uint32_t)HOS_MEMORY_STATE_FREE,
                                                       .attribute    = 0U,
                                                       .ipc_count    = 0U,
                                                       .device_count = 0U };
    }

    layout->region_count = new_count;
    layout->heap_size    = size;
    layout_publish_region_count(layout);
    return POUND_SUCCESS;
}

error_t
hos_memory_layout_set_permission(hos_memory_layout_t *POUND_RESTRICT layout,
                                 const uint64_t                      base,
                                 const uint64_t                      size,
                                 const uint32_t                      permission)
{
    if (layout == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout reprotect: layout is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (layout->magic != HOS_MEMORY_LAYOUT_MAGIC)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout reprotect: the layout is not initialised.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    if (size == 0U)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout reprotect at 0x%llx: zero bytes.",
                        (unsigned long long)base);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (((base % HOS_MEMORY_PAGE_SIZE) != 0U) || ((size % HOS_MEMORY_PAGE_SIZE) != 0U))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout reprotect: [0x%llx, 0x%llx) is not page-aligned.",
                        (unsigned long long)base,
                        (unsigned long long)(base + size));
        return POUND_ERROR_MEMORY_ALIGNMENT;
    }

    if (base > (UINT64_MAX - size))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout reprotect at 0x%llx: size 0x%llx overflows the address space.",
                        (unsigned long long)base,
                        (unsigned long long)size);
        return POUND_ERROR_GUEST_ADDRESS_OVERFLOW;
    }

    const uint64_t end = base + size;

    uint32_t first = 0U;
    uint32_t last  = 0U;

    error_t error = layout_covered_range(layout, base, end, &first, &last);

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    if (!layout_range_is_homogeneous(layout, first, last))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout reprotect of [0x%llx, 0x%llx): the range is not a single "
                        "configuration.",
                        (unsigned long long)base,
                        (unsigned long long)end);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const guest_memory_region_t *const head = &layout->regions[first];

    if (((head->state & HOS_MEMORY_STATE_FLAG_CAN_REPROTECT) == 0U) || (head->attribute != 0U))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout reprotect of [0x%llx, 0x%llx): state 0x%X with attribute 0x%X "
                        "may not be reprotected.",
                        (unsigned long long)base,
                        (unsigned long long)end,
                        (unsigned int)head->state,
                        (unsigned int)head->attribute);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (head->permission == permission)
    {
        // Horizon short-circuits a reprotection that changes nothing, before it
        // touches the block table -- so this answers success even for a range
        // that would have needed splitting.
        return POUND_SUCCESS;
    }

    uint32_t at = 0U;
    error = layout_split_range(layout, base, end, first, last, &at);

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    for (uint32_t index = at; (index < layout->region_count) && (layout->regions[index].base < end); index++)
    {
        layout->regions[index].permission = permission;
    }

    return POUND_SUCCESS;
}

error_t
hos_memory_layout_set_attribute(hos_memory_layout_t *POUND_RESTRICT layout,
                                const uint64_t                      base,
                                const uint64_t                      size,
                                const uint32_t                      mask,
                                const uint32_t                      attribute)
{
    if (layout == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout attribute change: layout is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (layout->magic != HOS_MEMORY_LAYOUT_MAGIC)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting memory layout attribute change: the layout is not initialised.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    if (size == 0U)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout attribute change at 0x%llx: zero bytes.",
                        (unsigned long long)base);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (((base % HOS_MEMORY_PAGE_SIZE) != 0U) || ((size % HOS_MEMORY_PAGE_SIZE) != 0U))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout attribute change: [0x%llx, 0x%llx) is not page-aligned.",
                        (unsigned long long)base,
                        (unsigned long long)(base + size));
        return POUND_ERROR_MEMORY_ALIGNMENT;
    }

    if (base > (UINT64_MAX - size))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout attribute change at 0x%llx: size 0x%llx overflows the address "
                        "space.",
                        (unsigned long long)base,
                        (unsigned long long)size);
        return POUND_ERROR_GUEST_ADDRESS_OVERFLOW;
    }

    const uint64_t end = base + size;

    uint32_t first = 0U;
    uint32_t last  = 0U;

    error_t error = layout_covered_range(layout, base, end, &first, &last);

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    if (!layout_range_is_homogeneous(layout, first, last))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout attribute change of [0x%llx, 0x%llx): the range is not a single "
                        "configuration.",
                        (unsigned long long)base,
                        (unsigned long long)end);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const guest_memory_region_t *const head = &layout->regions[first];

    // A region that already carries an attribute the caller is not masking is not
    // eligible: Horizon refuses to change some bits of a block that has others
    // set, rather than changing a subset of them.
    if ((head->attribute & ~mask) != 0U)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout attribute change of [0x%llx, 0x%llx): attribute 0x%X carries bits "
                        "the mask 0x%X does not.",
                        (unsigned long long)base,
                        (unsigned long long)end,
                        (unsigned int)head->attribute,
                        (unsigned int)mask);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (((mask & HOS_MEMORY_ATTRIBUTE_UNCACHED) != 0U) &&
        ((head->state & HOS_MEMORY_STATE_FLAG_CAN_CHANGE_ATTRIBUTE) == 0U))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting memory layout attribute change of [0x%llx, 0x%llx): state 0x%X may not have "
                        "its attribute changed.",
                        (unsigned long long)base,
                        (unsigned long long)end,
                        (unsigned int)head->state);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const uint32_t new_attribute = (head->attribute & ~mask) | (attribute & mask);

    if (head->attribute == new_attribute)
    {
        return POUND_SUCCESS;
    }

    uint32_t at = 0U;
    error = layout_split_range(layout, base, end, first, last, &at);

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    for (uint32_t index = at; (index < layout->region_count) && (layout->regions[index].base < end); index++)
    {
        layout->regions[index].attribute = (layout->regions[index].attribute & ~mask) | (attribute & mask);
    }

    return POUND_SUCCESS;
}

const guest_memory_region_t *
hos_memory_layout_find(const hos_memory_layout_t *POUND_RESTRICT layout, const uint64_t address)
{
    if ((layout == NULL) || (layout->magic != HOS_MEMORY_LAYOUT_MAGIC))
    {
        return NULL;
    }

    if ((address < layout->address_space_start) || (address >= layout->address_space_end))
    {
        return NULL;
    }

    const int index = layout_find_index(layout, address);
    return (index < 0) ? NULL : &layout->regions[index];
}

/*** end of file ***/

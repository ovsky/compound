//! Horizon OS memory layout: regions, their rights, and the process heap.
//!
//! Guest memory in Pound is one flat host buffer (`guest_memory_t`) with no
//! inherent structure: any address in the window translates to any byte.
//! Horizon is the opposite. A process's address space is carved into regions
//! whose *state* (`Free`, `Code`, `Normal`, `Stack`, ...) and *permission*
//! decide what the guest may do where, and `svcQueryMemory` exists precisely so
//! a guest can ask. Without a region table a title that maps its own code,
//! reads back what it mapped, or sets up a heap cannot get a true answer, and
//! an emulator that answers every query with "one big readable/writable region"
//! is telling the guest something the hardware would not.
//!
//! This module owns that table. It is the single writer of the
//! `guest_memory_region_t` array that `guest_memory_t` points at, so the two
//! views -- the access gate inside `guest_memory.c` and the `svcQueryMemory`
//! answer built from the same structs -- can never disagree about where a
//! region starts, ends, or what it permits.
//!
//! # The shape of a process address space
//!
//! Horizon gives a process a *type-2* address space whose lower half is the
//! **map region** (where the loader and the title map code, data, stacks and
//! shared memory) and whose upper half is the **heap region**
//! (`svcSetHeapSize` grows the heap there, and nothing else may be mapped into
//! it). `hos_memory_layout_init` reproduces exactly that split: it partitions
//! the window it is given at the midpoint, rounded up to a page, and both
//! halves start out `Free`.
//!
//! `Free` means exactly what it means on hardware: no access at all. A region
//! whose permission is zero faults on every touch, which is why a freshly
//! laid-out address space is inaccessible until a loader maps something into
//! it with `hos_memory_layout_map`.
//!
//! # Where the numbers come from
//!
//! The `HOS_MEMORY_STATE_*` values are the 4.0.0+ `MemoryState` encodings a
//! guest compares against when it reads a `MemoryInfo` back. They are written
//! as literals, with the pre-4.0.0 spelling of the ones that changed shown
//! beside them, for the same reason `syscall.h` writes its `Result` words as
//! literals: the encoding carries flag bits in the middle of the value, so a
//! value re-derived from the module/description bit layout would be off by
//! exactly those bits and still look plausible.
//!
//! The `guest_memory_region_t.permission` values are `GUEST_MEMORY_PERM_*`,
//! whose numeric layout is Horizon's `MemoryPermission` (bit 0 read, bit 1
//! write, bit 2 execute), so the same numbers a loader would compute for a
//! mapping are the numbers the access gate tests.

#ifndef POUND_HOS_MEMORY_LAYOUT_H
#define POUND_HOS_MEMORY_LAYOUT_H

#include "attributes.h"
#include "errors.h"
#include "memory/guest_memory.h"
#include <stdint.h>

/// Horizon's page size: the granularity of every region boundary.
#define HOS_MEMORY_PAGE_SIZE 0x1000ULL

/// Largest heap `svcSetHeapSize` accepts, from the homebrew wiki: `[2.0.0+]`
/// size must be less than or equal to 4GB. A larger request is `0xCA01`.
#define HOS_MEMORY_HEAP_SIZE_MAX 0x100000000ULL

/// Granularity `svcSetHeapSize` requires of its size: a multiple of 2MB. A
/// request that is not is `0xCA01`.
#define HOS_MEMORY_HEAP_GRANULARITY 0x200000ULL

/// The most regions one layout can describe.
///
/// Horizon itself is bounded by a slab allocator for memory blocks, not by a
/// fixed count; Pound's table has to be finite, so it is sized well past what a
/// real loader maps (code, rodata, data, bss, a couple of stacks, a few shared
/// mappings) and the operations that would overflow it refuse rather than
/// corrupt: `hos_memory_layout_map` returns an error, and `svcSetHeapSize`
/// answers the homebrew wiki's documented `0xCE01` ("KMemoryBlockAllocator slab
/// allocator exhausted") rather than an emulator error, because running out of
/// memory-block bookkeeping is exactly what `0xCE01` reports on hardware.
#define HOS_MEMORY_LAYOUT_MAX_REGIONS 32U

/// Horizon 4.0.0+ `MemoryState` encodings.
///
/// A `MemoryInfo` carries one of these in its `state` field. The comments name
/// the pre-4.0.0 value where it differs, because a title built against older
/// firmware may compare against either.
typedef enum
{
    HOS_MEMORY_STATE_FREE = 0x0U,

    /// Address space the kernel does not consider part of the process. A
    /// `svcQueryMemory` for an address outside the address space answers with
    /// this state and the fake block described in `hos_memory_layout_find`.
    HOS_MEMORY_STATE_INACCESSIBLE = 0x10U,

    HOS_MEMORY_STATE_IO                     = 0x2001U,
    HOS_MEMORY_STATE_KERNEL                 = 0x2013U,
    HOS_MEMORY_STATE_COVERAGE               = 0x2016U,
    HOS_MEMORY_STATE_STATIC                 = 0x42002U,
    HOS_MEMORY_STATE_THREAD_LOCAL           = 0x40200CU,
    HOS_MEMORY_STATE_SHARED                 = 0x402006U,
    HOS_MEMORY_STATE_SHARED_CODE            = 0x40380FU,
    HOS_MEMORY_STATE_GENERATED_CODE         = 0x402214U,
    HOS_MEMORY_STATE_CODE_OUT               = 0x402015U,
    HOS_MEMORY_STATE_ALIAS                  = 0x482907U,
    HOS_MEMORY_STATE_NON_DEVICE_IPC         = 0x4C2812U,
    HOS_MEMORY_STATE_IPC                    = 0x5C3C0AU,
    HOS_MEMORY_STATE_STACK                  = 0x5C3C0BU,
    HOS_MEMORY_STATE_TRANSFERED             = 0x15C3C0DU,
    HOS_MEMORY_STATE_SHARED_TRANSFERED      = 0x5C380EU,
    HOS_MEMORY_STATE_NON_SECURE_IPC         = 0x5C3811U,
    HOS_MEMORY_STATE_INSECURE               = 0x5583817U,

    /// Executable code. Pre-4.0.0, `Code` did not carry the flag bits and was
    /// `0xA03`.
    HOS_MEMORY_STATE_CODE = 0xDC7E03U,

    /// Code paired with data...
    HOS_MEMORY_STATE_CODE_DATA = 0x1FEBD04U,

    /// Alias of code...
    HOS_MEMORY_STATE_ALIAS_CODE = 0xDD7E08U,

    /// Alias of code/data...
    HOS_MEMORY_STATE_ALIAS_CODE_DATA = 0x1FFBD09U,

    /// Writable data, and what `svcSetHeapSize` commits a heap as. Pre-4.0.0
    /// this was `0x1EBD05` / `0x3EBD05`; Pound uses the `+0x20000000` spelling.
    HOS_MEMORY_STATE_NORMAL = 0x37EBD05U,
} hos_memory_state_t;

/// A process address space: the region table, the heap region, and the heap
/// inside it.
///
/// The `regions` array is the storage `guest_memory_t` points at once this
/// layout is bound; this struct must outlive every access that goes through
/// that window. `bound_memory` is kept so that a mutation can resynchronise the
/// `region_count` the gate reads -- the two counts are the same number, and
/// forgetting to copy it back would leave the gate reading a stale table.
///
/// The layout is plain data with no allocation: a caller embeds one wherever it
/// keeps process state.
typedef struct
{
    uint32_t magic;

    /// Live entries in `regions`.
    uint32_t region_count;

    /// First and one-past-last guest address of the address space.
    uint64_t address_space_start;
    uint64_t address_space_end;

    /// Where the heap region starts and how big it is. `heap_base` is a region
    /// boundary and no other region may cross it.
    uint64_t heap_base;
    uint64_t heap_region_size;

    /// How much of the heap region is currently committed as `Normal` readable
    /// and writable. The committed part is always `[heap_base, heap_base +
    /// heap_size)`; the rest of the region is `Free`.
    uint64_t heap_size;

    /// The window this table is installed in, so a mutation can keep its
    /// `region_count` in step. Set by `hos_memory_layout_init`.
    guest_memory_t *bound_memory;

    guest_memory_region_t regions[HOS_MEMORY_LAYOUT_MAX_REGIONS];
} hos_memory_layout_t;

/// Partitions `[guest_base, guest_end)` into a map region and a heap region and
/// installs the result into `guest_memory`.
///
/// Both halves are created `Free`, so an address space that has just been laid
/// out denies every access until a loader maps something into it -- which is
/// what makes `Free` worth modelling at all. The heap region is the upper half
/// and is reserved for `hos_memory_layout_set_heap_size`; `hos_memory_layout_map`
/// refuses to carve anything out of it.
///
/// `guest_base` and `guest_end` must be page-aligned, at least one page apart,
/// and inside the window `guest_memory` describes. The memory window must
/// already be initialised: this function records `guest_memory` and points it at
/// the table this struct owns, so re-initialising the window afterwards would
/// silently turn the gate off.
///
/// Returns:
/// - `POUND_SUCCESS`.
/// - `POUND_ERROR_INVALID_ARGUMENT` -- a NULL argument, an empty or too-small
///   range, or a window that is not described by `guest_memory`.
/// - `POUND_ERROR_MEMORY_ALIGNMENT` -- a bound is not page-aligned.
/// - `POUND_ERROR_ALREADY_INITIALIZED` -- the layout was initialised before.
error_t hos_memory_layout_init(hos_memory_layout_t *POUND_RESTRICT layout,
                               guest_memory_t *POUND_RESTRICT      guest_memory,
                               uint64_t                            guest_base,
                               uint64_t                            guest_end);

/// Carves `[base, base + size)` out of a `Free` region and gives it `state`,
/// `permission` and `attribute`.
///
/// This is the loader-facing primitive: the loader maps the title's code
/// (`HOS_MEMORY_STATE_CODE`, read|execute), its data (`HOS_MEMORY_STATE_NORMAL`,
/// read|write), stacks, and so on. The target must lie entirely inside one
/// `Free` region of the *map* area; the region is split so the mapped range is
/// one entry and the pieces on either side stay `Free`.
///
/// Returns:
/// - `POUND_SUCCESS`.
/// - `POUND_ERROR_INVALID_ARGUMENT` -- a NULL layout, a zero size, a target that
///   is not `Free`, a target that spans a region boundary, or a target that
///   overlaps the heap region.
/// - `POUND_ERROR_MEMORY_ALIGNMENT` -- a bound is not page-aligned.
/// - `POUND_ERROR_GUEST_ADDRESS_OVERFLOW` -- `base + size` wraps.
/// - `POUND_ERROR_GUEST_ADDRESS_OUT_OF_BOUNDS` -- a target outside the address
///   space.
/// - `POUND_ERROR_NOT_INITIALIZED` -- the layout was never initialised.
/// - `POUND_ERROR_CORRUPTED` -- the table is not tiling the address space.
/// - `POUND_ERROR_ALLOCATION_FAILED` -- the region table is full.
error_t hos_memory_layout_map(hos_memory_layout_t *POUND_RESTRICT layout,
                              uint64_t                            base,
                              uint64_t                            size,
                              uint32_t                            state,
                              uint32_t                            permission,
                              uint32_t                            attribute);

/// Sets the committed heap to `size` bytes, growing or shrinking as needed.
///
/// Commits `[heap_base, heap_base + size)` as `Normal` read|write and returns
/// everything past it to `Free`. `size` is validated at the granularity the
/// system call requires only for alignment; the caller is responsible for the
/// 2MB granularity and the 4GB ceiling, which are `svcSetHeapSize`'s documented
/// rules rather than a property of the layout.
///
/// Returns:
/// - `POUND_SUCCESS`.
/// - `POUND_ERROR_INVALID_ARGUMENT` -- a NULL layout, a non-page-aligned size,
///   or a size larger than the heap region.
/// - `POUND_ERROR_NOT_INITIALIZED` -- the layout was never initialised.
/// - `POUND_ERROR_ALLOCATION_FAILED` -- the region table is full. On hardware
///   this is `0xCE01`; `svcSetHeapSize` translates it.
error_t hos_memory_layout_set_heap_size(hos_memory_layout_t *POUND_RESTRICT layout, uint64_t size);

/// The region containing `address`, or NULL when `address` is outside the
/// address space.
///
/// The table tiles `[address_space_start, address_space_end)` with no gaps, so
/// an address inside the address space always finds exactly one region, and one
/// outside finds none. `svcQueryMemory` is the only caller: for a non-NULL
/// answer it reports that region, and for NULL it reports Horizon's fabricated
/// `Inaccessible` block covering `[address_space_end, 2^64)` -- which is what
/// the kernel does for any address not contained in the address space, above
/// the end or below the start alike.
const guest_memory_region_t *hos_memory_layout_find(const hos_memory_layout_t *POUND_RESTRICT layout,
                                                    uint64_t                               address);

#endif // POUND_HOS_MEMORY_LAYOUT_H

/*** end of file ***/

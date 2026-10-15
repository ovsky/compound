#ifndef POUND_GUEST_MEMORY_H
#define POUND_GUEST_MEMORY_H

#include "attributes.h"
#include "errors.h"
#include "stddef.h"
#include "stdint.h"
#include <assert.h>
#include <stdbool.h>

/// One contiguous piece of the guest address space, with the access rights the
/// guest is allowed to exercise inside it.
///
/// This is the single authoritative table of what the guest may touch: the
/// translate functions below gate every access on `permission`, and the
/// Horizon layer reads `state`/`attribute`/the counts to answer
/// `svcQueryMemory`. Keeping both views on one struct is what stops the two
/// from disagreeing about where a region ends.
///
/// The numeric values of `permission` are the same bit layout Horizon's
/// `MemoryPermission` uses (bit 0 read, bit 1 write, bit 2 execute), so a
/// region's rights can be written down from the same constants a loader or the
/// kernel layer uses. `state` and `attribute` are Horizon `MemoryState` and
/// `MemoryAttribute` encodings; this module never interprets them, it only
/// carries them.
///
/// Regions are page-aligned and non-overlapping by construction (the memory
/// layout module maintains both invariants); the translate functions do not
/// re-check them, because a malformed table is a bug in whoever installed it
/// rather than something a guest can provoke.
typedef struct
{
    /// First guest address of the region, page-aligned.
    uint64_t base;

    /// Size in bytes; `base + size` does not overflow and is page-aligned.
    uint64_t size;

    /// `GUEST_MEMORY_PERM_*` bits. Zero means no access at all, which is what
    /// makes a `Free` region fault on every touch exactly like hardware.
    uint32_t permission;

    /// Horizon `MemoryState` encoding (see `hos/memory_layout.h` for the values).
    uint32_t state;

    /// Horizon `MemoryAttribute` encoding.
    uint32_t attribute;

    /// `svcQueryMemory`'s ipc count field.
    uint32_t ipc_count;

    /// `svcQueryMemory`'s device count field.
    uint32_t device_count;
} guest_memory_region_t;

/// Read access, matching Horizon `MemoryPermission` bit 0.
#define GUEST_MEMORY_PERM_READ 0x1U

/// Write access, matching Horizon `MemoryPermission` bit 1.
#define GUEST_MEMORY_PERM_WRITE 0x2U

/// Execute access, matching Horizon `MemoryPermission` bit 2.
#define GUEST_MEMORY_PERM_EXEC 0x4U

/// Guest addresses in the range:
///
///     [guest_base, guest_base + size)
///
/// map directly to:
///     host_base + (guest_address - guest_base)
POUND_ALIGNED(64) typedef struct
{
    uint8_t *host_base;
    size_t   host_size;

    /// First valid guest address.
    size_t guest_base;

    /// One past the last valid guest address.
    size_t guest_end;

    /// The region table gating access, or NULL for a flat window with no
    /// regions installed.
    ///
    /// NULL is not "no regions exist" -- it is "nothing has partitioned this
    /// window yet", and it means every address in the window is readable,
    /// writable and executable. That keeps a harness that only wants a flat
    /// buffer (most of the test suite, and any stage before the loader runs)
    /// working unchanged; installing a table is what turns gating on. The
    /// table is owned by whoever installed it -- `hos_memory_layout_t` -- and
    /// must outlive this struct's use of it.
    const guest_memory_region_t *regions;

    /// Entries in `regions`. Kept in step by the owner of the table after
    /// every mutation (`hos_memory_layout_t` resyncs this), because a stale
    /// count would either leave a tail of the table unread or read past its
    /// end.
    uint32_t region_count;

    char pad[20];
} guest_memory_t;

static_assert(64 == sizeof(guest_memory_t), "Struct size mismatch");

/// `host_base` must be at least 16-byte aligned.
error_t guest_memory_init(guest_memory_t *POUND_RESTRICT guest_memory,
                          void *POUND_RESTRICT           host_base,
                          size_t                         host_size,
                          uint64_t                       guest_base);

/// Returns true if `guest_address` is inside this guest memory region.
bool guest_memory_contains(const guest_memory_t *POUND_RESTRICT guest_memory,
                           uint64_t                             guest_address);

/// Translates a guest address to a host read pointer.
///
/// On success it writes the number of contiguous readable bytes starting at the returned pointer
/// into `max_readable_bytes`.
///
/// When a region table is installed the address must fall inside a region with
/// `GUEST_MEMORY_PERM_READ`, and the byte count is clamped to that region's end:
/// the window being mapped says where bytes physically are, the region says
/// whether this access is allowed to see them.
const uint8_t *guest_memory_translate_read(const guest_memory_t *POUND_RESTRICT guest_memory,
                                           uint64_t                             guest_address,
                                           size_t *POUND_RESTRICT               max_readable_bytes);

/// Translates a guest address to a host write pointer.
///
/// On success it writes the number of contiguous writable bytes starting at the returned pointer
/// into `max_writable_bytes`.
///
/// Gated on `GUEST_MEMORY_PERM_WRITE` exactly like `guest_memory_translate_read`
/// is gated on `GUEST_MEMORY_PERM_READ`, with the same clamping to the region's
/// end.
uint8_t *guest_memory_translate_write(const guest_memory_t *POUND_RESTRICT guest_memory,
                                      uint64_t                             guest_address,
                                      size_t *POUND_RESTRICT               max_writable_bytes);

/// Translates a guest address to a host pointer for instruction fetch.
///
/// On success it writes the number of contiguous executable bytes starting at the returned pointer
/// into `max_executable_bytes`.
///
/// This exists because fetching is neither reading nor writing: a region that
/// is `R|X` must yield its bytes to the fetch loop and refuse them to a store,
/// and one `R|W` must do the opposite. With no table installed it behaves like
/// `guest_memory_translate_read`, so a flat window stays executable everywhere.
const uint8_t *guest_memory_translate_execute(const guest_memory_t *POUND_RESTRICT guest_memory,
                                              uint64_t                             guest_address,
                                              size_t *POUND_RESTRICT               max_executable_bytes);

#endif // POUND_GUEST_MEMORY_H

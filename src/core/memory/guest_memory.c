#include "guest_memory.h"
#include "log.h"

error_t
guest_memory_init(guest_memory_t *guest_memory,
                  void           *host_base,
                  const size_t    host_size,
                  const uint64_t  guest_base)
{
    if (guest_memory == NULL || host_base == NULL || host_size == 0)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting guest memory region: guest_memory=%p, host_base=%p, host_size=%zu.",
                        (const void *)guest_memory,
                        host_base,
                        host_size);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const uintptr_t host_address = (uintptr_t)host_base;

    if ((host_address & (uintptr_t)15) != (uintptr_t)0)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting guest memory region: host_base=%p is not 16-byte aligned.",
                        host_base);
        return POUND_ERROR_MEMORY_ALIGNMENT;
    }

    // `guest_end` is computed as `guest_base + host_size`, so a base one past
    // this limit would wrap the window to zero and make every subsequent
    // bounds check reject a region that is in fact valid.
    if (guest_base > UINT64_MAX - host_size)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting guest memory region: guest_base=0x%llx + host_size=%zu overflows a "
                        "64-bit guest address space.",
                        (unsigned long long)guest_base,
                        host_size);
        return POUND_ERROR_GUEST_ADDRESS_OVERFLOW;
    }

    guest_memory->host_base    = host_base;
    guest_memory->host_size    = host_size;
    guest_memory->guest_base   = guest_base;
    guest_memory->guest_end    = guest_base + host_size;

    // No table until someone installs one: a freshly initialised window is
    // flat, so every existing caller of the translate functions keeps seeing
    // the ungated behaviour until the memory layout module binds regions.
    guest_memory->regions      = NULL;
    guest_memory->region_count = 0U;
    return POUND_SUCCESS;
}

bool
guest_memory_contains(const guest_memory_t *guest_memory, const uint64_t guest_address)
{
    if (guest_memory == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting guest address containment check: region is NULL.");
        return false;
    }

    const uint64_t guest_base = guest_memory->guest_base;
    const uint64_t guest_end  = guest_memory->guest_end;
    const bool     contains   = guest_address >= guest_base && guest_address < guest_end;
    return contains;
}

/// Shared body of the three translate functions.
///
/// Validates the arguments, applies the window containment check every
/// translation must pass, then applies the region gate if a table is installed:
/// the address must sit in a region whose `permission` carries `required_perm`,
/// and the byte count is clamped to that region's end so no caller ever sees
/// bytes that spill across a region boundary. With no table installed the
/// region step is skipped entirely and the window itself is the authority,
/// which is what keeps a flat buffer readable, writable and executable
/// everywhere.
///
/// `what` names the access for the log lines: "read", "write" or "fetch".
static const uint8_t *
translate(const guest_memory_t *guest_memory,
          const uint64_t        guest_address,
          const uint32_t        required_perm,
          const char *const     what,
          size_t               *max_bytes)
{
    if (NULL == guest_memory || NULL == max_bytes)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting %s translation: guest_memory=%p, out=%p.",
                        what,
                        (const void *)guest_memory,
                        (void *)max_bytes);
        return NULL;
    }

    const bool is_guest_address_valid = guest_memory_contains(guest_memory, guest_address);

    if (false == is_guest_address_valid)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Guest %s fault: guest address 0x%llx is outside [0x%llx, 0x%llx).",
                        what,
                        (unsigned long long)guest_address,
                        (unsigned long long)guest_memory->guest_base,
                        (unsigned long long)guest_memory->guest_end);
        return NULL;
    }

    const uint64_t offset    = guest_address - guest_memory->guest_base;
    const uint64_t host_size = guest_memory->host_size;

    // Unreachable for any region produced by `guest_memory_init`, which
    // guarantees `guest_end == guest_base + host_size` -- so the containment
    // check above already established `offset < host_size`. It guards a region
    // assembled by hand, where `guest_end` and `host_size` disagree. Refusing
    // is still reported rather than returned as a bare NULL, because a silent
    // NULL here is indistinguishable from a genuine guest fault at the call
    // site.
    if (offset >= guest_memory->host_size)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting %s translation: offset 0x%llx is not below host_size=%llu; "
                        "the region is inconsistent.",
                        what,
                        (unsigned long long)offset,
                        (unsigned long long)guest_memory->host_size);
        return NULL;
    }

    // Bytes from the address to the end of the window. A region table may cut
    // this shorter; without one, the window is the whole truth.
    // Cast explicitly: `host_size` is a size_t widened to uint64_t above, so on
    // a 32-bit target the unwidened expression would be a narrowing conversion.
    uint64_t available = host_size - offset;

    if ((NULL != guest_memory->regions) && (0U != guest_memory->region_count))
    {
        const guest_memory_region_t *region = NULL;

        for (uint32_t index = 0U; index < guest_memory->region_count; index++)
        {
            const guest_memory_region_t *const candidate = &guest_memory->regions[index];

            if ((guest_address >= candidate->base) && (guest_address - candidate->base < candidate->size))
            {
                region = candidate;
                break;
            }
        }

        if (NULL == region)
        {
            POUND_LOG_ERROR(&thread_logger,
                            "Guest %s fault: guest address 0x%llx falls inside the window "
                            "[0x%llx, 0x%llx) but in no region.",
                            what,
                            (unsigned long long)guest_address,
                            (unsigned long long)guest_memory->guest_base,
                            (unsigned long long)guest_memory->guest_end);
            return NULL;
        }

        if (0U == (region->permission & required_perm))
        {
            // The same shape of fault as an address outside the window: a
            // region with no write permission is exactly as untouchable as
            // address space that does not exist, and reporting it any
            // differently would let a guest tell the two apart in a way the
            // hardware does not.
            POUND_LOG_ERROR(&thread_logger,
                            "Guest %s fault: guest address 0x%llx is in a region without that "
                            "permission (region [0x%llx, 0x%llx) permits 0x%X).",
                            what,
                            (unsigned long long)guest_address,
                            (unsigned long long)region->base,
                            (unsigned long long)(region->base + region->size),
                            (unsigned int)region->permission);
            return NULL;
        }

        const uint64_t region_remaining = region->size - (guest_address - region->base);

        if (region_remaining < available)
        {
            available = region_remaining;
        }
    }

    *max_bytes                       = (size_t)available;
    const uint8_t *POUND_RESTRICT host_base   = guest_memory->host_base;
    const uint8_t *POUND_RESTRICT host_offset = host_base + offset;
    return host_offset;
}

const uint8_t *
guest_memory_translate_read(const guest_memory_t *guest_memory,
                            const uint64_t        guest_address,
                            size_t               *max_readable_bytes)
{
    return translate(guest_memory, guest_address, GUEST_MEMORY_PERM_READ, "read", max_readable_bytes);
}

uint8_t *
guest_memory_translate_write(const guest_memory_t *POUND_RESTRICT guest_memory,
                             const uint64_t                       guest_address,
                             size_t *POUND_RESTRICT               max_writable_bytes)
{
    // The const cast is the one wart here: the host buffer behind the window
    // is mutable, and the constness of this function's first argument is a
    // statement about the *descriptor*, not about guest memory. The gate runs
    // before any pointer is handed out, so nothing writable escapes without
    // the permission check having seen it.
    const uint8_t *const readable =
        translate(guest_memory, guest_address, GUEST_MEMORY_PERM_WRITE, "write", max_writable_bytes);
    return (uint8_t *)(uintptr_t)readable;
}

const uint8_t *
guest_memory_translate_execute(const guest_memory_t *guest_memory,
                               const uint64_t        guest_address,
                               size_t               *max_executable_bytes)
{
    return translate(guest_memory, guest_address, GUEST_MEMORY_PERM_EXEC, "fetch", max_executable_bytes);
}

/*** end of file ***/

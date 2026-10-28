#include "syscall.h"
#include "log.h"
#include <string.h>

/// Handles one system call. `X0..X7` are read from and written to `state`, and the
/// context says what happened by setting `context->outcome`.
typedef error_t (*hos_syscall_fn_t)(hos_syscall_context_t *POUND_RESTRICT context,
                                    guest_state_t *POUND_RESTRICT          state);

/// One row of the system call table.
typedef struct
{
    /// The `SVC` immediate.
    uint32_t number;

    /// The name Horizon gives the call, used in every log line about it.
    const char *name;

    /// What runs it, or NULL for a number this module knows but does not implement.
    ///
    /// The NULL rows are not padding: their only job is to let a refusal say
    /// `svcSendSyncRequest (0x21)` instead of `svc 0x21`, which is the difference
    /// between a log line that names the missing feature and one that names a number.
    /// The numbers and spellings are the ones the homebrew wiki lists.
    hos_syscall_fn_t handler;
} hos_syscall_entry_t;

// -----------------------------------------------------------------------------
// Forward declarations, so the table below can sit above the handlers
// -----------------------------------------------------------------------------

static error_t svc_exit_process(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_set_heap_size(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_set_memory_permission(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_set_memory_attribute(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_query_memory(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_get_info(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_create_thread(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_start_thread(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_exit_thread(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_sleep_thread(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_get_thread_priority(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_set_thread_priority(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_get_current_processor_number(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_signal_event(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_clear_event(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_create_event(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_close_handle(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_reset_signal(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_wait_synchronization(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_cancel_synchronization(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_get_system_tick(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_connect_to_named_port(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_get_thread_id(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_break(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_output_debug_string(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_flush_entire_data_cache(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_flush_data_cache(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_sleep_system(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_map_memory(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_unmap_memory(hos_syscall_context_t *context, guest_state_t *state);

// -----------------------------------------------------------------------------
// The table
// -----------------------------------------------------------------------------

static const hos_syscall_entry_t HOS_SYSCALLS[] = {
    // Implemented.
    { HOS_SVC_SET_HEAP_SIZE, "svcSetHeapSize", svc_set_heap_size },
    { HOS_SVC_SET_MEMORY_PERMISSION, "svcSetMemoryPermission", svc_set_memory_permission },
    { HOS_SVC_SET_MEMORY_ATTRIBUTE, "svcSetMemoryAttribute", svc_set_memory_attribute },
    { HOS_SVC_QUERY_MEMORY, "svcQueryMemory", svc_query_memory },
    { HOS_SVC_GET_INFO, "svcGetInfo", svc_get_info },
    { HOS_SVC_EXIT_PROCESS, "svcExitProcess", svc_exit_process },
    { HOS_SVC_CREATE_THREAD, "svcCreateThread", svc_create_thread },
    { HOS_SVC_START_THREAD, "svcStartThread", svc_start_thread },
    { HOS_SVC_EXIT_THREAD, "svcExitThread", svc_exit_thread },
    { HOS_SVC_SLEEP_THREAD, "svcSleepThread", svc_sleep_thread },
    { HOS_SVC_GET_THREAD_PRIORITY, "svcGetThreadPriority", svc_get_thread_priority },
    { HOS_SVC_SET_THREAD_PRIORITY, "svcSetThreadPriority", svc_set_thread_priority },
    { HOS_SVC_GET_CURRENT_PROCESSOR_NUMBER, "svcGetCurrentProcessorNumber", svc_get_current_processor_number },
    { HOS_SVC_SIGNAL_EVENT, "svcSignalEvent", svc_signal_event },
    { HOS_SVC_CLEAR_EVENT, "svcClearEvent", svc_clear_event },
    { HOS_SVC_CLOSE_HANDLE, "svcCloseHandle", svc_close_handle },
    { HOS_SVC_RESET_SIGNAL, "svcResetSignal", svc_reset_signal },
    { HOS_SVC_WAIT_SYNCHRONIZATION, "svcWaitSynchronization", svc_wait_synchronization },
    { HOS_SVC_CANCEL_SYNCHRONIZATION, "svcCancelSynchronization", svc_cancel_synchronization },
    { HOS_SVC_GET_SYSTEM_TICK, "svcGetSystemTick", svc_get_system_tick },
    { HOS_SVC_CONNECT_TO_NAMED_PORT, "svcConnectToNamedPort", svc_connect_to_named_port },
    { HOS_SVC_GET_THREAD_ID, "svcGetThreadId", svc_get_thread_id },
    { HOS_SVC_BREAK, "svcBreak", svc_break },
    { HOS_SVC_OUTPUT_DEBUG_STRING, "svcOutputDebugString", svc_output_debug_string },
    { HOS_SVC_FLUSH_ENTIRE_DATA_CACHE, "svcFlushEntireDataCache", svc_flush_entire_data_cache },
    { HOS_SVC_FLUSH_DATA_CACHE, "svcFlushDataCache", svc_flush_data_cache },
    { HOS_SVC_CREATE_EVENT, "svcCreateEvent", svc_create_event },
    { HOS_SVC_SLEEP_SYSTEM, "svcSleepSystem", svc_sleep_system },
    { HOS_SVC_MAP_MEMORY, "svcMapMemory", svc_map_memory },
    { HOS_SVC_UNMAP_MEMORY, "svcUnmapMemory", svc_unmap_memory },

    // Known, named, and deliberately not implemented. Each of these is waiting on a
    // slice that does not exist yet, and each says which one in the header.
    { 0x13U, "svcMapSharedMemory", NULL },
    { 0x14U, "svcUnmapSharedMemory", NULL },
    { 0x13U, "svcMapSharedMemory", NULL },
    { 0x14U, "svcUnmapSharedMemory", NULL },
    { 0x15U, "svcCreateTransferMemory", NULL },
    { 0x20U, "svcSendSyncRequestLight", NULL },
    { 0x21U, "svcSendSyncRequest", NULL },
    { 0x22U, "svcSendSyncRequestWithUserBuffer", NULL },
    { 0x24U, "svcGetProcessId", NULL },
    { 0x41U, "svcAcceptSession", NULL },
    { 0x42U, "svcReplyAndReceiveLight", NULL },
    { 0x43U, "svcReplyAndReceive", NULL },
    { 0x44U, "svcReplyAndReceiveWithUserBuffer", NULL },
    { 0x50U, "svcCreateSharedMemory", NULL },
    { 0x70U, "svcCreatePort", NULL },
    { 0x71U, "svcManageNamedPort", NULL },
    { 0x72U, "svcConnectToPort", NULL },
    { 0x7FU, "svcCallSecureMonitor", NULL },
};

#define HOS_SYSCALL_COUNT (sizeof(HOS_SYSCALLS) / sizeof(HOS_SYSCALLS[0]))

/// Returns the table row for `number`, or NULL for a number the table does not carry.
static const hos_syscall_entry_t *
find_syscall(uint32_t number)
{
    for (size_t index = 0U; index < HOS_SYSCALL_COUNT; index++)
    {
        if (HOS_SYSCALLS[index].number == number)
        {
            return &HOS_SYSCALLS[index];
        }
    }

    return NULL;
}

const char *
hos_syscall_name(uint32_t number)
{
    const hos_syscall_entry_t *entry = find_syscall(number);
    return (entry == NULL) ? NULL : entry->name;
}

// -----------------------------------------------------------------------------
// Guest memory
// -----------------------------------------------------------------------------

/// Translates `size` contiguous readable guest bytes at `address`.
///
/// Returns false when the span is not fully mapped, which every caller turns into the
/// kernel's own `HOS_RESULT_INVALID_POINTER` rather than into an emulator error: a guest
/// pointer that is not mapped is something Horizon itself reports, and a guest is
/// allowed to make that mistake without stopping the machine.
static bool
read_guest(const hos_syscall_context_t *POUND_RESTRICT context,
           uint64_t                                    address,
           size_t                                      size,
           const uint8_t **POUND_RESTRICT              out)
{
    size_t available = 0U;

    if (size == 0U)
    {
        *out = NULL;
        return true;
    }

    const uint8_t *pointer = guest_memory_translate_read(context->memory, address, &available);

    if ((pointer == NULL) || (available < size))
    {
        return false;
    }

    *out = pointer;
    return true;
}

/// Reads the little-endian 32-bit word at `bytes`.
///
/// Spelled out rather than `memcpy`-ed through an integer so the byte order is part of
/// the code rather than a property of the host it happens to be compiled for.
static uint32_t
read_u32(const uint8_t *POUND_RESTRICT bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

/// Stores the little-endian 32-bit word `value` at `bytes`, the counterpart to
/// `read_u32`. Used to assemble a structure off to the side before it is handed to the
/// guest in one piece.
static void
store_u32(uint8_t *POUND_RESTRICT bytes, const uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

/// Stores the little-endian 64-bit word `value` at `bytes`, the counterpart to a 64-bit
/// `read_u32` sequence.
static void
store_u64(uint8_t *POUND_RESTRICT bytes, const uint64_t value)
{
    for (size_t index = 0U; index < sizeof(uint64_t); index++)
    {
        bytes[index] = (uint8_t)(value >> (8U * index));
    }
}

/// Returns the handle an argument register carries.
///
/// Handles are 32 bits and arrive in the low half of a 64-bit register, so the upper
/// half is discarded rather than rejected: the guest is entitled to leave anything
/// there, because AArch64 writes of `W0` zero it anyway.
static hos_handle_t
handle_argument(uint64_t value)
{
    return (hos_handle_t)(uint32_t)value;
}

/// Writes a little-endian 32-bit word to guest memory at `address`.
///
/// The counterpart to `read_guest`, and the reason the out-parameter calls write through
/// the pointer in `X0` before they replace `X0` with their `Result`: once the `Result` is
/// in the register, the address is gone. Returns false when the four bytes are not
/// writable, which every caller turns into `HOS_RESULT_INVALID_POINTER`.
static bool
write_guest_u32(const hos_syscall_context_t *POUND_RESTRICT context, uint64_t address, uint32_t value)
{
    size_t available = 0U;

    uint8_t *pointer = guest_memory_translate_write(context->memory, address, &available);

    if ((pointer == NULL) || (available < sizeof(uint32_t)))
    {
        return false;
    }

    pointer[0] = (uint8_t)value;
    pointer[1] = (uint8_t)(value >> 8);
    pointer[2] = (uint8_t)(value >> 16);
    pointer[3] = (uint8_t)(value >> 24);
    return true;
}

/// Writes a little-endian 64-bit word to guest memory at `address`.
///
/// A separate function rather than a call to `write_guest_u32` twice, because a guest
/// address is not guaranteed to have eight contiguous writable bytes just because it has
/// four: the span is checked for the width actually being written.
static bool
write_guest_u64(const hos_syscall_context_t *POUND_RESTRICT context, uint64_t address, uint64_t value)
{
    size_t available = 0U;

    uint8_t *pointer = guest_memory_translate_write(context->memory, address, &available);

    if ((pointer == NULL) || (available < sizeof(uint64_t)))
    {
        return false;
    }

    for (size_t index = 0U; index < sizeof(uint64_t); index++)
    {
        pointer[index] = (uint8_t)(value >> (8U * index));
    }

    return true;
}

/// Writes `size` plain bytes to guest memory at `address`.
///
/// The counterpart to `read_guest`, for a whole structure rather than a word. It exists
/// so a call that has already assembled an answer can hand it over in one piece: a
/// pointer that is not fully writable must leave the guest's memory untouched, and a
/// field-by-field write would have written half the struct before noticing. Returns
/// false when the whole span is not writable.
static bool
write_guest_bytes(const hos_syscall_context_t *POUND_RESTRICT context,
                  uint64_t                                    address,
                  const uint8_t *POUND_RESTRICT               bytes,
                  size_t                                      size)
{
    size_t available = 0U;

    if (size == 0U)
    {
        return true;
    }

    uint8_t *pointer = guest_memory_translate_write(context->memory, address, &available);

    if ((pointer == NULL) || (available < size))
    {
        return false;
    }

    memcpy(pointer, bytes, size);
    return true;
}

// -----------------------------------------------------------------------------
// Memory calls
// -----------------------------------------------------------------------------
//
// These describe the address space the process runs in. They need the layout: without
// one, there are no regions and no heap region, and an answer assembled from nothing
// would be an invented one. A context with no layout refuses them exactly as it refuses
// a thread call with no scheduler.

/// The size of a Horizon `MemoryInfo`, which `svcQueryMemory` writes through the pointer
/// in `X0`. Eight bytes of base, eight of size, then five 32-bit fields and padding:
/// `base`, `size`, `state`, `attribute`, `permission`, `ipc` count, `device` count, pad.
#define HOS_MEMORY_INFO_SIZE 0x28U

/// Reports whether a memory call must be refused because the context has no layout.
///
/// True means the caller should return `POUND_ERROR_UNSUPPORTED_SYSCALL`, and the log line
/// naming the call has already been written.
static bool
memory_call_needs_layout(const hos_syscall_context_t *POUND_RESTRICT context, const char *POUND_RESTRICT call)
{
    if (context->layout != NULL)
    {
        return false;
    }

    POUND_LOG_ERROR(&thread_logger,
                    "Refusing %s: the context has no memory layout, so there are no regions to describe "
                    "and no heap region to resize.",
                    call);
    return true;
}

/// Translates a failure from the region table into the `Result` the two
/// reprotection calls report, and says whether it could.
///
/// Both calls answer the same word -- Horizon's `ResultInvalidCurrentMemory`,
/// `0xD401` -- for "the range is not in the address space" and for "a region in
/// it may not be changed", so the layout's two errors collapse to one here. An
/// error with no documented `Result` is left for the caller to propagate rather
/// than invented into one.
static bool
memory_layout_error_to_result(error_t error, uint32_t *POUND_RESTRICT out_result)
{
    switch (error)
    {
        case POUND_SUCCESS:
            *out_result = HOS_RESULT_SUCCESS;
            return true;

        case POUND_ERROR_GUEST_ADDRESS_OUT_OF_BOUNDS:
        case POUND_ERROR_INVALID_ARGUMENT:
            *out_result = HOS_RESULT_INVALID_CURRENT_MEMORY;
            return true;

        case POUND_ERROR_ALLOCATION_FAILED:
            *out_result = HOS_RESULT_SLAB_EXHAUSTED;
            return true;

        default:
            return false;
    }
}

/// The permissions `svcSetMemoryPermission` accepts: no access, read-only and
/// read-write.
///
/// The kernel refuses write-only, and refuses execute here because that is
/// `svcSetProcessMemoryPermission`'s job, so exactly three words are valid.
static bool
memory_permission_is_settable(const uint32_t permission)
{
    return (permission == 0U) || (permission == (uint32_t)GUEST_MEMORY_PERM_READ) ||
           (permission == (uint32_t)(GUEST_MEMORY_PERM_READ | GUEST_MEMORY_PERM_WRITE));
}

static error_t
svc_set_heap_size(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    if (memory_call_needs_layout(context, "svcSetHeapSize"))
    {
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    // Horizon's signature: X1 is the requested size, and there is no X0 argument. The
    // answer is the register X1, which the caller's stub stores through the out-pointer it
    // saved before the trap -- libnx's is
    // `str x0, [sp, #-16]!; svc 0x1; ldr x2, [sp], #16; str x1, [x2]`. A kernel that
    // answered only through X0's pointer would leave a real guest storing whatever X1
    // happened to hold.
    const uint64_t size = state->x[1];

    // Horizon's two documented refusals for the request itself: `0xCA01` is "not a multiple
    // of 2MB" or "larger than 4GB", and `0xD001` is "larger than the heap region". Where
    // both could apply, the request is checked before the region, matching the wrapper.
    if (((size % HOS_MEMORY_HEAP_GRANULARITY) != 0U) || (size > HOS_MEMORY_HEAP_SIZE_MAX))
    {
        state->x[0] = HOS_RESULT_INVALID_SIZE;
        return POUND_SUCCESS;
    }

    if (size > context->layout->heap_region_size)
    {
        state->x[0] = HOS_RESULT_OUT_OF_MEMORY;
        return POUND_SUCCESS;
    }

    const error_t error = hos_memory_layout_set_heap_size(context->layout, size);

    if (error == POUND_ERROR_ALLOCATION_FAILED)
    {
        // Compound's region table is its memory-block pool, and running out of descriptors is
        // exactly what Horizon reports as `0xCE01` for this call. Refusing with an emulator
        // error here would wedge a guest that mapped enough of its address space to fill the
        // table, which on hardware it would have survived.
        state->x[0] = HOS_RESULT_SLAB_EXHAUSTED;
        return POUND_SUCCESS;
    }

    if (error != POUND_SUCCESS)
    {
        // Unreachable from here: the size was checked against the heap region above, which
        // is the only refusal the layout has left. Reported rather than hidden, because a
        // silently ignored failure would leave `heap_size` a lie.
        return error;
    }

    // The heap base is fixed for the life of the process, so every grow and shrink answers
    // the same address. It goes in `X1`, not `X0`, for the same reason as above.
    state->x[1] = context->layout->heap_base;
    state->x[0] = HOS_RESULT_SUCCESS;
    return POUND_SUCCESS;
}

static error_t
svc_set_memory_permission(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    if (memory_call_needs_layout(context, "svcSetMemoryPermission"))
    {
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    // Horizon's signature: X0 is the address, X1 the size, W2 the permission. There is no
    // out-pointer; the whole answer is `W0`.
    const uint64_t address    = state->x[0];
    const uint64_t size       = state->x[1];
    const uint32_t permission = (uint32_t)state->x[2];

    // The checks are Horizon's, in its order: the address first (`0xCC01`), then the size
    // (`0xCA01`, which covers zero), then the permission (`0xD801`). The overflow guard is
    // the kernel's own `addr < addr + size`; a range that wraps is not in the address
    // space, which is `0xD401`.
    if ((address % HOS_MEMORY_PAGE_SIZE) != 0U)
    {
        state->x[0] = HOS_RESULT_INVALID_ADDRESS;
        return POUND_SUCCESS;
    }

    if ((size == 0U) || ((size % HOS_MEMORY_PAGE_SIZE) != 0U))
    {
        state->x[0] = HOS_RESULT_INVALID_SIZE;
        return POUND_SUCCESS;
    }

    if (address > (UINT64_MAX - size))
    {
        state->x[0] = HOS_RESULT_INVALID_CURRENT_MEMORY;
        return POUND_SUCCESS;
    }

    if (!memory_permission_is_settable(permission))
    {
        state->x[0] = HOS_RESULT_INVALID_NEW_PERMISSION;
        return POUND_SUCCESS;
    }

    uint32_t      result = HOS_RESULT_SUCCESS;
    const error_t error  = hos_memory_layout_set_permission(context->layout, address, size, permission);

    if (!memory_layout_error_to_result(error, &result))
    {
        return error;
    }

    state->x[0] = result;
    return POUND_SUCCESS;
}

static error_t
svc_set_memory_attribute(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    if (memory_call_needs_layout(context, "svcSetMemoryAttribute"))
    {
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    // Horizon's signature: X0 the address, X1 the size, W2 the mask, W3 the value.
    const uint64_t address = state->x[0];
    const uint64_t size    = state->x[1];
    const uint32_t mask    = (uint32_t)state->x[2];
    const uint32_t value   = (uint32_t)state->x[3];

    if ((address % HOS_MEMORY_PAGE_SIZE) != 0U)
    {
        state->x[0] = HOS_RESULT_INVALID_ADDRESS;
        return POUND_SUCCESS;
    }

    if ((size == 0U) || ((size % HOS_MEMORY_PAGE_SIZE) != 0U))
    {
        state->x[0] = HOS_RESULT_INVALID_SIZE;
        return POUND_SUCCESS;
    }

    if (address > (UINT64_MAX - size))
    {
        state->x[0] = HOS_RESULT_INVALID_CURRENT_MEMORY;
        return POUND_SUCCESS;
    }

    // The value must be a subset of the mask -- a bit a caller gives a value for must also
    // be one it is asking to change -- and neither may reach outside the single attribute
    // Compound can change. Anything else is the kernel's `0xE801`.
    const uint32_t supported = HOS_MEMORY_ATTRIBUTE_UNCACHED;

    if (((mask | value) != mask) || ((mask | value | supported) != supported))
    {
        state->x[0] = HOS_RESULT_INVALID_COMBINATION;
        return POUND_SUCCESS;
    }

    uint32_t      result = HOS_RESULT_SUCCESS;
    const error_t error  = hos_memory_layout_set_attribute(context->layout, address, size, mask, value);

    if (!memory_layout_error_to_result(error, &result))
    {
        return error;
    }

    state->x[0] = result;
    return POUND_SUCCESS;
}

static error_t
svc_query_memory(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    if (memory_call_needs_layout(context, "svcQueryMemory"))
    {
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    // Horizon's signature: X0 is the `MemoryInfo` the kernel fills in, X1 is the caller's
    // `PageInfo` pointer (the kernel does not touch it; the stub stores W1 through it), and
    // X2 is the address being asked about. The register answer is `W1`.
    const uint64_t info_address = state->x[0];
    const uint64_t address      = state->x[2];

    // The lowest page-aligned mapping containing the address. The table tiles the address
    // space without gaps, so an address inside it lands in exactly one region and one
    // outside lands in none.
    const guest_memory_region_t *const region = hos_memory_layout_find(context->layout, address);

    // What a non-region address is reported as. This is not a placeholder: it is the block
    // `KPageTableBase::QueryInfo` fabricates for any address it does not contain -- above
    // the end *or* below the start -- with base at the end of the address space and a size
    // that reaches the top of memory, as `Inaccessible`, and with a success `Result`. A
    // guest that queries an address it never mapped sees exactly this, so answering
    // anything else would be the lie.
    uint64_t base         = context->layout->address_space_end;
    uint64_t size         = 0U - context->layout->address_space_end;
    uint32_t mem_state    = (uint32_t)HOS_MEMORY_STATE_INACCESSIBLE;
    uint32_t attribute    = 0U;
    uint32_t permission   = 0U;
    uint32_t ipc_count    = 0U;
    uint32_t device_count = 0U;

    if (region != NULL)
    {
        base         = region->base;
        size         = region->size;
        mem_state    = region->state;
        attribute    = region->attribute;
        permission   = region->permission;
        ipc_count    = region->ipc_count;
        device_count = region->device_count;
    }

    // Assembled off to the side and written in one piece, so a guest pointer that turns out
    // not to be writable leaves the guest's memory untouched rather than half-updated.
    uint8_t info[HOS_MEMORY_INFO_SIZE];

    memset(info, 0, sizeof(info));
    store_u64(&info[0x00], base);
    store_u64(&info[0x08], size);
    store_u32(&info[0x10], mem_state);
    store_u32(&info[0x14], attribute);
    store_u32(&info[0x18], permission);
    store_u32(&info[0x1C], ipc_count);
    store_u32(&info[0x20], device_count);
    // 0x24 is the struct's padding and stays zero.

    if (!write_guest_bytes(context, info_address, info, sizeof(info)))
    {
        // Horizon's own `Result` for an unwritable `MemoryInfo` pointer is not one Compound has
        // established, so the module's documented convention for an unwritable out-pointer
        // applies, and `W1` is left as the guest left it rather than given an invented value.
        state->x[0] = HOS_RESULT_INVALID_POINTER;
        return POUND_SUCCESS;
    }

    // `W1` is Horizon's `(Out) W1 PageInfo`, always zero: the kernel reports no page-level
    // detail here. The caller's stub stores it through the `PageInfo` pointer it passed in
    // X1, so it must be set even though Compound itself has nothing to say with it.
    state->x[0] = HOS_RESULT_SUCCESS;
    state->x[1] = 0U;
    return POUND_SUCCESS;
}

/// The `svcGetInfo` types this module answers. Each is a fact the layout already
/// holds; every other type needs state Pound does not model yet and is refused.
#define HOS_INFO_TYPE_HEAP_REGION_ADDRESS 4U
#define HOS_INFO_TYPE_HEAP_REGION_SIZE 5U
#define HOS_INFO_TYPE_ASLR_REGION_ADDRESS 12U
#define HOS_INFO_TYPE_ASLR_REGION_SIZE 13U

/// Horizon's pseudo-handle for the calling process. It names no slot in the handle
/// table: the kernel resolves it to whoever made the call.
#define HOS_PSEUDO_HANDLE_CURRENT_PROCESS 0xFFFF8001U

static error_t
svc_get_info(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    // Horizon's signature: X0 is the pointer the stub stores the answer through, X1 the
    // info type, W2 the handle, X3 the subtype. The answer is the 64-bit `(Out) X1`.
    const uint64_t     out_address = state->x[0];
    const uint64_t     info_type   = state->x[1];
    const hos_handle_t handle      = handle_argument(state->x[2]);
    const uint64_t     subtype     = state->x[3];

    if ((info_type != HOS_INFO_TYPE_HEAP_REGION_ADDRESS) && (info_type != HOS_INFO_TYPE_HEAP_REGION_SIZE) &&
        (info_type != HOS_INFO_TYPE_ASLR_REGION_ADDRESS) && (info_type != HOS_INFO_TYPE_ASLR_REGION_SIZE))
    {
        // Core and priority masks, memory totals, entropy, the alias and stack regions:
        // each is a real Horizon answer that needs a process or region Pound does not
        // have, so the trap is left standing rather than answered with a guess.
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing svcGetInfo type %llu: Pound has not implemented it.",
                        (unsigned long long)info_type);
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    if (memory_call_needs_layout(context, "svcGetInfo"))
    {
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    // Checked in the kernel's order: a non-zero subtype is refused before the handle is
    // looked at, so a guest that gets both wrong sees the same word it would on hardware.
    if (subtype != 0U)
    {
        state->x[0] = HOS_RESULT_INVALID_COMBINATION;
        return POUND_SUCCESS;
    }

    // These types ask about a process. Pound has no process object a handle could name,
    // so the only handle that can name one is the pseudo-handle for the caller; anything
    // else names nothing or names the wrong kind of object, which is `0xE401` either way.
    if (handle != HOS_PSEUDO_HANDLE_CURRENT_PROCESS)
    {
        state->x[0] = HOS_RESULT_INVALID_HANDLE;
        return POUND_SUCCESS;
    }

    const hos_memory_layout_t *const layout = context->layout;
    uint64_t                         value  = 0U;

    switch (info_type)
    {
        case HOS_INFO_TYPE_HEAP_REGION_ADDRESS:
            value = layout->heap_base;
            break;

        case HOS_INFO_TYPE_HEAP_REGION_SIZE:
            value = layout->heap_region_size;
            break;

        case HOS_INFO_TYPE_ASLR_REGION_ADDRESS:
            // The ASLR region is the span every other region is placed inside, which in
            // Pound's layout is the whole address space.
            value = layout->address_space_start;
            break;

        // The filter above leaves only the ASLR size for the default.
        case HOS_INFO_TYPE_ASLR_REGION_SIZE:
        default:
            value = layout->address_space_end - layout->address_space_start;
            break;
    }

    // As `svcGetThreadId`: the stub stores X1 through the pointer it saved, and the write
    // through X0 serves a guest that reads the slot directly.
    const bool delivered = write_guest_u64(context, out_address, value);

    if (delivered)
    {
        state->x[1] = value;
    }

    state->x[0] = delivered ? HOS_RESULT_SUCCESS : HOS_RESULT_INVALID_POINTER;
    return POUND_SUCCESS;
}

// -----------------------------------------------------------------------------
// Handle calls
// -----------------------------------------------------------------------------

/// Translates a failure from the handle layer into the `Result` Horizon reports.
///
/// The two failures a handle can produce -- naming nothing, and naming the wrong kind of
/// object -- are one `Result` on hardware, because Horizon's `0xE401` covers both with
/// the words "either does not exist, or is not a WritableEvent". Keeping them apart here
/// would be more precise than the machine being emulated, which is a different bug.
static bool
handle_failure_result(error_t error, uint32_t *POUND_RESTRICT out_result)
{
    if (error == POUND_SUCCESS)
    {
        return false;
    }

    if ((error == POUND_ERROR_INVALID_HANDLE) || (error == POUND_ERROR_INVALID_ARGUMENT))
    {
        *out_result = HOS_RESULT_INVALID_HANDLE;
        return true;
    }

    if (error == POUND_ERROR_INVALID_STATE)
    {
        *out_result = HOS_RESULT_INVALID_STATE;
        return true;
    }

    return false;
}

static error_t
svc_signal_event(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    uint32_t      result = HOS_RESULT_SUCCESS;
    const error_t error  = hos_process_event_signal(context->process, handle_argument(state->x[0]));

    if (handle_failure_result(error, &result))
    {
        state->x[0] = result;
        return POUND_SUCCESS;
    }

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    state->x[0] = HOS_RESULT_SUCCESS;
    return POUND_SUCCESS;
}

static error_t
svc_clear_event(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    uint32_t      result = HOS_RESULT_SUCCESS;
    const error_t error  = hos_process_event_clear(context->process, handle_argument(state->x[0]));

    if (handle_failure_result(error, &result))
    {
        state->x[0] = result;
        return POUND_SUCCESS;
    }

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    state->x[0] = HOS_RESULT_SUCCESS;
    return POUND_SUCCESS;
}

static error_t
svc_create_event(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    // Horizon's signature takes no arguments: the kernel answers with two handles in
    // registers, `W1` writable and `W2` readable. libnx's stub saves the caller's two
    // out-pointers before the trap and stores those registers through them afterwards, so
    // a handler that wrote through `X0` instead would leave a real guest with stale
    // handle values.
    hos_handle_t writable = HOS_HANDLE_INVALID;
    hos_handle_t readable = HOS_HANDLE_INVALID;

    const error_t error =
        hos_process_create_event_pair(context->process, false, &writable, &readable);

    if (error == POUND_ERROR_ALLOCATION_FAILED)
    {
        // Compound's object and handle tables are its event quota, and running out of slots
        // is exactly what Horizon reports as `0xCE01` for this call. Refusing with an
        // emulator error would wedge a guest that Horizon would have let run.
        state->x[0] = HOS_RESULT_SLAB_EXHAUSTED;
        return POUND_SUCCESS;
    }

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    state->x[0] = HOS_RESULT_SUCCESS;
    state->x[1] = writable;
    state->x[2] = readable;
    return POUND_SUCCESS;
}

static error_t
svc_close_handle(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    uint32_t      result = HOS_RESULT_SUCCESS;
    const error_t error  = hos_process_close_handle(context->process, handle_argument(state->x[0]));

    if (handle_failure_result(error, &result))
    {
        state->x[0] = result;
        return POUND_SUCCESS;
    }

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    state->x[0] = HOS_RESULT_SUCCESS;
    return POUND_SUCCESS;
}

static error_t
svc_reset_signal(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    uint32_t      result = HOS_RESULT_SUCCESS;
    const error_t error =
        hos_process_event_reset(context->process, handle_argument(state->x[0]));

    if (handle_failure_result(error, &result))
    {
        // `0xFA01` for an event that was already clear is the point of the call: it is
        // how a guest tells "I consumed the signal" from "something else got there
        // first", so it is a completed call with a `Result`, not an emulator error.
        state->x[0] = result;
        return POUND_SUCCESS;
    }

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    state->x[0] = HOS_RESULT_SUCCESS;
    return POUND_SUCCESS;
}

static error_t
svc_wait_synchronization(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    // A cancellation requested while this thread was not in a synchronization call is
    // waiting for this one. Horizon answers it before the wait looks at its handles -- the
    // flag means "the next synchronization call is interrupted" -- so a cancelled poll
    // reports no index even when a handle was already ready, and an auto-reset event is not
    // consumed by a call that never got as far as polling.
    if (context->scheduler != NULL)
    {
        const uint64_t current_id = hos_scheduler_current_thread_id(context->scheduler);
        bool           cancelled  = false;

        if ((hos_kernel_thread_take_cancel(context->kernel, current_id, &cancelled) == POUND_SUCCESS) &&
            cancelled)
        {
            state->x[0] = HOS_RESULT_INTERRUPTED;
            return POUND_SUCCESS;
        }
    }

    // Horizon's signature: X0 is a pointer to the ready index, X1 the handle array, W2 the
    // count and X3 the timeout. X0 is a pointer on entry and the `Result` on return, which
    // is why the index is written *through* it before it is replaced.
    const uint64_t out_index_address = state->x[0];
    const uint64_t handles_address   = state->x[1];
    const int32_t  count             = (int32_t)(uint32_t)state->x[2];
    const int64_t  timeout           = (int64_t)state->x[3];

    if (count == 0)
    {
        // Zero handles is Horizon's "wait forever", and this is a gap rather than a poll:
        // a wait on nothing can only be ended by the clock, and there is no honest way to
        // shorten it into a timeout the guest never asked for.
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing svcWaitSynchronization with no handles: a wait on nothing can only be "
                        "ended by a deadline, and Horizon documents this form as waiting forever.");
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    if ((count < 0) || ((uint32_t)count > HOS_SYSCALL_MAX_WAIT_HANDLES))
    {
        state->x[0] = HOS_RESULT_TOO_MANY_HANDLES;
        return POUND_SUCCESS;
    }

    const uint8_t *bytes = NULL;

    if (!read_guest(context, handles_address, (size_t)(uint32_t)count * sizeof(uint32_t), &bytes))
    {
        state->x[0] = HOS_RESULT_INVALID_POINTER;
        return POUND_SUCCESS;
    }

    hos_handle_t handles[HOS_SYSCALL_MAX_WAIT_HANDLES];

    for (uint32_t index = 0U; index < (uint32_t)count; index++)
    {
        handles[index] = (hos_handle_t)read_u32(bytes + ((size_t)index * sizeof(uint32_t)));
    }

    // The whole set is polled at once, in the kernel, because the answer is more than "is
    // anything ready": an auto-reset signal is consumed by the wait that wins it, and the
    // order that decides which one wins lives there. Polling one handle at a time, as an
    // earlier version did, could express neither.
    uint32_t ready_index = 0U;
    bool     ready       = false;

    error_t error = hos_process_wait_any(context->process, handles, (uint32_t)count, &ready_index, &ready);

    if (error == POUND_ERROR_INVALID_HANDLE)
    {
        // Horizon reports an invalid handle in the array as the call's failure rather than
        // skipping it: a wait over a handle that names nothing can never be satisfied, so
        // it is not something to look past.
        state->x[0] = HOS_RESULT_INVALID_HANDLE;
        return POUND_SUCCESS;
    }

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    if (ready)
    {
        // The common case, and the one that must never park a thread: a wait that can be
        // satisfied now is answered now, which is what a poll is.
        const bool delivered = write_guest_u32(context, out_index_address, ready_index);

        if (delivered)
        {
            // Horizon answers the index in `W1`, and the caller's stub stores that register
            // through the out-pointer (`str w1, [x2]`). Setting it is what makes a real
            // guest see the index; the write above only covers a guest that reads the slot
            // through `X0` directly.
            state->x[1] = ready_index;
        }

        state->x[0] = delivered ? HOS_RESULT_SUCCESS : HOS_RESULT_INVALID_POINTER;
        return POUND_SUCCESS;
    }

    if (timeout == 0)
    {
        state->x[0] = HOS_RESULT_TIMEOUT;
        return POUND_SUCCESS;
    }

    if (context->scheduler == NULL)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing svcWaitSynchronization for %lld ns: nothing in the set is ready, and "
                        "parking the caller until something is needs a scheduler. Answering a timeout "
                        "before it expires would be a lie the guest cannot detect.",
                        (long long)timeout);
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    // A negative timeout is Horizon's "wait forever": parked with no deadline, so only one
    // of the handles becoming ready can wake it. A positive one becomes a deadline on the
    // scheduler's clock. The deadline is taken now, not when the thread is parked, so the
    // wait is measured from the call rather than from whenever the scheduler next looked.
    const bool     has_deadline = timeout > 0;
    const uint64_t deadline =
        has_deadline
            ? hos_scheduler_tick(context->scheduler) + hos_scheduler_ticks_from_nanoseconds((uint64_t)timeout)
            : 0U;

    error = hos_scheduler_block_current(context->scheduler, handles, (uint32_t)count, has_deadline, deadline);

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    // The call is suspended, not finished: `pc` stays on the `SVC` and the pointer is
    // still in `X0`, because `hos_syscall_complete_block` needs it when the scheduler
    // wakes the thread.
    context->outcome = HOS_SYSCALL_BLOCKED;
    return POUND_SUCCESS;
}

static error_t
svc_cancel_synchronization(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    // Horizon's signature: W0 is a `Handle<Thread>` and the answer is a `Result` in W0.
    // Success covers both ways the request can land -- a parked wait is interrupted now, or
    // a flag is armed for the next one -- so the only guest-visible failure is the handle.
    uint32_t result = HOS_RESULT_SUCCESS;
    uint64_t id     = 0U;

    error_t error = hos_process_thread_id(context->process, handle_argument(state->x[0]), &id);

    if (handle_failure_result(error, &result))
    {
        state->x[0] = result;
        return POUND_SUCCESS;
    }

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    // The refusal is about the *context*, not the request: without a run queue Compound cannot
    // name the thread that is calling, so a flag set here could never be consumed by a wait
    // and answering success would promise an interruption that cannot happen. The handle was
    // still resolved first, because Horizon's `0xE401` is the same on every machine.
    if (context->scheduler == NULL)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing svcCancelSynchronization: cancelling a wait needs a scheduler to find "
                        "the thread, and a context without one cannot consume the request.");
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    // A thread that is parked on handles is ended here, by the layer that owns the park.
    // Anything else -- the caller itself, a sleeper, a runnable thread, one never admitted
    // -- gets the durable flag instead, which its next synchronization call consumes.
    if (!hos_scheduler_cancel(context->scheduler, id))
    {
        error = hos_kernel_thread_request_cancel(context->kernel, id);

        if (error != POUND_SUCCESS)
        {
            // Unreachable from here: `hos_process_thread_id` resolved the handle a moment
            // ago, and the thread's own reference keeps the object alive while it can run.
            return error;
        }
    }

    state->x[0] = HOS_RESULT_SUCCESS;
    return POUND_SUCCESS;
}

static error_t
svc_connect_to_named_port(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    const uint8_t *bytes = NULL;

    if (!read_guest(context, state->x[1], HOS_SYSCALL_SERVICE_NAME_MAX, &bytes))
    {
        state->x[0] = HOS_RESULT_INVALID_POINTER;
        return POUND_SUCCESS;
    }

    // Eight raw bytes on the wire, padded with spaces or with NULs depending on how the
    // guest built the string. Both paddings are stripped here: the padding is a
    // marshalling concern, and this layer compares names as C strings.
    char   name[HOS_SYSCALL_SERVICE_NAME_MAX + 1U];
    size_t length = 0U;

    while ((length < HOS_SYSCALL_SERVICE_NAME_MAX) && (bytes[length] != 0U))
    {
        name[length] = (char)bytes[length];
        length++;
    }

    while ((length > 0U) && (name[length - 1U] == ' '))
    {
        length--;
    }

    name[length] = '\0';

    if (length == 0U)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing svcConnectToNamedPort with an empty service name: Horizon documents "
                        "no Result for a nameless port, and inventing one would look like an answer.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    hos_handle_t  handle = HOS_HANDLE_INVALID;
    const error_t error  = hos_process_connect(context->process, name, &handle);

    if (error == POUND_SUCCESS)
    {
        // The handle is answered in `W1`, which is where Horizon documents it, and the
        // caller's stub is what stores it: libnx's `svcConnectToNamedPort` is
        // `str x0, [sp, #-16]!; svc 0x1F; ldr x2, [sp], #16; str w1, [x2]`. A kernel that
        // never set `W1` would have that stub store stale register contents as the session
        // handle.
        //
        // The handle is also written through the pointer `X0` held on entry. Hardware does
        // not do this -- the stub does -- but Compound's guests read out-parameters through
        // `X0`, so writing it keeps both conventions agreeing on the same handle. A guest
        // that only reads `W1` is unaffected by the extra write.
        if (!write_guest_u32(context, state->x[0], handle))
        {
            // The session was opened, but the guest cannot be told its handle. The handle
            // is taken straight back so the process does not keep a session it can never
            // name, and the bad pointer is reported exactly as Horizon would. `W1` is left
            // untouched: hardware's value here has not been established.
            hos_process_close_handle(context->process, handle);
            state->x[0] = HOS_RESULT_INVALID_POINTER;
            return POUND_SUCCESS;
        }

        state->x[1] = handle;
        state->x[0] = HOS_RESULT_SUCCESS;
        return POUND_SUCCESS;
    }

    if (error == POUND_ERROR_NOT_FOUND)
    {
        // Left as a code rather than turned into a Result: a title connecting to a
        // service this build has not registered is a gap in Compound, not a failure the
        // guest caused, and there is no honest Result for it. Not logged, per the
        // contract the code carries; the caller has the system call number and is
        // expected to report it.
        return POUND_ERROR_NOT_FOUND;
    }

    return error;
}

// -----------------------------------------------------------------------------
// Calls that report on the machine
// -----------------------------------------------------------------------------

static error_t
svc_get_current_processor_number(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    // A 32-bit result: the guest reads W0, so the upper half of the register is left
    // zero by the store.
    state->x[0] = context->processor_id;
    return POUND_SUCCESS;
}

static error_t
svc_get_system_tick(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    state->x[0] = context->system_tick;
    return POUND_SUCCESS;
}

static error_t
svc_flush_data_cache(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    const uint64_t size = state->x[1];
    const uint8_t *bytes = NULL;

    if (size == 0U)
    {
        state->x[0] = HOS_RESULT_SUCCESS;
        return POUND_SUCCESS;
    }

    // A host has no guest data cache, so the flush itself is a no-op. The range is still
    // checked, because a flush of an address the guest does not own is a guest bug and
    // this is the last place it is cheap to notice.
    if (!read_guest(context, state->x[0], (size_t)size, &bytes))
    {
        state->x[0] = HOS_RESULT_INVALID_POINTER;
        return POUND_SUCCESS;
    }

    state->x[0] = HOS_RESULT_SUCCESS;
    return POUND_SUCCESS;
}

static error_t
svc_flush_entire_data_cache(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    // Returns nothing, and there is nothing to do: the guest's cache is the host's.
    POUND_UNUSED(context);
    POUND_UNUSED(state);
    return POUND_SUCCESS;
}

static error_t
svc_output_debug_string(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    const uint64_t size  = state->x[1];
    const uint8_t *bytes = NULL;

    if (size == 0U)
    {
        state->x[0] = HOS_RESULT_SUCCESS;
        return POUND_SUCCESS;
    }

    if (!read_guest(context, state->x[0], (size_t)size, &bytes))
    {
        state->x[0] = HOS_RESULT_INVALID_POINTER;
        return POUND_SUCCESS;
    }

    const size_t logged =
        ((size_t)size > HOS_SYSCALL_DEBUG_STRING_LOG_MAX) ? HOS_SYSCALL_DEBUG_STRING_LOG_MAX : (size_t)size;

    if (logged < (size_t)size)
    {
        POUND_LOG_INFO(&thread_logger,
                       "Guest debug output (%llu bytes, first %llu): %.*s",
                       (unsigned long long)size,
                       (unsigned long long)logged,
                       (int)logged,
                       (const char *)bytes);
    }
    else
    {
        POUND_LOG_INFO(&thread_logger, "Guest debug output: %.*s", (int)logged, (const char *)bytes);
    }

    state->x[0] = HOS_RESULT_SUCCESS;
    return POUND_SUCCESS;
}

// -----------------------------------------------------------------------------
// Thread calls
// -----------------------------------------------------------------------------
//
// A thread has two names and these calls use both. The guest only ever holds a *handle*,
// which is the per-process authority that says it may touch this thread at all, so every
// call here starts from a handle and resolves it to the kernel-wide *identifier* the
// kernel and scheduler work in. Nothing here trusts a handle without resolving it: a
// thread another process owns has a different handle there and none here.
//
// A context with no scheduler has no run queue, so creating or starting a thread through
// it would make a thread that can never run. That is a gap rather than a shorter answer,
// and it is refused as one.

/// Reports whether a thread call must be refused because the context has no scheduler.
///
/// True means the caller should return `POUND_ERROR_UNSUPPORTED_SYSCALL`, and the log line
/// naming the call has already been written.
static bool
thread_call_needs_scheduler(const hos_syscall_context_t *POUND_RESTRICT context, const char *POUND_RESTRICT call)
{
    if (context->scheduler != NULL)
    {
        return false;
    }

    POUND_LOG_ERROR(&thread_logger,
                    "Refusing %s: the context has no scheduler, so there is no run queue for a thread to "
                    "be created in or started from.",
                    call);
    return true;
}

static error_t
svc_create_thread(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    if (thread_call_needs_scheduler(context, "svcCreateThread"))
    {
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    // Horizon's signature: X0 is a pointer to the new handle, X1 the entry point, X2 the
    // argument, X3 the stack bottom, W4 the priority and W5 the core. The core is ignored
    // rather than checked: one scheduler is one core, and a title naming a second is asking
    // for a second of something Compound has one of.
    const uint64_t out_handle_address = state->x[0];
    const uint64_t entry              = state->x[1];
    const uint64_t argument           = state->x[2];
    const uint64_t stack_bottom       = state->x[3];
    const uint32_t priority           = (uint32_t)state->x[4];

    if (priority > HOS_THREAD_PRIORITY_LOWEST)
    {
        // A gap rather than a `Result`: Horizon refuses an out-of-range priority, but Compound
        // has not established which `Result` word it uses, and inventing one would be an
        // answer the guest could branch on that nobody verified.
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing svcCreateThread with priority %u: Horizon's own Result for a priority "
                        "outside %u..%u has not been established, and inventing one would look like an "
                        "answer.",
                        (unsigned int)priority,
                        (unsigned int)HOS_THREAD_PRIORITY_HIGHEST,
                        (unsigned int)HOS_THREAD_PRIORITY_LOWEST);
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    // The register file the thread starts with: entry in `pc`, the argument in `x[0]` and
    // the stack pointer in `x[31]`, with everything else zero. Built here because this is
    // the layer that knows Horizon's calling convention; the kernel only copies it.
    guest_state_t initial;
    const error_t initialized = guest_state_init(&initial);

    if (initialized != POUND_SUCCESS)
    {
        return initialized;
    }

    initial.pc    = entry;
    initial.x[0]  = argument;
    initial.x[31] = stack_bottom;

    hos_handle_t  handle = HOS_HANDLE_INVALID;
    const error_t error  = hos_process_create_thread(context->process, &initial, priority, &handle);

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    if (!write_guest_u32(context, out_handle_address, handle))
    {
        // Created but undeliverable: give the handle straight back so the process does not
        // hold a thread it can never name. `W1` is left untouched, since hardware's value on
        // this failure has not been established.
        hos_process_close_handle(context->process, handle);
        state->x[0] = HOS_RESULT_INVALID_POINTER;
        return POUND_SUCCESS;
    }

    // Horizon answers the handle in `W1` and the caller's stub stores that register through
    // the out-pointer; `X1` held the entry point on entry, but it was snapshotted above, so
    // it is free to carry the answer now. The write above keeps a guest that reads the slot
    // through `X0` working too.
    state->x[1] = handle;
    state->x[0] = HOS_RESULT_SUCCESS;
    return POUND_SUCCESS;
}

static error_t
svc_start_thread(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    if (thread_call_needs_scheduler(context, "svcStartThread"))
    {
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    const hos_handle_t handle = handle_argument(state->x[0]);
    uint64_t           id     = 0U;

    uint32_t      result = HOS_RESULT_SUCCESS;
    error_t       error  = hos_process_thread_id(context->process, handle, &id);

    if (handle_failure_result(error, &result))
    {
        state->x[0] = result;
        return POUND_SUCCESS;
    }

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    // The status is read here rather than left to the kernel, because the kernel answers a
    // second start with an error code and this layer is the only place that can turn it
    // into the `Result` a guest compares against. Starting a thread that is already started
    // or has already finished is `0xFA01`, invalid state.
    hos_thread_status_t status = HOS_THREAD_CREATED;
    error                      = hos_kernel_thread_status(context->kernel, id, &status);

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    if (status != HOS_THREAD_CREATED)
    {
        state->x[0] = HOS_RESULT_INVALID_STATE;
        return POUND_SUCCESS;
    }

    error = hos_kernel_thread_start(context->kernel, id);

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    error = hos_scheduler_admit(context->scheduler, context->process, id);

    if (error != POUND_SUCCESS)
    {
        // Started but unschedulable, so it could never run: finish it rather than leave a
        // thread the guest believes is running and the scheduler has never heard of.
        hos_kernel_thread_terminate(context->kernel, id);
        return error;
    }

    state->x[0] = HOS_RESULT_SUCCESS;
    return POUND_SUCCESS;
}

static error_t
svc_exit_thread(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    POUND_UNUSED(state);

    if (thread_call_needs_scheduler(context, "svcExitThread"))
    {
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    // The calling thread is the one the scheduler most recently ran. There is no handle in
    // the arguments to name it with, so the identifier is the only name available.
    const uint64_t id = hos_scheduler_current_thread_id(context->scheduler);

    if (id == 0U)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting svcExitThread: no thread is currently running.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const error_t error = hos_kernel_thread_terminate(context->kernel, id);

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    // Returns nothing, and leaves `pc` on the `SVC`: the thread is over, and the last thing
    // it did is still where a debugger looks. The scheduler reaps the terminated thread on
    // its next decision.
    POUND_LOG_INFO(&thread_logger, "A guest thread called svcExitThread; it is finished.");
    return POUND_SUCCESS;
}

static error_t
svc_get_thread_priority(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    if (thread_call_needs_scheduler(context, "svcGetThreadPriority"))
    {
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    // Horizon's signature: X0 is a pointer to the priority, X1 the handle.
    const uint64_t     out_priority_address = state->x[0];
    const hos_handle_t handle               = handle_argument(state->x[1]);
    uint64_t           id                   = 0U;

    uint32_t      result = HOS_RESULT_SUCCESS;
    error_t       error  = hos_process_thread_id(context->process, handle, &id);

    if (handle_failure_result(error, &result))
    {
        state->x[0] = result;
        return POUND_SUCCESS;
    }

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    uint32_t priority = 0U;
    error             = hos_kernel_thread_priority(context->kernel, id, &priority);

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    // Horizon answers the priority in `W1`, which the caller's stub stores through the
    // out-pointer; `X1` held the handle on entry, but it was snapshotted above. The write
    // through `X0` also happens, for a guest that reads the slot directly.
    const bool delivered = write_guest_u32(context, out_priority_address, priority);

    if (delivered)
    {
        state->x[1] = priority;
    }

    state->x[0] = delivered ? HOS_RESULT_SUCCESS : HOS_RESULT_INVALID_POINTER;
    return POUND_SUCCESS;
}

static error_t
svc_set_thread_priority(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    if (thread_call_needs_scheduler(context, "svcSetThreadPriority"))
    {
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    // Horizon's signature: X0 is the handle, W1 the new priority. The order is the other
    // way round from `svcGetThreadPriority`, which is a real asymmetry and not a typo.
    const hos_handle_t handle   = handle_argument(state->x[0]);
    const uint32_t     priority = (uint32_t)state->x[1];

    if (priority > HOS_THREAD_PRIORITY_LOWEST)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing svcSetThreadPriority with priority %u: Horizon's own Result for a "
                        "priority outside %u..%u has not been established, and inventing one would look "
                        "like an answer.",
                        (unsigned int)priority,
                        (unsigned int)HOS_THREAD_PRIORITY_HIGHEST,
                        (unsigned int)HOS_THREAD_PRIORITY_LOWEST);
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    uint64_t id = 0U;

    uint32_t      result = HOS_RESULT_SUCCESS;
    error_t       error  = hos_process_thread_id(context->process, handle, &id);

    if (handle_failure_result(error, &result))
    {
        state->x[0] = result;
        return POUND_SUCCESS;
    }

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    error = hos_kernel_thread_set_priority(context->kernel, id, priority);

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    state->x[0] = HOS_RESULT_SUCCESS;
    return POUND_SUCCESS;
}

static error_t
svc_get_thread_id(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    if (thread_call_needs_scheduler(context, "svcGetThreadId"))
    {
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    // Horizon's signature: X0 is a pointer to the identifier, X1 the handle. The answer is
    // 64 bits, so it goes through a 64-bit write even though the identifier is small.
    const uint64_t     out_thread_id_address = state->x[0];
    const hos_handle_t handle                = handle_argument(state->x[1]);
    uint64_t           id                    = 0U;

    uint32_t      result = HOS_RESULT_SUCCESS;
    error_t       error  = hos_process_thread_id(context->process, handle, &id);

    if (handle_failure_result(error, &result))
    {
        state->x[0] = result;
        return POUND_SUCCESS;
    }

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    // Horizon's `(Out) X1 ThreadId` is 64 bits, and the caller's stub stores X1 through the
    // out-pointer (`str x1, [x2]`); `X1` held the handle on entry, snapshotted above. The
    // write through `X0` also happens, for a guest that reads the slot directly.
    const bool delivered = write_guest_u64(context, out_thread_id_address, id);

    if (delivered)
    {
        state->x[1] = id;
    }

    state->x[0] = delivered ? HOS_RESULT_SUCCESS : HOS_RESULT_INVALID_POINTER;
    return POUND_SUCCESS;
}

// -----------------------------------------------------------------------------
// Calls that stop the guest
// -----------------------------------------------------------------------------

static error_t
svc_exit_process(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    POUND_UNUSED(state);

    // Reports nothing back: the process is over, and there is no instruction after this
    // one in it to read a result. `pc` is left on the `SVC` rather than advanced past it,
    // so the last thing the process did is still where a debugger looks.
    POUND_LOG_INFO(&thread_logger, "The guest called svcExitProcess; stopping the process.");
    context->outcome = HOS_SYSCALL_EXITED_PROCESS;
    return POUND_SUCCESS;
}

static error_t
svc_break(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    // Logged at warning rather than error: unlike an unimplemented call, a break is the
    // guest doing exactly what it meant to, and the emulator's job is to get out of the
    // way with the reason visible.
    POUND_LOG_WARN(&thread_logger,
                   "The guest called svcBreak (reason 0x%llX, argument 0x%llX, argument 0x%llX).",
                   (unsigned long long)state->x[0],
                   (unsigned long long)state->x[1],
                   (unsigned long long)state->x[2]);

    context->outcome = HOS_SYSCALL_BREAK;
    return POUND_SUCCESS;
}

static error_t
svc_sleep_thread(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    const int64_t nanoseconds = (int64_t)state->x[0];

    if (nanoseconds <= 0)
    {
        // A yield: zero gives the rest of the slice to threads of the same priority, and a
        // negative duration gives it to every thread. Both are the same request to hand the
        // core over early, which is what a round-robin priority queue does with a move to
        // the back. Returns without sleeping, and returns nothing: `X0` keeps the duration
        // the guest passed, because `svcSleepThread` is a `void` call.
        if (context->scheduler != NULL)
        {
            hos_scheduler_yield(context->scheduler);
        }

        return POUND_SUCCESS;
    }

    if (context->scheduler == NULL)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing svcSleepThread(%lld): a sleep needs a scheduler to wake the caller, and "
                        "returning early would run the guest against a clock that did not advance.",
                        (long long)nanoseconds);
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    // The deadline is on the scheduler's clock, not the wall: a sleep of half a second is
    // a number of ticks added to whatever the clock reads now, so it cannot outlive the
    // guest's own sense of time or drift against the instructions it retires.
    const uint64_t deadline =
        hos_scheduler_tick(context->scheduler) + hos_scheduler_ticks_from_nanoseconds((uint64_t)nanoseconds);

    const error_t error = hos_scheduler_sleep_current(context->scheduler, deadline);

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    // Suspended, not finished: the run loop finishes it with `hos_syscall_complete_block`
    // when the clock reaches the deadline. `X0` is left holding the duration, untouched,
    // since a sleep reports nothing.
    context->outcome = HOS_SYSCALL_BLOCKED;
    return POUND_SUCCESS;
}

static error_t
svc_sleep_system(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    // Returns nothing. There is no system to put to sleep, so there is nothing to do and
    // nothing to approximate: the call has no effect a guest can observe.
    POUND_UNUSED(context);
    POUND_UNUSED(state);
    return POUND_SUCCESS;
}

// -----------------------------------------------------------------------------
// Dispatch
// -----------------------------------------------------------------------------

void
hos_syscall_context_init(hos_syscall_context_t *POUND_RESTRICT context,
                         hos_kernel_t *POUND_RESTRICT          kernel,
                         hos_process_t *POUND_RESTRICT         process,
                         guest_memory_t *POUND_RESTRICT        memory)
{
    if (context == NULL)
    {
        return;
    }

    memset(context, 0, sizeof(hos_syscall_context_t));
    context->kernel  = kernel;
    context->process = process;
    context->memory  = memory;

    // Zeroed tick and processor number, rather than whatever was on the stack: a caller
    // that has not said what the clock reads gets tick zero, which is at least a value
    // somebody can look at and ask about.
    context->outcome = HOS_SYSCALL_COMPLETED;
}

void
hos_syscall_context_set_scheduler(hos_syscall_context_t *POUND_RESTRICT context, hos_scheduler_t *POUND_RESTRICT scheduler)
{
    if (context == NULL)
    {
        return;
    }

    context->scheduler = scheduler;
}

void
hos_syscall_context_set_layout(hos_syscall_context_t *POUND_RESTRICT context, hos_memory_layout_t *POUND_RESTRICT layout)
{
    if (context == NULL)
    {
        return;
    }

    context->layout = layout;
}

error_t
hos_syscall_complete_block(hos_syscall_context_t *POUND_RESTRICT context,
                           guest_state_t *POUND_RESTRICT        state,
                           const hos_wake_t *POUND_RESTRICT     wake)
{
    if ((context == NULL) || (state == NULL) || (wake == NULL))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_syscall_complete_block: context, state or wake is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    switch (wake->reason)
    {
        case HOS_WAKE_SIGNALED:
        {
            // A handle the thread was waiting on became ready. The index goes through the
            // pointer the blocking call left in X0, and only then is X0 replaced by the
            // `Result`: afterwards the address is gone.
            const bool delivered = write_guest_u32(context, state->x[0], wake->ready_index);

            if (delivered)
            {
                // `W1` is Horizon's `(Out) W1 HandleIndex`, and the stub that resumes the
                // wait stores it through the out-pointer. The wait was parked with the
                // handle array in X1, so the register has to be set here or the stub reads
                // back the array address as the index.
                state->x[1] = wake->ready_index;
            }

            state->x[0] = delivered ? HOS_RESULT_SUCCESS : HOS_RESULT_INVALID_POINTER;
            break;
        }

        case HOS_WAKE_TIMED_OUT:
            // Nothing became ready before the deadline. A timeout carries no index, so the
            // pointer is not written at all -- which is also what keeps a stale index from
            // pointing the guest at a handle it never asked about.
            state->x[0] = HOS_RESULT_TIMEOUT;
            break;

        case HOS_WAKE_CANCELLED:
            // Another thread cancelled the wait. Like a timeout, it stopped without anything
            // becoming ready, so the index pointer is left alone and `W1` keeps whatever the
            // parked call left there. Horizon's `0xEC01` is the whole answer.
            state->x[0] = HOS_RESULT_INTERRUPTED;
            break;

        case HOS_WAKE_SLEPT:
            // A plain sleep ended. `svcSleepThread` returns nothing, so X0 keeps the
            // duration the guest passed. Writing a `Result` here would be the one thing a
            // void call must not do.
            break;

        default:
            POUND_LOG_ERROR(&thread_logger,
                            "Aborting hos_syscall_complete_block: unknown wake reason %d.",
                            (int)wake->reason);
            return POUND_ERROR_INVALID_ARGUMENT;
    }

    context->outcome = HOS_SYSCALL_COMPLETED;

    // The suspension is over, so the trap is: the call finishes and `pc` steps past the
    // `SVC` it was left on when the thread parked.
    state->pc += 4U;
    return POUND_SUCCESS;
}

error_t
hos_syscall_dispatch(hos_syscall_context_t *POUND_RESTRICT context,
                     uint32_t                             number,
                     guest_state_t *POUND_RESTRICT        state)
{
    if ((context == NULL) || (state == NULL))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_syscall_dispatch: context or state is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    // Reset first, so a caller that reads the outcome after a refusal sees this call's
    // outcome and not the last one's.
    context->outcome = HOS_SYSCALL_COMPLETED;

    if ((context->kernel == NULL) || (context->process == NULL) || (context->memory == NULL))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting svc 0x%02X: the context names no kernel, process or guest memory.",
                        (unsigned int)number);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!hos_process_is_open(context->process))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting svc 0x%02X: the calling process is not open, so there is no handle "
                        "table for the call to act on.",
                        (unsigned int)number);
        return POUND_ERROR_NOT_INITIALIZED;
    }

    if (context->process->kernel != context->kernel)
    {
        // The process's own kernel is the one its handles resolve in, so a context
        // pointing at a different one would have the call checked against one kernel and
        // run against another.
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting svc 0x%02X: the context's kernel is not the calling process's kernel.",
                        (unsigned int)number);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const hos_syscall_entry_t *entry = find_syscall(number);

    if (entry == NULL)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing svc 0x%02X: no Horizon OS system call has that number, so there is "
                        "nothing to dispatch it to.",
                        (unsigned int)number);
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    if (entry->handler == NULL)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing %s (0x%02X): Compound has not implemented it.",
                        entry->name,
                        (unsigned int)number);
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    const error_t error = entry->handler(context, state);

    if ((error == POUND_SUCCESS) && (context->outcome == HOS_SYSCALL_COMPLETED))
    {
        // The call is over, so the trap is: `pc` steps past the `SVC`. The two outcomes
        // that are not `COMPLETED` leave it alone on purpose -- see the header.
        state->pc += 4U;
    }

    return error;
}

/*** end of file ***/


static error_t svc_map_memory(hos_syscall_context_t *context, guest_state_t *state)
{
    if(memory_call_needs_layout(context, "svcMapMemory")) return POUND_ERROR_UNSUPPORTED_SYSCALL;
    const uint64_t dst_addr = state->x[0];
    const uint64_t src_addr = state->x[1];
    const uint64_t size = state->x[2];
    const uint32_t perm = (uint32_t)state->x[3];
    if((dst_addr % HOS_MEMORY_PAGE_SIZE)!=0U||(src_addr % HOS_MEMORY_PAGE_SIZE)!=0U||(size==0U)||(size % HOS_MEMORY_PAGE_SIZE)!=0U){state->x[0]=HOS_RESULT_INVALID_SIZE;return POUND_SUCCESS;}
    uint32_t res = HOS_RESULT_SUCCESS;
    error_t err = hos_memory_layout_map_alias(context->layout, src_addr, dst_addr, size, HOS_MEMORY_STATE_ALIAS, perm, 0U);
    if(!memory_layout_error_to_result(err, &res)){
        if(err==POUND_ERROR_INVALID_ARGUMENT) res = HOS_RESULT_INVALID_CURRENT_MEMORY;
        else if(err==POUND_ERROR_ALLOCATION_FAILED) res = HOS_RESULT_SLAB_EXHAUSTED;
        else if(err==POUND_ERROR_GUEST_ADDRESS_OUT_OF_BOUNDS||err==POUND_ERROR_GUEST_ADDRESS_OVERFLOW) res = HOS_RESULT_INVALID_ADDRESS;
        else return err;
    }
    state->x[0]=res; return POUND_SUCCESS;
}

static error_t svc_unmap_memory(hos_syscall_context_t *context, guest_state_t *state)
{
    if(memory_call_needs_layout(context, "svcUnmapMemory")) return POUND_ERROR_UNSUPPORTED_SYSCALL;
    const uint64_t addr = state->x[0];
    const uint64_t size = state->x[1];
    if((addr % HOS_MEMORY_PAGE_SIZE)!=0U||(size==0U)||(size % HOS_MEMORY_PAGE_SIZE)!=0U){state->x[0]=HOS_RESULT_INVALID_SIZE;return POUND_SUCCESS;}
    uint32_t res = HOS_RESULT_SUCCESS;
    error_t err = hos_memory_layout_unmap(context->layout, addr, size);
    if(!memory_layout_error_to_result(err, &res)){
        if(err==POUND_ERROR_INVALID_ARGUMENT) res = HOS_RESULT_INVALID_CURRENT_MEMORY;
        else if(err==POUND_ERROR_ALLOCATION_FAILED) res = HOS_RESULT_SLAB_EXHAUSTED;
        else if(err==POUND_ERROR_GUEST_ADDRESS_OUT_OF_BOUNDS||err==POUND_ERROR_GUEST_ADDRESS_OVERFLOW) res = HOS_RESULT_INVALID_ADDRESS;
        else return err;
    }
    state->x[0]=res; return POUND_SUCCESS;
}

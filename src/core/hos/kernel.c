#include "kernel.h"
#include "log.h"
#include "memory/memory.h"
#include <string.h>

/// Marks a kernel that `hos_kernel_init` has prepared.
#define HOS_KERNEL_MAGIC 0x484F534BU // 'H''O''S''K'

/// Marks a process whose handle table is live.
#define HOS_PROCESS_MAGIC 0x484F5350U // 'H''O''S''P'

/// The low bits of a handle that carry the slot index.
#define HOS_HANDLE_INDEX_MASK ((1U << HOS_HANDLE_INDEX_BITS) - 1U)

/// The high bits of a handle that carry the slot generation.
#define HOS_HANDLE_GENERATION_MASK (UINT32_MAX >> HOS_HANDLE_INDEX_BITS)

/// Names an object kind the way a sentence needs it named.
///
/// The article is part of the string because "a event" in a log line reads as a bug in
/// the emulator rather than in the guest, and these lines are read by people who are
/// already looking at a bug.
static const char *
object_kind_name(hos_object_type_t type)
{
    switch (type)
    {
        case HOS_OBJECT_EVENT: return "an event";
        case HOS_OBJECT_THREAD: return "a thread";
        case HOS_OBJECT_SESSION: return "a session";
    }

    return "an object of no known kind";
}

/// A thread's own state, carried inside the object slot that names it.
///
/// Embedded rather than pointed at, so the record's lifetime is exactly the slot's: there
/// is no second allocation to fail, nothing to free on destruction, and no window in which
/// a handle resolves to an object whose thread has already gone. The cost is that every
/// object slot is as large as a register file, which a title with a few hundred sessions
/// pays a few hundred kilobytes for -- and the alternative, a table of pointers that has
/// to be kept in step with the object table, is where that kind of bug lives.
struct hos_thread
{
    /// The register file: thirty-two registers, the program counter, the condition
    /// flags. Copied in at creation, out and back by the thread accessors, and owned by
    /// nobody but this slot.
    guest_state_t state;

    /// Kernel-wide identifier. Never reused, so it outlives the thread it names.
    uint64_t id;

    /// The thread's priority, in Horizon's downwards numbering.
    uint32_t priority;

    /// Where the thread is in its life.
    hos_thread_status_t status;

    /// Whether `svcCancelSynchronization` has asked for this thread's next
    /// synchronization call to be interrupted, and nothing has consumed the request yet.
    ///
    /// On the object rather than in the scheduler because Horizon keeps it on the thread:
    /// a cancellation survives until the thread synchronizes, which may be long after the
    /// call that requested it, and it applies even to a thread the scheduler never admitted.
    bool cancel_requested;
};

/// One object slot, in the kernel's table.
///
/// A slot is free when its reference count is zero, which is also what makes the
/// table's free list unnecessary: `allocate_object` scans for the first free slot,
/// and the scan is over at most a few dozen entries on any real title.
struct hos_object
{
    /// What a handle to this slot names.
    hos_object_type_t type;

    /// How many handles name this slot, summed over every process. Zero means the
    /// slot is free.
    ///
    /// For a started thread this count includes one reference taken by the thread on its
    /// own behalf and released when it terminates -- see `hos_process_thread_start` --
    /// which is why a thread cannot be freed while it is still able to run.
    uint32_t references;

    /// Index of the service this session is connected to. Meaningful only when
    /// `type` is `HOS_OBJECT_SESSION`, and stable for the object's whole life
    /// because services are append-only.
    uint32_t service_index;

    /// Clear mode of the event. Meaningful only when `type` is `HOS_OBJECT_EVENT`.
    hos_event_mode_t event_mode;

    /// Whether the event is currently signalled.
    bool event_signaled;

    /// The thread. Meaningful only when `type` is `HOS_OBJECT_THREAD`.
    struct hos_thread thread;
};

/// One service entry, in the kernel's append-only registry.
struct hos_service
{
    /// The name a title connects by, NUL-terminated within `HOS_SERVICE_NAME_MAX + 1`
    /// bytes.
    char name[HOS_SERVICE_NAME_MAX + 1U];

    /// Handles every request sent to a session connected to this service.
    hos_service_handler_fn handler;

    /// Passed to `handler` as its first argument. Borrowed, never freed here.
    void *context;
};

/// One handle slot, in one process's table.
struct hos_handle_slot
{
    /// Generation of this slot's current occupant. Loaded into every handle that
    /// names it and advanced on close, so a handle that outlives its object fails to
    /// resolve instead of naming whatever takes the slot next.
    uint32_t generation;

    /// Index into the owning kernel's object table. Meaningful only while `in_use`.
    uint32_t object_index;

    /// Whether this slot currently names an object.
    bool in_use;
};

// -----------------------------------------------------------------------------
// Handle encoding
// -----------------------------------------------------------------------------

/// Packs a slot index and its generation into a handle value.
///
/// The index is stored one-based so that slot 0 does not produce the handle 0, which
/// is reserved for "no object".
static hos_handle_t
make_handle(uint32_t generation, uint32_t index)
{
    return (hos_handle_t)(((generation & HOS_HANDLE_GENERATION_MASK) << HOS_HANDLE_INDEX_BITS) |
                          ((index + 1U) & HOS_HANDLE_INDEX_MASK));
}

/// Extracts the slot index a handle refers to.
static uint32_t
handle_slot_number(hos_handle_t handle)
{
    return (uint32_t)(handle & HOS_HANDLE_INDEX_MASK);
}

/// Extracts the generation a handle was minted with.
static uint32_t
handle_generation(hos_handle_t handle)
{
    return (uint32_t)(handle >> HOS_HANDLE_INDEX_BITS);
}

// -----------------------------------------------------------------------------
// Table growth
// -----------------------------------------------------------------------------

/// Grows the kernel's object table by doubling it, preserving every slot.
///
/// The whole old table is copied, not just its live prefix, because live objects are
/// not necessarily at the front once slots have been recycled.
static error_t
grow_object_table(hos_kernel_t *POUND_RESTRICT kernel)
{
    const uint32_t new_capacity =
        (kernel->object_capacity == 0U) ? HOS_KERNEL_INITIAL_OBJECTS : (kernel->object_capacity * 2U);

    struct hos_object *objects = (struct hos_object *)memory_subsystem_allocate(
        _Alignof(struct hos_object), (size_t)new_capacity * sizeof(struct hos_object));

    if (objects == NULL)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refused to grow the Horizon OS object table to %u slots: allocation failed.",
                        (unsigned int)new_capacity);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    memset(objects, 0, (size_t)new_capacity * sizeof(struct hos_object));

    if (kernel->objects != NULL)
    {
        memcpy(objects,
               kernel->objects,
               (size_t)kernel->object_capacity * sizeof(struct hos_object));
        memory_subsystem_free(kernel->objects);
    }

    kernel->objects         = objects;
    kernel->object_capacity = new_capacity;
    return POUND_SUCCESS;
}

/// Grows the kernel's service table by doubling it.
///
/// This moves existing entries, which is why a session stores its service's *index*
/// rather than a pointer to its entry: an index stays true across a move, and the
/// handler is looked up again on every request.
static error_t
grow_service_table(hos_kernel_t *POUND_RESTRICT kernel)
{
    const uint32_t new_capacity =
        (kernel->service_capacity == 0U) ? HOS_KERNEL_INITIAL_SERVICES : (kernel->service_capacity * 2U);

    struct hos_service *services = (struct hos_service *)memory_subsystem_allocate(
        _Alignof(struct hos_service), (size_t)new_capacity * sizeof(struct hos_service));

    if (services == NULL)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refused to grow the Horizon OS service table to %u entries: allocation failed.",
                        (unsigned int)new_capacity);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    memset(services, 0, (size_t)new_capacity * sizeof(struct hos_service));

    if (kernel->services != NULL)
    {
        memcpy(services,
               kernel->services,
               (size_t)kernel->service_count * sizeof(struct hos_service));
        memory_subsystem_free(kernel->services);
    }

    kernel->services         = services;
    kernel->service_capacity = new_capacity;
    return POUND_SUCCESS;
}

/// Grows a process's handle table by doubling it, preserving every generation.
///
/// Refuses once the table is large enough that the one-based slot number would not fit
/// in `HOS_HANDLE_INDEX_BITS`, because past that point two slots would share a handle
/// value and the generation check could not tell them apart.
static error_t
grow_handle_table(hos_process_t *POUND_RESTRICT process)
{
    const uint32_t new_capacity = (process->handle_capacity == 0U) ? HOS_PROCESS_INITIAL_HANDLES
                                                                  : (process->handle_capacity * 2U);

    if (new_capacity > HOS_HANDLE_INDEX_MASK)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refused to grow a process handle table past %u slots: a slot number would not "
                        "fit in a handle.",
                        (unsigned int)(HOS_HANDLE_INDEX_MASK - 1U));
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    struct hos_handle_slot *handles = (struct hos_handle_slot *)memory_subsystem_allocate(
        _Alignof(struct hos_handle_slot), (size_t)new_capacity * sizeof(struct hos_handle_slot));

    if (handles == NULL)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refused to grow a process handle table to %u slots: allocation failed.",
                        (unsigned int)new_capacity);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    memset(handles, 0, (size_t)new_capacity * sizeof(struct hos_handle_slot));

    if (process->handles != NULL)
    {
        memcpy(handles,
               process->handles,
               (size_t)process->handle_capacity * sizeof(struct hos_handle_slot));
        memory_subsystem_free(process->handles);
    }

    process->handles         = handles;
    process->handle_capacity = new_capacity;
    return POUND_SUCCESS;
}

// -----------------------------------------------------------------------------
// Object and handle allocation
// -----------------------------------------------------------------------------

/// Reserves a free object slot of `type`, growing the table if it is full.
///
/// The slot comes back with a zero reference count, so it is still free as far as every
/// other path is concerned until a handle is attached to it.
static error_t
allocate_object(hos_kernel_t *POUND_RESTRICT kernel, hos_object_type_t type, uint32_t *POUND_RESTRICT out_index)
{
    if (kernel->object_count == kernel->object_capacity)
    {
        const error_t error = grow_object_table(kernel);

        if (error != POUND_SUCCESS)
        {
            return error;
        }
    }

    for (uint32_t index = 0U; index < kernel->object_capacity; index++)
    {
        if (kernel->objects[index].references == 0U)
        {
            memset(&kernel->objects[index], 0, sizeof(struct hos_object));
            kernel->objects[index].type = type;
            *out_index                  = index;
            return POUND_SUCCESS;
        }
    }

    // Every slot is referenced, yet the count said otherwise. The two can only disagree
    // if something outside this module wrote to the table.
    POUND_LOG_ERROR(&thread_logger,
                    "Refused to allocate a Horizon OS object: %u slots are all referenced but only %u "
                    "are counted as live.",
                    (unsigned int)kernel->object_capacity,
                    (unsigned int)kernel->object_count);
    return POUND_ERROR_CORRUPTED;
}

/// Takes one reference to `object_index` and mints a handle that names it.
///
/// Must be called with the kernel lock held.
static error_t
allocate_handle(hos_process_t *POUND_RESTRICT process, uint32_t object_index, hos_handle_t *POUND_RESTRICT out_handle)
{
    if (process->handle_count == process->handle_capacity)
    {
        const error_t error = grow_handle_table(process);

        if (error != POUND_SUCCESS)
        {
            return error;
        }
    }

    for (uint32_t index = 0U; index < process->handle_capacity; index++)
    {
        struct hos_handle_slot *slot = &process->handles[index];

        if (slot->in_use)
        {
            continue;
        }

        slot->in_use       = true;
        slot->object_index = object_index;
        process->handle_count++;

        struct hos_object *object = &process->kernel->objects[object_index];

        if (object->references == 0U)
        {
            process->kernel->object_count++;
        }

        object->references++;
        *out_handle = make_handle(slot->generation, index);
        return POUND_SUCCESS;
    }

    // As in `allocate_object`, an unreachable state that is only reachable through
    // memory the module does not own.
    POUND_LOG_ERROR(&thread_logger,
                    "Refused to allocate a Horizon OS handle: %u slots are all in use but only %u are "
                    "counted as live.",
                    (unsigned int)process->handle_capacity,
                    (unsigned int)process->handle_count);
    return POUND_ERROR_CORRUPTED;
}

/// Drops one reference to `object_index`, freeing the slot if it was the last.
///
/// Must be called with the kernel lock held.
static void
release_object(hos_kernel_t *POUND_RESTRICT kernel, uint32_t object_index)
{
    struct hos_object *object = &kernel->objects[object_index];

    if (object->references > 0U)
    {
        object->references--;

        if (object->references == 0U)
        {
            memset(object, 0, sizeof(struct hos_object));

            if (kernel->object_count > 0U)
            {
                kernel->object_count--;
            }
        }
    }
}

/// Resolves `handle` to the object slot it names in `process`.
///
/// Must be called with the kernel lock held. A handle that names nothing is always
/// worth a log line -- see `POUND_ERROR_INVALID_HANDLE` -- because it is either a guest
/// bug or an emulator bug, never an ordinary answer.
static error_t
resolve_handle(const hos_process_t *POUND_RESTRICT process,
               hos_handle_t                        handle,
               uint32_t *POUND_RESTRICT            out_object_index)
{
    const uint32_t slot_number = handle_slot_number(handle);

    if (handle == HOS_HANDLE_INVALID || slot_number == 0U)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejected handle 0x%08X from process %p: zero names no object.",
                        (unsigned int)handle,
                        (const void *)process);
        return POUND_ERROR_INVALID_HANDLE;
    }

    const uint32_t index = slot_number - 1U;

    if (index >= process->handle_capacity)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejected handle 0x%08X from process %p: slot %u is outside the table of %u slots.",
                        (unsigned int)handle,
                        (const void *)process,
                        (unsigned int)index,
                        (unsigned int)process->handle_capacity);
        return POUND_ERROR_INVALID_HANDLE;
    }

    const struct hos_handle_slot *slot = &process->handles[index];

    if (!slot->in_use)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejected handle 0x%08X from process %p: slot %u is closed.",
                        (unsigned int)handle,
                        (const void *)process,
                        (unsigned int)index);
        return POUND_ERROR_INVALID_HANDLE;
    }

    if (handle_generation(handle) != slot->generation)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejected handle 0x%08X from process %p: slot %u has generation %u, not %u; the "
                        "handle outlived the object it named.",
                        (unsigned int)handle,
                        (const void *)process,
                        (unsigned int)index,
                        (unsigned int)slot->generation,
                        (unsigned int)handle_generation(handle));
        return POUND_ERROR_INVALID_HANDLE;
    }

    *out_object_index = slot->object_index;
    return POUND_SUCCESS;
}

/// Resolves `handle` and requires the object it names to be of `type`.
///
/// Must be called with the kernel lock held. A handle that names the wrong kind of
/// object is an argument error rather than an invalid handle: the handle itself is
/// perfectly good, it just names something the caller cannot use for this operation.
static error_t
resolve_handle_as(const hos_process_t *POUND_RESTRICT process,
                  hos_handle_t                        handle,
                  hos_object_type_t                   type,
                  uint32_t *POUND_RESTRICT            out_object_index)
{
    uint32_t         object_index = 0U;
    const error_t    error        = resolve_handle(process, handle, &object_index);

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    const hos_object_type_t actual = process->kernel->objects[object_index].type;

    if (actual != type)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejected handle 0x%08X from process %p: it names %s, and this operation needs "
                        "%s.",
                        (unsigned int)handle,
                        (const void *)process,
                        object_kind_name(actual),
                        object_kind_name(type));
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    *out_object_index = object_index;
    return POUND_SUCCESS;
}

// -----------------------------------------------------------------------------
// Shared entry-point guards
// -----------------------------------------------------------------------------

/// Checks the arguments and state every kernel-level entry point shares.
///
/// Returns `POUND_SUCCESS` with the kernel's magic verified.
static error_t
require_kernel(const hos_kernel_t *POUND_RESTRICT kernel, const char *POUND_RESTRICT operation)
{
    if (kernel == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting %s: kernel is NULL.", operation);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (kernel->magic != HOS_KERNEL_MAGIC)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting %s: the kernel was never initialised, or was already destroyed.",
                        operation);
        return POUND_ERROR_NOT_INITIALIZED;
    }

    return POUND_SUCCESS;
}

/// Validates a service name and reports its length.
///
/// Returns `POUND_SUCCESS` with `*out_length` set, having refused a NULL, empty or
/// over-long name.
static error_t
require_service_name(const char *POUND_RESTRICT name, const char *POUND_RESTRICT operation, size_t *out_length)
{
    if (name == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting %s: service name is NULL.", operation);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const size_t length = strlen(name);

    if ((length == 0U) || (length > HOS_SERVICE_NAME_MAX))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting %s: service name is %zu characters; it must be between 1 and %u.",
                        operation,
                        length,
                        (unsigned int)HOS_SERVICE_NAME_MAX);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    *out_length = length;
    return POUND_SUCCESS;
}

/// Returns the index of the service called `name`, or `UINT32_MAX` when there is none.
///
/// Must be called with the kernel lock held. A miss is an ordinary answer and does not
/// log: probing for a service a firmware version does not have is what titles do.
static uint32_t
service_index_of(const hos_kernel_t *POUND_RESTRICT kernel, const char *POUND_RESTRICT name)
{
    for (uint32_t index = 0U; index < kernel->service_count; index++)
    {
        if (strcmp(kernel->services[index].name, name) == 0)
        {
            return index;
        }
    }

    return UINT32_MAX;
}

/// Counts the live thread objects. Must be called with the kernel lock held.
static uint32_t
count_threads_locked(const hos_kernel_t *POUND_RESTRICT kernel)
{
    uint32_t count = 0U;

    for (uint32_t index = 0U; index < kernel->object_capacity; index++)
    {
        const struct hos_object *object = &kernel->objects[index];

        if ((object->references > 0U) && (object->type == HOS_OBJECT_THREAD))
        {
            count++;
        }
    }

    return count;
}

// -----------------------------------------------------------------------------
// Kernel lifecycle
// -----------------------------------------------------------------------------

error_t
hos_kernel_init(hos_kernel_t *POUND_RESTRICT kernel)
{
    if (kernel == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_kernel_init: kernel is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (kernel->magic == HOS_KERNEL_MAGIC)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_kernel_init: the kernel is already initialised.");
        return POUND_ERROR_ALREADY_INITIALIZED;
    }

    memset(kernel, 0, sizeof(hos_kernel_t));

    const error_t lock_error = mutex_init(&kernel->lock);

    if (lock_error != POUND_SUCCESS)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting hos_kernel_init: the kernel lock could not be initialised (%s).",
                        pound_error_to_string(lock_error));
        return lock_error;
    }

    kernel->objects = (struct hos_object *)memory_subsystem_allocate(
        _Alignof(struct hos_object), (size_t)HOS_KERNEL_INITIAL_OBJECTS * sizeof(struct hos_object));

    if (kernel->objects == NULL)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting hos_kernel_init: the initial object table could not be allocated.");
        mutex_destroy(&kernel->lock);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    memset(kernel->objects, 0, (size_t)HOS_KERNEL_INITIAL_OBJECTS * sizeof(struct hos_object));
    kernel->object_capacity = HOS_KERNEL_INITIAL_OBJECTS;

    kernel->services = (struct hos_service *)memory_subsystem_allocate(
        _Alignof(struct hos_service), (size_t)HOS_KERNEL_INITIAL_SERVICES * sizeof(struct hos_service));

    if (kernel->services == NULL)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting hos_kernel_init: the initial service table could not be allocated.");
        memory_subsystem_free(kernel->objects);
        kernel->objects         = NULL;
        kernel->object_capacity = 0U;
        mutex_destroy(&kernel->lock);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    memset(kernel->services, 0, (size_t)HOS_KERNEL_INITIAL_SERVICES * sizeof(struct hos_service));
    kernel->service_capacity = HOS_KERNEL_INITIAL_SERVICES;

    // One-based, so that a zeroed `uint64_t` is "no thread" the same way a zero handle
    // names no object.
    kernel->next_thread_id = 1U;
    kernel->magic          = HOS_KERNEL_MAGIC;

    POUND_LOG_INFO(&thread_logger,
                   "Horizon OS kernel ready: %u object slots, %u service entries.",
                   (unsigned int)kernel->object_capacity,
                   (unsigned int)kernel->service_capacity);
    return POUND_SUCCESS;
}

error_t
hos_kernel_destroy(hos_kernel_t *POUND_RESTRICT kernel)
{
    if (kernel == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_kernel_destroy: kernel is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (kernel->magic != HOS_KERNEL_MAGIC)
    {
        return POUND_SUCCESS;
    }

    mutex_lock(&kernel->lock);

    if (kernel->live_processes != 0U)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refused to destroy the Horizon OS kernel: %u process%s still open.",
                        (unsigned int)kernel->live_processes,
                        (kernel->live_processes == 1U) ? " is" : "es are");
        mutex_unlock(&kernel->lock);
        return POUND_ERROR_BUSY;
    }

    memory_subsystem_free(kernel->objects);
    memory_subsystem_free(kernel->services);

    const uint32_t object_capacity  = kernel->object_capacity;
    const uint32_t service_capacity = kernel->service_capacity;

    // The lock is destroyed before the struct is cleared, because clearing it first would
    // hand `mutex_destroy` a zeroed mutex to release -- which is a no-op on Windows but an
    // invalid object on POSIX.
    mutex_unlock(&kernel->lock);
    mutex_destroy(&kernel->lock);
    memset(kernel, 0, sizeof(hos_kernel_t));

    POUND_LOG_INFO(&thread_logger,
                   "Horizon OS kernel destroyed: %u object slots and %u service entries released.",
                   (unsigned int)object_capacity,
                   (unsigned int)service_capacity);
    return POUND_SUCCESS;
}

bool
hos_kernel_is_initialized(const hos_kernel_t *POUND_RESTRICT kernel)
{
    return (kernel != NULL) && (kernel->magic == HOS_KERNEL_MAGIC);
}

uint32_t
hos_kernel_object_count(const hos_kernel_t *POUND_RESTRICT kernel)
{
    if (require_kernel(kernel, "hos_kernel_object_count") != POUND_SUCCESS)
    {
        return 0U;
    }

    hos_kernel_t *mutable_kernel = (hos_kernel_t *)(uintptr_t)kernel;

    mutex_lock(&mutable_kernel->lock);
    const uint32_t count = mutable_kernel->object_count;
    mutex_unlock(&mutable_kernel->lock);

    return count;
}

uint32_t
hos_kernel_live_process_count(const hos_kernel_t *POUND_RESTRICT kernel)
{
    if (require_kernel(kernel, "hos_kernel_live_process_count") != POUND_SUCCESS)
    {
        return 0U;
    }

    hos_kernel_t *mutable_kernel = (hos_kernel_t *)(uintptr_t)kernel;

    mutex_lock(&mutable_kernel->lock);
    const uint32_t count = mutable_kernel->live_processes;
    mutex_unlock(&mutable_kernel->lock);

    return count;
}

uint32_t
hos_kernel_thread_count(const hos_kernel_t *POUND_RESTRICT kernel)
{
    if (require_kernel(kernel, "hos_kernel_thread_count") != POUND_SUCCESS)
    {
        return 0U;
    }

    hos_kernel_t *mutable_kernel = (hos_kernel_t *)(uintptr_t)kernel;

    mutex_lock(&mutable_kernel->lock);
    const uint32_t count = count_threads_locked(mutable_kernel);
    mutex_unlock(&mutable_kernel->lock);

    return count;
}

// -----------------------------------------------------------------------------
// Services
// -----------------------------------------------------------------------------

error_t
hos_kernel_register_service(hos_kernel_t *POUND_RESTRICT kernel,
                            const char *POUND_RESTRICT name,
                            hos_service_handler_fn      handler,
                            void *POUND_RESTRICT        context)
{
    const error_t kernel_error = require_kernel(kernel, "hos_kernel_register_service");

    if (kernel_error != POUND_SUCCESS)
    {
        return kernel_error;
    }

    size_t        name_length = 0U;
    const error_t name_error  = require_service_name(name, "hos_kernel_register_service", &name_length);

    if (name_error != POUND_SUCCESS)
    {
        return name_error;
    }

    if (handler == NULL)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting hos_kernel_register_service: service \"%s\" has no handler.",
                        name);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    mutex_lock(&kernel->lock);

    if (service_index_of(kernel, name) != UINT32_MAX)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refused to register service \"%s\": a service of that name is already registered.",
                        name);
        mutex_unlock(&kernel->lock);
        return POUND_ERROR_ALREADY_INITIALIZED;
    }

    if (kernel->service_count == kernel->service_capacity)
    {
        const error_t grow_error = grow_service_table(kernel);

        if (grow_error != POUND_SUCCESS)
        {
            mutex_unlock(&kernel->lock);
            return grow_error;
        }
    }

    struct hos_service *entry = &kernel->services[kernel->service_count];

    memcpy(entry->name, name, name_length + 1U);
    entry->handler = handler;
    entry->context = context;
    kernel->service_count++;

    POUND_LOG_INFO(&thread_logger,
                   "Registered Horizon OS service \"%s\" as entry %u.",
                   name,
                   (unsigned int)(kernel->service_count - 1U));

    mutex_unlock(&kernel->lock);
    return POUND_SUCCESS;
}

bool
hos_kernel_has_service(const hos_kernel_t *POUND_RESTRICT kernel, const char *POUND_RESTRICT name)
{
    if ((require_kernel(kernel, "hos_kernel_has_service") != POUND_SUCCESS) || (name == NULL) ||
        (strlen(name) > HOS_SERVICE_NAME_MAX))
    {
        return false;
    }

    hos_kernel_t *mutable_kernel = (hos_kernel_t *)(uintptr_t)kernel;

    mutex_lock(&mutable_kernel->lock);
    const bool present = (service_index_of(mutable_kernel, name) != UINT32_MAX);
    mutex_unlock(&mutable_kernel->lock);

    return present;
}

uint32_t
hos_kernel_service_count(const hos_kernel_t *POUND_RESTRICT kernel)
{
    if (require_kernel(kernel, "hos_kernel_service_count") != POUND_SUCCESS)
    {
        return 0U;
    }

    hos_kernel_t *mutable_kernel = (hos_kernel_t *)(uintptr_t)kernel;

    mutex_lock(&mutable_kernel->lock);
    const uint32_t count = mutable_kernel->service_count;
    mutex_unlock(&mutable_kernel->lock);

    return count;
}

bool
hos_kernel_service_name_at(const hos_kernel_t *POUND_RESTRICT kernel,
                           uint32_t                           index,
                           char *POUND_RESTRICT                out)
{
    if ((require_kernel(kernel, "hos_kernel_service_name_at") != POUND_SUCCESS) || (out == NULL))
    {
        return false;
    }

    hos_kernel_t *mutable_kernel = (hos_kernel_t *)(uintptr_t)kernel;

    mutex_lock(&mutable_kernel->lock);

    if (index >= mutable_kernel->service_count)
    {
        mutex_unlock(&mutable_kernel->lock);
        return false;
    }

    memcpy(out, mutable_kernel->services[index].name, HOS_SERVICE_NAME_MAX + 1U);
    mutex_unlock(&mutable_kernel->lock);

    return true;
}

// -----------------------------------------------------------------------------
// Process lifecycle
// -----------------------------------------------------------------------------

error_t
hos_process_create(hos_kernel_t *POUND_RESTRICT kernel, hos_process_t *POUND_RESTRICT process)
{
    const error_t kernel_error = require_kernel(kernel, "hos_process_create");

    if (kernel_error != POUND_SUCCESS)
    {
        return kernel_error;
    }

    if (process == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_process_create: process is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (process->magic == HOS_PROCESS_MAGIC)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_process_create: the process is already open.");
        return POUND_ERROR_ALREADY_INITIALIZED;
    }

    mutex_lock(&kernel->lock);

    struct hos_handle_slot *handles = (struct hos_handle_slot *)memory_subsystem_allocate(
        _Alignof(struct hos_handle_slot),
        (size_t)HOS_PROCESS_INITIAL_HANDLES * sizeof(struct hos_handle_slot));

    if (handles == NULL)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting hos_process_create: the initial handle table could not be allocated.");
        mutex_unlock(&kernel->lock);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    memset(handles, 0, (size_t)HOS_PROCESS_INITIAL_HANDLES * sizeof(struct hos_handle_slot));

    memset(process, 0, sizeof(hos_process_t));
    process->kernel          = kernel;
    process->handles         = handles;
    process->handle_capacity = HOS_PROCESS_INITIAL_HANDLES;
    process->magic           = HOS_PROCESS_MAGIC;

    kernel->live_processes++;

    POUND_LOG_INFO(&thread_logger,
                   "Horizon OS process %p created on kernel %p with %u handle slots.",
                   (const void *)process,
                   (const void *)kernel,
                   (unsigned int)process->handle_capacity);

    mutex_unlock(&kernel->lock);
    return POUND_SUCCESS;
}

void
hos_process_close(hos_process_t *POUND_RESTRICT process)
{
    if ((process == NULL) || (process->magic != HOS_PROCESS_MAGIC))
    {
        return;
    }

    hos_kernel_t *kernel = process->kernel;
    mutex_lock(&kernel->lock);

    uint32_t released = 0U;

    for (uint32_t index = 0U; index < process->handle_capacity; index++)
    {
        struct hos_handle_slot *slot = &process->handles[index];

        if (!slot->in_use)
        {
            continue;
        }

        const struct hos_object *object = &kernel->objects[slot->object_index];

        if ((object->type == HOS_OBJECT_THREAD) && (object->thread.status == HOS_THREAD_STARTED))
        {
            // A started thread holds a reference on its own object, so closing the process
            // has to release that one too -- otherwise a running thread's object would leak
            // with nothing left able to run or finish it. Marking it terminated first is what
            // makes the state honest for any handle another process might still hold.
            kernel->objects[slot->object_index].thread.status = HOS_THREAD_TERMINATED;
            release_object(kernel, slot->object_index);
        }

        release_object(kernel, slot->object_index);
        released++;
    }

    memory_subsystem_free(process->handles);

    if (kernel->live_processes > 0U)
    {
        kernel->live_processes--;
    }

    memset(process, 0, sizeof(hos_process_t));
    mutex_unlock(&kernel->lock);

    POUND_LOG_INFO(&thread_logger,
                   "Horizon OS process %p closed: %u handle%s released.",
                   (const void *)process,
                   (unsigned int)released,
                   (released == 1U) ? " was" : "s were");
}

bool
hos_process_is_open(const hos_process_t *POUND_RESTRICT process)
{
    return (process != NULL) && (process->magic == HOS_PROCESS_MAGIC);
}

uint32_t
hos_process_handle_count(const hos_process_t *POUND_RESTRICT process)
{
    if (!hos_process_is_open(process))
    {
        return 0U;
    }

    hos_process_t *mutable_process = (hos_process_t *)(uintptr_t)process;
    hos_kernel_t  *kernel          = mutable_process->kernel;

    mutex_lock(&kernel->lock);
    const uint32_t count = mutable_process->handle_count;
    mutex_unlock(&kernel->lock);

    return count;
}

// -----------------------------------------------------------------------------
// Handles
// -----------------------------------------------------------------------------

error_t
hos_process_create_event(hos_process_t *POUND_RESTRICT process,
                         hos_event_mode_t             mode,
                         bool                         initially_signaled,
                         hos_handle_t *POUND_RESTRICT out_handle)
{
    if (process == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_process_create_event: process is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (out_handle == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_process_create_event: out_handle is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!hos_process_is_open(process))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting hos_process_create_event: the process was never created, or was already "
                        "closed.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    hos_kernel_t *kernel = process->kernel;
    mutex_lock(&kernel->lock);

    uint32_t      object_index = 0U;
    error_t       error        = allocate_object(kernel, HOS_OBJECT_EVENT, &object_index);

    if (error == POUND_SUCCESS)
    {
        kernel->objects[object_index].event_mode     = mode;
        kernel->objects[object_index].event_signaled = initially_signaled;
        error = allocate_handle(process, object_index, out_handle);
    }

    mutex_unlock(&kernel->lock);
    return error;
}

error_t
hos_process_connect(hos_process_t *POUND_RESTRICT process,
                    const char *POUND_RESTRICT name,
                    hos_handle_t *POUND_RESTRICT out_handle)
{
    if (process == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_process_connect: process is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (out_handle == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_process_connect: out_handle is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!hos_process_is_open(process))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting hos_process_connect: the process was never created, or was already closed.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    size_t        ignored_length = 0U;
    const error_t name_error     = require_service_name(name, "hos_process_connect", &ignored_length);

    if (name_error != POUND_SUCCESS)
    {
        return name_error;
    }

    hos_kernel_t *kernel = process->kernel;
    mutex_lock(&kernel->lock);

    const uint32_t service_index = service_index_of(kernel, name);

    if (service_index == UINT32_MAX)
    {
        // A question, not a failure: this does not log, per the contract shared with
        // POUND_ERROR_NOT_FOUND everywhere else in the emulator.
        mutex_unlock(&kernel->lock);
        return POUND_ERROR_NOT_FOUND;
    }

    uint32_t object_index = 0U;
    error_t  error        = allocate_object(kernel, HOS_OBJECT_SESSION, &object_index);

    if (error == POUND_SUCCESS)
    {
        kernel->objects[object_index].service_index = service_index;
        error = allocate_handle(process, object_index, out_handle);
    }

    mutex_unlock(&kernel->lock);
    return error;
}

error_t
hos_process_duplicate_handle_into(hos_process_t *POUND_RESTRICT source,
                                  hos_handle_t                 handle,
                                  hos_process_t *POUND_RESTRICT target,
                                  hos_handle_t *POUND_RESTRICT out_handle)
{
    if ((source == NULL) || (target == NULL))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_process_duplicate_handle_into: a process is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (out_handle == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_process_duplicate_handle_into: out_handle is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!hos_process_is_open(source) || !hos_process_is_open(target))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting hos_process_duplicate_handle_into: a process was never created, or was "
                        "already closed.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    if (source->kernel != target->kernel)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting hos_process_duplicate_handle_into: process %p and process %p belong to "
                        "different kernels (%p and %p).",
                        (const void *)source,
                        (const void *)target,
                        (const void *)source->kernel,
                        (const void *)target->kernel);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    hos_kernel_t *kernel = source->kernel;
    mutex_lock(&kernel->lock);

    uint32_t      object_index = 0U;
    error_t       error        = resolve_handle(source, handle, &object_index);

    if (error == POUND_SUCCESS)
    {
        error = allocate_handle(target, object_index, out_handle);
    }

    mutex_unlock(&kernel->lock);
    return error;
}

error_t
hos_process_duplicate_handle(hos_process_t *POUND_RESTRICT process,
                             hos_handle_t                 handle,
                             hos_handle_t *POUND_RESTRICT out_handle)
{
    return hos_process_duplicate_handle_into(process, handle, process, out_handle);
}

error_t
hos_process_close_handle(hos_process_t *POUND_RESTRICT process, hos_handle_t handle)
{
    if (process == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_process_close_handle: process is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!hos_process_is_open(process))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting hos_process_close_handle: the process was never created, or was already "
                        "closed.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    hos_kernel_t *kernel = process->kernel;
    mutex_lock(&kernel->lock);

    uint32_t      object_index = 0U;
    const error_t error        = resolve_handle(process, handle, &object_index);

    if (error != POUND_SUCCESS)
    {
        mutex_unlock(&kernel->lock);
        return error;
    }

    struct hos_handle_slot *slot = &process->handles[handle_slot_number(handle) - 1U];

    release_object(kernel, object_index);

    slot->in_use       = false;
    slot->object_index = 0U;
    slot->generation   = (slot->generation + 1U) & HOS_HANDLE_GENERATION_MASK;

    if (process->handle_count > 0U)
    {
        process->handle_count--;
    }

    mutex_unlock(&kernel->lock);
    return POUND_SUCCESS;
}

error_t
hos_process_handle_type(const hos_process_t *POUND_RESTRICT process,
                        hos_handle_t                       handle,
                        hos_object_type_t *POUND_RESTRICT   out_type)
{
    if (process == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_process_handle_type: process is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (out_type == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_process_handle_type: out_type is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!hos_process_is_open(process))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting hos_process_handle_type: the process was never created, or was already "
                        "closed.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    hos_process_t *mutable_process = (hos_process_t *)(uintptr_t)process;
    hos_kernel_t  *kernel          = mutable_process->kernel;

    mutex_lock(&kernel->lock);

    uint32_t      object_index = 0U;
    const error_t error        = resolve_handle(mutable_process, handle, &object_index);

    if (error == POUND_SUCCESS)
    {
        *out_type = kernel->objects[object_index].type;
    }

    mutex_unlock(&kernel->lock);
    return error;
}

// -----------------------------------------------------------------------------
// Events
// -----------------------------------------------------------------------------

error_t
hos_process_event_signal(hos_process_t *POUND_RESTRICT process, hos_handle_t handle)
{
    if (process == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_process_event_signal: process is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!hos_process_is_open(process))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting hos_process_event_signal: the process was never created, or was already "
                        "closed.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    hos_kernel_t *kernel = process->kernel;
    mutex_lock(&kernel->lock);

    uint32_t      object_index = 0U;
    const error_t error        = resolve_handle_as(process, handle, HOS_OBJECT_EVENT, &object_index);

    if (error == POUND_SUCCESS)
    {
        kernel->objects[object_index].event_signaled = true;
    }

    mutex_unlock(&kernel->lock);
    return error;
}

error_t
hos_process_event_clear(hos_process_t *POUND_RESTRICT process, hos_handle_t handle)
{
    if (process == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_process_event_clear: process is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!hos_process_is_open(process))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting hos_process_event_clear: the process was never created, or was already "
                        "closed.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    hos_kernel_t *kernel = process->kernel;
    mutex_lock(&kernel->lock);

    uint32_t      object_index = 0U;
    const error_t error        = resolve_handle_as(process, handle, HOS_OBJECT_EVENT, &object_index);

    if (error == POUND_SUCCESS)
    {
        kernel->objects[object_index].event_signaled = false;
    }

    mutex_unlock(&kernel->lock);
    return error;
}

error_t
hos_process_event_is_signaled(const hos_process_t *POUND_RESTRICT process,
                              hos_handle_t                        handle,
                              bool *POUND_RESTRICT                out_signaled)
{
    if (process == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_process_event_is_signaled: process is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (out_signaled == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_process_event_is_signaled: out_signaled is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!hos_process_is_open(process))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting hos_process_event_is_signaled: the process was never created, or was "
                        "already closed.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    hos_process_t *mutable_process = (hos_process_t *)(uintptr_t)process;
    hos_kernel_t  *kernel          = mutable_process->kernel;

    mutex_lock(&kernel->lock);

    uint32_t      object_index = 0U;
    const error_t error = resolve_handle_as(mutable_process, handle, HOS_OBJECT_EVENT, &object_index);

    if (error == POUND_SUCCESS)
    {
        *out_signaled = kernel->objects[object_index].event_signaled;
    }

    mutex_unlock(&kernel->lock);
    return error;
}

error_t
hos_process_event_wait(hos_process_t *POUND_RESTRICT process,
                       hos_handle_t                 handle,
                       bool *POUND_RESTRICT         out_signaled)
{
    if (process == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_process_event_wait: process is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (out_signaled == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_process_event_wait: out_signaled is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!hos_process_is_open(process))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting hos_process_event_wait: the process was never created, or was already "
                        "closed.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    hos_kernel_t *kernel = process->kernel;
    mutex_lock(&kernel->lock);

    uint32_t      object_index = 0U;
    const error_t error        = resolve_handle_as(process, handle, HOS_OBJECT_EVENT, &object_index);

    if (error == POUND_SUCCESS)
    {
        struct hos_object *event = &kernel->objects[object_index];

        *out_signaled = event->event_signaled;

        if (event->event_signaled && (event->event_mode == HOS_EVENT_AUTO))
        {
            // An auto-reset event is a signal handed to exactly one waiter, so observing
            // it consumes it. A manual-reset event is a state that stays until cleared.
            event->event_signaled = false;
        }
    }

    mutex_unlock(&kernel->lock);
    return error;
}

// -----------------------------------------------------------------------------
// Waits
// -----------------------------------------------------------------------------

error_t
hos_process_wait_any(const hos_process_t *POUND_RESTRICT process,
                     const hos_handle_t *POUND_RESTRICT handles,
                     uint32_t                          count,
                     uint32_t *POUND_RESTRICT          out_index,
                     bool *POUND_RESTRICT              out_ready)
{
    if (process == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_process_wait_any: process is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if ((handles == NULL) || (out_index == NULL) || (out_ready == NULL))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting hos_process_wait_any: handles, out_index or out_ready is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if ((count == 0U) || (count > HOS_MAX_WAIT_HANDLES))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting hos_process_wait_any: waiting on %u handles; the range is 1 to %u.",
                        (unsigned int)count,
                        (unsigned int)HOS_MAX_WAIT_HANDLES);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!hos_process_is_open(process))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting hos_process_wait_any: the process was never created, or was already "
                        "closed.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    hos_process_t *mutable_process = (hos_process_t *)(uintptr_t)process;
    hos_kernel_t  *kernel          = mutable_process->kernel;

    mutex_lock(&kernel->lock);

    *out_ready = false;

    for (uint32_t position = 0U; position < count; position++)
    {
        uint32_t object_index = 0U;

        // Not `resolve_handle_as`: a session is a legitimate thing to wait on, it simply is
        // never the ready one, so the kind is inspected rather than required.
        const error_t error = resolve_handle(mutable_process, handles[position], &object_index);

        if (error != POUND_SUCCESS)
        {
            mutex_unlock(&kernel->lock);
            return error;
        }

        struct hos_object *object = &kernel->objects[object_index];

        if (object->type == HOS_OBJECT_THREAD)
        {
            // A thread becomes ready by finishing, which is how a join works. There is
            // nothing to consume: it stays ready, so a second waiter on the same thread is
            // woken too rather than being left behind by the first.
            if (object->thread.status != HOS_THREAD_TERMINATED)
            {
                continue;
            }
        }
        else if ((object->type != HOS_OBJECT_EVENT) || !object->event_signaled)
        {
            continue;
        }
        else if (object->event_mode == HOS_EVENT_AUTO)
        {
            // The signal belongs to the waiter that observed it, which is what makes "one
            // signal wakes one waiter" true even though nothing here blocks.
            object->event_signaled = false;
        }

        *out_index = position;
        *out_ready = true;

        mutex_unlock(&kernel->lock);
        return POUND_SUCCESS;
    }

    mutex_unlock(&kernel->lock);
    return POUND_SUCCESS;
}

// -----------------------------------------------------------------------------
// Threads
// -----------------------------------------------------------------------------

error_t
hos_process_create_thread(hos_process_t *POUND_RESTRICT       process,
                          const guest_state_t *POUND_RESTRICT initial_state,
                          uint32_t                           priority,
                          hos_handle_t *POUND_RESTRICT       out_handle)
{
    if (process == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_process_create_thread: process is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if ((initial_state == NULL) || (out_handle == NULL))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting hos_process_create_thread: initial_state or out_handle is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (priority > HOS_THREAD_PRIORITY_LOWEST)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting hos_process_create_thread: priority %u is outside %u to %u.",
                        (unsigned int)priority,
                        (unsigned int)HOS_THREAD_PRIORITY_HIGHEST,
                        (unsigned int)HOS_THREAD_PRIORITY_LOWEST);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!hos_process_is_open(process))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting hos_process_create_thread: the process was never created, or was already "
                        "closed.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    hos_kernel_t *kernel = process->kernel;
    mutex_lock(&kernel->lock);

    if (count_threads_locked(kernel) >= HOS_MAX_THREADS)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing to create a Horizon OS thread: the kernel already holds its maximum of "
                        "%u.",
                        (unsigned int)HOS_MAX_THREADS);
        mutex_unlock(&kernel->lock);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    uint32_t object_index = 0U;
    error_t  error        = allocate_object(kernel, HOS_OBJECT_THREAD, &object_index);

    if (error == POUND_SUCCESS)
    {
        struct hos_thread *thread = &kernel->objects[object_index].thread;

        thread->state    = *initial_state;
        thread->priority = priority;
        thread->status   = HOS_THREAD_CREATED;
        thread->id       = kernel->next_thread_id++;

        // `allocate_object` zeroed the slot, so this is already false; saying it here keeps
        // the new field visible beside the rest of the thread's initial state.
        thread->cancel_requested = false;

        error = allocate_handle(process, object_index, out_handle);
    }

    mutex_unlock(&kernel->lock);
    return error;
}

error_t
hos_process_thread_id(const hos_process_t *POUND_RESTRICT process,
                      hos_handle_t                        handle,
                      uint64_t *POUND_RESTRICT            out_id)
{
    if (process == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_process_thread_id: process is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (out_id == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_process_thread_id: out_id is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!hos_process_is_open(process))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting hos_process_thread_id: the process was never created, or was already "
                        "closed.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    hos_process_t *mutable_process = (hos_process_t *)(uintptr_t)process;
    hos_kernel_t  *kernel          = mutable_process->kernel;

    mutex_lock(&kernel->lock);

    uint32_t      object_index = 0U;
    const error_t error = resolve_handle_as(mutable_process, handle, HOS_OBJECT_THREAD, &object_index);

    if (error == POUND_SUCCESS)
    {
        *out_id = kernel->objects[object_index].thread.id;
    }

    mutex_unlock(&kernel->lock);
    return error;
}

/// Finds the live thread object with `id`.
///
/// Must be called with the kernel lock held. A miss does not log: the scheduler asks about
/// threads it is watching many times a second, and "that one has finished" is the answer it
/// is built to receive, not a fault to report.
static error_t
find_thread_locked(const hos_kernel_t *POUND_RESTRICT kernel, uint64_t id, uint32_t *POUND_RESTRICT out_object_index)
{
    for (uint32_t index = 0U; index < kernel->object_capacity; index++)
    {
        const struct hos_object *object = &kernel->objects[index];

        if ((object->references > 0U) && (object->type == HOS_OBJECT_THREAD) && (object->thread.id == id))
        {
            *out_object_index = index;
            return POUND_SUCCESS;
        }
    }

    return POUND_ERROR_NOT_FOUND;
}

error_t
hos_kernel_thread_status(const hos_kernel_t *POUND_RESTRICT     kernel,
                         uint64_t                              id,
                         hos_thread_status_t *POUND_RESTRICT   out_status)
{
    const error_t kernel_error = require_kernel(kernel, "hos_kernel_thread_status");

    if (kernel_error != POUND_SUCCESS)
    {
        return kernel_error;
    }

    if (out_status == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_kernel_thread_status: out_status is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    hos_kernel_t *mutable_kernel = (hos_kernel_t *)(uintptr_t)kernel;

    mutex_lock(&mutable_kernel->lock);

    uint32_t      object_index = 0U;
    const error_t error        = find_thread_locked(mutable_kernel, id, &object_index);

    if (error == POUND_SUCCESS)
    {
        *out_status = mutable_kernel->objects[object_index].thread.status;
    }

    mutex_unlock(&mutable_kernel->lock);
    return error;
}

error_t
hos_kernel_thread_start(hos_kernel_t *POUND_RESTRICT kernel, uint64_t id)
{
    const error_t kernel_error = require_kernel(kernel, "hos_kernel_thread_start");

    if (kernel_error != POUND_SUCCESS)
    {
        return kernel_error;
    }

    mutex_lock(&kernel->lock);

    uint32_t      object_index = 0U;
    const error_t error        = find_thread_locked(kernel, id, &object_index);

    if (error != POUND_SUCCESS)
    {
        mutex_unlock(&kernel->lock);
        return error;
    }

    struct hos_object *object = &kernel->objects[object_index];

    if (object->thread.status != HOS_THREAD_CREATED)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refused to start thread %llu: it is already %s.",
                        (unsigned long long)id,
                        (object->thread.status == HOS_THREAD_STARTED) ? "started" : "terminated");
        mutex_unlock(&kernel->lock);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    object->thread.status = HOS_THREAD_STARTED;

    // The thread's own reference. Taken directly rather than through `allocate_handle`,
    // because there is no new handle: this one keeps the object -- and the register file
    // inside it -- alive while the thread is able to run, so the guest closing its last
    // handle to a live thread does not free the state under it.
    object->references++;

    mutex_unlock(&kernel->lock);
    return POUND_SUCCESS;
}

error_t
hos_kernel_thread_terminate(hos_kernel_t *POUND_RESTRICT kernel, uint64_t id)
{
    const error_t kernel_error = require_kernel(kernel, "hos_kernel_thread_terminate");

    if (kernel_error != POUND_SUCCESS)
    {
        return kernel_error;
    }

    mutex_lock(&kernel->lock);

    uint32_t      object_index = 0U;
    const error_t error        = find_thread_locked(kernel, id, &object_index);

    if (error != POUND_SUCCESS)
    {
        mutex_unlock(&kernel->lock);
        return error;
    }

    struct hos_object *object = &kernel->objects[object_index];

    if (object->thread.status == HOS_THREAD_CREATED)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refused to terminate thread %llu: it was never started.",
                        (unsigned long long)id);
        mutex_unlock(&kernel->lock);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (object->thread.status == HOS_THREAD_STARTED)
    {
        // Write the status before releasing: the release cannot free the slot while a
        // handle names the thread, but the order is the one that stays correct if that
        // ever stops being true.
        object->thread.status = HOS_THREAD_TERMINATED;
        release_object(kernel, object_index);

        POUND_LOG_INFO(&thread_logger,
                       "Horizon OS thread %llu terminated.",
                       (unsigned long long)id);
    }

    mutex_unlock(&kernel->lock);
    return POUND_SUCCESS;
}

error_t
hos_kernel_thread_context(const hos_kernel_t *POUND_RESTRICT kernel,
                          uint64_t                          id,
                          guest_state_t *POUND_RESTRICT     out_state)
{
    const error_t kernel_error = require_kernel(kernel, "hos_kernel_thread_context");

    if (kernel_error != POUND_SUCCESS)
    {
        return kernel_error;
    }

    if (out_state == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_kernel_thread_context: out_state is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    hos_kernel_t *mutable_kernel = (hos_kernel_t *)(uintptr_t)kernel;

    mutex_lock(&mutable_kernel->lock);

    uint32_t      object_index = 0U;
    const error_t error        = find_thread_locked(mutable_kernel, id, &object_index);

    if (error == POUND_SUCCESS)
    {
        *out_state = mutable_kernel->objects[object_index].thread.state;
    }

    mutex_unlock(&mutable_kernel->lock);
    return error;
}

error_t
hos_kernel_thread_set_context(hos_kernel_t *POUND_RESTRICT       kernel,
                              uint64_t                           id,
                              const guest_state_t *POUND_RESTRICT state)
{
    const error_t kernel_error = require_kernel(kernel, "hos_kernel_thread_set_context");

    if (kernel_error != POUND_SUCCESS)
    {
        return kernel_error;
    }

    if (state == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_kernel_thread_set_context: state is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    mutex_lock(&kernel->lock);

    uint32_t      object_index = 0U;
    const error_t error        = find_thread_locked(kernel, id, &object_index);

    if (error == POUND_SUCCESS)
    {
        kernel->objects[object_index].thread.state = *state;
    }

    mutex_unlock(&kernel->lock);
    return error;
}

error_t
hos_kernel_thread_priority(const hos_kernel_t *POUND_RESTRICT kernel,
                           uint64_t                           id,
                           uint32_t *POUND_RESTRICT           out_priority)
{
    const error_t kernel_error = require_kernel(kernel, "hos_kernel_thread_priority");

    if (kernel_error != POUND_SUCCESS)
    {
        return kernel_error;
    }

    if (out_priority == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_kernel_thread_priority: out_priority is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    hos_kernel_t *mutable_kernel = (hos_kernel_t *)(uintptr_t)kernel;

    mutex_lock(&mutable_kernel->lock);

    uint32_t      object_index = 0U;
    const error_t error        = find_thread_locked(mutable_kernel, id, &object_index);

    if (error == POUND_SUCCESS)
    {
        *out_priority = mutable_kernel->objects[object_index].thread.priority;
    }

    mutex_unlock(&mutable_kernel->lock);
    return error;
}

error_t
hos_kernel_thread_set_priority(hos_kernel_t *POUND_RESTRICT kernel, uint64_t id, uint32_t priority)
{
    const error_t kernel_error = require_kernel(kernel, "hos_kernel_thread_set_priority");

    if (kernel_error != POUND_SUCCESS)
    {
        return kernel_error;
    }

    if (priority > HOS_THREAD_PRIORITY_LOWEST)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting hos_kernel_thread_set_priority: priority %u is outside %u to %u.",
                        (unsigned int)priority,
                        (unsigned int)HOS_THREAD_PRIORITY_HIGHEST,
                        (unsigned int)HOS_THREAD_PRIORITY_LOWEST);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    mutex_lock(&kernel->lock);

    uint32_t      object_index = 0U;
    const error_t error        = find_thread_locked(kernel, id, &object_index);

    if (error == POUND_SUCCESS)
    {
        kernel->objects[object_index].thread.priority = priority;
    }

    mutex_unlock(&kernel->lock);
    return error;
}

error_t
hos_kernel_thread_request_cancel(hos_kernel_t *POUND_RESTRICT kernel, uint64_t id)
{
    const error_t kernel_error = require_kernel(kernel, "hos_kernel_thread_request_cancel");

    if (kernel_error != POUND_SUCCESS)
    {
        return kernel_error;
    }

    mutex_lock(&kernel->lock);

    uint32_t      object_index = 0U;
    const error_t error        = find_thread_locked(kernel, id, &object_index);

    if (error == POUND_SUCCESS)
    {
        // A flag, not a count: Horizon keeps one request per thread, so a second request
        // before the thread synchronizes must not queue a second interruption.
        kernel->objects[object_index].thread.cancel_requested = true;
    }

    mutex_unlock(&kernel->lock);
    return error;
}

error_t
hos_kernel_thread_take_cancel(hos_kernel_t *POUND_RESTRICT kernel,
                              uint64_t                     id,
                              bool *POUND_RESTRICT         out_requested)
{
    const error_t kernel_error = require_kernel(kernel, "hos_kernel_thread_take_cancel");

    if (kernel_error != POUND_SUCCESS)
    {
        return kernel_error;
    }

    if (out_requested == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_kernel_thread_take_cancel: out_requested is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    mutex_lock(&kernel->lock);

    uint32_t      object_index = 0U;
    const error_t error        = find_thread_locked(kernel, id, &object_index);

    if (error == POUND_SUCCESS)
    {
        struct hos_thread *thread = &kernel->objects[object_index].thread;

        *out_requested           = thread->cancel_requested;
        thread->cancel_requested = false;
    }

    mutex_unlock(&kernel->lock);
    return error;
}

// -----------------------------------------------------------------------------
// Sessions
// -----------------------------------------------------------------------------

error_t
hos_process_session_request(const hos_process_t *POUND_RESTRICT process,
                            hos_handle_t                        session,
                            uint32_t                            command,
                            const void *POUND_RESTRICT          request,
                            size_t                              request_size,
                            void *POUND_RESTRICT                response,
                            size_t                              response_capacity,
                            size_t *POUND_RESTRICT              out_response_size)
{
    if (process == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_process_session_request: process is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (out_response_size == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_process_session_request: out_response_size is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if ((request == NULL) && (request_size != 0U))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting hos_process_session_request: request is NULL but request_size is %zu.",
                        request_size);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if ((response == NULL) && (response_capacity != 0U))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting hos_process_session_request: response is NULL but response_capacity is "
                        "%zu.",
                        response_capacity);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!hos_process_is_open(process))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting hos_process_session_request: the process was never created, or was "
                        "already closed.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    hos_process_t *mutable_process = (hos_process_t *)(uintptr_t)process;
    hos_kernel_t  *kernel          = mutable_process->kernel;

    mutex_lock(&kernel->lock);

    uint32_t      object_index = 0U;
    const error_t error = resolve_handle_as(mutable_process, session, HOS_OBJECT_SESSION, &object_index);

    if (error != POUND_SUCCESS)
    {
        mutex_unlock(&kernel->lock);
        return error;
    }

    const struct hos_service *service = &kernel->services[kernel->objects[object_index].service_index];

    hos_service_handler_fn handler        = service->handler;
    void *                 handler_context = service->context;

    // The handler is guest-code policy and will very often want to signal an event or
    // send the next request, so it runs with the kernel free. The service entry cannot
    // move or be removed, so what was copied out stays valid with the lock dropped.
    mutex_unlock(&kernel->lock);

    *out_response_size = 0U;
    return handler(handler_context,
                   command,
                   (const uint8_t *)request,
                   request_size,
                   (uint8_t *)response,
                   response_capacity,
                   out_response_size);
}

/*** end of file ***/

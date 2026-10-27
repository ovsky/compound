#include "scheduler.h"
#include "log.h"
#include "memory/memory.h"
#include <string.h>

/// Marks a scheduler that `hos_scheduler_init` has prepared.
#define HOS_SCHEDULER_MAGIC 0x484F5353U // 'H''O''S''S'

/// Nanoseconds in one system tick, and the numerator of the exact ratio.
///
/// The tick is 19.2 MHz, so one second is 19,200,000 ticks and 1,000,000,000 nanoseconds.
/// Both divide by 1,600,000 to give 12 and 625, which is why the conversion below is
/// exact integer arithmetic: there is no rounding error to accumulate, only the one
/// deliberate round-up.
#define HOS_SCHEDULER_TICK_NUMERATOR 12U
#define HOS_SCHEDULER_TICK_DENOMINATOR 625U

static void
wake_thread(hos_scheduler_t *POUND_RESTRICT scheduler, uint32_t index, const hos_wake_t *wake);

static error_t
require_scheduler(const hos_scheduler_t *POUND_RESTRICT scheduler, const char *POUND_RESTRICT operation)
{
    if (scheduler == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting %s: scheduler is NULL.", operation);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (scheduler->magic != HOS_SCHEDULER_MAGIC)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting %s: the scheduler was never initialised, or was already destroyed.",
                        operation);
        return POUND_ERROR_NOT_INITIALIZED;
    }

    return POUND_SUCCESS;
}

// -----------------------------------------------------------------------------
// Tables
// -----------------------------------------------------------------------------

/// Grows the thread table and the two arrays that shadow it, by doubling.
///
/// All three grow together and to the same capacity, which is what makes the rest of this
/// file allocation-free: the run queue can never hold more entries than there are threads,
/// the ordering scratch is never longer than a scheduling decision, so both can be filled
/// without a failure path -- and a scheduler that could fail to wake a thread because the
/// host was out of memory would be a scheduler with a wakeup to lose.
static error_t
grow_tables(hos_scheduler_t *POUND_RESTRICT scheduler)
{
    const uint32_t new_capacity = (scheduler->thread_capacity == 0U) ? HOS_SCHEDULER_INITIAL_THREADS
                                                                    : (scheduler->thread_capacity * 2U);

    if (new_capacity > HOS_SCHEDULER_MAX_THREADS)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refused to grow the Horizon OS scheduler past %u threads.",
                        (unsigned int)HOS_SCHEDULER_MAX_THREADS);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    hos_scheduler_thread_t *threads = (hos_scheduler_thread_t *)memory_subsystem_allocate(
        _Alignof(hos_scheduler_thread_t), (size_t)new_capacity * sizeof(hos_scheduler_thread_t));

    if (threads == NULL)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refused to grow the Horizon OS scheduler thread table to %u entries: allocation "
                        "failed.",
                        (unsigned int)new_capacity);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    uint32_t *ready = (uint32_t *)memory_subsystem_allocate(_Alignof(uint32_t),
                                                            (size_t)new_capacity * sizeof(uint32_t));

    hos_scheduler_order_t *order = (hos_scheduler_order_t *)memory_subsystem_allocate(
        _Alignof(hos_scheduler_order_t), (size_t)new_capacity * sizeof(hos_scheduler_order_t));

    if ((ready == NULL) || (order == NULL))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refused to grow the Horizon OS scheduler to %u threads: allocation failed.",
                        (unsigned int)new_capacity);
        memory_subsystem_free(threads);
        memory_subsystem_free(ready);
        memory_subsystem_free(order);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    memset(threads, 0, (size_t)new_capacity * sizeof(hos_scheduler_thread_t));
    memset(ready, 0, (size_t)new_capacity * sizeof(uint32_t));
    memset(order, 0, (size_t)new_capacity * sizeof(hos_scheduler_order_t));

    if (scheduler->threads != NULL)
    {
        memcpy(threads,
               scheduler->threads,
               (size_t)scheduler->thread_capacity * sizeof(hos_scheduler_thread_t));
        memory_subsystem_free(scheduler->threads);
    }

    if (scheduler->ready != NULL)
    {
        memcpy(ready, scheduler->ready, (size_t)scheduler->ready_capacity * sizeof(uint32_t));
        memory_subsystem_free(scheduler->ready);
    }

    if (scheduler->order != NULL)
    {
        memory_subsystem_free(scheduler->order);
    }

    scheduler->threads         = threads;
    scheduler->thread_capacity = new_capacity;
    scheduler->ready           = ready;
    scheduler->ready_capacity  = new_capacity;
    scheduler->order           = order;
    return POUND_SUCCESS;
}

/// Reserves a free table entry, growing the tables if they are full.
static error_t
allocate_entry(hos_scheduler_t *POUND_RESTRICT scheduler, uint32_t *POUND_RESTRICT out_index)
{
    if (scheduler->thread_count == scheduler->thread_capacity)
    {
        const error_t error = grow_tables(scheduler);

        if (error != POUND_SUCCESS)
        {
            return error;
        }
    }

    for (uint32_t index = 0U; index < scheduler->thread_capacity; index++)
    {
        if (!scheduler->threads[index].in_use)
        {
            memset(&scheduler->threads[index], 0, sizeof(hos_scheduler_thread_t));
            *out_index = index;
            return POUND_SUCCESS;
        }
    }

    // Only reachable through memory this module does not own, as elsewhere.
    POUND_LOG_ERROR(&thread_logger,
                    "Refused to add a thread to the scheduler: %u entries are all in use but only %u are "
                    "counted as live.",
                    (unsigned int)scheduler->thread_capacity,
                    (unsigned int)scheduler->thread_count);
    return POUND_ERROR_CORRUPTED;
}

/// Finds the entry for `thread_id`, or `UINT32_MAX`.
static uint32_t
find_entry(const hos_scheduler_t *POUND_RESTRICT scheduler, uint64_t thread_id)
{
    for (uint32_t index = 0U; index < scheduler->thread_capacity; index++)
    {
        const hos_scheduler_thread_t *entry = &scheduler->threads[index];

        if (entry->in_use && (entry->thread_id == thread_id))
        {
            return index;
        }
    }

    return UINT32_MAX;
}

/// Drops an entry, taking it out of the run queue too.
static void
remove_entry(hos_scheduler_t *POUND_RESTRICT scheduler, uint32_t index)
{
    scheduler->threads[index].in_use = false;

    if (scheduler->thread_count > 0U)
    {
        scheduler->thread_count--;
    }

    if (scheduler->current == (int32_t)index)
    {
        scheduler->current = -1;
    }

    for (uint32_t position = 0U; position < scheduler->ready_count; position++)
    {
        if (scheduler->ready[position] == index)
        {
            scheduler->ready[position] = scheduler->ready[scheduler->ready_count - 1U];
            scheduler->ready_count--;
            return;
        }
    }
}

// -----------------------------------------------------------------------------
// Run queue
// -----------------------------------------------------------------------------

/// Appends an entry to the run queue, ignoring one that is already in it.
///
/// The ignore is defensive rather than a behaviour anything relies on: every caller either
/// adds a thread that was just parked (and so cannot be in the queue) or admits a thread
/// that was just created.
static void
ready_push(hos_scheduler_t *POUND_RESTRICT scheduler, uint32_t index)
{
    if (scheduler->ready_count == scheduler->ready_capacity)
    {
        return;
    }

    for (uint32_t position = 0U; position < scheduler->ready_count; position++)
    {
        if (scheduler->ready[position] == index)
        {
            return;
        }
    }

    scheduler->ready[scheduler->ready_count++] = index;
}

/// Moves an entry to the back of the run queue.
///
/// The run queue's order is the tie-break among equal priorities, so moving to the back is
/// exactly what a yield means: everything at this priority that has been ready longer runs
/// first, and if there is nothing else, this thread is picked again and nothing was lost.
static void
ready_move_to_back(hos_scheduler_t *POUND_RESTRICT scheduler, uint32_t index)
{
    uint32_t position = UINT32_MAX;

    for (uint32_t candidate = 0U; candidate < scheduler->ready_count; candidate++)
    {
        if (scheduler->ready[candidate] == index)
        {
            position = candidate;
            break;
        }
    }

    if (position == UINT32_MAX)
    {
        return;
    }

    for (uint32_t candidate = position; candidate + 1U < scheduler->ready_count; candidate++)
    {
        scheduler->ready[candidate] = scheduler->ready[candidate + 1U];
    }

    scheduler->ready[scheduler->ready_count - 1U] = index;
}

/// The runnable thread that should run next, or `UINT32_MAX` when none should.
///
/// The lowest priority number wins, and the run queue's position breaks ties, which is what
/// makes a priority class a round robin. A thread whose priority cannot be read has gone
/// and is skipped; the next decision reaps it.
static uint32_t
pick_next(hos_scheduler_t *POUND_RESTRICT scheduler)
{
    uint32_t best          = UINT32_MAX;
    uint32_t best_priority = 0U;

    for (uint32_t position = 0U; position < scheduler->ready_count; position++)
    {
        const uint32_t index = scheduler->ready[position];

        uint32_t priority = 0U;

        if (hos_kernel_thread_priority(scheduler->kernel, scheduler->threads[index].thread_id, &priority) !=
            POUND_SUCCESS)
        {
            continue;
        }

        // Strictly less, so an equally urgent thread earlier in the queue keeps the core.
        if ((best == UINT32_MAX) || (priority < best_priority))
        {
            best          = index;
            best_priority = priority;
        }
    }

    return best;
}

// -----------------------------------------------------------------------------
// Waking
// -----------------------------------------------------------------------------

/// Makes a parked thread runnable again and records what woke it.
static void
wake_thread(hos_scheduler_t *POUND_RESTRICT scheduler, uint32_t index, const hos_wake_t *wake)
{
    hos_scheduler_thread_t *entry = &scheduler->threads[index];

    entry->waiting       = false;
    entry->wake          = *wake;
    entry->wake_pending  = true;
    entry->wait_count    = 0U;
    entry->has_deadline  = false;
    entry->waited_on_handles = false;
    entry->deadline      = 0U;

    ready_push(scheduler, index);
}

/// Wakes every parked thread whose deadline has passed.
static void
wake_due_sleepers(hos_scheduler_t *POUND_RESTRICT scheduler)
{
    for (uint32_t index = 0U; index < scheduler->thread_capacity; index++)
    {
        const hos_scheduler_thread_t *entry = &scheduler->threads[index];

        if (!entry->in_use || !entry->waiting || !entry->has_deadline)
        {
            continue;
        }

        if (entry->deadline > scheduler->tick)
        {
            continue;
        }

        hos_wake_t wake;

        // A thread that was waiting on handles has something to report -- that nothing it
        // asked about was ever ready -- and a thread that was only sleeping has not. The
        // distinction survives even if the handles were dropped in the meantime, which is
        // why it is its own flag rather than a count of what is left.
        wake.reason      = entry->waited_on_handles ? HOS_WAKE_TIMED_OUT : HOS_WAKE_SLEPT;
        wake.ready_index = 0U;

        wake_thread(scheduler, index, &wake);
    }
}

/// Resolves the parked handle waits that can be resolved now, in priority order.
///
/// Order is the point: when two threads are parked on the same auto-reset event, the more
/// urgent one must get the signal, and the first to be polled is the one that consumes it.
/// Two threads at the same priority are resolved in the order they were admitted, so that
/// which one wins does not depend on which table slot was free.
static void
resolve_waits(hos_scheduler_t *POUND_RESTRICT scheduler)
{
    uint32_t waiting = 0U;

    for (uint32_t index = 0U; index < scheduler->thread_capacity; index++)
    {
        const hos_scheduler_thread_t *entry = &scheduler->threads[index];

        if (!entry->in_use || !entry->waiting || (entry->wait_count == 0U))
        {
            continue;
        }

        uint32_t priority = 0U;

        if (hos_kernel_thread_priority(scheduler->kernel, entry->thread_id, &priority) != POUND_SUCCESS)
        {
            continue;
        }

        scheduler->order[waiting].index    = index;
        scheduler->order[waiting].priority = priority;
        scheduler->order[waiting].sequence = entry->sequence;
        waiting++;
    }

    // Selection sort: the set is a handful of threads on any real title, and this keeps the
    // whole decision allocation-free.
    for (uint32_t start = 0U; start < waiting; start++)
    {
        uint32_t best = start;

        for (uint32_t candidate = start + 1U; candidate < waiting; candidate++)
        {
            const hos_scheduler_order_t *a = &scheduler->order[candidate];
            const hos_scheduler_order_t *b = &scheduler->order[best];

            if ((a->priority < b->priority) ||
                ((a->priority == b->priority) && (a->sequence < b->sequence)))
            {
                best = candidate;
            }
        }

        if (best != start)
        {
            const hos_scheduler_order_t swap = scheduler->order[start];
            scheduler->order[start]          = scheduler->order[best];
            scheduler->order[best]           = swap;
        }
    }

    for (uint32_t position = 0U; position < waiting; position++)
    {
        hos_scheduler_thread_t *entry = &scheduler->threads[scheduler->order[position].index];

        // The entry may have been woken by an earlier one in this very loop -- a manual
        // event two of them waited on -- and then it is no longer parked.
        if (!entry->waiting || (entry->wait_count == 0U))
        {
            continue;
        }

        uint32_t ready_index = 0U;
        bool     ready       = false;

        const error_t error = hos_process_wait_any(entry->process,
                                                   entry->wait_handles,
                                                   entry->wait_count,
                                                   &ready_index,
                                                   &ready);

        if (error == POUND_ERROR_NOT_INITIALIZED)
        {
            // The process is gone. The thread is dropped on the next decision, which is
            // sooner than anything this loop could do about it.
            continue;
        }

        if (error == POUND_ERROR_INVALID_HANDLE)
        {
            // A handle the guest closed while this thread was parked on it. The wait can
            // never be satisfied now, so it is not polled again -- which is also what keeps
            // the refusal from being logged once per scheduling decision. The thread keeps
            // its deadline if it had one; a wait with neither deadline nor live handle is
            // the deadlock the next decision reports, and that is the honest answer.
            entry->wait_count = 0U;
            continue;
        }

        if (error != POUND_SUCCESS)
        {
            continue;
        }

        if (!ready)
        {
            continue;
        }

        hos_wake_t wake;
        wake.reason      = HOS_WAKE_SIGNALED;
        wake.ready_index = ready_index;

        wake_thread(scheduler, scheduler->order[position].index, &wake);
    }
}

/// The earliest deadline any parked thread is waiting for.
static bool
earliest_deadline(const hos_scheduler_t *POUND_RESTRICT scheduler, uint64_t *POUND_RESTRICT out_deadline)
{
    bool found = false;

    for (uint32_t index = 0U; index < scheduler->thread_capacity; index++)
    {
        const hos_scheduler_thread_t *entry = &scheduler->threads[index];

        if (!entry->in_use || !entry->waiting || !entry->has_deadline)
        {
            continue;
        }

        if (!found || (entry->deadline < *out_deadline))
        {
            *out_deadline = entry->deadline;
            found         = true;
        }
    }

    return found;
}

/// Drops every entry whose thread is no longer something that could run.
static void
reap_threads(hos_scheduler_t *POUND_RESTRICT scheduler)
{
    for (uint32_t index = 0U; index < scheduler->thread_capacity; index++)
    {
        const hos_scheduler_thread_t *entry = &scheduler->threads[index];

        if (!entry->in_use)
        {
            continue;
        }

        hos_thread_status_t status = HOS_THREAD_CREATED;

        // Three ways to be gone, and all of them mean the same thing here: the process
        // closed (so every thread it owned was terminated), the thread finished and was
        // freed, or its handle-less identity no longer resolves. A thread that merely has
        // no *handle* left is none of these: it is still started, so it is still run.
        const bool gone = !hos_process_is_open(entry->process) ||
                          (hos_kernel_thread_status(scheduler->kernel, entry->thread_id, &status) !=
                           POUND_SUCCESS) ||
                          (status != HOS_THREAD_STARTED);

        if (gone)
        {
            remove_entry(scheduler, index);
        }
    }
}

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

error_t
hos_scheduler_init(hos_scheduler_t *POUND_RESTRICT scheduler, hos_kernel_t *POUND_RESTRICT kernel)
{
    if (scheduler == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_scheduler_init: scheduler is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (scheduler->magic == HOS_SCHEDULER_MAGIC)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_scheduler_init: the scheduler is already initialised.");
        return POUND_ERROR_ALREADY_INITIALIZED;
    }

    if (!hos_kernel_is_initialized(kernel))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting hos_scheduler_init: the kernel was never initialised, or was already "
                        "destroyed.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    memset(scheduler, 0, sizeof(hos_scheduler_t));

    scheduler->kernel  = kernel;
    scheduler->current = -1;
    scheduler->quantum = HOS_SCHEDULER_DEFAULT_QUANTUM;

    const error_t error = grow_tables(scheduler);

    if (error != POUND_SUCCESS)
    {
        memset(scheduler, 0, sizeof(hos_scheduler_t));
        return error;
    }

    scheduler->magic = HOS_SCHEDULER_MAGIC;
    return POUND_SUCCESS;
}

void
hos_scheduler_destroy(hos_scheduler_t *POUND_RESTRICT scheduler)
{
    if ((scheduler == NULL) || (scheduler->magic != HOS_SCHEDULER_MAGIC))
    {
        return;
    }

    memory_subsystem_free(scheduler->threads);
    memory_subsystem_free(scheduler->ready);
    memory_subsystem_free(scheduler->order);
    memset(scheduler, 0, sizeof(hos_scheduler_t));
}

bool
hos_scheduler_is_initialized(const hos_scheduler_t *POUND_RESTRICT scheduler)
{
    return (scheduler != NULL) && (scheduler->magic == HOS_SCHEDULER_MAGIC);
}

uint32_t
hos_scheduler_thread_count(const hos_scheduler_t *POUND_RESTRICT scheduler)
{
    if (require_scheduler(scheduler, "hos_scheduler_thread_count") != POUND_SUCCESS)
    {
        return 0U;
    }

    return scheduler->thread_count;
}

uint32_t
hos_scheduler_ready_count(const hos_scheduler_t *POUND_RESTRICT scheduler)
{
    if (require_scheduler(scheduler, "hos_scheduler_ready_count") != POUND_SUCCESS)
    {
        return 0U;
    }

    return scheduler->ready_count;
}

error_t
hos_scheduler_set_quantum(hos_scheduler_t *POUND_RESTRICT scheduler, uint32_t quantum)
{
    const error_t error = require_scheduler(scheduler, "hos_scheduler_set_quantum");

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    if (quantum == 0U)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting hos_scheduler_set_quantum: a quantum of zero would let one thread be "
                        "picked without ever retiring an instruction.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    scheduler->quantum = quantum;
    return POUND_SUCCESS;
}

error_t
hos_scheduler_admit(hos_scheduler_t *POUND_RESTRICT scheduler,
                    hos_process_t *POUND_RESTRICT    process,
                    uint64_t                         thread_id)
{
    const error_t error = require_scheduler(scheduler, "hos_scheduler_admit");

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    if (process == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_scheduler_admit: process is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (thread_id == 0U)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_scheduler_admit: thread_id is zero, which names no thread.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    hos_thread_status_t status = HOS_THREAD_CREATED;

    if (hos_kernel_thread_status(scheduler->kernel, thread_id, &status) != POUND_SUCCESS)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refused to admit thread %llu: no live thread has that identifier.",
                        (unsigned long long)thread_id);
        return POUND_ERROR_NOT_FOUND;
    }

    if (status != HOS_THREAD_STARTED)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refused to admit thread %llu: it has %s.",
                        (unsigned long long)thread_id,
                        (status == HOS_THREAD_CREATED) ? "not been started" : "already terminated");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (find_entry(scheduler, thread_id) != UINT32_MAX)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refused to admit thread %llu: the scheduler is already running it.",
                        (unsigned long long)thread_id);
        return POUND_ERROR_ALREADY_INITIALIZED;
    }

    uint32_t      index      = 0U;
    const error_t table_error = allocate_entry(scheduler, &index);

    if (table_error != POUND_SUCCESS)
    {
        return table_error;
    }

    hos_scheduler_thread_t *entry = &scheduler->threads[index];

    entry->process   = process;
    entry->thread_id = thread_id;
    entry->sequence  = scheduler->next_sequence++;
    entry->in_use    = true;

    scheduler->thread_count++;
    ready_push(scheduler, index);
    return POUND_SUCCESS;
}

// -----------------------------------------------------------------------------
// The current thread
// -----------------------------------------------------------------------------

uint64_t
hos_scheduler_current_thread_id(const hos_scheduler_t *POUND_RESTRICT scheduler)
{
    if (require_scheduler(scheduler, "hos_scheduler_current_thread_id") != POUND_SUCCESS)
    {
        return 0U;
    }

    if (scheduler->current < 0)
    {
        return 0U;
    }

    return scheduler->threads[scheduler->current].thread_id;
}

hos_process_t *
hos_scheduler_current_process(const hos_scheduler_t *POUND_RESTRICT scheduler)
{
    if (require_scheduler(scheduler, "hos_scheduler_current_process") != POUND_SUCCESS)
    {
        return NULL;
    }

    if (scheduler->current < 0)
    {
        return NULL;
    }

    return scheduler->threads[scheduler->current].process;
}

bool
hos_scheduler_take_wake(hos_scheduler_t *POUND_RESTRICT scheduler, hos_wake_t *POUND_RESTRICT out_wake)
{
    if ((require_scheduler(scheduler, "hos_scheduler_take_wake") != POUND_SUCCESS) || (out_wake == NULL))
    {
        return false;
    }

    if (scheduler->current < 0)
    {
        return false;
    }

    hos_scheduler_thread_t *entry = &scheduler->threads[scheduler->current];

    if (!entry->wake_pending)
    {
        return false;
    }

    entry->wake_pending = false;
    *out_wake           = entry->wake;
    return true;
}

// -----------------------------------------------------------------------------
// Park, yield and wait
// -----------------------------------------------------------------------------

/// Parks the current thread. `handles` may be NULL only when `count` is zero.
static error_t
park_current(hos_scheduler_t *POUND_RESTRICT     scheduler,
             const hos_handle_t *POUND_RESTRICT handles,
             uint32_t                           count,
             bool                               has_deadline,
             uint64_t                           deadline)
{
    if (scheduler->current < 0)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refused to park: no thread is running, so there is nothing to park.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    hos_scheduler_thread_t *entry = &scheduler->threads[scheduler->current];

    if (entry->waiting)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refused to park thread %llu: it is already parked.",
                        (unsigned long long)entry->thread_id);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (count > 0U)
    {
        memcpy(entry->wait_handles, handles, (size_t)count * sizeof(hos_handle_t));
    }

    entry->wait_count         = count;
    entry->waited_on_handles  = (count > 0U);
    entry->has_deadline       = has_deadline;
    entry->deadline           = deadline;
    entry->waiting            = true;
    entry->wake_pending       = false;

    const uint32_t index = (uint32_t)scheduler->current;
    scheduler->current   = -1;

    // Out of the run queue while parked, which is what makes the queue the set of threads
    // that could run *now* rather than a list to filter on every decision.
    for (uint32_t position = 0U; position < scheduler->ready_count; position++)
    {
        if (scheduler->ready[position] == index)
        {
            scheduler->ready[position] = scheduler->ready[scheduler->ready_count - 1U];
            scheduler->ready_count--;
            break;
        }
    }

    return POUND_SUCCESS;
}

error_t
hos_scheduler_block_current(hos_scheduler_t *POUND_RESTRICT     scheduler,
                            const hos_handle_t *POUND_RESTRICT handles,
                            uint32_t                           count,
                            bool                               has_deadline,
                            uint64_t                           deadline)
{
    const error_t error = require_scheduler(scheduler, "hos_scheduler_block_current");

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    if ((handles == NULL) || (count == 0U) || (count > HOS_SCHEDULER_MAX_WAIT_HANDLES))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting hos_scheduler_block_current: %u handles, and the range is 1 to %u.",
                        (unsigned int)count,
                        (unsigned int)HOS_SCHEDULER_MAX_WAIT_HANDLES);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    return park_current(scheduler, handles, count, has_deadline, deadline);
}

bool
hos_scheduler_cancel(hos_scheduler_t *POUND_RESTRICT scheduler, uint64_t thread_id)
{
    if (require_scheduler(scheduler, "hos_scheduler_cancel") != POUND_SUCCESS)
    {
        return false;
    }

    const uint32_t index = find_entry(scheduler, thread_id);

    if (index == UINT32_MAX)
    {
        return false;
    }

    const hos_scheduler_thread_t *entry = &scheduler->threads[index];

    // `waited_on_handles` rather than `wait_count`: a wait whose handles were all closed is
    // still a synchronization call Horizon would interrupt, even though there is nothing
    // left to poll. A sleeping thread is not, and neither is one that is already runnable.
    if (!entry->waiting || !entry->waited_on_handles)
    {
        return false;
    }

    hos_wake_t wake;
    wake.reason      = HOS_WAKE_CANCELLED;
    wake.ready_index = 0U;

    wake_thread(scheduler, index, &wake);
    return true;
}

error_t
hos_scheduler_sleep_current(hos_scheduler_t *POUND_RESTRICT scheduler, uint64_t deadline)
{
    const error_t error = require_scheduler(scheduler, "hos_scheduler_sleep_current");

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    return park_current(scheduler, NULL, 0U, true, deadline);
}

void
hos_scheduler_yield(hos_scheduler_t *POUND_RESTRICT scheduler)
{
    if ((require_scheduler(scheduler, "hos_scheduler_yield") != POUND_SUCCESS) || (scheduler->current < 0))
    {
        return;
    }

    ready_move_to_back(scheduler, (uint32_t)scheduler->current);
}

// -----------------------------------------------------------------------------
// The step
// -----------------------------------------------------------------------------

error_t
hos_scheduler_step(hos_scheduler_t *POUND_RESTRICT          scheduler,
                   hos_scheduler_run_fn                   run,
                   void *POUND_RESTRICT                   context,
                   hos_scheduler_outcome_t *POUND_RESTRICT out_outcome)
{
    const error_t error = require_scheduler(scheduler, "hos_scheduler_step");

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    if (run == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_scheduler_step: run is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (out_outcome == NULL)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_scheduler_step: out_outcome is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    for (;;)
    {
        // Every decision starts from what is true now: threads that have gone are dropped,
        // threads whose time is up are woken, and threads whose handles are ready are woken.
        // Nothing has to tell the scheduler any of it happened, which is what removes the
        // whole class of lost wakeups from this design.
        reap_threads(scheduler);
        wake_due_sleepers(scheduler);
        resolve_waits(scheduler);

        const uint32_t index = pick_next(scheduler);

        if (index == UINT32_MAX)
        {
            if (scheduler->thread_count == 0U)
            {
                *out_outcome = HOS_SCHEDULER_FINISHED;
                return POUND_SUCCESS;
            }

            uint64_t deadline = 0U;

            if (!earliest_deadline(scheduler, &deadline))
            {
                *out_outcome = HOS_SCHEDULER_DEADLOCKED;
                return POUND_SUCCESS;
            }

            // Nothing to run, but a timer is pending: wait for it, the way a core with an
            // empty run queue waits for its next interrupt rather than spinning. The
            // deadline is strictly ahead -- everything already due was just woken -- so this
            // cannot loop in place.
            scheduler->tick = deadline;
            continue;
        }

        hos_scheduler_thread_t *entry     = &scheduler->threads[index];
        hos_process_t *         process   = entry->process;
        const uint64_t          thread_id = entry->thread_id;

        guest_state_t state;

        if (hos_kernel_thread_context(scheduler->kernel, thread_id, &state) != POUND_SUCCESS)
        {
            // It finished between being picked and being loaded. Drop it and choose again.
            remove_entry(scheduler, index);
            continue;
        }

        scheduler->current = (int32_t)index;

        uint32_t executed = 0U;

        const error_t run_error = run(context, process, thread_id, &state, scheduler->quantum, &executed);

        // Store the registers back only while there is a register file to store them into.
        // A thread that exited, or one whose process closed, has none: writing one back
        // would either fail or, worse, succeed against a recycled object and hand the next
        // thread a program counter it never earned.
        hos_thread_status_t status = HOS_THREAD_CREATED;

        if ((hos_kernel_thread_status(scheduler->kernel, thread_id, &status) == POUND_SUCCESS) &&
            (status == HOS_THREAD_STARTED))
        {
            (void)hos_kernel_thread_set_context(scheduler->kernel, thread_id, &state);
        }

        // A thread that used its whole quantum has had its turn, so it goes to the back of
        // its priority -- which is what makes the run queue a round robin instead of a
        // "whoever is at the front, forever" list. A thread that stopped early keeps its
        // place, because stopping early is what a syscall looks like: Horizon's quick calls
        // do not cost the caller the rest of its turn, and only an explicit yield does.
        if (executed >= scheduler->quantum)
        {
            ready_move_to_back(scheduler, index);
        }

        // A thread that parked gave up the core; one that is still current is the thread
        // that just ran. Either way the clock moves by the work that was actually done.
        if (entry->waiting)
        {
            scheduler->current = -1;
        }

        scheduler->tick += (uint64_t)executed;

        *out_outcome = HOS_SCHEDULER_RAN;
        return run_error;
    }
}

// -----------------------------------------------------------------------------
// The clock
// -----------------------------------------------------------------------------

uint64_t
hos_scheduler_tick(const hos_scheduler_t *POUND_RESTRICT scheduler)
{
    if (require_scheduler(scheduler, "hos_scheduler_tick") != POUND_SUCCESS)
    {
        return 0U;
    }

    return scheduler->tick;
}

void
hos_scheduler_advance(hos_scheduler_t *POUND_RESTRICT scheduler, uint64_t ticks)
{
    if (require_scheduler(scheduler, "hos_scheduler_advance") != POUND_SUCCESS)
    {
        return;
    }

    if (ticks > (UINT64_MAX - scheduler->tick))
    {
        scheduler->tick = UINT64_MAX;
    }
    else
    {
        scheduler->tick += ticks;
    }

    wake_due_sleepers(scheduler);
}

uint64_t
hos_scheduler_ticks_from_nanoseconds(uint64_t nanoseconds)
{
    // 19.2 MHz is exactly 12 ticks per 625 nanoseconds, so this is exact integer arithmetic
    // with one deliberate round-up and no accumulated error. The guard refuses the only
    // input that could wrap: a duration so long that scaling it would overflow.
    if (nanoseconds > ((UINT64_MAX - (HOS_SCHEDULER_TICK_DENOMINATOR - 1U)) / HOS_SCHEDULER_TICK_NUMERATOR))
    {
        return UINT64_MAX;
    }

    return (nanoseconds * HOS_SCHEDULER_TICK_NUMERATOR + (HOS_SCHEDULER_TICK_DENOMINATOR - 1U)) /
           HOS_SCHEDULER_TICK_DENOMINATOR;
}

//! The Horizon OS scheduler: which thread runs, and what wakes one that is parked.
//!
//! `hos/kernel.h` owns threads as objects -- their register files, priorities and
//! lifecycle -- but never decides which one runs. This module is that decision, and the
//! only thing in Pound that knows the clock.
//!
//! # The model
//!
//! One scheduler is one guest core. Threads are runnable, running or parked; the
//! scheduler picks the runnable one with the lowest priority *number* (Horizon numbers
//! priorities downwards, so zero is the most urgent) and, among equals, the one that
//! became runnable first. It hands that thread to a caller-supplied function for one
//! quantum, takes the result, and repeats. That function is where the interpreter and the
//! syscall layer live, which is what keeps this module from having to know about either.
//!
//! # Why nothing has to call back
//!
//! A wait in most kernels is an edge: a signal arrives, a waiter is found, it is woken
//! then and there. Here it is a *level* and is re-read: before every decision the
//! scheduler looks at each parked thread's handles and at the clock, and a parked thread
//! whose wait can now be satisfied becomes runnable. Nothing signals the scheduler,
//! because nothing has to -- there is no lost wakeup to lose, since the state is checked
//! rather than delivered. It also means the wake order is decided in one place, in
//! priority order, so a signal two threads are waiting on goes to the more urgent one
//! without any of the bookkeeping a wake queue would need.
//!
//! # The clock
//!
//! Time is counted in Horizon's system ticks, 19.2 MHz, and the scheduler owns the
//! counter. `svcGetSystemTick` reads it through `hos_scheduler_tick`, and a sleep or a
//! timeout is a deadline on it. What advances it is the caller: `hos_scheduler_step`
//! advances it by the number of instructions the thread it just ran actually retired,
//! which is a model rather than a measurement -- an interpreter has no wall clock, and a
//! title that spins on `svcGetSystemTick` has to see it move. One retired instruction is
//! one tick, which puts a guest at 19.2 million instructions a second; a faster host runs
//! guest time faster, and nothing in this slice observes the difference.
//!
//! A core with nothing to run does not spin: it waits for its next timer, exactly as
//! hardware does between interrupts, so the tick jumps to the earliest deadline. That is
//! what keeps a sleeping thread from being a livelock.
//!
//! # What this slice does not do
//!
//! - **No interruption inside a quantum.** A thread that will not yield still loses the core
//!   when its quantum is up -- that is what the rotation after each step is for -- but it
//!   cannot be stopped *during* one. Nothing here could: that would take the interpreter or
//!   the JIT checking a deadline per instruction, and a quantum is chosen small enough that
//!   the difference is not observable for now.
//! - **No core affinity.** `svcCreateThread`'s core number is accepted and ignored,
//!   because there is one core to place a thread on.
//! - **No priority inheritance or boosting.** A parked thread's priority is whatever it
//!   was given; Horizon's own rules for raising a thread that holds a lock do not apply
//!   yet, because nothing here takes a lock of the guest's.

#ifndef POUND_HOS_SCHEDULER_H
#define POUND_HOS_SCHEDULER_H

#include "attributes.h"
#include "errors.h"
#include "guest_state.h"
#include "hos/kernel.h"
#include <stdbool.h>
#include <stdint.h>

/// Number of thread entries a fresh scheduler holds before it first grows.
#define HOS_SCHEDULER_INITIAL_THREADS 8U

/// The most threads one scheduler will hold.
///
/// The same cap as the kernel's object table, because no scheduler can be asked to run
/// more threads than the kernel can create.
#define HOS_SCHEDULER_MAX_THREADS HOS_MAX_THREADS

/// The most handles one parked wait may name. The kernel's cap, for the same reason.
#define HOS_SCHEDULER_MAX_WAIT_HANDLES HOS_MAX_WAIT_HANDLES

/// Instructions one thread runs before the scheduler looks around again.
///
/// One quantum is one call into the caller's run function, so this is really a decision
/// about how much scheduling overhead to amortise: a small quantum interleaves threads
/// closely and pays for a register-file copy each time, a large one does the opposite.
/// Ten thousand instructions is a few micro-seconds of guest code on any host Pound runs
/// on, which is close to the coarseness a title can observe.
#define HOS_SCHEDULER_DEFAULT_QUANTUM 10000U

/// Horizon's system tick rate, in Hz. `svcGetSystemTick` counts at this rate.
#define HOS_SYSTEM_TICK_HZ 19200000U

/// Why a parked thread became runnable again.
typedef enum
{
    /// One of the handles it was waiting on was ready. `ready_index` says which.
    HOS_WAKE_SIGNALED = 0,

    /// Its deadline passed while it was waiting on handles, which is a timeout rather
    /// than a result: nothing it asked about was ever ready.
    HOS_WAKE_TIMED_OUT = 1,

    /// Its deadline passed while it was simply sleeping -- `svcSleepThread` with a
    /// duration. There was nothing to report, so only the passing of time woke it.
    HOS_WAKE_SLEPT = 2,

    /// Another thread cancelled this one's synchronization call with
    /// `svcCancelSynchronization`. The call it was parked in answers `0xEC01` and reports
    /// no index, exactly as Horizon's "Interrupted" result does.
    HOS_WAKE_CANCELLED = 3,
} hos_wake_reason_t;

/// What a thread was woken by, and by which of its handles.
///
/// The scheduler reports this and stops there: writing it into `X0`, or through a pointer
/// the guest passed in `X0`, is the syscall layer's business, and the register file is
/// still sitting exactly as the blocking call left it, so that layer has everything it
/// needs to finish the call.
typedef struct
{
    /// Why the thread is runnable again.
    hos_wake_reason_t reason;

    /// Position in the parked wait's handle array of the handle that was ready.
    /// Meaningful only for `HOS_WAKE_SIGNALED`.
    uint32_t ready_index;
} hos_wake_t;

/// One thread, as the scheduler sees it.
///
/// The register file is *not* here. It lives in the kernel object the identifier names, and
/// is copied out to run and back afterwards -- so this table can move when it grows without
/// anything pointing into it, and there is exactly one copy of a thread's registers at any
/// moment.
///
/// The identifier, not the handle, is what this entry runs on. A title may close the handle
/// of a thread that is still running, and the thread keeps running: the kernel holds the
/// object open on the thread's own behalf, and the identifier -- which is never reused --
/// still names it. A scheduler that knew the thread only by handle would lose a live thread
/// the moment its handle was closed.
typedef struct
{
    /// The process the thread belongs to, passed to the run function. Never dereferenced
    /// here: validity is the identifier's business, and a closed process has already
    /// terminated every thread it owned, so this entry is dropped before anything could
    /// hand the pointer to a caller.
    hos_process_t *process;

    /// The thread's kernel-wide identifier, which is what every kernel call here uses.
    uint64_t thread_id;

    /// The order the thread was admitted in.
    ///
    /// Used only to break ties between parked threads at the same priority: the one that
    /// arrived first is resolved first. A table position could not do it, since slots are
    /// recycled and the lowest free slot says nothing about how long a thread has waited.
    uint64_t sequence;

    /// What the thread is parked on. Only meaningful while `waiting`.
    hos_handle_t wait_handles[HOS_SCHEDULER_MAX_WAIT_HANDLES];

    /// How many of `wait_handles` are in use. Zero means nothing is left to watch -- a
    /// plain sleep, or a handle wait that stopped being satisfiable.
    uint32_t wait_count;

    /// Tick at which a parked thread stops waiting and is woken anyway.
    uint64_t deadline;

    /// The wake the thread has not been told about yet. Cleared by
    /// `hos_scheduler_take_wake`, which is how the syscall layer learns it has a call to
    /// finish.
    hos_wake_t wake;

    /// Whether the thread is parked.
    bool waiting;

    /// Whether there is a wake in `wake` that has not been taken.
    bool wake_pending;

    /// Whether this entry names a thread at all. A cleared entry is a spare slot; the
    /// table never shrinks, so that a title creating and joining threads in a loop does
    /// not reallocate on every iteration.
    bool in_use;

    /// Whether the thread had a deadline set at all. A wait with no deadline -- Horizon's
    /// "wait forever" -- can only be woken by a handle becoming ready.
    bool has_deadline;

    /// Whether this park was a wait *on handles*, as opposed to a plain sleep.
    ///
    /// Kept apart from `wait_count` because the two stop agreeing: a handle closed while a
    /// thread waits on it leaves nothing to poll, but the wait is still a wait on handles
    /// and a deadline that passes must still be reported as a timeout rather than as a
    /// sleep that ended.
    bool waited_on_handles;
} hos_scheduler_thread_t;

/// One parked thread, while the waiting set is put in priority order.
///
/// The ordering scratch lives in the scheduler rather than on the stack because its length
/// is the number of threads, which is not a compile-time bound: allocating with the tables
/// means a scheduling decision never allocates, and so can never fail to wake a thread.
typedef struct
{
    /// Position in the thread table.
    uint32_t index;

    /// The thread's priority, read once so the ordering need not ask the kernel again.
    uint32_t priority;

    /// The order the thread was admitted in.
    uint64_t sequence;
} hos_scheduler_order_t;

/// The scheduler: the threads it knows about, the run queue and the clock.
///
/// Caller-owned, like the kernel: `hos_scheduler_init` prepares it and
/// `hos_scheduler_destroy` releases it, and both are safe on a zeroed struct.
///
/// A scheduler belongs to one guest core and is driven by one host thread. It has no lock
/// because that is the concurrency model rather than an omission: the two threads of a
/// guest are not two threads of the host, and stepping a scheduler from two host threads
/// at once is a caller's bug, not something a mutex here would make correct.
typedef struct
{
    /// Marks a scheduler `hos_scheduler_init` has prepared.
    uint32_t magic;

    /// The kernel the threads live in. Borrowed, never freed here, and it must outlive
    /// the scheduler.
    hos_kernel_t *kernel;

    /// The thread table. Grows by doubling.
    hos_scheduler_thread_t *threads;

    /// Entries `threads` has room for.
    uint32_t thread_capacity;

    /// Entries in use, including spare slots left by removed threads.
    uint32_t thread_count;

    /// Index of the thread that is currently running, or -1 when none is: the last step
    /// either found nothing runnable or has not been asked yet.
    int32_t current;

    /// Indices of the runnable threads, oldest first. The run queue proper: the pick is
    /// a scan for the lowest priority number over this list, and the position in the list
    /// breaks ties, which is what makes the scan a round robin among equals.
    uint32_t *ready;

    /// Entries `ready` has room for.
    uint32_t ready_capacity;

    /// How many of `ready` are in use.
    uint32_t ready_count;

    /// Scratch space for ordering the parked threads by priority, sized with the tables.
    hos_scheduler_order_t *order;

    /// The next `hos_scheduler_thread_t::sequence` to hand out.
    uint64_t next_sequence;

    /// The clock, in system ticks.
    uint64_t tick;

    /// Instructions a thread runs before the scheduler looks around again.
    uint32_t quantum;
} hos_scheduler_t;

/// Runs one quantum of one thread and reports what it did.
///
/// Called with the thread's register file loaded and `budget` instructions to spend. The
/// implementation is expected to run the guest -- through the interpreter, or a JIT, or a
/// stub in a test -- and to set `*out_executed` to the number of instructions it actually
/// retired, which is what the clock advances by. It may call back into the scheduler:
/// `hos_scheduler_yield`, `hos_scheduler_block_current` and `hos_scheduler_sleep_current`
/// are all legal from here, and are how a syscall makes a thread give up the core.
///
/// The thread may also finish, or its whole process close, in which case the register file
/// is not stored back -- there is nothing left to store it into.
typedef error_t (*hos_scheduler_run_fn)(void *POUND_RESTRICT           context,
                                        hos_process_t *POUND_RESTRICT   process,
                                        uint64_t                       thread_id,
                                        guest_state_t *POUND_RESTRICT  state,
                                        uint32_t                       budget,
                                        uint32_t *POUND_RESTRICT       out_executed);

/// What one `hos_scheduler_step` did.
typedef enum
{
    /// A thread ran. It may have yielded, parked or finished while it did; the next step
    /// will see which.
    HOS_SCHEDULER_RAN = 0,

    /// Nothing ran and nothing can: every remaining thread is parked with no deadline and
    /// no handle ready to wake it. This is a fact about the guest -- it has deadlocked --
    /// and reporting it is the whole point of checking, because the alternative is an
    /// emulator that spins forever looking busy.
    HOS_SCHEDULER_DEADLOCKED = 1,

    /// Nothing ran because there are no threads left. A process whose last thread exits
    /// reaches this, and so does a scheduler that was never given one.
    HOS_SCHEDULER_FINISHED = 2,
} hos_scheduler_outcome_t;

/// Prepares `scheduler` to run threads from `kernel`.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` for a NULL argument or a `kernel` that was not
/// initialised, `POUND_ERROR_ALREADY_INITIALIZED` when the scheduler is already prepared,
/// and `POUND_ERROR_ALLOCATION_FAILED` when its tables cannot be allocated.
error_t hos_scheduler_init(hos_scheduler_t *POUND_RESTRICT scheduler, hos_kernel_t *POUND_RESTRICT kernel);

/// Releases the scheduler's tables and returns it to a zeroed state.
///
/// Safe on a zeroed struct and safe to call twice. Does not touch any thread: closing the
/// threads is the caller's business, and this layer never owned them.
void hos_scheduler_destroy(hos_scheduler_t *POUND_RESTRICT scheduler);

/// Whether `scheduler` has been prepared and not yet destroyed.
bool hos_scheduler_is_initialized(const hos_scheduler_t *POUND_RESTRICT scheduler);

/// How many threads the scheduler is watching.
uint32_t hos_scheduler_thread_count(const hos_scheduler_t *POUND_RESTRICT scheduler);

/// How many of them are runnable right now, including the one running.
uint32_t hos_scheduler_ready_count(const hos_scheduler_t *POUND_RESTRICT scheduler);

/// Sets how many instructions a thread runs before the scheduler looks around again.
///
/// Zero is refused: a quantum of zero would let the same thread be picked forever without
/// retiring an instruction, which is a livelock rather than a schedule.
error_t hos_scheduler_set_quantum(hos_scheduler_t *POUND_RESTRICT scheduler, uint32_t quantum);

/// Adds a started thread to the run queue.
///
/// `thread_id` must name a `HOS_THREAD_STARTED` thread, which is the state
/// `hos_kernel_thread_start` puts it in: a thread that was never started is not
/// schedulable, and one that has finished has nothing left to run. `process` is the
/// process the thread belongs to and is only handed back to the run function.
///
/// Adding the same thread twice is refused, so a caller that admits threads from more
/// than one place cannot silently run one of them twice.
error_t hos_scheduler_admit(hos_scheduler_t *POUND_RESTRICT scheduler,
                            hos_process_t *POUND_RESTRICT    process,
                            uint64_t                         thread_id);

/// The identifier of the thread that ran most recently, or 0 for none.
///
/// The syscall layer reads this to answer "which thread is asking", which is what
/// `svcExitThread` needs.
uint64_t hos_scheduler_current_thread_id(const hos_scheduler_t *POUND_RESTRICT scheduler);

/// The process that owns the thread which ran most recently, or NULL.
hos_process_t *hos_scheduler_current_process(const hos_scheduler_t *POUND_RESTRICT scheduler);

/// Takes the current thread's pending wake, if it has one.
///
/// Returns `true` and fills `*out_wake` exactly once per wake. A caller that runs a thread
/// asks this first: a thread that was parked inside a syscall still has that syscall's
/// arguments in its registers, and this is how the layer that owns the syscall contract
/// learns it has a call to finish. A thread that is running for the first time, or one
/// that was only preempted, has nothing pending and answers `false`.
bool hos_scheduler_take_wake(hos_scheduler_t *POUND_RESTRICT scheduler, hos_wake_t *POUND_RESTRICT out_wake);

/// Parks the current thread until one of `handles` is ready, or until `deadline`.
///
/// The handles are copied, and are re-read from the kernel every time the scheduler looks
/// at them, so a handle closed while the thread is parked fails the wait rather than
/// naming whatever took its slot. `has_deadline` false is Horizon's "wait forever".
///
/// The caller must have polled the set first and found nothing ready: a wait that could be
/// satisfied immediately has no business parking a thread, and the syscall layer answers
/// such a call without ever getting here.
error_t hos_scheduler_block_current(hos_scheduler_t *POUND_RESTRICT        scheduler,
                                    const hos_handle_t *POUND_RESTRICT    handles,
                                    uint32_t                              count,
                                    bool                                  has_deadline,
                                    uint64_t                              deadline);

/// Cancels `thread_id`'s parked wait on handles, if it has one.
///
/// Returns `true` when a parked wait was ended -- the thread is now runnable with a
/// `HOS_WAKE_CANCELLED` wake, which the run loop finishes with `0xEC01` -- and `false`
/// when the thread is not in such a wait: it is running, runnable, sleeping, not admitted,
/// or not known at all. A plain sleep is deliberately not cancellable, because Horizon's
/// cancellation targets synchronization calls rather than the clock.
///
/// `false` is not an error and not a claim that nothing happened: the caller is expected to
/// record the request on the thread object instead (`hos_kernel_thread_request_cancel`), so
/// that the thread's *next* synchronization call consumes it. Splitting the two is what
/// reproduces Horizon's two cases without guessing which one the caller meant.
///
/// Safe to call with a scheduler that was never initialised: it returns `false`.
bool hos_scheduler_cancel(hos_scheduler_t *POUND_RESTRICT scheduler, uint64_t thread_id);

/// Parks the current thread until `deadline`, with nothing to wait on.
///
/// This is `svcSleepThread` with a duration: the thread is woken by the clock alone, so
/// the wake it gets reports `HOS_WAKE_SLEPT` and carries nothing else.
error_t hos_scheduler_sleep_current(hos_scheduler_t *POUND_RESTRICT scheduler, uint64_t deadline);

/// Moves the current thread to the back of its priority's queue.
///
/// What `svcSleepThread` with a zero or negative duration does: it gives up the core to
/// everything at its own priority that has been waiting longer, and, if nothing else can
/// run, comes straight back.
void hos_scheduler_yield(hos_scheduler_t *POUND_RESTRICT scheduler);

/// Runs one quantum of the most deserving thread, or reports why it could not.
///
/// This is the loop: pick, run, apply, repeat. It advances the clock, wakes sleepers whose
/// deadlines have passed, resolves the parked waits it can, and, when nothing is runnable
/// but something has a deadline, waits for that deadline rather than spinning. It returns
/// when a thread has run, or when there is nothing left that could.
///
/// The thread that ran goes to the back of its priority when it used its whole quantum, so
/// a thread that never yields still shares the core with its equals. A thread that stopped
/// early keeps its place: a syscall is not a yield, and only `hos_scheduler_yield` says it
/// is one.
///
/// `run`'s own errors are returned unchanged, having stored the thread's registers first,
/// so a caller can tell a guest fault from a scheduler one.
error_t hos_scheduler_step(hos_scheduler_t *POUND_RESTRICT          scheduler,
                           hos_scheduler_run_fn                   run,
                           void *POUND_RESTRICT                   context,
                           hos_scheduler_outcome_t *POUND_RESTRICT out_outcome);

/// The clock, in system ticks.
uint64_t hos_scheduler_tick(const hos_scheduler_t *POUND_RESTRICT scheduler);

/// Advances the clock by `ticks`, waking anything that is now due.
///
/// The caller's escape hatch for time that passed outside a step -- a frame boundary, a
/// host clock, a test that wants to jump an hour -- and it does what the clock does in
/// `hos_scheduler_step`: a parked thread whose deadline has passed becomes runnable.
void hos_scheduler_advance(hos_scheduler_t *POUND_RESTRICT scheduler, uint64_t ticks);

/// Converts a duration in nanoseconds to system ticks, rounding up.
///
/// Rounds up rather than down so that a sleep of one nanosecond is a sleep of one tick
/// rather than of none: a title asking for a positive duration must not be told it has
/// already elapsed. Saturates at `UINT64_MAX` rather than wrapping, so an absurd duration
/// is a very long sleep instead of a short one.
uint64_t hos_scheduler_ticks_from_nanoseconds(uint64_t nanoseconds);

#endif // POUND_HOS_SCHEDULER_H

/*** end of file ***/

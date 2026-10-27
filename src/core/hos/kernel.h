//! Horizon OS kernel objects, handles and services.
//!
//! Horizon OS does not expose its kernel as a set of calls that mutate global state.
//! It exposes a *namespace of handles*, and every subsystem a title talks to -- the
//! filesystem, the applet manager, the GPU driver -- is reached by first connecting to
//! a named service and then sending that service numbered requests. A handle is the
//! unit of authority: a title can do exactly what its handles let it do, and closing
//! one is how that authority is given back.
//!
//! This module is that layer and nothing above it. It owns three things:
//!
//! - **Objects** -- kernel-side things a handle can name. This slice implements events
//!   and sessions. Objects live in one kernel-wide table and are reference-counted by
//!   the number of handles that name them, so an object outlives any single process's
//!   handle and dies only when the last one closes.
//! - **Handles** -- one process's names for objects. A handle is an index into that
//!   process's own table, which is why the same object has a different handle in each
//!   process that holds one, and why two processes routinely hand out the same handle
//!   *value* for two different objects.
//! - **Services** -- the named request handlers a session can be opened against: the
//!   kernel-side half of "connect to `fsp-srv`".
//!
//! # Handle values are generation-checked
//!
//! A handle is 32 bits: a 20-bit slot index in the low bits and a 12-bit generation in
//! the high bits. Zero is not a valid handle, so a zeroed `hos_handle_t` is never
//! mistaken for a live one.
//!
//! The generation is the point. Closing a handle frees its slot, and the next opening
//! reuses it -- so without a generation, a handle that was closed and then copied
//! around would silently name whatever object landed in the slot afterwards. That is
//! the worst possible failure: not a crash, but a title reading another title's data
//! through a name it thought it had given back. Bumping the slot's generation on close
//! makes the stale handle fail to resolve, and it fails as a lookup miss rather than as
//! a wrong object.
//!
//! Twelve bits of generation wrap after 4096 close/open cycles of the *same slot*. The
//! wrap is not repairable in 32 bits, and 32 bits is what the guest passes around; what
//! matters is that the window in which a stale handle works again is bounded by slot
//! reuse rather than being the common case.
//!
//! # One lock, taken everywhere, and released before a handler runs
//!
//! The objects, the services and every process's handle table all sit behind a single
//! kernel mutex. Splitting it would give handle operations a lock order -- the target
//! process's table inside the kernel's -- that the object path would then have to take
//! in reverse, which is a deadlock waiting to happen for no gain this layer cares about:
//! nothing here is on the JIT dispatch path, so the lock is not the thing worth
//! optimising.
//!
//! The one place the lock is *not* held is a service handler. A handler is guest-code
//! policy and routinely wants to signal an event or send the next request, so calling it
//! under a non-recursive lock would deadlock the first handler that did. The service
//! entry is copied out under the lock, the lock is dropped, and the handler runs with the
//! kernel free for re-entry.
//!
//! # What this slice does not do
//!
//! - **No scheduling and no blocking waits.** This module creates threads, holds their
//!   register files and answers questions about them, but it never decides which one
//!   runs: `hos_process_event_wait` polls, and `hos_process_wait_any` polls a set of
//!   handles. Parking a thread and waking it is `hos/scheduler.h`, which is one layer up
//!   and the only thing that knows the clock.
//! - **No shared-memory or port objects.** Only the three object kinds above are
//!   implemented. An encoding outside them is refused rather than half-handled.
//! - **No request marshalling.** `hos_process_session_request` carries an opaque byte
//!   span and a command number. The Horizon IPC wire format -- headers, handle
//!   descriptors, the domain protocol -- is the syscall layer's job.
//! - **No service removal.** Services are append-only, which is what makes it safe to
//!   keep calling a handler after the lock is dropped: the entry cannot move or vanish.

#ifndef POUND_HOS_KERNEL_H
#define POUND_HOS_KERNEL_H

#include "attributes.h"
#include "errors.h"
#include "guest_state.h"
#include "sync/mutex.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Longest service name, in characters, excluding the terminating NUL.
///
/// Horizon service names are exactly eight bytes on the wire, space-padded. A name
/// handed to this layer is a C string of one to eight characters and is compared
/// exactly; the padding is a marshalling concern for the IPC layer, not a property of
/// the name.
#define HOS_SERVICE_NAME_MAX 8U

/// The handle that names no object.
///
/// Zero, so that a zeroed `hos_handle_t` is a well-defined miss rather than an
/// accidental reference to the first slot of a table.
#define HOS_HANDLE_INVALID 0U

/// Number of low bits of a handle that carry the slot index.
///
/// The remaining high bits carry the slot's generation. See the module comment.
#define HOS_HANDLE_INDEX_BITS 20U

/// Number of objects a fresh kernel's table holds before it first grows.
#define HOS_KERNEL_INITIAL_OBJECTS 8U

/// Number of services a fresh kernel's table holds before it first grows.
#define HOS_KERNEL_INITIAL_SERVICES 4U

/// Number of handles a fresh process's table holds before it first grows.
#define HOS_PROCESS_INITIAL_HANDLES 8U

/// Priority of the most urgent thread a title may ask for.
///
/// Horizon numbers priorities downwards: zero is the most urgent, and the scheduler
/// always runs the lowest number among the threads that are ready. It is the opposite
/// of the usual "higher is better", which is why every comparison in this module and
/// the scheduler reads as an inversion until the naming catches up to it.
#define HOS_THREAD_PRIORITY_HIGHEST 0U

/// Priority of the least urgent thread, which is also the idle thread's.
#define HOS_THREAD_PRIORITY_LOWEST 0x3FU

/// Priority Horizon gives an application's main thread unless it asks for another, and
/// the default for a thread created without one.
#define HOS_THREAD_PRIORITY_DEFAULT 0x2CU

/// The most threads a kernel's object table will hold before refusing.
///
/// A cap rather than a doubling forever: every thread carries a full register file, so
/// an unbounded thread table is an unbounded allocation driven by guest requests. A
/// title that wants more than this is not one Pound can serve, and saying so beats
/// exhausting host memory.
#define HOS_MAX_THREADS 1024U

/// The most handles one `hos_process_wait_any` call accepts.
///
/// Horizon's own cap on `svcWaitSynchronization`, so a wait the kernel refuses and a wait
/// the guest cannot express are the same size.
#define HOS_MAX_WAIT_HANDLES 0x40U

/// What a handle names.
typedef enum
{
    /// A signal a process can set, clear and wait on.
    HOS_OBJECT_EVENT = 0,

    /// A live connection to a registered service.
    HOS_OBJECT_SESSION = 1,

    /// A thread, with its own register file. See the thread section below.
    HOS_OBJECT_THREAD = 2,
} hos_object_type_t;

/// Which side of an event a handle carries.
///
/// Horizon mints two handles for one event -- a writable one that signals it and a
/// readable one that waits on it -- and refuses the wrong side with `0xE401`. The right
/// lives on the handle rather than on the object, so a duplicate copies it and cannot
/// gain one the source did not have. Other object kinds carry `HOS_HANDLE_RIGHT_ALL`;
/// only events are split today.
typedef enum
{
    /// The handle may observe the event: wait on it and read its state.
    HOS_HANDLE_RIGHT_READ = 0x1,

    /// The handle may signal the event.
    HOS_HANDLE_RIGHT_WRITE = 0x2,

    /// Both sides, which is what a handle the emulator mints for itself carries.
    HOS_HANDLE_RIGHT_ALL = 0x3,
} hos_handle_right_t;

/// What a wait on an event does to it.
typedef enum
{
    /// Stays signalled until something clears it explicitly.
    HOS_EVENT_MANUAL = 0,

    /// Cleared by the first wait that observes it signalled, so one signal wakes one
    /// waiter. This is Horizon's default clear mode.
    HOS_EVENT_AUTO = 1,
} hos_event_mode_t;

/// How far a thread has got in its life.
///
/// This is the Horizon-visible lifecycle -- the part `svcCreateThread`, `svcStartThread`
/// and `svcExitThread` move -- and deliberately says nothing about whether the thread is
/// running right now. That is the scheduler's question, and it changes thousands of times
/// a second; answering it here would make this table the second place that knows, and the
/// two would disagree the first time one of them was wrong.
typedef enum
{
    /// Created and named by a handle, but not yet started. Not schedulable.
    HOS_THREAD_CREATED = 0,

    /// Started. The thread is schedulable, or running, or parked on a wait.
    HOS_THREAD_STARTED = 1,

    /// Finished. The register file and the exit state stay readable for as long as a
    /// handle names the thread, which is what lets a title join a thread it started.
    HOS_THREAD_TERMINATED = 2,
} hos_thread_status_t;

/// A process's name for an object.
///
/// Opaque by type and comparable by value, but not meaningful across processes: a
/// handle returned by one process names nothing in another, and passing one to a
/// different process is not detectable here. That is not a gap to be closed -- it is how
/// a per-process handle namespace works, and Horizon has the same property.
typedef uint32_t hos_handle_t;

/// Handles one command sent over a session.
///
/// `context` is whatever was registered with the service. `request` and `response` are
/// the caller's spans and are never retained. A handler must not write more than
/// `response_capacity` bytes; if the reply does not fit it returns an error instead of
/// truncating, because a truncated reply the caller cannot detect is worse than a
/// refusal. `command` is the guest's request number, passed through unread.
///
/// Called with the kernel lock released, so a handler may call back into this module.
/// See the module comment.
typedef error_t (*hos_service_handler_fn)(void *context,
                                          uint32_t command,
                                          const uint8_t *request,
                                          size_t request_size,
                                          uint8_t *response,
                                          size_t response_capacity,
                                          size_t *out_response_size);

/// The layout of an object, a service entry and a handle slot.
///
/// All three are incomplete here on purpose: their fields are this module's business,
/// and a caller that could see them could make one out of step with the others. The
/// tables below are pointers to them so that a kernel can be declared without them.
struct hos_object;
struct hos_service;
struct hos_handle_slot;

/// The kernel: the objects, the services and the lock over both.
///
/// Caller-owned. `hos_kernel_init` prepares it and `hos_kernel_destroy` releases it;
/// both are safe on a zeroed struct, so declaring one and calling destroy unconditionally
/// is correct.
///
/// A process holds a pointer to its kernel, so the kernel must outlive every process
/// created from it. `hos_kernel_destroy` enforces that rather than trusting it: it
/// refuses while a process is still open.
typedef struct
{
    /// Set by `hos_kernel_init` and cleared by `hos_kernel_destroy`. Anything else means
    /// the struct was never initialised, or was initialised and then torn down.
    uint32_t magic;

    /// Guards every field below and every handle table of every process on this kernel.
    mutex_t lock;

    /// Object slots, `object_capacity` of them. A slot is live when its reference count
    /// is non-zero; `object_count` is how many are.
    struct hos_object *POUND_RESTRICT objects;
    uint32_t                         object_capacity;
    uint32_t                         object_count;

    /// Service entries, `service_count` of them. Append-only, so an entry never moves.
    struct hos_service *POUND_RESTRICT services;
    uint32_t                          service_capacity;
    uint32_t                          service_count;

    /// Number of processes created from this kernel and not yet closed.
    uint32_t live_processes;

    /// The next thread identifier to hand out.
    ///
    /// One counter for the whole kernel rather than one per process, because a thread
    /// identifier is meant to be unique across the machine -- a title logs one when it is
    /// complaining about a thread, and two processes reporting the same id would make
    /// that log useless.
    uint64_t next_thread_id;
} hos_kernel_t;

/// A process's handle table.
///
/// Caller-owned, like the kernel. One process's handles are invisible to another, which
/// is the whole point of the table living here rather than in the kernel.
typedef struct
{
    /// Set by `hos_process_create` and cleared by `hos_process_close`.
    uint32_t magic;

    /// The kernel this process was created from, which owns the objects its handles
    /// name. Must outlive the process.
    hos_kernel_t *POUND_RESTRICT kernel;

    /// Handle slots, `handle_capacity` of them. `handle_count` are in use.
    struct hos_handle_slot *POUND_RESTRICT handles;
    uint32_t                              handle_capacity;
    uint32_t                              handle_count;
} hos_process_t;

// -----------------------------------------------------------------------------
// Kernel lifecycle
// -----------------------------------------------------------------------------

/// Prepares `kernel` to hold objects, services and processes.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` if `kernel` is NULL,
/// `POUND_ERROR_ALREADY_INITIALIZED` if it is already initialised, and
/// `POUND_ERROR_ALLOCATION_FAILED` if the initial tables could not be allocated -- in
/// which case the kernel is left uninitialised, so `hos_kernel_destroy` on it is a
/// no-op rather than a double free.
error_t hos_kernel_init(hos_kernel_t *POUND_RESTRICT kernel);

/// Releases the kernel's tables.
///
/// Safe on a zeroed struct and safe to call twice. Refuses to tear down a kernel that
/// still has open processes, returning `POUND_ERROR_BUSY` and logging which count is
/// outstanding, because those processes hold pointers into the tables this would free.
/// The refusal is the point: an ordering mistake surfaces as one loud error at teardown
/// instead of as a use-after-free much later.
error_t hos_kernel_destroy(hos_kernel_t *POUND_RESTRICT kernel);

/// Whether `kernel` is initialised and has not been destroyed.
bool hos_kernel_is_initialized(const hos_kernel_t *POUND_RESTRICT kernel);

/// Number of live objects, summed over every process.
///
/// For assertions and diagnostics. An object is live while at least one handle names it.
uint32_t hos_kernel_object_count(const hos_kernel_t *POUND_RESTRICT kernel);

/// Number of processes created from this kernel and not yet closed.
uint32_t hos_kernel_live_process_count(const hos_kernel_t *POUND_RESTRICT kernel);

/// Number of live thread objects, summed over every process.
///
/// For assertions and diagnostics, like `hos_kernel_object_count`.
uint32_t hos_kernel_thread_count(const hos_kernel_t *POUND_RESTRICT kernel);

// -----------------------------------------------------------------------------
// Services
// -----------------------------------------------------------------------------

/// Registers `handler` under `name`.
///
/// `name` is a C string of one to `HOS_SERVICE_NAME_MAX` characters.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` if any argument is NULL or the name is empty or
/// too long, `POUND_ERROR_ALREADY_INITIALIZED` if a service of that name is already
/// registered -- names are the namespace a title connects by, so a second registration
/// would silently redirect every existing session -- `POUND_ERROR_NOT_INITIALIZED` if the
/// kernel is not initialised, and `POUND_ERROR_ALLOCATION_FAILED` if the service table
/// could not grow.
///
/// The context is borrowed, not owned: it must outlive the kernel, and this layer never
/// frees it.
error_t hos_kernel_register_service(hos_kernel_t *POUND_RESTRICT kernel,
                                    const char *POUND_RESTRICT name,
                                    hos_service_handler_fn handler,
                                    void *context);

/// Whether a service called `name` is registered.
///
/// A miss is an ordinary answer -- a title probing for a service a firmware version
/// does not have -- so this does not log.
bool hos_kernel_has_service(const hos_kernel_t *POUND_RESTRICT kernel, const char *POUND_RESTRICT name);

/// Number of registered services.
uint32_t hos_kernel_service_count(const hos_kernel_t *POUND_RESTRICT kernel);

/// Copies the name of the service at `index` into `out`, which must have room for
/// `HOS_SERVICE_NAME_MAX + 1` characters.
///
/// Returns false if the kernel is not initialised or `index` is out of range, leaving
/// `out` untouched.
bool hos_kernel_service_name_at(const hos_kernel_t *POUND_RESTRICT kernel,
                                uint32_t                           index,
                                char *POUND_RESTRICT                out);

// -----------------------------------------------------------------------------
// Process lifecycle
// -----------------------------------------------------------------------------

/// Creates a process with an empty handle table, belonging to `kernel`.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` if either argument is NULL,
/// `POUND_ERROR_NOT_INITIALIZED` if the kernel is not initialised,
/// `POUND_ERROR_ALREADY_INITIALIZED` if `process` already holds an open process, and
/// `POUND_ERROR_ALLOCATION_FAILED` if the handle table could not be allocated.
error_t hos_process_create(hos_kernel_t *POUND_RESTRICT kernel, hos_process_t *POUND_RESTRICT process);

/// Closes every handle the process holds and releases its table.
///
/// Safe on a zeroed struct and safe to call twice. Each handle drops one reference to
/// its object, so an object another process still names survives; only the last
/// reference destroys it.
///
/// A process must not be closed while another thread is inside any `hos_process_*` entry
/// point on it, and must not be used after this returns. Both are the caller's contract,
/// as with any other object whose memory is released.
void hos_process_close(hos_process_t *POUND_RESTRICT process);

/// Whether `process` holds an open handle table.
bool hos_process_is_open(const hos_process_t *POUND_RESTRICT process);

/// Number of handles the process currently holds.
uint32_t hos_process_handle_count(const hos_process_t *POUND_RESTRICT process);

// -----------------------------------------------------------------------------
// Handles
// -----------------------------------------------------------------------------

/// Creates an event and returns a handle to it.
///
/// An auto-reset event that is created signalled has already consumed its signal the
/// moment anything waits on it, which is why the initial state is an argument rather
/// than an afterthought.
///
/// This is the emulator's own creation path, so the single handle it returns carries
/// both rights. Horizon's `svcCreateEvent` instead returns two one-sided handles; that
/// pair is `hos_process_create_event_pair`.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` if `process` or `out_handle` is NULL,
/// `POUND_ERROR_NOT_INITIALIZED` if the process is not open,
/// `POUND_ERROR_ALLOCATION_FAILED` if a table could not grow.
error_t hos_process_create_event(hos_process_t *POUND_RESTRICT process,
                                 hos_event_mode_t             mode,
                                 bool                         initially_signaled,
                                 hos_handle_t *POUND_RESTRICT out_handle);

/// Creates one manual-reset event and returns the two handles `svcCreateEvent` does.
///
/// `out_writable` carries `HOS_HANDLE_RIGHT_WRITE` and `out_readable` carries
/// `HOS_HANDLE_RIGHT_READ`; the two name the same object, so closing either leaves the
/// other usable. The event is manual-reset because that is what Horizon creates:
/// libnx's autoclear mode is a userspace `svcResetSignal` after a successful wait, not a
/// kernel clear, so a kernel that consumed the signal in the wait would break it.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` if `process` is NULL, either output is NULL, or
/// the two outputs are the same pointer (one of the handles would be leaked),
/// `POUND_ERROR_NOT_INITIALIZED` if the process is not open, and
/// `POUND_ERROR_ALLOCATION_FAILED` if the object or either handle could not be allocated.
error_t hos_process_create_event_pair(hos_process_t *POUND_RESTRICT process,
                                      bool                         initially_signaled,
                                      hos_handle_t *POUND_RESTRICT out_writable,
                                      hos_handle_t *POUND_RESTRICT out_readable);

/// Opens a session with the service called `name` and returns a handle to it.
///
/// Returns `POUND_ERROR_NOT_FOUND` if no service has that name -- logged by nothing,
/// because a title probing for an absent service is ordinary -- plus the errors
/// `hos_process_create_event` lists for the arguments and the allocation.
error_t hos_process_connect(hos_process_t *POUND_RESTRICT process,
                            const char *POUND_RESTRICT name,
                            hos_handle_t *POUND_RESTRICT out_handle);

/// Copies `handle` into `target`, giving the target its own name for the same object.
///
/// The object's reference count rises by one, so closing `handle` in the source no longer
/// destroys it. The handle's rights are copied with it, so a duplicate of a readable event
/// handle cannot signal the event. Both processes must have been created from the same
/// kernel, or the target would hold a handle into another kernel's object table; that is
/// `POUND_ERROR_INVALID_ARGUMENT`.
///
/// Returns `POUND_ERROR_INVALID_HANDLE` if `handle` names nothing live in `source`,
/// `POUND_ERROR_INVALID_ARGUMENT` for a NULL argument or a kernel mismatch,
/// `POUND_ERROR_NOT_INITIALIZED` if either process is not open, and
/// `POUND_ERROR_ALLOCATION_FAILED` if the target's table could not grow.
error_t hos_process_duplicate_handle_into(hos_process_t *POUND_RESTRICT source,
                                          hos_handle_t                 handle,
                                          hos_process_t *POUND_RESTRICT target,
                                          hos_handle_t *POUND_RESTRICT out_handle);

/// Copies `handle` into a second handle in the same process.
///
/// The two handles are independent: closing either leaves the object alive through the
/// other. Returns what `hos_process_duplicate_handle_into` returns.
error_t hos_process_duplicate_handle(hos_process_t *POUND_RESTRICT process,
                                     hos_handle_t                 handle,
                                     hos_handle_t *POUND_RESTRICT out_handle);

/// Closes `handle`, dropping one reference to the object it names.
///
/// The slot's generation advances, so this handle value -- and any copy of it -- stops
/// naming anything, even if the slot is reopened for a different object. When the last
/// reference goes the object is destroyed.
///
/// Returns `POUND_ERROR_INVALID_HANDLE` if `handle` names nothing live in `process`,
/// `POUND_ERROR_INVALID_ARGUMENT` if `process` is NULL, and
/// `POUND_ERROR_NOT_INITIALIZED` if the process is not open.
error_t hos_process_close_handle(hos_process_t *POUND_RESTRICT process, hos_handle_t handle);

/// Reports what `handle` names.
///
/// Returns `POUND_ERROR_INVALID_HANDLE` if it names nothing live, plus the argument and
/// open-state errors above.
error_t hos_process_handle_type(const hos_process_t *POUND_RESTRICT process,
                                hos_handle_t                       handle,
                                hos_object_type_t *POUND_RESTRICT   out_type);

// -----------------------------------------------------------------------------
// Events
// -----------------------------------------------------------------------------

/// Signals the event through its writable handle.
///
/// Signalling an already-signalled event is not an error and does not queue a second
/// wake: an event is a state, not a counter.
///
/// Returns `POUND_ERROR_INVALID_HANDLE` if the handle names nothing live,
/// `POUND_ERROR_INVALID_ARGUMENT` if it names something other than an event or names the
/// event's readable side (Horizon's `Handle<WritableEvent>`), plus the argument and
/// open-state errors.
error_t hos_process_event_signal(hos_process_t *POUND_RESTRICT process, hos_handle_t handle);

/// Clears the event through either side of it, whatever its clear mode.
///
/// Returns `POUND_ERROR_INVALID_STATE` if the event was not signalled -- Horizon's
/// `0xFA01`, which is how a guest tells "I consumed it" from "someone else did" -- and
/// `POUND_ERROR_INVALID_ARGUMENT` for a handle that names another kind of object.
error_t hos_process_event_clear(hos_process_t *POUND_RESTRICT process, hos_handle_t handle);

/// Clears the event through its readable handle, which is what `svcResetSignal` does.
///
/// Clears an event that is signalled and reports `POUND_ERROR_INVALID_STATE` otherwise,
/// so the call distinguishes having consumed a signal from finding it already gone. A
/// writable handle is refused with `POUND_ERROR_INVALID_ARGUMENT`, because Horizon's
/// reset takes a `ReadableEvent`.
error_t hos_process_event_reset(hos_process_t *POUND_RESTRICT process, hos_handle_t handle);

/// Reads the event's state without changing it, through either side of it.
error_t hos_process_event_is_signaled(const hos_process_t *POUND_RESTRICT  process,
                                      hos_handle_t                        handle,
                                      bool *POUND_RESTRICT                 out_signaled);

/// Waits for the event and does not block.
///
/// Requires the readable side: a writable handle has no wait to satisfy and is refused as
/// an argument error, which the syscall layer reports as Horizon's `0xE401`.
///
/// Reports whether the event was signalled, and clears an auto-reset event that was, so
/// that the signal is consumed exactly once. A manual-reset event is left signalled.
/// A wait on an unsignalled event returns `POUND_SUCCESS` with `*out_signaled` false:
/// "not yet" is a result, not a failure, and a blocking wait is the scheduler's job.
///
/// Returns the same errors as `hos_process_event_is_signaled`, plus
/// `POUND_ERROR_INVALID_ARGUMENT` for a writable handle.
error_t hos_process_event_wait(hos_process_t *POUND_RESTRICT process,
                               hos_handle_t                 handle,
                               bool *POUND_RESTRICT         out_signaled);

// -----------------------------------------------------------------------------
// Waits
// -----------------------------------------------------------------------------

/// Polls a set of handles and reports the first one that is ready.
///
/// This is one wait, not a loop: it resolves every handle, and the first that is ready
/// wins. An auto-reset event that is ready is consumed here, so the signal belongs to the
/// caller and cannot be seen by a second waiter -- which is what makes "one signal wakes
/// one waiter" true even though nothing blocks.
///
/// A session is a legitimate thing to wait on and is reported as never ready, because
/// nothing queues a message on one in this slice. That is deliberately not an error: a
/// title waiting on a session and an event together is a normal shape for a main loop.
///
/// A thread is ready once it has terminated, which is how a title joins one: waiting on a
/// thread handle and being woken when it exits is a level rather than an edge, so it needs
/// no signal and nothing to consume. A writable event handle is not waitable and is
/// refused as `POUND_ERROR_INVALID_HANDLE`, which the syscall layer reports as `0xE401`.
///
/// On `POUND_SUCCESS`, `*out_ready` says whether anything was ready and, when it was,
/// `*out_index` is the position in `handles` of the one that was. On a timeout the index
/// is left alone: there is no index to report, and a stale one would point the guest at a
/// handle it never asked about.
///
/// Returns `POUND_ERROR_INVALID_HANDLE` when a handle in the array names nothing -- the
/// wait can never be satisfied, so it is the call's failure rather than something to skip
/// past -- `POUND_ERROR_INVALID_ARGUMENT` for a NULL argument or a count of zero or above
/// `HOS_MAX_WAIT_HANDLES`, and `POUND_ERROR_NOT_INITIALIZED` when the process is not open.
error_t hos_process_wait_any(const hos_process_t *POUND_RESTRICT process,
                             const hos_handle_t *POUND_RESTRICT handles,
                             uint32_t                          count,
                             uint32_t *POUND_RESTRICT          out_index,
                             bool *POUND_RESTRICT              out_ready);

// -----------------------------------------------------------------------------
// Threads
// -----------------------------------------------------------------------------
//
// A thread has two names, and the split matters.
//
// A **handle** names a thread inside one process, so it is what a syscall must use: a
// title can only reach the threads its own handle table names, which is the whole point
// of a handle being the unit of authority. `hos_process_create_thread` mints one and
// `hos_process_thread_id` resolves one to the other name.
//
// A **thread identifier** names a thread across the whole kernel, is never reused, and
// outlives the handle. It is what the scheduler uses, and it has to be: a title may close
// the handle of a thread that is still running -- Horizon allows it, and the thread keeps
// running, because the kernel keeps a reference on its own behalf -- and a scheduler
// that knew such a thread only by handle would lose it mid-run. The `hos_kernel_thread_*`
// calls below therefore take identifiers and enforce no per-process authority, because
// they are the emulator's own view of a thread rather than anything a guest can reach.
// No syscall goes through them.

/// Creates a thread and returns a handle to it, in the `HOS_THREAD_CREATED` state.
///
/// `initial_state` is copied, not retained: it is the thread's register file at the moment
/// it is started, so a caller builds one with the entry point in `pc`, the argument in
/// `x[0]` and the stack pointer in `x[31]`, and may then forget it. A thread that is
/// created but never started is still an object: closing its last handle destroys it and
/// it never runs.
///
/// `priority` must be between `HOS_THREAD_PRIORITY_HIGHEST` and
/// `HOS_THREAD_PRIORITY_LOWEST`.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` for a NULL argument, a priority out of range, or
/// a kernel already holding `HOS_MAX_THREADS`, plus the open-state and allocation errors
/// the other handle-creating calls list.
error_t hos_process_create_thread(hos_process_t *POUND_RESTRICT       process,
                                  const guest_state_t *POUND_RESTRICT initial_state,
                                  uint32_t                           priority,
                                  hos_handle_t *POUND_RESTRICT       out_handle);

/// Resolves a thread handle to the thread's kernel-wide identifier.
///
/// Also the way a caller asks "is this handle a thread", since a handle naming anything
/// else is refused. Returns the invalid-handle, wrong-type and open-state errors.
error_t hos_process_thread_id(const hos_process_t *POUND_RESTRICT process,
                              hos_handle_t                        handle,
                              uint64_t *POUND_RESTRICT            out_id);

/// Finds a thread by identifier and reports how far it has got.
///
/// Returns `POUND_ERROR_NOT_FOUND` -- without logging, as everywhere -- when no live
/// thread has that identifier, which is an ordinary answer here: it is how the scheduler
/// learns that a thread it was watching has finished and been freed. The register file and
/// the priority stay readable for as long as the thread lives, including after it has
/// terminated, which is what lets a title inspect a thread it started.
error_t hos_kernel_thread_status(const hos_kernel_t *POUND_RESTRICT kernel,
                                 uint64_t                           id,
                                 hos_thread_status_t *POUND_RESTRICT out_status);

/// Starts a thread, making it schedulable.
///
/// Takes a reference on the thread's object on the thread's own behalf, released by
/// `hos_kernel_thread_terminate` or by the process closing. That is what makes closing the
/// last *handle* to a running thread harmless: the thread keeps itself alive until it
/// finishes, exactly as Horizon's does. Without it, a title that closed a thread's handle
/// and then let it run would have the register file freed under its own feet.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` when the thread has already been started or has
/// already terminated -- a caller that wants Horizon's `Result` for a second start asks
/// `hos_kernel_thread_status` first -- plus the not-found and kernel-state errors.
error_t hos_kernel_thread_start(hos_kernel_t *POUND_RESTRICT kernel, uint64_t id);

/// Marks a started thread as terminated and releases the reference `start` took.
///
/// The register file and priority stay readable while anything still names the thread, so
/// a thread that has finished can still be inspected and its handle still closed.
/// Terminating an already-terminated thread is a success, because two paths can
/// legitimately reach the same finish -- the thread exiting and its process closing -- and
/// the second must not be an error.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` for a thread that was never started, plus the
/// not-found and kernel-state errors.
error_t hos_kernel_thread_terminate(hos_kernel_t *POUND_RESTRICT kernel, uint64_t id);

/// Copies a thread's register file out.
///
/// A copy rather than a pointer into the object table, because that table grows by
/// doubling and moves every slot: a caller holding a pointer across any other call would
/// be holding one into freed memory.
error_t hos_kernel_thread_context(const hos_kernel_t *POUND_RESTRICT kernel,
                                  uint64_t                           id,
                                  guest_state_t *POUND_RESTRICT       out_state);

/// Stores a thread's register file back, after it has run.
///
/// The whole file is written, so a caller must have kept it from
/// `hos_kernel_thread_context` rather than editing a fresh one.
error_t hos_kernel_thread_set_context(hos_kernel_t *POUND_RESTRICT       kernel,
                                      uint64_t                           id,
                                      const guest_state_t *POUND_RESTRICT state);

/// Reports a thread's priority.
error_t hos_kernel_thread_priority(const hos_kernel_t *POUND_RESTRICT kernel,
                                   uint64_t                           id,
                                   uint32_t *POUND_RESTRICT           out_priority);

/// Sets a thread's priority.
///
/// Valid on a thread at any point in its life, including one that has terminated: Horizon
/// keeps the last priority a thread held, and a title that lowers a joining thread's
/// priority before closing it is not doing anything wrong.
error_t hos_kernel_thread_set_priority(hos_kernel_t *POUND_RESTRICT kernel,
                                       uint64_t                     id,
                                       uint32_t                     priority);

/// Requests that `id`'s next synchronization call be interrupted.
///
/// This is the half of `svcCancelSynchronization` that outlives the request: the flag is
/// stored on the thread object, survives until the thread makes a synchronization call or
/// dies, and is consumed exactly once by `hos_kernel_thread_take_cancel`. Requesting it
/// twice before that call is idempotent -- Horizon keeps a flag, not a queue, so two
/// cancellations do not interrupt two waits.
///
/// The other half is `hos_scheduler_cancel`, which ends a wait that is already parked. A
/// caller does that first and raises this flag only when the thread had no parked wait to
/// interrupt, which is how Horizon's "currently in a synchronization call" and "not
/// currently in one" cases are told apart.
///
/// Returns `POUND_ERROR_NOT_FOUND` when no live thread has that identifier, plus the
/// kernel-state errors.
error_t hos_kernel_thread_request_cancel(hos_kernel_t *POUND_RESTRICT kernel, uint64_t id);

/// Takes and clears `id`'s pending cancellation request.
///
/// `*out_requested` is false both when the thread never had one and when a previous call
/// already consumed it: the flag is an edge, and reading it twice must not interrupt two
/// waits. The out-parameter is always written on success.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` for a NULL `out_requested`, the kernel-state
/// errors, and `POUND_ERROR_NOT_FOUND` when no live thread has that identifier.
error_t hos_kernel_thread_take_cancel(hos_kernel_t *POUND_RESTRICT kernel,
                                      uint64_t                     id,
                                      bool *POUND_RESTRICT         out_requested);

// -----------------------------------------------------------------------------
// Sessions
// -----------------------------------------------------------------------------
/// Sends `command` and `request` over a session and collects the reply.
///
/// `response` and `response_capacity` describe the caller's buffer and may be NULL and
/// zero for a request that expects no reply; `out_response_size` reports what the handler
/// wrote and must not be NULL. The handler runs with the kernel lock released, so it may
/// call back into this module -- see the module comment.
///
/// Returns `POUND_ERROR_INVALID_HANDLE` if `session` names nothing live,
/// `POUND_ERROR_INVALID_ARGUMENT` if it names something other than a session or an
/// argument is NULL, and otherwise whatever the handler returned.
error_t hos_process_session_request(const hos_process_t *POUND_RESTRICT process,
                                    hos_handle_t                        session,
                                    uint32_t                            command,
                                    const void *POUND_RESTRICT          request,
                                    size_t                              request_size,
                                    void *POUND_RESTRICT                response,
                                    size_t                              response_capacity,
                                    size_t *POUND_RESTRICT              out_response_size);

#endif // POUND_HOS_KERNEL_H

/*** end of file ***/

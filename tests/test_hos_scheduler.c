//! Tests for the Horizon OS scheduler.
//!
//! # What is under test
//!
//! `hos/hos_scheduler.h` decides which thread runs, when a parked one is woken, and what
//! the clock reads. Everything it needs from the rest of the emulator comes in through one
//! callback, so these cases supply their own: a stub engine that runs real ARM64 code
//! through `interpreter_run` and answers a handful of system calls by calling back into the
//! scheduler exactly as the syscall layer will.
//!
//! That shape is the point of the design, and it is what these cases exercise end to end:
//! real instructions retire in a real guest memory region, threads really park and really
//! wake, and the assertions are about the state the scheduler and the kernel are left in --
//! no thread is simulated with a counter standing in for a program counter.
//!
//! # The stub engine's contract
//!
//! Four system calls, numbered by their own `SVC` immediates, and each reads the same
//! registers the real call does:
//!
//! - `svcWaitSynchronization(out_index, handles, count, timeout_ns)` -- `x[0]` is where the
//!   ready index is written, `x[1]` points at the handle array in guest memory, `w2` is how
//!   many handles there are, `x[3]` is the timeout in nanoseconds. The result goes in `x[0]`
//!   afterwards, which is why the index is written through the pointer rather than into a
//!   register.
//! - `svcSleepThread(nanoseconds)` -- void, so it writes no result at all. A positive
//!   duration parks the thread, anything else yields.
//! - An event signal, so that one thread can wake another. Not a Horizon call: Horizon's own
//!   signalling lives behind a session, which is a layer neither of these modules owns.
//! - `svcExitThread()`, which terminates the calling thread.
//!
//! A parked call is finished the same way the syscall layer finishes one: on the thread's
//! next turn the stub takes the wake the scheduler recorded, writes through the pointer the
//! blocking path deliberately left intact, and steps the program counter past the `SVC`.

#include "pound_test.h"

#include "cpu/interpreter.h"
#include "guest_state.h"
#include "hos/kernel.h"
#include "hos/scheduler.h"
#include "log.h"
#include "memory/guest_memory.h"

#include <stddef.h>
#include <string.h>

// -----------------------------------------------------------------------------
// A guest to run
// -----------------------------------------------------------------------------

#define TEST_GUEST_BASE 0x40000000ULL
#define TEST_GUEST_SIZE (1U << 16U)

/// Where the emitter starts writing programs, front to back.
#define TEST_PROGRAM_BASE 0x40000000ULL

/// Where the cases put data -- handle arrays and result slots -- so that no program can
/// grow into it unnoticed.
#define TEST_DATA_BASE 0x40008000ULL

/// Where a `svcWaitSynchronization` result index is written.
#define TEST_OUT_SLOT(which) (TEST_DATA_BASE + ((which) * 16ULL))

/// Where a handle array for `svcWaitSynchronization` is written.
#define TEST_HANDLES(which) (TEST_DATA_BASE + 0x1000ULL + ((which) * 0x100ULL))

/// The `SVC` immediates the stub engine understands.
#define TEST_SVC_WAIT   0x0001U
#define TEST_SVC_SLEEP  0x0002U
#define TEST_SVC_SIGNAL 0x0003U
#define TEST_SVC_EXIT   0x0004U

/// Horizon's `Result` for a wait that ran out of time, mirrored rather than included: this
/// suite is about the scheduler, and the syscall suite is where the values are checked.
#define TEST_RESULT_SUCCESS 0x0000U
#define TEST_RESULT_TIMEOUT 0xEA01U
#define TEST_RESULT_INTERRUPTED 0xEC01U

POUND_ALIGNED(64) static uint8_t g_memory[TEST_GUEST_SIZE];

static guest_memory_t g_memory_region;

static hos_kernel_t    g_kernel;
static hos_process_t   g_process;
static hos_scheduler_t g_scheduler;

/// Where the next emitted instruction goes.
static uint64_t g_emit_cursor;

/// The threads the stub engine has been asked to run, in order, so that a case can assert on
/// the schedule itself rather than only on where it ended up.
static uint64_t g_run_log[32];
static uint32_t g_run_count;

/// The last wake the stub engine completed a call for.
static hos_wake_t g_last_wake;
static uint64_t   g_last_wake_thread;
static uint32_t   g_wake_count;

/// How many times a stub call parked its thread.
static uint32_t g_park_count;

/// How many times a stub call was asked to do something it does not do, which in a correct
/// case is never.
static uint32_t g_stub_errors;

// -----------------------------------------------------------------------------
// Emitting guest code
// -----------------------------------------------------------------------------

static uint8_t *
host_at(uint64_t guest_address)
{
    return &g_memory[(size_t)(guest_address - TEST_GUEST_BASE)];
}

static void
write_u32(uint64_t guest_address, uint32_t value)
{
    uint8_t *const bytes = host_at(guest_address);
    bytes[0]             = (uint8_t)value;
    bytes[1]             = (uint8_t)(value >> 8U);
    bytes[2]             = (uint8_t)(value >> 16U);
    bytes[3]             = (uint8_t)(value >> 24U);
}

static uint32_t
read_u32(uint64_t guest_address)
{
    const uint8_t *const bytes = host_at(guest_address);
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) | ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static void
emit_word(uint32_t word)
{
    POUND_CHECK(g_emit_cursor + 4U <= TEST_DATA_BASE);
    write_u32(g_emit_cursor, word);
    g_emit_cursor += 4U;
}

/// Starts a program at the current cursor and returns its entry address.
static uint64_t
program_begin(void)
{
    return g_emit_cursor;
}

/// `movz Xd, #imm16`.
static void
emit_movz(uint32_t rd, uint16_t immediate)
{
    emit_word(0xD2800000U | ((uint32_t)immediate << 5U) | rd);
}

/// `movk Xd, #imm16, lsl #shift`.
static void
emit_movk(uint32_t rd, uint16_t immediate, uint32_t shift)
{
    emit_word(0xF2800000U | (((shift / 16U) & 3U) << 21U) | ((uint32_t)immediate << 5U) | rd);
}

/// Loads a whole 64-bit constant the way a compiler does, one halfword at a time.
static void
emit_load_u64(uint32_t rd, uint64_t value)
{
    emit_movz(rd, (uint16_t)(value & 0xFFFFU));
    emit_movk(rd, (uint16_t)((value >> 16U) & 0xFFFFU), 16U);
    emit_movk(rd, (uint16_t)((value >> 32U) & 0xFFFFU), 32U);
    emit_movk(rd, (uint16_t)((value >> 48U) & 0xFFFFU), 48U);
}

/// `svc #imm16`.
static void
emit_svc(uint32_t immediate)
{
    emit_word(0xD4000001U | ((immediate & 0xFFFFU) << 5U));
}

/// `nop`.
static void
emit_nop(void)
{
    emit_word(0xD503201FU);
}

/// `b .` -- branch to itself, so a program that should keep running forever does.
static void
emit_spin(void)
{
    emit_word(0x14000000U);
}

/// Emits a `svcWaitSynchronization` and the `SVC` that parks on it.
///
/// Returns the address of the `SVC`, which is where a parked call's program counter comes to
/// rest and so what a case compares against after a wake.
static uint64_t
emit_wait(uint64_t out_address, uint64_t handles_address, uint32_t count, int64_t timeout_ns)
{
    emit_load_u64(0U, out_address);
    emit_load_u64(1U, handles_address);
    emit_load_u64(2U, (uint64_t)count);
    emit_load_u64(3U, (uint64_t)timeout_ns);

    const uint64_t address = g_emit_cursor;
    emit_svc(TEST_SVC_WAIT);
    return address;
}

/// Emits a `svcSleepThread`. Returns the address of the `SVC`.
static uint64_t
emit_sleep(int64_t nanoseconds)
{
    emit_load_u64(0U, (uint64_t)nanoseconds);

    const uint64_t address = g_emit_cursor;
    emit_svc(TEST_SVC_SLEEP);
    return address;
}

/// Emits the signal the stub engine answers. Returns the address of the `SVC`.
static uint64_t
emit_signal(hos_handle_t event)
{
    emit_load_u64(0U, (uint64_t)event);

    const uint64_t address = g_emit_cursor;
    emit_svc(TEST_SVC_SIGNAL);
    return address;
}

/// Emits `svcExitThread`. Returns the address of the `SVC`.
static uint64_t
emit_exit(void)
{
    const uint64_t address = g_emit_cursor;
    emit_svc(TEST_SVC_EXIT);
    return address;
}

/// Writes a handle array into guest memory and returns its address.
static uint64_t
place_handles(uint32_t which, const hos_handle_t *handles, uint32_t count)
{
    for (uint32_t index = 0U; index < count; index++)
    {
        write_u32(TEST_HANDLES(which) + ((uint64_t)index * 4ULL), handles[index]);
    }

    return TEST_HANDLES(which);
}

// -----------------------------------------------------------------------------
// The stub engine
// -----------------------------------------------------------------------------

/// Answers a stub system call in the thread that trapped on it.
static void
stub_syscall(guest_state_t *POUND_RESTRICT state, uint64_t thread_id, uint32_t immediate)
{
    switch (immediate)
    {
        case TEST_SVC_WAIT:
        {
            const uint64_t out_address     = state->x[0];
            const uint64_t handles_address = state->x[1];
            const int32_t  count           = (int32_t)(uint32_t)state->x[2];
            const int64_t  timeout_ns      = (int64_t)state->x[3];

            if ((count <= 0) || ((uint32_t)count > HOS_MAX_WAIT_HANDLES))
            {
                g_stub_errors++;
                return;
            }

            hos_handle_t handles[HOS_MAX_WAIT_HANDLES];

            for (int32_t index = 0; index < count; index++)
            {
                handles[index] = read_u32(handles_address + ((uint64_t)(uint32_t)index * 4ULL));
            }

            uint32_t ready_index = 0U;
            bool     ready       = false;

            if (hos_process_wait_any(&g_process, handles, (uint32_t)count, &ready_index, &ready) !=
                POUND_SUCCESS)
            {
                g_stub_errors++;
                return;
            }

            if (ready)
            {
                write_u32(out_address, ready_index);
                state->x[0] = TEST_RESULT_SUCCESS;
                state->pc += 4U;
                return;
            }

            if (timeout_ns == 0)
            {
                // A poll, and the whole point of a poll is that it does not park: reporting
                // the timeout now is the honest answer, and doing it without a context
                // switch is what a guest asking with a zero timeout is asking for.
                state->x[0] = TEST_RESULT_TIMEOUT;
                state->pc += 4U;
                return;
            }

            bool     has_deadline = false;
            uint64_t deadline     = 0U;

            if (timeout_ns > 0)
            {
                has_deadline = true;
                deadline     = hos_scheduler_tick(&g_scheduler) +
                           hos_scheduler_ticks_from_nanoseconds((uint64_t)timeout_ns);
            }

            // A negative timeout is Horizon's "wait forever": a deadline of none.
            g_park_count++;

            // The result of the call is left for the completion, and `x[0]` -- the pointer
            // the index is written through -- is deliberately not touched. That is what
            // makes finishing the call later possible at all.
            if (hos_scheduler_block_current(&g_scheduler,
                                           handles,
                                           (uint32_t)count,
                                           has_deadline,
                                           deadline) != POUND_SUCCESS)
            {
                g_stub_errors++;
            }

            return;
        }

        case TEST_SVC_SLEEP:
        {
            const int64_t nanoseconds = (int64_t)state->x[0];

            if (nanoseconds <= 0)
            {
                // Horizon's yield: give up the core to everything at this thread's own
                // priority that has waited longer. Void, so `x[0]` keeps the duration.
                hos_scheduler_yield(&g_scheduler);
                state->pc += 4U;
                return;
            }

            const uint64_t deadline =
                hos_scheduler_tick(&g_scheduler) + hos_scheduler_ticks_from_nanoseconds((uint64_t)nanoseconds);

            g_park_count++;

            if (hos_scheduler_sleep_current(&g_scheduler, deadline) != POUND_SUCCESS)
            {
                g_stub_errors++;
            }

            return;
        }

        case TEST_SVC_SIGNAL:
        {
            const error_t error = hos_process_event_signal(&g_process, (hos_handle_t)state->x[0]);

            state->x[0] = (error == POUND_SUCCESS) ? TEST_RESULT_SUCCESS : (uint64_t)(uint32_t)error;
            state->pc += 4U;
            return;
        }

        case TEST_SVC_EXIT:
            // No program counter step: a thread that has exited has nothing to resume, and
            // leaving `pc` on the call is what makes that visible.
            if (hos_kernel_thread_terminate(&g_kernel, thread_id) != POUND_SUCCESS)
            {
                g_stub_errors++;
            }

            return;

        default:
            g_stub_errors++;
            return;
    }
}

/// Runs one thread for one quantum.
static error_t
stub_run(void *POUND_RESTRICT       context,
         hos_process_t *POUND_RESTRICT process,
         uint64_t                     thread_id,
         guest_state_t *POUND_RESTRICT state,
         uint32_t                     budget,
         uint32_t *POUND_RESTRICT     out_executed)
{
    (void)context;
    (void)process;

    if (g_run_count < (sizeof(g_run_log) / sizeof(g_run_log[0])))
    {
        g_run_log[g_run_count] = thread_id;
    }

    g_run_count++;
    *out_executed = 0U;

    // A thread that was parked inside a stub call still has that call's arguments in its
    // registers, and finishing it is this layer's job -- exactly as it will be the syscall
    // layer's. Nothing else writes the result, so a wake that is not taken here is a syscall
    // that never returns, which is the failure this shape makes obvious.
    hos_wake_t wake;

    if (hos_scheduler_take_wake(&g_scheduler, &wake))
    {
        g_wake_count++;
        g_last_wake        = wake;
        g_last_wake_thread = thread_id;

        switch (wake.reason)
        {
            case HOS_WAKE_SIGNALED:
                // Through the pointer in `x[0]`, which the blocking path left alone.
                write_u32(state->x[0], wake.ready_index);
                state->x[0] = TEST_RESULT_SUCCESS;
                break;

            case HOS_WAKE_TIMED_OUT:
                // A wait on handles that ran out of time: a Result, and so a write to `x[0]`.
                state->x[0] = TEST_RESULT_TIMEOUT;
                break;

            case HOS_WAKE_CANCELLED:
                // Another thread cancelled the wait. The index pointer is not written, for
                // the same reason a timeout does not write it: nothing became ready.
                state->x[0] = TEST_RESULT_INTERRUPTED;
                break;

            case HOS_WAKE_SLEPT:
                // A plain sleep is void. Writing anything here would be inventing a result
                // for a call that has none, which is the mistake the three-way reason exists
                // to prevent.
                break;
        }

        state->pc += 4U;
    }

    interpreter_trap_t trap;
    memset(&trap, 0, sizeof(trap));

    const error_t error = interpreter_run(state, &g_memory_region, budget, out_executed, &trap);

    if ((error == POUND_SUCCESS) && (trap.kind == INTERPRETER_TRAP_SVC))
    {
        stub_syscall(state, thread_id, (uint32_t)trap.imm);
    }

    return error;
}

// -----------------------------------------------------------------------------
// Machine setup
// -----------------------------------------------------------------------------

static void
machine_reset(void)
{
    memset(g_memory, 0, sizeof(g_memory));

    POUND_REQUIRE(POUND_SUCCESS ==
                  guest_memory_init(&g_memory_region, g_memory, sizeof(g_memory), TEST_GUEST_BASE));

    memset(&g_kernel, 0, sizeof(g_kernel));
    memset(&g_process, 0, sizeof(g_process));
    memset(&g_scheduler, 0, sizeof(g_scheduler));

    POUND_REQUIRE(POUND_SUCCESS == hos_kernel_init(&g_kernel));
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create(&g_kernel, &g_process));
    POUND_REQUIRE(POUND_SUCCESS == hos_scheduler_init(&g_scheduler, &g_kernel));

    g_emit_cursor  = TEST_PROGRAM_BASE;
    g_run_count    = 0U;
    g_wake_count   = 0U;
    g_park_count   = 0U;
    g_stub_errors  = 0U;
    g_last_wake_thread = 0U;
    memset(&g_last_wake, 0, sizeof(g_last_wake));
    memset(g_run_log, 0, sizeof(g_run_log));
}

static void
machine_teardown(void)
{
    hos_scheduler_destroy(&g_scheduler);
    hos_process_close(&g_process);
    hos_kernel_destroy(&g_kernel);
}

/// Creates a thread whose program starts at `entry`.
static hos_handle_t
create_thread(uint64_t entry, uint32_t priority)
{
    guest_state_t initial;

    POUND_CHECK_EQ_U64(guest_state_init(&initial), POUND_SUCCESS);
    initial.pc = entry;

    hos_handle_t handle = HOS_HANDLE_INVALID;
    POUND_CHECK_EQ_U64(hos_process_create_thread(&g_process, &initial, priority, &handle),
                       POUND_SUCCESS);
    return handle;
}

/// Creates, starts and admits a thread, which is what `svcCreateThread` and `svcStartThread`
/// do between them, and returns its identifier.
static uint64_t
admit_thread(uint64_t entry, uint32_t priority)
{
    const hos_handle_t handle = create_thread(entry, priority);

    uint64_t id = 0U;
    POUND_CHECK_EQ_U64(hos_process_thread_id(&g_process, handle, &id), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(hos_kernel_thread_start(&g_kernel, id), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(hos_scheduler_admit(&g_scheduler, &g_process, id), POUND_SUCCESS);

    return id;
}

/// Runs one scheduling decision.
static hos_scheduler_outcome_t
step(void)
{
    hos_scheduler_outcome_t outcome = HOS_SCHEDULER_DEADLOCKED;

    POUND_CHECK_EQ_U64(hos_scheduler_step(&g_scheduler, stub_run, NULL, &outcome), POUND_SUCCESS);
    return outcome;
}

/// Reads a thread's program counter.
static uint64_t
thread_pc(uint64_t thread_id)
{
    guest_state_t state;

    if (hos_kernel_thread_context(&g_kernel, thread_id, &state) != POUND_SUCCESS)
    {
        return UINT64_MAX;
    }

    return state.pc;
}

/// Reads a thread's `x[0]`.
static uint64_t
thread_x0(uint64_t thread_id)
{
    guest_state_t state;

    if (hos_kernel_thread_context(&g_kernel, thread_id, &state) != POUND_SUCCESS)
    {
        return UINT64_MAX;
    }

    return state.x[0];
}

// -----------------------------------------------------------------------------
// Which thread runs
// -----------------------------------------------------------------------------

POUND_TEST(hos_scheduler, quantum_expiry_hands_the_core_to_the_next_thread)
{
    machine_reset();
    POUND_REQUIRE(POUND_SUCCESS == hos_scheduler_set_quantum(&g_scheduler, 4U));

    // Two threads that would each run forever, at the same priority. Only the quantum
    // hands the core over: neither of them ever yields.
    const uint64_t first = program_begin();
    emit_nop();
    emit_spin();

    const uint64_t second = program_begin();
    emit_nop();
    emit_spin();

    const uint64_t first_id  = admit_thread(first, HOS_THREAD_PRIORITY_DEFAULT);
    const uint64_t second_id = admit_thread(second, HOS_THREAD_PRIORITY_DEFAULT);

    POUND_CHECK_EQ_U64(hos_scheduler_ready_count(&g_scheduler), 2U);

    for (uint32_t turn = 0U; turn < 4U; turn++)
    {
        POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);
    }

    // Strict alternation, in the order they were admitted.
    POUND_REQUIRE(g_run_count == 4U);
    POUND_CHECK_EQ_U64(g_run_log[0], first_id);
    POUND_CHECK_EQ_U64(g_run_log[1], second_id);
    POUND_CHECK_EQ_U64(g_run_log[2], first_id);
    POUND_CHECK_EQ_U64(g_run_log[3], second_id);

    // Both really ran: each got as far as the instruction after its `nop`.
    POUND_CHECK_EQ_U64(thread_pc(first_id), first + 4U);
    POUND_CHECK_EQ_U64(thread_pc(second_id), second + 4U);

    // And the clock moved by exactly the work that was done.
    POUND_CHECK_EQ_U64(hos_scheduler_tick(&g_scheduler), 16U);
    POUND_CHECK_EQ_U64(g_stub_errors, 0U);

    machine_teardown();
}

POUND_TEST(hos_scheduler, the_most_urgent_thread_keeps_the_core)
{
    machine_reset();
    POUND_REQUIRE(POUND_SUCCESS == hos_scheduler_set_quantum(&g_scheduler, 4U));

    const uint64_t urgent = program_begin();
    emit_nop();
    emit_spin();

    const uint64_t idle = program_begin();
    emit_nop();
    emit_spin();

    const uint64_t urgent_id = admit_thread(urgent, HOS_THREAD_PRIORITY_DEFAULT);
    const uint64_t idle_id   = admit_thread(idle, HOS_THREAD_PRIORITY_LOWEST);

    for (uint32_t turn = 0U; turn < 3U; turn++)
    {
        POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);
    }

    // A priority number three lower than another's is a lower number, which Horizon means as
    // more urgent: the idle thread must not have run at all.
    POUND_REQUIRE(g_run_count == 3U);

    for (uint32_t turn = 0U; turn < 3U; turn++)
    {
        POUND_CHECK_EQ_U64(g_run_log[turn], urgent_id);
    }

    POUND_CHECK_EQ_U64(thread_pc(idle_id), idle);

    machine_teardown();
}

POUND_TEST(hos_scheduler, a_yield_lets_an_equal_priority_peer_in)
{
    machine_reset();
    POUND_REQUIRE(POUND_SUCCESS == hos_scheduler_set_quantum(&g_scheduler, 8U));

    // The first thread yields as its very first action, so it uses one instruction of its
    // quantum and hands the rest over.
    const uint64_t yielder = program_begin();
    emit_svc(TEST_SVC_SLEEP);
    emit_spin();

    const uint64_t worker = program_begin();
    emit_nop();
    emit_spin();

    const uint64_t yielder_id = admit_thread(yielder, HOS_THREAD_PRIORITY_DEFAULT);
    const uint64_t worker_id  = admit_thread(worker, HOS_THREAD_PRIORITY_DEFAULT);

    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);
    POUND_CHECK_EQ_U64(thread_pc(yielder_id), yielder + 4U);

    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);

    POUND_REQUIRE(g_run_count == 2U);
    POUND_CHECK_EQ_U64(g_run_log[0], yielder_id);
    POUND_CHECK_EQ_U64(g_run_log[1], worker_id);

    // The yield did not park anything: the yielder is ready, not waiting.
    POUND_CHECK_EQ_U64(hos_scheduler_ready_count(&g_scheduler), 2U);
    POUND_CHECK_EQ_U64(g_park_count, 0U);

    machine_teardown();
}

POUND_TEST(hos_scheduler, a_higher_priority_thread_is_not_displaced_by_a_yield)
{
    machine_reset();
    POUND_REQUIRE(POUND_SUCCESS == hos_scheduler_set_quantum(&g_scheduler, 8U));

    const uint64_t urgent = program_begin();
    emit_svc(TEST_SVC_SLEEP); // Yields, which it is entitled to do.
    emit_spin();

    const uint64_t idle = program_begin();
    emit_nop();
    emit_spin();

    const uint64_t urgent_id = admit_thread(urgent, HOS_THREAD_PRIORITY_HIGHEST);
    const uint64_t idle_id   = admit_thread(idle, HOS_THREAD_PRIORITY_LOWEST);

    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);
    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);

    // Yielding gives the core to everything at the *same* priority that has waited longer.
    // There is nothing at this thread's priority, so it comes straight back: an urgent
    // thread that yields must not be punished for it.
    POUND_REQUIRE(g_run_count == 2U);
    POUND_CHECK_EQ_U64(g_run_log[0], urgent_id);
    POUND_CHECK_EQ_U64(g_run_log[1], urgent_id);
    POUND_CHECK_EQ_U64(thread_pc(idle_id), idle);

    machine_teardown();
}

// -----------------------------------------------------------------------------
// Parking and waking
// -----------------------------------------------------------------------------

/// The quantum the parking cases use.
///
/// Roomier than the scheduling cases on purpose: a constant load is emitted as four
/// instructions and a parking program sets up four arguments before it calls, so a quantum
/// has to comfortably exceed that prologue or the call would never be reached inside one
/// turn. A case that wants a thread stopped mid-turn sets its own smaller quantum.
#define TEST_PARK_QUANTUM 24U

POUND_TEST(hos_scheduler, a_signalled_event_wakes_the_waiting_thread)
{
    machine_reset();
    POUND_REQUIRE(POUND_SUCCESS == hos_scheduler_set_quantum(&g_scheduler, TEST_PARK_QUANTUM));

    hos_handle_t event = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create_event(&g_process, HOS_EVENT_AUTO, false, &event));

    const uint64_t handles = place_handles(0U, &event, 1U);

    // The waiter parks forever on the event, then spins once it is awake.
    const uint64_t waiter     = program_begin();
    const uint64_t waiter_svc = emit_wait(TEST_OUT_SLOT(0U), handles, 1U, -1);
    emit_spin();

    // The signaller sets the event and then spins.
    const uint64_t signaller     = program_begin();
    const uint64_t signaller_svc = emit_signal(event);
    emit_spin();

    const uint64_t waiter_id    = admit_thread(waiter, HOS_THREAD_PRIORITY_DEFAULT);
    const uint64_t signaller_id = admit_thread(signaller, HOS_THREAD_PRIORITY_DEFAULT);

    // The waiter goes first, finds the event clear and parks. Nothing advanced its program
    // counter past the call: that is what leaves it resumable.
    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);
    POUND_CHECK_EQ_U64(g_park_count, 1U);
    POUND_CHECK_EQ_U64(thread_pc(waiter_id), waiter_svc);
    POUND_CHECK_EQ_U64(hos_scheduler_current_thread_id(&g_scheduler), 0U);

    // The signaller runs and sets the event. Nothing tells the scheduler; it simply looks
    // again before its next decision.
    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);
    POUND_REQUIRE(g_run_count == 2U);
    POUND_CHECK_EQ_U64(g_run_log[1], signaller_id);
    POUND_CHECK_EQ_U64(thread_pc(signaller_id), signaller_svc + 4U);

    POUND_CHECK_EQ_U64(hos_scheduler_ready_count(&g_scheduler), 1U);
    POUND_CHECK_EQ_U64(g_wake_count, 0U);

    // One more decision and the waiter is runnable again, outranking nothing and losing the
    // tie to the signaller, which has been ready longer.
    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);
    POUND_CHECK_EQ_U64(hos_scheduler_ready_count(&g_scheduler), 2U);
    POUND_CHECK_EQ_U64(g_wake_count, 0U);

    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);

    // The waiter's call was finished: the wake was SIGNALED at index 0, the index went
    // through the pointer it left in `x[0]`, the result replaced it, and the program counter
    // stepped past the `SVC`.
    POUND_CHECK_EQ_U64(g_wake_count, 1U);
    POUND_CHECK_EQ_U64(g_last_wake_thread, waiter_id);
    POUND_CHECK_EQ_U64(g_last_wake.reason, HOS_WAKE_SIGNALED);
    POUND_CHECK_EQ_U64(g_last_wake.ready_index, 0U);
    POUND_CHECK_EQ_U64(read_u32(TEST_OUT_SLOT(0U)), 0U);
    POUND_CHECK_EQ_U64(thread_x0(waiter_id), TEST_RESULT_SUCCESS);
    POUND_CHECK_EQ_U64(thread_pc(waiter_id), waiter_svc + 4U);
    POUND_CHECK_EQ_U64(g_stub_errors, 0U);

    machine_teardown();
}

POUND_TEST(hos_scheduler, a_poll_does_not_park_a_thread)
{
    machine_reset();
    POUND_REQUIRE(POUND_SUCCESS == hos_scheduler_set_quantum(&g_scheduler, TEST_PARK_QUANTUM));

    hos_handle_t event = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create_event(&g_process, HOS_EVENT_AUTO, false, &event));

    const uint64_t handles = place_handles(0U, &event, 1U);

    // A zero timeout is Horizon's poll, and the stub answers it without a context switch.
    const uint64_t poller     = program_begin();
    const uint64_t poller_svc = emit_wait(TEST_OUT_SLOT(1U), handles, 1U, 0);
    emit_spin();

    const uint64_t poller_id = admit_thread(poller, HOS_THREAD_PRIORITY_DEFAULT);

    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);

    POUND_CHECK_EQ_U64(g_park_count, 0U);
    POUND_CHECK_EQ_U64(g_wake_count, 0U);
    POUND_CHECK_EQ_U64(hos_scheduler_current_thread_id(&g_scheduler), poller_id);
    POUND_CHECK_EQ_U64(thread_x0(poller_id), TEST_RESULT_TIMEOUT);
    POUND_CHECK_EQ_U64(thread_pc(poller_id), poller_svc + 4U);

    machine_teardown();
}

POUND_TEST(hos_scheduler, cancelling_a_parked_wait_wakes_it_with_an_interruption)
{
    machine_reset();
    POUND_REQUIRE(POUND_SUCCESS == hos_scheduler_set_quantum(&g_scheduler, TEST_PARK_QUANTUM));

    hos_handle_t event = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create_event(&g_process, HOS_EVENT_AUTO, false, &event));

    const uint64_t handles = place_handles(0U, &event, 1U);

    const uint64_t waiter     = program_begin();
    const uint64_t waiter_svc = emit_wait(TEST_OUT_SLOT(3U), handles, 1U, -1);
    emit_spin();

    const uint64_t waiter_id = admit_thread(waiter, HOS_THREAD_PRIORITY_DEFAULT);

    // A wait on an unready handle with no deadline parks the thread, and nothing is runnable
    // until somebody ends it.
    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);
    POUND_CHECK_EQ_U64(g_park_count, 1U);
    POUND_CHECK_EQ_U64(hos_scheduler_ready_count(&g_scheduler), 0U);

    // A cancellation for a thread the scheduler has never seen is not an interruption, and
    // neither is a second one for this thread: the first took it out of the parked state.
    POUND_CHECK(!hos_scheduler_cancel(&g_scheduler, waiter_id + 1000U));
    POUND_CHECK(hos_scheduler_cancel(&g_scheduler, waiter_id));
    POUND_CHECK(!hos_scheduler_cancel(&g_scheduler, waiter_id));
    POUND_CHECK_EQ_U64(hos_scheduler_ready_count(&g_scheduler), 1U);

    // The next decision runs the cancelled thread and finishes its call: the wake is
    // CANCELLED, X0 is Horizon's `0xEC01`, the index slot is untouched, and the program
    // counter finally steps past the `SVC` that was left standing.
    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);
    POUND_CHECK_EQ_U64(g_wake_count, 1U);
    POUND_CHECK_EQ_U64(g_last_wake_thread, waiter_id);
    POUND_CHECK_EQ_U64(g_last_wake.reason, HOS_WAKE_CANCELLED);
    POUND_CHECK_EQ_U64(g_last_wake.ready_index, 0U);
    POUND_CHECK_EQ_U64(read_u32(TEST_OUT_SLOT(3U)), 0U);
    POUND_CHECK_EQ_U64(thread_x0(waiter_id), TEST_RESULT_INTERRUPTED);
    POUND_CHECK_EQ_U64(thread_pc(waiter_id), waiter_svc + 4U);
    POUND_CHECK_EQ_U64(g_stub_errors, 0U);

    machine_teardown();
}

POUND_TEST(hos_scheduler, cancelling_a_sleep_is_not_an_interruption)
{
    machine_reset();
    POUND_REQUIRE(POUND_SUCCESS == hos_scheduler_set_quantum(&g_scheduler, TEST_PARK_QUANTUM));

    const uint64_t sleeper     = program_begin();
    const uint64_t sleeper_svc = emit_sleep(1000000); // One millisecond.
    emit_spin();

    const uint64_t sleeper_id = admit_thread(sleeper, HOS_THREAD_PRIORITY_DEFAULT);

    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);
    POUND_CHECK_EQ_U64(g_park_count, 1U);

    // Horizon cancels synchronization calls, not the clock. A cancellation does not end a
    // sleep, and the sleep still reports what a sleep reports when its deadline passes.
    POUND_CHECK(!hos_scheduler_cancel(&g_scheduler, sleeper_id));
    POUND_CHECK_EQ_U64(hos_scheduler_ready_count(&g_scheduler), 0U);

    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);
    POUND_CHECK_EQ_U64(g_wake_count, 1U);
    POUND_CHECK_EQ_U64(g_last_wake.reason, HOS_WAKE_SLEPT);
    POUND_CHECK_EQ_U64(thread_x0(sleeper_id), 1000000U);
    POUND_CHECK_EQ_U64(thread_pc(sleeper_id), sleeper_svc + 4U);

    machine_teardown();
}

POUND_TEST(hos_scheduler, a_wait_times_out_when_the_clock_passes_its_deadline)
{
    machine_reset();
    POUND_REQUIRE(POUND_SUCCESS == hos_scheduler_set_quantum(&g_scheduler, TEST_PARK_QUANTUM));

    hos_handle_t event = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create_event(&g_process, HOS_EVENT_AUTO, false, &event));

    const uint64_t handles = place_handles(0U, &event, 1U);

    const uint64_t waiter     = program_begin();
    const uint64_t waiter_svc = emit_wait(TEST_OUT_SLOT(2U), handles, 1U, 1000000); // One millisecond.
    emit_spin();

    const uint64_t waiter_id = admit_thread(waiter, HOS_THREAD_PRIORITY_DEFAULT);

    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);
    POUND_CHECK_EQ_U64(g_park_count, 1U);
    POUND_CHECK_EQ_U64(thread_pc(waiter_id), waiter_svc);

    // Nothing else is runnable, so the next decision does not spin: a core with an empty run
    // queue waits for its next timer, and the clock jumps to the deadline rather than retiring
    // 19,200 instructions of nothing. The only work after the jump is the one turn the woken
    // thread takes.
    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);

    const uint64_t deadline = hos_scheduler_ticks_from_nanoseconds(1000000U);

    POUND_CHECK(hos_scheduler_tick(&g_scheduler) >= deadline);
    POUND_CHECK(hos_scheduler_tick(&g_scheduler) <= deadline + TEST_PARK_QUANTUM);

    POUND_CHECK_EQ_U64(g_wake_count, 1U);
    POUND_CHECK_EQ_U64(g_last_wake_thread, waiter_id);
    POUND_CHECK_EQ_U64(g_last_wake.reason, HOS_WAKE_TIMED_OUT);

    // A timeout is a Result, and it replaces the pointer that was in `x[0]`.
    POUND_CHECK_EQ_U64(thread_x0(waiter_id), TEST_RESULT_TIMEOUT);
    POUND_CHECK_EQ_U64(thread_pc(waiter_id), waiter_svc + 4U);

    // The index slot was not written: a timeout has no index to report, and a stale one
    // would point the guest at a handle it never asked about.
    POUND_CHECK_EQ_U64(read_u32(TEST_OUT_SLOT(2U)), 0U);

    machine_teardown();
}

POUND_TEST(hos_scheduler, a_plain_sleep_wakes_without_a_result)
{
    machine_reset();
    POUND_REQUIRE(POUND_SUCCESS == hos_scheduler_set_quantum(&g_scheduler, TEST_PARK_QUANTUM));

    const uint64_t sleeper     = program_begin();
    const uint64_t sleeper_svc = emit_sleep(1000000);
    emit_spin();

    const uint64_t sleeper_id = admit_thread(sleeper, HOS_THREAD_PRIORITY_DEFAULT);

    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);
    POUND_CHECK_EQ_U64(g_park_count, 1U);
    POUND_CHECK_EQ_U64(thread_pc(sleeper_id), sleeper_svc);

    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);
    POUND_CHECK_EQ_U64(g_wake_count, 1U);
    POUND_CHECK_EQ_U64(g_last_wake.reason, HOS_WAKE_SLEPT);

    // `svcSleepThread` is void, so nothing may have written `x[0]`: it still holds the
    // duration the guest asked for.
    POUND_CHECK_EQ_U64(thread_x0(sleeper_id), 1000000U);
    POUND_CHECK_EQ_U64(thread_pc(sleeper_id), sleeper_svc + 4U);

    machine_teardown();
}

POUND_TEST(hos_scheduler, the_higher_priority_waiter_takes_an_auto_reset_signal)
{
    machine_reset();
    POUND_REQUIRE(POUND_SUCCESS == hos_scheduler_set_quantum(&g_scheduler, TEST_PARK_QUANTUM));

    hos_handle_t event = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create_event(&g_process, HOS_EVENT_AUTO, false, &event));

    const uint64_t handles = place_handles(0U, &event, 1U);

    // Two waiters on the same event, each with its own result slot so the case can tell which
    // of them was served.
    const uint64_t low         = program_begin();
    const uint64_t low_svc     = emit_wait(TEST_OUT_SLOT(3U), handles, 1U, -1);
    emit_spin();

    const uint64_t high        = program_begin();
    const uint64_t high_svc    = emit_wait(TEST_OUT_SLOT(4U), handles, 1U, -1);
    emit_spin();

    const uint64_t signaller     = program_begin();
    const uint64_t signaller_svc = emit_signal(event);
    emit_spin();

    // The signaller is the least urgent of the three, so both waiters park before it gets a
    // turn.
    const uint64_t low_id       = admit_thread(low, HOS_THREAD_PRIORITY_LOWEST);
    const uint64_t high_id      = admit_thread(high, HOS_THREAD_PRIORITY_LOWEST - 4U);
    const uint64_t signaller_id = admit_thread(signaller, HOS_THREAD_PRIORITY_LOWEST);

    step(); // The more urgent waiter parks.
    step(); // The less urgent waiter parks.
    POUND_CHECK_EQ_U64(g_park_count, 2U);
    POUND_CHECK_EQ_U64(thread_pc(high_id), high_svc);
    POUND_CHECK_EQ_U64(thread_pc(low_id), low_svc);

    step(); // The signaller sets the event. Nothing tells the scheduler.
    POUND_CHECK_EQ_U64(thread_pc(signaller_id), signaller_svc + 4U);
    POUND_CHECK_EQ_U64(hos_scheduler_thread_count(&g_scheduler), 3U);

    POUND_CHECK_EQ_U64(g_wake_count, 0U);

    // One signal, one waiter: the event is auto-reset, so the first thread the scheduler
    // resolves consumes it. Polling in priority order is what makes "first" mean the more
    // urgent waiter rather than whichever table slot came first.
    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);
    POUND_CHECK_EQ_U64(g_wake_count, 1U);
    POUND_CHECK_EQ_U64(g_last_wake_thread, high_id);
    POUND_CHECK_EQ_U64(g_last_wake.reason, HOS_WAKE_SIGNALED);
    POUND_CHECK_EQ_U64(read_u32(TEST_OUT_SLOT(4U)), 0U);
    POUND_CHECK_EQ_U64(thread_x0(high_id), TEST_RESULT_SUCCESS);
    POUND_CHECK_EQ_U64(thread_pc(high_id), high_svc + 4U);

    // The less urgent waiter is still parked, and its call was not touched.
    POUND_CHECK_EQ_U64(thread_pc(low_id), low_svc);
    POUND_CHECK_EQ_U64(read_u32(TEST_OUT_SLOT(3U)), 0U);

    // Finishing the other two threads leaves the less urgent waiter parked on an event
    // nobody can set: that is a deadlock, and the scheduler says so rather than spinning.
    POUND_REQUIRE(POUND_SUCCESS == hos_kernel_thread_terminate(&g_kernel, high_id));
    POUND_REQUIRE(POUND_SUCCESS == hos_kernel_thread_terminate(&g_kernel, signaller_id));

    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_DEADLOCKED);

    machine_teardown();
}

POUND_TEST(hos_scheduler, a_wait_nothing_can_satisfy_deadlocks)
{
    machine_reset();

    hos_handle_t event = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create_event(&g_process, HOS_EVENT_AUTO, false, &event));

    const uint64_t handles = place_handles(0U, &event, 1U);

    const uint64_t waiter = program_begin();
    emit_wait(TEST_OUT_SLOT(5U), handles, 1U, -1);
    emit_spin();

    admit_thread(waiter, HOS_THREAD_PRIORITY_DEFAULT);

    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);
    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_DEADLOCKED);

    // Reportable rather than fatal: the thread is still there, still parked, and still
    // holding the call it could not finish.
    POUND_CHECK_EQ_U64(hos_scheduler_thread_count(&g_scheduler), 1U);
    POUND_CHECK_EQ_U64(hos_scheduler_ready_count(&g_scheduler), 0U);

    machine_teardown();
}

POUND_TEST(hos_scheduler, a_deadline_that_has_passed_is_not_a_deadlock)
{
    machine_reset();

    const uint64_t sleeper = program_begin();
    emit_sleep(1000000000); // One second: far beyond what these cases retire.
    emit_spin();

    admit_thread(sleeper, HOS_THREAD_PRIORITY_DEFAULT);

    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);
    POUND_CHECK_EQ_U64(g_park_count, 1U);

    // Nothing is runnable and the thread is parked, but it has a deadline, so the next
    // decision waits for it rather than declaring a deadlock. The clock is exactly at the
    // deadline and the thread is awake again.
    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);
    POUND_CHECK_EQ_U64(g_wake_count, 1U);
    POUND_CHECK_EQ_U64(g_last_wake.reason, HOS_WAKE_SLEPT);

    machine_teardown();
}

POUND_TEST(hos_scheduler, a_thread_whose_process_is_closed_is_dropped)
{
    machine_reset();

    const uint64_t waiter = program_begin();
    emit_spin();

    const uint64_t handled = program_begin();
    emit_spin();

    admit_thread(waiter, HOS_THREAD_PRIORITY_DEFAULT);
    admit_thread(handled, HOS_THREAD_PRIORITY_DEFAULT);

    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);

    // Closing the process terminates every thread it owned, which is what tells the
    // scheduler to stop watching them: it does not need to be told, it looks.
    hos_process_close(&g_process);

    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_FINISHED);
    POUND_CHECK_EQ_U64(hos_scheduler_thread_count(&g_scheduler), 0U);

    // The kernel can then be destroyed, so nothing was left holding it open.
    hos_scheduler_destroy(&g_scheduler);
    POUND_CHECK_EQ_U64(hos_kernel_destroy(&g_kernel), POUND_SUCCESS);
}

// -----------------------------------------------------------------------------
// Threads appearing and disappearing
// -----------------------------------------------------------------------------

POUND_TEST(hos_scheduler, an_exiting_thread_is_reaped)
{
    machine_reset();
    POUND_REQUIRE(POUND_SUCCESS == hos_scheduler_set_quantum(&g_scheduler, 4U));

    const uint64_t exiting = program_begin();
    emit_exit();
    emit_spin();

    const uint64_t worker = program_begin();
    emit_nop();
    emit_spin();

    const uint64_t exiting_id = admit_thread(exiting, HOS_THREAD_PRIORITY_DEFAULT);
    const uint64_t worker_id  = admit_thread(worker, HOS_THREAD_PRIORITY_DEFAULT);

    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);
    POUND_CHECK_EQ_U64(thread_pc(exiting_id), exiting);

    // The thread that exited is gone, so the next decision only has the other one to pick --
    // and it is picked without anything having had to tell the scheduler that a thread had
    // finished.
    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);
    POUND_REQUIRE(g_run_count == 2U);
    POUND_CHECK_EQ_U64(g_run_log[1], worker_id);
    POUND_CHECK_EQ_U64(hos_scheduler_thread_count(&g_scheduler), 1U);

    machine_teardown();
}

POUND_TEST(hos_scheduler, a_thread_keeps_running_after_its_handle_is_closed)
{
    machine_reset();
    POUND_REQUIRE(POUND_SUCCESS == hos_scheduler_set_quantum(&g_scheduler, 4U));

    const uint64_t program = program_begin();
    emit_nop();
    emit_spin();

    const hos_handle_t handle = create_thread(program, HOS_THREAD_PRIORITY_DEFAULT);

    uint64_t id = 0U;
    POUND_REQUIRE(POUND_SUCCESS == hos_process_thread_id(&g_process, handle, &id));
    POUND_REQUIRE(POUND_SUCCESS == hos_kernel_thread_start(&g_kernel, id));
    POUND_REQUIRE(POUND_SUCCESS == hos_scheduler_admit(&g_scheduler, &g_process, id));

    // The title closes its last handle to the thread while the thread is running. This is the
    // case the scheduler is built on identifiers to survive: a scheduler holding a handle
    // would lose the thread here and never run it again.
    POUND_REQUIRE(POUND_SUCCESS == hos_process_close_handle(&g_process, handle));
    POUND_CHECK_EQ_U64(hos_kernel_thread_count(&g_kernel), 1U);

    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);
    POUND_CHECK_EQ_U64(hos_scheduler_current_thread_id(&g_scheduler), id);
    POUND_CHECK_EQ_U64(thread_pc(id), program + 4U);

    // It can still be finished by identifier, and then it really is gone.
    POUND_REQUIRE(POUND_SUCCESS == hos_kernel_thread_terminate(&g_kernel, id));
    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_FINISHED);
    POUND_CHECK_EQ_U64(hos_kernel_thread_count(&g_kernel), 0U);

    machine_teardown();
}

POUND_TEST(hos_scheduler, finished_before_anything_is_admitted)
{
    machine_reset();

    // No threads at all is not a deadlock: there is nothing that was ever going to run.
    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_FINISHED);
    POUND_CHECK_EQ_U64(hos_scheduler_thread_count(&g_scheduler), 0U);
    POUND_CHECK_EQ_U64(hos_scheduler_current_thread_id(&g_scheduler), 0U);
    POUND_CHECK_PTR_NULL(hos_scheduler_current_process(&g_scheduler));

    machine_teardown();
}

POUND_TEST(hos_scheduler, admit_validates_what_it_is_given)
{
    machine_reset();

    const uint64_t program = program_begin();
    emit_spin();

    const hos_handle_t handle = create_thread(program, HOS_THREAD_PRIORITY_DEFAULT);

    uint64_t id = 0U;
    POUND_REQUIRE(POUND_SUCCESS == hos_process_thread_id(&g_process, handle, &id));

    // Not started yet, so there is nothing to schedule.
    POUND_CHECK_EQ_U64(hos_scheduler_admit(&g_scheduler, &g_process, id), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_scheduler_admit(&g_scheduler, NULL, id), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_scheduler_admit(&g_scheduler, &g_process, 0U), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_scheduler_admit(&g_scheduler, &g_process, 0xDEADU), POUND_ERROR_NOT_FOUND);

    POUND_REQUIRE(POUND_SUCCESS == hos_kernel_thread_start(&g_kernel, id));
    POUND_REQUIRE(POUND_SUCCESS == hos_scheduler_admit(&g_scheduler, &g_process, id));

    // Admitting the same thread twice would run one thread as if it were two.
    POUND_CHECK_EQ_U64(hos_scheduler_admit(&g_scheduler, &g_process, id), POUND_ERROR_ALREADY_INITIALIZED);
    POUND_CHECK_EQ_U64(hos_scheduler_thread_count(&g_scheduler), 1U);
    POUND_CHECK_EQ_U64(hos_scheduler_ready_count(&g_scheduler), 1U);

    machine_teardown();
}

// -----------------------------------------------------------------------------
// Lifecycle and arguments
// -----------------------------------------------------------------------------

POUND_TEST(hos_scheduler, lifecycle_is_idempotent_and_reusable)
{
    hos_kernel_t    kernel;
    hos_scheduler_t scheduler;

    memset(&kernel, 0, sizeof(kernel));
    memset(&scheduler, 0xAA, sizeof(scheduler)); // Init clears it itself.

    POUND_REQUIRE(POUND_SUCCESS == hos_kernel_init(&kernel));
    POUND_CHECK(!hos_scheduler_is_initialized(&scheduler));

    POUND_CHECK_EQ_U64(hos_scheduler_init(&scheduler, &kernel), POUND_SUCCESS);
    POUND_CHECK(hos_scheduler_is_initialized(&scheduler));

    POUND_CHECK_EQ_U64(hos_scheduler_init(&scheduler, &kernel), POUND_ERROR_ALREADY_INITIALIZED);

    // Destroy twice, and use it again afterwards: a scheduler is a stack object a case may
    // bring up more than once.
    hos_scheduler_destroy(&scheduler);
    hos_scheduler_destroy(&scheduler);

    POUND_CHECK(!hos_scheduler_is_initialized(&scheduler));
    POUND_CHECK_EQ_U64(hos_scheduler_init(&scheduler, &kernel), POUND_SUCCESS);

    hos_scheduler_destroy(&scheduler);
    hos_kernel_destroy(&kernel);
}

POUND_TEST(hos_scheduler, arguments_are_validated)
{
    machine_reset();

    POUND_CHECK(!hos_scheduler_is_initialized(NULL));
    POUND_CHECK_EQ_U64(hos_scheduler_init(NULL, &g_kernel), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_scheduler_set_quantum(NULL, 4U), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_scheduler_tick(NULL), 0U);
    POUND_CHECK_EQ_U64(hos_scheduler_thread_count(NULL), 0U);
    POUND_CHECK_EQ_U64(hos_scheduler_ready_count(NULL), 0U);
    POUND_CHECK_EQ_U64(hos_scheduler_current_thread_id(NULL), 0U);
    POUND_CHECK_PTR_NULL(hos_scheduler_current_process(NULL));

    // A quantum of zero would let one thread be picked without retiring an instruction, so
    // it is refused rather than treated as "no limit".
    POUND_CHECK_EQ_U64(hos_scheduler_set_quantum(&g_scheduler, 0U), POUND_ERROR_INVALID_ARGUMENT);

    hos_wake_t wake;
    POUND_CHECK(!hos_scheduler_take_wake(&g_scheduler, &wake));
    POUND_CHECK(!hos_scheduler_take_wake(&g_scheduler, NULL));

    hos_scheduler_outcome_t outcome = HOS_SCHEDULER_RAN;
    POUND_CHECK_EQ_U64(hos_scheduler_step(&g_scheduler, NULL, NULL, &outcome),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_scheduler_step(&g_scheduler, stub_run, NULL, NULL),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_scheduler_step(NULL, stub_run, NULL, &outcome),
                       POUND_ERROR_INVALID_ARGUMENT);

    // A scheduler that was never initialised refuses everything that would touch its tables,
    // rather than running off an uninitialised pointer.
    hos_scheduler_t uninitialised;
    memset(&uninitialised, 0, sizeof(uninitialised));

    POUND_CHECK_EQ_U64(hos_scheduler_set_quantum(&uninitialised, 4U), POUND_ERROR_NOT_INITIALIZED);
    POUND_CHECK_EQ_U64(hos_scheduler_admit(&uninitialised, &g_process, 1U), POUND_ERROR_NOT_INITIALIZED);
    POUND_CHECK_EQ_U64(hos_scheduler_block_current(&uninitialised, (const hos_handle_t *)&outcome, 1U, false, 0U),
                       POUND_ERROR_NOT_INITIALIZED);
    POUND_CHECK_EQ_U64(hos_scheduler_sleep_current(&uninitialised, 1U), POUND_ERROR_NOT_INITIALIZED);

    hos_scheduler_destroy(&uninitialised); // Safe on a struct that never came up.

    machine_teardown();
}

POUND_TEST(hos_scheduler, parking_outside_a_run_is_refused)
{
    machine_reset();

    const hos_handle_t handles[1] = {HOS_HANDLE_INVALID};

    // Nothing is running, so there is no thread to park -- and saying so beats parking some
    // other thread by accident.
    POUND_CHECK_EQ_U64(hos_scheduler_block_current(&g_scheduler, handles, 1U, false, 0U),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_scheduler_sleep_current(&g_scheduler, 1U), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_scheduler_block_current(&g_scheduler, NULL, 1U, false, 0U),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_scheduler_block_current(&g_scheduler, handles, 0U, false, 0U),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(
        hos_scheduler_block_current(&g_scheduler, handles, HOS_MAX_WAIT_HANDLES + 1U, false, 0U),
        POUND_ERROR_INVALID_ARGUMENT);

    // A yield from outside a run is a no-op rather than a crash.
    hos_scheduler_yield(&g_scheduler);

    machine_teardown();
}

// -----------------------------------------------------------------------------
// The clock
// -----------------------------------------------------------------------------

POUND_TEST(hos_scheduler, the_clock_advances_and_wakes_what_it_passes)
{
    machine_reset();
    POUND_REQUIRE(POUND_SUCCESS == hos_scheduler_set_quantum(&g_scheduler, 4U));

    POUND_CHECK_EQ_U64(hos_scheduler_tick(&g_scheduler), 0U);

    const uint64_t spinner = program_begin();
    emit_nop();
    emit_spin();

    admit_thread(spinner, HOS_THREAD_PRIORITY_DEFAULT);

    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);

    // One retired instruction is one tick, so a full quantum of four advances the clock by
    // four.
    POUND_CHECK_EQ_U64(hos_scheduler_tick(&g_scheduler), 4U);

    hos_scheduler_advance(&g_scheduler, 6U);
    POUND_CHECK_EQ_U64(hos_scheduler_tick(&g_scheduler), 10U);

    // The guard refuses to wrap rather than reporting a clock that jumped backwards.
    hos_scheduler_advance(&g_scheduler, UINT64_MAX);
    POUND_CHECK_EQ_U64(hos_scheduler_tick(&g_scheduler), UINT64_MAX);

    machine_teardown();
}

POUND_TEST(hos_scheduler, ticks_from_nanoseconds_rounds_up_and_saturates)
{
    // 19.2 MHz: one second is 19,200,000 ticks, and the ratio is exactly 12 ticks per 625
    // nanoseconds.
    POUND_CHECK_EQ_U64(hos_scheduler_ticks_from_nanoseconds(0U), 0U);
    POUND_CHECK_EQ_U64(hos_scheduler_ticks_from_nanoseconds(624U), 12U);
    POUND_CHECK_EQ_U64(hos_scheduler_ticks_from_nanoseconds(625U), 12U);
    POUND_CHECK_EQ_U64(hos_scheduler_ticks_from_nanoseconds(626U), 13U);
    POUND_CHECK_EQ_U64(hos_scheduler_ticks_from_nanoseconds(1000000000U), 19200000U);

    // The round-up is the point: a title asking for a positive duration must never be told it
    // has already elapsed.
    POUND_CHECK_EQ_U64(hos_scheduler_ticks_from_nanoseconds(1U), 1U);

    // An absurd duration is a very long sleep rather than a short one.
    POUND_CHECK_EQ_U64(hos_scheduler_ticks_from_nanoseconds(UINT64_MAX), UINT64_MAX);
}

POUND_TEST(hos_scheduler, threads_beyond_the_first_table_growth_are_scheduled)
{
    machine_reset();
    POUND_REQUIRE(POUND_SUCCESS == hos_scheduler_set_quantum(&g_scheduler, 4U));

    // One more thread than the fresh scheduler's table holds, so the growth path -- and the
    // copy of the run queue and the ordering scratch with it -- is exercised with threads
    // already in both.
    const uint32_t extra = HOS_SCHEDULER_INITIAL_THREADS + 1U;

    uint64_t ids[HOS_SCHEDULER_INITIAL_THREADS + 1U];

    for (uint32_t index = 0U; index < extra; index++)
    {
        const uint64_t program = program_begin();
        emit_nop();
        emit_spin();

        ids[index] = admit_thread(program, HOS_THREAD_PRIORITY_DEFAULT);
    }

    POUND_CHECK_EQ_U64(hos_scheduler_thread_count(&g_scheduler), extra);
    POUND_CHECK_EQ_U64(hos_scheduler_ready_count(&g_scheduler), extra);

    // Strict round robin over the whole set, in admission order.
    for (uint32_t turn = 0U; turn < extra; turn++)
    {
        POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);
    }

    POUND_REQUIRE(g_run_count == extra);

    for (uint32_t turn = 0U; turn < extra; turn++)
    {
        POUND_CHECK_EQ_U64(g_run_log[turn], ids[turn]);
    }

    machine_teardown();
}

// -----------------------------------------------------------------------------

POUND_TEST_SUITE(hos_scheduler,
                 POUND_TEST_CASE(hos_scheduler, quantum_expiry_hands_the_core_to_the_next_thread),
                 POUND_TEST_CASE(hos_scheduler, the_most_urgent_thread_keeps_the_core),
                 POUND_TEST_CASE(hos_scheduler, a_yield_lets_an_equal_priority_peer_in),
                 POUND_TEST_CASE(hos_scheduler, a_higher_priority_thread_is_not_displaced_by_a_yield),
                 POUND_TEST_CASE(hos_scheduler, a_signalled_event_wakes_the_waiting_thread),
                 POUND_TEST_CASE(hos_scheduler, a_poll_does_not_park_a_thread),
                 POUND_TEST_CASE(hos_scheduler, a_wait_times_out_when_the_clock_passes_its_deadline),
                 POUND_TEST_CASE(hos_scheduler, cancelling_a_parked_wait_wakes_it_with_an_interruption),
                 POUND_TEST_CASE(hos_scheduler, cancelling_a_sleep_is_not_an_interruption),
                 POUND_TEST_CASE(hos_scheduler, a_plain_sleep_wakes_without_a_result),
                 POUND_TEST_CASE(hos_scheduler, the_higher_priority_waiter_takes_an_auto_reset_signal),
                 POUND_TEST_CASE(hos_scheduler, a_wait_nothing_can_satisfy_deadlocks),
                 POUND_TEST_CASE(hos_scheduler, a_deadline_that_has_passed_is_not_a_deadlock),
                 POUND_TEST_CASE(hos_scheduler, a_thread_whose_process_is_closed_is_dropped),
                 POUND_TEST_CASE(hos_scheduler, an_exiting_thread_is_reaped),
                 POUND_TEST_CASE(hos_scheduler, a_thread_keeps_running_after_its_handle_is_closed),
                 POUND_TEST_CASE(hos_scheduler, finished_before_anything_is_admitted),
                 POUND_TEST_CASE(hos_scheduler, admit_validates_what_it_is_given),
                 POUND_TEST_CASE(hos_scheduler, lifecycle_is_idempotent_and_reusable),
                 POUND_TEST_CASE(hos_scheduler, arguments_are_validated),
                 POUND_TEST_CASE(hos_scheduler, parking_outside_a_run_is_refused),
                 POUND_TEST_CASE(hos_scheduler, the_clock_advances_and_wakes_what_it_passes),
                 POUND_TEST_CASE(hos_scheduler, ticks_from_nanoseconds_rounds_up_and_saturates),
                 POUND_TEST_CASE(hos_scheduler, threads_beyond_the_first_table_growth_are_scheduled))

/*** end of file ***/

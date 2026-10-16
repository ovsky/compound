//! Tests for the Horizon OS system call layer.
//!
//! ## The distinction most of these cases exist to hold down
//!
//! Every system call has two ways to go wrong, and they must not look alike: something
//! Horizon itself refuses, which is a `Result` in `X0` and a completed call, and something
//! Pound has not implemented, which is an error code and a trap left standing. A guest can
//! see the first and cannot see the second, so a case that asserts the *error code* and a
//! case that asserts the *Result* are testing opposite things even when they feed the
//! layer the same shape of bad input.
//!
//! ## Why the register file is the thing asserted on
//!
//! These cases write `X0..X3` and read them back, rather than calling handlers directly,
//! because the register file *is* the interface. A `Result` in the wrong register, or a
//! 32-bit answer that fails to zero the upper half of `X0`, is invisible from a handler's
//! return value and obvious from the registers.
//!
//! ## Why `pc` is checked as often as `X0`
//!
//! `pc` advancing is what makes a call complete. A refusal must leave it alone, because
//! a trap that silently steps past itself is a call that appears to have succeeded with no
//! answer -- so the `pc` assertions are load-bearing rather than incidental.

#include "pound_test.h"

#include "guest_state.h"
#include "hos/kernel.h"
#include "hos/memory_layout.h"
#include "hos/syscall.h"
#include "log.h"
#include "memory/guest_memory.h"

#include <stddef.h>
#include <string.h>

// -----------------------------------------------------------------------------
// Machine setup
// -----------------------------------------------------------------------------

#define TEST_GUEST_BASE 0x50000000ULL
#define TEST_GUEST_SIZE (1U << 16U)

/// Where a case puts things the guest is supposed to be able to point at.
#define TEST_NAME_ADDRESS (TEST_GUEST_BASE + 0x1000ULL)
#define TEST_HANDLES_ADDRESS (TEST_GUEST_BASE + 0x2000ULL)
#define TEST_STRING_ADDRESS (TEST_GUEST_BASE + 0x3000ULL)

/// Where a call that answers an out-parameter leaves its answer in guest memory.
///
/// Horizon answers these calls twice, and this file checks both places. The *kernel's*
/// answer is a register -- `svcWaitSynchronization`'s ready index is its `(Out) W1`, and
/// `svcConnectToNamedPort`, `svcCreateThread`, `svcGetThreadPriority` and `svcGetThreadId`
/// answer in `W1`/`X1` the same way -- and the caller's *stub* is what stores that register
/// through the out-pointer the caller passed (libnx's `svc.s` does `str x0, [sp, #-16]!;
/// svc ...; ldr x2, [sp], #16; str w1, [x2]`). Pound sets the register *and* writes the
/// slot directly, so a case that read only one of the two would pass against a layer that
/// answered in the wrong place for the other convention. These slots are the second place.
#define TEST_OUT_SLOT_0 (TEST_GUEST_BASE + 0x4000ULL)
#define TEST_OUT_SLOT_1 (TEST_GUEST_BASE + 0x4008ULL)

/// One past the end of the window, so a pointer there is unmapped by construction.
#define TEST_UNMAPPED_ADDRESS (TEST_GUEST_BASE + TEST_GUEST_SIZE)

POUND_ALIGNED(64) static uint8_t g_memory[TEST_GUEST_SIZE];

static guest_memory_t        g_memory_region;
static guest_state_t         g_state;
static hos_kernel_t          g_kernel;
static hos_process_t         g_process;
static hos_syscall_context_t g_context;
static hos_scheduler_t       g_scheduler;
static hos_memory_layout_t   g_layout;

/// The thread the blocking cases run, and its kernel-wide identifier.
static hos_handle_t g_thread;
static uint64_t     g_thread_id;

/// The one call a driver turn makes, and whether there is one left to make.
///
/// Stands in for an interpreter: a real run loop fetches the next `SVC` from the guest's
/// `pc` and dispatches it, and these cases dispatch a call they chose instead. What is
/// being tested is the same either way -- that a call which blocks parks the thread, and
/// that the thread is finished by `hos_syscall_complete_block` when it is woken.
static uint32_t g_call_number;
static bool     g_call_pending;

static uint8_t *
host_at(const uint64_t guest_address)
{
    return &g_memory[(size_t)(guest_address - TEST_GUEST_BASE)];
}

/// Reads the little-endian 64-bit word a call left at `guest_address`.
static uint64_t
read_u64_at(const uint64_t guest_address)
{
    const uint8_t *bytes = host_at(guest_address);
    uint64_t       value = 0U;

    for (size_t index = 0U; index < sizeof(uint64_t); index++)
    {
        value |= (uint64_t)bytes[index] << (8U * index);
    }

    return value;
}

/// Reads the little-endian 32-bit word a call left at `guest_address`.
static uint32_t
read_u32_at(const uint64_t guest_address)
{
    const uint8_t *bytes = host_at(guest_address);
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/// Brings up a kernel, a process, a guest window and a context over all three.
static void
machine_reset(void)
{
    memset(g_memory, 0, sizeof(g_memory));
    memset(&g_kernel, 0, sizeof(g_kernel));
    memset(&g_process, 0, sizeof(g_process));
    memset(&g_context, 0, sizeof(g_context));
    memset(&g_scheduler, 0, sizeof(g_scheduler));
    memset(&g_layout, 0, sizeof(g_layout));

    g_thread       = HOS_HANDLE_INVALID;
    g_thread_id    = 0U;
    g_call_number  = 0U;
    g_call_pending = false;

    POUND_REQUIRE(POUND_SUCCESS ==
                  guest_memory_init(&g_memory_region, g_memory, sizeof(g_memory), TEST_GUEST_BASE));
    POUND_REQUIRE(POUND_SUCCESS == guest_state_init(&g_state));
    g_state.pc = TEST_GUEST_BASE;

    POUND_REQUIRE(POUND_SUCCESS == hos_kernel_init(&g_kernel));
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create(&g_kernel, &g_process));

    hos_syscall_context_init(&g_context, &g_kernel, &g_process, &g_memory_region);

    // Non-zero, so a case that forgets to set them cannot pass by accident: a tick of
    // zero and a core of zero are both plausible-looking and both the zeroed default.
    g_context.processor_id = 2U;
    g_context.system_tick  = 0x1122334455667788ULL;
}

static void
machine_teardown(void)
{
    // Threads live in the kernel's object table and the scheduler only borrows their
    // identifiers, so the scheduler is torn down first: closing the process terminates and
    // frees its threads, and nothing should still be watching them when that happens.
    hos_scheduler_destroy(&g_scheduler);

    hos_process_close(&g_process);
    hos_kernel_destroy(&g_kernel);
}

/// Gives the machine a scheduler, which is what makes a context able to block.
///
/// Deliberately separate from `machine_reset`, because the refusal a context without a
/// scheduler produces is itself under test: a case that wants that refusal simply does not
/// call this.
static void
machine_add_scheduler(void)
{
    POUND_REQUIRE(POUND_SUCCESS == hos_scheduler_init(&g_scheduler, &g_kernel));
    POUND_REQUIRE(POUND_SUCCESS == hos_scheduler_set_quantum(&g_scheduler, 4096U));
    hos_syscall_context_set_scheduler(&g_context, &g_scheduler);
}

/// Gives the machine an address-space layout over its whole window, and binds it.
///
/// The window is the same flat buffer `machine_reset` installed, now carved into a map
/// region and a heap region, both `Free`. That means the gate is on: a case that wants to
/// point the guest at writable memory has to map it first with
/// `hos_memory_layout_map`. Separate from `machine_reset` for the same reason the
/// scheduler is: the refusal a context with no layout produces is itself under test.
static void
machine_add_layout(void)
{
    POUND_REQUIRE(POUND_SUCCESS ==
                  hos_memory_layout_init(&g_layout, &g_memory_region, TEST_GUEST_BASE, TEST_GUEST_BASE + TEST_GUEST_SIZE));
    hos_syscall_context_set_layout(&g_context, &g_layout);
}

/// Maps one page of the window read|write, so a call may write an answer into it.
///
/// The layout's heap region is the upper half of the window, and the out-slots are in the
/// lower half, so a page there is in the map region and Free until this is called.
static void
map_output_page(const uint64_t page_base)
{
    POUND_REQUIRE(POUND_SUCCESS == hos_memory_layout_map(&g_layout,
                                                         page_base,
                                                         HOS_MEMORY_PAGE_SIZE,
                                                         (uint32_t)HOS_MEMORY_STATE_NORMAL,
                                                         (GUEST_MEMORY_PERM_READ | GUEST_MEMORY_PERM_WRITE),
                                                         0U));
}

/// Creates, starts and admits one thread, ready to be run.
static void
start_a_thread(void)
{
    guest_state_t initial;

    POUND_REQUIRE(POUND_SUCCESS == guest_state_init(&initial));
    initial.pc = TEST_GUEST_BASE;

    POUND_REQUIRE(POUND_SUCCESS ==
                  hos_process_create_thread(&g_process, &initial, HOS_THREAD_PRIORITY_DEFAULT, &g_thread));
    POUND_REQUIRE(POUND_SUCCESS == hos_process_thread_id(&g_process, g_thread, &g_thread_id));
    POUND_REQUIRE(POUND_SUCCESS == hos_kernel_thread_start(&g_kernel, g_thread_id));
    POUND_REQUIRE(POUND_SUCCESS == hos_scheduler_admit(&g_scheduler, &g_process, g_thread_id));
}

/// The run function the scheduler is stepped with: real SVC dispatch, no interpreter.
///
/// It first finishes a call that was parked -- exactly what a run loop does with
/// `hos_scheduler_take_wake` and `hos_syscall_complete_block` -- and otherwise makes the one
/// call the case scripted, then reports zero instructions retired.
///
/// Zero is honest here rather than a stub: the clock is advanced by instructions retired,
/// and what the blocking cases examine is not the clock's rate but whether a thread can park
/// and be finished. A sleeping thread with no runnable peer still reaches its deadline,
/// because the scheduler jumps the clock forward when nothing can run -- so nothing is
/// retired and the deadline is still reached.
static error_t
driver_run(void *POUND_RESTRICT          context,
           hos_process_t *POUND_RESTRICT process,
           uint64_t                      thread_id,
           guest_state_t *POUND_RESTRICT state,
           uint32_t                      budget,
           uint32_t *POUND_RESTRICT      out_executed)
{
    POUND_UNUSED(context);
    POUND_UNUSED(process);
    POUND_UNUSED(thread_id);
    POUND_UNUSED(budget);

    *out_executed = 0U;

    hos_wake_t wake;

    if (hos_scheduler_take_wake(&g_scheduler, &wake))
    {
        return hos_syscall_complete_block(&g_context, state, &wake);
    }

    if (g_call_pending)
    {
        g_call_pending = false;
        return hos_syscall_dispatch(&g_context, g_call_number, state);
    }

    return POUND_SUCCESS;
}

/// Steps the scheduler once and reports what it did.
static hos_scheduler_outcome_t
step(void)
{
    // Not `POUND_REQUIRE`: that does a bare `return`, which this function cannot do because
    // it hands a value back.
    hos_scheduler_outcome_t outcome = HOS_SCHEDULER_DEADLOCKED;
    const error_t           error   = hos_scheduler_step(&g_scheduler, driver_run, NULL, &outcome);

    POUND_CHECK_EQ_U64(error, POUND_SUCCESS);
    return outcome;
}

/// Copies the running thread's register file out, so a case can read what a call left.
static void
thread_registers(guest_state_t *out_state)
{
    POUND_REQUIRE(POUND_SUCCESS == hos_kernel_thread_context(&g_kernel, g_thread_id, out_state));
}

/// Dispatches `number` with whatever is in the register file.
static error_t
call(const uint32_t number)
{
    return hos_syscall_dispatch(&g_context, number, &g_state);
}

/// Writes an eight-byte, space-padded service name, which is its form on the wire.
static void
put_name(const char *name, const uint64_t address)
{
    const size_t length = strlen(name);

    POUND_REQUIRE(length <= HOS_SYSCALL_SERVICE_NAME_MAX);

    uint8_t *bytes = host_at(address);
    memset(bytes, ' ', HOS_SYSCALL_SERVICE_NAME_MAX);
    memcpy(bytes, name, length);
}

/// Writes an array of handles the way the guest would have them laid out.
static void
put_handles(const hos_handle_t *handles, const uint32_t count, const uint64_t address)
{
    uint8_t *bytes = host_at(address);

    for (uint32_t index = 0U; index < count; index++)
    {
        const hos_handle_t handle = handles[index];
        const size_t       offset = (size_t)index * sizeof(uint32_t);

        bytes[offset + 0U] = (uint8_t)(handle);
        bytes[offset + 1U] = (uint8_t)(handle >> 8U);
        bytes[offset + 2U] = (uint8_t)(handle >> 16U);
        bytes[offset + 3U] = (uint8_t)(handle >> 24U);
    }
}

/// A service handler that copies its request into its reply.
static error_t
echo_handler(void *context,
             uint32_t command,
             const uint8_t *request,
             size_t request_size,
             uint8_t *response,
             size_t response_capacity,
             size_t *out_response_size)
{
    POUND_UNUSED(context);
    POUND_UNUSED(command);

    if (request_size > response_capacity)
    {
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (request_size > 0U)
    {
        memcpy(response, request, request_size);
    }

    *out_response_size = request_size;
    return POUND_SUCCESS;
}

// -----------------------------------------------------------------------------
// Dispatch
// -----------------------------------------------------------------------------

POUND_TEST(hos_syscall, name_table)
{
    POUND_CHECK_STR_EQ(hos_syscall_name(HOS_SVC_SIGNAL_EVENT), "svcSignalEvent");
    POUND_CHECK_STR_EQ(hos_syscall_name(HOS_SVC_WAIT_SYNCHRONIZATION), "svcWaitSynchronization");
    POUND_CHECK_STR_EQ(hos_syscall_name(HOS_SVC_CONNECT_TO_NAMED_PORT), "svcConnectToNamedPort");

    // Known to Horizon and named, even though Pound does not implement it: the name is
    // what makes the refusal below readable.
    POUND_CHECK_STR_EQ(hos_syscall_name(0x21U), "svcSendSyncRequest");
    POUND_CHECK_STR_EQ(hos_syscall_name(0x01U), "svcSetHeapSize");

    // Not a Horizon number at all: 0x34 through 0x40 are not system calls.
    POUND_CHECK(hos_syscall_name(0x38U) == NULL);
    POUND_CHECK(hos_syscall_name(0xFFU) == NULL);
}

POUND_TEST(hos_syscall, unimplemented_call_is_refused_and_leaves_the_trap)
{
    machine_reset();

    // A number Horizon defines and Pound does not implement: the refusal must name it.
    g_state.x[0] = 0U;
    pound_test_log_reset();
    POUND_CHECK_EQ_U64(call(0x21U), POUND_ERROR_UNSUPPORTED_SYSCALL);
    POUND_CHECK(pound_test_log_contains("svcSendSyncRequest"));
    POUND_CHECK(pound_test_log_contains("has not implemented"));

    // A number Horizon does not define: there is no name to give, so the number is given.
    pound_test_log_reset();
    POUND_CHECK_EQ_U64(call(0x38U), POUND_ERROR_UNSUPPORTED_SYSCALL);
    POUND_CHECK(pound_test_log_contains("0x38"));

    // Neither refusal moved the guest: `pc` still points at the `SVC`, and `X0` was not
    // overwritten with a `Result` that would look like an answer.
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE);
    POUND_CHECK_EQ_U64(g_state.x[0], 0U);
    POUND_CHECK_EQ_U64(g_context.outcome, HOS_SYSCALL_COMPLETED);

    machine_teardown();
}

POUND_TEST(hos_syscall, a_completed_call_steps_over_itself)
{
    machine_reset();

    g_state.x[0] = (uint64_t)HOS_SVC_GET_SYSTEM_TICK;
    POUND_CHECK_EQ_U64(call(HOS_SVC_GET_SYSTEM_TICK), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 4U);
    POUND_CHECK_EQ_U64(g_context.outcome, HOS_SYSCALL_COMPLETED);

    // A call that reports a *failure* to the guest still completed: the guest asked a
    // question, got an answer, and runs on. This is the case that separates a `Result`
    // from an error code.
    g_state.x[0] = HOS_HANDLE_INVALID;
    pound_test_log_reset();
    POUND_CHECK_EQ_U64(call(HOS_SVC_SIGNAL_EVENT), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_HANDLE);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 8U);

    machine_teardown();
}

POUND_TEST(hos_syscall, a_context_that_names_nothing_is_refused)
{
    POUND_CHECK_EQ_U64(hos_syscall_dispatch(NULL, HOS_SVC_GET_SYSTEM_TICK, &g_state),
                       POUND_ERROR_INVALID_ARGUMENT);

    machine_reset();

    POUND_CHECK_EQ_U64(hos_syscall_dispatch(&g_context, HOS_SVC_GET_SYSTEM_TICK, NULL),
                       POUND_ERROR_INVALID_ARGUMENT);

    // A context that was never pointed anywhere: every field is zero.
    hos_syscall_context_t blank;
    memset(&blank, 0, sizeof(blank));
    POUND_CHECK(!hos_kernel_is_initialized(blank.kernel));
    POUND_CHECK_EQ_U64(hos_syscall_dispatch(&blank, HOS_SVC_GET_SYSTEM_TICK, &g_state),
                       POUND_ERROR_INVALID_ARGUMENT);

    // The kernel is real but the process is not.
    hos_process_t unopened;
    memset(&unopened, 0, sizeof(unopened));
    hos_syscall_context_t other;
    hos_syscall_context_init(&other, &g_kernel, &unopened, &g_memory_region);
    POUND_CHECK_EQ_U64(hos_syscall_dispatch(&other, HOS_SVC_GET_SYSTEM_TICK, &g_state),
                       POUND_ERROR_NOT_INITIALIZED);

    // The process is real but belongs to a different kernel, which would have the call
    // checked against one kernel and run against another.
    hos_kernel_t  stranger_kernel;
    hos_process_t stranger_process;
    memset(&stranger_kernel, 0, sizeof(stranger_kernel));
    memset(&stranger_process, 0, sizeof(stranger_process));
    POUND_REQUIRE(POUND_SUCCESS == hos_kernel_init(&stranger_kernel));
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create(&stranger_kernel, &stranger_process));

    hos_syscall_context_init(&other, &g_kernel, &stranger_process, &g_memory_region);
    POUND_CHECK_EQ_U64(hos_syscall_dispatch(&other, HOS_SVC_GET_SYSTEM_TICK, &g_state),
                       POUND_ERROR_INVALID_ARGUMENT);

    // Initialising a context over a NULL context is a no-op rather than a crash, so a
    // caller cannot be made to fault by its own argument checking.
    hos_syscall_context_init(NULL, &g_kernel, &g_process, &g_memory_region);

    hos_process_close(&stranger_process);
    POUND_CHECK_EQ_U64(hos_kernel_destroy(&stranger_kernel), POUND_SUCCESS);
    machine_teardown();
}

// -----------------------------------------------------------------------------
// Calls that report on the machine
// -----------------------------------------------------------------------------

POUND_TEST(hos_syscall, processor_number_and_system_tick)
{
    machine_reset();

    POUND_CHECK_EQ_U64(call(HOS_SVC_GET_CURRENT_PROCESSOR_NUMBER), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 2U);

    // The upper half of the register must be clear: the guest reads `W0`, and a stale
    // upper half is the kind of bug that only shows up in a title that sign-extends.
    POUND_CHECK_EQ_U64(g_state.x[0] >> 32U, 0U);

    POUND_CHECK_EQ_U64(call(HOS_SVC_GET_SYSTEM_TICK), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0x1122334455667788ULL);

    // The tick is the caller's clock, so advancing it changes the answer without
    // anything else moving.
    g_context.system_tick = 0x1122334455667789ULL;
    POUND_CHECK_EQ_U64(call(HOS_SVC_GET_SYSTEM_TICK), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0x1122334455667789ULL);

    machine_teardown();
}

POUND_TEST(hos_syscall, cache_flushes_validate_the_range_they_are_given)
{
    machine_reset();

    // Returns nothing, and there is nothing to do, so the register file is left alone.
    g_state.x[0] = 0xDEADBEEFULL;
    POUND_CHECK_EQ_U64(call(HOS_SVC_FLUSH_ENTIRE_DATA_CACHE), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0xDEADBEEFULL);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 4U);

    // A zero-length range is a success with nothing to check.
    g_state.x[0] = TEST_GUEST_BASE;
    g_state.x[1] = 0U;
    POUND_CHECK_EQ_U64(call(HOS_SVC_FLUSH_DATA_CACHE), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);

    // A range inside the window flushes as a no-op on the host, but is accepted.
    g_state.x[0] = TEST_GUEST_BASE + 0x100ULL;
    g_state.x[1] = 0x100ULL;
    POUND_CHECK_EQ_U64(call(HOS_SVC_FLUSH_DATA_CACHE), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);

    // A range that runs off the end is the guest's mistake, and it is reported the way
    // the kernel reports it rather than crashing the emulator.
    g_state.x[0] = TEST_GUEST_BASE + 0x100ULL;
    g_state.x[1] = TEST_GUEST_SIZE;
    POUND_CHECK_EQ_U64(call(HOS_SVC_FLUSH_DATA_CACHE), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_POINTER);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 16U);

    machine_teardown();
}

POUND_TEST(hos_syscall, output_debug_string_writes_to_the_log)
{
    machine_reset();

    static const char message[] = "hello from the guest";
    memcpy(host_at(TEST_STRING_ADDRESS), message, sizeof(message) - 1U);

    g_state.x[0] = TEST_STRING_ADDRESS;
    g_state.x[1] = sizeof(message) - 1U;

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(call(HOS_SVC_OUTPUT_DEBUG_STRING), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);
    POUND_CHECK(pound_test_log_contains(message));

    // An empty string is a success: there is nothing to write, which is not an error.
    g_state.x[0] = TEST_STRING_ADDRESS;
    g_state.x[1] = 0U;
    POUND_CHECK_EQ_U64(call(HOS_SVC_OUTPUT_DEBUG_STRING), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);

    // A string that is not fully mapped is reported, not read.
    g_state.x[0] = TEST_UNMAPPED_ADDRESS;
    g_state.x[1] = 8U;
    POUND_CHECK_EQ_U64(call(HOS_SVC_OUTPUT_DEBUG_STRING), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_POINTER);

    machine_teardown();
}

// -----------------------------------------------------------------------------
// Calls that stop the guest
// -----------------------------------------------------------------------------

POUND_TEST(hos_syscall, exit_process_ends_the_process)
{
    machine_reset();

    POUND_CHECK_EQ_U64(call(HOS_SVC_EXIT_PROCESS), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_context.outcome, HOS_SYSCALL_EXITED_PROCESS);

    // `pc` is left on the `SVC` rather than stepped over: this is the last instruction
    // the process ran, and a debugger looking at the stop wants to see it.
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE);

    // And the outcome does not stick to the context: the next call resets it, so a
    // caller cannot read a previous call's exit.
    POUND_CHECK_EQ_U64(call(HOS_SVC_GET_SYSTEM_TICK), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_context.outcome, HOS_SYSCALL_COMPLETED);

    machine_teardown();
}

POUND_TEST(hos_syscall, break_stops_the_guest_without_pretending_to_answer)
{
    machine_reset();

    g_state.x[0] = 0x1234U;
    g_state.x[1] = 0x5678U;
    g_state.x[2] = 0x9ABCU;

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(call(HOS_SVC_BREAK), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_context.outcome, HOS_SYSCALL_BREAK);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE);
    POUND_CHECK_EQ_U64(g_state.x[0], 0x1234U);

    // The reason is in the log, because the emulator's job with a break is to get out of
    // the way with the reason still visible.
    POUND_CHECK(pound_test_log_contains("svcBreak"));
    POUND_CHECK(pound_test_log_contains("0x1234"));

    machine_teardown();
}

POUND_TEST(hos_syscall, yields_and_sleeps_without_a_scheduler)
{
    machine_reset();

    // The yields. A yield returns immediately, and with no scheduler there is nothing to
    // hand the core to, which makes it exactly the no-op it looks like.
    const int64_t yields[] = { 0, -1, -2 };

    for (uint32_t index = 0U; index < 3U; index++)
    {
        g_state.x[0] = (uint64_t)yields[index];
        POUND_CHECK_EQ_U64(call(HOS_SVC_SLEEP_THREAD), POUND_SUCCESS);
        POUND_CHECK_EQ_U64(g_context.outcome, HOS_SYSCALL_COMPLETED);
    }

    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 12U);

    // A real sleep must block, and returning early would run the guest against a clock that
    // did not advance -- so with no scheduler it is refused rather than shortened. (With
    // one it parks the caller; see the blocking cases below.)
    g_state.x[0] = 1000000U;
    pound_test_log_reset();
    POUND_CHECK_EQ_U64(call(HOS_SVC_SLEEP_THREAD), POUND_ERROR_UNSUPPORTED_SYSCALL);
    POUND_CHECK(pound_test_log_contains("svcSleepThread"));
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 12U);

    machine_teardown();
}

POUND_TEST(hos_syscall, sleep_system_returns)
{
    machine_reset();

    g_state.x[0] = 0xFFFFULL;
    POUND_CHECK_EQ_U64(call(HOS_SVC_SLEEP_SYSTEM), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0xFFFFULL);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 4U);

    machine_teardown();
}

// -----------------------------------------------------------------------------
// Handles and events
// -----------------------------------------------------------------------------

POUND_TEST(hos_syscall, signal_and_clear_an_event)
{
    machine_reset();

    hos_handle_t event = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS ==
                  hos_process_create_event(&g_process, HOS_EVENT_MANUAL, false, &event));

    g_state.x[0] = event;
    POUND_CHECK_EQ_U64(call(HOS_SVC_SIGNAL_EVENT), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 4U);

    bool signaled = false;
    POUND_CHECK_EQ_U64(hos_process_event_is_signaled(&g_process, event, &signaled), POUND_SUCCESS);
    POUND_CHECK(signaled);

    // `X0` is the call's answer as well as its argument, so every call below has to be
    // handed the handle again: leaving the previous `Result` in place would send
    // `0x00000000` to the next call, and the case would be testing a zero handle.
    g_state.x[0] = event;
    POUND_CHECK_EQ_U64(call(HOS_SVC_CLEAR_EVENT), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);
    POUND_CHECK_EQ_U64(hos_process_event_is_signaled(&g_process, event, &signaled), POUND_SUCCESS);
    POUND_CHECK(!signaled);

    // Clearing an event that is already clear is a success: `svcClearEvent` has to be
    // usable as "make sure this is clear".
    g_state.x[0] = event;
    POUND_CHECK_EQ_U64(call(HOS_SVC_CLEAR_EVENT), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);

    machine_teardown();
}

POUND_TEST(hos_syscall, event_calls_report_an_invalid_handle)
{
    machine_reset();

    POUND_REQUIRE(POUND_SUCCESS ==
                  hos_kernel_register_service(&g_kernel, "test-srv", echo_handler, NULL));

    hos_handle_t session = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS == hos_process_connect(&g_process, "test-srv", &session));

    // Zero names no object.
    g_state.x[0] = HOS_HANDLE_INVALID;
    POUND_CHECK_EQ_U64(call(HOS_SVC_SIGNAL_EVENT), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_HANDLE);

    // A session is a live handle of the wrong kind, and Horizon reports the two the same
    // way: the words for 0xE401 cover both. Each call is given the handle again, because
    // the previous call's `Result` is sitting in `X0` where the handle has to be.
    g_state.x[0] = session;
    POUND_CHECK_EQ_U64(call(HOS_SVC_SIGNAL_EVENT), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_HANDLE);

    g_state.x[0] = session;
    POUND_CHECK_EQ_U64(call(HOS_SVC_CLEAR_EVENT), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_HANDLE);

    g_state.x[0] = session;
    POUND_CHECK_EQ_U64(call(HOS_SVC_RESET_SIGNAL), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_HANDLE);

    machine_teardown();
}

POUND_TEST(hos_syscall, reset_signal_reports_whether_it_was_signaled)
{
    machine_reset();

    hos_handle_t event = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS ==
                  hos_process_create_event(&g_process, HOS_EVENT_MANUAL, false, &event));

    // Nothing to reset: a failure rather than a no-op, which is how a guest tells
    // "I consumed the signal" from "something else did".
    g_state.x[0] = event;
    POUND_CHECK_EQ_U64(call(HOS_SVC_RESET_SIGNAL), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_STATE);

    g_state.x[0] = event;
    POUND_CHECK_EQ_U64(call(HOS_SVC_SIGNAL_EVENT), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);

    g_state.x[0] = event;
    POUND_CHECK_EQ_U64(call(HOS_SVC_RESET_SIGNAL), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);

    bool signaled = true;
    POUND_CHECK_EQ_U64(hos_process_event_is_signaled(&g_process, event, &signaled), POUND_SUCCESS);
    POUND_CHECK(!signaled);

    // Once consumed it stays consumed, so the second reset reports the same thing the
    // first one would have.
    g_state.x[0] = event;
    POUND_CHECK_EQ_U64(call(HOS_SVC_RESET_SIGNAL), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_STATE);

    machine_teardown();
}

POUND_TEST(hos_syscall, close_handle_gives_the_handle_back)
{
    machine_reset();

    hos_handle_t event = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS ==
                  hos_process_create_event(&g_process, HOS_EVENT_AUTO, false, &event));

    POUND_CHECK_EQ_U64(hos_process_handle_count(&g_process), 1U);
    POUND_CHECK_EQ_U64(hos_kernel_object_count(&g_kernel), 1U);

    g_state.x[0] = event;
    POUND_CHECK_EQ_U64(call(HOS_SVC_CLOSE_HANDLE), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);
    POUND_CHECK_EQ_U64(hos_process_handle_count(&g_process), 0U);
    POUND_CHECK_EQ_U64(hos_kernel_object_count(&g_kernel), 0U);

    // Closing it again is a failure, not a second success: a double close that reported
    // success would hide a handle the guest still believes it holds. The handle is the
    // same value as before, which no longer names anything once closed.
    g_state.x[0] = event;
    POUND_CHECK_EQ_U64(call(HOS_SVC_CLOSE_HANDLE), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_HANDLE);

    machine_teardown();
}

// -----------------------------------------------------------------------------
// svcWaitSynchronization
// -----------------------------------------------------------------------------

POUND_TEST(hos_syscall, wait_synchronization_polls_the_handles)
{
    machine_reset();

    hos_handle_t quiet = HOS_HANDLE_INVALID;
    hos_handle_t ready = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create_event(&g_process, HOS_EVENT_AUTO, false, &quiet));
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create_event(&g_process, HOS_EVENT_AUTO, false, &ready));
    POUND_CHECK_EQ_U64(hos_process_event_signal(&g_process, ready), POUND_SUCCESS);

    const hos_handle_t handles[2] = { quiet, ready };
    put_handles(handles, 2U, TEST_HANDLES_ADDRESS);

    g_state.x[0] = TEST_OUT_SLOT_0;
    g_state.x[1] = TEST_HANDLES_ADDRESS;
    g_state.x[2] = 2U;
    g_state.x[3] = 0U;

    POUND_CHECK_EQ_U64(call(HOS_SVC_WAIT_SYNCHRONIZATION), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);

    // The index of the handle that was ready, counted from zero in the array. It lands in
    // two places: the slot through the pointer in X0 (Pound's belt-and-braces), and the
    // register W1, which is Horizon's actual `(Out)` and which the caller's stub stores
    // through that pointer itself. Both are asserted, because a layer that set only one
    // would satisfy one convention and silently break the other.
    POUND_CHECK_EQ_U64(read_u64_at(TEST_OUT_SLOT_0), 1U);
    POUND_CHECK_EQ_U64(g_state.x[1], 1U);

    // An auto-reset event is handed to exactly one waiter, so the wait consumed it and
    // the next one over the same array has nothing left to find.
    bool signaled = true;
    POUND_CHECK_EQ_U64(hos_process_event_is_signaled(&g_process, ready, &signaled), POUND_SUCCESS);
    POUND_CHECK(!signaled);

    // The second wait is over the same array, which now has nothing ready in it: the
    // signal was consumed above.
    g_state.x[0] = TEST_OUT_SLOT_0;
    g_state.x[1] = TEST_HANDLES_ADDRESS;
    g_state.x[2] = 2U;
    g_state.x[3] = 0U;

    // Poisoned first, to prove a timeout writes no index at all: a stale one would point
    // the guest at a handle it never asked about. W1 keeps the handle-array address it had
    // on entry, because a timeout has no index to answer with.
    memset(host_at(TEST_OUT_SLOT_0), 0xAA, sizeof(uint64_t));

    POUND_CHECK_EQ_U64(call(HOS_SVC_WAIT_SYNCHRONIZATION), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_TIMEOUT);
    POUND_CHECK_EQ_U64(read_u64_at(TEST_OUT_SLOT_0), 0xAAAAAAAAAAAAAAAAULL);
    POUND_CHECK_EQ_U64(g_state.x[1], TEST_HANDLES_ADDRESS);

    machine_teardown();
}

POUND_TEST(hos_syscall, wait_synchronization_treats_a_session_as_never_ready)
{
    machine_reset();

    POUND_REQUIRE(POUND_SUCCESS ==
                  hos_kernel_register_service(&g_kernel, "test-srv", echo_handler, NULL));

    hos_handle_t session = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS == hos_process_connect(&g_process, "test-srv", &session));

    const hos_handle_t handles[1] = { session };
    put_handles(handles, 1U, TEST_HANDLES_ADDRESS);

    g_state.x[0] = TEST_OUT_SLOT_0;
    g_state.x[1] = TEST_HANDLES_ADDRESS;
    g_state.x[2] = 1U;
    g_state.x[3] = 0U;

    // A session is a legitimate thing to wait on, so this is a timeout rather than an
    // error: the guest asked a fair question and the answer is "not yet".
    POUND_CHECK_EQ_U64(call(HOS_SVC_WAIT_SYNCHRONIZATION), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_TIMEOUT);

    machine_teardown();
}

POUND_TEST(hos_syscall, wait_synchronization_rejects_what_it_cannot_do)
{
    machine_reset();

    const hos_handle_t handles[1] = { HOS_HANDLE_INVALID };
    put_handles(handles, 1U, TEST_HANDLES_ADDRESS);

    g_state.x[0] = TEST_OUT_SLOT_0;
    g_state.x[1] = TEST_HANDLES_ADDRESS;
    g_state.x[2] = 1U;
    g_state.x[3] = 0U;

    // A handle in the array that names nothing can never be satisfied, so it is the
    // call's failure rather than something to skip past.
    POUND_CHECK_EQ_U64(call(HOS_SVC_WAIT_SYNCHRONIZATION), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_HANDLE);

    // An array the guest does not own.
    g_state.x[1] = TEST_UNMAPPED_ADDRESS;
    POUND_CHECK_EQ_U64(call(HOS_SVC_WAIT_SYNCHRONIZATION), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_POINTER);

    // More handles than the kernel takes.
    g_state.x[1] = TEST_HANDLES_ADDRESS;
    g_state.x[2] = HOS_SYSCALL_MAX_WAIT_HANDLES + 1U;
    POUND_CHECK_EQ_U64(call(HOS_SVC_WAIT_SYNCHRONIZATION), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_TOO_MANY_HANDLES);

    // A negative count is not a count.
    g_state.x[2] = 0xFFFFFFFFU;
    POUND_CHECK_EQ_U64(call(HOS_SVC_WAIT_SYNCHRONIZATION), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_TOO_MANY_HANDLES);

    machine_teardown();
}

POUND_TEST(hos_syscall, wait_synchronization_refuses_to_block_without_a_scheduler)
{
    machine_reset();

    hos_handle_t quiet = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create_event(&g_process, HOS_EVENT_AUTO, false, &quiet));

    const hos_handle_t handles[1] = { quiet };
    put_handles(handles, 1U, TEST_HANDLES_ADDRESS);

    // Zero handles is Horizon's "wait forever", and the one form that is a gap rather than
    // a poll: it can only be ended by the clock, and there is no honest way to shorten it
    // into a timeout the guest never asked for.
    g_state.x[0] = TEST_OUT_SLOT_0;
    g_state.x[1] = TEST_HANDLES_ADDRESS;
    g_state.x[2] = 0U;
    g_state.x[3] = 0U;
    pound_test_log_reset();
    POUND_CHECK_EQ_U64(call(HOS_SVC_WAIT_SYNCHRONIZATION), POUND_ERROR_UNSUPPORTED_SYSCALL);
    POUND_CHECK(pound_test_log_contains("no handles"));
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE);

    // A positive timeout on a handle that is not ready has to park the caller, and a
    // context with no scheduler has nowhere to park it. Answering with a timeout now would
    // be a lie the guest cannot detect, so the call is refused and the trap is left
    // standing -- and `X0`, which the call would have overwritten with a `Result`, is
    // untouched too.
    g_state.x[2] = 1U;
    g_state.x[3] = 1000000U;
    pound_test_log_reset();
    POUND_CHECK_EQ_U64(call(HOS_SVC_WAIT_SYNCHRONIZATION), POUND_ERROR_UNSUPPORTED_SYSCALL);
    POUND_CHECK(pound_test_log_contains("needs a scheduler"));
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE);
    POUND_CHECK_EQ_U64(g_state.x[0], TEST_OUT_SLOT_0);

    machine_teardown();
}

// -----------------------------------------------------------------------------
// svcConnectToNamedPort
// -----------------------------------------------------------------------------

POUND_TEST(hos_syscall, connect_to_named_port_returns_a_session)
{
    machine_reset();

    POUND_REQUIRE(POUND_SUCCESS ==
                  hos_kernel_register_service(&g_kernel, "fsp-srv", echo_handler, NULL));

    put_name("fsp-srv", TEST_NAME_ADDRESS);
    g_state.x[0] = TEST_OUT_SLOT_0;
    g_state.x[1] = TEST_NAME_ADDRESS;

    POUND_CHECK_EQ_U64(call(HOS_SVC_CONNECT_TO_NAMED_PORT), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);

    // The handle lands in two places: the register W1 (Horizon's `(Out)`, and what the
    // caller's stub stores through the pointer in X0), and the slot through that pointer
    // directly (Pound's belt-and-braces). The slot is read here, and W1 is asserted too --
    // a layer that answered in only one of the two would break the other convention.
    const hos_handle_t session = (hos_handle_t)(uint32_t)read_u64_at(TEST_OUT_SLOT_0);
    POUND_CHECK(session != HOS_HANDLE_INVALID);
    POUND_CHECK_EQ_U64((uint32_t)g_state.x[1], (uint32_t)session);
    POUND_CHECK_EQ_U64(hos_process_handle_count(&g_process), 1U);

    hos_object_type_t type = HOS_OBJECT_EVENT;
    POUND_CHECK_EQ_U64(hos_process_handle_type(&g_process, session, &type), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(type, HOS_OBJECT_SESSION);

    machine_teardown();
}

POUND_TEST(hos_syscall, connect_to_named_port_accepts_either_padding)
{
    machine_reset();

    POUND_REQUIRE(POUND_SUCCESS ==
                  hos_kernel_register_service(&g_kernel, "fsp-srv", echo_handler, NULL));

    // Space-padded is the wire form; a NUL-padded name is what a guest that copied a C
    // string in produces. Both mean the same port, so both must connect.
    put_name("fsp-srv", TEST_NAME_ADDRESS);
    g_state.x[0] = TEST_OUT_SLOT_0;
    g_state.x[1] = TEST_NAME_ADDRESS;
    POUND_CHECK_EQ_U64(call(HOS_SVC_CONNECT_TO_NAMED_PORT), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);
    POUND_CHECK_EQ_U64(hos_process_handle_count(&g_process), 1U);

    host_at(TEST_NAME_ADDRESS)[7] = 0U; // "fsp-srv" plus a NUL where the space was
    g_state.x[0] = TEST_OUT_SLOT_1;
    g_state.x[1] = TEST_NAME_ADDRESS;
    POUND_CHECK_EQ_U64(call(HOS_SVC_CONNECT_TO_NAMED_PORT), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);
    POUND_CHECK_EQ_U64(hos_process_handle_count(&g_process), 2U);

    machine_teardown();
}

POUND_TEST(hos_syscall, connect_to_named_port_reports_an_unwritable_out_pointer)
{
    machine_reset();

    POUND_REQUIRE(POUND_SUCCESS ==
                  hos_kernel_register_service(&g_kernel, "fsp-srv", echo_handler, NULL));

    put_name("fsp-srv", TEST_NAME_ADDRESS);

    // A pointer the guest does not own where the handle should go. The session is opened
    // and then handed straight back, because there is no way to tell the guest about it --
    // and leaving it open would give the process a session it can never name.
    g_state.x[0] = TEST_UNMAPPED_ADDRESS;
    g_state.x[1] = TEST_NAME_ADDRESS;

    POUND_CHECK_EQ_U64(call(HOS_SVC_CONNECT_TO_NAMED_PORT), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_POINTER);
    POUND_CHECK_EQ_U64(hos_process_handle_count(&g_process), 0U);
    POUND_CHECK_EQ_U64(hos_kernel_object_count(&g_kernel), 0U);

    machine_teardown();
}

POUND_TEST(hos_syscall, connect_to_a_service_pound_does_not_have_is_a_question)
{
    machine_reset();

    put_name("nosuchsv", TEST_NAME_ADDRESS);
    g_state.x[1] = TEST_NAME_ADDRESS;
    g_state.x[0] = 0xABCDULL;

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(call(HOS_SVC_CONNECT_TO_NAMED_PORT), POUND_ERROR_NOT_FOUND);

    // Not a guest-visible failure and not logged: this is a gap in Pound, and handing
    // back a `Result` for it would make the emulator look like firmware that answered.
    POUND_CHECK_EQ_U64(pound_test_log_count(), 0U);
    POUND_CHECK_EQ_U64(g_state.x[0], 0xABCDULL);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE);
    POUND_CHECK_EQ_U64(hos_process_handle_count(&g_process), 0U);

    machine_teardown();
}

POUND_TEST(hos_syscall, connect_to_named_port_rejects_a_nameless_port)
{
    machine_reset();

    // All eight bytes blank, which is no name at all. Horizon documents no `Result` for
    // this, so it is refused rather than answered.
    memset(host_at(TEST_NAME_ADDRESS), ' ', HOS_SYSCALL_SERVICE_NAME_MAX);
    g_state.x[0] = TEST_OUT_SLOT_0;
    g_state.x[1] = TEST_NAME_ADDRESS;

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(call(HOS_SVC_CONNECT_TO_NAMED_PORT), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK(pound_test_log_contains("empty service name"));
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE);

    // And a name the guest does not own is reported the way the kernel reports it.
    g_state.x[1] = TEST_UNMAPPED_ADDRESS;
    POUND_CHECK_EQ_U64(call(HOS_SVC_CONNECT_TO_NAMED_PORT), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_POINTER);

    machine_teardown();
}

// -----------------------------------------------------------------------------
// Blocking: the call parks the thread and the wake finishes it
// -----------------------------------------------------------------------------
//
// These cases run a real thread through a real stepper. The stepper is not an interpreter
// -- it dispatches one chosen `SVC` and reports zero instructions retired -- but everything
// on either side of it is real: the call parks the thread through the scheduler, the
// scheduler decides when the thread runs again and why, and `hos_syscall_complete_block`
// finishes the call with the wake's answer. That is the whole loop, minus the fetch.

POUND_TEST(hos_syscall, complete_block_writes_what_each_wake_means)
{
    machine_reset();

    // SIGNALED: the ready index goes through the pointer X0 still holds, and only then does
    // X0 become the Result.
    g_state.pc   = TEST_GUEST_BASE;
    g_state.x[0] = TEST_OUT_SLOT_0;

    hos_wake_t wake = { HOS_WAKE_SIGNALED, 3U };
    POUND_CHECK_EQ_U64(hos_syscall_complete_block(&g_context, &g_state, &wake), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(read_u64_at(TEST_OUT_SLOT_0), 3U);
    POUND_CHECK_EQ_U64(g_state.x[1], 3U);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 4U);
    POUND_CHECK_EQ_U64(g_context.outcome, HOS_SYSCALL_COMPLETED);

    // TIMED_OUT: there is no index to report, so the pointer is not written and the answer
    // is the timeout. The slot is poisoned first to prove nothing was written.
    g_state.pc   = TEST_GUEST_BASE;
    g_state.x[0] = TEST_OUT_SLOT_0;
    memset(host_at(TEST_OUT_SLOT_0), 0xAA, sizeof(uint64_t));

    wake = (hos_wake_t){ HOS_WAKE_TIMED_OUT, 0U };
    POUND_CHECK_EQ_U64(hos_syscall_complete_block(&g_context, &g_state, &wake), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(read_u64_at(TEST_OUT_SLOT_0), 0xAAAAAAAAAAAAAAAAULL);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_TIMEOUT);

    // SLEPT: svcSleepThread returns nothing, so X0 keeps the duration the guest passed. A
    // Result here would be the one thing a void call must not do.
    g_state.pc   = TEST_GUEST_BASE;
    g_state.x[0] = 1234567U;

    wake = (hos_wake_t){ HOS_WAKE_SLEPT, 0U };
    POUND_CHECK_EQ_U64(hos_syscall_complete_block(&g_context, &g_state, &wake), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 1234567U);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 4U);

    // A NULL argument is refused rather than dereferenced.
    POUND_CHECK_EQ_U64(hos_syscall_complete_block(NULL, &g_state, &wake), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_syscall_complete_block(&g_context, NULL, &wake), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_syscall_complete_block(&g_context, &g_state, NULL), POUND_ERROR_INVALID_ARGUMENT);

    machine_teardown();
}

POUND_TEST(hos_syscall, a_sleep_parks_the_caller_and_the_clock_wakes_it)
{
    machine_reset();
    machine_add_scheduler();
    start_a_thread();

    // The register file the call will read is set on the *thread*, because that is the copy
    // the stepper is handed: the shared `g_state` is not what runs here.
    guest_state_t registers;
    thread_registers(&registers);
    registers.x[0] = 1000000U; // about a millisecond
    POUND_REQUIRE(POUND_SUCCESS == hos_kernel_thread_set_context(&g_kernel, g_thread_id, &registers));

    // The one call the thread makes: a sleep of about a millisecond. It cannot be answered
    // now, so the thread has to park -- and it is the scheduler, not this layer, that will
    // bring it back.
    g_call_number  = HOS_SVC_SLEEP_THREAD;
    g_call_pending = true;

    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);

    // Parked, so nothing is runnable: a parked thread is not a ready one.
    POUND_CHECK_EQ_U64(hos_scheduler_ready_count(&g_scheduler), 0U);

    thread_registers(&registers);

    // Suspended mid-call rather than finished: `pc` is still on the SVC, and X0 still holds
    // the duration, because the call has not returned.
    POUND_CHECK_EQ_U64(registers.pc, TEST_GUEST_BASE);
    POUND_CHECK_EQ_U64(registers.x[0], 1000000U);

    // One more step reaches the deadline. Nothing else can run, so the scheduler jumps the
    // clock to it rather than spinning, and the thread wakes having slept.
    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);

    thread_registers(&registers);

    // svcSleepThread returns nothing, so X0 is exactly as the guest left it and only `pc`
    // has moved, past the SVC.
    POUND_CHECK_EQ_U64(registers.x[0], 1000000U);
    POUND_CHECK_EQ_U64(registers.pc, TEST_GUEST_BASE + 4U);
    POUND_CHECK(hos_scheduler_tick(&g_scheduler) >= hos_scheduler_ticks_from_nanoseconds(1000000U));

    machine_teardown();
}

POUND_TEST(hos_syscall, a_wait_parks_the_caller_and_a_signal_wakes_it)
{
    machine_reset();
    machine_add_scheduler();
    start_a_thread();

    hos_handle_t event = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create_event(&g_process, HOS_EVENT_AUTO, false, &event));

    const hos_handle_t handles[1] = { event };
    put_handles(handles, 1U, TEST_HANDLES_ADDRESS);

    // The register file the call will read is set on the *thread*, because that is the copy
    // the stepper is handed: the shared `g_state` is not what runs here.
    guest_state_t registers;
    thread_registers(&registers);
    registers.x[0] = TEST_OUT_SLOT_0;
    registers.x[1] = TEST_HANDLES_ADDRESS;
    registers.x[2] = 1U;
    registers.x[3] = (uint64_t)-1; // wait forever
    POUND_REQUIRE(POUND_SUCCESS == hos_kernel_thread_set_context(&g_kernel, g_thread_id, &registers));

    g_call_number  = HOS_SVC_WAIT_SYNCHRONIZATION;
    g_call_pending = true;

    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);
    POUND_CHECK_EQ_U64(hos_scheduler_ready_count(&g_scheduler), 0U);

    thread_registers(&registers);
    POUND_CHECK_EQ_U64(registers.pc, TEST_GUEST_BASE);   // still parked on the SVC
    POUND_CHECK_EQ_U64(registers.x[0], TEST_OUT_SLOT_0); // and the pointer is intact

    // Signal after the thread is already parked: this is the wake that matters, because a
    // signal before the wait would have been found by the poll and parked nothing.
    POUND_CHECK_EQ_U64(hos_process_event_signal(&g_process, event), POUND_SUCCESS);

    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);

    thread_registers(&registers);
    POUND_CHECK_EQ_U64(registers.x[0], HOS_RESULT_SUCCESS);
    POUND_CHECK_EQ_U64(registers.pc, TEST_GUEST_BASE + 4U);

    // The index of the handle that was ready, written through the pointer the parked call
    // left in X0.
    POUND_CHECK_EQ_U64(read_u64_at(TEST_OUT_SLOT_0), 0U);

    machine_teardown();
}

POUND_TEST(hos_syscall, a_wait_parks_the_caller_and_the_deadline_times_it_out)
{
    machine_reset();
    machine_add_scheduler();
    start_a_thread();

    hos_handle_t event = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create_event(&g_process, HOS_EVENT_AUTO, false, &event));

    const hos_handle_t handles[1] = { event };
    put_handles(handles, 1U, TEST_HANDLES_ADDRESS);

    guest_state_t registers;
    thread_registers(&registers);
    registers.x[0] = TEST_OUT_SLOT_0;
    registers.x[1] = TEST_HANDLES_ADDRESS;
    registers.x[2] = 1U;
    registers.x[3] = 1000000U; // one millisecond
    POUND_REQUIRE(POUND_SUCCESS == hos_kernel_thread_set_context(&g_kernel, g_thread_id, &registers));

    g_call_number  = HOS_SVC_WAIT_SYNCHRONIZATION;
    g_call_pending = true;

    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);
    POUND_CHECK_EQ_U64(hos_scheduler_ready_count(&g_scheduler), 0U);

    // Left ready the whole time, so the only thing that can end the wait is the deadline.
    memset(host_at(TEST_OUT_SLOT_0), 0xAA, sizeof(uint64_t));

    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);

    thread_registers(&registers);
    POUND_CHECK_EQ_U64(registers.x[0], HOS_RESULT_TIMEOUT);
    POUND_CHECK_EQ_U64(registers.pc, TEST_GUEST_BASE + 4U);

    // A timeout carries no index, so the slot is exactly as it was poisoned.
    POUND_CHECK_EQ_U64(read_u64_at(TEST_OUT_SLOT_0), 0xAAAAAAAAAAAAAAAAULL);

    // The event is still set, because nothing ever consumed it.
    bool signaled = false;
    POUND_CHECK_EQ_U64(hos_process_event_is_signaled(&g_process, event, &signaled), POUND_SUCCESS);
    POUND_CHECK(!signaled);

    machine_teardown();
}

// -----------------------------------------------------------------------------
// Threads
// -----------------------------------------------------------------------------

POUND_TEST(hos_syscall, thread_calls_are_refused_without_a_scheduler)
{
    machine_reset();

    // Every thread call, with no scheduler in the context: a thread that could be created
    // but never run would be one the guest believes in and the machine has never heard of.
    static const uint32_t calls[] = {
        HOS_SVC_CREATE_THREAD,       HOS_SVC_START_THREAD,         HOS_SVC_EXIT_THREAD,
        HOS_SVC_GET_THREAD_PRIORITY, HOS_SVC_SET_THREAD_PRIORITY,  HOS_SVC_GET_THREAD_ID,
    };

    for (size_t index = 0U; index < sizeof(calls) / sizeof(calls[0]); index++)
    {
        g_state.pc = TEST_GUEST_BASE;
        pound_test_log_reset();

        POUND_CHECK_EQ_U64(call(calls[index]), POUND_ERROR_UNSUPPORTED_SYSCALL);
        POUND_CHECK(pound_test_log_contains("no scheduler"));
        POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE);
    }

    machine_teardown();
}

POUND_TEST(hos_syscall, create_start_and_query_a_thread)
{
    machine_reset();
    machine_add_scheduler();

    // svcCreateThread: X0 out handle, X1 entry, X2 argument, X3 stack bottom, W4 priority.
    g_state.x[0] = TEST_OUT_SLOT_0;
    g_state.x[1] = 0x1234567890ABCDEFULL;
    g_state.x[2] = 0xFEEDFACEULL;
    g_state.x[3] = 0x40000000ULL;
    g_state.x[4] = HOS_THREAD_PRIORITY_DEFAULT;

    POUND_CHECK_EQ_U64(call(HOS_SVC_CREATE_THREAD), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);

    const hos_handle_t thread = (hos_handle_t)(uint32_t)read_u64_at(TEST_OUT_SLOT_0);
    POUND_CHECK(thread != HOS_HANDLE_INVALID);

    // The handle is also the register answer `W1`, which is where a libnx stub reads it
    // from before storing it through the pointer in X0.
    POUND_CHECK_EQ_U64((uint32_t)g_state.x[1], (uint32_t)thread);

    uint64_t id = 0U;
    POUND_REQUIRE(POUND_SUCCESS == hos_process_thread_id(&g_process, thread, &id));

    // Created, not started: nothing runs until svcStartThread, so the scheduler has not
    // been told about it.
    hos_thread_status_t status = HOS_THREAD_TERMINATED;
    POUND_CHECK_EQ_U64(hos_kernel_thread_status(&g_kernel, id, &status), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(status, HOS_THREAD_CREATED);
    POUND_CHECK_EQ_U64(hos_scheduler_thread_count(&g_scheduler), 0U);

    // The register file is the calling convention this layer is responsible for: entry in
    // `pc`, the argument in X0 and the stack pointer in X31.
    guest_state_t registers;
    POUND_REQUIRE(POUND_SUCCESS == hos_kernel_thread_context(&g_kernel, id, &registers));
    POUND_CHECK_EQ_U64(registers.pc, 0x1234567890ABCDEFULL);
    POUND_CHECK_EQ_U64(registers.x[0], 0xFEEDFACEULL);
    POUND_CHECK_EQ_U64(registers.x[31], 0x40000000ULL);

    // svcGetThreadPriority: pointer in X0, handle in X1.
    g_state.x[0] = TEST_OUT_SLOT_0;
    g_state.x[1] = thread;
    POUND_CHECK_EQ_U64(call(HOS_SVC_GET_THREAD_PRIORITY), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);
    POUND_CHECK_EQ_U64((uint32_t)read_u64_at(TEST_OUT_SLOT_0), HOS_THREAD_PRIORITY_DEFAULT);
    POUND_CHECK_EQ_U64((uint32_t)g_state.x[1], HOS_THREAD_PRIORITY_DEFAULT);

    // svcGetThreadId: pointer in X0, handle in X1. The answer is the kernel-wide id, which
    // is what the scheduler runs on and not the handle the guest used to ask. It is 64 bits
    // and answers in X1, unlike the 32-bit W1 of the other calls.
    g_state.x[0] = TEST_OUT_SLOT_1;
    g_state.x[1] = thread;
    POUND_CHECK_EQ_U64(call(HOS_SVC_GET_THREAD_ID), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);
    POUND_CHECK_EQ_U64(read_u64_at(TEST_OUT_SLOT_1), id);
    POUND_CHECK_EQ_U64(g_state.x[1], id);

    // svcStartThread: the handle in X0 and nothing else. Now it is schedulable.
    g_state.x[0] = thread;
    POUND_CHECK_EQ_U64(call(HOS_SVC_START_THREAD), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);
    POUND_CHECK_EQ_U64(hos_kernel_thread_status(&g_kernel, id, &status), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(status, HOS_THREAD_STARTED);
    POUND_CHECK_EQ_U64(hos_scheduler_thread_count(&g_scheduler), 1U);

    // Starting it again is 0xFA01, and the *layer* produces it rather than the kernel: the
    // kernel answers a second start with an error code, and only this layer knows which
    // Result word Horizon uses.
    g_state.x[0] = thread;
    POUND_CHECK_EQ_U64(call(HOS_SVC_START_THREAD), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_STATE);

    // svcSetThreadPriority: handle in X0, priority in W1 -- the other way round from the
    // getter, which is a real asymmetry in Horizon and not a typo here.
    g_state.x[0] = thread;
    g_state.x[1] = 0x10U;
    POUND_CHECK_EQ_U64(call(HOS_SVC_SET_THREAD_PRIORITY), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);

    g_state.x[0] = TEST_OUT_SLOT_0;
    g_state.x[1] = thread;
    POUND_CHECK_EQ_U64(call(HOS_SVC_GET_THREAD_PRIORITY), POUND_SUCCESS);
    POUND_CHECK_EQ_U64((uint32_t)read_u64_at(TEST_OUT_SLOT_0), 0x10U);
    POUND_CHECK_EQ_U64((uint32_t)g_state.x[1], 0x10U);

    machine_teardown();
}

POUND_TEST(hos_syscall, thread_calls_report_an_invalid_handle)
{
    machine_reset();
    machine_add_scheduler();

    // A handle that names nothing.
    g_state.x[0] = TEST_OUT_SLOT_1;
    g_state.x[1] = HOS_HANDLE_INVALID;
    POUND_CHECK_EQ_U64(call(HOS_SVC_GET_THREAD_ID), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_HANDLE);

    // A handle of the wrong kind: an event is not a thread, and Horizon reports the two the
    // same way, with the words `0xE401` carries.
    hos_handle_t event = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create_event(&g_process, HOS_EVENT_AUTO, false, &event));

    g_state.x[0] = event;
    POUND_CHECK_EQ_U64(call(HOS_SVC_START_THREAD), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_HANDLE);

    machine_teardown();
}

POUND_TEST(hos_syscall, an_out_of_range_priority_is_refused_not_answered)
{
    machine_reset();
    machine_add_scheduler();

    // Horizon refuses a priority outside 0..0x3F, but Pound has not established which
    // Result word it uses. Answering one would be a value a guest could branch on that
    // nobody verified, so the call is refused and the trap is left standing.
    g_state.x[0] = TEST_OUT_SLOT_0;
    g_state.x[1] = 0x1000U;
    g_state.x[2] = 0U;
    g_state.x[3] = 0U;
    g_state.x[4] = HOS_THREAD_PRIORITY_LOWEST + 1U;

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(call(HOS_SVC_CREATE_THREAD), POUND_ERROR_UNSUPPORTED_SYSCALL);
    POUND_CHECK(pound_test_log_contains("priority"));
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE);
    POUND_CHECK_EQ_U64(hos_process_handle_count(&g_process), 0U);

    machine_teardown();
}

POUND_TEST(hos_syscall, exit_thread_finishes_the_calling_thread)
{
    machine_reset();
    machine_add_scheduler();
    start_a_thread();

    g_call_number  = HOS_SVC_EXIT_THREAD;
    g_call_pending = true;

    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_RAN);

    // The thread finished itself. There is no handle in the arguments -- the caller is the
    // thread -- so what names it here is the scheduler's idea of who is running.
    hos_thread_status_t status = HOS_THREAD_STARTED;
    POUND_CHECK_EQ_U64(hos_kernel_thread_status(&g_kernel, g_thread_id, &status), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(status, HOS_THREAD_TERMINATED);

    // The next step reaps it and finds nothing left to run.
    POUND_CHECK_EQ_U64(step(), HOS_SCHEDULER_FINISHED);
    POUND_CHECK_EQ_U64(hos_scheduler_thread_count(&g_scheduler), 0U);

    machine_teardown();
}

// -----------------------------------------------------------------------------
// Memory calls: svcSetHeapSize and svcQueryMemory
// -----------------------------------------------------------------------------
//
// These answer from the region table, so a case has to give the machine a layout first.
// The window is small -- 64 KiB -- which is enough to exercise the shape of every answer
// and every refusal without pretending to be a real heap: the heap region is 32 KiB, so
// the only *size* `svcSetHeapSize` can accept here is zero, and every non-zero aligned
// request is the documented "larger than the heap region" refusal.

POUND_TEST(hos_syscall, set_heap_size_needs_a_layout)
{
    machine_reset();

    // No layout: there is no heap region to resize, so the call is refused as an
    // unimplemented feature rather than answered from an address space that does not exist.
    g_state.x[1] = 0U;
    pound_test_log_reset();

    POUND_CHECK_EQ_U64(call(HOS_SVC_SET_HEAP_SIZE), POUND_ERROR_UNSUPPORTED_SYSCALL);
    POUND_CHECK(pound_test_log_contains("no memory layout"));
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE);
    POUND_CHECK_EQ_U64(g_state.x[0], UINT64_C(0));

    machine_teardown();
}

POUND_TEST(hos_syscall, set_heap_size_answers_the_heap_base_and_its_refusals)
{
    machine_reset();
    machine_add_layout();

    const uint64_t heap_base = TEST_GUEST_BASE + (TEST_GUEST_SIZE / 2U);

    // Not a multiple of 2MB: 0xCA01, the request refused before the region is consulted.
    g_state.pc   = TEST_GUEST_BASE;
    g_state.x[1] = HOS_MEMORY_HEAP_GRANULARITY / 2U;
    POUND_CHECK_EQ_U64(call(HOS_SVC_SET_HEAP_SIZE), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_SIZE);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 4U);

    // Larger than 4GB, but aligned: still 0xCA01.
    g_state.pc   = TEST_GUEST_BASE;
    g_state.x[1] = HOS_MEMORY_HEAP_SIZE_MAX + HOS_MEMORY_HEAP_GRANULARITY;
    POUND_CHECK_EQ_U64(call(HOS_SVC_SET_HEAP_SIZE), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_SIZE);

    // Aligned and within 4GB, but larger than the 32 KiB heap region: 0xD001.
    g_state.pc   = TEST_GUEST_BASE;
    g_state.x[1] = HOS_MEMORY_HEAP_GRANULARITY;
    POUND_CHECK_EQ_U64(call(HOS_SVC_SET_HEAP_SIZE), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_OUT_OF_MEMORY);
    POUND_CHECK_EQ_U64(g_layout.heap_size, 0U);

    // Zero is a legitimate size, and the only one this window accepts. The answer is the
    // base address in X1 -- Horizon's `(Out) X1 HeapAddress` -- not in X0.
    g_state.pc   = TEST_GUEST_BASE;
    g_state.x[1] = 0U;
    POUND_CHECK_EQ_U64(call(HOS_SVC_SET_HEAP_SIZE), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[1], heap_base);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 4U);
    POUND_CHECK_EQ_U64(g_layout.heap_size, 0U);

    machine_teardown();
}

POUND_TEST(hos_syscall, query_memory_needs_a_layout)
{
    machine_reset();

    g_state.x[0] = TEST_OUT_SLOT_0;
    g_state.x[2] = TEST_GUEST_BASE;
    pound_test_log_reset();

    POUND_CHECK_EQ_U64(call(HOS_SVC_QUERY_MEMORY), POUND_ERROR_UNSUPPORTED_SYSCALL);
    POUND_CHECK(pound_test_log_contains("no memory layout"));
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE);

    machine_teardown();
}

POUND_TEST(hos_syscall, query_memory_reports_a_region_and_fabricates_one_outside)
{
    machine_reset();
    machine_add_layout();

    // Map the page the out-slot lives in, read|write, so the MemoryInfo has somewhere to go.
    map_output_page(TEST_GUEST_BASE + 0x4000U);

    // A mapped region: the answer is that region, field for field.
    g_state.pc   = TEST_GUEST_BASE;
    g_state.x[0] = TEST_OUT_SLOT_0;
    g_state.x[2] = TEST_GUEST_BASE + 0x4000U;
    POUND_CHECK_EQ_U64(call(HOS_SVC_QUERY_MEMORY), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[1], 0U); // Horizon's `(Out) W1 PageInfo`, always zero
    POUND_CHECK_EQ_U64(read_u64_at(TEST_OUT_SLOT_0), TEST_GUEST_BASE + 0x4000U);
    POUND_CHECK_EQ_U64(read_u64_at(TEST_OUT_SLOT_0 + 8U), 0x1000U);
    POUND_CHECK_EQ_U64(read_u32_at(TEST_OUT_SLOT_0 + 0x10U), HOS_MEMORY_STATE_NORMAL);
    POUND_CHECK_EQ_U64(read_u32_at(TEST_OUT_SLOT_0 + 0x14U), 0U);
    POUND_CHECK_EQ_U64(read_u32_at(TEST_OUT_SLOT_0 + 0x18U), GUEST_MEMORY_PERM_READ | GUEST_MEMORY_PERM_WRITE);
    POUND_CHECK_EQ_U64(read_u32_at(TEST_OUT_SLOT_0 + 0x1CU), 0U);
    POUND_CHECK_EQ_U64(read_u32_at(TEST_OUT_SLOT_0 + 0x20U), 0U);

    // A Free region inside the address space: reported as itself, with no permission.
    g_state.pc   = TEST_GUEST_BASE;
    g_state.x[0] = TEST_OUT_SLOT_0;
    g_state.x[2] = TEST_GUEST_BASE + 0x1000U;
    POUND_CHECK_EQ_U64(call(HOS_SVC_QUERY_MEMORY), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[1], 0U);
    POUND_CHECK_EQ_U64(read_u64_at(TEST_OUT_SLOT_0), TEST_GUEST_BASE);
    POUND_CHECK_EQ_U64(read_u64_at(TEST_OUT_SLOT_0 + 8U), 0x4000U);
    POUND_CHECK_EQ_U64(read_u32_at(TEST_OUT_SLOT_0 + 0x10U), HOS_MEMORY_STATE_FREE);
    POUND_CHECK_EQ_U64(read_u32_at(TEST_OUT_SLOT_0 + 0x18U), 0U);

    // An address outside the address space gets Horizon's fabricated `Inaccessible` block:
    // base at the end of the address space, a size reaching the top of memory, and success.
    g_state.pc   = TEST_GUEST_BASE;
    g_state.x[0] = TEST_OUT_SLOT_0;
    g_state.x[2] = TEST_GUEST_BASE + TEST_GUEST_SIZE;
    POUND_CHECK_EQ_U64(call(HOS_SVC_QUERY_MEMORY), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[1], 0U);
    POUND_CHECK_EQ_U64(read_u64_at(TEST_OUT_SLOT_0), TEST_GUEST_BASE + TEST_GUEST_SIZE);
    POUND_CHECK_EQ_U64(read_u64_at(TEST_OUT_SLOT_0 + 8U), (uint64_t)0U - (TEST_GUEST_BASE + TEST_GUEST_SIZE));
    POUND_CHECK_EQ_U64(read_u32_at(TEST_OUT_SLOT_0 + 0x10U), HOS_MEMORY_STATE_INACCESSIBLE);

    // Below the address space gets the same block. The kernel's containment check is
    // `start <= addr < end`, so "below" is simply not contained.
    g_state.pc   = TEST_GUEST_BASE;
    g_state.x[0] = TEST_OUT_SLOT_0;
    g_state.x[2] = TEST_GUEST_BASE - 0x1000U;
    POUND_CHECK_EQ_U64(call(HOS_SVC_QUERY_MEMORY), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);
    POUND_CHECK_EQ_U64(read_u64_at(TEST_OUT_SLOT_0), TEST_GUEST_BASE + TEST_GUEST_SIZE);
    POUND_CHECK_EQ_U64(read_u32_at(TEST_OUT_SLOT_0 + 0x10U), HOS_MEMORY_STATE_INACCESSIBLE);

    // An unwritable MemoryInfo pointer: the region is Free, so the write is refused and the
    // call reports a bad address rather than half-writing the struct. X1 is left as the
    // guest left it, since hardware's value on this failure is not established.
    g_state.pc   = TEST_GUEST_BASE;
    g_state.x[0] = TEST_GUEST_BASE + 0x1000U; // Free, inside the window
    g_state.x[1] = 0xABCDEFU;
    g_state.x[2] = TEST_GUEST_BASE + 0x4000U;
    POUND_CHECK_EQ_U64(call(HOS_SVC_QUERY_MEMORY), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_POINTER);
    POUND_CHECK_EQ_U64(g_state.x[1], 0xABCDEFU);

    machine_teardown();
}

// -----------------------------------------------------------------------------
// Memory calls: svcSetMemoryPermission and svcSetMemoryAttribute
// -----------------------------------------------------------------------------
//
// These reprotect a range that is already mapped, so a case maps a page first. The
// window is the same 64 KiB one, and the page they touch is in the map region.

POUND_TEST(hos_syscall, set_memory_permission_needs_a_layout)
{
    machine_reset();

    g_state.pc   = TEST_GUEST_BASE;
    g_state.x[0] = TEST_GUEST_BASE;
    g_state.x[1] = HOS_MEMORY_PAGE_SIZE;
    g_state.x[2] = GUEST_MEMORY_PERM_READ;

    POUND_CHECK_EQ_U64(call(HOS_SVC_SET_MEMORY_PERMISSION), POUND_ERROR_UNSUPPORTED_SYSCALL);
    POUND_CHECK(pound_test_log_contains("no memory layout"));
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE);

    machine_teardown();
}

POUND_TEST(hos_syscall, set_memory_attribute_needs_a_layout)
{
    machine_reset();

    g_state.pc   = TEST_GUEST_BASE;
    g_state.x[0] = TEST_GUEST_BASE;
    g_state.x[1] = HOS_MEMORY_PAGE_SIZE;
    g_state.x[2] = HOS_MEMORY_ATTRIBUTE_UNCACHED;
    g_state.x[3] = HOS_MEMORY_ATTRIBUTE_UNCACHED;

    POUND_CHECK_EQ_U64(call(HOS_SVC_SET_MEMORY_ATTRIBUTE), POUND_ERROR_UNSUPPORTED_SYSCALL);
    POUND_CHECK(pound_test_log_contains("no memory layout"));
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE);

    machine_teardown();
}

POUND_TEST(hos_syscall, set_memory_permission_reports_its_refusals_and_reprotects)
{
    machine_reset();
    machine_add_layout();

    const uint64_t normal_base = TEST_GUEST_BASE + 0x1000U;
    map_output_page(normal_base);

    // The checks are Horizon's, in its order: address, then size, then permission.
    g_state.pc   = TEST_GUEST_BASE;
    g_state.x[0] = normal_base + 1U; // misaligned address
    g_state.x[1] = HOS_MEMORY_PAGE_SIZE;
    g_state.x[2] = GUEST_MEMORY_PERM_READ;
    POUND_CHECK_EQ_U64(call(HOS_SVC_SET_MEMORY_PERMISSION), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_ADDRESS);

    g_state.x[0] = normal_base;
    g_state.x[1] = 0U; // zero size
    POUND_CHECK_EQ_U64(call(HOS_SVC_SET_MEMORY_PERMISSION), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_SIZE);

    g_state.x[0] = normal_base;
    g_state.x[1] = HOS_MEMORY_PAGE_SIZE + 1U; // misaligned size
    POUND_CHECK_EQ_U64(call(HOS_SVC_SET_MEMORY_PERMISSION), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_SIZE);

    // Write-only, and read|execute: neither is a permission this call takes.
    g_state.x[0] = normal_base;
    g_state.x[1] = HOS_MEMORY_PAGE_SIZE;
    g_state.x[2] = GUEST_MEMORY_PERM_WRITE;
    POUND_CHECK_EQ_U64(call(HOS_SVC_SET_MEMORY_PERMISSION), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_NEW_PERMISSION);

    g_state.x[0] = normal_base;
    g_state.x[2] = GUEST_MEMORY_PERM_READ | GUEST_MEMORY_PERM_EXEC;
    POUND_CHECK_EQ_U64(call(HOS_SVC_SET_MEMORY_PERMISSION), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_NEW_PERMISSION);

    // A range outside the address space, and a Free region inside it that may not be
    // reprotected: both are the kernel's `0xD401`.
    g_state.x[2] = GUEST_MEMORY_PERM_READ;
    g_state.x[0] = TEST_GUEST_BASE + TEST_GUEST_SIZE;
    POUND_CHECK_EQ_U64(call(HOS_SVC_SET_MEMORY_PERMISSION), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_CURRENT_MEMORY);

    g_state.x[0] = TEST_GUEST_BASE; // Free
    POUND_CHECK_EQ_U64(call(HOS_SVC_SET_MEMORY_PERMISSION), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_CURRENT_MEMORY);

    // Success: the mapped page becomes read-only, and the gate agrees.
    g_state.x[0] = normal_base;
    POUND_CHECK_EQ_U64(call(HOS_SVC_SET_MEMORY_PERMISSION), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);

    const guest_memory_region_t *region = hos_memory_layout_find(&g_layout, normal_base);
    POUND_REQUIRE(region != NULL);
    POUND_CHECK_EQ_U64(region->permission, GUEST_MEMORY_PERM_READ);

    size_t bytes = 0U;
    POUND_CHECK_PTR_NON_NULL(guest_memory_translate_read(&g_memory_region, normal_base, &bytes));
    POUND_CHECK_PTR_NULL(guest_memory_translate_write(&g_memory_region, normal_base, &bytes));

    machine_teardown();
}

POUND_TEST(hos_syscall, set_memory_attribute_reports_its_refusals_and_changes_the_bit)
{
    machine_reset();
    machine_add_layout();

    const uint64_t normal_base = TEST_GUEST_BASE + 0x1000U;
    map_output_page(normal_base);

    // Only the uncached bit may be changed: a mask reaching for the locked bit is the
    // kernel's `0xE801`, even before the range is looked at.
    g_state.pc   = TEST_GUEST_BASE;
    g_state.x[0] = normal_base;
    g_state.x[1] = HOS_MEMORY_PAGE_SIZE;
    g_state.x[2] = HOS_MEMORY_ATTRIBUTE_LOCKED;
    g_state.x[3] = HOS_MEMORY_ATTRIBUTE_LOCKED;
    POUND_CHECK_EQ_U64(call(HOS_SVC_SET_MEMORY_ATTRIBUTE), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_COMBINATION);

    // A value that is not a subset of the mask is refused the same way.
    g_state.x[0] = normal_base;
    g_state.x[2] = HOS_MEMORY_ATTRIBUTE_UNCACHED;
    g_state.x[3] = HOS_MEMORY_ATTRIBUTE_LOCKED;
    POUND_CHECK_EQ_U64(call(HOS_SVC_SET_MEMORY_ATTRIBUTE), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_COMBINATION);

    // Address and size come before the region.
    g_state.x[2] = HOS_MEMORY_ATTRIBUTE_UNCACHED;
    g_state.x[3] = HOS_MEMORY_ATTRIBUTE_UNCACHED;
    g_state.x[0] = normal_base + 1U;
    POUND_CHECK_EQ_U64(call(HOS_SVC_SET_MEMORY_ATTRIBUTE), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_ADDRESS);

    g_state.x[0] = normal_base;
    g_state.x[1] = 0U;
    POUND_CHECK_EQ_U64(call(HOS_SVC_SET_MEMORY_ATTRIBUTE), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_SIZE);

    // A Free region carries no change flag.
    g_state.x[1] = HOS_MEMORY_PAGE_SIZE;
    g_state.x[0] = TEST_GUEST_BASE;
    POUND_CHECK_EQ_U64(call(HOS_SVC_SET_MEMORY_ATTRIBUTE), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_CURRENT_MEMORY);

    // Success: the uncached bit is set, and the table says so.
    g_state.x[0] = normal_base;
    POUND_CHECK_EQ_U64(call(HOS_SVC_SET_MEMORY_ATTRIBUTE), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);

    const guest_memory_region_t *region = hos_memory_layout_find(&g_layout, normal_base);
    POUND_REQUIRE(region != NULL);
    POUND_CHECK_EQ_U64(region->attribute, HOS_MEMORY_ATTRIBUTE_UNCACHED);

    // Clearing it again succeeds and is visible in the table.
    g_state.x[0] = normal_base;
    g_state.x[3] = 0U;
    POUND_CHECK_EQ_U64(call(HOS_SVC_SET_MEMORY_ATTRIBUTE), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);
    POUND_CHECK_EQ_U64(region->attribute, 0U);

    machine_teardown();
}

// -----------------------------------------------------------------------------

POUND_TEST_SUITE(hos_syscall,
                 POUND_TEST_CASE(hos_syscall, name_table),
                 POUND_TEST_CASE(hos_syscall, unimplemented_call_is_refused_and_leaves_the_trap),
                 POUND_TEST_CASE(hos_syscall, a_completed_call_steps_over_itself),
                 POUND_TEST_CASE(hos_syscall, a_context_that_names_nothing_is_refused),
                 POUND_TEST_CASE(hos_syscall, processor_number_and_system_tick),
                 POUND_TEST_CASE(hos_syscall, cache_flushes_validate_the_range_they_are_given),
                 POUND_TEST_CASE(hos_syscall, output_debug_string_writes_to_the_log),
                 POUND_TEST_CASE(hos_syscall, exit_process_ends_the_process),
                 POUND_TEST_CASE(hos_syscall, break_stops_the_guest_without_pretending_to_answer),
                 POUND_TEST_CASE(hos_syscall, yields_and_sleeps_without_a_scheduler),
                 POUND_TEST_CASE(hos_syscall, sleep_system_returns),
                 POUND_TEST_CASE(hos_syscall, signal_and_clear_an_event),
                 POUND_TEST_CASE(hos_syscall, event_calls_report_an_invalid_handle),
                 POUND_TEST_CASE(hos_syscall, reset_signal_reports_whether_it_was_signaled),
                 POUND_TEST_CASE(hos_syscall, close_handle_gives_the_handle_back),
                 POUND_TEST_CASE(hos_syscall, wait_synchronization_polls_the_handles),
                 POUND_TEST_CASE(hos_syscall, wait_synchronization_treats_a_session_as_never_ready),
                 POUND_TEST_CASE(hos_syscall, wait_synchronization_rejects_what_it_cannot_do),
                 POUND_TEST_CASE(hos_syscall, wait_synchronization_refuses_to_block_without_a_scheduler),
                 POUND_TEST_CASE(hos_syscall, connect_to_named_port_returns_a_session),
                 POUND_TEST_CASE(hos_syscall, connect_to_named_port_accepts_either_padding),
                 POUND_TEST_CASE(hos_syscall, connect_to_named_port_reports_an_unwritable_out_pointer),
                 POUND_TEST_CASE(hos_syscall, connect_to_a_service_pound_does_not_have_is_a_question),
                 POUND_TEST_CASE(hos_syscall, connect_to_named_port_rejects_a_nameless_port),
                 POUND_TEST_CASE(hos_syscall, complete_block_writes_what_each_wake_means),
                 POUND_TEST_CASE(hos_syscall, a_sleep_parks_the_caller_and_the_clock_wakes_it),
                 POUND_TEST_CASE(hos_syscall, a_wait_parks_the_caller_and_a_signal_wakes_it),
                 POUND_TEST_CASE(hos_syscall, a_wait_parks_the_caller_and_the_deadline_times_it_out),
                 POUND_TEST_CASE(hos_syscall, thread_calls_are_refused_without_a_scheduler),
                 POUND_TEST_CASE(hos_syscall, create_start_and_query_a_thread),
                 POUND_TEST_CASE(hos_syscall, thread_calls_report_an_invalid_handle),
                 POUND_TEST_CASE(hos_syscall, an_out_of_range_priority_is_refused_not_answered),
                 POUND_TEST_CASE(hos_syscall, exit_thread_finishes_the_calling_thread),
                 POUND_TEST_CASE(hos_syscall, set_heap_size_needs_a_layout),
                 POUND_TEST_CASE(hos_syscall, set_heap_size_answers_the_heap_base_and_its_refusals),
                 POUND_TEST_CASE(hos_syscall, query_memory_needs_a_layout),
                 POUND_TEST_CASE(hos_syscall, query_memory_reports_a_region_and_fabricates_one_outside),
                 POUND_TEST_CASE(hos_syscall, set_memory_permission_needs_a_layout),
                 POUND_TEST_CASE(hos_syscall, set_memory_permission_reports_its_refusals_and_reprotects),
                 POUND_TEST_CASE(hos_syscall, set_memory_attribute_needs_a_layout),
                 POUND_TEST_CASE(hos_syscall, set_memory_attribute_reports_its_refusals_and_changes_the_bit))

/*** end of file ***/

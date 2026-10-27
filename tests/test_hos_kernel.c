//! Tests for the Horizon OS kernel abstraction.
//!
//! ## What these cases are pinning
//!
//! Three properties of this module are the reason it exists, and most of the cases below
//! exist to hold one of them down:
//!
//! - **A handle is per-process authority.** The same object has a different handle in
//!   each process that holds one, closing a handle gives that authority back, and the
//!   object dies only when the last handle to it does. The duplication and close cases
//!   check the reference count from both sides, because an off-by-one there is
//!   invisible until it is a use-after-free or a leak.
//! - **A closed handle never comes back.** A handle that names nothing fails, and it
//!   keeps failing after its slot is reused for a different object. That is the case
//!   `close_handle_advances_the_generation` exists for: without the generation, the
//!   second event created in the same slot would be reachable through the first
//!   event's stale handle.
//! - **A handler runs with the kernel unlocked.** `handler_may_re_enter_the_kernel`
//!   signals an event from inside a service handler. If the lock were still held this
//!   would block forever on a non-recursive mutex, so the case is a real check of the
//!   contract rather than a description of it.
//!
//! ## What the log assertions are pinning
//!
//! Two errors in this module are silent on purpose and one is loud on purpose, and the
//! distinction is the contract rather than a detail. `POUND_ERROR_NOT_FOUND` never logs:
//! probing for a service a firmware version does not have is an ordinary question.
//! `POUND_ERROR_INVALID_HANDLE` always logs: a handle that names nothing is a guest bug
//! or an emulator bug, and the line naming *which* handle was rejected is the whole
//! value of the code. So the absent-service cases assert the capture stayed empty and
//! the bad-handle cases assert it did not.

#include "pound_test.h"

#include "hos/kernel.h"
#include "log.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

// -----------------------------------------------------------------------------
// A service that is just enough to observe how a request was routed
// -----------------------------------------------------------------------------

/// Commands the test service below understands.
enum
{
    /// Copies the request into the response, refusing if the reply does not fit.
    TEST_COMMAND_ECHO = 1,

    /// Always fails, to check that a handler's error reaches the caller unchanged.
    TEST_COMMAND_REFUSE = 2,

    /// Signals the event named by the context, from inside the handler.
    TEST_COMMAND_SIGNAL_EVENT = 3,

    /// Answers with a single byte identifying which registration was called.
    TEST_COMMAND_IDENTITY = 4,

    /// Succeeds without writing a reply, for requests that expect none.
    TEST_COMMAND_PING = 5,
};

typedef struct
{
    /// Which registration this context belongs to. Written into an `IDENTITY` reply.
    uint32_t id;

    /// The process and event `SIGNAL_EVENT` re-enters the kernel with.
    hos_process_t *process;
    hos_handle_t   event;

    /// Bookkeeping the cases assert on.
    uint32_t calls;
    uint32_t last_command;
    size_t   last_request_size;
    size_t   last_response_capacity;
} test_service_context_t;

/// One context per registration the suite makes, so no case has to share state.
static test_service_context_t g_service_contexts[8];

static error_t
test_service_handler(void *context,
                     uint32_t command,
                     const uint8_t *request,
                     size_t request_size,
                     uint8_t *response,
                     size_t response_capacity,
                     size_t *out_response_size)
{
    test_service_context_t *state = (test_service_context_t *)context;

    state->calls++;
    state->last_command           = command;
    state->last_request_size      = request_size;
    state->last_response_capacity = response_capacity;

    switch (command)
    {
        case TEST_COMMAND_ECHO:
            // A reply that does not fit is refused rather than truncated: a truncated
            // reply the caller cannot detect is worse than a refusal.
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

        case TEST_COMMAND_REFUSE:
            return POUND_ERROR_IO;

        case TEST_COMMAND_SIGNAL_EVENT:
            // Re-enters the kernel from inside a handler. The kernel lock is released
            // around this call, so this must not block.
            return hos_process_event_signal(state->process, state->event);

        case TEST_COMMAND_IDENTITY:
            if (response_capacity < 1U)
            {
                return POUND_ERROR_INVALID_ARGUMENT;
            }

            response[0]        = (uint8_t)state->id;
            *out_response_size = 1U;
            return POUND_SUCCESS;

        case TEST_COMMAND_PING:
            return POUND_SUCCESS;

        default:
            return POUND_ERROR_UNSUPPORTED_INSTRUCTION;
    }
}

// -----------------------------------------------------------------------------
// Machine setup
// -----------------------------------------------------------------------------

static hos_kernel_t  g_kernel;
static hos_process_t g_process;

/// Zeroes and brings up a kernel with one process on it.
static void
machine_reset(void)
{
    memset(&g_kernel, 0, sizeof(g_kernel));
    memset(&g_process, 0, sizeof(g_process));
    memset(g_service_contexts, 0, sizeof(g_service_contexts));

    POUND_REQUIRE(POUND_SUCCESS == hos_kernel_init(&g_kernel));
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create(&g_kernel, &g_process));
}

/// Tears the machine down, so a failure in the body cannot leak into the next case.
static void
machine_teardown(void)
{
    hos_process_close(&g_process);
    hos_kernel_destroy(&g_kernel);
}

/// Registers `name` against the context at `context_index`, returning the error.
static error_t
register_test_service(const char *name, uint32_t context_index)
{
    g_service_contexts[context_index].id = context_index + 1U;
    return hos_kernel_register_service(
        &g_kernel, name, test_service_handler, &g_service_contexts[context_index]);
}

/// Registers the one service most cases need and connects the process to it.
static hos_handle_t
connect_test_service(void)
{
    hos_handle_t session = HOS_HANDLE_INVALID;

    // POUND_CHECK rather than POUND_REQUIRE: this helper returns a value, so it cannot
    // abandon the case from here. A failure is recorded and the returned zero handle
    // makes the case's own assertions fail too, which is the useful first failure.
    POUND_CHECK_EQ_U64(register_test_service("test-srv", 0U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(hos_process_connect(&g_process, "test-srv", &session), POUND_SUCCESS);
    return session;
}

// -----------------------------------------------------------------------------
// Kernel lifecycle
// -----------------------------------------------------------------------------

POUND_TEST(hos_kernel, kernel_lifecycle)
{
    // Deliberately not zeroed: `hos_kernel_init` clears the struct itself, so a caller
    // that declares one on the stack and forgets to initialise it is still correct.
    hos_kernel_t kernel;
    memset(&kernel, 0xAA, sizeof(kernel));

    POUND_CHECK(!hos_kernel_is_initialized(&kernel));
    POUND_CHECK_EQ_U64(hos_kernel_init(&kernel), POUND_SUCCESS);
    POUND_CHECK(hos_kernel_is_initialized(&kernel));
    POUND_CHECK_EQ_U64(hos_kernel_object_count(&kernel), 0U);
    POUND_CHECK_EQ_U64(hos_kernel_service_count(&kernel), 0U);
    POUND_CHECK_EQ_U64(hos_kernel_live_process_count(&kernel), 0U);

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(hos_kernel_init(&kernel), POUND_ERROR_ALREADY_INITIALIZED);
    POUND_CHECK(pound_test_log_contains("already initialised"));

    POUND_CHECK_EQ_U64(hos_kernel_destroy(&kernel), POUND_SUCCESS);
    POUND_CHECK(!hos_kernel_is_initialized(&kernel));
    POUND_CHECK_EQ_U64(hos_kernel_object_count(&kernel), 0U);
}

POUND_TEST(hos_kernel, teardown_is_idempotent_and_reusable)
{
    hos_kernel_t kernel;
    memset(&kernel, 0, sizeof(kernel));

    // Destroying a kernel that was never initialised is a no-op, not an error: the
    // "safe on a zeroed struct" contract is what lets a caller destroy unconditionally.
    POUND_CHECK_EQ_U64(hos_kernel_destroy(&kernel), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(hos_kernel_destroy(&kernel), POUND_SUCCESS);

    POUND_CHECK_EQ_U64(hos_kernel_init(&kernel), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(hos_kernel_destroy(&kernel), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(hos_kernel_destroy(&kernel), POUND_SUCCESS);

    // And the storage is reusable afterwards, which is the property a caller that keeps
    // one kernel in a struct for the lifetime of the emulator depends on.
    POUND_CHECK_EQ_U64(hos_kernel_init(&kernel), POUND_SUCCESS);
    POUND_CHECK(hos_kernel_is_initialized(&kernel));
    POUND_CHECK_EQ_U64(hos_kernel_destroy(&kernel), POUND_SUCCESS);
}

POUND_TEST(hos_kernel, destroy_refuses_while_a_process_is_open)
{
    machine_reset();

    POUND_CHECK_EQ_U64(hos_kernel_live_process_count(&g_kernel), 1U);

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(hos_kernel_destroy(&g_kernel), POUND_ERROR_BUSY);
    POUND_CHECK(pound_test_log_contains("still open"));
    POUND_CHECK(hos_kernel_is_initialized(&g_kernel));

    // The refusal is transient, so the teardown succeeds once the process is closed.
    hos_process_close(&g_process);
    POUND_CHECK_EQ_U64(hos_kernel_live_process_count(&g_kernel), 0U);
    POUND_CHECK_EQ_U64(hos_kernel_destroy(&g_kernel), POUND_SUCCESS);
}

POUND_TEST(hos_kernel, kernel_rejects_null_arguments)
{
    char name[HOS_SERVICE_NAME_MAX + 1U];

    POUND_CHECK_EQ_U64(hos_kernel_init(NULL), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_kernel_destroy(NULL), POUND_ERROR_INVALID_ARGUMENT);

    POUND_CHECK(!hos_kernel_is_initialized(NULL));
    POUND_CHECK_EQ_U64(hos_kernel_object_count(NULL), 0U);
    POUND_CHECK_EQ_U64(hos_kernel_service_count(NULL), 0U);
    POUND_CHECK_EQ_U64(hos_kernel_live_process_count(NULL), 0U);
    POUND_CHECK(!hos_kernel_has_service(NULL, "test-srv"));
    POUND_CHECK(!hos_kernel_service_name_at(NULL, 0U, name));
    POUND_CHECK_EQ_U64(hos_kernel_register_service(NULL, "test-srv", test_service_handler, NULL),
                       POUND_ERROR_INVALID_ARGUMENT);
}

POUND_TEST(hos_kernel, uninitialised_kernel_is_refused)
{
    hos_kernel_t blank;
    char         name[HOS_SERVICE_NAME_MAX + 1U];
    memset(&blank, 0, sizeof(blank));

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(hos_kernel_register_service(&blank, "test-srv", test_service_handler, NULL),
                       POUND_ERROR_NOT_INITIALIZED);
    POUND_CHECK(pound_test_log_contains("never initialised"));

    POUND_CHECK_EQ_U64(hos_kernel_object_count(&blank), 0U);
    POUND_CHECK_EQ_U64(hos_kernel_service_count(&blank), 0U);
    POUND_CHECK_EQ_U64(hos_kernel_live_process_count(&blank), 0U);
    POUND_CHECK(!hos_kernel_has_service(&blank, "test-srv"));
    POUND_CHECK(!hos_kernel_service_name_at(&blank, 0U, name));
}

// -----------------------------------------------------------------------------
// Services
// -----------------------------------------------------------------------------

POUND_TEST(hos_kernel, service_registration)
{
    machine_reset();

    POUND_CHECK(!hos_kernel_has_service(&g_kernel, "fsp-srv"));
    POUND_CHECK_EQ_U64(hos_kernel_service_count(&g_kernel), 0U);

    POUND_CHECK_EQ_U64(register_test_service("fsp-srv", 0U), POUND_SUCCESS);
    POUND_CHECK(hos_kernel_has_service(&g_kernel, "fsp-srv"));
    POUND_CHECK_EQ_U64(hos_kernel_service_count(&g_kernel), 1U);

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(register_test_service("fsp-srv", 1U), POUND_ERROR_ALREADY_INITIALIZED);
    POUND_CHECK(pound_test_log_contains("already registered"));
    POUND_CHECK_EQ_U64(hos_kernel_service_count(&g_kernel), 1U);

    // A name is a namespace key, so it matches exactly or not at all.
    POUND_CHECK(!hos_kernel_has_service(&g_kernel, "fsp-sr"));
    POUND_CHECK(!hos_kernel_has_service(&g_kernel, "fsp-srv2"));

    char name[HOS_SERVICE_NAME_MAX + 1U];
    POUND_CHECK(hos_kernel_service_name_at(&g_kernel, 0U, name));
    POUND_CHECK_STR_EQ(name, "fsp-srv");
    POUND_CHECK(!hos_kernel_service_name_at(&g_kernel, 1U, name));
    POUND_CHECK(!hos_kernel_service_name_at(&g_kernel, 0U, NULL));

    machine_teardown();
}

POUND_TEST(hos_kernel, service_registration_refuses_bad_names_and_handlers)
{
    machine_reset();

    pound_test_log_reset();

    POUND_CHECK_EQ_U64(hos_kernel_register_service(&g_kernel, NULL, test_service_handler, NULL),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_kernel_register_service(&g_kernel, "", test_service_handler, NULL),
                       POUND_ERROR_INVALID_ARGUMENT);

    // Nine characters; the limit is eight, because a Horizon service name is eight bytes
    // on the wire.
    POUND_CHECK_EQ_U64(hos_kernel_register_service(&g_kernel, "012345678", test_service_handler, NULL),
                       POUND_ERROR_INVALID_ARGUMENT);

    POUND_CHECK_EQ_U64(hos_kernel_register_service(&g_kernel, "test-srv", NULL, NULL),
                       POUND_ERROR_INVALID_ARGUMENT);

    POUND_CHECK(pound_test_log_count() > 0U);

    // Eight characters is exactly the limit, so this is the boundary the three refusals
    // above are measured against.
    POUND_CHECK_EQ_U64(hos_kernel_register_service(&g_kernel, "01234567", test_service_handler, NULL),
                       POUND_SUCCESS);
    POUND_CHECK_EQ_U64(hos_kernel_service_count(&g_kernel), 1U);
    POUND_CHECK(hos_kernel_has_service(&g_kernel, "01234567"));

    machine_teardown();
}

POUND_TEST(hos_kernel, service_table_grows_and_keeps_names)
{
    machine_reset();

    // The initial table holds four entries, so six forces at least one growth -- and the
    // growth moves the entries, which is what the name checks below are for.
    for (uint32_t index = 0U; index < 6U; index++)
    {
        char name[HOS_SERVICE_NAME_MAX + 1U];
        snprintf(name, sizeof(name), "svc-%04u", (unsigned int)index);
        POUND_REQUIRE(POUND_SUCCESS == register_test_service(name, index));
    }

    POUND_CHECK_EQ_U64(hos_kernel_service_count(&g_kernel), 6U);

    for (uint32_t index = 0U; index < 6U; index++)
    {
        char expected[HOS_SERVICE_NAME_MAX + 1U];
        char actual[HOS_SERVICE_NAME_MAX + 1U];

        snprintf(expected, sizeof(expected), "svc-%04u", (unsigned int)index);

        POUND_CHECK(hos_kernel_service_name_at(&g_kernel, index, actual));
        POUND_CHECK_STR_EQ(actual, expected);
        POUND_CHECK(hos_kernel_has_service(&g_kernel, expected));
    }

    machine_teardown();
}

POUND_TEST(hos_kernel, session_survives_service_table_growth)
{
    machine_reset();

    // Connect first, then move the service table underneath the session. A session stores
    // the service's index and re-reads the entry on every request, so the handler reached
    // afterwards must still be the one that was connected to; a session that cached the
    // entry's address would call whatever moved into that address instead.
    const hos_handle_t session = connect_test_service();

    for (uint32_t index = 1U; index < 5U; index++)
    {
        char name[HOS_SERVICE_NAME_MAX + 1U];
        snprintf(name, sizeof(name), "svc-%04u", (unsigned int)index);
        POUND_REQUIRE(POUND_SUCCESS == register_test_service(name, index));
    }

    POUND_CHECK_EQ_U64(hos_kernel_service_count(&g_kernel), 5U);

    uint8_t response[1]    = { 0xFFU };
    size_t  response_size  = 0U;

    POUND_CHECK_EQ_U64(hos_process_session_request(
                           &g_process, session, TEST_COMMAND_IDENTITY, NULL, 0U, response, sizeof(response), &response_size),
                       POUND_SUCCESS);
    POUND_CHECK_EQ_U64(response_size, 1U);
    POUND_CHECK_EQ_U64(response[0], 1U); // context 0, which is "test-srv"
    POUND_CHECK_EQ_U64(g_service_contexts[0].calls, 1U);

    machine_teardown();
}

POUND_TEST(hos_kernel, absent_service_lookups_are_silent)
{
    machine_reset();
    pound_test_log_reset();

    POUND_CHECK(!hos_kernel_has_service(&g_kernel, "nosuchsv"));
    POUND_CHECK_EQ_U64(hos_kernel_service_count(&g_kernel), 0U);

    hos_handle_t handle = HOS_HANDLE_INVALID;
    POUND_CHECK_EQ_U64(hos_process_connect(&g_process, "nosuchsv", &handle), POUND_ERROR_NOT_FOUND);
    POUND_CHECK_EQ_U64(handle, HOS_HANDLE_INVALID);
    POUND_CHECK_EQ_U64(hos_process_handle_count(&g_process), 0U);

    // POUND_ERROR_NOT_FOUND is a question rather than a failure, so the whole probe --
    // lookup included -- must leave the log empty.
    POUND_CHECK_EQ_U64(pound_test_log_count(), 0U);

    machine_teardown();
}

// -----------------------------------------------------------------------------
// Handles
// -----------------------------------------------------------------------------

POUND_TEST(hos_kernel, connect_returns_a_session_handle)
{
    machine_reset();

    hos_handle_t session = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS == register_test_service("test-srv", 0U));
    POUND_CHECK_EQ_U64(hos_process_connect(&g_process, "test-srv", &session), POUND_SUCCESS);
    POUND_CHECK(session != HOS_HANDLE_INVALID);
    POUND_CHECK_EQ_U64(hos_process_handle_count(&g_process), 1U);
    POUND_CHECK_EQ_U64(hos_kernel_object_count(&g_kernel), 1U);

    hos_object_type_t type = HOS_OBJECT_EVENT;
    POUND_CHECK_EQ_U64(hos_process_handle_type(&g_process, session, &type), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(type, HOS_OBJECT_SESSION);

    machine_teardown();
}

POUND_TEST(hos_kernel, connect_rejects_bad_arguments)
{
    machine_reset();

    hos_handle_t session = HOS_HANDLE_INVALID;

    POUND_CHECK_EQ_U64(hos_process_connect(NULL, "test-srv", &session), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_process_connect(&g_process, "test-srv", NULL), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_process_connect(&g_process, NULL, &session), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_process_connect(&g_process, "", &session), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_process_connect(&g_process, "012345678", &session),
                       POUND_ERROR_INVALID_ARGUMENT);

    // A process that was never created is not an argument error, so it is reported
    // separately from the four above.
    hos_process_t closed;
    memset(&closed, 0, sizeof(closed));
    POUND_CHECK_EQ_U64(hos_process_connect(&closed, "test-srv", &session), POUND_ERROR_NOT_INITIALIZED);

    machine_teardown();
}

POUND_TEST(hos_kernel, handle_type_and_counts)
{
    machine_reset();

    POUND_CHECK_EQ_U64(hos_process_handle_count(&g_process), 0U);

    hos_handle_t session = connect_test_service();
    hos_handle_t event   = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create_event(&g_process, HOS_EVENT_AUTO, false, &event));

    POUND_CHECK_EQ_U64(hos_process_handle_count(&g_process), 2U);
    POUND_CHECK_EQ_U64(hos_kernel_object_count(&g_kernel), 2U);

    hos_object_type_t type = HOS_OBJECT_SESSION;
    POUND_CHECK_EQ_U64(hos_process_handle_type(&g_process, event, &type), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(type, HOS_OBJECT_EVENT);
    POUND_CHECK_EQ_U64(hos_process_handle_type(&g_process, session, &type), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(type, HOS_OBJECT_SESSION);

    POUND_CHECK_EQ_U64(hos_process_handle_type(NULL, event, &type), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_process_handle_type(&g_process, event, NULL), POUND_ERROR_INVALID_ARGUMENT);

    hos_process_t closed;
    memset(&closed, 0, sizeof(closed));
    POUND_CHECK_EQ_U64(hos_process_handle_type(&closed, event, &type), POUND_ERROR_NOT_INITIALIZED);
    POUND_CHECK_EQ_U64(hos_process_handle_count(&closed), 0U);

    machine_teardown();
}

POUND_TEST(hos_kernel, close_handle_advances_the_generation)
{
    machine_reset();

    hos_handle_t first = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create_event(&g_process, HOS_EVENT_AUTO, false, &first));
    POUND_CHECK_EQ_U64(hos_process_handle_count(&g_process), 1U);
    POUND_CHECK_EQ_U64(hos_kernel_object_count(&g_kernel), 1U);

    POUND_CHECK_EQ_U64(hos_process_close_handle(&g_process, first), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(hos_process_handle_count(&g_process), 0U);
    POUND_CHECK_EQ_U64(hos_kernel_object_count(&g_kernel), 0U);

    // The slot the handle named is now free and the next event takes it. The stale handle
    // must not resolve to it: that is the whole reason a handle carries a generation.
    hos_handle_t second = HOS_HANDLE_INVALID;
    POUND_CHECK_EQ_U64(hos_process_create_event(&g_process, HOS_EVENT_AUTO, false, &second), POUND_SUCCESS);
    POUND_CHECK(second != first);
    POUND_CHECK_EQ_U64(hos_kernel_object_count(&g_kernel), 1U);

    pound_test_log_reset();
    hos_object_type_t type       = HOS_OBJECT_EVENT;
    hos_handle_t      stale_copy = HOS_HANDLE_INVALID;
    POUND_CHECK_EQ_U64(hos_process_handle_type(&g_process, first, &type), POUND_ERROR_INVALID_HANDLE);
    POUND_CHECK(pound_test_log_contains("outlived"));
    POUND_CHECK_EQ_U64(hos_process_close_handle(&g_process, first), POUND_ERROR_INVALID_HANDLE);
    POUND_CHECK_EQ_U64(hos_process_duplicate_handle(&g_process, first, &stale_copy),
                       POUND_ERROR_INVALID_HANDLE);

    // Closing twice is refused rather than ignored, so a double close of a live slot is
    // not mistaken for a successful teardown.
    POUND_CHECK_EQ_U64(hos_process_close_handle(&g_process, second), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(hos_process_close_handle(&g_process, second), POUND_ERROR_INVALID_HANDLE);
    POUND_CHECK_EQ_U64(hos_kernel_object_count(&g_kernel), 0U);

    machine_teardown();
}

POUND_TEST(hos_kernel, invalid_handle_values_are_rejected)
{
    machine_reset();

    hos_object_type_t type = HOS_OBJECT_EVENT;

    // Zero is reserved for "no object", so a zeroed handle is a well-defined miss rather
    // than an accidental reference to the first slot.
    POUND_CHECK_EQ_U64(hos_process_handle_type(&g_process, HOS_HANDLE_INVALID, &type),
                       POUND_ERROR_INVALID_HANDLE);
    POUND_CHECK_EQ_U64(hos_process_close_handle(&g_process, HOS_HANDLE_INVALID),
                       POUND_ERROR_INVALID_HANDLE);

    // An index past the end of the table, and a generation that cannot be live.
    POUND_CHECK_EQ_U64(hos_process_handle_type(&g_process, 0x00010000U, &type),
                       POUND_ERROR_INVALID_HANDLE);
    POUND_CHECK_EQ_U64(hos_process_handle_type(&g_process, 0xFFFFFFFFU, &type),
                       POUND_ERROR_INVALID_HANDLE);
    POUND_CHECK_EQ_U64(hos_process_handle_type(&g_process, 0xFFF00001U, &type),
                       POUND_ERROR_INVALID_HANDLE);

    // Every one of those is a bug worth a line, so the capture must not be empty.
    POUND_CHECK(pound_test_log_count() > 0U);

    machine_teardown();
}

POUND_TEST(hos_kernel, duplicate_handle_keeps_the_object_alive)
{
    machine_reset();

    hos_handle_t original = HOS_HANDLE_INVALID;
    hos_handle_t copy     = HOS_HANDLE_INVALID;

    POUND_REQUIRE(POUND_SUCCESS == hos_process_create_event(&g_process, HOS_EVENT_MANUAL, false, &original));
    POUND_CHECK_EQ_U64(hos_kernel_object_count(&g_kernel), 1U);

    POUND_CHECK_EQ_U64(hos_process_duplicate_handle(&g_process, original, &copy), POUND_SUCCESS);
    POUND_CHECK(copy != original);
    POUND_CHECK_EQ_U64(hos_process_handle_count(&g_process), 2U);

    // Two handles, one object: duplication is not a second object.
    POUND_CHECK_EQ_U64(hos_kernel_object_count(&g_kernel), 1U);

    POUND_CHECK_EQ_U64(hos_process_close_handle(&g_process, original), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(hos_kernel_object_count(&g_kernel), 1U);
    POUND_CHECK_EQ_U64(hos_process_handle_count(&g_process), 1U);

    // The surviving handle is fully usable, which is what "kept alive" has to mean.
    POUND_CHECK_EQ_U64(hos_process_event_signal(&g_process, copy), POUND_SUCCESS);
    bool signaled = false;
    POUND_CHECK_EQ_U64(hos_process_event_is_signaled(&g_process, copy, &signaled), POUND_SUCCESS);
    POUND_CHECK(signaled);

    POUND_CHECK_EQ_U64(hos_process_close_handle(&g_process, copy), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(hos_kernel_object_count(&g_kernel), 0U);
    POUND_CHECK_EQ_U64(hos_process_handle_count(&g_process), 0U);

    machine_teardown();
}

POUND_TEST(hos_kernel, cross_process_duplication)
{
    machine_reset();

    hos_process_t other;
    memset(&other, 0, sizeof(other));
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create(&g_kernel, &other));
    POUND_CHECK_EQ_U64(hos_kernel_live_process_count(&g_kernel), 2U);

    hos_handle_t in_source = HOS_HANDLE_INVALID;
    hos_handle_t in_target = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create_event(&g_process, HOS_EVENT_MANUAL, false, &in_source));

    POUND_CHECK_EQ_U64(hos_process_duplicate_handle_into(&g_process, in_source, &other, &in_target),
                       POUND_SUCCESS);
    POUND_CHECK_EQ_U64(hos_kernel_object_count(&g_kernel), 1U);
    POUND_CHECK_EQ_U64(hos_process_handle_count(&other), 1U);

    // The source gives its authority back; the object outlives it because the target
    // still names it.
    POUND_CHECK_EQ_U64(hos_process_close_handle(&g_process, in_source), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(hos_kernel_object_count(&g_kernel), 1U);
    POUND_CHECK_EQ_U64(hos_process_handle_count(&g_process), 0U);

    POUND_CHECK_EQ_U64(hos_process_event_signal(&other, in_target), POUND_SUCCESS);
    bool signaled = false;
    POUND_CHECK_EQ_U64(hos_process_event_is_signaled(&other, in_target, &signaled), POUND_SUCCESS);
    POUND_CHECK(signaled);

    // The target's handle names nothing in the source, which is the property that makes a
    // handle per-process authority rather than a global name.
    hos_object_type_t type = HOS_OBJECT_EVENT;
    POUND_CHECK_EQ_U64(hos_process_handle_type(&g_process, in_target, &type),
                       POUND_ERROR_INVALID_HANDLE);

    POUND_CHECK_EQ_U64(hos_process_close_handle(&other, in_target), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(hos_kernel_object_count(&g_kernel), 0U);

    hos_process_close(&other);
    POUND_CHECK_EQ_U64(hos_kernel_live_process_count(&g_kernel), 1U);

    machine_teardown();
}

POUND_TEST(hos_kernel, duplication_rejects_arguments_and_foreign_kernels)
{
    machine_reset();

    hos_handle_t handle = HOS_HANDLE_INVALID;
    hos_handle_t copy   = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create_event(&g_process, HOS_EVENT_AUTO, false, &handle));

    POUND_CHECK_EQ_U64(hos_process_duplicate_handle_into(NULL, handle, &g_process, &copy),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_process_duplicate_handle_into(&g_process, handle, NULL, &copy),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_process_duplicate_handle_into(&g_process, handle, &g_process, NULL),
                       POUND_ERROR_INVALID_ARGUMENT);

    hos_kernel_t  other_kernel;
    hos_process_t other_process;
    memset(&other_kernel, 0, sizeof(other_kernel));
    memset(&other_process, 0, sizeof(other_process));
    POUND_REQUIRE(POUND_SUCCESS == hos_kernel_init(&other_kernel));
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create(&other_kernel, &other_process));

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(hos_process_duplicate_handle_into(&g_process, handle, &other_process, &copy),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK(pound_test_log_contains("different kernels"));
    POUND_CHECK_EQ_U64(copy, HOS_HANDLE_INVALID);
    POUND_CHECK_EQ_U64(hos_process_handle_count(&other_process), 0U);

    // A closed target is refused before the kernels are compared, so the two failures
    // stay distinguishable.
    hos_process_close(&other_process);
    POUND_CHECK_EQ_U64(hos_process_duplicate_handle_into(&g_process, handle, &other_process, &copy),
                       POUND_ERROR_NOT_INITIALIZED);

    POUND_CHECK_EQ_U64(hos_kernel_destroy(&other_kernel), POUND_SUCCESS);
    machine_teardown();
}

// -----------------------------------------------------------------------------
// Events
// -----------------------------------------------------------------------------

POUND_TEST(hos_kernel, event_auto_reset_consumes_one_signal)
{
    machine_reset();

    hos_handle_t event = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create_event(&g_process, HOS_EVENT_AUTO, false, &event));

    bool signaled   = true;
    bool observed   = false;

    POUND_CHECK_EQ_U64(hos_process_event_is_signaled(&g_process, event, &observed), POUND_SUCCESS);
    POUND_CHECK(!observed);

    // A wait on an unsignalled event is a result, not a failure: "not yet" is what a
    // non-blocking poll is asking for.
    POUND_CHECK_EQ_U64(hos_process_event_wait(&g_process, event, &observed), POUND_SUCCESS);
    POUND_CHECK(!observed);

    POUND_CHECK_EQ_U64(hos_process_event_signal(&g_process, event), POUND_SUCCESS);

    // Reading the state does not consume it; only a wait does.
    POUND_CHECK_EQ_U64(hos_process_event_is_signaled(&g_process, event, &signaled), POUND_SUCCESS);
    POUND_CHECK(signaled);

    POUND_CHECK_EQ_U64(hos_process_event_wait(&g_process, event, &observed), POUND_SUCCESS);
    POUND_CHECK(observed);
    POUND_CHECK_EQ_U64(hos_process_event_is_signaled(&g_process, event, &signaled), POUND_SUCCESS);
    POUND_CHECK(!signaled);

    // An auto-reset event is a signal handed to exactly one waiter.
    POUND_CHECK_EQ_U64(hos_process_event_wait(&g_process, event, &observed), POUND_SUCCESS);
    POUND_CHECK(!observed);

    // Signalling an already-signalled event is not an error and does not queue a second
    // wake: an event is a state, not a counter.
    POUND_CHECK_EQ_U64(hos_process_event_signal(&g_process, event), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(hos_process_event_signal(&g_process, event), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(hos_process_event_wait(&g_process, event, &observed), POUND_SUCCESS);
    POUND_CHECK(observed);
    POUND_CHECK_EQ_U64(hos_process_event_wait(&g_process, event, &observed), POUND_SUCCESS);
    POUND_CHECK(!observed);

    machine_teardown();
}

POUND_TEST(hos_kernel, event_manual_reset_stays_signalled)
{
    machine_reset();

    hos_handle_t event = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create_event(&g_process, HOS_EVENT_MANUAL, false, &event));

    bool observed = false;
    POUND_CHECK_EQ_U64(hos_process_event_wait(&g_process, event, &observed), POUND_SUCCESS);
    POUND_CHECK(!observed);

    POUND_CHECK_EQ_U64(hos_process_event_signal(&g_process, event), POUND_SUCCESS);

    // A manual-reset event is a state, so a wait observes it without consuming it and
    // the next wait sees it too.
    for (uint32_t attempt = 0U; attempt < 3U; attempt++)
    {
        POUND_CHECK_EQ_U64(hos_process_event_wait(&g_process, event, &observed), POUND_SUCCESS);
        POUND_CHECK_MSG(observed, "wait %u saw the event cleared", (unsigned int)attempt);
    }

    POUND_CHECK_EQ_U64(hos_process_event_clear(&g_process, event), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(hos_process_event_wait(&g_process, event, &observed), POUND_SUCCESS);
    POUND_CHECK(!observed);

    machine_teardown();
}

POUND_TEST(hos_kernel, event_can_be_created_signalled)
{
    machine_reset();

    hos_handle_t manual = HOS_HANDLE_INVALID;
    hos_handle_t auto_  = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create_event(&g_process, HOS_EVENT_MANUAL, true, &manual));
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create_event(&g_process, HOS_EVENT_AUTO, true, &auto_));

    bool signaled = false;
    POUND_CHECK_EQ_U64(hos_process_event_is_signaled(&g_process, manual, &signaled), POUND_SUCCESS);
    POUND_CHECK(signaled);
    POUND_CHECK_EQ_U64(hos_process_event_is_signaled(&g_process, auto_, &signaled), POUND_SUCCESS);
    POUND_CHECK(signaled);

    // The initial signal of an auto-reset event is one signal, so it is consumed by the
    // first wait like any other.
    POUND_CHECK_EQ_U64(hos_process_event_wait(&g_process, auto_, &signaled), POUND_SUCCESS);
    POUND_CHECK(signaled);
    POUND_CHECK_EQ_U64(hos_process_event_wait(&g_process, auto_, &signaled), POUND_SUCCESS);
    POUND_CHECK(!signaled);

    POUND_CHECK_EQ_U64(hos_process_event_wait(&g_process, manual, &signaled), POUND_SUCCESS);
    POUND_CHECK(signaled);
    POUND_CHECK_EQ_U64(hos_process_event_wait(&g_process, manual, &signaled), POUND_SUCCESS);
    POUND_CHECK(signaled);

    machine_teardown();
}

POUND_TEST(hos_kernel, event_operations_require_an_event_handle)
{
    machine_reset();

    const hos_handle_t session = connect_test_service();

    pound_test_log_reset();

    POUND_CHECK_EQ_U64(hos_process_event_signal(&g_process, session), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK(pound_test_log_contains("it names a session"));

    POUND_CHECK_EQ_U64(hos_process_event_clear(&g_process, session), POUND_ERROR_INVALID_ARGUMENT);

    bool signaled = false;
    POUND_CHECK_EQ_U64(hos_process_event_wait(&g_process, session, &signaled),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_process_event_is_signaled(&g_process, session, &signaled),
                       POUND_ERROR_INVALID_ARGUMENT);

    machine_teardown();
}

POUND_TEST(hos_kernel, event_rejects_null_arguments)
{
    machine_reset();

    pound_test_log_reset();

    POUND_CHECK_EQ_U64(hos_process_create_event(NULL, HOS_EVENT_AUTO, false, NULL),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK(pound_test_log_count() > 0U);

    hos_handle_t event = HOS_HANDLE_INVALID;
    POUND_CHECK_EQ_U64(hos_process_create_event(&g_process, HOS_EVENT_AUTO, false, NULL),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_process_create_event(&g_process, HOS_EVENT_AUTO, false, &event), POUND_SUCCESS);

    bool signaled = false;
    POUND_CHECK_EQ_U64(hos_process_event_wait(&g_process, event, NULL), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_process_event_wait(NULL, event, &signaled), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_process_event_is_signaled(&g_process, event, NULL),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_process_event_is_signaled(NULL, event, &signaled),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_process_event_signal(NULL, event), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_process_event_clear(NULL, event), POUND_ERROR_INVALID_ARGUMENT);

    // A process that is not open is reported as such rather than as a bad handle, because
    // the handle may be perfectly good and simply not look-up-able here.
    hos_process_t closed;
    memset(&closed, 0, sizeof(closed));
    POUND_CHECK_EQ_U64(hos_process_event_signal(&closed, event), POUND_ERROR_NOT_INITIALIZED);
    POUND_CHECK_EQ_U64(hos_process_close_handle(&closed, event), POUND_ERROR_NOT_INITIALIZED);

    machine_teardown();
}

// -----------------------------------------------------------------------------
// Processes
// -----------------------------------------------------------------------------

POUND_TEST(hos_kernel, process_lifecycle)
{
    hos_kernel_t  kernel;
    hos_process_t process;
    memset(&kernel, 0, sizeof(kernel));
    memset(&process, 0, sizeof(process));

    POUND_CHECK(!hos_process_is_open(&process));
    POUND_CHECK(!hos_process_is_open(NULL));
    POUND_CHECK_EQ_U64(hos_process_handle_count(&process), 0U);

    POUND_REQUIRE(POUND_SUCCESS == hos_kernel_init(&kernel));

    POUND_CHECK_EQ_U64(hos_process_create(NULL, &process), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_process_create(&kernel, NULL), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_process_create(&kernel, &process), POUND_SUCCESS);
    POUND_CHECK(hos_process_is_open(&process));
    POUND_CHECK_EQ_U64(hos_kernel_live_process_count(&kernel), 1U);

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(hos_process_create(&kernel, &process), POUND_ERROR_ALREADY_INITIALIZED);
    POUND_CHECK(pound_test_log_contains("already open"));
    POUND_CHECK_EQ_U64(hos_kernel_live_process_count(&kernel), 1U);

    hos_process_close(&process);
    POUND_CHECK(!hos_process_is_open(&process));
    POUND_CHECK_EQ_U64(hos_kernel_live_process_count(&kernel), 0U);

    // Safe on a zeroed struct and safe to call twice, like the kernel's teardown.
    hos_process_close(&process);
    hos_process_close(NULL);
    POUND_CHECK_EQ_U64(hos_process_handle_count(&process), 0U);

    POUND_CHECK_EQ_U64(hos_kernel_destroy(&kernel), POUND_SUCCESS);
}

POUND_TEST(hos_kernel, process_close_releases_every_handle)
{
    machine_reset();

    hos_handle_t session = connect_test_service();
    hos_handle_t event_a = HOS_HANDLE_INVALID;
    hos_handle_t event_b = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create_event(&g_process, HOS_EVENT_AUTO, false, &event_a));
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create_event(&g_process, HOS_EVENT_MANUAL, true, &event_b));

    POUND_CHECK_EQ_U64(hos_process_handle_count(&g_process), 3U);
    POUND_CHECK_EQ_U64(hos_kernel_object_count(&g_kernel), 3U);

    // Closing the process is the whole namespace going away at once, so every object it
    // held must be released -- including the session, which is a kernel object like any
    // other rather than a pointer the process owns.
    hos_process_close(&g_process);

    POUND_CHECK_EQ_U64(hos_kernel_object_count(&g_kernel), 0U);
    POUND_CHECK_EQ_U64(hos_kernel_live_process_count(&g_kernel), 0U);
    POUND_CHECK(!hos_process_is_open(&g_process));

    // The handles are unreachable now, and the kernel is destroyable, which is the
    // property the count above is standing in for.
    POUND_CHECK_EQ_U64(hos_kernel_destroy(&g_kernel), POUND_SUCCESS);
    POUND_UNUSED(session);
}

// -----------------------------------------------------------------------------
// Sessions
// -----------------------------------------------------------------------------

POUND_TEST(hos_kernel, session_request_routes_command_and_payload)
{
    machine_reset();

    test_service_context_t *state = &g_service_contexts[0];
    const hos_handle_t      session = connect_test_service();

    const uint8_t request[4]  = { 1U, 2U, 3U, 4U };
    uint8_t       response[8] = { 0U };
    size_t        response_size = 0U;

    POUND_CHECK_EQ_U64(hos_process_session_request(&g_process,
                                                   session,
                                                   TEST_COMMAND_ECHO,
                                                   request,
                                                   sizeof(request),
                                                   response,
                                                   sizeof(response),
                                                   &response_size),
                       POUND_SUCCESS);

    POUND_CHECK_EQ_U64(response_size, sizeof(request));
    POUND_CHECK(memcmp(request, response, sizeof(request)) == 0);
    POUND_CHECK_EQ_U64(state->calls, 1U);
    POUND_CHECK_EQ_U64(state->last_command, TEST_COMMAND_ECHO);
    POUND_CHECK_EQ_U64(state->last_request_size, sizeof(request));
    POUND_CHECK_EQ_U64(state->last_response_capacity, sizeof(response));

    // A request that expects no reply is allowed to describe no buffer at all.
    response_size = 99U;
    POUND_CHECK_EQ_U64(hos_process_session_request(
                           &g_process, session, TEST_COMMAND_PING, NULL, 0U, NULL, 0U, &response_size),
                       POUND_SUCCESS);
    POUND_CHECK_EQ_U64(response_size, 0U);
    POUND_CHECK_EQ_U64(state->calls, 2U);

    machine_teardown();
}

POUND_TEST(hos_kernel, session_request_propagates_handler_errors)
{
    machine_reset();

    const hos_handle_t session = connect_test_service();
    size_t             response_size = 99U;

    // Whatever a handler returns is what the caller sees; this layer does not reinterpret
    // it, which is what lets a service report a Horizon result code straight through.
    POUND_CHECK_EQ_U64(hos_process_session_request(
                           &g_process, session, TEST_COMMAND_REFUSE, NULL, 0U, NULL, 0U, &response_size),
                       POUND_ERROR_IO);
    POUND_CHECK_EQ_U64(response_size, 0U);

    POUND_CHECK_EQ_U64(g_service_contexts[0].calls, 1U);

    machine_teardown();
}

POUND_TEST(hos_kernel, session_request_refuses_a_reply_that_does_not_fit)
{
    machine_reset();

    const hos_handle_t session = connect_test_service();

    const uint8_t request[4]    = { 1U, 2U, 3U, 4U };
    uint8_t       response[2]   = { 0U, 0U };
    size_t        response_size = 99U;

    // The handler is handed the buffer's capacity and refuses rather than truncating, and
    // the kernel zeroes the reported size before the call so a refused reply cannot be
    // mistaken for a short one.
    POUND_CHECK_EQ_U64(hos_process_session_request(&g_process,
                                                   session,
                                                   TEST_COMMAND_ECHO,
                                                   request,
                                                   sizeof(request),
                                                   response,
                                                   sizeof(response),
                                                   &response_size),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(response_size, 0U);
    POUND_CHECK_EQ_U64(g_service_contexts[0].last_response_capacity, sizeof(response));

    machine_teardown();
}

POUND_TEST(hos_kernel, session_operations_require_a_session_handle)
{
    machine_reset();

    hos_handle_t event = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create_event(&g_process, HOS_EVENT_AUTO, false, &event));

    pound_test_log_reset();

    size_t response_size = 0U;
    POUND_CHECK_EQ_U64(hos_process_session_request(
                           &g_process, event, TEST_COMMAND_ECHO, NULL, 0U, NULL, 0U, &response_size),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK(pound_test_log_contains("it names an event"));

    POUND_CHECK_EQ_U64(response_size, 0U);

    machine_teardown();
}

POUND_TEST(hos_kernel, session_request_rejects_bad_arguments)
{
    machine_reset();

    const hos_handle_t session = connect_test_service();

    const uint8_t request[1] = { 1U };
    size_t        response_size = 0U;

    POUND_CHECK_EQ_U64(hos_process_session_request(
                           NULL, session, TEST_COMMAND_ECHO, NULL, 0U, NULL, 0U, &response_size),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_process_session_request(
                           &g_process, session, TEST_COMMAND_ECHO, NULL, 0U, NULL, 0U, NULL),
                       POUND_ERROR_INVALID_ARGUMENT);

    // A size without a buffer is a caller bug, and it is caught here rather than left for
    // the handler to read past the end of.
    POUND_CHECK_EQ_U64(hos_process_session_request(
                           &g_process, session, TEST_COMMAND_ECHO, NULL, 1U, NULL, 0U, &response_size),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_process_session_request(&g_process,
                                                   session,
                                                   TEST_COMMAND_ECHO,
                                                   request,
                                                   sizeof(request),
                                                   NULL,
                                                   1U,
                                                   &response_size),
                       POUND_ERROR_INVALID_ARGUMENT);

    // None of those reached the handler.
    POUND_CHECK_EQ_U64(g_service_contexts[0].calls, 0U);

    hos_process_t closed;
    memset(&closed, 0, sizeof(closed));
    POUND_CHECK_EQ_U64(hos_process_session_request(
                           &closed, session, TEST_COMMAND_ECHO, NULL, 0U, NULL, 0U, &response_size),
                       POUND_ERROR_NOT_INITIALIZED);

    machine_teardown();
}

POUND_TEST(hos_kernel, handler_may_re_enter_the_kernel)
{
    machine_reset();

    test_service_context_t *state = &g_service_contexts[0];

    hos_handle_t event = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create_event(&g_process, HOS_EVENT_MANUAL, false, &event));

    const hos_handle_t session = connect_test_service();

    // The handler signals this event from inside the call. A kernel lock that was still
    // held across the handler would make this block forever, so the return value is not
    // the only thing under test here: arriving at the assertion at all is half of it.
    state->process = &g_process;
    state->event   = event;

    bool signaled = true;
    POUND_CHECK_EQ_U64(hos_process_event_is_signaled(&g_process, event, &signaled), POUND_SUCCESS);
    POUND_CHECK(!signaled);

    size_t response_size = 0U;
    POUND_CHECK_EQ_U64(hos_process_session_request(
                           &g_process, session, TEST_COMMAND_SIGNAL_EVENT, NULL, 0U, NULL, 0U, &response_size),
                       POUND_SUCCESS);
    POUND_CHECK_EQ_U64(response_size, 0U);

    POUND_CHECK_EQ_U64(hos_process_event_is_signaled(&g_process, event, &signaled), POUND_SUCCESS);
    POUND_CHECK(signaled);

    machine_teardown();
}

// -----------------------------------------------------------------------------
// Threads and waits
// -----------------------------------------------------------------------------

/// Builds a thread state whose registers make it obvious in an assertion.
static guest_state_t
thread_state(uint64_t entry, uint64_t argument, uint64_t stack)
{
    guest_state_t state;

    POUND_CHECK_EQ_U64(guest_state_init(&state), POUND_SUCCESS);

    state.pc    = entry;
    state.x[0]  = argument;
    state.x[31] = stack;
    return state;
}

/// Creates a thread on the shared process, checking that it worked.
///
/// POUND_CHECK rather than POUND_REQUIRE, because this returns a value; a failure leaves
/// the caller with the invalid handle, which fails its own assertions too.
static hos_handle_t
create_test_thread(uint64_t entry, uint32_t priority)
{
    const guest_state_t initial = thread_state(entry, 0x42U, 0x8000U);
    hos_handle_t        thread  = HOS_HANDLE_INVALID;

    POUND_CHECK_EQ_U64(hos_process_create_thread(&g_process, &initial, priority, &thread),
                       POUND_SUCCESS);
    return thread;
}

POUND_TEST(hos_kernel, thread_creation_and_identity)
{
    machine_reset();

    const guest_state_t initial = thread_state(0x1000U, 0x42U, 0x8000U);
    hos_handle_t        thread  = HOS_HANDLE_INVALID;

    POUND_CHECK_EQ_U64(
        hos_process_create_thread(&g_process, &initial, HOS_THREAD_PRIORITY_DEFAULT, &thread),
        POUND_SUCCESS);
    POUND_CHECK(thread != HOS_HANDLE_INVALID);
    POUND_CHECK_EQ_U64(hos_kernel_thread_count(&g_kernel), 1U);

    hos_object_type_t type = HOS_OBJECT_EVENT;
    POUND_CHECK_EQ_U64(hos_process_handle_type(&g_process, thread, &type), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(type, HOS_OBJECT_THREAD);

    uint64_t id = 0U;
    POUND_CHECK_EQ_U64(hos_process_thread_id(&g_process, thread, &id), POUND_SUCCESS);
    POUND_CHECK(id != 0U);

    hos_thread_status_t status = HOS_THREAD_STARTED;
    POUND_CHECK_EQ_U64(hos_kernel_thread_status(&g_kernel, id, &status), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(status, HOS_THREAD_CREATED);

    uint32_t priority = 0U;
    POUND_CHECK_EQ_U64(hos_kernel_thread_priority(&g_kernel, id, &priority), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(priority, HOS_THREAD_PRIORITY_DEFAULT);

    // The initial state was copied, not retained: the thread's register file is the one the
    // caller built, and it says so.
    guest_state_t loaded;
    POUND_CHECK_EQ_U64(hos_kernel_thread_context(&g_kernel, id, &loaded), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(loaded.pc, 0x1000U);
    POUND_CHECK_EQ_U64(loaded.x[0], 0x42U);
    POUND_CHECK_EQ_U64(loaded.x[31], 0x8000U);

    // And a thread identifier is not a handle: the kernel-wide lookups take the identifier,
    // and one nothing has answers not-found rather than reaching the thread whose handle
    // happens to share the number.
    hos_thread_status_t other = HOS_THREAD_CREATED;
    POUND_CHECK_EQ_U64(hos_kernel_thread_status(&g_kernel, id + 1000U, &other), POUND_ERROR_NOT_FOUND);

    machine_teardown();
}

POUND_TEST(hos_kernel, thread_start_and_terminate)
{
    machine_reset();

    const hos_handle_t thread = create_test_thread(0x2000U, HOS_THREAD_PRIORITY_DEFAULT);

    uint64_t thread_id = 0U;
    POUND_CHECK_EQ_U64(hos_process_thread_id(&g_process, thread, &thread_id), POUND_SUCCESS);

    // Terminating a thread that was never started is refused rather than quietly allowed:
    // a caller that means to finish something must not be told it did when it did not.
    POUND_CHECK_EQ_U64(hos_kernel_thread_terminate(&g_kernel, thread_id), POUND_ERROR_INVALID_ARGUMENT);

    POUND_CHECK_EQ_U64(hos_kernel_thread_start(&g_kernel, thread_id), POUND_SUCCESS);

    hos_thread_status_t status = HOS_THREAD_CREATED;
    POUND_CHECK_EQ_U64(hos_kernel_thread_status(&g_kernel, thread_id, &status), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(status, HOS_THREAD_STARTED);

    // Starting twice is refused, which is the error the syscall layer turns into Horizon's
    // invalid-state Result after checking the status itself.
    POUND_CHECK_EQ_U64(hos_kernel_thread_start(&g_kernel, thread_id), POUND_ERROR_INVALID_ARGUMENT);

    POUND_CHECK_EQ_U64(hos_kernel_thread_terminate(&g_kernel, thread_id), POUND_SUCCESS);

    POUND_CHECK_EQ_U64(hos_kernel_thread_status(&g_kernel, thread_id, &status), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(status, HOS_THREAD_TERMINATED);

    // The second terminate is a success: the thread exiting and its process closing both
    // lead here, and the second of them must not be an error.
    POUND_CHECK_EQ_U64(hos_kernel_thread_terminate(&g_kernel, thread_id), POUND_SUCCESS);

    // A terminated thread that a handle still names can still be inspected, and starting it
    // again is refused rather than resurrecting it.
    POUND_CHECK_EQ_U64(hos_kernel_thread_start(&g_kernel, thread_id), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_kernel_thread_count(&g_kernel), 1U);

    machine_teardown();
}

POUND_TEST(hos_kernel, thread_cancel_request_is_a_consumed_flag)
{
    machine_reset();

    const hos_handle_t thread = create_test_thread(0x4000U, HOS_THREAD_PRIORITY_DEFAULT);

    uint64_t thread_id = 0U;
    POUND_CHECK_EQ_U64(hos_process_thread_id(&g_process, thread, &thread_id), POUND_SUCCESS);

    // Asking before anything was requested is not an error: the flag is simply clear, and
    // reading it clears nothing because there was nothing to clear.
    bool requested = true;
    POUND_CHECK_EQ_U64(hos_kernel_thread_take_cancel(&g_kernel, thread_id, &requested), POUND_SUCCESS);
    POUND_CHECK(!requested);

    // A request is a flag, not a count. Two requests before the thread synchronizes are one
    // interruption, because Horizon keeps a `bool` and not a queue.
    POUND_CHECK_EQ_U64(hos_kernel_thread_request_cancel(&g_kernel, thread_id), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(hos_kernel_thread_request_cancel(&g_kernel, thread_id), POUND_SUCCESS);

    POUND_CHECK_EQ_U64(hos_kernel_thread_take_cancel(&g_kernel, thread_id, &requested), POUND_SUCCESS);
    POUND_CHECK(requested);
    POUND_CHECK_EQ_U64(hos_kernel_thread_take_cancel(&g_kernel, thread_id, &requested), POUND_SUCCESS);
    POUND_CHECK(!requested);

    // The flag lives on the thread object, so it survives the thread starting and is there
    // for the first synchronization call it makes.
    POUND_CHECK_EQ_U64(hos_kernel_thread_request_cancel(&g_kernel, thread_id), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(hos_kernel_thread_start(&g_kernel, thread_id), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(hos_kernel_thread_take_cancel(&g_kernel, thread_id, &requested), POUND_SUCCESS);
    POUND_CHECK(requested);

    // A miss is an error rather than a silent "not requested": a request for a thread that
    // does not exist must not look like one that was accepted.
    POUND_CHECK_EQ_U64(hos_kernel_thread_request_cancel(&g_kernel, thread_id + 1000U), POUND_ERROR_NOT_FOUND);
    POUND_CHECK_EQ_U64(hos_kernel_thread_take_cancel(&g_kernel, thread_id + 1000U, &requested),
                       POUND_ERROR_NOT_FOUND);
    POUND_CHECK_EQ_U64(hos_kernel_thread_take_cancel(&g_kernel, thread_id, NULL), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_kernel_thread_request_cancel(NULL, thread_id), POUND_ERROR_INVALID_ARGUMENT);

    machine_teardown();
}

POUND_TEST(hos_kernel, thread_outlives_its_last_handle)
{
    machine_reset();

    const hos_handle_t thread = create_test_thread(0x3000U, HOS_THREAD_PRIORITY_DEFAULT);

    uint64_t thread_id = 0U;
    POUND_CHECK_EQ_U64(hos_process_thread_id(&g_process, thread, &thread_id), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(hos_kernel_thread_start(&g_kernel, thread_id), POUND_SUCCESS);

    // The title closes its handle while the thread is still running. Horizon allows it, and
    // the thread keeps running: the kernel holds the object open on the thread's own behalf,
    // so the register file cannot be freed under a thread that still has instructions to run.
    POUND_CHECK_EQ_U64(hos_process_close_handle(&g_process, thread), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(hos_kernel_thread_count(&g_kernel), 1U);

    hos_thread_status_t status = HOS_THREAD_CREATED;
    POUND_CHECK_EQ_U64(hos_kernel_thread_status(&g_kernel, thread_id, &status), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(status, HOS_THREAD_STARTED);

    guest_state_t loaded;
    POUND_CHECK_EQ_U64(hos_kernel_thread_context(&g_kernel, thread_id, &loaded), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(loaded.pc, 0x3000U);

    // Finishing it by identifier still works, and only then is the object gone: the last
    // reference went with the thread rather than with the handle.
    POUND_CHECK_EQ_U64(hos_kernel_thread_terminate(&g_kernel, thread_id), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(hos_kernel_thread_count(&g_kernel), 0U);
    POUND_CHECK_EQ_U64(hos_kernel_thread_status(&g_kernel, thread_id, &status), POUND_ERROR_NOT_FOUND);

    // Identifiers are never reused, so the one that has gone stays gone even as a later
    // thread is created.
    const hos_handle_t next      = create_test_thread(0x4000U, HOS_THREAD_PRIORITY_DEFAULT);
    uint64_t           next_id   = 0U;
    POUND_CHECK_EQ_U64(hos_process_thread_id(&g_process, next, &next_id), POUND_SUCCESS);
    POUND_CHECK(next_id > thread_id);

    machine_teardown();
}

POUND_TEST(hos_kernel, thread_arguments_are_validated)
{
    machine_reset();

    const guest_state_t initial = thread_state(0x5000U, 0U, 0U);
    hos_handle_t        thread  = HOS_HANDLE_INVALID;

    POUND_CHECK_EQ_U64(hos_process_create_thread(NULL, &initial, HOS_THREAD_PRIORITY_DEFAULT, &thread),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_process_create_thread(&g_process, NULL, HOS_THREAD_PRIORITY_DEFAULT, &thread),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_process_create_thread(&g_process, &initial, HOS_THREAD_PRIORITY_DEFAULT, NULL),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_process_create_thread(&g_process,
                                                 &initial,
                                                 HOS_THREAD_PRIORITY_LOWEST + 1U,
                                                 &thread),
                       POUND_ERROR_INVALID_ARGUMENT);

    // Both ends of the range are legal.
    hos_handle_t highest = HOS_HANDLE_INVALID;
    hos_handle_t lowest  = HOS_HANDLE_INVALID;

    POUND_CHECK_EQ_U64(
        hos_process_create_thread(&g_process, &initial, HOS_THREAD_PRIORITY_HIGHEST, &highest),
        POUND_SUCCESS);
    POUND_CHECK_EQ_U64(
        hos_process_create_thread(&g_process, &initial, HOS_THREAD_PRIORITY_LOWEST, &lowest),
        POUND_SUCCESS);

    // An event is not a thread, so resolving one refuses rather than answering with a
    // garbage identifier.
    hos_handle_t event = HOS_HANDLE_INVALID;
    POUND_CHECK_EQ_U64(hos_process_create_event(&g_process, HOS_EVENT_AUTO, false, &event), POUND_SUCCESS);

    uint64_t id = 0U;
    POUND_CHECK_EQ_U64(hos_process_thread_id(&g_process, event, &id), POUND_ERROR_INVALID_ARGUMENT);

    POUND_CHECK_EQ_U64(hos_process_thread_id(NULL, event, &id), POUND_ERROR_INVALID_ARGUMENT);

    // A kernel-level lookup for an identifier nothing has answers not-found, and does not log
    // it: that is the answer the scheduler asks for constantly.
    hos_thread_status_t status = HOS_THREAD_CREATED;

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(hos_kernel_thread_status(&g_kernel, 0xDEADU, &status), POUND_ERROR_NOT_FOUND);
    POUND_CHECK_EQ_U64(pound_test_log_count(), 0U);

    POUND_CHECK_EQ_U64(hos_kernel_thread_context(&g_kernel, 0xDEADU, NULL), POUND_ERROR_INVALID_ARGUMENT);

    machine_teardown();
}

POUND_TEST(hos_kernel, thread_priorities_are_settable_while_terminated)
{
    machine_reset();

    const hos_handle_t thread = create_test_thread(0x6000U, HOS_THREAD_PRIORITY_DEFAULT);

    uint64_t thread_id = 0U;
    POUND_CHECK_EQ_U64(hos_process_thread_id(&g_process, thread, &thread_id), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(hos_kernel_thread_start(&g_kernel, thread_id), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(hos_kernel_thread_terminate(&g_kernel, thread_id), POUND_SUCCESS);

    // Horizon keeps the priority a finished thread held, and a title that adjusts a thread
    // it has already joined is not doing anything wrong.
    POUND_CHECK_EQ_U64(hos_kernel_thread_set_priority(&g_kernel, thread_id, HOS_THREAD_PRIORITY_HIGHEST),
                       POUND_SUCCESS);

    uint32_t priority = 0U;
    POUND_CHECK_EQ_U64(hos_kernel_thread_priority(&g_kernel, thread_id, &priority), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(priority, HOS_THREAD_PRIORITY_HIGHEST);

    POUND_CHECK_EQ_U64(
        hos_kernel_thread_set_priority(&g_kernel, thread_id, HOS_THREAD_PRIORITY_LOWEST + 1U),
        POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_kernel_thread_set_priority(&g_kernel, 0xDEADU, HOS_THREAD_PRIORITY_DEFAULT),
                       POUND_ERROR_NOT_FOUND);
    POUND_CHECK_EQ_U64(hos_kernel_thread_priority(&g_kernel, thread_id, NULL), POUND_ERROR_INVALID_ARGUMENT);

    machine_teardown();
}

POUND_TEST(hos_kernel, wait_any_reports_the_first_ready_handle)
{
    machine_reset();

    hos_handle_t automatic = HOS_HANDLE_INVALID;
    hos_handle_t manual    = HOS_HANDLE_INVALID;

    POUND_CHECK_EQ_U64(hos_process_create_event(&g_process, HOS_EVENT_AUTO, false, &automatic),
                       POUND_SUCCESS);
    POUND_CHECK_EQ_U64(hos_process_create_event(&g_process, HOS_EVENT_MANUAL, true, &manual),
                       POUND_SUCCESS);

    const hos_handle_t handles[2] = {automatic, manual};

    uint32_t index = 0xFFFFU;
    bool     ready = false;

    // The manual event is the second handle and the only one set, so the second is the one
    // reported. It stays set, so a second wait says the same thing.
    POUND_CHECK_EQ_U64(hos_process_wait_any(&g_process, handles, 2U, &index, &ready), POUND_SUCCESS);
    POUND_CHECK(ready);
    POUND_CHECK_EQ_U64(index, 1U);

    POUND_CHECK_EQ_U64(hos_process_wait_any(&g_process, handles, 2U, &index, &ready), POUND_SUCCESS);
    POUND_CHECK(ready);
    POUND_CHECK_EQ_U64(index, 1U);

    // Now the auto-reset event is signalled too, and it wins by being first -- and the wait
    // consumes it, so the next wait falls back to the manual event.
    POUND_CHECK_EQ_U64(hos_process_event_signal(&g_process, automatic), POUND_SUCCESS);

    POUND_CHECK_EQ_U64(hos_process_wait_any(&g_process, handles, 2U, &index, &ready), POUND_SUCCESS);
    POUND_CHECK(ready);
    POUND_CHECK_EQ_U64(index, 0U);

    POUND_CHECK_EQ_U64(hos_process_wait_any(&g_process, handles, 2U, &index, &ready), POUND_SUCCESS);
    POUND_CHECK(ready);
    POUND_CHECK_EQ_U64(index, 1U);

    machine_teardown();
}

POUND_TEST(hos_kernel, wait_any_never_reports_a_running_thread)
{
    machine_reset();

    const hos_handle_t thread = create_test_thread(0x7000U, HOS_THREAD_PRIORITY_DEFAULT);

    uint64_t thread_id = 0U;
    POUND_CHECK_EQ_U64(hos_process_thread_id(&g_process, thread, &thread_id), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(hos_kernel_thread_start(&g_kernel, thread_id), POUND_SUCCESS);

    // A join is a level: waiting on a running thread is simply not ready yet.
    uint32_t index = 0xFFFFU;
    bool     ready = true;

    POUND_CHECK_EQ_U64(hos_process_wait_any(&g_process, &thread, 1U, &index, &ready), POUND_SUCCESS);
    POUND_CHECK(!ready);

    POUND_CHECK_EQ_U64(hos_kernel_thread_terminate(&g_kernel, thread_id), POUND_SUCCESS);

    // Once it has finished, the same wait is ready -- without anything having signalled
    // anything, which is the whole reason nothing here needs a wakeup path.
    POUND_CHECK_EQ_U64(hos_process_wait_any(&g_process, &thread, 1U, &index, &ready), POUND_SUCCESS);
    POUND_CHECK(ready);
    POUND_CHECK_EQ_U64(index, 0U);

    machine_teardown();
}

POUND_TEST(hos_kernel, wait_any_rejects_bad_arguments)
{
    machine_reset();

    hos_handle_t automatic = HOS_HANDLE_INVALID;
    POUND_CHECK_EQ_U64(hos_process_create_event(&g_process, HOS_EVENT_AUTO, false, &automatic),
                       POUND_SUCCESS);

    uint32_t index = 0U;
    bool     ready = false;

    POUND_CHECK_EQ_U64(hos_process_wait_any(&g_process, &automatic, 0U, &index, &ready),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_process_wait_any(&g_process,
                                            &automatic,
                                            HOS_MAX_WAIT_HANDLES + 1U,
                                            &index,
                                            &ready),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_process_wait_any(NULL, &automatic, 1U, &index, &ready),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_process_wait_any(&g_process, NULL, 1U, &index, &ready),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_process_wait_any(&g_process, &automatic, 1U, NULL, &ready),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_process_wait_any(&g_process, &automatic, 1U, &index, NULL),
                       POUND_ERROR_INVALID_ARGUMENT);

    // A handle that names nothing makes the whole wait unsatisfiable, which is a failure of
    // the call rather than a handle to skip past: a title waiting on a set is waiting on all
    // of it, and answering "not ready" would park it forever on a handle it cannot use.
    const hos_handle_t dead = 0x00100001U;
    POUND_CHECK_EQ_U64(hos_process_wait_any(&g_process, &dead, 1U, &index, &ready),
                       POUND_ERROR_INVALID_HANDLE);

    machine_teardown();
}

POUND_TEST(hos_kernel, process_close_terminates_its_threads)
{
    machine_reset();

    const hos_handle_t thread = create_test_thread(0x8000U, HOS_THREAD_PRIORITY_DEFAULT);

    uint64_t thread_id = 0U;
    POUND_CHECK_EQ_U64(hos_process_thread_id(&g_process, thread, &thread_id), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(hos_kernel_thread_start(&g_kernel, thread_id), POUND_SUCCESS);

    // Closing the process takes the thread's own reference with it, so the object does not
    // survive as a thread nothing can run or finish -- which is what a scheduler holding it
    // by identifier would otherwise be left watching.
    hos_process_close(&g_process);
    POUND_CHECK_EQ_U64(hos_kernel_thread_count(&g_kernel), 0U);

    hos_thread_status_t status = HOS_THREAD_STARTED;
    POUND_CHECK_EQ_U64(hos_kernel_thread_status(&g_kernel, thread_id, &status), POUND_ERROR_NOT_FOUND);

    // And the kernel can be torn down: nothing is left holding it open.
    POUND_CHECK_EQ_U64(hos_kernel_destroy(&g_kernel), POUND_SUCCESS);
}

// -----------------------------------------------------------------------------

POUND_TEST_SUITE(hos_kernel,
                 POUND_TEST_CASE(hos_kernel, kernel_lifecycle),
                 POUND_TEST_CASE(hos_kernel, teardown_is_idempotent_and_reusable),
                 POUND_TEST_CASE(hos_kernel, destroy_refuses_while_a_process_is_open),
                 POUND_TEST_CASE(hos_kernel, kernel_rejects_null_arguments),
                 POUND_TEST_CASE(hos_kernel, uninitialised_kernel_is_refused),
                 POUND_TEST_CASE(hos_kernel, service_registration),
                 POUND_TEST_CASE(hos_kernel, service_registration_refuses_bad_names_and_handlers),
                 POUND_TEST_CASE(hos_kernel, service_table_grows_and_keeps_names),
                 POUND_TEST_CASE(hos_kernel, session_survives_service_table_growth),
                 POUND_TEST_CASE(hos_kernel, absent_service_lookups_are_silent),
                 POUND_TEST_CASE(hos_kernel, connect_returns_a_session_handle),
                 POUND_TEST_CASE(hos_kernel, connect_rejects_bad_arguments),
                 POUND_TEST_CASE(hos_kernel, handle_type_and_counts),
                 POUND_TEST_CASE(hos_kernel, close_handle_advances_the_generation),
                 POUND_TEST_CASE(hos_kernel, invalid_handle_values_are_rejected),
                 POUND_TEST_CASE(hos_kernel, duplicate_handle_keeps_the_object_alive),
                 POUND_TEST_CASE(hos_kernel, cross_process_duplication),
                 POUND_TEST_CASE(hos_kernel, duplication_rejects_arguments_and_foreign_kernels),
                 POUND_TEST_CASE(hos_kernel, event_auto_reset_consumes_one_signal),
                 POUND_TEST_CASE(hos_kernel, event_manual_reset_stays_signalled),
                 POUND_TEST_CASE(hos_kernel, event_can_be_created_signalled),
                 POUND_TEST_CASE(hos_kernel, event_operations_require_an_event_handle),
                 POUND_TEST_CASE(hos_kernel, event_rejects_null_arguments),
                 POUND_TEST_CASE(hos_kernel, process_lifecycle),
                 POUND_TEST_CASE(hos_kernel, process_close_releases_every_handle),
                 POUND_TEST_CASE(hos_kernel, session_request_routes_command_and_payload),
                 POUND_TEST_CASE(hos_kernel, session_request_propagates_handler_errors),
                 POUND_TEST_CASE(hos_kernel, session_request_refuses_a_reply_that_does_not_fit),
                 POUND_TEST_CASE(hos_kernel, session_operations_require_a_session_handle),
                 POUND_TEST_CASE(hos_kernel, session_request_rejects_bad_arguments),
                 POUND_TEST_CASE(hos_kernel, handler_may_re_enter_the_kernel),
                 POUND_TEST_CASE(hos_kernel, thread_creation_and_identity),
                 POUND_TEST_CASE(hos_kernel, thread_start_and_terminate),
                 POUND_TEST_CASE(hos_kernel, thread_outlives_its_last_handle),
                 POUND_TEST_CASE(hos_kernel, thread_arguments_are_validated),
                 POUND_TEST_CASE(hos_kernel, thread_priorities_are_settable_while_terminated),
                 POUND_TEST_CASE(hos_kernel, thread_cancel_request_is_a_consumed_flag),
                 POUND_TEST_CASE(hos_kernel, wait_any_reports_the_first_ready_handle),
                 POUND_TEST_CASE(hos_kernel, wait_any_never_reports_a_running_thread),
                 POUND_TEST_CASE(hos_kernel, wait_any_rejects_bad_arguments),
                 POUND_TEST_CASE(hos_kernel, process_close_terminates_its_threads))

/*** end of file ***/

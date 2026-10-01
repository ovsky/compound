//! Unit tests for `guest_state.h`.
//!
//! `guest_state_t` is the emulated CPU register file. Ballistic loads and
//! stores it directly, so its layout, size and initial contents are part of
//! the ABI between the recompiler and the guest CPU implementation.

#include "pound_test.h"

#include "guest_state.h"
#include <stddef.h>
#include <string.h>

POUND_TEST(guest_state, init_rejects_null)
{
    pound_test_log_reset();

    POUND_CHECK_EQ_I64(guest_state_init(NULL), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(pound_test_log_count_at(LOG_LEVEL_ERROR), 1U);
    POUND_CHECK(pound_test_log_contains("state is NULL"));
}

POUND_TEST(guest_state, init_zeroes_the_entire_register_file)
{
    guest_state_t state;

    // Poison the whole struct first so a partial initialisation is detectable.
    memset(&state, 0xCD, sizeof(guest_state_t));

    POUND_REQUIRE(POUND_SUCCESS == guest_state_init(&state));

    for (size_t i = 0; i < GUEST_STATE_REGISTER_COUNT; ++i)
    {
        POUND_CHECK_EQ_U64(state.x[i], 0U);
    }

    POUND_CHECK_EQ_U64(state.pc, 0U);
    POUND_CHECK_EQ_U64(state.flag_n, 0U);
    POUND_CHECK_EQ_U64(state.flag_z, 0U);
    POUND_CHECK_EQ_U64(state.flag_c, 0U);
    POUND_CHECK_EQ_U64(state.flag_v, 0U);
    POUND_CHECK_EQ_U64(state.pad, 0U);
}

POUND_TEST(guest_state, layout_is_the_size_the_jit_assumes)
{
    // The recompiler loads and stores this struct by offset, so the field
    // positions are part of its ABI and must be pinned here.
    //
    // Layout: x[32] at 0, pc at 256, the four NZCV flags at 264, and a 4-byte
    // pad at 268. The POUND_ALIGNED(64) attribute then rounds the 272-byte
    // extent up to a multiple of 64.
    POUND_CHECK_EQ_U64(GUEST_STATE_REGISTER_COUNT, 32U);
    POUND_CHECK_EQ_U64(_Alignof(guest_state_t), 64U);
    POUND_CHECK_EQ_U64(offsetof(guest_state_t, x), 0U);
    POUND_CHECK_EQ_U64(offsetof(guest_state_t, pc), 256U);
    POUND_CHECK_EQ_U64(offsetof(guest_state_t, flag_n), 264U);
    POUND_CHECK_EQ_U64(offsetof(guest_state_t, flag_z), 265U);
    POUND_CHECK_EQ_U64(offsetof(guest_state_t, flag_c), 266U);
    POUND_CHECK_EQ_U64(offsetof(guest_state_t, flag_v), 267U);
    POUND_CHECK_EQ_U64(offsetof(guest_state_t, pad), 268U);

    // A C struct is always padded up to its own alignment: 272 rounded up to the
    // next multiple of 64.
    POUND_CHECK_EQ_U64(sizeof(guest_state_t), 320U);
}

POUND_TEST(guest_state, a_zeroed_state_is_idempotently_reinitialisable)
{
    guest_state_t state;
    POUND_REQUIRE(POUND_SUCCESS == guest_state_init(&state));

    // Simulate the JIT having written live register values.
    state.x[0]  = UINT64_C(0x0123456789ABCDEF);
    state.x[31] = UINT64_MAX;
    state.pc     = UINT64_C(0x0000000100001000);
    state.flag_z = 1U;

    POUND_REQUIRE(POUND_SUCCESS == guest_state_init(&state));

    POUND_CHECK_EQ_U64(state.x[0], 0U);
    POUND_CHECK_EQ_U64(state.x[31], 0U);
    POUND_CHECK_EQ_U64(state.pc, 0U);
    POUND_CHECK_EQ_U64(state.flag_z, 0U);
}

POUND_TEST(guest_state, register_file_has_no_aliasing_overlap)
{
    guest_state_t state;
    POUND_REQUIRE(POUND_SUCCESS == guest_state_init(&state));

    // Writing one register must not disturb its neighbour: the recompiler relies
    // on independent slots.
    for (size_t i = 0; i < GUEST_STATE_REGISTER_COUNT; ++i)
    {
        state.x[i] = (uint64_t)i + 1U;
    }

    for (size_t i = 0; i < GUEST_STATE_REGISTER_COUNT; ++i)
    {
        POUND_CHECK_EQ_U64(state.x[i], (uint64_t)i + 1U);
    }
}

POUND_TEST_SUITE(guest_state,
    POUND_TEST_CASE(guest_state, init_rejects_null),
    POUND_TEST_CASE(guest_state, init_zeroes_the_entire_register_file),
    POUND_TEST_CASE(guest_state, layout_is_the_size_the_jit_assumes),
    POUND_TEST_CASE(guest_state, a_zeroed_state_is_idempotently_reinitialisable),
    POUND_TEST_CASE(guest_state, register_file_has_no_aliasing_overlap))
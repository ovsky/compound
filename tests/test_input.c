//! Tests for the SDL-independent input state.

#include "pound_test.h"

#include "platform/input.h"

#include <math.h>

POUND_TEST(input, init_is_disconnected_and_centred)
{
    pound_input_state_t state;
    memset(&state, 0xFF, sizeof(state));

    pound_input_init(&state);

    POUND_CHECK_EQ_U64(state.buttons, 0U);
    POUND_CHECK(!state.connected);
    POUND_CHECK(!state.keyboard_active);
    POUND_CHECK_EQ_U64(state.axes[POUND_INPUT_STICK_LEFT][POUND_INPUT_AXIS_X], 0U);
    POUND_CHECK_EQ_U64(state.axes[POUND_INPUT_STICK_RIGHT][POUND_INPUT_AXIS_Y], 0U);
}

POUND_TEST(input, buttons_are_independent_bits)
{
    pound_input_state_t state;
    pound_input_init(&state);

    POUND_REQUIRE(POUND_SUCCESS == pound_input_set_button(&state, POUND_INPUT_BUTTON_A, true));
    POUND_REQUIRE(POUND_SUCCESS == pound_input_set_button(&state, POUND_INPUT_BUTTON_ZR, true));
    POUND_CHECK(pound_input_is_pressed(&state, POUND_INPUT_BUTTON_A));
    POUND_CHECK(pound_input_is_pressed(&state, POUND_INPUT_BUTTON_ZR));
    POUND_CHECK(!pound_input_is_pressed(&state, POUND_INPUT_BUTTON_B));

    POUND_REQUIRE(POUND_SUCCESS == pound_input_set_button(&state, POUND_INPUT_BUTTON_A, false));
    POUND_CHECK(!pound_input_is_pressed(&state, POUND_INPUT_BUTTON_A));
    POUND_CHECK(pound_input_is_pressed(&state, POUND_INPUT_BUTTON_ZR));
}

POUND_TEST(input, invalid_button_is_rejected_without_changing_state)
{
    pound_input_state_t state;
    pound_input_init(&state);

    POUND_CHECK_EQ_I64(pound_input_set_button(&state, POUND_INPUT_BUTTON_COUNT, true),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(pound_input_set_button(NULL, POUND_INPUT_BUTTON_A, true),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(state.buttons, 0U);
    POUND_CHECK(!pound_input_is_pressed(NULL, POUND_INPUT_BUTTON_A));
}

POUND_TEST(input, sticks_apply_deadzone_and_clamp)
{
    pound_input_state_t state;
    pound_input_init(&state);

    POUND_REQUIRE(POUND_SUCCESS == pound_input_set_stick(&state, POUND_INPUT_STICK_LEFT, 0.1f, -0.1f));
    POUND_CHECK_EQ_U64(pound_input_get_axis(&state, POUND_INPUT_STICK_LEFT, POUND_INPUT_AXIS_X), 0U);
    POUND_CHECK_EQ_U64(pound_input_get_axis(&state, POUND_INPUT_STICK_LEFT, POUND_INPUT_AXIS_Y), 0U);

    POUND_REQUIRE(POUND_SUCCESS == pound_input_set_stick(&state, POUND_INPUT_STICK_LEFT, 1.0f, -1.0f));
    POUND_CHECK_EQ_U64(pound_input_get_axis(&state, POUND_INPUT_STICK_LEFT, POUND_INPUT_AXIS_X),
                       POUND_INPUT_AXIS_MAX);
    POUND_CHECK(pound_input_get_axis(&state, POUND_INPUT_STICK_LEFT, POUND_INPUT_AXIS_Y)
                == -POUND_INPUT_AXIS_MAX);

    POUND_REQUIRE(POUND_SUCCESS == pound_input_set_stick(&state, POUND_INPUT_STICK_RIGHT, 8.0f, -8.0f));
    POUND_CHECK_EQ_U64(pound_input_get_axis(&state, POUND_INPUT_STICK_RIGHT, POUND_INPUT_AXIS_X),
                       POUND_INPUT_AXIS_MAX);
    POUND_CHECK(pound_input_get_axis(&state, POUND_INPUT_STICK_RIGHT, POUND_INPUT_AXIS_Y)
                == -POUND_INPUT_AXIS_MAX);

    POUND_REQUIRE(POUND_SUCCESS == pound_input_set_stick(&state, POUND_INPUT_STICK_RIGHT, NAN, NAN));
    POUND_CHECK_EQ_U64(pound_input_get_axis(&state, POUND_INPUT_STICK_RIGHT, POUND_INPUT_AXIS_X), 0U);
    POUND_CHECK_EQ_U64(pound_input_get_axis(&state, POUND_INPUT_STICK_RIGHT, POUND_INPUT_AXIS_Y), 0U);
}

POUND_TEST(input, invalid_sticks_and_axes_are_safe)
{
    pound_input_state_t state;
    pound_input_init(&state);

    POUND_CHECK_EQ_I64(pound_input_set_stick(&state, POUND_INPUT_STICK_COUNT, 0.0f, 0.0f),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(pound_input_set_stick(NULL, POUND_INPUT_STICK_LEFT, 0.0f, 0.0f),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(pound_input_get_axis(&state, POUND_INPUT_STICK_COUNT, POUND_INPUT_AXIS_X), 0U);
    POUND_CHECK_EQ_U64(pound_input_get_axis(&state, POUND_INPUT_STICK_LEFT, POUND_INPUT_AXIS_COUNT), 0U);
}

POUND_TEST(input, clear_keeps_connection_and_keyboard_flags)
{
    pound_input_state_t state;
    pound_input_init(&state);

    pound_input_set_connected(&state, true);
    pound_input_set_keyboard_active(&state, true);
    POUND_REQUIRE(POUND_SUCCESS == pound_input_set_button(&state, POUND_INPUT_BUTTON_PLUS, true));
    POUND_REQUIRE(POUND_SUCCESS == pound_input_set_stick(&state, POUND_INPUT_STICK_LEFT, 0.5f, 0.5f));

    pound_input_clear(&state);

    POUND_CHECK(state.connected);
    POUND_CHECK(state.keyboard_active);
    POUND_CHECK_EQ_U64(state.buttons, 0U);
    POUND_CHECK_EQ_U64(pound_input_get_axis(&state, POUND_INPUT_STICK_LEFT, POUND_INPUT_AXIS_X), 0U);
}

POUND_TEST_SUITE(input,
                 POUND_TEST_CASE(input, init_is_disconnected_and_centred),
                 POUND_TEST_CASE(input, buttons_are_independent_bits),
                 POUND_TEST_CASE(input, invalid_button_is_rejected_without_changing_state),
                 POUND_TEST_CASE(input, sticks_apply_deadzone_and_clamp),
                 POUND_TEST_CASE(input, invalid_sticks_and_axes_are_safe),
                 POUND_TEST_CASE(input, clear_keeps_connection_and_keyboard_flags))

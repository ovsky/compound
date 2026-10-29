#include "platform/input.h"

#include "log.h"

#include <string.h>

static bool
button_is_valid(const pound_input_button_t button)
{
    return ((int)button >= 0) && (button < POUND_INPUT_BUTTON_COUNT);
}

static bool
stick_is_valid(const pound_input_stick_t stick)
{
    return ((int)stick >= 0) && (stick < POUND_INPUT_STICK_COUNT);
}

/// Applies a per-axis deadzone and maps [-1, 1] to the signed 16-bit range.
///
/// Per-axis rather than radial: a radial deadzone would turn a diagonal push
/// into a smaller vector after normalisation, which is not what Horizon's
/// calibration data describes. Values that are not finite are centred instead
/// of being cast to an implementation-defined integer.
static float
apply_deadzone(const float value)
{
    if (!(value >= -1.0f && value <= 1.0f))
    {
        // Covers NaN as well as out-of-range input.
        if (!(value > 0.0f) && !(value < 0.0f))
        {
            return 0.0f;
        }

        return (value > 1.0f) ? 1.0f : ((value < -1.0f) ? -1.0f : value);
    }

    if (value > POUND_INPUT_STICK_DEADZONE)
    {
        return (value - POUND_INPUT_STICK_DEADZONE) / (1.0f - POUND_INPUT_STICK_DEADZONE);
    }

    if (value < -POUND_INPUT_STICK_DEADZONE)
    {
        return (value + POUND_INPUT_STICK_DEADZONE) / (1.0f - POUND_INPUT_STICK_DEADZONE);
    }

    return 0.0f;
}

static int16_t
to_axis(const float value)
{
    const float scaled = apply_deadzone(value) * (float)POUND_INPUT_AXIS_MAX;

    if (scaled >= (float)POUND_INPUT_AXIS_MAX)
    {
        return (int16_t)POUND_INPUT_AXIS_MAX;
    }
    if (scaled <= -(float)POUND_INPUT_AXIS_MAX)
    {
        return (int16_t)-POUND_INPUT_AXIS_MAX;
    }

    return (int16_t)((scaled >= 0.0f) ? (scaled + 0.5f) : (scaled - 0.5f));
}

void
pound_input_init(pound_input_state_t *POUND_RESTRICT state)
{
    if (NULL == state)
    {
        return;
    }

    memset(state, 0, sizeof(*state));
}

void
pound_input_clear(pound_input_state_t *POUND_RESTRICT state)
{
    if (NULL == state)
    {
        return;
    }

    state->buttons = 0U;
    memset(state->axes, 0, sizeof(state->axes));
}

error_t
pound_input_set_button(pound_input_state_t *POUND_RESTRICT state,
                       const pound_input_button_t           button,
                       const bool                           pressed)
{
    if (NULL == state)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting button update: state is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!button_is_valid(button))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting button update: button %d is out of range.", (int)button);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const uint32_t mask = UINT32_C(1) << (uint32_t)button;

    if (pressed)
    {
        state->buttons |= mask;
    }
    else
    {
        state->buttons &= ~mask;
    }

    return POUND_SUCCESS;
}

bool
pound_input_is_pressed(const pound_input_state_t *POUND_RESTRICT state, const pound_input_button_t button)
{
    if ((NULL == state) || !button_is_valid(button))
    {
        return false;
    }

    return (state->buttons & (UINT32_C(1) << (uint32_t)button)) != 0U;
}

error_t
pound_input_set_stick(pound_input_state_t *POUND_RESTRICT state,
                      const pound_input_stick_t           stick,
                      const float                         x,
                      const float                         y)
{
    if (NULL == state)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting stick update: state is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!stick_is_valid(stick))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting stick update: stick %d is out of range.", (int)stick);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    state->axes[stick][POUND_INPUT_AXIS_X] = to_axis(x);
    state->axes[stick][POUND_INPUT_AXIS_Y] = to_axis(y);
    return POUND_SUCCESS;
}

int16_t
pound_input_get_axis(const pound_input_state_t *POUND_RESTRICT state,
                     const pound_input_stick_t           stick,
                     const pound_input_axis_t            axis)
{
    if ((NULL == state) || !stick_is_valid(stick) || ((int)axis < 0) || (axis >= POUND_INPUT_AXIS_COUNT))
    {
        return 0;
    }

    return state->axes[stick][axis];
}

void
pound_input_set_connected(pound_input_state_t *POUND_RESTRICT state, const bool connected)
{
    if (NULL != state)
    {
        state->connected = connected;
    }
}

void
pound_input_set_keyboard_active(pound_input_state_t *POUND_RESTRICT state, const bool active)
{
    if (NULL != state)
    {
        state->keyboard_active = active;
    }
}

/*** end of file ***/

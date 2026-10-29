#ifndef POUND_PLATFORM_INPUT_H
#define POUND_PLATFORM_INPUT_H

//! Controller and keyboard state, independent of SDL.
//!
//! The emulator's input surface is deliberately a plain value type. SDL event
//! handling maps a host key or gamepad button into these calls, and the guest
//! HID service will later read the same struct. Keeping the mapping table in
//! `main.c` rather than here is what lets the core stay headless and testable.
//!
//! Buttons are a bitmask, so a state can be compared, copied, and asserted on
//! without allocating. Sticks are signed 16-bit because that is the range
//! Horizon reports, and the deadzone is applied here once rather than by every
//! consumer.

#include "attributes.h"
#include "errors.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    POUND_INPUT_BUTTON_A = 0,
    POUND_INPUT_BUTTON_B,
    POUND_INPUT_BUTTON_X,
    POUND_INPUT_BUTTON_Y,
    POUND_INPUT_BUTTON_L,
    POUND_INPUT_BUTTON_R,
    POUND_INPUT_BUTTON_ZL,
    POUND_INPUT_BUTTON_ZR,
    POUND_INPUT_BUTTON_PLUS,
    POUND_INPUT_BUTTON_MINUS,
    POUND_INPUT_BUTTON_HOME,
    POUND_INPUT_BUTTON_CAPTURE,
    POUND_INPUT_BUTTON_DPAD_UP,
    POUND_INPUT_BUTTON_DPAD_DOWN,
    POUND_INPUT_BUTTON_DPAD_LEFT,
    POUND_INPUT_BUTTON_DPAD_RIGHT,
    POUND_INPUT_BUTTON_STICK_LEFT,
    POUND_INPUT_BUTTON_STICK_RIGHT,
    POUND_INPUT_BUTTON_COUNT,
} pound_input_button_t;

typedef enum
{
    POUND_INPUT_STICK_LEFT = 0,
    POUND_INPUT_STICK_RIGHT,
    POUND_INPUT_STICK_COUNT,
} pound_input_stick_t;

typedef enum
{
    POUND_INPUT_AXIS_X = 0,
    POUND_INPUT_AXIS_Y,
    POUND_INPUT_AXIS_COUNT,
} pound_input_axis_t;

/// Fraction of full deflection ignored around the centre. 0.15 is the
/// conventional Switch analogue-stick deadzone and is applied per axis.
#define POUND_INPUT_STICK_DEADZONE 0.15f

/// Full-scale value for a signed 16-bit stick axis.
#define POUND_INPUT_AXIS_MAX 32767

typedef struct
{
    /// One bit per `pound_input_button_t`.
    uint32_t buttons;

    /// `axes[stick][axis]`, in [-32767, 32767].
    int16_t axes[POUND_INPUT_STICK_COUNT][POUND_INPUT_AXIS_COUNT];

    bool connected;
    bool keyboard_active;
    uint8_t pad[2];
} pound_input_state_t;

/// Zeroes a state and marks it disconnected.
void pound_input_init(pound_input_state_t *POUND_RESTRICT state);

/// Zeroes buttons and axes but keeps `connected` and `keyboard_active`.
void pound_input_clear(pound_input_state_t *POUND_RESTRICT state);

/// Sets or clears one button.
error_t pound_input_set_button(pound_input_state_t *POUND_RESTRICT state,
                               pound_input_button_t button,
                               bool pressed);

/// Returns whether `button` is held. False for an invalid button or NULL state.
bool pound_input_is_pressed(const pound_input_state_t *POUND_RESTRICT state, pound_input_button_t button);

/// Sets a stick from two full-deflection values, applying the deadzone.
///
/// Values outside [-1, 1] are clamped; NaN is treated as centred. The result is
/// stored as signed 16-bit with the deadzone removed, so no consumer has to
/// reapply it.
error_t pound_input_set_stick(pound_input_state_t *POUND_RESTRICT state,
                              pound_input_stick_t stick,
                              float x,
                              float y);

/// Reads one axis. Returns 0 for an invalid state or index.
int16_t pound_input_get_axis(const pound_input_state_t *POUND_RESTRICT state,
                             pound_input_stick_t stick,
                             pound_input_axis_t axis);

/// Records whether a host controller is attached.
void pound_input_set_connected(pound_input_state_t *POUND_RESTRICT state, bool connected);

/// Records whether the host keyboard is being used as a controller.
void pound_input_set_keyboard_active(pound_input_state_t *POUND_RESTRICT state, bool active);

#ifdef __cplusplus
}
#endif

#endif // POUND_PLATFORM_INPUT_H

/*** end of file ***/

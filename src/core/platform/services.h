#ifndef POUND_PLATFORM_SERVICES_H
#define POUND_PLATFORM_SERVICES_H

//! Host lifecycle, timing, and capability reporting.
//!
//! This is the small piece of state that every host backend has to agree on:
//! whether the application was asked to quit, which optional services actually
//! started, how many gamepads are attached, and what frame budget the loop is
//! running under. It contains no SDL types, so the core and the tests can use it
//! without a window.
//!
//! Capabilities are reported, not assumed. A UI that asks "is audio available?"
//! must get false when the host denied the audio device, rather than a failure
//! discovered later by a silent stream. The same rule applies to gamepads: a
//! count of zero and "input is available" are different facts.

#include "attributes.h"
#include "errors.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POUND_PLATFORM_MAX_GAMEPADS 8U

typedef enum
{
    POUND_PLATFORM_CAPABILITY_AUDIO_OUTPUT = 0,
    POUND_PLATFORM_CAPABILITY_GAMEPADS,
    POUND_PLATFORM_CAPABILITY_KEYBOARD,
    POUND_PLATFORM_CAPABILITY_CLIPBOARD,
    POUND_PLATFORM_CAPABILITY_BATTERY,
    POUND_PLATFORM_CAPABILITY_COUNT,
} pound_platform_capability_t;

typedef struct
{
    bool     initialized;
    bool     shutdown_requested;

    /// Indexed by `pound_platform_capability_t`.
    bool capabilities[POUND_PLATFORM_CAPABILITY_COUNT];

    uint32_t target_fps;
    uint32_t gamepad_count;

    uint64_t start_ms;
    uint64_t last_frame_ms;
    uint64_t frame_count;
} pound_platform_services_t;

/// Initialises the service set at `now_ms` with a target frame rate.
///
/// `target_fps` must be in [1, 1000]. All capabilities start unavailable; a
/// host backend enables the ones it actually created.
error_t pound_platform_services_init(pound_platform_services_t *POUND_RESTRICT services,
                                     uint64_t                                      now_ms,
                                     uint32_t                                      target_fps);

/// Zeroes the service set. Safe on NULL and on an already-zeroed value.
void pound_platform_services_shutdown(pound_platform_services_t *POUND_RESTRICT services);

/// Records whether one capability is available.
error_t pound_platform_services_set_capability(pound_platform_services_t *POUND_RESTRICT services,
                                               pound_platform_capability_t capability,
                                               bool available);

bool pound_platform_services_has_capability(const pound_platform_services_t *POUND_RESTRICT services,
                                            pound_platform_capability_t capability);

/// Records the attached gamepad count. Zero is valid and means none.
error_t pound_platform_services_set_gamepad_count(pound_platform_services_t *POUND_RESTRICT services,
                                                  uint32_t                                      count);

/// Advances the frame counter and records `now_ms`.
///
/// A timestamp earlier than the last one is rejected rather than clamped: it
/// means the caller mixed clocks, and a negative frame time would silently make
/// every later measurement wrong.
error_t pound_platform_services_tick(pound_platform_services_t *POUND_RESTRICT services, uint64_t now_ms);

/// Milliseconds since initialisation. Returns 0 for an invalid or earlier time.
uint64_t pound_platform_services_elapsed_ms(const pound_platform_services_t *POUND_RESTRICT services,
                                            uint64_t                                      now_ms);

/// Frame budget in whole milliseconds, rounded up (17 ms for 60 FPS).
uint32_t pound_platform_services_frame_interval_ms(const pound_platform_services_t *POUND_RESTRICT services);

/// Requests shutdown. Idempotent.
void pound_platform_services_request_shutdown(pound_platform_services_t *POUND_RESTRICT services);

bool pound_platform_services_should_shutdown(const pound_platform_services_t *POUND_RESTRICT services);

#ifdef __cplusplus
}
#endif

#endif // POUND_PLATFORM_SERVICES_H

/*** end of file ***/

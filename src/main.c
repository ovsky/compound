#include "gui/gui.h"
#include "log.h"
#include "memory/memory.h"
#include "gpu/vulkan/vulkan_backend.h"
#include "loader/title_loader.h"
#include "loader/boot/boot.h"
#include "platform/audio.h"
#include "platform/input.h"
#include "platform/services.h"

#include <SDL3/SDL.h>

// SDL's Android entry point is invoked from Java: SDLActivity dlopen()s
// libmain.so and calls the exported `SDL_main` through JNI. SDL_main.h is
// deliberately not pulled in by SDL.h, and it is what performs the
// `main` -> `SDL_main` renaming for that platform.
#if POUND_PLATFORM_ANDROID
#include <SDL3/SDL_main.h>
#endif

#include <mimalloc-override.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// This is required to perform hot reloading using Windows DLLs.
#if POUND_PLATFORM_WINDOWS

#define IMGL3W_IMPL

#endif // POUND_PLATFORM_WINDOWS

#include "imgui/backends/imgui_impl_opengl3_loader.h"

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"
#include "cimgui_impl.h"

#define GUI_FREEZE_REASON_SIZE 256

/// Milliseconds between hot-reload polls of the plugin's mtime.
#define GUI_RELOAD_POLL_INTERVAL_MS 150U

typedef struct
{
    // Ordered widest-first so the only padding in the struct is the trailing
    // alignment gap, which `pad` below accounts for explicitly. Interleaving
    // the bools with the pointers instead left a 5-byte hole in the middle,
    // which -Wpadded flags and which `app_t` is large enough for to matter:
    // it embeds gui_plugin_t, and that struct carries its own 4096-byte path.
    SDL_Window   *window;
    SDL_GLContext gl_context;
    ImGuiContext *imgui_context;
    uint64_t      gui_source_time;
    uint64_t      gui_pending_time;
    uint64_t      gui_pending_since;
    char          gui_frozen_reason[GUI_FREEZE_REASON_SIZE];
    char          gui_source_path[POUND_PATH_MAX];
    gui_plugin_t  gui;
    bool          running;
    bool          gui_reload_request;
    bool          gui_frozen;
    char          pad[5];
} app_t;

/// Host services the event loop owns: input, audio, and lifecycle state.
///
/// Kept beside `app_t` rather than inside it so the large GUI plugin and the
/// per-frame runtime state stay independent: `app_t` is about the window and
/// hot-reloaded UI, this is about what the emulator is doing.
typedef struct
{
    pound_platform_services_t services;
    pound_input_state_t       input;
    pound_audio_t             audio;
    SDL_AudioStream          *audio_stream;
    SDL_Gamepad              *gamepad;
    uint32_t                  gamepad_count;
    bool                      audio_ready;
    char                      pad[3];
} app_runtime_t;

static bool app_video_init(app_t *app);
static bool app_video_shutdown(app_t *app);
static bool app_gui_init(app_t *app);
static bool app_gui_shutdown(app_t *app);
static bool app_gui_update(app_t *app, bool force);
static void app_gui_freeze(app_t *app, const char *reason);
static void app_gui_unfreeze(app_t *app);
static bool app_hot_reload_init(app_t *app);
static bool app_hot_reload_shutdown(app_t *app);
#if !POUND_PLATFORM_SUPPORTS_HOT_RELOAD
static bool app_gui_bind_static(app_t *app);
#endif // !POUND_PLATFORM_SUPPORTS_HOT_RELOAD
static void app_runtime_init(app_runtime_t *runtime);
static void app_runtime_shutdown(app_runtime_t *runtime);
static bool app_audio_start(app_runtime_t *runtime);
static void app_audio_stop(app_runtime_t *runtime);
static void app_gamepad_scan(app_runtime_t *runtime);
static void app_gamepad_close(app_runtime_t *runtime);
static void app_poll_keyboard(app_runtime_t *runtime);
static void app_poll_gamepad(app_runtime_t *runtime);
static void app_poll_events(app_t *app, app_runtime_t *runtime);
static void app_render_frame(app_t *app);
static void app_render_frozen_overlay(app_t *app);
static void app_memory_churn(void);
static bool app_memory_churn_requested(int argc, char *argv[]);
static void app_frame_limit(uint64_t frame_start_ms);

// -----------------------------------------------------------------------------
// Entry point
// -----------------------------------------------------------------------------

/// SDL3 defines no command-line introspection API -- the SDL2 `SDL_GetNumArgv`
/// / `SDL_GetArgv` pair was removed outright -- so the only way to see the
/// arguments is to accept them in the entry point. `SDL_main.h` renames this to
/// `SDL_main`, and SDL invokes it through `SDL_main_func`, whose signature is
/// `int (int, char *[])`; declaring `main(void)` here would therefore have
/// disagreed with the function pointer SDL actually calls.
int
main(int argc, char *argv[])
{
    mi_option_set(mi_option_arena_reserve, 128 * 1024);
    pound_logger_init_default();
    memory_subsystem_init();
    (void)memory_subsystem_set_bucket(MEMORY_BUCKET_UI);
    pound_title_loader_init();
    pound_vulkan_backend_t *vulkan_backend = NULL;
    pound_vulkan_config_t vulkan_cfg = {0};
    vulkan_cfg.application_name = "Compound";
    (void)pound_vulkan_init(&vulkan_backend, &vulkan_cfg);
    pound_boot_state_t boot_state = {0};
    (void)pound_boot_init(&boot_state);

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting process: Failed to initialise SDL because %s.",
                        SDL_GetError());
        return EXIT_FAILURE;
    }

    app_t app   = { 0 };
    app.running = true;

    if (false == app_video_init(&app))
    {
        app_video_shutdown(&app);
        SDL_Quit();
        return EXIT_FAILURE;
    }

    if (false == app_gui_init(&app))
    {
        app_gui_shutdown(&app);
        app_video_shutdown(&app);
        SDL_Quit();
        return EXIT_FAILURE;
    }

    if (false == app_hot_reload_init(&app))
    {
        POUND_LOG_WARN(&thread_logger, "Hot reloading is disabled. GUI will remain frozen.");
        app_gui_freeze(&app, "hot reload initialization failed");
    }

    const bool force_reload = true;

    if (false == app_gui_update(&app, force_reload))
    {
        POUND_LOG_WARN(
            &thread_logger,
            "Initial GUI load failed. GUI will remain frozen until hot reload succeeds.");
    }

    if (NULL == app.window)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting process: window is missing before first show.");
        app_hot_reload_shutdown(&app);
        app_gui_shutdown(&app);
        app_video_shutdown(&app);
        SDL_Quit();
        return EXIT_FAILURE;
    }

    SDL_ShowWindow(app.window);

    if ((SDL_GetWindowFlags(app.window) & SDL_WINDOW_HIDDEN) != 0)
    {
        POUND_LOG_WARN(&thread_logger, "Window was shown but remains hidden, continuing anyway.");
    }

    app_runtime_t runtime;
    app_runtime_init(&runtime);

    if (SDL_InitSubSystem(SDL_INIT_AUDIO))
    {
        (void)app_audio_start(&runtime);
    }
    else
    {
        POUND_LOG_WARN(&thread_logger, "Audio is unavailable because %s.", SDL_GetError());
    }

    if (SDL_InitSubSystem(SDL_INIT_GAMEPAD))
    {
        (void)pound_platform_services_set_capability(
            &runtime.services, POUND_PLATFORM_CAPABILITY_GAMEPADS, true);
        app_gamepad_scan(&runtime);
    }
    else
    {
        POUND_LOG_WARN(&thread_logger, "Gamepad input is unavailable because %s.", SDL_GetError());
    }

    POUND_LOG_INFO(&thread_logger, "Starting main loop on %s.", POUND_PLATFORM_NAME);
    if (argc > 1)
    {
        pound_title_info_t title_info;
        const error_t detect_error = pound_title_detect(argv[1], &title_info);
        if (POUND_SUCCESS == detect_error)
        {
            const error_t load_error = pound_title_load(&title_info);
            if (POUND_SUCCESS == load_error)
            {
                const error_t boot_error = pound_boot_load_title(&boot_state, &title_info);
                if (POUND_SUCCESS == boot_error)
                {
                    (void)pound_boot_start(&boot_state);
                }
                else
                {
                    POUND_LOG_ERROR(&thread_logger, "Boot refused the title: %s.",
                                    pound_error_to_string(boot_error));
                }
            }
            else
            {
                POUND_LOG_ERROR(&thread_logger, "Loading the title failed: %s.",
                                pound_error_to_string(load_error));
            }
        }
        else
        {
            POUND_LOG_ERROR(&thread_logger, "Detecting %s failed: %s.", argv[1],
                            pound_error_to_string(detect_error));
        }
    }

    // Diagnostic only: see app_memory_churn_requested().
    const bool run_memory_selftest = app_memory_churn_requested(argc, argv);

    if (run_memory_selftest)
    {
        POUND_LOG_INFO(&thread_logger, "Memory tracker self-test enabled (--selftest-memory).");
    }

    while (app.running && !pound_platform_services_should_shutdown(&runtime.services))
    {
        const uint64_t frame_start = SDL_GetTicks();

        app_poll_events(&app, &runtime);
        (void)pound_platform_services_tick(&runtime.services, SDL_GetTicks());

        if (run_memory_selftest)
        {
            app_memory_churn();
        }

        app_render_frame(&app);
        app_frame_limit(frame_start);
    }

    app_runtime_shutdown(&runtime);
    pound_boot_shutdown(&boot_state);
    app_gui_shutdown(&app);
    app_video_shutdown(&app);
    if (vulkan_backend != NULL)
    {
        pound_vulkan_shutdown(vulkan_backend);
    }
    memory_subsystem_destroy();
    SDL_Quit();

    return EXIT_SUCCESS;
}

// -----------------------------------------------------------------------------
// Platform services: audio, gamepads, keyboard
// -----------------------------------------------------------------------------

static void
app_runtime_init(app_runtime_t *runtime)
{
    if (NULL == runtime)
    {
        return;
    }

    memset(runtime, 0, sizeof(*runtime));
    pound_input_init(&runtime->input);

    if (POUND_SUCCESS != pound_platform_services_init(&runtime->services, SDL_GetTicks(), 60U))
    {
        POUND_LOG_ERROR(&thread_logger, "Platform services did not initialise; using defaults.");
    }

    (void)pound_platform_services_set_capability(
        &runtime->services, POUND_PLATFORM_CAPABILITY_KEYBOARD, true);
}

static void
app_runtime_shutdown(app_runtime_t *runtime)
{
    if (NULL == runtime)
    {
        return;
    }

    app_audio_stop(runtime);
    app_gamepad_close(runtime);
    pound_platform_services_shutdown(&runtime->services);
    memset(runtime, 0, sizeof(*runtime));
}

/// SDL audio thread callback: drain the core ring and pad an underrun with
/// silence.
///
/// The ring is allowed to be empty; SDL wants exactly the requested number of
/// bytes, and returning fewer causes a device underrun. Filling the tail with
/// zeros is the correct sound for "the guest has not produced audio yet".
static void SDLCALL
app_audio_callback(void *userdata, SDL_AudioStream *stream, const int additional_amount,
                   const int total_amount)
{
    POUND_UNUSED(total_amount);

    if ((NULL == stream) || (additional_amount <= 0))
    {
        return;
    }

    pound_audio_t *audio = (pound_audio_t *)userdata;
    int16_t        samples[1024U * 2U];
    int            remaining = additional_amount;

    while (remaining > 0)
    {
        const int frames_per_chunk = (int)(sizeof(samples) / (2U * sizeof(int16_t)));
        int       frames           = remaining / (int)(2U * sizeof(int16_t));

        if (frames > frames_per_chunk)
        {
            frames = frames_per_chunk;
        }

        if (frames <= 0)
        {
            // Fewer than one frame left; SDL normally only asks for whole
            // frames, so drop the remainder rather than writing a torn sample.
            break;
        }

        size_t got = 0U;
        if (NULL != audio)
        {
            got = pound_audio_read(audio, samples, (size_t)frames);
        }

        if (got < (size_t)frames)
        {
            memset(samples + (got * 2U), 0, ((size_t)frames - got) * 2U * sizeof(int16_t));
        }

        if (!SDL_PutAudioStreamData(stream, samples, frames * (int)(2U * sizeof(int16_t))))
        {
            POUND_LOG_ERROR(&thread_logger, "Queueing audio to SDL failed because %s.", SDL_GetError());
            break;
        }

        remaining -= frames * (int)(2U * sizeof(int16_t));
    }
}

static bool
app_audio_start(app_runtime_t *runtime)
{
    if (NULL == runtime)
    {
        return false;
    }

    if (POUND_SUCCESS
        != pound_audio_init(&runtime->audio, 48000U, 2U, 4096U))
    {
        POUND_LOG_ERROR(&thread_logger, "The core audio ring could not be created.");
        return false;
    }

    const SDL_AudioSpec spec = {
        .format   = SDL_AUDIO_S16,
        .channels = 2,
        .freq     = 48000,
    };

    runtime->audio_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, app_audio_callback, &runtime->audio);

    if (NULL == runtime->audio_stream)
    {
        POUND_LOG_WARN(&thread_logger, "Opening the default audio device failed because %s.",
                       SDL_GetError());
        pound_audio_shutdown(&runtime->audio);
        return false;
    }

    if (!SDL_ResumeAudioStreamDevice(runtime->audio_stream))
    {
        POUND_LOG_WARN(&thread_logger, "Resuming the audio device failed because %s.", SDL_GetError());
        SDL_DestroyAudioStream(runtime->audio_stream);
        runtime->audio_stream = NULL;
        pound_audio_shutdown(&runtime->audio);
        return false;
    }

    runtime->audio_ready = true;
    (void)pound_platform_services_set_capability(
        &runtime->services, POUND_PLATFORM_CAPABILITY_AUDIO_OUTPUT, true);

    POUND_LOG_INFO(&thread_logger, "Audio output opened at 48000 Hz, stereo S16.");
    return true;
}

static void
app_audio_stop(app_runtime_t *runtime)
{
    if (NULL == runtime)
    {
        return;
    }

    if (NULL != runtime->audio_stream)
    {
        SDL_DestroyAudioStream(runtime->audio_stream);
        runtime->audio_stream = NULL;
    }

    if (runtime->audio_ready)
    {
        pound_audio_shutdown(&runtime->audio);
        runtime->audio_ready = false;
    }
}

static void
app_gamepad_close(app_runtime_t *runtime)
{
    if (NULL == runtime)
    {
        return;
    }

    if (NULL != runtime->gamepad)
    {
        SDL_CloseGamepad(runtime->gamepad);
        runtime->gamepad = NULL;
    }

    runtime->gamepad_count = 0U;
    pound_input_set_connected(&runtime->input, false);
    pound_input_clear(&runtime->input);
    (void)pound_platform_services_set_gamepad_count(&runtime->services, 0U);
}

/// Closes any open gamepad and opens the first one SDL reports.
///
/// Compound currently exposes one guest controller. Counting every attached
/// gamepad is still useful to the UI, and is reported separately, but opening
/// more than one would create a second source of buttons with no guest object
/// to route them to.
static void
app_gamepad_scan(app_runtime_t *runtime)
{
    if (NULL == runtime)
    {
        return;
    }

    app_gamepad_close(runtime);

    int             count = 0;
    SDL_JoystickID *ids   = SDL_GetGamepads(&count);

    if (NULL != ids)
    {
        uint32_t gamepads = 0U;

        for (int index = 0; index < count; ++index)
        {
            if (!SDL_IsGamepad(ids[index]))
            {
                continue;
            }

            ++gamepads;

            if (NULL == runtime->gamepad)
            {
                runtime->gamepad = SDL_OpenGamepad(ids[index]);

                if (NULL == runtime->gamepad)
                {
                    POUND_LOG_WARN(&thread_logger, "Opening a gamepad failed because %s.",
                                   SDL_GetError());
                }
                else
                {
                    const char *name = SDL_GetGamepadName(runtime->gamepad);
                    POUND_LOG_INFO(&thread_logger, "Gamepad connected: %s.",
                                   (NULL != name) ? name : "unnamed");
                }
            }
        }

        SDL_free(ids);

        runtime->gamepad_count = (gamepads > POUND_PLATFORM_MAX_GAMEPADS)
                                   ? POUND_PLATFORM_MAX_GAMEPADS
                                   : gamepads;
        (void)pound_platform_services_set_gamepad_count(&runtime->services,
                                                        runtime->gamepad_count);
    }

    pound_input_set_connected(&runtime->input, NULL != runtime->gamepad);
}

static void
app_poll_keyboard(app_runtime_t *runtime)
{
    if ((NULL == runtime) || (NULL != runtime->gamepad))
    {
        return;
    }

    const bool *keys = SDL_GetKeyboardState(NULL);
    if (NULL == keys)
    {
        return;
    }

    const struct
    {
        SDL_Scancode        scancode;
        pound_input_button_t button;
    } bindings[] = {
        { SDL_SCANCODE_Z, POUND_INPUT_BUTTON_A },
        { SDL_SCANCODE_X, POUND_INPUT_BUTTON_B },
        { SDL_SCANCODE_C, POUND_INPUT_BUTTON_X },
        { SDL_SCANCODE_V, POUND_INPUT_BUTTON_Y },
        { SDL_SCANCODE_Q, POUND_INPUT_BUTTON_L },
        { SDL_SCANCODE_E, POUND_INPUT_BUTTON_R },
        { SDL_SCANCODE_1, POUND_INPUT_BUTTON_ZL },
        { SDL_SCANCODE_3, POUND_INPUT_BUTTON_ZR },
        { SDL_SCANCODE_RETURN, POUND_INPUT_BUTTON_PLUS },
        { SDL_SCANCODE_BACKSPACE, POUND_INPUT_BUTTON_MINUS },
        { SDL_SCANCODE_M, POUND_INPUT_BUTTON_HOME },
        { SDL_SCANCODE_P, POUND_INPUT_BUTTON_CAPTURE },
        { SDL_SCANCODE_UP, POUND_INPUT_BUTTON_DPAD_UP },
        { SDL_SCANCODE_DOWN, POUND_INPUT_BUTTON_DPAD_DOWN },
        { SDL_SCANCODE_LEFT, POUND_INPUT_BUTTON_DPAD_LEFT },
        { SDL_SCANCODE_RIGHT, POUND_INPUT_BUTTON_DPAD_RIGHT },
        { SDL_SCANCODE_T, POUND_INPUT_BUTTON_STICK_LEFT },
        { SDL_SCANCODE_G, POUND_INPUT_BUTTON_STICK_RIGHT },
    };

    bool active = false;

    for (size_t index = 0U; index < (sizeof(bindings) / sizeof(bindings[0])); ++index)
    {
        const bool pressed = keys[bindings[index].scancode];
        (void)pound_input_set_button(&runtime->input, bindings[index].button, pressed);
        active = active || pressed;
    }

    const float left_x = (keys[SDL_SCANCODE_D] ? 1.0f : 0.0f) - (keys[SDL_SCANCODE_A] ? 1.0f : 0.0f);
    const float left_y = (keys[SDL_SCANCODE_W] ? 1.0f : 0.0f) - (keys[SDL_SCANCODE_S] ? 1.0f : 0.0f);
    const float right_x = (keys[SDL_SCANCODE_L] ? 1.0f : 0.0f) - (keys[SDL_SCANCODE_J] ? 1.0f : 0.0f);
    const float right_y = (keys[SDL_SCANCODE_I] ? 1.0f : 0.0f) - (keys[SDL_SCANCODE_K] ? 1.0f : 0.0f);

    (void)pound_input_set_stick(&runtime->input, POUND_INPUT_STICK_LEFT, left_x, left_y);
    (void)pound_input_set_stick(&runtime->input, POUND_INPUT_STICK_RIGHT, right_x, right_y);

    active = active || (left_x != 0.0f) || (left_y != 0.0f) || (right_x != 0.0f) || (right_y != 0.0f);
    pound_input_set_keyboard_active(&runtime->input, active);
}

static void
app_poll_gamepad(app_runtime_t *runtime)
{
    if ((NULL == runtime) || (NULL == runtime->gamepad))
    {
        return;
    }

    const float left_x = (float)SDL_GetGamepadAxis(runtime->gamepad, SDL_GAMEPAD_AXIS_LEFTX)
                       / (float)POUND_INPUT_AXIS_MAX;
    const float left_y = (float)SDL_GetGamepadAxis(runtime->gamepad, SDL_GAMEPAD_AXIS_LEFTY)
                       / (float)POUND_INPUT_AXIS_MAX;
    const float right_x = (float)SDL_GetGamepadAxis(runtime->gamepad, SDL_GAMEPAD_AXIS_RIGHTX)
                        / (float)POUND_INPUT_AXIS_MAX;
    const float right_y = (float)SDL_GetGamepadAxis(runtime->gamepad, SDL_GAMEPAD_AXIS_RIGHTY)
                        / (float)POUND_INPUT_AXIS_MAX;

    // SDL's Y axis grows downward; Horizon's grows upward.
    (void)pound_input_set_stick(&runtime->input, POUND_INPUT_STICK_LEFT, left_x, -left_y);
    (void)pound_input_set_stick(&runtime->input, POUND_INPUT_STICK_RIGHT, right_x, -right_y);

    const Sint16 zl = SDL_GetGamepadAxis(runtime->gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    const Sint16 zr = SDL_GetGamepadAxis(runtime->gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);

    (void)pound_input_set_button(&runtime->input, POUND_INPUT_BUTTON_ZL, zl > 8192);
    (void)pound_input_set_button(&runtime->input, POUND_INPUT_BUTTON_ZR, zr > 8192);
}

/// Maps SDL's positional gamepad buttons to Switch's labelled buttons.
///
/// SDL names the diamond by position (south/east/west/north) because Nintendo
/// and Xbox disagree about where A is. Switch's A is the east face button and B
/// is the south one, so the mapping is deliberately not south-to-A.
static void
app_gamepad_button(app_runtime_t *runtime, const Uint8 button, const bool pressed)
{
    if (NULL == runtime)
    {
        return;
    }

    pound_input_button_t target;

    switch ((SDL_GamepadButton)button)
    {
        case SDL_GAMEPAD_BUTTON_SOUTH:
            target = POUND_INPUT_BUTTON_B;
            break;
        case SDL_GAMEPAD_BUTTON_EAST:
            target = POUND_INPUT_BUTTON_A;
            break;
        case SDL_GAMEPAD_BUTTON_WEST:
            target = POUND_INPUT_BUTTON_X;
            break;
        case SDL_GAMEPAD_BUTTON_NORTH:
            target = POUND_INPUT_BUTTON_Y;
            break;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
            target = POUND_INPUT_BUTTON_L;
            break;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
            target = POUND_INPUT_BUTTON_R;
            break;
        case SDL_GAMEPAD_BUTTON_BACK:
            target = POUND_INPUT_BUTTON_MINUS;
            break;
        case SDL_GAMEPAD_BUTTON_START:
            target = POUND_INPUT_BUTTON_PLUS;
            break;
        case SDL_GAMEPAD_BUTTON_GUIDE:
            target = POUND_INPUT_BUTTON_HOME;
            break;
        case SDL_GAMEPAD_BUTTON_MISC1:
            target = POUND_INPUT_BUTTON_CAPTURE;
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_UP:
            target = POUND_INPUT_BUTTON_DPAD_UP;
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
            target = POUND_INPUT_BUTTON_DPAD_DOWN;
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
            target = POUND_INPUT_BUTTON_DPAD_LEFT;
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
            target = POUND_INPUT_BUTTON_DPAD_RIGHT;
            break;
        case SDL_GAMEPAD_BUTTON_LEFT_STICK:
            target = POUND_INPUT_BUTTON_STICK_LEFT;
            break;
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
            target = POUND_INPUT_BUTTON_STICK_RIGHT;
            break;
        default:
            return;
    }

    (void)pound_input_set_button(&runtime->input, target, pressed);
}

// -----------------------------------------------------------------------------
// Video
// -----------------------------------------------------------------------------

static bool
app_video_init(app_t *app)
{
    if (NULL == app)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: app context is NULL.");
        return false;
    }

    if (0 == SDL_WasInit(SDL_INIT_VIDEO))
    {
        POUND_LOG_WARN(&thread_logger,
                       "SDL video subsystem was not initialised. Attempting SDL_InitSubSystem.");

        if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
        {
            POUND_LOG_ERROR(
                &thread_logger,
                "Aborting function: failed to initialise SDL video subsystem because %s.",
                SDL_GetError());
            return false;
        }
    }

    POUND_LOG_DEBUG(&thread_logger, "Configuring video subsystem...");

    if (!SDL_SetHint(SDL_HINT_APP_NAME, "Compound Emulator"))
    {
        POUND_LOG_WARN(
            &thread_logger, "Failed to set SDL application name because %s.", SDL_GetError());
    }

    const struct
    {
        SDL_GLAttr  attr;
        int         value;
        const char *name;
    } gl_attributes[] = {
        { .attr  = SDL_GL_CONTEXT_MAJOR_VERSION,
          .value = 3,
          .name  = "SDL_GL_CONTEXT_MAJOR_VERSION" },
        { .attr  = SDL_GL_CONTEXT_MINOR_VERSION,
          .value = 3,
          .name  = "SDL_GL_CONTEXT_MINOR_VERSION" },
        { .attr  = SDL_GL_CONTEXT_PROFILE_MASK,
          .value = SDL_GL_CONTEXT_PROFILE_CORE,
          .name  = "SDL_GL_CONTEXT_PROFILE_MASK" },
        { .attr  = SDL_GL_CONTEXT_FLAGS,
          .value = SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG,
          .name  = "SDL_GL_CONTEXT_FLAGS" },
        { .attr = SDL_GL_DOUBLEBUFFER, .value = 1, .name = "SDL_GL_DOUBLEBUFFER" },
        { .attr = SDL_GL_DEPTH_SIZE, .value = 24, .name = "SDL_GL_DEPTH_SIZE" },
        { .attr = SDL_GL_STENCIL_SIZE, .value = 8, .name = "SDL_GL_STENCIL_SIZE" },
    };

    for (size_t i = 0; i < (sizeof(gl_attributes) / sizeof(gl_attributes[0])); ++i)
    {
        if (!SDL_GL_SetAttribute(gl_attributes[i].attr, gl_attributes[i].value))
        {
            POUND_LOG_WARN(&thread_logger,
                           "Failed to set OpenGL attribute %s because %s.",
                           gl_attributes[i].name,
                           SDL_GetError());
        }
    }

    app->window = SDL_CreateWindow("Compound Emulator",
                                   1270,
                                   720,
                                   SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE
                                       | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);

    if (NULL == app->window)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: failed to create window because %s.",
                        SDL_GetError());
        return false;
    }

    app->gl_context = SDL_GL_CreateContext(app->window);

    if (NULL == app->gl_context)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: failed to create OpenGL context because %s.",
                        SDL_GetError());
        return false;
    }

    if (false == SDL_GL_MakeCurrent(app->window, app->gl_context))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: failed to make OpenGL context current because %s.",
                        SDL_GetError());
        return false;
    }

    if (imgl3wInit() != GL3W_OK)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: failed to initialise OpenGL loader (imgl3wInit).");
        return false;
    }

    if (false == SDL_GL_SetSwapInterval(1))
    {
        POUND_LOG_WARN(&thread_logger,
                       "Failed to set swap interval because %s. "
                       "Frames will be paced by app_frame_limit instead.",
                       SDL_GetError());
    }

    POUND_LOG_INFO(&thread_logger, "Successfully configured video subsystem.");
    return true;
}

static bool
app_video_shutdown(app_t *app)
{
    if (NULL == app)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: app context is NULL.");
        return false;
    }

    bool ok = true;

    // Tear the context down before the window: a live context still references
    // the window, and destroying the window first leaves a dangling context.
    if (app->gl_context != NULL)
    {
        if (false == SDL_GL_MakeCurrent(app->window, app->gl_context))
        {
            POUND_LOG_WARN(&thread_logger,
                           "SDL_GL_MakeCurrent(window, NULL) failed because %s.",
                           SDL_GetError());
            ok = false;
        }

        if (false == SDL_GL_DestroyContext(app->gl_context))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "SDL_GL_DestroyContext failed because %s.",
                            SDL_GetError());
            ok = false;
        }

        app->gl_context = NULL;
    }

    if (app->window != NULL)
    {
        // SDL_DestroyWindow returns void, so unlike the context teardown above
        // there is no status to branch on and nothing to report. The pointer is
        // cleared unconditionally so a later shutdown cannot double free.
        SDL_DestroyWindow(app->window);
        app->window = NULL;
    }

    return ok;
}

// -----------------------------------------------------------------------------
// ImGui
// -----------------------------------------------------------------------------

static bool
app_gui_init(app_t *app)
{
    if (NULL == app)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: app context is NULL.");
        return false;
    }

    if (NULL == app->window)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: SDL window is NULL.");
        return false;
    }

    if (NULL == app->gl_context)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: OpenGL context is NULL.");
        return false;
    }

    POUND_LOG_DEBUG(&thread_logger, "Configuring UI subsystem...");
    app->imgui_context = igCreateContext(NULL);

    if (NULL == app->imgui_context)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: failed to create ImGui context.");
        return false;
    }

    if (false == ImGui_ImplSDL3_InitForOpenGL(app->window, app->gl_context))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: failed to initialize ImGui SDL3 backend.");
        igDestroyContext(app->imgui_context);
        app->imgui_context = NULL;
        return false;
    }

    if (false == ImGui_ImplOpenGL3_Init("#version 330"))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: failed to initialize OpenGL3 backend.");
        ImGui_ImplSDL3_Shutdown();
        igDestroyContext(app->imgui_context);
        app->imgui_context = NULL;
        return false;
    }

    ImGuiIO *io = igGetIO_ContextPtr(app->imgui_context);

    if (NULL == io)
    {
        POUND_LOG_WARN(&thread_logger,
                       "ImGui IO is unavailable after initialisation, continuing with defaults.");
    }
    else
    {
        io->ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io->ConfigErrorRecovery             = true;
        io->ConfigErrorRecoveryEnableAssert = false;
    }

    igStyleColorsDark(NULL);
    POUND_LOG_INFO(&thread_logger, "Successfully configured GUI subsystem.");
    return true;
}

static bool
app_gui_shutdown(app_t *app)
{
    if (NULL == app)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: app context is NULL.");
        return false;
    }

    // Unload the plugin even when ImGui never came up: on the failure path the
    // module may already be mapped and would otherwise leak its image.
    gui_plugin_destroy(&app->gui);

    if (NULL == app->imgui_context)
    {
        return false;
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    igDestroyContext(app->imgui_context);
    app->imgui_context = NULL;
    return true;
}

// -----------------------------------------------------------------------------
// GUI plugin lifecycle
// -----------------------------------------------------------------------------

static bool
app_gui_update(app_t *app, const bool force)
{
    if (POUND_UNLIKELY(NULL == app))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: app context is NULL.");
        return false;
    }

#if !POUND_PLATFORM_SUPPORTS_HOT_RELOAD

    // There is no reload mechanism on this platform, so a GUI that app_gui_bind_static()
    // already resolved stays valid for the lifetime of the process. Bailing out here
    // also keeps app->gui_source_path permanently empty, which is what stops the
    // reload path below from trying to dlopen a file that does not exist.
    if (true == app->gui.loaded)
    {
        return true;
    }

#endif

    if (POUND_UNLIKELY(0 == app->gui_source_path[0]))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: GUI plugin path is empty.");
        return false;
    }

    const uint64_t source_time = file_modified_time(app->gui_source_path);

    if (0 == source_time)
    {
        app->gui_pending_time = 0;

        if (true == app->gui.loaded)
        {
            return true;
        }

        app->gui_source_time = 0;
        return false;
    }

    if (false == force)
    {
        if (source_time == app->gui_source_time)
        {
            return app->gui.loaded;
        }

        // Delay reload so we do not load while the linker is working its magic.
        if (source_time != app->gui_pending_time)
        {
            app->gui_pending_time  = source_time;
            app->gui_pending_since = SDL_GetTicks();
            return app->gui.loaded;
        }

        if ((SDL_GetTicks() - app->gui_pending_since) < GUI_RELOAD_POLL_INTERVAL_MS)
        {
            return app->gui.loaded;
        }
    }

    app->gui_pending_time = 0;

    // Save old plugin state.

    void  *hot_reloaded_code      = NULL;
    size_t hot_reloaded_code_size = 0;

    if (app->gui.loaded && app->gui.gui_context && app->gui.exports.save)
    {
        const gui_plugin_error_t error
            = app->gui.exports.save(app->gui.gui_context, NULL, 0, &hot_reloaded_code_size);

        if (GUI_PLUGIN_SUCCESS == error && hot_reloaded_code_size > 0)
        {
            const size_t memory_alignment = 8U;
            hot_reloaded_code            = memory_subsystem_allocate(memory_alignment,
                                                             hot_reloaded_code_size);

            if (hot_reloaded_code != NULL)
            {
                const gui_plugin_error_t write_error = app->gui.exports.save(
                    app->gui.gui_context, hot_reloaded_code, hot_reloaded_code_size,
                    &hot_reloaded_code_size);

                if (GUI_PLUGIN_SUCCESS != write_error)
                {
                    POUND_LOG_ERROR(&thread_logger,
                                    "Failed to save GUI state because %s.",
                                    gui_plugin_error_to_string(write_error));
                    memory_subsystem_free(hot_reloaded_code);
                    hot_reloaded_code      = NULL;
                    hot_reloaded_code_size = 0;
                }
            }
            else
            {
                POUND_LOG_ERROR(&thread_logger,
                                "Failed to allocate %zu bytes for GUI state save.",
                                hot_reloaded_code_size);
                hot_reloaded_code_size = 0;
            }
        }
        else if (GUI_PLUGIN_SUCCESS != error)
        {
            POUND_LOG_WARN(&thread_logger,
                           "GUI state size query failed because %s.",
                           gui_plugin_error_to_string(error));
        }
        else
        {
            // The plugin reported success with a zero-byte state: nothing to carry over.
            POUND_LOG_DEBUG(&thread_logger, "GUI plugin reported an empty state blob.");
        }
    }

    // Save ImGui layout.

    char       *ini_copy = NULL;
    const char *ini      = igSaveIniSettingsToMemory(NULL);

    if (ini != NULL)
    {
        const size_t ini_length       = strlen(ini);
        const size_t memory_alignment = 8U;
        ini_copy                      = memory_subsystem_allocate(memory_alignment, ini_length + 1);

        if (ini_copy != NULL)
        {
            memcpy(ini_copy, ini, ini_length);
            ini_copy[ini_length] = '\0';
        }
        else
        {
            POUND_LOG_WARN(&thread_logger,
                           "Failed to allocate %zu bytes to preserve ImGui layout.",
                           ini_length + 1);
        }
    }

    bool         ok   = false;
    gui_plugin_t next = { 0 };

    if (true == gui_plugin_load_module(&next, app->gui_source_path))
    {
        void                    *next_gui_context = NULL;
        const gui_plugin_error_t error
            = next.exports.create(hot_reloaded_code, hot_reloaded_code_size, &next_gui_context);

        if (GUI_PLUGIN_SUCCESS == error && next_gui_context != NULL)
        {
            gui_plugin_t old     = app->gui;
            app->gui             = next;
            app->gui.gui_context = next_gui_context;
            app->gui.loaded      = true;

            gui_plugin_destroy(&old);

            if (ini_copy != NULL)
            {
                const size_t ini_size = strlen(ini_copy);
                igLoadIniSettingsFromMemory(ini_copy, ini_size);
                memory_subsystem_free(ini_copy);
                ini_copy = NULL;
            }

            app_gui_unfreeze(app);

            POUND_LOG_INFO(&thread_logger, "Loaded GUI plugin at %s", app->gui_source_path);
            ok = true;
        }
        else
        {
            POUND_LOG_ERROR(&thread_logger,
                            "Failed to reload GUI plugin at %s because %s.",
                            app->gui_source_path,
                            gui_plugin_error_to_string(error));
            gui_plugin_destroy(&next);
        }
    }
    else
    {
        POUND_LOG_ERROR(&thread_logger, "Failed to load GUI plugin at %s.", app->gui_source_path);
    }

    memory_subsystem_free(hot_reloaded_code);
    memory_subsystem_free(ini_copy);
    app->gui_source_time = source_time;
    return ok;
}

static void
app_gui_freeze(app_t *app, const char *reason)
{
    if (NULL == app)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: app context is NULL.");
        return;
    }

    if (NULL == reason)
    {
        reason = "Unknown";
    }

    if (true == app->gui_frozen)
    {
        if (0 != strncmp(app->gui_frozen_reason, reason, sizeof(app->gui_frozen_reason)))
        {
            const int written
                = snprintf(app->gui_frozen_reason, sizeof(app->gui_frozen_reason), "%s", reason);

            if (written < 0 || (size_t)written >= sizeof(app->gui_frozen_reason))
            {
                app->gui_frozen_reason[sizeof(app->gui_frozen_reason) - 1] = '\0';
                POUND_LOG_WARN(&thread_logger, "GUI freeze reason was truncated.");
            }

            POUND_LOG_DEBUG(&thread_logger, "GUI freeze reason updated to %s", reason);
        }
        return;
    }

    POUND_LOG_ERROR(&thread_logger, "Freezing GUI because %s.", reason);
    app->gui_frozen = true;
    const int written
        = snprintf(app->gui_frozen_reason, sizeof(app->gui_frozen_reason), "%s", reason);

    if (written < 0 || (size_t)written >= sizeof(app->gui_frozen_reason))
    {
        app->gui_frozen_reason[sizeof(app->gui_frozen_reason) - 1] = '\0';
        POUND_LOG_WARN(&thread_logger, "GUI freeze reason was truncated.");
    }
}

static void
app_gui_unfreeze(app_t *app)
{
    if (NULL == app)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: app context is NULL.");
        return;
    }

    if (app->gui_frozen)
    {
        POUND_LOG_INFO(&thread_logger, "GUI hot reload succeeded; unfreezing GUI.");
    }

    app->gui_frozen           = false;
    app->gui_frozen_reason[0] = '\0';
}

// -----------------------------------------------------------------------------
// Hot reload
// -----------------------------------------------------------------------------

static bool
app_hot_reload_init(app_t *app)
{
    if (NULL == app)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: app context is NULL.");
        return false;
    }

#if !POUND_PLATFORM_SUPPORTS_HOT_RELOAD

    // Android ships CompoundGui as a static library: SDL_GetBasePath() resolves
    // inside the read-only APK, so there is nowhere writable to copy a freshly
    // linked image to and no way to dlopen an arbitrary path. The same
    // `gui_plugin_exports_t` vtable the loader would have populated is resolved
    // directly instead, which keeps every downstream call site identical.
    POUND_LOG_INFO(&thread_logger,
                   "Hot reloading is unavailable on %s; binding the statically linked GUI.",
                   POUND_PLATFORM_NAME);

    return app_gui_bind_static(app);

#else

    const char *base = SDL_GetBasePath();

    if (NULL == base)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: failed to get SDL base path.");
        return false;
    }

    const char  *separator   = "";
    const size_t base_length = strlen(base);

    if ((base_length > 0) && (base[base_length - 1] != '/') && (base[base_length - 1] != '\\'))
    {
        separator = "/";
    }

    const int written = snprintf(app->gui_source_path,
                                 sizeof(app->gui_source_path),
                                 "%s%s%s",
                                 base,
                                 separator,
                                 GUI_PLUGIN_NAME);

    if ((written < 0) || ((size_t)written >= sizeof(app->gui_source_path)))
    {
        app->gui_source_path[0] = '\0';
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: GUI plugin path exceeded %zu bytes.",
                        sizeof(app->gui_source_path));
        return false;
    }

    app->gui_source_time   = file_modified_time(app->gui_source_path);
    app->gui_pending_time  = 0;
    app->gui_pending_since = 0;
    POUND_LOG_INFO(&thread_logger, "Found GUI plugin at %s", app->gui_source_path);
    POUND_LOG_DEBUG(&thread_logger, "Hot reloading is enabled.");
    return true;

#endif // !POUND_PLATFORM_SUPPORTS_HOT_RELOAD
}

#if !POUND_PLATFORM_SUPPORTS_HOT_RELOAD

/// Binds the statically linked GUI into `app->gui`.
///
/// The plugin contract is identical to the hot-reload path; only the mechanism
/// that obtains the vtable differs. `module` stays NULL so
/// `gui_plugin_destroy` skips the `dlclose`, and `loaded_path` stays empty so
/// nothing is unlinked from disk.
///
/// Compiled only where hot reload is unavailable: on the platforms that do
/// support it the vtable always comes from `gui_plugin_load_module`, and a
/// definition with no caller would be dead code the compiler rightly rejects
/// under -Wunused-function.
static bool
app_gui_bind_static(app_t *app)
{
    if (NULL == app)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: app context is NULL.");
        return false;
    }

    if (true == app->gui.loaded)
    {
        return true;
    }

    gui_plugin_exports_t exports = { 0 };

    const gui_plugin_error_t error = gui_plugin_exports_get(&exports);

    if (GUI_PLUGIN_SUCCESS != error)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: gui_plugin_exports_get() returned %s.",
                        gui_plugin_error_to_string(error));
        return false;
    }

    if ((NULL == exports.create) || (NULL == exports.destroy) || (NULL == exports.render_frame)
        || (NULL == exports.save))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the statically linked GUI exported an incomplete "
                        "callback table.");
        return false;
    }

    void                    *gui_context = NULL;
    const gui_plugin_error_t create_error
        = exports.create(NULL, 0U, &gui_context);

    if ((GUI_PLUGIN_SUCCESS != create_error) || (NULL == gui_context))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: creating the GUI context failed because %s.",
                        gui_plugin_error_to_string(create_error));
        return false;
    }

    app->gui.exports     = exports;
    app->gui.module      = NULL;
    app->gui.gui_context = gui_context;
    app->gui.loaded      = true;
    app->gui.loaded_path[0] = '\0';

    POUND_LOG_INFO(&thread_logger, "Bound the statically linked GUI at %s.", POUND_PLATFORM_NAME);
    return true;
}

#endif // !POUND_PLATFORM_SUPPORTS_HOT_RELOAD

static bool
app_hot_reload_shutdown(app_t *app)
{
    if (NULL == app)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: app context is NULL.");
        return false;
    }

    gui_plugin_destroy(&app->gui);

    app->gui_reload_request   = false;
    app->gui_frozen           = false;
    app->gui_source_time      = 0;
    app->gui_pending_time     = 0;
    app->gui_pending_since    = 0;
    app->gui_source_path[0]   = '\0';
    app->gui_frozen_reason[0] = '\0';

    return true;
}

// -----------------------------------------------------------------------------
// Frame
// -----------------------------------------------------------------------------

static void
app_poll_events(app_t *app, app_runtime_t *runtime)
{
    if (NULL == app)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: app context is NULL.");
        return;
    }

    if (NULL == app->window)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: SDL window is NULL.");
        return;
    }

    SDL_Event event;

    while (SDL_PollEvent(&event))
    {
        ImGui_ImplSDL3_ProcessEvent(&event);

        switch (event.type)
        {
            case SDL_EVENT_QUIT: {
                POUND_LOG_INFO(&thread_logger, "Received SDL quit event.");
                app->running = false;
                if (NULL != runtime)
                {
                    pound_platform_services_request_shutdown(&runtime->services);
                }
                break;
            }

            case SDL_EVENT_WINDOW_CLOSE_REQUESTED: {
                if (event.window.windowID == SDL_GetWindowID(app->window))
                {
                    POUND_LOG_INFO(&thread_logger, "Received window close request.");
                    app->running = false;
                    if (NULL != runtime)
                    {
                        pound_platform_services_request_shutdown(&runtime->services);
                    }
                }
                break;
            }

            case SDL_EVENT_KEY_DOWN: {
                if (SDL_SCANCODE_F5 == event.key.scancode)
                {
                    POUND_LOG_DEBUG(&thread_logger, "F5 pressed; GUI reload requested.");
                    app->gui_reload_request = true;
                }
                break;
            }

            case SDL_EVENT_GAMEPAD_ADDED: {
                if (NULL != runtime)
                {
                    POUND_LOG_INFO(&thread_logger, "A gamepad was added; rescanning.");
                    app_gamepad_scan(runtime);
                }
                break;
            }

            case SDL_EVENT_GAMEPAD_REMOVED: {
                if ((NULL != runtime) && (NULL != runtime->gamepad)
                    && (event.gdevice.which == SDL_GetGamepadID(runtime->gamepad)))
                {
                    POUND_LOG_INFO(&thread_logger, "The active gamepad was removed.");
                    app_gamepad_scan(runtime);
                }
                break;
            }

            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP: {
                if ((NULL != runtime) && (NULL != runtime->gamepad)
                    && (event.gbutton.which == SDL_GetGamepadID(runtime->gamepad)))
                {
                    app_gamepad_button(runtime,
                                       event.gbutton.button,
                                       SDL_EVENT_GAMEPAD_BUTTON_DOWN == event.type);
                }
                break;
            }

            default: {
                break;
            }
        }
    }

    if (NULL != runtime)
    {
        app_poll_keyboard(runtime);
        app_poll_gamepad(runtime);
    }
}

static void
app_render_frame(app_t *app)
{
    if (POUND_UNLIKELY(NULL == app))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: app context is NULL.");
        return;
    }

    if (POUND_UNLIKELY(NULL == app->window))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: SDL window is NULL.");
        app->running = false;
        return;
    }

    if (POUND_UNLIKELY(NULL == app->gl_context))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: OpenGL context is NULL.");
        app->running = false;
        return;
    }

    if (POUND_UNLIKELY(false == app_gui_update(app, app->gui_reload_request)))
    {
        if (false == app->gui_frozen)
        {
            app_gui_freeze(app, "GUI update failed.");
        }
    }

    app->gui_reload_request = false;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    igNewFrame();

    if (app->gui_frozen)
    {
        app_render_frozen_overlay(app);
    }
    else if (app->gui.loaded && app->gui.gui_context && app->gui.exports.render_frame)
    {
        app->gui.exports.render_frame(app->gui.gui_context);
    }
    else
    {
        app_gui_freeze(app, "GUI plugin state became invalid.");
        app_render_frozen_overlay(app);
    }

    igRender();

    glClearColor(0.06f, 0.06f, 0.06f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(igGetDrawData());
    SDL_GL_SwapWindow(app->window);
}

static void
app_render_frozen_overlay(app_t *app)
{
    if (NULL == app)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: app context is NULL.");
        return;
    }

    if (NULL == app->imgui_context)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: ImGui context is NULL.");
        return;
    }

    ImGuiIO *io = igGetIO_ContextPtr(app->imgui_context);

    if (NULL == io)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "ImGui IO is unavailable; cannot render frozen GUI overlay.");
        return;
    }

    ImVec2 zero;
    zero.x = 0.0f;
    zero.y = 0.0f;

    igSetNextWindowPos(zero, ImGuiCond_Always, zero);
    igSetNextWindowSize(io->DisplaySize, ImGuiCond_Always);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                                   | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
                                   | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;

    if (igBegin("Compound GUI Frozen", NULL, flags))
    {
        igText("GUI is frozen due to a hot reload failure.");
        igSeparator();

        igText("Source plugin:");
        igTextWrapped("%s", app->gui_source_path[0] != '\0' ? app->gui_source_path : "<unknown>");
        igSeparator();

        igText("Reason:");
        igTextWrapped("%s", app->gui_frozen_reason[0] != '\0' ? app->gui_frozen_reason : "Unknown");
        igSeparator();

        igText("The GUI will remain frozen until a hot reload succeeds.");
        igText("Press F5 to force a reload.");
    }

    igEnd();
}

// -----------------------------------------------------------------------------
// Frame pacing
// -----------------------------------------------------------------------------

static void
app_frame_limit(const uint64_t frame_start_ms)
{
    // Desktop targets are paced by the vsync established in app_video_init.
    // Android ignores SDL_GL_SetSwapInterval, so without this the loop would
    // spin at whatever rate the GPU drains, which is both a battery hazard and
    // a source of input latency on 120 Hz panels.
#if POUND_PLATFORM_ANDROID

    /// Upper bound on the main loop period. `SDL_Delay` is skipped entirely when
    /// a frame already overran this budget, so a slow frame degrades into a
    /// dropped frame rather than an unbounded catch-up burst.
    const uint64_t frame_budget_ms = 250U;
    const uint64_t elapsed         = SDL_GetTicks() - frame_start_ms;

    if (elapsed < frame_budget_ms)
    {
        SDL_Delay(frame_budget_ms - elapsed);
    }

#else

    POUND_UNUSED(frame_start_ms);

#endif
}

// -----------------------------------------------------------------------------
// Memory self-test
// -----------------------------------------------------------------------------

// Slowly allocate and deallocate memory over 10 seconds.
// This is meant to test the GUI memory debug tracker.
static void
app_memory_churn(void)
{
    enum
    {
        BLOCK_MAX = 128
    };

    static void    *blocks[BLOCK_MAX] = { 0 };
    static size_t   count             = 0;
    static uint64_t start_ms          = 0;

    const size_t   block_size  = 256 * 1024;
    const uint64_t duration_ms = 10000;
    const uint64_t half_ms     = duration_ms / 2;
    const size_t   max_step    = 4;

    const uint64_t now = SDL_GetTicks();

    if (0 == start_ms)
    {
        start_ms = now;
    }

    const uint64_t elapsed = (now - start_ms) % duration_ms;

    size_t target;

    if (elapsed < half_ms)
    {
        target = (size_t)((elapsed * BLOCK_MAX) / half_ms);
    }
    else
    {
        target = BLOCK_MAX - (size_t)(((elapsed - half_ms) * BLOCK_MAX) / half_ms);
    }

    if (target > BLOCK_MAX)
    {
        target = BLOCK_MAX;
    }

    size_t step = 0;

    while (count < target && step < max_step)
    {
        const size_t memory_alignment = 8U;
        void        *p                = memory_subsystem_allocate(memory_alignment, block_size);

        if (NULL == p)
        {
            break;
        }

        memset(p, 0xCD, block_size);
        blocks[count++] = p;
        ++step;
    }

    while (count > target && step < max_step)
    {
        memory_subsystem_free(blocks[--count]);
        blocks[count] = NULL;
        ++step;
    }
}

/// The churn loop is a diagnostic for the memory tracker, not emulator work.
/// Running it unconditionally would burn 32 MiB of bandwidth per cycle in
/// release builds, so it is opt-in via `--selftest-memory` or
/// `POUND_SELFTEST_MEMORY=1`.
static bool
app_memory_churn_requested(int argc, char *argv[])
{
    static int cached = -1;

    if (cached < 0)
    {
        cached = 0;

        for (int i = 1; i < argc; ++i)
        {
            if (NULL != argv[i] && (0 == SDL_strcmp(argv[i], "--selftest-memory")))
            {
                cached = 1;
                break;
            }
        }

        if (0 == cached)
        {
            const char *env = SDL_getenv("POUND_SELFTEST_MEMORY");

            if ((env != NULL) && (0 != SDL_strcmp(env, "0")))
            {
                cached = 1;
            }
        }
    }

    return cached == 1;
}

/*** end of file ***/

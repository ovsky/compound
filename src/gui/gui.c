#include "gui.h"

#include "config/config.h"
#include "debug/debug_memory.h"
#include "log.h"
#include "log_ring.h"
#include "memory/memory.h"
#include "theme/fonts.h"
#include "theme/theme.h"
#include "theme/theme_imgui.h"

#include <stdio.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "gui_layout.h"
#include "pages/pages.h"

#include <SDL3/SDL.h>
#include <cimgui.h>

/// The shell.
///
/// Three fixed regions and a stack of optional floating windows. The left strip is
/// the sidebar, the top strip is the title bar, and everything left over is the
/// page the user selected. `gui_layout.c` computes the rectangles from fractions
/// of the window, so the same code produces a phone layout and a desktop one
/// without a second set of constants.
///
/// The region model is the reason the old panel registry is gone. That design let
/// a caller register an arbitrary number of dockable windows, which meant the
/// shell could not guarantee any of them were on screen, and a saved state that
/// named a panel index stopped meaning anything as soon as a panel was added. A
/// shell has a sidebar and a title bar whether or not a page is open; encoding
/// that as fixed registers rather than as a registry makes the "the interface has
/// no chrome" failure unrepresentable.
///
/// What this file owns, and nothing else does:
///
///  * The lifetime of the config, the theme, the font atlas, the log ring and the
///    page view state.
///  * Turning a changed config into a changed running interface.
///  * Serialising the part of that state a hot reload should carry across.
///
/// What it deliberately does not own: what any page draws. That is `pages.c`, and
/// the contract between the two is `pages.h`.

/// The version shown in the sidebar. Defaulted rather than required so this file
/// still compiles in a translation unit built without the definition.
#ifndef POUND_VERSION
#define POUND_VERSION "unknown"
#endif

/// The sidebar's width at a scale of 1.0, in logical pixels.
#define GUI_SIDEBAR_BASE_WIDTH 232.0F

/// The title bar's height at a scale of 1.0, in logical pixels.
#define GUI_TITLE_BASE_HEIGHT 54.0F

/// Bounds on the fraction each chrome region may claim.
///
/// A fraction rather than a pixel width because the layout divides the window, and
/// a very small window has to keep a usable page: without a floor, a narrow window
/// would hand almost all of itself to the sidebar; without a ceiling, a wide one
/// would grow the sidebar until it was mostly empty.
#define GUI_SIDEBAR_MIN_FRACTION 0.14F
#define GUI_SIDEBAR_MAX_FRACTION 0.34F
#define GUI_TITLE_MIN_FRACTION 0.05F
#define GUI_TITLE_MAX_FRACTION 0.16F

/// Identifies a saved-state blob as Pound's.
#define GUI_SAVED_STATE_MAGIC 0x504E4455U

/// Bumped whenever the saved fields change meaning.
///
/// A blob from a different version is ignored rather than reinterpreted: the whole
/// point of versioning this is that reading an old layout as if it were the new
/// one is how a saved state turns into a crash instead of a reset.
#define GUI_SAVED_STATE_VERSION 2U

/// The durable part of the interface, written on a hot reload and read back.
///
/// Plain scalars only. The previous format copied the whole `gui_state_t`, which
/// held the config, a pointer into ImGui's font atlas and a mutex; restoring it
/// into a freshly loaded image would have reproduced pointers from the image the
/// loader had just unmapped. Scalars cannot dangle, and the font atlas is rebuilt
/// from the scale rather than restored.
typedef struct
{
    uint32_t magic;
    uint32_t version;
    int32_t  current_page;
    int32_t  theme_mode;
    int32_t  log_level;
    int32_t  ui_scale_milli;
    int32_t  log_min_level;
    int32_t  log_auto_scroll;
    int32_t  show_debug_menu;
    int32_t  show_hot_reload_guide;
    int32_t  show_imgui_demo;
} gui_saved_state_t;

/// Everything the shell keeps between frames.
///
/// The member order is chosen so the struct has no *implicit* padding, which the
/// build treats as a portability defect: eight-byte members first, then the
/// four-byte-aligned theme and view, then the byte array and the flags, with an
/// explicit four-byte tail. Reordering these is not free -- check the padding
/// warning before committing a new field in the middle.
typedef struct
{
    /// Settings, loaded from disk and edited live by the Settings page.
    pound_config_t config;

    /// The one font every role draws with.
    pound_fonts_t fonts;

    /// The Logs page's snapshot buffer, owned here and reused every frame.
    pound_log_record_t *log_scratch;

    /// Guest-memory visualisation state, rendered by the Debug window.
    debug_memory_tracker_t memory_tracker;

    /// The ring the Logs page reads back.
    pound_log_ring_t log_ring;

    /// The active palette.
    pound_theme_t theme;

    /// Scroll position and log filter, owned by the pages.
    pound_pages_view_t view;

    /// Where the Settings page saves to and loads from.
    char settings_path[POUND_CONFIG_PATH_MAX];

    /// The page the content region is showing.
    int current_page;

    /// Whether the floating Debug window is open.
    bool show_debug_menu;

    /// Whether the hot-reload help window is open.
    bool show_hot_reload_guide;

    /// Whether Dear ImGui's own demo window is open.
    bool show_imgui_demo;

    /// Set when the theme or the scale changed and the running interface has not
    /// picked the change up yet. Cleared at the top of the next frame.
    bool config_appearance_dirty;

    /// Explicit tail padding; see the note on member order.
    char pad[4];
} gui_state_t;

static gui_plugin_error_t gui_create(const void *saved_data, size_t saved_size, void **out);
static gui_plugin_error_t gui_destroy(void *gui_state);
static gui_plugin_error_t gui_render_frame(void *gui_state);
static gui_plugin_error_t gui_save(void   *gui_state,
                                   void   *out_gui,
                                   size_t  capacity,
                                   size_t *out_size);

static void gui_resolve_settings_path(gui_state_t *POUND_RESTRICT state);
static void gui_apply_saved_state(gui_state_t *POUND_RESTRICT state,
                                  const void   *POUND_RESTRICT saved_data,
                                  size_t                       saved_size);
static void gui_apply_appearance(gui_state_t *POUND_RESTRICT state);
static void gui_render_sidebar(gui_state_t *POUND_RESTRICT                  state,
                               const gui_layout_rectangle_t *POUND_RESTRICT rectangle,
                               float                                        origin_x,
                               float                                        origin_y);
static void gui_render_title_bar(gui_state_t *POUND_RESTRICT                  state,
                                 const gui_layout_rectangle_t *POUND_RESTRICT rectangle,
                                 float                                        origin_x,
                                 float                                        origin_y);
static void gui_render_content_window(gui_state_t *POUND_RESTRICT                  state,
                                      const gui_layout_rectangle_t *POUND_RESTRICT rectangle,
                                      float                                        origin_x,
                                      float                                        origin_y);
static void gui_render_debug_windows(gui_state_t *POUND_RESTRICT state);

gui_plugin_error_t
gui_plugin_exports_get(gui_plugin_exports_t *out)
{
    if (NULL == out)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: out is NULL.");
        return GUI_PLUGIN_ERROR_INVALID_ARGUMENT;
    }

    // As a separate shared image the plugin carries its own copy of core, and
    // therefore its own `thread_logger`, which starts out zero-initialised. Its
    // `log` member being NULL is what makes `pound_log_message` drop every
    // record, so a default sink is installed here -- before the first call the
    // host can make -- rather than letting the GUI fail silently. Assigning it
    // unconditionally is safe: `pound_logger_init_default()` only writes the two
    // fields, and the host may have configured a richer sink on *its* copy.
    if (NULL == thread_logger.log)
    {
        pound_logger_init_default();
    }

    out->create       = gui_create;
    out->destroy      = gui_destroy;
    out->render_frame = gui_render_frame;
    out->save         = gui_save;
    return GUI_PLUGIN_SUCCESS;
}

const char *
gui_plugin_error_to_string(const gui_plugin_error_t error)
{
    switch (error)
    {
        case GUI_PLUGIN_SUCCESS:
            return "a gui plugin operation was successful";
        case GUI_PLUGIN_ERROR_INVALID_ARGUMENT:
            return "a passed function argument was invalid";
        case GUI_PLUGIN_ERROR_ALLOCATION_FAILED:
            return "an allocator failed to allocate memory";
        case GUI_PLUGIN_ERROR_BUFFER_TOO_SMALL:
            return "a buffer was too small";
        case GUI_PLUGIN_ERROR_PANEL_REGISTRY_FULL:
            return "the panel registry is full";
    }
    return "UNKNOWN ERROR";
}

/// Names the settings file beside the rest of the user's Pound data.
///
/// SDL owns the platform rules -- `%APPDATA%` on Windows, XDG on Linux, the app
/// container on a phone -- so this asks it rather than reconstructing them. A
/// platform that cannot answer is not fatal: the fallback is a relative path,
/// which is correct for a portable build run from a stick.
static void
gui_resolve_settings_path(gui_state_t *POUND_RESTRICT state)
{
    state->settings_path[0] = '\0';

    char *const preference_path = SDL_GetPrefPath("Pound", "Pound");

    if (NULL == preference_path)
    {
        POUND_LOG_WARN(&thread_logger,
                       "SDL could not name a per-user settings directory (%s); falling back to "
                       "'pound.cfg' beside the working directory.",
                       SDL_GetError());
        snprintf(state->settings_path, sizeof(state->settings_path), "%s", "pound.cfg");
        return;
    }

    const int written
        = snprintf(state->settings_path, sizeof(state->settings_path), "%spound.cfg", preference_path);

    SDL_free(preference_path);

    if ((written < 0) || ((size_t)written >= sizeof(state->settings_path)))
    {
        POUND_LOG_WARN(&thread_logger,
                       "The per-user settings path does not fit in %zu bytes; falling back to "
                       "'pound.cfg' beside the working directory.",
                       sizeof(state->settings_path));
        snprintf(state->settings_path, sizeof(state->settings_path), "%s", "pound.cfg");
    }
}

/// Restores one saved setting, noting when a setter refused the stored value.
///
/// The setter is the one that logs *why* -- "theme 7 is not a theme" -- so this
/// adds only where the value came from. It exists rather than an ignored return
/// because a setting silently failing to restore is exactly the kind of bug a
/// saved state is supposed to prevent.
static void
gui_restore_note(const char *POUND_RESTRICT field, const error_t status)
{
    if (POUND_SUCCESS != status)
    {
        POUND_LOG_WARN(&thread_logger,
                       "The saved %s was refused; keeping the value currently in the config.",
                       field);
    }
}

static void
gui_apply_saved_state(gui_state_t *POUND_RESTRICT state,
                      const void   *POUND_RESTRICT saved_data,
                      const size_t                 saved_size)
{
    if (NULL == saved_data)
    {
        POUND_LOG_DEBUG(&thread_logger, "No saved interface state; starting from defaults.");
        return;
    }

    if (saved_size < sizeof(gui_saved_state_t))
    {
        POUND_LOG_WARN(&thread_logger,
                       "Saved interface state is %zu bytes, smaller than the %zu-byte record; "
                       "starting from defaults.",
                       saved_size,
                       sizeof(gui_saved_state_t));
        return;
    }

    gui_saved_state_t saved;
    memcpy(&saved, saved_data, sizeof(saved));

    if ((GUI_SAVED_STATE_MAGIC != saved.magic) || (GUI_SAVED_STATE_VERSION != saved.version))
    {
        POUND_LOG_WARN(&thread_logger,
                       "Saved interface state carries magic 0x%08X and version %u, not this "
                       "build's 0x%08X and %u; starting from defaults.",
                       (unsigned int)saved.magic,
                       (unsigned int)saved.version,
                       (unsigned int)GUI_SAVED_STATE_MAGIC,
                       (unsigned int)GUI_SAVED_STATE_VERSION);
        return;
    }

    if (true == pound_page_is_valid((int)saved.current_page))
    {
        state->current_page = (int)saved.current_page;
    }
    else
    {
        POUND_LOG_WARN(&thread_logger,
                       "The saved page %d is not a page; opening the Library instead.",
                       (int)saved.current_page);
    }

    gui_restore_note("theme",
                     pound_config_set_theme(&state->config, (pound_config_theme_t)saved.theme_mode));
    gui_restore_note("log level",
                     pound_config_set_log_level(&state->config, (log_level_t)saved.log_level));
    gui_restore_note("interface scale",
                     pound_config_set_ui_scale(&state->config,
                                               (float)saved.ui_scale_milli / 1000.0F));

    if ((saved.log_min_level >= (int32_t)LOG_LEVEL_NONE)
        && (saved.log_min_level <= (int32_t)LOG_LEVEL_TRACE))
    {
        state->view.log_min_level = (log_level_t)saved.log_min_level;
    }
    else
    {
        POUND_LOG_WARN(&thread_logger,
                       "The saved log floor %d is not a level; showing warnings and above.",
                       (int)saved.log_min_level);
    }

    state->view.log_auto_scroll    = (0 != saved.log_auto_scroll);
    state->show_debug_menu         = (0 != saved.show_debug_menu);
    state->show_hot_reload_guide   = (0 != saved.show_hot_reload_guide);
    state->show_imgui_demo         = (0 != saved.show_imgui_demo);
}

/// Pushes the config's appearance into the running interface.
///
/// The theme and the scale are applied together because they are one decision from
/// the user's point of view -- "how the interface looks" -- and a frame that drew
/// the new colours at the old size would be a visible glitch on every change.
static void
gui_apply_appearance(gui_state_t *POUND_RESTRICT state)
{
    const pound_theme_mode_t mode = (POUND_CONFIG_THEME_LIGHT == state->config.theme)
                                      ? POUND_THEME_LIGHT
                                      : POUND_THEME_DARK;

    if (POUND_SUCCESS != pound_theme_set_mode(&state->theme, mode))
    {
        return;
    }

    if (POUND_SUCCESS != pound_theme_imgui_apply(&state->theme))
    {
        return;
    }

    if (POUND_SUCCESS != pound_fonts_set_scale(&state->fonts, state->config.ui_scale))
    {
        return;
    }
}

static gui_plugin_error_t
gui_create(const void *POUND_RESTRICT saved_data, const size_t saved_size, void **out)
{
    if (NULL == out)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: out is NULL.");
        return GUI_PLUGIN_ERROR_INVALID_ARGUMENT;
    }

    *out = NULL;

    if ((saved_size > 0U) && (NULL == saved_data))
    {
        POUND_LOG_WARN(&thread_logger,
                       "saved_size is %zu but saved_data is NULL; ignoring saved state.",
                       saved_size);
    }

    const size_t memory_alignment = 8U;
    gui_state_t *POUND_RESTRICT state
        = memory_subsystem_allocate(memory_alignment, sizeof(gui_state_t));

    if (POUND_UNLIKELY(NULL == state))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: failed to allocate gui_state_t (%zu bytes).",
                        sizeof(gui_state_t));
        return GUI_PLUGIN_ERROR_ALLOCATION_FAILED;
    }

    memset(state, 0, sizeof(gui_state_t));

    // Defaults first, so every field is meaningful before a file or a saved blob is
    // allowed to change it. `config_appearance_dirty` starts set because the first
    // frame has to push the palette and the scale even when nothing saved changed.
    state->current_page               = (int)POUND_PAGE_LIBRARY;
    state->config_appearance_dirty    = true;
    state->memory_tracker.first_time_run = true;
    state->view.log_min_level         = LOG_LEVEL_WARN;
    state->view.log_auto_scroll       = true;
    state->view.settings_status[0]    = '\0';
    state->view.settings_status_is_error = false;

    if (POUND_SUCCESS != pound_theme_init(&state->theme))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the theme could not be initialised; refusing to draw "
                        "an unstyled interface.");
        memory_subsystem_free(state);
        return GUI_PLUGIN_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_SUCCESS != pound_config_set_defaults(&state->config))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the config defaults could not be loaded.");
        memory_subsystem_free(state);
        return GUI_PLUGIN_ERROR_INVALID_ARGUMENT;
    }

    gui_resolve_settings_path(state);

    if (POUND_SUCCESS != pound_config_load(&state->config, state->settings_path))
    {
        // A syntax error still applied the lines that parsed; only a missing or
        // unreadable file leaves the defaults. Either way the interface is usable,
        // and the precise reason is already in the log.
        POUND_LOG_WARN(&thread_logger,
                       "Starting from partial or default settings because '%s' could not be read "
                       "cleanly.",
                       state->settings_path);
    }
    else
    {
        pound_config_log_summary(&state->config);
    }

    // The atlas is built once, at the scale the config asked for. A later change to
    // the scale moves `style.FontScaleMain` and does not rebuild it: ImGui 1.92
    // rasterises each size on demand, and clearing the atlas mid-frame frees
    // storage ImGui is still holding.
    if (POUND_SUCCESS == pound_fonts_setup(&state->fonts, state->config.ui_scale))
    {
        pound_fonts_log_summary(&state->fonts);
    }
    else
    {
        POUND_LOG_WARN(&thread_logger,
                       "Falling back to ImGui's embedded font because the interface font could "
                       "not be built.");
    }

    // After the config, so a saved scale and theme win over the file: the saved blob
    // is the state the user last saw, and the file is the state they last committed.
    gui_apply_saved_state(state, saved_data, saved_size);

    gui_apply_appearance(state);
    state->config_appearance_dirty = false;
    pound_theme_log_summary(&state->theme);

    // The scene is complete enough to capture its own records now.
    state->log_scratch
        = memory_subsystem_allocate(memory_alignment,
                                    sizeof(pound_log_record_t) * (size_t)POUND_LOG_SNAPSHOT_MAX);

    if (NULL == state->log_scratch)
    {
        POUND_LOG_WARN(&thread_logger,
                       "Could not allocate the %d-record log snapshot buffer; the Logs page will "
                       "show a placeholder.",
                       POUND_LOG_SNAPSHOT_MAX);
    }

    if (POUND_SUCCESS
        != pound_log_ring_init(&state->log_ring, (size_t)POUND_LOG_RING_DEFAULT_CAPACITY))
    {
        POUND_LOG_WARN(&thread_logger,
                       "Could not start the %d-record log ring; the Logs page will read nothing.",
                       POUND_LOG_RING_DEFAULT_CAPACITY);
    }
    else if (POUND_SUCCESS != pound_log_ring_install(&state->log_ring))
    {
        POUND_LOG_WARN(&thread_logger,
                       "The log ring was built but could not be installed; the Logs page will read "
                       "nothing.");
    }
    else
    {
        pound_log_ring_log_summary(&state->log_ring);
    }

    POUND_LOG_INFO(&thread_logger,
                   "Interface ready: %s theme, %.2fx scale, settings at '%s'.",
                   pound_theme_mode_to_string(pound_theme_get_mode(&state->theme)),
                   (double)state->config.ui_scale,
                   state->settings_path);

    *out = state;
    return GUI_PLUGIN_SUCCESS;
}

static gui_plugin_error_t
gui_destroy(void *gui_state)
{
    if (NULL == gui_state)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: gui_state is NULL.");
        return GUI_PLUGIN_ERROR_INVALID_ARGUMENT;
    }

    gui_state_t *POUND_RESTRICT state = gui_state;

    // Uninstall before freeing: the ring's `previous` sink is restored first, so a
    // record emitted by a later shutdown path still reaches the console.
    pound_log_ring_uninstall(&state->log_ring);
    pound_log_ring_shutdown(&state->log_ring);

    if (NULL != state->log_scratch)
    {
        memory_subsystem_free(state->log_scratch);
        state->log_scratch = NULL;
    }

    memory_subsystem_free(state);
    return GUI_PLUGIN_SUCCESS;
}

static void
gui_render_sidebar(gui_state_t *POUND_RESTRICT                  state,
                   const gui_layout_rectangle_t *POUND_RESTRICT rectangle,
                   const float                                  origin_x,
                   const float                                  origin_y)
{
    igSetNextWindowPos((ImVec2){ origin_x + rectangle->x, origin_y + rectangle->y },
                       ImGuiCond_Always,
                       (ImVec2){ 0.0F, 0.0F });
    igSetNextWindowSize((ImVec2){ rectangle->width, rectangle->height }, ImGuiCond_Always);
    igPushStyleColor_Vec4(ImGuiCol_WindowBg,
                          pound_theme_imgui_color(&state->theme, POUND_THEME_SURFACE));

    const ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                                        | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse
                                        | ImGuiWindowFlags_NoSavedSettings
                                        | ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (igBegin("##sidebar", NULL, window_flags))
    {
        const bool wordmark_pushed = pound_fonts_push(&state->fonts, POUND_FONT_ROLE_TITLE);
        igPushStyleColor_Vec4(ImGuiCol_Text,
                              pound_theme_imgui_color(&state->theme, POUND_THEME_ACCENT));
        igTextUnformatted("Pound", NULL);
        igPopStyleColor(1);
        pound_fonts_pop(wordmark_pushed);

        const bool caption_pushed = pound_fonts_push(&state->fonts, POUND_FONT_ROLE_SMALL);
        igPushStyleColor_Vec4(ImGuiCol_Text,
                              pound_theme_imgui_color(&state->theme, POUND_THEME_TEXT_SECONDARY));
        igText("v%s", POUND_VERSION);
        igPopStyleColor(1);
        pound_fonts_pop(caption_pushed);

        igSpacing();
        igSeparator();
        igSpacing();

        const pound_page_descriptor_t *const registry   = pound_pages_registry();
        const float                          row_height = igGetFrameHeightWithSpacing();

        // The indent is a style variable rather than literal spaces in the label:
        // spaces would be part of the ID and would make the selected-row highlight
        // a different width from the text it covers.
        igPushStyleVar_Vec2(ImGuiStyleVar_FramePadding, (ImVec2){ 14.0F, 8.0F });

        for (int i = 0; i < (int)POUND_PAGE_COUNT; i++)
        {
            const bool selected = (state->current_page == i);

            if (true
                == igSelectable_Bool(registry[i].name,
                                     selected,
                                     ImGuiSelectableFlags_None,
                                     (ImVec2){ 0.0F, row_height }))
            {
                state->current_page = i;
            }
        }

        igPopStyleVar(1);
    }

    igEnd();
    igPopStyleColor(1);
}

static void
gui_render_title_bar(gui_state_t *POUND_RESTRICT                  state,
                     const gui_layout_rectangle_t *POUND_RESTRICT rectangle,
                     const float                                  origin_x,
                     const float                                  origin_y)
{
    igSetNextWindowPos((ImVec2){ origin_x + rectangle->x, origin_y + rectangle->y },
                       ImGuiCond_Always,
                       (ImVec2){ 0.0F, 0.0F });
    igSetNextWindowSize((ImVec2){ rectangle->width, rectangle->height }, ImGuiCond_Always);
    igPushStyleColor_Vec4(ImGuiCol_WindowBg,
                          pound_theme_imgui_color(&state->theme, POUND_THEME_SURFACE));

    const ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                                        | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse
                                        | ImGuiWindowFlags_NoScrollbar
                                        | ImGuiWindowFlags_NoSavedSettings
                                        | ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (igBegin("##title_bar", NULL, window_flags))
    {
        const pound_page_descriptor_t *const registry = pound_pages_registry();
        const int page = (true == pound_page_is_valid(state->current_page))
                           ? state->current_page
                           : (int)POUND_PAGE_LIBRARY;

        igAlignTextToFramePadding();

        const bool title_pushed = pound_fonts_push(&state->fonts, POUND_FONT_ROLE_TITLE);
        igTextUnformatted(registry[page].name, NULL);
        pound_fonts_pop(title_pushed);

        igSameLine(0.0F, 12.0F);

        const bool subtitle_pushed = pound_fonts_push(&state->fonts, POUND_FONT_ROLE_SMALL);
        igPushStyleColor_Vec4(ImGuiCol_Text,
                              pound_theme_imgui_color(&state->theme, POUND_THEME_TEXT_SECONDARY));
        igTextUnformatted(registry[page].subtitle, NULL);
        igPopStyleColor(1);
        pound_fonts_pop(subtitle_pushed);

        // The controls are placed against the right edge rather than with a spacer:
        // a spacer would have to be measured anyway, and measuring directly is what
        // keeps them on the same line as the title at every scale.
        igSameLine(0.0F, 8.0F);

        const ImGuiStyle *const style = igGetStyle();
        const char *const       theme_label
            = (POUND_THEME_DARK == pound_theme_get_mode(&state->theme)) ? "Light mode" : "Dark mode";
        const char *const debug_label = "Debug";

        const float theme_width
            = igCalcTextSize(theme_label, NULL, false, 0.0F).x + (style->FramePadding.x * 2.0F);
        const float debug_width
            = igCalcTextSize(debug_label, NULL, false, 0.0F).x + (style->FramePadding.x * 2.0F);

        const float cursor_x     = igGetCursorPosX();
        const float content_right = cursor_x + igGetContentRegionAvail().x;
        const float controls     = theme_width + debug_width + style->ItemSpacing.x;
        const float target_x     = content_right - controls;

        if (target_x > cursor_x)
        {
            igSetCursorPosX(target_x);
        }

        if (igButton(theme_label, (ImVec2){ theme_width, 0.0F }))
        {
            const pound_config_theme_t wanted
                = (POUND_THEME_DARK == pound_theme_get_mode(&state->theme)) ? POUND_CONFIG_THEME_LIGHT
                                                                            : POUND_CONFIG_THEME_DARK;

            if (POUND_SUCCESS != pound_config_set_theme(&state->config, wanted))
            {
                POUND_LOG_WARN(&thread_logger,
                               "The appearance switch was refused; keeping the current ramp.");
            }
            else
            {
                state->config_appearance_dirty = true;
            }
        }

        igSameLine(0.0F, style->ItemSpacing.x);

        if (igButton(debug_label, (ImVec2){ debug_width, 0.0F }))
        {
            state->show_debug_menu = !state->show_debug_menu;
        }
    }

    igEnd();
    igPopStyleColor(1);
}

static void
gui_render_content_window(gui_state_t *POUND_RESTRICT                  state,
                          const gui_layout_rectangle_t *POUND_RESTRICT rectangle,
                          const float                                  origin_x,
                          const float                                  origin_y)
{
    igSetNextWindowPos((ImVec2){ origin_x + rectangle->x, origin_y + rectangle->y },
                       ImGuiCond_Always,
                       (ImVec2){ 0.0F, 0.0F });
    igSetNextWindowSize((ImVec2){ rectangle->width, rectangle->height }, ImGuiCond_Always);

    const ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                                        | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse
                                        | ImGuiWindowFlags_NoSavedSettings
                                        | ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (igBegin("##page_content", NULL, window_flags))
    {
        pound_page_context_t context;
        memset(&context, 0, sizeof(context));

        context.config               = &state->config;
        context.theme                = &state->theme;
        context.fonts                = &state->fonts;
        context.settings_path        = state->settings_path;
        context.log_ring             = &state->log_ring;
        context.log_scratch          = state->log_scratch;
        context.log_scratch_capacity = (size_t)POUND_LOG_SNAPSHOT_MAX;
        context.memory_tracker       = &state->memory_tracker;
        context.view                 = &state->view;
        context.config_dirty         = &state->config_appearance_dirty;

        const int page = (true == pound_page_is_valid(state->current_page))
                           ? state->current_page
                           : (int)POUND_PAGE_LIBRARY;

        pound_page_render((pound_page_t)page, &context);
    }

    igEnd();
}

static void
gui_render_debug_windows(gui_state_t *POUND_RESTRICT state)
{
    if (true == state->show_debug_menu)
    {
        if (igBegin("Debug##debug_menu", &state->show_debug_menu, ImGuiWindowFlags_None))
        {
            if (igBeginChild_Str("##debug_memory",
                                 (ImVec2){ 0.0F, 240.0F },
                                 ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                                 ImGuiWindowFlags_None))
            {
                debug_memory_render(&state->memory_tracker);
            }
            igEndChild();

            igSeparatorText("Interface");
            igCheckbox("Hot reload guide", &state->show_hot_reload_guide);
            igCheckbox("Dear ImGui demo", &state->show_imgui_demo);
        }
        igEnd();
    }

    if (true == state->show_hot_reload_guide)
    {
        const ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoCollapse;

        if (igBegin("Hot Reloading Guide", &state->show_hot_reload_guide, window_flags))
        {
            igTextWrapped("This build loads its interface from a shared library. Rebuild the "
                          "'PoundGui' target and Pound swaps the new code in without restarting.");
            igSpacing();
            igTextWrapped("Press F5 at any time to reload by hand.");
        }
        igEnd();
    }

    if (true == state->show_imgui_demo)
    {
        igShowDemoWindow(&state->show_imgui_demo);
    }
}

static gui_plugin_error_t
gui_render_frame(void *gui_state)
{
    if (POUND_UNLIKELY(NULL == gui_state))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: gui_state is NULL.");
        return GUI_PLUGIN_ERROR_INVALID_ARGUMENT;
    }

    gui_state_t *POUND_RESTRICT state         = gui_state;
    const ImGuiViewport        *main_viewport = igGetMainViewport();

    if (POUND_UNLIKELY(NULL == main_viewport))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: main viewport is NULL.");
        return GUI_PLUGIN_ERROR_INVALID_ARGUMENT;
    }

    const float origin_x      = main_viewport->Pos.x;
    const float origin_y      = main_viewport->Pos.y;
    const float screen_width  = main_viewport->Size.x;
    const float screen_height = main_viewport->Size.y;

    // The configured level takes effect as soon as it changes, and the ring
    // installed at create time sits behind it, so raising the floor starts
    // capturing at once rather than at the next restart.
    thread_logger.min_level = state->config.log_level;

    if (true == state->config_appearance_dirty)
    {
        gui_apply_appearance(state);
        state->config_appearance_dirty = false;
    }

    const float effective_scale
        = (true == state->fonts.setup) ? state->fonts.scale : state->config.ui_scale;

    float sidebar_fraction = GUI_SIDEBAR_MIN_FRACTION;

    if (screen_width > 0.0F)
    {
        sidebar_fraction = (GUI_SIDEBAR_BASE_WIDTH * effective_scale) / screen_width;

        if (sidebar_fraction < GUI_SIDEBAR_MIN_FRACTION)
        {
            sidebar_fraction = GUI_SIDEBAR_MIN_FRACTION;
        }

        if (sidebar_fraction > GUI_SIDEBAR_MAX_FRACTION)
        {
            sidebar_fraction = GUI_SIDEBAR_MAX_FRACTION;
        }
    }

    float title_fraction = GUI_TITLE_MIN_FRACTION;

    if (screen_height > 0.0F)
    {
        title_fraction = (GUI_TITLE_BASE_HEIGHT * effective_scale) / screen_height;

        if (title_fraction < GUI_TITLE_MIN_FRACTION)
        {
            title_fraction = GUI_TITLE_MIN_FRACTION;
        }

        if (title_fraction > GUI_TITLE_MAX_FRACTION)
        {
            title_fraction = GUI_TITLE_MAX_FRACTION;
        }
    }

    gui_layout_t layout = { 0 };
    gui_plugin_error_t status = gui_layout_add_panel(&layout, GUI_LAYOUT_REGION_LEFT, sidebar_fraction);

    if (GUI_PLUGIN_SUCCESS == status)
    {
        status = gui_layout_add_panel(&layout, GUI_LAYOUT_REGION_TOP, title_fraction);
    }

    gui_layout_rectangle_t content = { 0.0F, 0.0F, screen_width, screen_height };

    if ((GUI_PLUGIN_SUCCESS == status) && (2 == layout.panel_count))
    {
        status = gui_layout_compute(&layout, screen_width, screen_height, &content);
    }

    if ((GUI_PLUGIN_SUCCESS == status) && (true == layout.is_computed))
    {
        gui_render_sidebar(state, &layout.panels[0], origin_x, origin_y);
        gui_render_title_bar(state, &layout.panels[1], origin_x, origin_y);
        gui_render_content_window(state, &content, origin_x, origin_y);
    }
    else
    {
        POUND_LOG_WARN(&thread_logger,
                       "The shell layout could not be computed; drawing the page across the whole "
                       "window.");
        content.x      = 0.0F;
        content.y      = 0.0F;
        content.width  = screen_width;
        content.height = screen_height;
        gui_render_content_window(state, &content, origin_x, origin_y);
    }

    gui_render_debug_windows(state);

    return GUI_PLUGIN_SUCCESS;
}

static gui_plugin_error_t
gui_save(void *gui_state, void *out_gui, const size_t capacity, size_t *out_size)
{
    if (NULL == gui_state)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: gui_state is NULL.");
        return GUI_PLUGIN_ERROR_INVALID_ARGUMENT;
    }

    if (NULL == out_gui)
    {
        if (NULL == out_size)
        {
            POUND_LOG_ERROR(&thread_logger,
                            "Aborting function: both out_gui and out_size are NULL.");
            return GUI_PLUGIN_ERROR_INVALID_ARGUMENT;
        }

        *out_size = sizeof(gui_saved_state_t);
        POUND_LOG_DEBUG(&thread_logger,
                        "Reporting the %zu-byte saved-state size.",
                        sizeof(gui_saved_state_t));
        return GUI_PLUGIN_SUCCESS;
    }

    if (capacity < sizeof(gui_saved_state_t))
    {
        if (NULL != out_size)
        {
            *out_size = sizeof(gui_saved_state_t);
        }

        POUND_LOG_WARN(&thread_logger,
                       "capacity (%zu) < sizeof(gui_saved_state_t) (%zu); cannot save state.",
                       capacity,
                       sizeof(gui_saved_state_t));
        return GUI_PLUGIN_ERROR_BUFFER_TOO_SMALL;
    }

    const gui_state_t *POUND_RESTRICT state = gui_state;

    gui_saved_state_t saved;
    memset(&saved, 0, sizeof(saved));

    saved.magic                  = GUI_SAVED_STATE_MAGIC;
    saved.version                = GUI_SAVED_STATE_VERSION;
    saved.current_page           = (int32_t)state->current_page;
    saved.theme_mode             = (int32_t)state->config.theme;
    saved.log_level              = (int32_t)state->config.log_level;
    saved.ui_scale_milli         = (int32_t)(state->config.ui_scale * 1000.0F);
    saved.log_min_level          = (int32_t)state->view.log_min_level;
    saved.log_auto_scroll        = (0 != state->view.log_auto_scroll) ? 1 : 0;
    saved.show_debug_menu        = (0 != state->show_debug_menu) ? 1 : 0;
    saved.show_hot_reload_guide  = (0 != state->show_hot_reload_guide) ? 1 : 0;
    saved.show_imgui_demo        = (0 != state->show_imgui_demo) ? 1 : 0;

    memcpy(out_gui, &saved, sizeof(saved));

    if (NULL != out_size)
    {
        *out_size = sizeof(saved);
    }

    POUND_LOG_DEBUG(&thread_logger,
                    "Serialised %zu bytes of interface state on page '%s'.",
                    sizeof(saved),
                    pound_page_name(state->current_page));
    return GUI_PLUGIN_SUCCESS;
}

/*** end of file ***/

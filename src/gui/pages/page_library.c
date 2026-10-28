#include "pages.h"

#include "config/config.h"
#include "fs/keys.h"
#include "loader/boot/boot.h"
#include "loader/title_loader.h"

#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/// Longest status line the page keeps between frames.
#define POUND_LIBRARY_STATUS_MAX 192

/// Filters the two file pickers offer.
///
/// SDL takes a semicolon-separated list of extensions, so the game picker names
/// every container Compound can open and the key picker names the two spellings
/// the ecosystem uses for a key file.
static const SDL_DialogFileFilter POUND_LIBRARY_GAME_FILTERS[] = {
    { "Nintendo Switch titles", "xci;nsp;nca;nro;nso;hac" },
    { "All files", "*" },
};

static const SDL_DialogFileFilter POUND_LIBRARY_KEY_FILTERS[] = {
    { "Key files", "keys;txt" },
    { "All files", "*" },
};

/// State the page keeps across frames.
///
/// Deliberately `static` rather than on the view: these are buffers the native
/// file dialog writes into from its own callback, and the page has to see the
/// result whether or not the user is still looking at the Library page.
static char library_game_path[POUND_CONFIG_PATH_MAX];
static char library_keys_path[POUND_CONFIG_PATH_MAX];

/// Set by a dialog callback, consumed by the next render so the status line is
/// written from the render thread.
static bool library_game_chosen;
static bool library_keys_chosen;

/// Set when SDL could not even show a picker, so the page can say why.
///
/// SDL reports a picker that failed to open as a callback with a NULL filelist;
/// a picker the user simply cancelled arrives as a list whose first entry is
/// NULL. Distinguishing them is what turns "the button did nothing" into an
/// error the user can act on.
static bool library_dialog_failed;
static char library_dialog_error[POUND_LIBRARY_STATUS_MAX];

/// The parsed key store, and whether it has been initialised.
static key_store_t library_keys;
static bool        library_keys_initialized;

/// The boot subsystem this page drives.
static pound_boot_state_t library_boot;
static bool               library_boot_initialized;

/// Records an outcome for the page to show.
///
/// Kept on the view as well as logged because the user is looking at this page
/// when they press the button; a message that only reaches the Logs page is a
/// message they will not see while the default log level is above it.
static void
pound_library_set_status(pound_pages_view_t *POUND_RESTRICT view,
                         const bool                          is_error,
                         const char *POUND_RESTRICT          message)
{
    if (NULL == view)
    {
        return;
    }

    view->library_status_is_error = is_error;

    if (NULL == message)
    {
        view->library_status[0] = '\0';
        return;
    }

    // Logged as well as shown: a message that only reaches the page is one a bug
    // report does not carry, and the Logs page is where a user goes to find out
    // why a title would not open.
    if (true == is_error)
    {
        POUND_LOG_WARN(&thread_logger, "Library: %s", message);
    }
    else
    {
        POUND_LOG_INFO(&thread_logger, "Library: %s", message);
    }

    const int written
        = snprintf(view->library_status, sizeof(view->library_status), "%s", message);

    if (written < 0)
    {
        view->library_status[0] = '\0';
    }
}

/// The file name part of a path, for a status line that fits.
static const char *
pound_library_file_name(const char *POUND_RESTRICT path)
{
    const char *const forward  = strrchr(path, '/');
    const char *const backward = strrchr(path, '\\');
    const char *const slash    = (forward > backward) ? forward : backward;

    return (NULL == slash) ? path : (slash + 1);
}

/// The human-readable name of a title type.
static const char *
pound_library_type_name(const pound_title_type_t type)
{
    switch (type)
    {
        case POUND_TITLE_NRO:
            return "NRO homebrew";
        case POUND_TITLE_NSO:
            return "NSO executable";
        case POUND_TITLE_NCA:
            return "NCA title archive";
        case POUND_TITLE_NSP:
            return "NSP package";
        case POUND_TITLE_XCI:
            return "XCI cartridge image";
        case POUND_TITLE_HAC:
            return "HAC image";
        case POUND_TITLE_UNKNOWN:
            break;
    }

    return "unknown";
}

/// Called by SDL when the game picker closes.
static void SDLCALL
pound_library_game_dialog_done(void *userdata, const char *const *filelist, int filter)
{
    (void)userdata;
    (void)filter;

    if (NULL == filelist)
    {
        (void)snprintf(library_dialog_error,
                       sizeof(library_dialog_error),
                       "The file picker could not open: %s",
                       SDL_GetError());
        library_dialog_failed = true;
        return;
    }

    if (NULL == filelist[0])
    {
        return;
    }

    (void)pound_config_set_path(library_game_path, sizeof(library_game_path), filelist[0]);
    library_game_chosen = true;
}

/// Called by SDL when the key picker closes.
static void SDLCALL
pound_library_key_dialog_done(void *userdata, const char *const *filelist, int filter)
{
    (void)userdata;
    (void)filter;

    if (NULL == filelist)
    {
        (void)snprintf(library_dialog_error,
                       sizeof(library_dialog_error),
                       "The file picker could not open: %s",
                       SDL_GetError());
        library_dialog_failed = true;
        return;
    }

    if (NULL == filelist[0])
    {
        return;
    }

    (void)pound_config_set_path(library_keys_path, sizeof(library_keys_path), filelist[0]);
    library_keys_chosen = true;
}

/// The window a picker should be modal to.
///
/// SDL runs the Windows picker on its own thread and owns it to the window it is
/// given. The ImGui backend keeps the main viewport's SDL window ID, so the page
/// can recover the window without the shell having to hand it down. A NULL
/// parent still works, but the dialog can then open behind the emulator, which
/// looks exactly like a button that did nothing.
static SDL_Window *
pound_library_parent_window(void)
{
    const ImGuiViewport *const viewport = igGetMainViewport();

    if ((NULL == viewport) || (0 == (intptr_t)viewport->PlatformHandle))
    {
        return NULL;
    }

    return SDL_GetWindowFromID((SDL_WindowID)(intptr_t)viewport->PlatformHandle);
}

/// Opens the native picker for a game image.
static void
pound_library_browse_game(void)
{
    SDL_ShowOpenFileDialog(pound_library_game_dialog_done,
                           NULL,
                           pound_library_parent_window(),
                           POUND_LIBRARY_GAME_FILTERS,
                           (int)SDL_arraysize(POUND_LIBRARY_GAME_FILTERS),
                           NULL,
                           false);
}

/// Opens the native picker for a key file.
static void
pound_library_browse_keys(void)
{
    SDL_ShowOpenFileDialog(pound_library_key_dialog_done,
                           NULL,
                           pound_library_parent_window(),
                           POUND_LIBRARY_KEY_FILTERS,
                           (int)SDL_arraysize(POUND_LIBRARY_KEY_FILTERS),
                           NULL,
                           false);
}

/// Verifies, identifies and loads the game in `library_game_path`.
static void
pound_library_load_game(pound_pages_view_t *POUND_RESTRICT view)
{
    char status[POUND_LIBRARY_STATUS_MAX];

    if ('\0' == library_game_path[0])
    {
        pound_library_set_status(view, true, "Choose a game file first.");
        return;
    }

    SDL_PathInfo path_info;

    if (false == SDL_GetPathInfo(library_game_path, &path_info))
    {
        (void)snprintf(status,
                       sizeof(status),
                       "Cannot open '%s': %s",
                       pound_library_file_name(library_game_path),
                       SDL_GetError());
        pound_library_set_status(view, true, status);
        return;
    }

    if (SDL_PATHTYPE_FILE != path_info.type)
    {
        pound_library_set_status(view, true, "That path is not a file.");
        return;
    }

    pound_title_info_t title_info;

    if (POUND_SUCCESS != pound_title_detect(library_game_path, &title_info))
    {
        pound_library_set_status(view, true, "That file could not be read as a title.");
        return;
    }

    if (POUND_TITLE_UNKNOWN == title_info.type)
    {
        (void)snprintf(status,
                       sizeof(status),
                       "'%s' is not a format Compound opens. Use XCI, NSP, NCA, NRO, NSO or HAC.",
                       pound_library_file_name(library_game_path));
        pound_library_set_status(view, true, status);
        return;
    }

    if (POUND_SUCCESS != pound_title_load(&title_info))
    {
        pound_library_set_status(view, true, "The title loader refused that file.");
        return;
    }

    if (false == library_boot_initialized)
    {
        (void)pound_boot_init(&library_boot);
        library_boot_initialized = true;
    }

    (void)pound_boot_load_title(&library_boot, &title_info);
    (void)pound_boot_start(&library_boot);

    (void)snprintf(status,
                   sizeof(status),
                   "Loaded %s - %s, %.1f MiB.",
                   pound_library_file_name(library_game_path),
                   pound_library_type_name(title_info.type),
                   (double)path_info.size / (1024.0 * 1024.0));
    pound_library_set_status(view, false, status);
}

/// Reads, parses and remembers the key file in `library_keys_path`.
static void
pound_library_load_keys(pound_pages_view_t *POUND_RESTRICT view,
                        pound_config_t *POUND_RESTRICT     config,
                        const char *POUND_RESTRICT         settings_path)
{
    char status[POUND_LIBRARY_STATUS_MAX];

    if ('\0' == library_keys_path[0])
    {
        pound_library_set_status(view, true, "Choose a keys file first.");
        return;
    }

    size_t size  = 0;
    void  *bytes = SDL_LoadFile(library_keys_path, &size);

    if (NULL == bytes)
    {
        (void)snprintf(status,
                       sizeof(status),
                       "Cannot read '%s': %s",
                       pound_library_file_name(library_keys_path),
                       SDL_GetError());
        pound_library_set_status(view, true, status);
        return;
    }

    if (false == library_keys_initialized)
    {
        key_store_init(&library_keys);
        library_keys_initialized = true;
    }

    const error_t parsed = key_store_load_text(&library_keys, (const char *)bytes, size);
    SDL_free(bytes);

    key_store_log_summary(&library_keys);

    const size_t count = key_store_count(&library_keys);

    if (NULL != config)
    {
        (void)pound_config_set_path(config->key_file,
                                    sizeof(config->key_file),
                                    library_keys_path);

        if (NULL != settings_path)
        {
            (void)pound_config_save(config, settings_path);
        }
    }

    if (POUND_SUCCESS == parsed)
    {
        (void)snprintf(status,
                       sizeof(status),
                       "Loaded %zu keys from %s.",
                       count,
                       pound_library_file_name(library_keys_path));
        pound_library_set_status(view, false, status);
    }
    else
    {
        (void)snprintf(status,
                       sizeof(status),
                       "Loaded %zu keys from %s, but some lines were rejected. See the Logs page.",
                       count,
                       pound_library_file_name(library_keys_path));
        pound_library_set_status(view, true, status);
    }
}

/// Draws the outcome of the last action, if there is one.
static void
pound_library_render_status(const pound_page_context_t *POUND_RESTRICT context)
{
    if (NULL == context->view)
    {
        return;
    }

    const pound_pages_view_t *const view = context->view;

    if ('\0' == view->library_status[0])
    {
        return;
    }

    igSpacing();

    if (true == view->library_status_is_error)
    {
        const ImVec4 error_color = { 0.906F, 0.298F, 0.235F, 1.0F };
        igPushStyleColor_Vec4(ImGuiCol_Text, error_color);
    }
    else
    {
        igPushStyleColor_Vec4(ImGuiCol_Text,
                              pound_theme_imgui_color(context->theme, POUND_THEME_ACCENT));
    }

    igTextWrapped("%s", view->library_status);
    igPopStyleColor(1);
}

/// The Library page.
void
pound_page_render_library(const pound_page_context_t *POUND_RESTRICT context)
{
    if (POUND_UNLIKELY(NULL == context))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: context is NULL.");
        return;
    }

    pound_pages_view_t *const view = context->view;

    if (true == library_dialog_failed)
    {
        library_dialog_failed = false;
        pound_library_set_status(view, true, library_dialog_error);
    }

    pound_page_heading(context,
                       "LIBRARY",
                       "Library",
                       "Load dumped Nintendo Switch/Switch 2 games and keys.");

    pound_page_section(context, "Load Game");

    igTextWrapped("Select a dumped game file (XCI, NSP, NCA, NRO, NSO, HAC) to load.");
    igSpacing();

    if (true == library_game_chosen)
    {
        library_game_chosen = false;
        pound_library_set_status(view, false, "Game file chosen. Press Load Game.");
    }

    igInputText("Game Path",
                library_game_path,
                sizeof(library_game_path),
                ImGuiInputTextFlags_None,
                NULL,
                NULL);

    if (igButton("Browse Game...", (ImVec2){ 0.0F, 0.0F }))
    {
        pound_library_browse_game();
    }

    igSameLine(0.0F, 8.0F);

    if (igButton("Load Game", (ImVec2){ 0.0F, 0.0F }))
    {
        pound_library_load_game(view);
    }

    pound_library_render_status(context);

    igSpacing();
    igSeparator();
    igSpacing();

    pound_page_section(context, "Load Keys");

    igTextWrapped("Load your prod.keys or keys.txt file (required for encrypted titles).");
    igSpacing();

    const pound_config_t *const config = context->config;

    if (true == library_keys_chosen)
    {
        library_keys_chosen = false;
        pound_library_set_status(view, false, "Key file chosen. Press Load Keys.");
    }

    if (('\0' == library_keys_path[0]) && (NULL != config) && ('\0' != config->key_file[0]))
    {
        // Start from the configured key file, so the common case -- the user
        // already told Settings where their keys live -- is one press of Load Keys.
        (void)pound_config_set_path(library_keys_path,
                                    sizeof(library_keys_path),
                                    config->key_file);
    }

    igInputText("Keys Path",
                library_keys_path,
                sizeof(library_keys_path),
                ImGuiInputTextFlags_None,
                NULL,
                NULL);

    if (igButton("Browse Keys...", (ImVec2){ 0.0F, 0.0F }))
    {
        pound_library_browse_keys();
    }

    igSameLine(0.0F, 8.0F);

    if (igButton("Load Keys", (ImVec2){ 0.0F, 0.0F }))
    {
        pound_library_load_keys(view, (pound_config_t *)config, context->settings_path);
    }

    pound_library_render_status(context);

    igSpacing();
    igSeparator();
    igSpacing();

    pound_page_section(context, "Where Compound looks");

    if (NULL == config)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "The Library page has no config, so it cannot show the content folder.");
        igTextDisabled("Settings are unavailable this frame; see the Logs page.");
        return;
    }

    pound_page_labeled_value(context,
                             "Content folder",
                             ('\0' != config->content_dir[0]) ? config->content_dir : "(not set)",
                             0.3F);
    pound_page_labeled_value(context,
                             "Keys file",
                             ('\0' != config->key_file[0]) ? config->key_file : "(not set)",
                             0.3F);

    if (true == library_keys_initialized)
    {
        char loaded[32];
        (void)snprintf(loaded, sizeof(loaded), "%zu", key_store_count(&library_keys));
        pound_page_labeled_value(context, "Keys loaded", loaded, 0.3F);
    }

    pound_page_section(context, "Formats Compound supports");

    igTextWrapped("Cartridge images (XCI) - Switch/Switch 2");
    igTextWrapped("Digital packages (NSP) - Switch/Switch 2");
    igTextWrapped("Encrypted title archives (NCA) - opened with your keys");
    igTextWrapped("Homebrew (NRO, NSO) - Switch/Switch 2 compatible");
    igSpacing();
    igPushStyleColor_Vec4(
        ImGuiCol_Text, pound_theme_imgui_color(context->theme, POUND_THEME_TEXT_SECONDARY));
    const bool font_pushed = pound_fonts_push(context->fonts, POUND_FONT_ROLE_SMALL);
    igTextWrapped("Compound never ships or embeds Nintendo's keys. You supply a keys file.");
    pound_fonts_pop(font_pushed);
    igPopStyleColor(1);
}

/*** end of file ***/

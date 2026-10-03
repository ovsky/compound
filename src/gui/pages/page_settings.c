#include "pages.h"

#include <stdio.h>
#include <string.h>

/// The theme names offered by the appearance control, in `pound_config_theme_t`
/// order: dark is 0, light is 1.
static const char POUND_SETTINGS_THEME_ITEMS[] = "Dark\0Light\0";

/// The narrowest and widest the JIT cache slider goes, in mebibytes.
///
/// The same numbers as `POUND_CONFIG_JIT_MIN_BYTES` and
/// `POUND_CONFIG_JIT_MAX_BYTES`, restated for the widget because the widget works
/// in whole mebibytes and the config in bytes. A `_Static_assert` cannot see across
/// the two units, so the far end is checked by `pound_config_set_jit_cache_bytes`
/// refusing anything out of range.
#define POUND_SETTINGS_JIT_MIN_MIB ((int)(POUND_CONFIG_JIT_MIN_BYTES / (1024ULL * 1024ULL)))
#define POUND_SETTINGS_JIT_MAX_MIB ((int)(POUND_CONFIG_JIT_MAX_BYTES / (1024ULL * 1024ULL)))

/// Records a save or reload outcome for the page to show.
///
/// Stored on the view rather than only logged because the user is looking here when
/// it happens; truncated into the field rather than trusted, because the message is
/// composed for a person.
static void
pound_settings_set_status(pound_pages_view_t *POUND_RESTRICT view,
                          const bool                         is_error,
                          const char *POUND_RESTRICT         message)
{
    view->settings_status_is_error = is_error;

    if (NULL == message)
    {
        view->settings_status[0] = '\0';
        return;
    }

    const int written = snprintf(view->settings_status, sizeof(view->settings_status), "%s", message);

    if (written < 0)
    {
        // `snprintf` only fails here for an encoding error, which cannot happen for
        // the plain ASCII this page writes; clearing is still safer than leaving the
        // previous message, which would attribute an old outcome to a new click.
        view->settings_status[0] = '\0';
    }
}

/// Marks the appearance as changed, so the shell re-applies the theme and scale.
///
/// Separate from writing the config because the config is the durable record and
/// this is the request to redraw: a change to the theme has to reach the running
/// interface in the same interaction, not on the next start.
static void
pound_settings_mark_appearance_dirty(const pound_page_context_t *POUND_RESTRICT context)
{
    if (NULL != context->config_dirty)
    {
        *context->config_dirty = true;
    }
}

void
pound_page_render_settings(const pound_page_context_t *POUND_RESTRICT context)
{
    if (POUND_UNLIKELY(NULL == context))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: context is NULL.");
        return;
    }

    pound_page_heading(context,
                       "SETTINGS",
                       "Settings",
                       "Where Pound's files live, how it looks, and how much it says.");

    if (POUND_UNLIKELY(NULL == context->config))
    {
        POUND_LOG_ERROR(&thread_logger, "The Settings page has no config to edit.");
        igTextDisabled("Settings are unavailable this frame; see the Logs page.");
        return;
    }

    pound_config_t *const      config = context->config;
    pound_pages_view_t *const  view   = context->view;

    if (POUND_UNLIKELY(NULL == view))
    {
        POUND_LOG_ERROR(&thread_logger, "The Settings page has no view state.");
        igTextDisabled("Settings are unavailable this frame; see the Logs page.");
        return;
    }

    pound_page_section(context, "Appearance");

    // Theme. The index is derived from the config each frame rather than cached, so
    // a reload that changed the theme is reflected without a second field to keep in
    // step.
    int theme_index = (POUND_CONFIG_THEME_LIGHT == config->theme) ? 1 : 0;

    pound_page_setting_label(context, "Theme", "The colour ramp for every window.");

    igSetNextItemWidth(180.0F);

    if (igCombo_Str("##theme", &theme_index, POUND_SETTINGS_THEME_ITEMS, 2))
    {
        const pound_config_theme_t wanted
            = (1 == theme_index) ? POUND_CONFIG_THEME_LIGHT : POUND_CONFIG_THEME_DARK;
        const error_t status = pound_config_set_theme(config, wanted);

        if (POUND_SUCCESS == status)
        {
            pound_settings_mark_appearance_dirty(context);
        }
        else
        {
            pound_settings_set_status(view, true, "That theme is not one Pound knows.");
        }
    }

    igSpacing();

    pound_page_setting_label(context,
                             "Interface scale",
                             "Multiplies every text size. Useful on a dense panel or a phone.");

    float scale = config->ui_scale;

    igSetNextItemWidth(240.0F);

    if (igSliderFloat("##ui_scale",
                      &scale,
                      POUND_CONFIG_UI_SCALE_MIN,
                      POUND_CONFIG_UI_SCALE_MAX,
                      "%.2fx",
                      ImGuiSliderFlags_None))
    {
        const error_t status = pound_config_set_ui_scale(config, scale);

        if (POUND_SUCCESS == status)
        {
            pound_settings_mark_appearance_dirty(context);
        }
        else
        {
            // The slider was moved outside the accepted range. The config keeps the
            // old value, and saying so avoids a control that snaps back for no
            // visible reason.
            pound_settings_set_status(view, true, "That scale is outside the range Pound allows.");
        }
    }

    pound_page_section(context, "Logging");

    int level_index = pound_page_log_level_index(config->log_level);

    pound_page_setting_label(context,
                             "Log level",
                             "The quietest record the Logs page and the console keep. None silences "
                             "everything above a failure to start.");

    igSetNextItemWidth(180.0F);

    if (igCombo_Str("##log_level", &level_index, pound_page_log_level_items(), 6))
    {
        const error_t status = pound_config_set_log_level(config, pound_page_log_level_at(level_index));

        if (POUND_SUCCESS != status)
        {
            pound_settings_set_status(view, true, "That log level is not one Pound knows.");
        }
    }

    pound_page_section(context, "Performance");

    int megabytes = (int)(config->jit_cache_bytes / (1024U * 1024U));

    pound_page_setting_label(context,
                             "JIT cache",
                             "How much translated code Pound may hold. Larger keeps more titles warm; "
                             "smaller leaves more memory for the guest.");

    igSetNextItemWidth(240.0F);

    if (igSliderInt("##jit_cache",
                    &megabytes,
                    POUND_SETTINGS_JIT_MIN_MIB,
                    POUND_SETTINGS_JIT_MAX_MIB,
                    "%d MiB",
                    ImGuiSliderFlags_None))
    {
        const size_t bytes  = (size_t)megabytes * 1024U * 1024U;
        const error_t status = pound_config_set_jit_cache_bytes(config, bytes);

        if (POUND_SUCCESS != status)
        {
            pound_settings_set_status(view, true, "That cache size is outside the range Pound allows.");
        }
    }

    pound_page_section(context, "Files");

    pound_page_setting_label(context,
                             "Key file",
                             "Your dumped console keys. Pound ships none and reads this file only when "
                             "a title is opened.");

    // Typing is not itself an event worth reporting: the value is read when a title
    // is opened, and that is where a missing or malformed file is caught.
    igInputText("##key_file",
                config->key_file,
                sizeof(config->key_file),
                ImGuiInputTextFlags_None,
                NULL,
                NULL);

    igSpacing();

    pound_page_setting_label(context,
                             "Content folder",
                             "Where installed titles are looked for.");

    igInputText("##content_dir",
                config->content_dir,
                sizeof(config->content_dir),
                ImGuiInputTextFlags_None,
                NULL,
                NULL);

    igSpacing();

    pound_page_setting_label(context,
                             "Typeface",
                             "An optional font file. Empty uses the system face Pound finds, or its "
                             "embedded one.");

    igInputText("##font_path",
                config->font_path,
                sizeof(config->font_path),
                ImGuiInputTextFlags_None,
                NULL,
                NULL);

    pound_page_section(context, "Storage");

    igText("Settings file:");
    igSameLine(0.0F, 8.0F);

    if (NULL != context->settings_path)
    {
        igTextUnformatted(context->settings_path, NULL);
    }
    else
    {
        igTextDisabled("(no path; this build cannot save)");
    }

    igSpacing();

    // Save and reload are the two operations the file needs, and they are the only
    // two buttons on the page. Everything above applies to the running interface
    // immediately; these decide whether it also survives a restart.
    if (igButton("Save", (ImVec2){ 0.0F, 0.0F }))
    {
        if (NULL == context->settings_path)
        {
            POUND_LOG_ERROR(&thread_logger,
                            "The Settings page was asked to save, but no path is configured.");
            pound_settings_set_status(view, true, "There is nowhere to save to.");
        }
        else
        {
            const error_t status = pound_config_save(config, context->settings_path);

            if (POUND_SUCCESS == status)
            {
                pound_settings_set_status(view, false, "Settings saved.");
            }
            else
            {
                POUND_LOG_ERROR(&thread_logger,
                                "Saving the settings to '%s' failed with status %d.",
                                context->settings_path,
                                (int)status);
                pound_settings_set_status(view, true, "Could not save; see the Logs page for why.");
            }
        }
    }

    igSameLine(0.0F, 8.0F);

    if (igButton("Reload from file", (ImVec2){ 0.0F, 0.0F }))
    {
        if (NULL == context->settings_path)
        {
            POUND_LOG_ERROR(&thread_logger,
                            "The Settings page was asked to reload, but no path is configured.");
            pound_settings_set_status(view, true, "There is nowhere to reload from.");
        }
        else
        {
            const error_t status = pound_config_load(config, context->settings_path);

            if (POUND_SUCCESS == status)
            {
                // A reload can change the theme and the scale, so the appearance is
                // re-applied from whatever the file said.
                pound_settings_mark_appearance_dirty(context);
                pound_settings_set_status(view, false, "Settings reloaded from the file.");
            }
            else
            {
                POUND_LOG_ERROR(&thread_logger,
                                "Reloading the settings from '%s' failed with status %d.",
                                context->settings_path,
                                (int)status);
                pound_settings_set_status(
                    view, true, "Reloaded with problems; see the Logs page for the lines involved.");
            }
        }
    }

    if ('\0' != view->settings_status[0])
    {
        igSpacing();
        const int role = (true == view->settings_status_is_error) ? POUND_THEME_STATUS_ERROR
                                                                  : POUND_THEME_STATUS_SUCCESS;
        igPushStyleColor_Vec4(ImGuiCol_Text, pound_theme_imgui_color(context->theme, role));
        igTextUnformatted(view->settings_status, NULL);
        igPopStyleColor(1);
    }
}

/*** end of file ***/

#include "pages.h"

/// The version string CMake derived from `project(... VERSION ...)`.
///
/// Defaulted rather than required so the file still compiles in a translation unit
/// that is built without the definition -- the test suite, for instance. An
/// "unknown" version in a bug report is a useful signal; a build failure over a
/// label is not.
#ifndef POUND_VERSION
#define POUND_VERSION "unknown"
#endif

/// The About page.
///
/// It says what this build is and, more importantly, what it is not. An emulator's
/// About box that promises more than the build does is the first thing that makes a
/// user distrust the rest of the interface, so the capability list here is written
/// against the code that exists, not against a roadmap.
void
pound_page_render_about(const pound_page_context_t *POUND_RESTRICT context)
{
    if (POUND_UNLIKELY(NULL == context))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: context is NULL.");
        return;
    }

    pound_page_heading(context,
                       "ABOUT",
                       "Pound",
                       "An experimental Nintendo Switch emulator, written in C.");

    if (pound_page_begin_card(context, "##about_versions", 0.0F))
    {
        pound_page_labeled_value(context, "Pound", POUND_VERSION, 0.35F);

        const char *const imgui_version = igGetVersion();
        pound_page_labeled_value(context,
                                 "Dear ImGui",
                                 (NULL != imgui_version) ? imgui_version : "unknown",
                                 0.35F);

        pound_page_labeled_value(
            context,
            "Theme",
            pound_theme_mode_to_string(pound_theme_get_mode(context->theme)),
            0.35F);
    }

    pound_page_end_card();

    pound_page_section(context, "What this build does");

    igTextWrapped("Draws a themed, scaled interface with a settings file that persists between "
                  "runs, a live log view, and a map of the guest address space.");
    igTextWrapped("Reads PFS0 and HFS0 partitions, including per-entry hashes and the contents "
                  "of each entry.");

    pound_page_section(context, "What this build does not do yet");

    igPushStyleColor_Vec4(
        ImGuiCol_Text, pound_theme_imgui_color(context->theme, POUND_THEME_TEXT_SECONDARY));
    const bool body_font_pushed = pound_fonts_push(context->fonts, POUND_FONT_ROLE_SMALL);
    igTextWrapped(
        "Open or run a game. The encrypted title archive and the AES layer beneath it are still "
        "being written, so a game image can be listed but not decrypted or executed.");
    igTextWrapped(
        "Render anything. The graphics processor in this hardware exposes no documented instruction "
        "set, so no renderer for it can be written from public information; Pound does not claim "
        "one.");
    pound_fonts_pop(body_font_pushed);
    igPopStyleColor(1);

    pound_page_section(context, "Keys");

    igPushStyleColor_Vec4(
        ImGuiCol_Text, pound_theme_imgui_color(context->theme, POUND_THEME_TEXT_SECONDARY));
    const bool keys_font_pushed = pound_fonts_push(context->fonts, POUND_FONT_ROLE_SMALL);
    igTextWrapped("Pound contains no Nintendo keys and never will. Reading an encrypted title "
                  "requires a keys file you provide yourself, exactly as other Switch emulators "
                  "require.");
    pound_fonts_pop(keys_font_pushed);
    igPopStyleColor(1);
}

/*** end of file ***/

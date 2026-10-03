#include "pages.h"

/// The Library page.
///
/// There is no content scanner yet: the parsing layers that turn a card image
/// into a loadable executable are still being built, so this page cannot list
/// titles. What it can do honestly is show where Pound will look and say plainly
/// that the shelf is empty because nothing has been added, rather than showing a
/// skeleton grid of fake entries. A placeholder that looks like data is worse than
/// an empty state, because a user cannot tell the two apart.
void
pound_page_render_library(const pound_page_context_t *POUND_RESTRICT context)
{
    if (POUND_UNLIKELY(NULL == context))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: context is NULL.");
        return;
    }

    pound_page_heading(context,
                       "LIBRARY",
                       "Library",
                       "Every title Pound can open will be listed here, with its icon and "
                       "the format it was read from.");

    if (pound_page_begin_card(context, "##library_empty_state", 220.0F))
    {
        pound_page_empty_state(context,
                               "Nothing on the shelf yet",
                               "Pound is not scanning for games yet -- the layers that read a "
                               "card image are still being written. Once they land, the folder "
                               "below is where this page will look.");
    }

    pound_page_end_card();

    pound_page_section(context, "Where Pound looks");

    const pound_config_t *const config = context->config;

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

    pound_page_section(context, "Formats Pound is being taught to read");

    igTextWrapped("Cartridge images (XCI) and their partitions.");
    igTextWrapped("Digital packages (NSP) and the filesystems inside them.");
    igTextWrapped("The encrypted title archives (NCA) those contain, opened with your keys.");
    igSpacing();
    igPushStyleColor_Vec4(
        ImGuiCol_Text, pound_theme_imgui_color(context->theme, POUND_THEME_TEXT_SECONDARY));
    const bool font_pushed = pound_fonts_push(context->fonts, POUND_FONT_ROLE_SMALL);
    igTextWrapped("Pound never ships or embeds Nintendo's keys. You supply a keys file, exactly "
                  "as other Switch emulators require.");
    pound_fonts_pop(font_pushed);
    igPopStyleColor(1);
}

/*** end of file ***/

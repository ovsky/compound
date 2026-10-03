#include "pages.h"

#include <math.h>

/// The registry, indexed by `pound_page_t`.
///
/// Every entry names a function that is defined in one of the five page
/// translation units beside this file. The table is the only place that maps a
/// page to its renderer, so a page that exists but is missing here is a link
/// error, and a page here but absent from the enum is impossible by construction.
static const pound_page_descriptor_t POUND_PAGE_REGISTRY[POUND_PAGE_COUNT] = {
    [POUND_PAGE_LIBRARY]  = { "Library", "Your installed titles", pound_page_render_library },
    [POUND_PAGE_MEMORY]   = { "Memory", "The guest address space", pound_page_render_memory },
    [POUND_PAGE_LOGS]     = { "Logs", "Everything Pound has reported", pound_page_render_logs },
    [POUND_PAGE_SETTINGS] = { "Settings", "Where things live and how loud to be",
                              pound_page_render_settings },
    [POUND_PAGE_ABOUT]    = { "About", "What this build can do", pound_page_render_about },
};

/// The registry is indexed by the enum, so a mismatch is a compile-time fact
/// rather than a runtime surprise.
_Static_assert((int)POUND_PAGE_COUNT == 5,
               "POUND_PAGE_REGISTRY has a different length from pound_page_t");

const pound_page_descriptor_t *
pound_pages_registry(void)
{
    return POUND_PAGE_REGISTRY;
}

bool
pound_page_is_valid(const int page)
{
    return (page >= 0) && (page < (int)POUND_PAGE_COUNT);
}

const char *
pound_page_name(const int page)
{
    if (false == pound_page_is_valid(page))
    {
        return "Unknown";
    }

    return POUND_PAGE_REGISTRY[page].name;
}

void
pound_page_render(const pound_page_t page, const pound_page_context_t *POUND_RESTRICT context)
{
    if (POUND_UNLIKELY(NULL == context))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: context is NULL.");
        return;
    }

    if (POUND_UNLIKELY(false == pound_page_is_valid((int)page)))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: page %d is not a page.", (int)page);
        return;
    }

    POUND_PAGE_REGISTRY[page].render(context);
}

/// Pushes a font role, returning whether it took effect.
///
/// Silently does nothing when the atlas was never built: that failure is already
/// logged once, at setup, and repeating it every frame would bury every other
/// record under it. A caller pairs this with `pound_page_pop_font` using the
/// returned value, so a page that runs before fonts exist draws at the default
/// size instead of pushing and popping a font it does not have.
static bool
pound_page_push_font(const pound_page_context_t *POUND_RESTRICT context, const pound_font_role_t role)
{
    if ((NULL == context) || (NULL == context->fonts) || (false == context->fonts->setup))
    {
        return false;
    }

    return pound_fonts_push(context->fonts, role);
}

static void
pound_page_pop_font(const bool pushed)
{
    pound_fonts_pop(pushed);
}

void
pound_page_heading(const pound_page_context_t *POUND_RESTRICT context,
                   const char *POUND_RESTRICT                  eyebrow,
                   const char *POUND_RESTRICT                  title,
                   const char *POUND_RESTRICT                  subtitle)
{
    if (POUND_UNLIKELY(NULL == context) || POUND_UNLIKELY(NULL == title))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: context or title is NULL.");
        return;
    }

    if (NULL != eyebrow)
    {
        igPushStyleColor_Vec4(ImGuiCol_Text, pound_theme_imgui_color(context->theme, POUND_THEME_ACCENT));
        const bool font_pushed = pound_page_push_font(context, POUND_FONT_ROLE_SMALL);
        igTextUnformatted(eyebrow, NULL);
        pound_page_pop_font(font_pushed);
        igPopStyleColor(1);
        igSpacing();
    }

    const bool title_pushed = pound_page_push_font(context, POUND_FONT_ROLE_TITLE);
    igTextUnformatted(title, NULL);
    pound_page_pop_font(title_pushed);

    if (NULL != subtitle)
    {
        const ImVec4 muted = pound_theme_imgui_color(context->theme, POUND_THEME_TEXT_SECONDARY);
        igPushStyleColor_Vec4(ImGuiCol_Text, muted);
        const float wrap = igGetContentRegionAvail().x * 0.75F;
        if (wrap > 0.0F)
        {
            igPushTextWrapPos(wrap);
        }

        igTextUnformatted(subtitle, NULL);

        if (wrap > 0.0F)
        {
            igPopTextWrapPos();
        }

        igPopStyleColor(1);
    }

    igSpacing();
}

void
pound_page_section(const pound_page_context_t *POUND_RESTRICT context,
                   const char *POUND_RESTRICT                  title)
{
    if (POUND_UNLIKELY(NULL == context) || POUND_UNLIKELY(NULL == title))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: context or title is NULL.");
        return;
    }

    igSpacing();
    igSeparatorText(title);
}

bool
pound_page_begin_card(const pound_page_context_t *POUND_RESTRICT context,
                      const char *POUND_RESTRICT                  id,
                      const float                                 height)
{
    if (POUND_UNLIKELY(NULL == context) || POUND_UNLIKELY(NULL == id))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: context or id is NULL.");
        return false;
    }

    const ImVec4 surface = pound_theme_imgui_color(context->theme, POUND_THEME_SURFACE);
    const ImVec4 border  = pound_theme_imgui_color(context->theme, POUND_THEME_BORDER);

    igPushStyleColor_Vec4(ImGuiCol_ChildBg, surface);
    igPushStyleColor_Vec4(ImGuiCol_Border, border);
    igPushStyleVar_Float(ImGuiStyleVar_ChildBorderSize,
                         (context->theme != NULL) ? context->theme->stroke.subtle : 1.0F);

    const ImVec2 size = { 0.0F, height };

    // The border flag is what makes `ChildBorderSize` mean anything; without it the
    // card is a rectangle of surface colour with no edge, which reads as a layout
    // accident on a surface-coloured page.
    return igBeginChild_Str(id,
                            size,
                            ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                            ImGuiWindowFlags_None);
}

void
pound_page_end_card(void)
{
    igEndChild();
    igPopStyleVar(1);
    igPopStyleColor(2);
}

void
pound_page_empty_state(const pound_page_context_t *POUND_RESTRICT context,
                       const char *POUND_RESTRICT                  title,
                       const char *POUND_RESTRICT                  body)
{
    if (POUND_UNLIKELY(NULL == context) || POUND_UNLIKELY(NULL == title))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: context or title is NULL.");
        return;
    }

    // Pushed well down the card so the message sits in the middle of the space
    // rather than clinging to the top, and wrapped to a readable column so a long
    // explanation does not run the full width of a wide window.
    const ImVec2 available   = igGetContentRegionAvail();
    const float  wrap_width  = (available.x < 560.0F) ? available.x : 560.0F;
    const float  top_padding = (available.y > 180.0F) ? 64.0F : 16.0F;

    igDummy((ImVec2){ 0.0F, top_padding });

    const ImVec2 title_size = igCalcTextSize(title, NULL, false, 0.0F);
    const float  title_x    = (available.x > title_size.x) ? (available.x - title_size.x) * 0.5F : 0.0F;
    const float  origin_x   = igGetCursorPosX();
    igSetCursorPosX(origin_x + title_x);

    const bool title_pushed = pound_page_push_font(context, POUND_FONT_ROLE_TITLE);
    igTextUnformatted(title, NULL);
    pound_page_pop_font(title_pushed);

    if (NULL == body)
    {
        return;
    }

    igSpacing();

    const float body_x = (available.x > wrap_width) ? (available.x - wrap_width) * 0.5F : 0.0F;
    igSetCursorPosX(igGetCursorPosX() + body_x);

    igPushStyleColor_Vec4(ImGuiCol_Text, pound_theme_imgui_color(context->theme, POUND_THEME_TEXT_SECONDARY));
    igPushTextWrapPos(igGetCursorPosX() + wrap_width);
    igTextUnformatted(body, NULL);
    igPopTextWrapPos();
    igPopStyleColor(1);
}

void
pound_page_labeled_value(const pound_page_context_t *POUND_RESTRICT context,
                         const char *POUND_RESTRICT                  label,
                         const char *POUND_RESTRICT                  value,
                         const float                                 percent_width)
{
    if (POUND_UNLIKELY(NULL == context) || POUND_UNLIKELY(NULL == label))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: context or label is NULL.");
        return;
    }

    float fraction = percent_width;

    if (POUND_UNLIKELY((false == isfinite(fraction)) || (fraction <= 0.0F) || (fraction >= 1.0F)))
    {
        POUND_LOG_WARN(&thread_logger,
                       "A label width of %g is outside (0, 1); using 0.3.",
                       (double)fraction);
        fraction = 0.3F;
    }

    const float width = igGetContentRegionAvail().x * fraction;

    igPushStyleColor_Vec4(ImGuiCol_Text, pound_theme_imgui_color(context->theme, POUND_THEME_TEXT_SECONDARY));
    igAlignTextToFramePadding();
    igTextUnformatted(label, NULL);
    igPopStyleColor(1);

    igSameLine(width, -1.0F);
    igTextUnformatted((NULL != value) ? value : "-", NULL);
}

void
pound_page_setting_label(const pound_page_context_t *POUND_RESTRICT context,
                         const char *POUND_RESTRICT                  label,
                         const char *POUND_RESTRICT                  help)
{
    if (POUND_UNLIKELY(NULL == context) || POUND_UNLIKELY(NULL == label))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: context or label is NULL.");
        return;
    }

    igTextUnformatted(label, NULL);

    if (NULL == help)
    {
        return;
    }

    igPushStyleColor_Vec4(ImGuiCol_Text, pound_theme_imgui_color(context->theme, POUND_THEME_TEXT_SECONDARY));
    const bool font_pushed = pound_page_push_font(context, POUND_FONT_ROLE_SMALL);
    const float wrap = igGetContentRegionAvail().x * 0.8F;
    if (wrap > 0.0F)
    {
        igPushTextWrapPos(wrap);
    }

    igTextUnformatted(help, NULL);

    if (wrap > 0.0F)
    {
        igPopTextWrapPos();
    }

    pound_page_pop_font(font_pushed);
    igPopStyleColor(1);
}

/// The levels, loudest first, with the names the interface uses for them.
///
/// One table drives the filter combo, the settings control and the labels drawn on
/// each log record, so those three cannot drift apart.
static const struct
{
    const char *name;
    log_level_t level;

    /// Explicit padding: a pointer and a four-byte enum leave four bytes of tail
    /// that the build will not accept as implicit padding.
    char pad[4];
} POUND_PAGE_LOG_LEVELS[] = {
    { "Trace", LOG_LEVEL_TRACE, { 0 } },
    { "Debug", LOG_LEVEL_DEBUG, { 0 } },
    { "Info", LOG_LEVEL_INFO, { 0 } },
    { "Warn", LOG_LEVEL_WARN, { 0 } },
    { "Error", LOG_LEVEL_ERROR, { 0 } },
    { "None", LOG_LEVEL_NONE, { 0 } },
};

#define POUND_PAGE_LOG_LEVEL_COUNT (sizeof(POUND_PAGE_LOG_LEVELS) / sizeof(POUND_PAGE_LOG_LEVELS[0]))

_Static_assert((int)POUND_PAGE_LOG_LEVEL_COUNT == 6, "the log level table has the wrong length");

const char *
pound_page_log_level_items(void)
{
    // The same six names as the table above, in the same order, joined by NULs with
    // a second NUL at the end. ImGui reads it as a list; nothing else parses it, so
    // the duplication is checked by eye against six adjacent lines.
    return "Trace\0Debug\0Info\0Warn\0Error\0None\0";
}

int
pound_page_log_level_index(const log_level_t level)
{
    for (size_t i = 0; i < POUND_PAGE_LOG_LEVEL_COUNT; i++)
    {
        if (POUND_PAGE_LOG_LEVELS[i].level == level)
        {
            return (int)i;
        }
    }

    return 3; // Warn, the level the config also ships with.
}

log_level_t
pound_page_log_level_at(const int index)
{
    if ((index < 0) || ((size_t)index >= POUND_PAGE_LOG_LEVEL_COUNT))
    {
        return LOG_LEVEL_WARN;
    }

    return POUND_PAGE_LOG_LEVELS[index].level;
}

const char *
pound_page_log_level_label(const log_level_t level)
{
    switch (level)
    {
        case LOG_LEVEL_TRACE:
            return "trace";
        case LOG_LEVEL_DEBUG:
            return "debug";
        case LOG_LEVEL_INFO:
            return "info";
        case LOG_LEVEL_WARN:
            return "warn";
        case LOG_LEVEL_ERROR:
            return "error";
        case LOG_LEVEL_NONE:
            return "none";
    }

    return "unknown";
}

/*** end of file ***/

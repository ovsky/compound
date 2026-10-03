#include "theme_imgui.h"

#include "log.h"

/// Reads a role out of the palette.
///
/// Every colour write below goes through this rather than reaching into the
/// struct, so the mapping from a palette member to an ImGui slot exists in exactly
/// one place and the "no literal colours on the ImGui side" rule is enforceable by
/// reading rather than by remembering.
static ImVec4 pound_theme_slot(const pound_theme_t *POUND_RESTRICT theme, const int role_index)
{
    const pound_theme_color_t *const color = pound_theme_color_role(&theme->colors, role_index);

    if (NULL == color)
    {
        return (ImVec4){ 0.0F, 0.0F, 0.0F, 0.0F };
    }

    return (ImVec4){ (*color)[0], (*color)[1], (*color)[2], (*color)[3] };
}

error_t
pound_theme_imgui_apply(const pound_theme_t *POUND_RESTRICT theme)
{
    if (POUND_UNLIKELY(NULL == theme))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: theme is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(false == pound_theme_is_valid(theme)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the theme is incomplete, so the style has been "
                        "left as it was rather than half-recoloured.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    ImGuiStyle *style = igGetStyle();

    if (POUND_UNLIKELY(NULL == style))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: there is no ImGui context, so there is no "
                        "style to apply the theme to.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    // Metrics. Padding and spacing come off the scale rather than off literals so
    // the density of the interface is a theme decision, not a per-widget one.
    style->WindowPadding.x    = theme->spacing.md;
    style->WindowPadding.y    = theme->spacing.md;
    style->FramePadding.x     = theme->spacing.md;
    style->FramePadding.y     = theme->spacing.sm;
    style->ItemSpacing.x      = theme->spacing.sm;
    style->ItemSpacing.y      = theme->spacing.sm;
    style->ItemInnerSpacing.x = theme->spacing.xs;
    style->ItemInnerSpacing.y = theme->spacing.xs;
    style->CellPadding.x      = theme->spacing.xs;
    style->CellPadding.y      = theme->spacing.xs;
    style->IndentSpacing.x    = theme->spacing.lg;
    style->ScrollbarSize      = theme->spacing.lg;
    style->GrabMinSize        = theme->spacing.lg;
    style->SeparatorTextBorderSize
        = (theme->stroke.hairline > 0.0F) ? theme->stroke.hairline : theme->spacing.xxs;

    // Corners. The pill radius is a sentinel, so it is left at ImGui's default of
    // 0 rather than being pushed through as 999 -- a tab bar rounded by 999px is
    // rounded by 999px. The renderer clamps per control where it wants a pill.
    style->WindowRounding  = theme->radius.md;
    style->ChildRounding   = theme->radius.md;
    style->FrameRounding   = theme->radius.sm;
    style->PopupRounding   = theme->radius.md;
    style->GrabRounding    = theme->radius.sm;
    style->TabRounding     = theme->radius.sm;
    style->ScrollbarRounding = theme->radius.pill;

    // Borders. Windows and children carry no outline by default: the interface is
    // separated by surface value, and a border round every panel would turn the
    // layout into a grid of boxes.
    style->WindowBorderSize = theme->stroke.none;
    style->ChildBorderSize  = theme->stroke.none;
    style->PopupBorderSize  = theme->stroke.hairline;
    style->FrameBorderSize  = theme->stroke.subtle;
    style->TabBarBorderSize = theme->stroke.hairline;
    style->TabBorderSize    = theme->stroke.subtle;
    style->SeparatorSize     = theme->stroke.hairline;
    style->Alpha             = 1.0F;
    // Disabled controls are dimmed through `DisabledAlpha` rather than through a
    // separate colour, so a disabled button stays recognisably the same button.
    style->DisabledAlpha     = 0.45F;

    ImVec4 *const colors = style->Colors;

    colors[ImGuiCol_Text]           = pound_theme_slot(theme, POUND_THEME_TEXT_PRIMARY);
    colors[ImGuiCol_TextDisabled]   = pound_theme_slot(theme, POUND_THEME_TEXT_DISABLED);
    colors[ImGuiCol_WindowBg]       = pound_theme_slot(theme, POUND_THEME_SURFACE_BACKGROUND);
    colors[ImGuiCol_ChildBg]        = (ImVec4){ 0.0F, 0.0F, 0.0F, 0.0F };
    colors[ImGuiCol_PopupBg]        = pound_theme_slot(theme, POUND_THEME_SURFACE);
    colors[ImGuiCol_Border]         = pound_theme_slot(theme, POUND_THEME_BORDER);
    colors[ImGuiCol_BorderShadow]   = (ImVec4){ 0.0F, 0.0F, 0.0F, 0.0F };
    colors[ImGuiCol_FrameBg]        = pound_theme_slot(theme, POUND_THEME_SURFACE_RAISED);
    colors[ImGuiCol_FrameBgHovered] = pound_theme_slot(theme, POUND_THEME_SURFACE_HOVER);
    colors[ImGuiCol_FrameBgActive]  = pound_theme_slot(theme, POUND_THEME_ACCENT_MUTED);
    colors[ImGuiCol_TitleBg]        = pound_theme_slot(theme, POUND_THEME_SURFACE_BACKGROUND);
    colors[ImGuiCol_TitleBgActive]  = pound_theme_slot(theme, POUND_THEME_SURFACE_BACKGROUND);
    colors[ImGuiCol_TitleBgCollapsed] = pound_theme_slot(theme, POUND_THEME_SURFACE_BACKGROUND);
    colors[ImGuiCol_MenuBarBg]      = pound_theme_slot(theme, POUND_THEME_SURFACE);
    colors[ImGuiCol_ScrollbarBg]    = (ImVec4){ 0.0F, 0.0F, 0.0F, 0.0F };
    colors[ImGuiCol_ScrollbarGrab]  = pound_theme_slot(theme, POUND_THEME_BORDER);
    colors[ImGuiCol_ScrollbarGrabHovered]
        = pound_theme_slot(theme, POUND_THEME_BORDER_STRONG);
    colors[ImGuiCol_ScrollbarGrabActive] = pound_theme_slot(theme, POUND_THEME_ACCENT);

    // Accent for every control that carries the primary action, and the on-accent
    // text that sits on top of them.
    colors[ImGuiCol_Button]            = pound_theme_slot(theme, POUND_THEME_ACCENT);
    colors[ImGuiCol_ButtonHovered]      = pound_theme_slot(theme, POUND_THEME_ACCENT_HOVER);
    colors[ImGuiCol_ButtonActive]     = pound_theme_slot(theme, POUND_THEME_ACCENT_ACTIVE);
    colors[ImGuiCol_CheckMark]        = pound_theme_slot(theme, POUND_THEME_ACCENT);
    colors[ImGuiCol_SliderGrab]       = pound_theme_slot(theme, POUND_THEME_ACCENT);
    colors[ImGuiCol_SliderGrabActive] = pound_theme_slot(theme, POUND_THEME_ACCENT_HOVER);

    // Headers are the sidebar's and the page's own rows, so they get the accent
    // wash rather than a raised surface: a selected row should read as tinted,
    // not as a box.
    colors[ImGuiCol_Header]        = pound_theme_slot(theme, POUND_THEME_ACCENT_MUTED);
    colors[ImGuiCol_HeaderHovered] = pound_theme_slot(theme, POUND_THEME_SURFACE_HOVER);
    colors[ImGuiCol_HeaderActive]  = pound_theme_slot(theme, POUND_THEME_ACCENT_MUTED);

    colors[ImGuiCol_Separator]            = pound_theme_slot(theme, POUND_THEME_BORDER);
    colors[ImGuiCol_SeparatorHovered]     = pound_theme_slot(theme, POUND_THEME_BORDER_STRONG);
    colors[ImGuiCol_SeparatorActive]      = pound_theme_slot(theme, POUND_THEME_ACCENT);
    colors[ImGuiCol_ResizeGrip]           = (ImVec4){ 0.0F, 0.0F, 0.0F, 0.0F };
    colors[ImGuiCol_ResizeGripHovered]    = (ImVec4){ 0.0F, 0.0F, 0.0F, 0.0F };
    colors[ImGuiCol_ResizeGripActive]     = (ImVec4){ 0.0F, 0.0F, 0.0F, 0.0F };

    colors[ImGuiCol_Tab]                = pound_theme_slot(theme, POUND_THEME_SURFACE_BACKGROUND);
    colors[ImGuiCol_TabHovered]         = pound_theme_slot(theme, POUND_THEME_SURFACE_HOVER);
    colors[ImGuiCol_TabSelected] = pound_theme_slot(theme, POUND_THEME_SURFACE);
    colors[ImGuiCol_TabDimmed]   = pound_theme_slot(theme, POUND_THEME_SURFACE_BACKGROUND);
    colors[ImGuiCol_TabDimmedSelected]  = pound_theme_slot(theme, POUND_THEME_SURFACE);
    colors[ImGuiCol_DockingPreview]     = pound_theme_slot(theme, POUND_THEME_ACCENT_MUTED);
    colors[ImGuiCol_DockingEmptyBg]     = pound_theme_slot(theme, POUND_THEME_SURFACE_BACKGROUND);

    colors[ImGuiCol_PlotLines]       = pound_theme_slot(theme, POUND_THEME_ACCENT);
    colors[ImGuiCol_PlotLinesHovered] = pound_theme_slot(theme, POUND_THEME_ACCENT_HOVER);
    colors[ImGuiCol_PlotHistogram]   = pound_theme_slot(theme, POUND_THEME_ACCENT_MUTED);
    colors[ImGuiCol_PlotHistogramHovered] = pound_theme_slot(theme, POUND_THEME_ACCENT);

    colors[ImGuiCol_TableHeaderBg]      = pound_theme_slot(theme, POUND_THEME_SURFACE_RAISED);
    colors[ImGuiCol_TableBorderStrong]  = pound_theme_slot(theme, POUND_THEME_BORDER);
    colors[ImGuiCol_TableBorderLight]   = pound_theme_slot(theme, POUND_THEME_BORDER);
    colors[ImGuiCol_TableRowBg]         = (ImVec4){ 0.0F, 0.0F, 0.0F, 0.0F };
    colors[ImGuiCol_TableRowBgAlt]      = (ImVec4){ 0.0F, 0.0F, 0.0F, 0.0F };
    colors[ImGuiCol_TextSelectedBg]     = pound_theme_slot(theme, POUND_THEME_ACCENT_MUTED);
    colors[ImGuiCol_TextLink]           = pound_theme_slot(theme, POUND_THEME_ACCENT);
    colors[ImGuiCol_TextSelectedBgFocused] = pound_theme_slot(theme, POUND_THEME_ACCENT_MUTED);

    colors[ImGuiCol_DragAverage]             = pound_theme_slot(theme, POUND_THEME_ACCENT);
    colors[ImGuiCol_DragAverageActive]       = pound_theme_slot(theme, POUND_THEME_ACCENT_HOVER);
    colors[ImGuiCol_DragDropTarget]          = pound_theme_slot(theme, POUND_THEME_ACCENT_HOVER);
    colors[ImGuiCol_NavCursor]               = pound_theme_slot(theme, POUND_THEME_BORDER_STRONG);
    colors[ImGuiCol_NavWindowingHighlight]   = pound_theme_slot(theme, POUND_THEME_ACCENT);
    colors[ImGuiCol_NavWindowingDimBg]       = (ImVec4){ 0.0F, 0.0F, 0.0F, 0.0F };
    colors[ImGuiCol_ModalWindowDimBg]
        = { 0.0F, 0.0F, 0.0F, 0.55F }; // Dimming the whole viewport for a modal is the one
                                        // place a literal is right: it is not a palette
                                        // colour, it is an amount of veil.

    return POUND_SUCCESS;
}

ImVec4
pound_theme_imgui_color(const pound_theme_t *POUND_RESTRICT theme, const int role_index)
{
    if (POUND_UNLIKELY(NULL == theme))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: theme is NULL.");
        return (ImVec4){ 0.0F, 0.0F, 0.0F, 0.0F };
    }

    return pound_theme_slot(theme, role_index);
}

/*** end of file ***/
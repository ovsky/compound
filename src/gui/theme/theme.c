//! Modern, clean, minimalistic theme implementation.

#include "theme.h"

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"
#include <math.h>

static void pound_theme_set_dark(pound_theme_t *POUND_RESTRICT theme)
{
    // Backgrounds
    theme->colors.bg_primary[0] = 0.05f; theme->colors.bg_primary[1] = 0.05f; theme->colors.bg_primary[2] = 0.06f; theme->colors.bg_primary[3] = 1.0f;
    theme->colors.bg_secondary[0] = 0.08f; theme->colors.bg_secondary[1] = 0.08f; theme->colors.bg_secondary[2] = 0.09f; theme->colors.bg_secondary[3] = 1.0f;
    theme->colors.bg_tertiary[0] = 0.10f; theme->colors.bg_tertiary[1] = 0.10f; theme->colors.bg_tertiary[2] = 0.11f; theme->colors.bg_tertiary[3] = 1.0f;
    theme->colors.bg_panel[0] = 0.07f; theme->colors.bg_panel[1] = 0.07f; theme->colors.bg_panel[2] = 0.08f; theme->colors.bg_panel[3] = 0.98f;
    theme->colors.bg_surface[0] = 0.09f; theme->colors.bg_surface[1] = 0.09f; theme->colors.bg_surface[2] = 0.10f; theme->colors.bg_surface[3] = 1.0f;
    theme->colors.border[0] = 0.18f; theme->colors.border[1] = 0.18f; theme->colors.border[2] = 0.20f; theme->colors.border[3] = 1.0f;
    // Text
    theme->colors.text_primary[0] = 0.92f; theme->colors.text_primary[1] = 0.92f; theme->colors.text_primary[2] = 0.94f; theme->colors.text_primary[3] = 1.0f;
    theme->colors.text_secondary[0] = 0.70f; theme->colors.text_secondary[1] = 0.70f; theme->colors.text_secondary[2] = 0.74f; theme->colors.text_secondary[3] = 1.0f;
    theme->colors.text_disabled[0] = 0.40f; theme->colors.text_disabled[1] = 0.40f; theme->colors.text_disabled[2] = 0.44f; theme->colors.text_disabled[3] = 1.0f;
    // Accent
    theme->colors.accent_primary[0] = 0.45f; theme->colors.accent_primary[1] = 0.65f; theme->colors.accent_primary[2] = 1.00f; theme->colors.accent_primary[3] = 1.0f;
    theme->colors.accent_hover[0] = 0.55f; theme->colors.accent_hover[1] = 0.72f; theme->colors.accent_hover[2] = 1.00f; theme->colors.accent_hover[3] = 1.0f;
    theme->colors.accent_active[0] = 0.38f; theme->colors.accent_active[1] = 0.58f; theme->colors.accent_active[2] = 0.92f; theme->colors.accent_active[3] = 1.0f;
    theme->colors.accent_faded[0] = 0.45f; theme->colors.accent_faded[1] = 0.65f; theme->colors.accent_faded[2] = 1.00f; theme->colors.accent_faded[3] = 0.20f;
    // Status
    theme->colors.success[0] = 0.36f; theme->colors.success[1] = 0.80f; theme->colors.success[2] = 0.48f; theme->colors.success[3] = 1.0f;
    theme->colors.warning[0] = 1.00f; theme->colors.warning[1] = 0.78f; theme->colors.warning[2] = 0.28f; theme->colors.warning[3] = 1.0f;
    theme->colors.error[0] = 1.00f; theme->colors.error[1] = 0.44f; theme->colors.error[2] = 0.44f; theme->colors.error[3] = 1.0f;
    theme->colors.info[0] = 0.40f; theme->colors.info[1] = 0.70f; theme->colors.info[2] = 1.00f; theme->colors.info[3] = 1.0f;
    theme->colors.shadow[0] = 0.00f; theme->colors.shadow[1] = 0.00f; theme->colors.shadow[2] = 0.00f; theme->colors.shadow[3] = 0.40f;
}

static void pound_theme_set_light(pound_theme_t *POUND_RESTRICT theme)
{
    theme->colors.bg_primary[0] = 0.98f; theme->colors.bg_primary[1] = 0.98f; theme->colors.bg_primary[2] = 1.00f; theme->colors.bg_primary[3] = 1.0f;
    theme->colors.bg_secondary[0] = 0.96f; theme->colors.bg_secondary[1] = 0.96f; theme->colors.bg_secondary[2] = 0.98f; theme->colors.bg_secondary[3] = 1.0f;
    theme->colors.bg_tertiary[0] = 0.94f; theme->colors.bg_tertiary[1] = 0.94f; theme->colors.bg_tertiary[2] = 0.96f; theme->colors.bg_tertiary[3] = 1.0f;
    theme->colors.bg_panel[0] = 0.98f; theme->colors.bg_panel[1] = 0.98f; theme->colors.bg_panel[2] = 1.00f; theme->colors.bg_panel[3] = 0.98f;
    theme->colors.bg_surface[0] = 1.00f; theme->colors.bg_surface[1] = 1.00f; theme->colors.bg_surface[2] = 1.00f; theme->colors.bg_surface[3] = 1.0f;
    theme->colors.border[0] = 0.84f; theme->colors.border[1] = 0.84f; theme->colors.border[2] = 0.88f; theme->colors.border[3] = 1.0f;
    theme->colors.text_primary[0] = 0.10f; theme->colors.text_primary[1] = 0.10f; theme->colors.text_primary[2] = 0.12f; theme->colors.text_primary[3] = 1.0f;
    theme->colors.text_secondary[0] = 0.32f; theme->colors.text_secondary[1] = 0.32f; theme->colors.text_secondary[2] = 0.36f; theme->colors.text_secondary[3] = 1.0f;
    theme->colors.text_disabled[0] = 0.60f; theme->colors.text_disabled[1] = 0.60f; theme->colors.text_disabled[2] = 0.64f; theme->colors.text_disabled[3] = 1.0f;
    theme->colors.accent_primary[0] = 0.20f; theme->colors.accent_primary[1] = 0.50f; theme->colors.accent_primary[2] = 0.95f; theme->colors.accent_primary[3] = 1.0f;
    theme->colors.accent_hover[0] = 0.24f; theme->colors.accent_hover[1] = 0.54f; theme->colors.accent_hover[2] = 1.00f; theme->colors.accent_hover[3] = 1.0f;
    theme->colors.accent_active[0] = 0.18f; theme->colors.accent_active[1] = 0.44f; theme->colors.accent_active[2] = 0.88f; theme->colors.accent_active[3] = 1.0f;
    theme->colors.accent_faded[0] = 0.20f; theme->colors.accent_faded[1] = 0.50f; theme->colors.accent_faded[2] = 0.95f; theme->colors.accent_faded[3] = 0.12f;
    theme->colors.success[0] = 0.20f; theme->colors.success[1] = 0.68f; theme->colors.success[2] = 0.32f; theme->colors.success[3] = 1.0f;
    theme->colors.warning[0] = 0.90f; theme->colors.warning[1] = 0.60f; theme->colors.warning[2] = 0.10f; theme->colors.warning[3] = 1.0f;
    theme->colors.error[0] = 0.92f; theme->colors.error[1] = 0.26f; theme->colors.error[2] = 0.26f; theme->colors.error[3] = 1.0f;
    theme->colors.info[0] = 0.20f; theme->colors.info[1] = 0.56f; theme->colors.info[2] = 0.92f; theme->colors.info[3] = 1.0f;
    theme->colors.shadow[0] = 0.00f; theme->colors.shadow[1] = 0.00f; theme->colors.shadow[2] = 0.00f; theme->colors.shadow[3] = 0.16f;
}

void pound_theme_init(pound_theme_t *POUND_RESTRICT theme)
{
    if (NULL == theme)
        return;
    theme->mode = POUND_THEME_DARK;
    // Spacing scale
    theme->spacing.space_xs = 4.0f;
    theme->spacing.space_s = 6.0f;
    theme->spacing.space_m = 8.0f;
    theme->spacing.space_l = 12.0f;
    theme->spacing.space_xl = 16.0f;
    theme->spacing.space_2xl = 24.0f;
    // Radius
    theme->radius.radius_xs = 4.0f;
    theme->radius.radius_s = 6.0f;
    theme->radius.radius_m = 8.0f;
    theme->radius.radius_l = 10.0f;
    theme->radius.radius_xl = 12.0f;
    // Elevation
    theme->elevation.elevation_0 = 0.0f;
    theme->elevation.elevation_1 = 2.0f;
    theme->elevation.elevation_2 = 4.0f;
    theme->elevation.elevation_3 = 8.0f;
    pound_theme_set_dark(theme);
    theme->initialized = true;
}

void pound_theme_set_mode(pound_theme_t *POUND_RESTRICT theme, pound_theme_mode_t mode)
{
    if (NULL == theme)
        return;
    theme->mode = mode;
    if (POUND_THEME_LIGHT == mode)
        pound_theme_set_light(theme);
    else
        pound_theme_set_dark(theme);
}

pound_theme_mode_t pound_theme_get_mode(const pound_theme_t *POUND_RESTRICT theme)
{
    if (NULL == theme)
        return POUND_THEME_DARK;
    return theme->mode;
}

void pound_theme_apply_to_imgui(const pound_theme_t *POUND_RESTRICT theme)
{
    if (NULL == theme)
        return;
    ImGuiStyle *style = igGetStyle();
    if (NULL == style)
        return;
    // Style minimal clean
    style->WindowPadding.x = theme->spacing.space_l;
    style->WindowPadding.y = theme->spacing.space_l;
    style->FramePadding.x = theme->spacing.space_m;
    style->FramePadding.y = theme->spacing.space_s;
    style->ItemSpacing.x = theme->spacing.space_m;
    style->ItemSpacing.y = theme->spacing.space_s;
    style->ItemInnerSpacing.x = theme->spacing.space_s;
    style->ItemInnerSpacing.y = theme->spacing.space_s;
    style->ScrollbarSize = 12.0f;
    style->GrabMinSize = 8.0f;
    style->WindowBorderSize = 1.0f;
    style->ChildBorderSize = 1.0f;
    style->PopupBorderSize = 1.0f;
    style->FrameBorderSize = 1.0f;
    style->TabBorderSize = 0.0f;
    style->WindowRounding = theme->radius.radius_m;
    style->ChildRounding = theme->radius.radius_s;
    style->FrameRounding = theme->radius.radius_s;
    style->PopupRounding = theme->radius.radius_s;
    style->ScrollbarRounding = theme->radius.radius_l;
    style->GrabRounding = theme->radius.radius_l;
    style->TabRounding = theme->radius.radius_s;

    // Colors
    ImVec4 *cols = style->Colors;
    // Map minimal set
    cols[ImGuiCol_Text] = (ImVec4){ theme->colors.text_primary[0], theme->colors.text_primary[1], theme->colors.text_primary[2], theme->colors.text_primary[3] };
    cols[ImGuiCol_TextDisabled] = (ImVec4){ theme->colors.text_disabled[0], theme->colors.text_disabled[1], theme->colors.text_disabled[2], theme->colors.text_disabled[3] };
    cols[ImGuiCol_WindowBg] = (ImVec4){ theme->colors.bg_panel[0], theme->colors.bg_panel[1], theme->colors.bg_panel[2], theme->colors.bg_panel[3] };
    cols[ImGuiCol_ChildBg] = (ImVec4){ theme->colors.bg_secondary[0], theme->colors.bg_secondary[1], theme->colors.bg_secondary[2], theme->colors.bg_secondary[3] };
    cols[ImGuiCol_PopupBg] = (ImVec4){ theme->colors.bg_surface[0], theme->colors.bg_surface[1], theme->colors.bg_surface[2], theme->colors.bg_surface[3] };
    cols[ImGuiCol_Border] = (ImVec4){ theme->colors.border[0], theme->colors.border[1], theme->colors.border[2], theme->colors.border[3] };
    cols[ImGuiCol_FrameBg] = (ImVec4){ theme->colors.bg_surface[0], theme->colors.bg_surface[1], theme->colors.bg_surface[2], theme->colors.bg_surface[3] };
    cols[ImGuiCol_FrameBgHovered] = (ImVec4){ theme->colors.accent_faded[0], theme->colors.accent_faded[1], theme->colors.accent_faded[2], theme->colors.accent_faded[3] };
    cols[ImGuiCol_FrameBgActive] = (ImVec4){ theme->colors.accent_primary[0], theme->colors.accent_primary[1], theme->colors.accent_primary[2], theme->colors.accent_primary[3] };
    cols[ImGuiCol_TitleBg] = (ImVec4){ theme->colors.bg_tertiary[0], theme->colors.bg_tertiary[1], theme->colors.bg_tertiary[2], theme->colors.bg_tertiary[3] };
    cols[ImGuiCol_TitleBgActive] = (ImVec4){ theme->colors.bg_tertiary[0], theme->colors.bg_tertiary[1], theme->colors.bg_tertiary[2], theme->colors.bg_tertiary[3] };
    cols[ImGuiCol_Button] = (ImVec4){ theme->colors.accent_primary[0], theme->colors.accent_primary[1], theme->colors.accent_primary[2], theme->colors.accent_primary[3] };
    cols[ImGuiCol_ButtonHovered] = (ImVec4){ theme->colors.accent_hover[0], theme->colors.accent_hover[1], theme->colors.accent_hover[2], theme->colors.accent_hover[3] };
    cols[ImGuiCol_ButtonActive] = (ImVec4){ theme->colors.accent_active[0], theme->colors.accent_active[1], theme->colors.accent_active[2], theme->colors.accent_active[3] };
    cols[ImGuiCol_CheckMark] = (ImVec4){ theme->colors.text_primary[0], theme->colors.text_primary[1], theme->colors.text_primary[2], theme->colors.text_primary[3] };
    cols[ImGuiCol_SliderGrab] = (ImVec4){ theme->colors.accent_primary[0], theme->colors.accent_primary[1], theme->colors.accent_primary[2], theme->colors.accent_primary[3] };
    cols[ImGuiCol_Header] = (ImVec4){ theme->colors.accent_faded[0], theme->colors.accent_faded[1], theme->colors.accent_faded[2], theme->colors.accent_faded[3] };
    cols[ImGuiCol_HeaderHovered] = (ImVec4){ theme->colors.accent_primary[0]*0.8f, theme->colors.accent_primary[1]*0.8f, theme->colors.accent_primary[2]*0.8f, theme->colors.accent_primary[3]*0.8f };
    cols[ImGuiCol_HeaderActive] = (ImVec4){ theme->colors.accent_primary[0], theme->colors.accent_primary[1], theme->colors.accent_primary[2], theme->colors.accent_primary[3] };
    cols[ImGuiCol_Tab] = (ImVec4){ theme->colors.bg_tertiary[0], theme->colors.bg_tertiary[1], theme->colors.bg_tertiary[2], theme->colors.bg_tertiary[3] };
    cols[ImGuiCol_TabActive] = (ImVec4){ theme->colors.accent_primary[0], theme->colors.accent_primary[1], theme->colors.accent_primary[2], theme->colors.accent_primary[3] };
    cols[ImGuiCol_TabHovered] = (ImVec4){ theme->colors.accent_hover[0], theme->colors.accent_hover[1], theme->colors.accent_hover[2], theme->colors.accent_hover[3] };
    cols[ImGuiCol_TabUnfocused] = (ImVec4){ theme->colors.bg_tertiary[0]*0.9f, theme->colors.bg_tertiary[1]*0.9f, theme->colors.bg_tertiary[2]*0.9f, theme->colors.bg_tertiary[3] };
    cols[ImGuiCol_TabUnfocusedActive] = (ImVec4){ theme->colors.accent_faded[0], theme->colors.accent_faded[1], theme->colors.accent_faded[2], theme->colors.accent_faded[3] };
    cols[ImGuiCol_TabUnfocused] = (ImVec4){ theme->colors.bg_tertiary[0]*0.9f, theme->colors.bg_tertiary[1]*0.9f, theme->colors.bg_tertiary[2]*0.9f, theme->colors.bg_tertiary[3] };
}

#ifndef POUND_GUI_THEME_H
#define POUND_GUI_THEME_H

//! Theme - Modern, clean, minimalistic UI theming.
//! Single source of truth for colours, spacing, radius and elevation.

#include "attributes.h"
#include <stdbool.h>

typedef enum
{
    POUND_THEME_DARK = 0,
    POUND_THEME_LIGHT = 1
} pound_theme_mode_t;

typedef struct
{
    // Core background/foreground
    float bg_primary[4];
    float bg_secondary[4];
    float bg_tertiary[4];
    float bg_panel[4];
    float bg_surface[4];
    float border[4];
    float text_primary[4];
    float text_secondary[4];
    float text_disabled[4];
    // Accent
    float accent_primary[4];
    float accent_hover[4];
    float accent_active[4];
    float accent_faded[4];
    // Status
    float success[4];
    float warning[4];
    float error[4];
    float info[4];
    // Elevation
    float shadow[4];
} pound_theme_colors_t;

typedef struct
{
    float radius_xs;
    float radius_s;
    float radius_m;
    float radius_l;
    float radius_xl;
} pound_theme_radius_t;

typedef struct
{
    float space_xs;
    float space_s;
    float space_m;
    float space_l;
    float space_xl;
    float space_2xl;
} pound_theme_spacing_t;

typedef struct
{
    float elevation_0;
    float elevation_1;
    float elevation_2;
    float elevation_3;
} pound_theme_elevation_t;

typedef struct
{
    pound_theme_mode_t      mode;
    pound_theme_colors_t    colors;
    pound_theme_radius_t    radius;
    pound_theme_spacing_t   spacing;
    pound_theme_elevation_t elevation;
    bool                    initialized;
    char                    pad[7];
} pound_theme_t;

void pound_theme_init(pound_theme_t *POUND_RESTRICT theme);
void pound_theme_set_mode(pound_theme_t *POUND_RESTRICT theme, pound_theme_mode_t mode);
void pound_theme_apply_to_imgui(const pound_theme_t *POUND_RESTRICT theme);
pound_theme_mode_t pound_theme_get_mode(const pound_theme_t *POUND_RESTRICT theme);

#endif // POUND_GUI_THEME_H

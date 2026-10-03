#ifndef POUND_GUI_THEME_H
#define POUND_GUI_THEME_H

/// Theme -- the single source of truth for every colour, gap, radius and shadow
/// Pound draws.
///
/// Two properties this file exists to enforce:
///
///  * No panel names a colour. A page that wants "the muted text colour" asks for
///    `POUND_THEME_TEXT_SECONDARY` and gets whatever the active mode says that is.
///    Switching to the light theme is therefore a change to this file alone, and a
///    page cannot drift away from the rest of the interface by hardcoding a hex
///    value in a place somebody will forget to revisit.
///
///  * No magic numbers. Spacing and radii come from the scales below rather than
///    from literals sprinkled through the render code, so "make the interface a
///    little airier" is a one-line change that moves every panel at once.
///
/// This half of the theme is deliberately free of ImGui: it is plain data with
/// plain setters, which is what lets the test suite exercise it headlessly. The
/// half that pushes the values into ImGui lives in `theme_imgui.h`.

#include "attributes.h"
#include "errors.h"
#include <stdbool.h>

/// Which of the two ramps is active.
///
/// Values are part of the saved GUI state, so they must not be renumbered: an
/// existing save naming `POUND_THEME_LIGHT = 1` has to keep meaning light.
typedef enum
{
    POUND_THEME_DARK = 0,
    POUND_THEME_LIGHT = 1
} pound_theme_mode_t;

/// How many members a colour has, so the arrays below are self-describing.
#define POUND_THEME_COLOR_CHANNELS 4

/// A colour as four normalised channels.
///
/// A plain array rather than an ImGui vector so this header does not have to pull
/// ImGui into every translation unit that wants to ask about a colour. Convert at
/// the point of use; `theme_imgui.h` is where that conversion lives.
typedef float pound_theme_color_t[POUND_THEME_COLOR_CHANNELS];

/// Surfaces, from furthest back to nearest front.
///
/// The ordering is what makes the interface read as layered rather than as a
/// collection of boxes: each step is a small, even lift in lightness, so depth
/// comes from value contrast alone and no drop shadows or gradients are needed.
typedef struct
{
    pound_theme_color_t background;      ///< The window behind everything.
    pound_theme_color_t surface;         ///< Cards, the sidebar, the title bar.
    pound_theme_color_t surface_raised;  ///< Controls that sit on a surface.
    pound_theme_color_t surface_hover;   ///< A control under the cursor.
    pound_theme_color_t border;          ///< Dividers and control outlines.
    pound_theme_color_t border_strong;   ///< The outline of the focused control.
} pound_theme_surfaces_t;

/// Text, from loudest to quietest.
typedef struct
{
    pound_theme_color_t primary;
    pound_theme_color_t secondary;
    pound_theme_color_t disabled;
    pound_theme_color_t on_accent;
} pound_theme_text_t;

/// The single accent colour and the states derived from it.
typedef struct
{
    pound_theme_color_t base;
    pound_theme_color_t hover;
    pound_theme_color_t active;
    pound_theme_color_t muted; ///< A wash of the accent, for selected-row fills.
} pound_theme_accent_t;

/// Colours that carry meaning rather than identity.
typedef struct
{
    pound_theme_color_t success;
    pound_theme_color_t warning;
    pound_theme_color_t error;
    pound_theme_color_t info;
} pound_theme_status_t;

typedef struct
{
    pound_theme_surfaces_t surfaces;
    pound_theme_text_t     text;
    pound_theme_accent_t   accent;
    pound_theme_status_t   status;
} pound_theme_colors_t;

/// The spacing scale. Every gap in the interface is one of these.
typedef struct
{
    float xxs;  ///< 2  -- inside a control, between an icon and its label.
    float xs;   ///< 4  -- between related controls.
    float sm;   ///< 8  -- the default between controls.
    float md;   ///< 12 -- inside a panel.
    float lg;   ///< 16 -- between panels.
    float xl;   ///< 24 -- between page sections.
    float xxl;  ///< 40 -- page top padding.
} pound_theme_spacing_t;

/// Corner radii, named by size rather than by role.
typedef struct
{
    float none;
    float sm;
    float md;
    float lg;
    float pill; ///< Half the control height: a fully rounded end.
} pound_theme_radius_t;

/// Shadow and border strengths, for the few places that need a lift.
typedef struct
{
    float none;
    float hairline; ///< A 1px separator.
    float subtle;   ///< A resting border.
    float strong;   ///< The focused or selected border.
} pound_theme_stroke_t;

typedef struct
{
    pound_theme_mode_t    mode;
    pound_theme_colors_t  colors;
    pound_theme_spacing_t spacing;
    pound_theme_radius_t  radius;
    pound_theme_stroke_t  stroke;
    bool                  initialized;
    char                  pad[7];
} pound_theme_t;

/// Fills `theme` with the dark ramp and every scale, and marks it initialised.
///
/// Idempotent, so it can also be used to repair a theme a caller has scribbled
/// over -- the GUI re-runs it whenever it sees `initialized` is false.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` and logs if `theme` is NULL. That is a
/// programming error rather than a runtime condition, so it is reported rather
/// than absorbed: a caller that cannot store its theme has nowhere to put a
/// failure and would otherwise keep drawing with an uninitialised one.
error_t pound_theme_init(pound_theme_t *POUND_RESTRICT theme);

/// Loads the dark or light ramp into `theme`, leaving the scales alone.
///
/// The scales are mode-independent by design: a light theme with tighter spacing
/// would not be the same interface in a different colour, it would be a different
/// interface. An unrecognised `mode` is a bug in the caller and is refused.
error_t pound_theme_set_mode(pound_theme_t *POUND_RESTRICT theme, pound_theme_mode_t mode);

/// The mode `theme` currently holds, or `POUND_THEME_DARK` for a NULL theme.
///
/// Total on purpose -- this is read while building a menu, where a failure branch
/// has nowhere sensible to go.
pound_theme_mode_t pound_theme_get_mode(const pound_theme_t *POUND_RESTRICT theme);

/// The display name of a mode, for the appearance selector and for saves.
///
/// Never NULL; an unrecognised value renders as "Unknown".
const char *pound_theme_mode_to_string(pound_theme_mode_t mode);

/// Parses a mode name as written by `pound_theme_mode_to_string` or by a save.
///
/// Case-insensitive and tolerant of surrounding spaces, because the string comes
/// from a file a person edits. Returns false and logs for anything else rather
/// than falling back to a default: silently showing the dark theme because a
/// setting was misspelled is how a user concludes the setting does not work.
bool pound_theme_mode_from_string(const char *POUND_RESTRICT text,
                                  pound_theme_mode_t *POUND_RESTRICT out_mode);

/// True when `theme` carries a complete, self-consistent set of tokens.
///
/// Every channel in `[0, 1]`, every scale finite and non-negative. Used at the
/// point the theme reaches the renderer so a partially initialised theme shows up
/// as a refusal with a log record instead of as a screen full of black rectangles.
bool pound_theme_is_valid(const pound_theme_t *POUND_RESTRICT theme);

/// The palette, addressed by role.
///
/// A named index rather than a pointer to a member, so a page can say
/// `POUND_THEME_TEXT_SECONDARY` and cannot accidentally reach a role that does not
/// exist: the enum's last member is also the count, and `pound_theme_color_role`
/// refuses anything outside it.
///
/// The order matches `POUND_THEME_ROLE_TABLE` in `theme.c` one for one, and the
/// static assertion there fails the build if the two ever drift apart.
typedef enum
{
    POUND_THEME_SURFACE_BACKGROUND = 0,
    POUND_THEME_SURFACE_RAISED,
    POUND_THEME_SURFACE_HOVER,
    POUND_THEME_SURFACE,
    POUND_THEME_BORDER,
    POUND_THEME_BORDER_STRONG,
    POUND_THEME_TEXT_PRIMARY,
    POUND_THEME_TEXT_SECONDARY,
    POUND_THEME_TEXT_DISABLED,
    POUND_THEME_TEXT_ON_ACCENT,
    POUND_THEME_ACCENT,
    POUND_THEME_ACCENT_HOVER,
    POUND_THEME_ACCENT_ACTIVE,
    POUND_THEME_ACCENT_MUTED,
    POUND_THEME_STATUS_SUCCESS,
    POUND_THEME_STATUS_WARNING,
    POUND_THEME_STATUS_ERROR,
    POUND_THEME_STATUS_INFO,
    POUND_THEME_ROLE_COUNT
} pound_theme_role_t;

/// How many named colour roles the palette exposes, and the count the role table
/// is checked against.
#define POUND_THEME_COLOR_ROLE_COUNT ((int)POUND_THEME_ROLE_COUNT)

/// The pointer to role `index` inside `colors`, or NULL if `index` is out of
/// range.
///
/// Exposed as an accessor rather than a public table so the mapping from role to
/// member stays inside this module, where adding a role means touching one place.
const pound_theme_color_t *pound_theme_color_role(const pound_theme_colors_t *POUND_RESTRICT colors,
                                                  int                            role_index);

/// The name of role `index`, for logs. Never NULL.
const char *pound_theme_color_role_name(int role_index);

/// Logs the whole active ramp at INFO, one line per role.
///
/// Written as a loop over the role table rather than as thirty log calls because
/// a role that is added to the palette and forgotten here is a colour nobody can
/// debug from a log.
void pound_theme_log_summary(const pound_theme_t *POUND_RESTRICT theme);

#endif // POUND_GUI_THEME_H

/*** end of file ***/
#ifndef POUND_GUI_THEME_FONTS_H
#define POUND_GUI_THEME_FONTS_H

/// Font management.
///
/// Pound had none of this before: the SDL3 backend's built-in loader left the
/// atlas holding ImGui's embedded Proggy at a fixed 13px, which is legible on a
/// 1080p desktop monitor and unreadable on a 4K one or a phone. This is the
/// smallest thing that makes type readable everywhere without shipping a
/// typeface:
///
///  * One font is loaded, from the system when a known face is present and from
///    ImGui's embedded one when it is not. ImGui 1.92 bakes sizes on demand, so
///    the four interface sizes are four `PushFont` calls against that one font
///    rather than four rasterised copies held in the atlas.
///
///  * The display scale is applied once, through `style.FontScaleMain`, rather
///    than baked into the point sizes. That is what makes the same build correct
///    on a phone and on a monitor: the backend reports a density, the scale lands
///    here, and every size moves together.
///
///  * The scale is clamped to a range that stays legible at both ends. Below the
///    floor the rasteriser loses hinting and glyph edges smear; above the ceiling
///    a sidebar row no longer fits its own label.
///
/// A missing system font is a logged downgrade, not a failed start-up. Refusing
/// to open a window over a typeface is the wrong trade for an emulator.
///
/// This half needs ImGui, and lives beside `theme_imgui.c` for the same reason
/// the palette does not: the palette is testable without a renderer and an atlas
/// is not.

#include "attributes.h"
#include "errors.h"
#include <stdbool.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

/// Base type size in points, before the display scale.
///
/// 18 rather than ImGui's 13: at 18 the sidebar labels, page titles and log lines
/// all sit comfortably inside their rows, and `FontScaleMain` moves the whole
/// interface from there for higher-density panels.
#define POUND_FONT_BASE_SIZE 18.0F

/// Bounds on the applied scale.
#define POUND_FONT_MIN_SCALE 0.85F
#define POUND_FONT_MAX_SCALE 2.50F

/// Which of the interface's type sizes a caller wants.
typedef enum
{
    POUND_FONT_ROLE_BODY = 0, ///< Everything default: labels, list rows, buttons.
    POUND_FONT_ROLE_TITLE,    ///< Page titles and the wordmark.
    POUND_FONT_ROLE_MONO,     ///< Addresses, sizes, hashes, log lines.
    POUND_FONT_ROLE_SMALL,    ///< Secondary text: captions, units, timestamps.
    POUND_FONT_ROLE_COUNT
} pound_font_role_t;

/// The atlas handle, the font on it, and the scale it was built at.
typedef struct
{
    /// The one font every role draws with. NULL until `pound_fonts_setup` runs.
    ImFont *font;

    /// The scale actually applied, after clamping.
    float scale;

    /// Whether `pound_fonts_setup` succeeded.
    bool setup;

    /// True when no system face was found and the embedded font is in use.
    bool using_embedded_font;

    char pad[2];
} pound_fonts_t;

/// The multiple of the base size a role is drawn at.
///
/// Exposed so a layout that has to reserve a row for a role -- the sidebar, the
/// title bar -- agrees with what `pound_fonts_push` will actually do, instead of
/// hardcoding a height that is correct at exactly one scale.
float pound_font_role_scale(pound_font_role_t role);

/// True when `role` is a member of `pound_font_role_t`.
bool pound_font_role_is_valid(int role);

/// Builds the atlas for a display scale.
///
/// Clamps `scale` into `[POUND_FONT_MIN_SCALE, POUND_FONT_MAX_SCALE]`, records
/// what it used in `out_fonts->scale`, and sets `style.FontScaleMain` so every
/// size the interface pushes is scaled consistently.
///
/// Safe to call again after a scale change: the atlas is cleared first, so moving
/// a window between monitors of different density rebuilds rather than
/// accumulates.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` and logs for a NULL or non-positive
/// scale, and `POUND_ERROR_NOT_INITIALIZED` if there is no ImGui context.
error_t pound_fonts_setup(pound_fonts_t *POUND_RESTRICT out_fonts, float scale);

/// Applies a new display scale without touching the atlas.
///
/// `pound_fonts_setup` rebuilds the atlas, which cannot happen once a frame is
/// being built: clearing it leaves ImGui holding font pointers into storage that
/// is about to be freed. Scaling does not need a rebuild at all -- ImGui 1.92
/// rasterises each size on demand, and `style.FontScaleMain` moves every pushed
/// size together -- so a scale change is a clamped float written to the style.
/// This is that, and it is what the Settings page calls when the slider moves.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` for a NULL, non-finite or non-positive
/// scale. Returns `POUND_SUCCESS` even when the atlas was never built; the style
/// is set either way, and the next `pound_fonts_setup` will pick the value up.
error_t pound_fonts_set_scale(pound_fonts_t *POUND_RESTRICT fonts, float scale);

/// Pushes one of the interface's type sizes, pairing with `igPopFont`.
///
/// Returns false and logs when the fonts were never set up or `role` is not a
/// role. Returning false rather than pushing a default matters: `igPushFont(NULL,
/// 0)` silently pushes ImGui's current font, so a mistyped role would produce an
/// invisible layout bug instead of a reported one.
bool pound_fonts_push(const pound_fonts_t *POUND_RESTRICT fonts, pound_font_role_t role);

/// Pops a font, but only when `pushed` is true.
///
/// A convenience for the common shape -- capture the result of
/// `pound_fonts_push`, draw, pop -- so a failure path cannot accidentally
/// unbalance the font stack with a bare `igPopFont`.
void pound_fonts_pop(bool pushed);

/// Logs what the atlas ended up holding, for a bug report or a sanity check.
void pound_fonts_log_summary(const pound_fonts_t *POUND_RESTRICT fonts);

#endif // POUND_GUI_THEME_FONTS_H

/*** end of file ***/

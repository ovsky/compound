#ifndef POUND_GUI_THEME_IMGUI_H
#define POUND_GUI_THEME_IMGUI_H

/// The ImGui half of the theme.
///
/// Separated from `theme.h` so the palette itself stays free of ImGui: the test
/// suite links the tokens and validates the ramps without a window or a renderer,
/// which is the only way a colour regression gets caught in CI rather than by
/// somebody looking at a screenshot.
///
/// Everything here reads from a `pound_theme_t` and writes into ImGui's global
/// style. There are no theme values on this side at all -- if a colour appears
/// here as a literal, that is a bug, because it is a colour the light theme will
/// not reach.

#include "attributes.h"
#include "errors.h"
#include "theme.h"

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

/// Pushes the theme's colours, radii and metrics into ImGui's global style.
///
/// Call once after a context exists and again whenever the mode changes. Safe to
/// call every frame: it writes the same values, and ImGui's own `igStyleColorsDark`
/// is never called afterwards to undo them.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` (and logs) for a NULL theme or a theme
/// that fails `pound_theme_is_valid`. The style is left untouched in that case:
/// half-applying a palette is what produces a screen with unreadable text, which
/// is worse than refusing and keeping the last known-good look.
error_t pound_theme_imgui_apply(const pound_theme_t *POUND_RESTRICT theme);

/// A theme colour as an ImGui vector, for the few places ImGui wants a value
/// rather than an index (`igTextColored`, draw-list calls).
///
/// Returns a fully transparent black for a NULL theme or an out-of-range role, so
/// a caller that ignores the status draws something invisible rather than reading
/// through a pointer that is not there.
ImVec4 pound_theme_imgui_color(const pound_theme_t *POUND_RESTRICT theme, int role_index);

#endif // POUND_GUI_THEME_IMGUI_H

/*** end of file ***/
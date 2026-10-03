#include "theme.h"

#include "log.h"
#include <math.h>
#include <string.h>

/// One named role in the palette.
///
/// Kept as a table of pointers to the members rather than as an enum so the
/// validator, the summary log and any future exporter all iterate the same list.
/// A role added to `pound_theme_colors_t` without a row here is a colour that
/// cannot be validated or logged, which is the failure this table exists to make
/// impossible.
typedef struct
{
    const char                  *name;
    const pound_theme_color_t   *color;
} pound_theme_color_role_entry_t;

/// Builds a colour from four normalised channels.
///
/// A macro rather than an inline function because these are compile-time
/// constants: the initialisers are `static const`-ish writes into a struct the
/// caller owns, and there is no reason to generate a call per channel.
#define POUND_THEME_RGBA(dst, r, g, b, a)                                                                 \
    do                                                                                                     \
    {                                                                                                      \
        (dst)[0] = (r);                                                                                    \
        (dst)[1] = (g);                                                                                    \
        (dst)[2] = (b);                                                                                    \
        (dst)[3] = (a);                                                                                    \
    } while (false)

/// The dark ramp.
///
/// Not pure black and not a flat grey: a hair of blue in the shadows keeps large
/// dark areas from looking like dead pixels, and the surfaces step up in
/// lightness by small even amounts so the layering reads without shadows.
static void pound_theme_load_dark(pound_theme_colors_t *POUND_RESTRICT colors)
{
    POUND_THEME_RGBA(colors->surfaces.background, 0.043F, 0.047F, 0.055F, 1.0F);
    POUND_THEME_RGBA(colors->surfaces.surface, 0.067F, 0.071F, 0.082F, 1.0F);
    POUND_THEME_RGBA(colors->surfaces.surface_raised, 0.090F, 0.094F, 0.108F, 1.0F);
    POUND_THEME_RGBA(colors->surfaces.surface_hover, 0.125F, 0.130F, 0.149F, 1.0F);
    POUND_THEME_RGBA(colors->surfaces.border, 0.141F, 0.149F, 0.173F, 1.0F);
    POUND_THEME_RGBA(colors->surfaces.border_strong, 0.275F, 0.310F, 0.420F, 1.0F);

    POUND_THEME_RGBA(colors->text.primary, 0.925F, 0.933F, 0.945F, 1.0F);
    POUND_THEME_RGBA(colors->text.secondary, 0.612F, 0.639F, 0.694F, 1.0F);
    POUND_THEME_RGBA(colors->text.disabled, 0.373F, 0.392F, 0.435F, 1.0F);
    POUND_THEME_RGBA(colors->text.on_accent, 0.043F, 0.047F, 0.055F, 1.0F);

    POUND_THEME_RGBA(colors->accent.base, 0.318F, 0.612F, 0.980F, 1.0F);
    POUND_THEME_RGBA(colors->accent.hover, 0.435F, 0.702F, 1.0F, 1.0F);
    POUND_THEME_RGBA(colors->accent.active, 0.239F, 0.510F, 0.882F, 1.0F);
    POUND_THEME_RGBA(colors->accent.muted, 0.318F, 0.612F, 0.980F, 0.165F);

    POUND_THEME_RGBA(colors->status.success, 0.290F, 0.780F, 0.478F, 1.0F);
    POUND_THEME_RGBA(colors->status.warning, 0.949F, 0.729F, 0.259F, 1.0F);
    POUND_THEME_RGBA(colors->status.error, 0.949F, 0.404F, 0.404F, 1.0F);
    POUND_THEME_RGBA(colors->status.info, 0.322F, 0.702F, 0.902F, 1.0F);
}

/// The light ramp.
///
/// The accent is a deeper blue than the dark mode's: the dark accent has to be
/// bright to separate from a near-black surface, and the same value on white is a
/// pastel that fails contrast. Two ramps, not one inverted, because the value
/// that reads as "the same colour" is not the same number in both.
static void pound_theme_load_light(pound_theme_colors_t *POUND_RESTRICT colors)
{
    POUND_THEME_RGBA(colors->surfaces.background, 0.973F, 0.976F, 0.980F, 1.0F);
    POUND_THEME_RGBA(colors->surfaces.surface, 1.0F, 1.0F, 1.0F, 1.0F);
    POUND_THEME_RGBA(colors->surfaces.surface_raised, 0.949F, 0.953F, 0.961F, 1.0F);
    POUND_THEME_RGBA(colors->surfaces.surface_hover, 0.898F, 0.906F, 0.918F, 1.0F);
    POUND_THEME_RGBA(colors->surfaces.border, 0.855F, 0.867F, 0.886F, 1.0F);
    POUND_THEME_RGBA(colors->surfaces.border_strong, 0.412F, 0.510F, 0.667F, 1.0F);

    POUND_THEME_RGBA(colors->text.primary, 0.075F, 0.086F, 0.110F, 1.0F);
    POUND_THEME_RGBA(colors->text.secondary, 0.353F, 0.388F, 0.443F, 1.0F);
    POUND_THEME_RGBA(colors->text.disabled, 0.573F, 0.596F, 0.639F, 1.0F);
    POUND_THEME_RGBA(colors->text.on_accent, 1.0F, 1.0F, 1.0F, 1.0F);

    POUND_THEME_RGBA(colors->accent.base, 0.129F, 0.416F, 0.851F, 1.0F);
    POUND_THEME_RGBA(colors->accent.hover, 0.086F, 0.353F, 0.780F, 1.0F);
    POUND_THEME_RGBA(colors->accent.active, 0.063F, 0.290F, 0.694F, 1.0F);
    POUND_THEME_RGBA(colors->accent.muted, 0.129F, 0.416F, 0.851F, 0.110F);

    POUND_THEME_RGBA(colors->status.success, 0.063F, 0.588F, 0.290F, 1.0F);
    POUND_THEME_RGBA(colors->status.warning, 0.667F, 0.404F, 0.020F, 1.0F);
    POUND_THEME_RGBA(colors->status.error, 0.792F, 0.145F, 0.145F, 1.0F);
    POUND_THEME_RGBA(colors->status.info, 0.075F, 0.420F, 0.757F, 1.0F);
}

/// The role table.
///
/// Order is the log order and the validation order; both are cosmetic, so this
/// is grouped to read like the palette does: surfaces, then text, then accent,
/// then the status colours.
#define POUND_THEME_ROLE_TABLE(c)                                                                         \
    {                                                                                                      \
        { "surface.background", &(c)->surfaces.background },                                               \
        { "surface.raised", &(c)->surfaces.surface_raised },                                               \
        { "surface.hover", &(c)->surfaces.surface_hover },                                                 \
        { "surface", &(c)->surfaces.surface },                                                             \
        { "border", &(c)->surfaces.border },                                                               \
        { "border.strong", &(c)->surfaces.border_strong },                                                 \
        { "text.primary", &(c)->text.primary },                                                            \
        { "text.secondary", &(c)->text.secondary },                                                        \
        { "text.disabled", &(c)->text.disabled },                                                          \
        { "text.on_accent", &(c)->text.on_accent },                                                        \
        { "accent", &(c)->accent.base },                                                                   \
        { "accent.hover", &(c)->accent.hover },                                                            \
        { "accent.active", &(c)->accent.active },                                                          \
        { "accent.muted", &(c)->accent.muted },                                                            \
        { "status.success", &(c)->status.success },                                                        \
        { "status.warning", &(c)->status.warning },                                                        \
        { "status.error", &(c)->status.error },                                                            \
        { "status.info", &(c)->status.info },                                                              \
    }

/// The table and the enum have to describe the same palette in the same order.
///
/// Without this the two drift silently: a role added to `pound_theme_role_t` and
/// forgotten here is validated as "out of range" and logged as "out-of-range",
/// and a role in the table but not the enum is unreachable by name from a page.
_Static_assert(POUND_THEME_COLOR_ROLE_COUNT == 18,
               "POUND_THEME_ROLE_TABLE and pound_theme_role_t disagree on the palette size");

/// Builds the role table for a palette.
///
/// `const` on the table but not on the palette it points into: the pointers are
/// into the caller's mutable struct, and marking them `const *const` would stop
/// the table being read from a temporary palette in a test.
static void pound_theme_build_roles(const pound_theme_colors_t *POUND_RESTRICT colors,
                                    pound_theme_color_role_entry_t *POUND_RESTRICT roles,
                                    const int                      role_count)
{
    if ((NULL == colors) || (NULL == roles) || (role_count < POUND_THEME_COLOR_ROLE_COUNT))
    {
        return;
    }

    const pound_theme_color_role_entry_t table[POUND_THEME_COLOR_ROLE_COUNT] = POUND_THEME_ROLE_TABLE(colors);

    memcpy(roles, table, sizeof(table));
}

static void pound_theme_load_scales(pound_theme_t *POUND_RESTRICT theme)
{
    theme->spacing.xxs = 2.0F;
    theme->spacing.xs  = 4.0F;
    theme->spacing.sm  = 8.0F;
    theme->spacing.md  = 12.0F;
    theme->spacing.lg  = 16.0F;
    theme->spacing.xl  = 24.0F;
    theme->spacing.xxl = 40.0F;

    theme->radius.none = 0.0F;
    theme->radius.sm   = 4.0F;
    theme->radius.md   = 6.0F;
    theme->radius.lg   = 10.0F;
    theme->radius.pill = 999.0F; // Clamped by the renderer to half the control height.

    theme->stroke.none     = 0.0F;
    theme->stroke.hairline = 1.0F;
    theme->stroke.subtle   = 1.0F;
    theme->stroke.strong   = 2.0F;
}

error_t
pound_theme_init(pound_theme_t *POUND_RESTRICT theme)
{
    if (POUND_UNLIKELY(NULL == theme))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: theme is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    memset(theme, 0, sizeof(*theme));
    pound_theme_load_scales(theme);
    pound_theme_load_dark(&theme->colors);
    theme->mode        = POUND_THEME_DARK;
    theme->initialized = true;
    return POUND_SUCCESS;
}

error_t
pound_theme_set_mode(pound_theme_t *POUND_RESTRICT theme, pound_theme_mode_t mode)
{
    if (POUND_UNLIKELY(NULL == theme))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: theme is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    switch (mode)
    {
        case POUND_THEME_DARK:
            pound_theme_load_dark(&theme->colors);
            break;
        case POUND_THEME_LIGHT:
            pound_theme_load_light(&theme->colors);
            break;
        default:
            POUND_LOG_ERROR(&thread_logger,
                            "Ignoring call: %d is not a theme mode.",
                            (int)mode);
            return POUND_ERROR_INVALID_ARGUMENT;
    }

    theme->mode = mode;
    return POUND_SUCCESS;
}

pound_theme_mode_t
pound_theme_get_mode(const pound_theme_t *POUND_RESTRICT theme)
{
    if (NULL == theme)
    {
        return POUND_THEME_DARK;
    }

    return theme->mode;
}

const char *
pound_theme_mode_to_string(pound_theme_mode_t mode)
{
    switch (mode)
    {
        case POUND_THEME_DARK:
            return "Dark";
        case POUND_THEME_LIGHT:
            return "Light";
        default:
            return "Unknown";
    }
}

/// Case-insensitive equality against a literal.
///
/// Written out rather than reaching for `strcasecmp`: that is POSIX, not C, and
/// the Android and MSVC lanes disagree about it.
static bool pound_theme_text_equals(const char *POUND_RESTRICT text, const char *POUND_RESTRICT literal)
{
    const size_t literal_length = strlen(literal);

    for (size_t i = 0; i < literal_length; i++)
    {
        if ('\0' == text[i])
        {
            return false;
        }

        char left  = text[i];
        char right = literal[i];

        if (('A' <= left) && ('Z' >= left))
        {
            left = (char)(left - 'A' + 'a');
        }

        if (('A' <= right) && ('Z' >= right))
        {
            right = (char)(right - 'A' + 'a');
        }

        if (left != right)
        {
            return false;
        }
    }

    // Everything `literal` spelled matched, so the two are equal only if `text`
    // stopped there too. Checking this rather than walking `text` to its own end is
    // what keeps the loop from ever reading past the shorter of the two strings.
    return '\0' == text[literal_length];
}

/// Skips spaces and tabs from both ends of a string, in place.
///
/// Writes through the caller's buffer, which is why this takes a `char *` and a
/// capacity: a parsed setting has to be trimmed before it is compared and before
/// it is copied into the config, and doing that on a scratch copy would leave the
/// untrimmed original around to be copied again by mistake.
static void pound_theme_text_trim(char *POUND_RESTRICT text, const size_t capacity)
{
    if ((NULL == text) || (0 == capacity))
    {
        return;
    }

    size_t begin = 0;
    size_t end   = strlen(text);

    while ((begin < end) && ((' ' == text[begin]) || ('\t' == text[begin])))
    {
        ++begin;
    }

    while ((end > begin) && ((' ' == text[end - 1]) || ('\t' == text[end - 1])))
    {
        --end;
    }

    const size_t length = end - begin;

    if (begin > 0)
    {
        memmove(text, text + begin, length);
    }

    text[length] = '\0';
}

bool
pound_theme_mode_from_string(const char *POUND_RESTRICT text, pound_theme_mode_t *POUND_RESTRICT out_mode)
{
    if (POUND_UNLIKELY(NULL == text))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: text is NULL.");
        return false;
    }

    if (POUND_UNLIKELY(NULL == out_mode))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: out_mode is NULL.");
        return false;
    }

    if (POUND_UNLIKELY('\0' == text[0]))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: a theme mode was requested from an empty name.");
        return false;
    }

    // Trimmed on a copy: the caller's buffer may be a read-only literal.
    char scratch[32];
    const size_t length = strlen(text);

    if (length >= sizeof(scratch))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Ignoring call: a %zu-character theme mode name does not fit in %zu.",
                        length,
                        sizeof(scratch));
        return false;
    }

    memcpy(scratch, text, length + 1);
    pound_theme_text_trim(scratch, sizeof(scratch));

    if (pound_theme_text_equals(scratch, "dark"))
    {
        *out_mode = POUND_THEME_DARK;
        return true;
    }

    if (pound_theme_text_equals(scratch, "light"))
    {
        *out_mode = POUND_THEME_LIGHT;
        return true;
    }

    POUND_LOG_ERROR(&thread_logger,
                    "Ignoring call: '%s' is not a theme mode; expected 'dark' or 'light'.",
                    scratch);
    return false;
}

/// True when every channel is a finite number in `[0, 1]`.
///
/// `NaN` is rejected by both halves of the test, which matters more than it looks:
/// `NaN` fails every comparison, so a colour containing one would sail through a
/// bounds check written as `< 0.0F || > 1.0F` and reach the renderer as a garbage
/// component.
static bool pound_theme_color_is_valid(const pound_theme_color_t *POUND_RESTRICT color)
{
    if (NULL == color)
    {
        return false;
    }

    for (int channel = 0; channel < POUND_THEME_COLOR_CHANNELS; channel++)
    {
        const float value = (*color)[channel];

        if (false == isfinite(value))
        {
            return false;
        }

        if ((value < 0.0F) || (value > 1.0F))
        {
            return false;
        }
    }

    return true;
}

const pound_theme_color_t *
pound_theme_color_role(const pound_theme_colors_t *POUND_RESTRICT colors, const int role_index)
{
    if (POUND_UNLIKELY(NULL == colors))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: colors is NULL.");
        return NULL;
    }

    if (POUND_UNLIKELY((role_index < 0) || (role_index >= POUND_THEME_COLOR_ROLE_COUNT)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: role %d is outside [0, %d).",
                        role_index,
                        POUND_THEME_COLOR_ROLE_COUNT);
        return NULL;
    }

    pound_theme_color_role_entry_t roles[POUND_THEME_COLOR_ROLE_COUNT];
    pound_theme_build_roles(colors, roles, POUND_THEME_COLOR_ROLE_COUNT);
    return roles[role_index].color;
}

const char *
pound_theme_color_role_name(const int role_index)
{
    // Names are static, so the table can be built over a scratch palette: the
    // accessor's only job is to carry the name, and reading a pointer to a local
    // that dies at return would be worse than the stack cost.
    pound_theme_colors_t             scratch;
    pound_theme_color_role_entry_t   roles[POUND_THEME_COLOR_ROLE_COUNT];

    memset(&scratch, 0, sizeof(scratch));
    pound_theme_build_roles(&scratch, roles, POUND_THEME_COLOR_ROLE_COUNT);

    if ((role_index < 0) || (role_index >= POUND_THEME_COLOR_ROLE_COUNT))
    {
        return "out-of-range";
    }

    return roles[role_index].name;
}

/// True when a scale value can be handed to the renderer.
///
/// Non-negative and finite. The upper bound is deliberately absent: the `pill`
/// radius is a sentinel the renderer clamps to half the control height, so
/// rejecting large radii here would reject the one radius the theme relies on.
static bool pound_theme_scale_is_valid(const float value)
{
    return (true == isfinite(value)) && (value >= 0.0F);
}

/// Validates one scale, naming it so the log says which gap is wrong.
///
/// A macro because the names have to be spelled at the call site; a table of
/// `{name, &value}` would cost an extra array and read worse.
#define POUND_THEME_CHECK_SCALE(name, value)                                                               \
    do                                                                                                     \
    {                                                                                                      \
        if (POUND_UNLIKELY(false == pound_theme_scale_is_valid(value)))                                    \
        {                                                                                                  \
            POUND_LOG_ERROR(&thread_logger,                                                                \
                            "Aborting function: scale '%s' is %f, which is not a "                         \
                            "non-negative finite value.",                                                  \
                            (name),                                                                        \
                            (double)(value));                                                              \
            return false;                                                                                  \
        }                                                                                                  \
    } while (false)

bool
pound_theme_is_valid(const pound_theme_t *POUND_RESTRICT theme)
{
    if (POUND_UNLIKELY(NULL == theme))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: theme is NULL.");
        return false;
    }

    if (POUND_UNLIKELY(false == theme->initialized))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the theme was never passed through "
                        "pound_theme_init.");
        return false;
    }

    if ((theme->mode != POUND_THEME_DARK) && (theme->mode != POUND_THEME_LIGHT))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: mode %d is not a theme mode.",
                        (int)theme->mode);
        return false;
    }

    pound_theme_color_role_entry_t roles[POUND_THEME_COLOR_ROLE_COUNT];
    pound_theme_build_roles(&theme->colors, roles, POUND_THEME_COLOR_ROLE_COUNT);

    for (int i = 0; i < POUND_THEME_COLOR_ROLE_COUNT; i++)
    {
        if (POUND_UNLIKELY(false == pound_theme_color_is_valid(roles[i].color)))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "Aborting function: colour '%s' is not four channels in [0, 1].",
                            roles[i].name);
            return false;
        }
    }

    POUND_THEME_CHECK_SCALE("spacing.xxs", theme->spacing.xxs);
    POUND_THEME_CHECK_SCALE("spacing.xs", theme->spacing.xs);
    POUND_THEME_CHECK_SCALE("spacing.sm", theme->spacing.sm);
    POUND_THEME_CHECK_SCALE("spacing.md", theme->spacing.md);
    POUND_THEME_CHECK_SCALE("spacing.lg", theme->spacing.lg);
    POUND_THEME_CHECK_SCALE("spacing.xl", theme->spacing.xl);
    POUND_THEME_CHECK_SCALE("spacing.xxl", theme->spacing.xxl);

    POUND_THEME_CHECK_SCALE("radius.none", theme->radius.none);
    POUND_THEME_CHECK_SCALE("radius.sm", theme->radius.sm);
    POUND_THEME_CHECK_SCALE("radius.md", theme->radius.md);
    POUND_THEME_CHECK_SCALE("radius.lg", theme->radius.lg);
    POUND_THEME_CHECK_SCALE("radius.pill", theme->radius.pill);

    POUND_THEME_CHECK_SCALE("stroke.none", theme->stroke.none);
    POUND_THEME_CHECK_SCALE("stroke.hairline", theme->stroke.hairline);
    POUND_THEME_CHECK_SCALE("stroke.subtle", theme->stroke.subtle);
    POUND_THEME_CHECK_SCALE("stroke.strong", theme->stroke.strong);

    return true;
}

void
pound_theme_log_summary(const pound_theme_t *POUND_RESTRICT theme)
{
    if (POUND_UNLIKELY(NULL == theme))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: theme is NULL.");
        return;
    }

    POUND_LOG_INFO(&thread_logger,
                   "Theme: %s, spacing %g..%g, radius %g..%g.",
                   pound_theme_mode_to_string(theme->mode),
                   (double)theme->spacing.xxs,
                   (double)theme->spacing.xxl,
                   (double)theme->radius.none,
                   (double)theme->radius.pill);

    pound_theme_color_role_entry_t roles[POUND_THEME_COLOR_ROLE_COUNT];
    pound_theme_build_roles(&theme->colors, roles, POUND_THEME_COLOR_ROLE_COUNT);

    for (int i = 0; i < POUND_THEME_COLOR_ROLE_COUNT; i++)
    {
        // `color` is a pointer to the array, not to its first element, so the
        // channels are reached by indexing the array it points at.
        const pound_theme_color_t *const color = roles[i].color;
        POUND_LOG_INFO(&thread_logger,
                       "  %-20s #%02X%02X%02X a=%g",
                       roles[i].name,
                       (unsigned int)((*color)[0] * 255.0F + 0.5F),
                       (unsigned int)((*color)[1] * 255.0F + 0.5F),
                       (unsigned int)((*color)[2] * 255.0F + 0.5F),
                       (double)(*color)[3]);
    }
}

#undef POUND_THEME_RGBA

/*** end of file ***/
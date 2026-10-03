#include "fonts.h"

#include "log.h"
#include <math.h>
#include <string.h>

/// Candidate typefaces, in the order they are tried.
///
/// Inter first because it is the face this interface was designed against: a tall
/// x-height makes 18px text legible at small sizes, and its numerals are tabular,
/// so a column of byte counts does not jitter row to row. The rest are platform
/// faces that exist without anyone bundling anything.
///
/// Both Linux paths are listed because distributions disagree about where a
/// user-installed font lands. A candidate that is not there is one skipped line in
/// the log, not a failure.
static const char *const POUND_FONT_CANDIDATES[] = {
    "/usr/share/fonts/truetype/inter/Inter-Regular.ttf",
    "/usr/share/fonts/truetype/inter/Inter_18pt-Regular.ttf",
    "/usr/share/fonts/TTF/Inter-Regular.ttf",
    "/usr/share/fonts/inter/Inter-Regular.ttf",
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "C:/Windows/Fonts/segoeui.ttf",
    "C:/Windows/Fonts/arial.ttf",
    "C:/Windows/Fonts/tahoma.ttf",
};

#define POUND_FONT_CANDIDATE_COUNT (sizeof(POUND_FONT_CANDIDATES) / sizeof(POUND_FONT_CANDIDATES[0]))

/// The unscaled point size of each role, as a multiple of the base.
///
/// Titles and small text are deliberately not a whole step away from the body: a
/// 1.5x title beside an 18px body looks like two different interfaces, and 1.25x
/// reads as emphasis rather than as a heading.
static const float POUND_FONT_ROLE_SCALES[POUND_FONT_ROLE_COUNT] = {
    [POUND_FONT_ROLE_BODY]  = 1.00F,
    [POUND_FONT_ROLE_TITLE] = 1.25F,
    [POUND_FONT_ROLE_MONO]  = 0.94F,
    [POUND_FONT_ROLE_SMALL] = 0.85F,
};

_Static_assert((int)POUND_FONT_ROLE_COUNT == 4,
               "POUND_FONT_ROLE_SCALES does not cover every pound_font_role_t");

float
pound_font_role_scale(const pound_font_role_t role)
{
    if (false == pound_font_role_is_valid((int)role))
    {
        return 1.0F;
    }

    return POUND_FONT_ROLE_SCALES[role];
}

bool
pound_font_role_is_valid(const int role)
{
    return (role >= 0) && (role < (int)POUND_FONT_ROLE_COUNT);
}

/// Builds an `ImFontConfig` for one size.
///
/// Zeroed rather than left to ImGui's defaulting: `AddFontFromFileTTF` only fills
/// in the fields the caller left at a sentinel, and a struct that was never
/// initialised happens to satisfy most of those and silently not the rest.
static ImFontConfig pound_font_make_config(const float size_pixels, const char *POUND_RESTRICT name)
{
    ImFontConfig config;
    memset(&config, 0, sizeof(config));

    if (NULL != name)
    {
        // `Name` is a fixed 40-byte field that exists purely so a font is readable
        // in a debugger, hence the bounded copy: `strncpy` alone would not
        // terminate a name longer than the field.
        const size_t limit  = sizeof(config.Name) - 1U;
        const size_t length = strlen(name);

        memcpy(config.Name, name, (length < limit) ? length : limit);
        config.Name[limit] = '\0';
    }

    config.SizePixels  = size_pixels;
    config.OversampleH = 2;
    config.OversampleV = 1;
    config.PixelSnapH  = false;

    return config;
}

/// Clamps a requested scale into the range the interface stays legible at.
///
/// Logs when it moves the value. A caller asking for 4.0 and being handed 2.5
/// without a word looks like a bug in the scaling, and the two have very different
/// causes.
static float pound_font_clamp_scale(const float scale)
{
    float clamped = scale;

    if (clamped < POUND_FONT_MIN_SCALE)
    {
        clamped = POUND_FONT_MIN_SCALE;
    }

    if (clamped > POUND_FONT_MAX_SCALE)
    {
        clamped = POUND_FONT_MAX_SCALE;
    }

    if (clamped != scale)
    {
        POUND_LOG_WARN(&thread_logger,
                       "A type scale of %g was asked for; using %g, the range that stays "
                       "legible.",
                       (double)scale,
                       (double)clamped);
    }

    return clamped;
}

error_t
pound_fonts_setup(pound_fonts_t *POUND_RESTRICT out_fonts, const float scale)
{
    if (POUND_UNLIKELY(NULL == out_fonts))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: out_fonts is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    memset(out_fonts, 0, sizeof(*out_fonts));

    if (POUND_UNLIKELY(false == isfinite(scale)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: a type scale of %g is not a finite number.",
                        (double)scale);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(scale <= 0.0F))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: a type scale of %g is not positive.",
                        (double)scale);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    ImGuiIO *io = igGetIO_Nil();

    if (POUND_UNLIKELY(NULL == io))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: there is no ImGui context to build a font atlas for.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    if (POUND_UNLIKELY(NULL == io->Fonts))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the ImGui context has no font atlas.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    ImFontAtlas *const atlas = io->Fonts;

    // Cleared before anything is added. ImGui loads its embedded font the first
    // time the atlas builds, so without this a rebuild leaves the previous run's
    // fonts in place alongside the new ones and nothing can say which is which.
    ImFontAtlas_Clear(atlas);

    const float applied_scale = pound_font_clamp_scale(scale);

    // A candidate path is not probed with `fopen` first: ImGui's loader reports the
    // failure by returning NULL, and probing would mean two answers to keep in step.
    ImFont     *base_font   = NULL;
    const char *chosen_path = NULL;

    for (size_t i = 0; i < POUND_FONT_CANDIDATE_COUNT; i++)
    {
        ImFontConfig  config    = pound_font_make_config(POUND_FONT_BASE_SIZE, "Pound Body");
        ImFont *const candidate = ImFontAtlas_AddFontFromFileTTF(
            atlas, POUND_FONT_CANDIDATES[i], POUND_FONT_BASE_SIZE, &config, NULL);

        if (NULL != candidate)
        {
            base_font   = candidate;
            chosen_path = POUND_FONT_CANDIDATES[i];
            break;
        }
    }

    if (NULL == base_font)
    {
        POUND_LOG_WARN(&thread_logger,
                       "None of the %zu system fonts tried were loadable, so Pound is using "
                       "ImGui's embedded font. Text will be blockier than the interface was "
                       "laid out for.",
                       POUND_FONT_CANDIDATE_COUNT);

        ImFontConfig config = pound_font_make_config(POUND_FONT_BASE_SIZE, "Pound Embedded");
        base_font           = ImFontAtlas_AddFontDefault(atlas, &config);

        if (POUND_UNLIKELY(NULL == base_font))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "Aborting function: even the embedded font would not load, so the "
                            "atlas has nothing to draw with.");
            return POUND_ERROR_ALLOCATION_FAILED;
        }

        out_fonts->using_embedded_font = true;
    }
    else
    {
        POUND_LOG_INFO(&thread_logger, "Type face: %s", chosen_path);
        out_fonts->using_embedded_font = false;
    }

    // The whole interface scales through the style, not through baked point sizes,
    // so a density change moves titles and captions by the same factor.
    ImGuiStyle *const style = igGetStyle();

    if (NULL != style)
    {
        style->FontScaleMain = applied_scale;
    }

    out_fonts->font  = base_font;
    out_fonts->scale = applied_scale;
    out_fonts->setup = true;
    return POUND_SUCCESS;
}

error_t
pound_fonts_set_scale(pound_fonts_t *POUND_RESTRICT fonts, const float scale)
{
    if (POUND_UNLIKELY(NULL == fonts))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: fonts is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY((false == isfinite(scale)) || (scale <= 0.0F)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: a type scale of %g is not a positive number.",
                        (double)scale);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const float applied_scale = pound_font_clamp_scale(scale);

    ImGuiStyle *const style = igGetStyle();

    // A missing style means there is no ImGui context, but the clamped value is
    // still recorded: the alternative is a caller that set a scale, saw it accepted,
    // and then found the interface unchanged with nothing to explain why.
    if (NULL != style)
    {
        style->FontScaleMain = applied_scale;
    }

    fonts->scale = applied_scale;
    return POUND_SUCCESS;
}

bool
pound_fonts_push(const pound_fonts_t *POUND_RESTRICT fonts, const pound_font_role_t role)
{
    if (POUND_UNLIKELY(NULL == fonts))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: fonts is NULL.");
        return false;
    }

    if (POUND_UNLIKELY(false == fonts->setup) || POUND_UNLIKELY(NULL == fonts->font))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: pound_fonts_setup has not run, so there is no font "
                        "to push.");
        return false;
    }

    if (POUND_UNLIKELY(false == pound_font_role_is_valid((int)role)))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: %d is not a font role.", (int)role);
        return false;
    }

    igPushFont(fonts->font, POUND_FONT_BASE_SIZE * POUND_FONT_ROLE_SCALES[role]);
    return true;
}

void
pound_fonts_pop(const bool pushed)
{
    if (true == pushed)
    {
        igPopFont();
    }
}

void
pound_fonts_log_summary(const pound_fonts_t *POUND_RESTRICT fonts)
{
    if (POUND_UNLIKELY(NULL == fonts))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: fonts is NULL.");
        return;
    }

    if (POUND_UNLIKELY(false == fonts->setup))
    {
        POUND_LOG_INFO(&thread_logger, "Fonts: not set up; ImGui's defaults are in use.");
        return;
    }

    POUND_LOG_INFO(&thread_logger,
                   "Fonts: base %gpt at %gx (%s), body %g, title %g, mono %g, small %g.",
                   (double)POUND_FONT_BASE_SIZE,
                   (double)fonts->scale,
                   (true == fonts->using_embedded_font) ? "embedded" : "system",
                   (double)(POUND_FONT_BASE_SIZE * POUND_FONT_ROLE_SCALES[POUND_FONT_ROLE_BODY]),
                   (double)(POUND_FONT_BASE_SIZE * POUND_FONT_ROLE_SCALES[POUND_FONT_ROLE_TITLE]),
                   (double)(POUND_FONT_BASE_SIZE * POUND_FONT_ROLE_SCALES[POUND_FONT_ROLE_MONO]),
                   (double)(POUND_FONT_BASE_SIZE * POUND_FONT_ROLE_SCALES[POUND_FONT_ROLE_SMALL]));
}

/*** end of file ***/

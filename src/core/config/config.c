#include "config.h"

#include "jit/jit_cache.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// The value of every field, as a single initialiser.
///
/// `config.h` publishes this so a test can compare against it and so the defaults
/// are one list rather than one per accessor. Assigning it here means the struct
/// is defaulted in one statement, which is also what makes it obvious when a new
/// field has been added without a default.
static const pound_config_t POUND_CONFIG_DEFAULTS = POUND_CONFIG_DEFAULT_INITIALIZER;

/// The two settings whose valid ranges are also compile-time facts elsewhere.
///
/// The JIT bounds are stated in the config header because the parser has to able
/// to check them before a cache exists; this is where that duplication is checked
/// against the cache's own constants, so a change to one without the other is a
/// build failure rather than a config that silently exceeds the cache.
_Static_assert(POUND_CONFIG_JIT_MIN_BYTES <= POUND_CONFIG_JIT_DEFAULT_BYTES,
               "the default JIT cache size is below the minimum");
_Static_assert(POUND_CONFIG_JIT_DEFAULT_BYTES <= POUND_CONFIG_JIT_MAX_BYTES,
               "the default JIT cache size is above the maximum");

error_t
pound_config_set_defaults(pound_config_t *POUND_RESTRICT config)
{
    if (POUND_UNLIKELY(NULL == config))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: config is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    *config = POUND_CONFIG_DEFAULTS;
    return POUND_SUCCESS;
}

error_t
pound_config_set_jit_cache_bytes(pound_config_t *POUND_RESTRICT config, const size_t bytes)
{
    if (POUND_UNLIKELY(NULL == config))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: config is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY((bytes < (size_t)POUND_CONFIG_JIT_MIN_BYTES)
                       || (bytes > (size_t)POUND_CONFIG_JIT_MAX_BYTES)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing %zu bytes for the JIT cache: it must be between %llu and "
                        "%llu bytes.",
                        bytes,
                        (unsigned long long)POUND_CONFIG_JIT_MIN_BYTES,
                        (unsigned long long)POUND_CONFIG_JIT_MAX_BYTES);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    config->jit_cache_bytes = bytes;
    return POUND_SUCCESS;
}

error_t
pound_config_set_log_level(pound_config_t *POUND_RESTRICT config, const log_level_t level)
{
    if (POUND_UNLIKELY(NULL == config))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: config is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    // Every level is legal, including NONE, which is how a user turns logging off
    // from the Settings page. The check is for a value outside the enum, which only
    // a corrupt saved file can produce.
    if (POUND_UNLIKELY((level < LOG_LEVEL_NONE) || (level > LOG_LEVEL_TRACE)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing log level %d: it is outside [%d, %d].",
                        (int)level,
                        (int)LOG_LEVEL_NONE,
                        (int)LOG_LEVEL_TRACE);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    config->log_level = level;
    return POUND_SUCCESS;
}

error_t
pound_config_set_ui_scale(pound_config_t *POUND_RESTRICT config, const float scale)
{
    if (POUND_UNLIKELY(NULL == config))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: config is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY((false == isfinite(scale)) || (scale < POUND_CONFIG_UI_SCALE_MIN)
                       || (scale > POUND_CONFIG_UI_SCALE_MAX)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing a UI scale of %g: it must be between %g and %g.",
                        (double)scale,
                        (double)POUND_CONFIG_UI_SCALE_MIN,
                        (double)POUND_CONFIG_UI_SCALE_MAX);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    config->ui_scale = scale;
    return POUND_SUCCESS;
}

error_t
pound_config_set_theme(pound_config_t *POUND_RESTRICT config, const pound_config_theme_t theme)
{
    if (POUND_UNLIKELY(NULL == config))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: config is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY((theme != POUND_CONFIG_THEME_DARK) && (theme != POUND_CONFIG_THEME_LIGHT)))
    {
        POUND_LOG_ERROR(&thread_logger, "Refusing theme %d: it is not a theme.", (int)theme);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    config->theme = theme;
    return POUND_SUCCESS;
}

bool
pound_config_set_path(char *POUND_RESTRICT destination,
                      const size_t              capacity,
                      const char *POUND_RESTRICT value)
{
    if (POUND_UNLIKELY(NULL == destination))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: destination is NULL.");
        return false;
    }

    if (POUND_UNLIKELY(0U == capacity))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: capacity is zero.");
        return false;
    }

    if (POUND_UNLIKELY(NULL == value))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: value is NULL.");
        destination[0] = '\0';
        return false;
    }

    const size_t length = strlen(value);

    if (length >= capacity)
    {
        POUND_LOG_WARN(&thread_logger,
                       "A %zu-character value does not fit in %zu bytes and has been "
                       "truncated.",
                       length,
                       capacity);
        memcpy(destination, value, capacity - 1);
        destination[capacity - 1] = '\0';
        return false;
    }

    memcpy(destination, value, length + 1);
    return true;
}

const char *
pound_config_log_level_to_string(const log_level_t level)
{
    switch (level)
    {
        case LOG_LEVEL_NONE:
            return "none";
        case LOG_LEVEL_ERROR:
            return "error";
        case LOG_LEVEL_WARN:
            return "warn";
        case LOG_LEVEL_INFO:
            return "info";
        case LOG_LEVEL_DEBUG:
            return "debug";
        case LOG_LEVEL_TRACE:
            return "trace";
    }

    return "unknown";
}

/// Case-insensitive equality against a literal.
///
/// Local rather than shared with the theme's copy: core does not include GUI
/// headers, and this is fifteen lines that would otherwise have to live in a
/// third place to be reachable from both.
static bool pound_config_text_equals(const char *POUND_RESTRICT text, const char *POUND_RESTRICT literal)
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

    return '\0' == text[literal_length];
}

bool
pound_config_log_level_from_string(const char *POUND_RESTRICT name, log_level_t *POUND_RESTRICT out_level)
{
    if (POUND_UNLIKELY(NULL == name))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: name is NULL.");
        return false;
    }

    if (POUND_UNLIKELY(NULL == out_level))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: out_level is NULL.");
        return false;
    }

    static const struct
    {
        const char *name;
        log_level_t level;

        /// Explicit padding: a pointer and a four-byte enum leave four bytes the
        /// build will not accept as implicit padding.
        char pad[4];
    } LEVELS[] = {
        { "none", LOG_LEVEL_NONE, { 0 } },
        { "error", LOG_LEVEL_ERROR, { 0 } },
        { "warn", LOG_LEVEL_WARN, { 0 } },
        { "info", LOG_LEVEL_INFO, { 0 } },
        { "debug", LOG_LEVEL_DEBUG, { 0 } },
        { "trace", LOG_LEVEL_TRACE, { 0 } },
    };

    for (size_t i = 0; i < (sizeof(LEVELS) / sizeof(LEVELS[0])); i++)
    {
        if (pound_config_text_equals(name, LEVELS[i].name))
        {
            *out_level = LEVELS[i].level;
            return true;
        }
    }

    POUND_LOG_ERROR(&thread_logger,
                    "Ignoring call: '%s' is not a log level; expected none, error, warn, info, "
                    "debug or trace.",
                    name);
    return false;
}

bool
pound_config_is_valid(const pound_config_t *POUND_RESTRICT config)
{
    if (POUND_UNLIKELY(NULL == config))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: config is NULL.");
        return false;
    }

    if (POUND_UNLIKELY(false == config->initialized))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the config was never passed through "
                        "pound_config_set_defaults.");
        return false;
    }

    if (POUND_UNLIKELY((config->jit_cache_bytes < (size_t)POUND_CONFIG_JIT_MIN_BYTES)
                       || (config->jit_cache_bytes > (size_t)POUND_CONFIG_JIT_MAX_BYTES)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the JIT cache size %zu is outside [%llu, %llu].",
                        config->jit_cache_bytes,
                        (unsigned long long)POUND_CONFIG_JIT_MIN_BYTES,
                        (unsigned long long)POUND_CONFIG_JIT_MAX_BYTES);
        return false;
    }

    if (POUND_UNLIKELY((config->log_level < LOG_LEVEL_NONE)
                       || (config->log_level > LOG_LEVEL_TRACE)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: log level %d is outside [%d, %d].",
                        (int)config->log_level,
                        (int)LOG_LEVEL_NONE,
                        (int)LOG_LEVEL_TRACE);
        return false;
    }

    if (POUND_UNLIKELY((false == isfinite(config->ui_scale))
                       || (config->ui_scale < POUND_CONFIG_UI_SCALE_MIN)
                       || (config->ui_scale > POUND_CONFIG_UI_SCALE_MAX)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the UI scale %g is outside [%g, %g].",
                        (double)config->ui_scale,
                        (double)POUND_CONFIG_UI_SCALE_MIN,
                        (double)POUND_CONFIG_UI_SCALE_MAX);
        return false;
    }

    if (POUND_UNLIKELY((config->theme != POUND_CONFIG_THEME_DARK)
                       && (config->theme != POUND_CONFIG_THEME_LIGHT)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: theme %d is not a theme.",
                        (int)config->theme);
        return false;
    }

    // Empty is legal for `key_file` and `font_path` -- it means "not set" -- but
    // not for `content_dir`, which has no sensible empty interpretation.
    if (POUND_UNLIKELY('\0' == config->content_dir[0]))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: content_dir is empty, and an empty directory is "
                        "not a place to look for titles.");
        return false;
    }

    return true;
}

/// Trims leading and trailing spaces and tabs, in place.
static void pound_config_trim(char *POUND_RESTRICT text)
{
    if (NULL == text)
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

/// Applies one parsed assignment.
///
/// Returns false for an unknown key or a value the setter refused. An unknown key
/// is not an error the loader aborts on: it is how a config written by a newer
/// Pound loads under an older one without losing the lines the newer one added.
static bool pound_config_apply(pound_config_t *POUND_RESTRICT config,
                               const char *POUND_RESTRICT    key,
                               const char *POUND_RESTRICT    value)
{
    if (pound_config_text_equals(key, "jit_cache_bytes"))
    {
        char *end   = NULL;
        const unsigned long long parsed = strtoull(value, &end, 10);

        if ((NULL == end) || (*end != '\0') || (end == value) || (parsed > (unsigned long long)SIZE_MAX))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "Ignoring setting 'jit_cache_bytes': '%s' is not a byte count.",
                            value);
            return false;
        }

        return POUND_SUCCESS == pound_config_set_jit_cache_bytes(config, (size_t)parsed);
    }

    if (pound_config_text_equals(key, "key_file"))
    {
        return pound_config_set_path(config->key_file, sizeof(config->key_file), value);
    }

    if (pound_config_text_equals(key, "content_dir"))
    {
        return pound_config_set_path(config->content_dir, sizeof(config->content_dir), value);
    }

    if (pound_config_text_equals(key, "font_path"))
    {
        return pound_config_set_path(config->font_path, sizeof(config->font_path), value);
    }

    if (pound_config_text_equals(key, "log_level"))
    {
        log_level_t level = LOG_LEVEL_WARN;

        if (false == pound_config_log_level_from_string(value, &level))
        {
            return false;
        }

        return POUND_SUCCESS == pound_config_set_log_level(config, level);
    }

    if (pound_config_text_equals(key, "ui_scale"))
    {
        char *end         = NULL;
        const double parsed = strtod(value, &end);

        if ((NULL == end) || (*end != '\0') || (end == value))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "Ignoring setting 'ui_scale': '%s' is not a number.",
                            value);
            return false;
        }

        return POUND_SUCCESS == pound_config_set_ui_scale(config, (float)parsed);
    }

    if (pound_config_text_equals(key, "theme"))
    {
        if (pound_config_text_equals(value, "dark"))
        {
            return POUND_SUCCESS == pound_config_set_theme(config, POUND_CONFIG_THEME_DARK);
        }

        if (pound_config_text_equals(value, "light"))
        {
            return POUND_SUCCESS == pound_config_set_theme(config, POUND_CONFIG_THEME_LIGHT);
        }

        POUND_LOG_ERROR(&thread_logger,
                        "Ignoring setting 'theme': '%s' is not dark or light.",
                        value);
        return false;
    }

    POUND_LOG_WARN(&thread_logger,
                   "Keeping unknown setting '%s'. It may belong to a newer Pound.",
                   key);
    return false;
}

/// Closes a file whose open succeeded, on a path where an earlier failure has
/// already decided the return value.
///
/// A close can fail -- a buffered write is flushed here, so this is where a full
/// disk surfaces -- and swallowing that would report success for a config that was
/// never finished. It is a warning rather than an error because the caller is
/// already returning one, and the more specific failure is the one worth keeping.
static void pound_config_close_after_failure(FILE *file, const char *POUND_RESTRICT path)
{
    if (0 != fclose(file))
    {
        POUND_LOG_WARN(&thread_logger,
                       "Closing '%s' also failed while reporting an earlier problem.",
                       path);
    }
}

error_t
pound_config_load(pound_config_t *POUND_RESTRICT config, const char *POUND_RESTRICT path)
{
    if (POUND_UNLIKELY(NULL == config))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: config is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(NULL == path))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: path is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY('\0' == path[0]))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: path is empty.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    // Defaults first, then the file over them. A file that is missing, empty or
    // partly unparseable therefore leaves a usable config behind rather than a
    // zeroed one that a subsystem would act on.
    if (POUND_SUCCESS != pound_config_set_defaults(config))
    {
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    FILE *file = fopen(path, "rb");

    if (NULL == file)
    {
        POUND_LOG_INFO(&thread_logger,
                       "No settings file at '%s'; using the defaults.",
                       path);
        return POUND_SUCCESS;
    }

    char   line[POUND_CONFIG_LINE_MAX];
    int    line_number  = 0;
    int    applied      = 0;
    int    rejected     = 0;
    error_t status      = POUND_SUCCESS;

    while (NULL != fgets(line, (int)sizeof(line), file))
    {
        ++line_number;

        size_t length = strlen(line);

        // A line that fills the buffer without a newline was longer than the
        // buffer, so what was read is a prefix of the real line. Parsing the
        // prefix would apply half a setting, so it is refused and the rest of the
        // line is skipped rather than parsed as a line of its own.
        if ((length == (sizeof(line) - 1U)) && ('\n' != line[length - 1U]))
        {
            int ch = 0;

            while (((ch = fgetc(file)) != '\n') && (EOF != ch))
            {
                // Skipping the remainder of an over-long line.
            }

            POUND_LOG_ERROR(&thread_logger,
                            "%s:%d is longer than the %d-byte line limit and was ignored.",
                            path,
                            line_number,
                            (int)POUND_CONFIG_LINE_MAX);
            ++rejected;
            status = POUND_ERROR_MALFORMED_HEADER;
            continue;
        }

        // Newlines and carriage returns are separators, not content. Both are
        // stripped because the same file may have been written on either platform.
        while ((length > 0U) && (('\n' == line[length - 1U]) || ('\r' == line[length - 1U])))
        {
            line[length - 1U] = '\0';
            --length;
        }

        pound_config_trim(line);

        if ((0U == length) || ('#' == line[0]) || (';' == line[0]))
        {
            continue;
        }

        char *separator = strchr(line, '=');

        if (NULL == separator)
        {
            POUND_LOG_ERROR(&thread_logger,
                            "%s:%d is not a 'key = value' assignment and was ignored.",
                            path,
                            line_number);
            ++rejected;
            status = POUND_ERROR_MALFORMED_HEADER;
            continue;
        }

        *separator = '\0';
        char *key   = line;
        char *value = separator + 1;

        pound_config_trim(key);
        pound_config_trim(value);

        if ('\0' == key[0])
        {
            POUND_LOG_ERROR(&thread_logger,
                            "%s:%d has an empty key and was ignored.",
                            path,
                            line_number);
            ++rejected;
            status = POUND_ERROR_MALFORMED_HEADER;
            continue;
        }

        if (pound_config_apply(config, key, value))
        {
            ++applied;
        }
        else
        {
            ++rejected;
        }
    }

    if (0 != ferror(file))
    {
        POUND_LOG_ERROR(&thread_logger, "A read from '%s' failed partway through.", path);
        pound_config_close_after_failure(file, path);
        return POUND_ERROR_IO;
    }

    if (0 != fclose(file))
    {
        POUND_LOG_ERROR(&thread_logger, "Closing '%s' failed after reading it.", path);
        return POUND_ERROR_IO;
    }

    if (0 != rejected)
    {
        POUND_LOG_WARN(&thread_logger,
                       "%s: applied %d setting(s) and skipped %d.",
                       path,
                       applied,
                       rejected);
    }
    else
    {
        POUND_LOG_INFO(&thread_logger, "%s: applied %d setting(s).", path, applied);
    }

    // The config is usable either way -- the assignments that parsed are in it --
    // so a malformed line is reported through the status without a reset.
    return status;
}

error_t
pound_config_save(const pound_config_t *POUND_RESTRICT config, const char *POUND_RESTRICT path)
{
    if (POUND_UNLIKELY(NULL == config))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: config is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(NULL == path))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: path is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY('\0' == path[0]))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: path is empty.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    // Validated before the file is opened, so a refusal does not leave a truncated
    // config where a good one used to be.
    if (POUND_UNLIKELY(false == pound_config_is_valid(config)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the config is invalid, so '%s' has been left "
                        "as it was.",
                        path);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    FILE *file = fopen(path, "wb");

    if (NULL == file)
    {
        POUND_LOG_ERROR(&thread_logger, "Could not open '%s' to write the settings.", path);
        return POUND_ERROR_IO;
    }

    int written = fprintf(file,
                          "# Pound settings. Written by the Settings page; safe to edit.\n"
                          "# jit_cache_bytes is clamped between %llu and %llu.\n"
                          "jit_cache_bytes = %zu\n"
                          "key_file = %s\n"
                          "content_dir = %s\n"
                          "font_path = %s\n"
                          "log_level = %s\n"
                          "ui_scale = %g\n"
                          "theme = %s\n",
                          (unsigned long long)POUND_CONFIG_JIT_MIN_BYTES,
                          (unsigned long long)POUND_CONFIG_JIT_MAX_BYTES,
                          config->jit_cache_bytes,
                          config->key_file,
                          config->content_dir,
                          config->font_path,
                          pound_config_log_level_to_string(config->log_level),
                          (double)config->ui_scale,
                          (POUND_CONFIG_THEME_LIGHT == config->theme) ? "light" : "dark");

    if (written < 0)
    {
        POUND_LOG_ERROR(&thread_logger, "Writing the settings to '%s' failed.", path);
        pound_config_close_after_failure(file, path);
        return POUND_ERROR_IO;
    }

    if (0 != fclose(file))
    {
        POUND_LOG_ERROR(&thread_logger, "Closing '%s' failed after writing it.", path);
        return POUND_ERROR_IO;
    }

    POUND_LOG_INFO(&thread_logger, "Wrote %d byte(s) of settings to '%s'.", written, path);
    return POUND_SUCCESS;
}

void
pound_config_log_summary(const pound_config_t *POUND_RESTRICT config)
{
    if (POUND_UNLIKELY(NULL == config))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: config is NULL.");
        return;
    }

    POUND_LOG_INFO(&thread_logger,
                   "Config: jit cache %zu bytes, log level %s, UI scale %g, theme %s.",
                   config->jit_cache_bytes,
                   pound_config_log_level_to_string(config->log_level),
                   (double)config->ui_scale,
                   (POUND_CONFIG_THEME_LIGHT == config->theme) ? "light" : "dark");
    POUND_LOG_INFO(&thread_logger,
                   "Config: key file '%s', content dir '%s', font '%s'.",
                   config->key_file[0] ? config->key_file : "(unset)",
                   config->content_dir,
                   config->font_path[0] ? config->font_path : "(built-in)");
}

/*** end of file ***/
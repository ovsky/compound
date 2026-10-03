#ifndef POUND_CORE_CONFIG_H
#define POUND_CORE_CONFIG_H

/// Pound's user settings, and the file they live in.
///
/// A single struct, a single set of defaults, and one parser and one serialiser
/// for the on-disk form. The point of collecting them here rather than scattering
/// them through the GUI is that a setting has to be *loadable* to be worth having:
/// a value the Settings page can change but that is forgotten when the window
/// closes is not a setting, it is a hidden state variable.
///
/// The file format is deliberately the dullest thing that works -- one
/// `key = value` per line, `#` for comments, unknown keys kept -- because the file
/// is meant to be hand-editable and a user who typos a key should get a warning
/// naming the line, not a parse failure that throws away the rest of the file.

#include "attributes.h"
#include "errors.h"
#include "log.h"
#include <stdbool.h>
#include <stddef.h>

/// Longest path Pound stores in the config.
///
/// A separate constant from `POUND_PATH_MAX` in the GUI header because that one
/// belongs to the window layer and this one has to be visible to core, which does
/// not and must not know that SDL exists.
#define POUND_CONFIG_PATH_MAX 512

/// Bounds the JIT cache may be configured to.
///
/// Duplicated from the JIT cache's own defaults rather than included from it: the
/// config is parsed before anything is allocated, and the bounds have to be
/// checkable without a live cache. `config.c` asserts the two agree.
#define POUND_CONFIG_JIT_MIN_BYTES (64ULL * 1024ULL * 1024ULL)
#define POUND_CONFIG_JIT_MAX_BYTES (4096ULL * 1024ULL * 1024ULL)
#define POUND_CONFIG_JIT_DEFAULT_BYTES (512ULL * 1024ULL * 1024ULL)

/// Bounds the UI scale may be configured to.
#define POUND_CONFIG_UI_SCALE_MIN 0.75F
#define POUND_CONFIG_UI_SCALE_MAX 3.00F
#define POUND_CONFIG_UI_SCALE_DEFAULT 1.00F

/// Which ramp the interface starts in.
///
/// A plain two-member enum rather than the theme module's: core does not include
/// GUI headers, and the GUI maps this onto `pound_theme_mode_t` where it belongs.
/// The integer values match on purpose so the mapping is a cast and not a table.
typedef enum
{
    POUND_CONFIG_THEME_DARK = 0,
    POUND_CONFIG_THEME_LIGHT = 1
} pound_config_theme_t;

/// How loud the log is, wanted by the Logs page and by the file sink.
///
/// Spelled with core's own level values so a `log_level_t` can be stored without
/// a translation step that somebody has to remember to reverse.
typedef struct
{
    /// Bytes of host code the JIT cache may hold.
    size_t jit_cache_bytes;

    /// The user's key file, as given. Not resolved until a title is opened, so a
    /// config can name a file that does not exist yet.
    char key_file[POUND_CONFIG_PATH_MAX];

    /// Where installed titles live, used by the library scan.
    char content_dir[POUND_CONFIG_PATH_MAX];

    /// An optional typeface. Empty means "use the built-in choice".
    char font_path[POUND_CONFIG_PATH_MAX];

    /// The minimum level a record has to reach to be emitted.
    log_level_t log_level;

    /// The interface scale, as a multiplier on the base type size.
    float ui_scale;

    /// Which ramp the interface starts in.
    pound_config_theme_t theme;

    /// False until `pound_config_set_defaults` has run. Checked by the GUI so an
    /// all-zero config shows up as "not loaded" rather than as a 0-byte cache and
    /// an empty key file.
    bool initialized;

    /// Explicit padding. The three paths, two enums and a float above leave the
    /// struct one byte past a four-byte boundary; three bytes of tail bring it to
    /// an eight-byte multiple, which the build requires rather than accepts as
    /// implicit padding.
    char pad[3];
} pound_config_t;

/// The value `pound_config_init` would produce, as a compile-time constant.
///
/// Exists so a test can assert that the defaults are exactly a known set rather
/// than "whatever the initialiser happens to do", and so a caller holding the
/// struct by value has something to reset to without an initialiser call.
#define POUND_CONFIG_DEFAULT_INITIALIZER                                                                   \
    {                                                                                                      \
        .jit_cache_bytes = POUND_CONFIG_JIT_DEFAULT_BYTES,                                                 \
        .key_file        = "keys.txt",                                                                     \
        .content_dir     = ".",                                                                            \
        .font_path       = "",                                                                             \
        .log_level       = LOG_LEVEL_WARN,                                                                 \
        .ui_scale        = POUND_CONFIG_UI_SCALE_DEFAULT,                                                  \
        .theme           = POUND_CONFIG_THEME_DARK,                                                        \
        .initialized     = true,                                                                           \
    }

/// Loads the defaults into `config`, discarding whatever was there.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` and logs if `config` is NULL.
error_t pound_config_set_defaults(pound_config_t *POUND_RESTRICT config);

/// True when `config` holds values a subsystem can act on.
///
/// Checks the ranges the setters enforce as well as the ones a hand-edited file
/// could violate, so a config that reached the struct without going through this
/// module is caught before it is used to size an allocation.
///
/// Returns false and logs the offending field.
bool pound_config_is_valid(const pound_config_t *POUND_RESTRICT config);

/// Sets the JIT cache size, in bytes, refusing a value outside the built-in
/// bounds rather than clamping it.
///
/// Refused rather than clamped because this is the one setting whose value also
/// decides whether the emulator can start at all: a 1-byte cache would be silently
/// promoted to 64 MiB and the user would believe a setting took effect that did
/// not. The caller learns the value was wrong and can say so.
error_t pound_config_set_jit_cache_bytes(pound_config_t *POUND_RESTRICT config, size_t bytes);

/// Sets the log level. Any member of `log_level_t` is accepted, including
/// `LOG_LEVEL_NONE`.
error_t pound_config_set_log_level(pound_config_t *POUND_RESTRICT config, log_level_t level);

/// Sets the UI scale, refusing a value outside the built-in bounds.
error_t pound_config_set_ui_scale(pound_config_t *POUND_RESTRICT config, float scale);

/// Sets the theme, refusing a value that is not a `pound_config_theme_t`.
error_t pound_config_set_theme(pound_config_t *POUND_RESTRICT config, pound_config_theme_t theme);

/// Copies `value` into `destination`, truncating rather than refusing.
///
/// Paths are copied from user input and from a file, and a too-long path is a
/// reason to warn rather than to discard a whole config. Returns false when it had
/// to truncate, so the caller can say which field was too long.
bool pound_config_set_path(char *POUND_RESTRICT       destination,
                           const size_t              capacity,
                           const char *POUND_RESTRICT value);

/// The `log_level_t` a name like `"warn"` or `"debug"` means.
///
/// Returns false and logs for an unknown name. Case-insensitive, because the name
/// comes from a file a person edits.
bool pound_config_log_level_from_string(const char *POUND_RESTRICT name,
                                        log_level_t *POUND_RESTRICT out_level);

/// The canonical name of a level, as written by the serialiser. Never NULL.
const char *pound_config_log_level_to_string(log_level_t level);

/// Reads `path` over the defaults and leaves the result in `config`.
///
/// A missing file is not an error: it is the ordinary first-run case, and is
/// reported at INFO with the defaults kept. Any other read failure, or a syntax
/// error on a line, is an error with a log record naming the line; the lines that
/// did parse are still applied, because a config with one bad line is more useful
/// than a reset one.
///
/// The file is parsed with a fixed-size line buffer, so a line longer than
/// `POUND_CONFIG_LINE_MAX` is rejected as malformed rather than silently split.
error_t pound_config_load(pound_config_t *POUND_RESTRICT config, const char *POUND_RESTRICT path);

/// Writes `config` to `path`, replacing whatever was there.
///
/// Written to `path` directly rather than through a temporary and a rename: the
/// settings file is small, the window that writes it is single-threaded, and a
/// half-written config is recoverable by deleting it. A rename-based write would
/// need a temporary path the caller cannot supply on all platforms, for a file
/// whose loss costs a user their log level.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` for a NULL argument or an invalid
/// config, and `POUND_ERROR_IO` when the file cannot be opened or a write fails,
/// logging the reason either way.
error_t pound_config_save(const pound_config_t *POUND_RESTRICT config,
                          const char *POUND_RESTRICT           path);

/// The longest line `pound_config_load` will consider.
///
/// A path is `POUND_CONFIG_PATH_MAX` bytes, a key is short, and a whole
/// assignment therefore fits with room to spare. A line at the cap means the file
/// is not the file this parser writes, and saying so is better than reading a
/// truncated setting.
#define POUND_CONFIG_LINE_MAX 1024

/// Logs every field at INFO.
void pound_config_log_summary(const pound_config_t *POUND_RESTRICT config);

#endif // POUND_CORE_CONFIG_H

/*** end of file ***/
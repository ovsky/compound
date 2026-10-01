#ifndef POUND_GUI_H
#define POUND_GUI_H

#include "attributes.h"
#include "platform.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Longest file path Pound will construct or accept.
///
/// Deliberately *not* named `MAX_PATH`: on Windows that identifier belongs to
/// the Win32 SDK (260) and redefining it is a hard compiler error.
#define POUND_PATH_MAX 4096

/// Size of the staging buffer used by `copy_file`.
#define FILE_BUFFER_SIZE 65536

#if POUND_PLATFORM_WINDOWS

#define GUI_PLUGIN_NAME "libPoundGui.dll"

#elif POUND_PLATFORM_APPLE

#define GUI_PLUGIN_NAME "libPoundGui.dylib"

#else

#define GUI_PLUGIN_NAME "libPoundGui.so"

#endif // POUND_PLATFORM_WINDOWS

/// Hot reload copies the freshly linked plugin to a unique sibling path and
/// `dlopen`/`LoadLibrary`es that copy, so the running image is never mutated
/// underneath the loader.
///
/// Android is excluded: `SDL_GetBasePath()` resolves inside the read-only APK,
/// so neither the copy nor the `dlopen` of an arbitrary path is possible on a
/// stock device. The GUI therefore ships as a static library there.
#if POUND_PLATFORM_ANDROID
#define POUND_PLATFORM_SUPPORTS_HOT_RELOAD 0
#else
#define POUND_PLATFORM_SUPPORTS_HOT_RELOAD 1
#endif

typedef enum
{
    GUI_PLUGIN_SUCCESS = 0,
    GUI_PLUGIN_ERROR_INVALID_ARGUMENT,
    GUI_PLUGIN_ERROR_ALLOCATION_FAILED,
    GUI_PLUGIN_ERROR_BUFFER_TOO_SMALL,
    GUI_PLUGIN_ERROR_PANEL_REGISTRY_FULL,
} gui_plugin_error_t;

/// Stable C ABI exported by the GUI shared object.
///
/// The host resolves exactly one symbol, `gui_plugin_exports_get`, and calls
/// the four function pointers it returns. Both sides must agree on this layout,
/// so the struct and every callback signature are append-only.
typedef struct
{
    /// Constructs a `gui_state_t` from an optional opaque state blob.
    gui_plugin_error_t (*create)(const void *saved_state, size_t saved_size, void **out);

    /// Destroys a context previously returned by `create`.
    gui_plugin_error_t (*destroy)(void *gui);

    /// Draws one frame.
    gui_plugin_error_t (*render_frame)(void *gui);

    /// Serialises state. Pass `out_gui == NULL` to query the required size.
    gui_plugin_error_t (*save)(void *gui, void *out_gui, size_t capacity, size_t *out_size);
} gui_plugin_exports_t;

typedef struct
{
    gui_plugin_exports_t exports;
    void                *module;

    /// gui_state_t.
    void *gui_context;

    /// Path of the private copy that is actually mapped, not the source path.
    char loaded_path[POUND_PATH_MAX];

    bool loaded;
    char pad[7];
} gui_plugin_t;

/// Returns a human readable description of `error`.
///
/// Never returns `NULL`; unknown values map to a sentinel string.
const char *gui_plugin_error_to_string(gui_plugin_error_t error);

/// gui.c
///
/// The single entry point the host resolves out of the plugin image.
POUND_EXPORT gui_plugin_error_t gui_plugin_exports_get(gui_plugin_exports_t *out);

/// gui_hot_reload.c

/// Loads `source_path`, resolves its exports and validates every callback.
POUND_EXPORT bool gui_plugin_load_module(gui_plugin_t *POUND_RESTRICT plugin,
                                         const char *POUND_RESTRICT   source_path);

/// Invokes `destroy`, unmaps the image and removes the private copy.
POUND_EXPORT void gui_plugin_destroy(gui_plugin_t *plugin);

/// Returns the last-write time of `path` in host units, or `0` when unavailable.
POUND_EXPORT uint64_t file_modified_time(const char *path);

/// Byte-for-byte copy. Removes the destination and returns `false` on failure.
POUND_EXPORT bool copy_file(const char *POUND_RESTRICT source,
                            const char *POUND_RESTRICT destination);

#endif // POUND_GUI_H

/*** end of file ***/

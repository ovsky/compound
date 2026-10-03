#ifndef POUND_GUI_PAGES_H
#define POUND_GUI_PAGES_H

/// The pages of the interface, and the shared furniture they draw with.
///
/// The shell in `gui.c` owns the chrome -- the sidebar, the title bar, the
/// keyboard shortcuts -- and knows nothing about what a page contains. This
/// module is the other half of that split: each page is a function that draws its
/// body into whatever rectangle the shell has already established, and the only
/// thing the two agree on is this header.
///
/// One deliberate limitation, stated here rather than discovered later: a page
/// draws *content*, not a window. The shell has already opened one and set the
/// clip rectangle; a page that called `igBegin` would produce a floating window
/// inside the content area, which is how an interface ends up with a window whose
/// title bar says "Library" floating over a page whose header also says
/// "Library".

#include "attributes.h"
#include "config/config.h"
#include "log.h"
#include "log_ring.h"
#include "theme/fonts.h"
#include "theme/theme.h"
#include "theme/theme_imgui.h"

#include "debug/debug_memory.h"

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include <stdbool.h>
#include <stddef.h>

/// Every page the sidebar can show, in the order the sidebar lists them.
///
/// The order is the enum's order because the registry table is indexed by it, so
/// reordering the enum reorders the sidebar and there is no second list to keep in
/// step.
typedef enum
{
    POUND_PAGE_LIBRARY = 0,
    POUND_PAGE_MEMORY,
    POUND_PAGE_LOGS,
    POUND_PAGE_SETTINGS,
    POUND_PAGE_ABOUT,
    POUND_PAGE_COUNT
} pound_page_t;

/// View state a page needs to remember from one frame to the next.
///
/// Held by the shell rather than as a function-local `static` in a page: the GUI
/// loads and reloads as a shared image during a hot reload, and state that lives
/// inside that image is state the user loses every time the code is rebuilt.
typedef struct
{
    /// The quietest record the Logs page shows.
    log_level_t log_min_level;

    /// Whether the Logs page keeps its newest record in view.
    ///
    /// On by default. Turned off while the user scrolls up, because a view that
    /// yanks itself back to the bottom while somebody is reading is worse than one
    /// that stops following.
    bool log_auto_scroll;

    /// The outcome of the most recent save or reload, shown on the Settings page.
    ///
    /// Stored rather than logged only so the user sees it where they clicked; it is
    /// logged as well, because a message that is only on screen is one a bug report
    /// does not carry.
    char settings_status[192];

    /// True when `settings_status` describes a failure.
    bool settings_status_is_error;

    /// Explicit padding. The level, the flag and the 192-byte status message leave
    /// the struct two bytes short of a four-byte multiple; two bytes of tail bring
    /// it there without an implicit pad.
    char pad[2];
} pound_pages_view_t;

/// How many log records the Logs page copies into its scratch buffer.
///
/// A cap rather than the ring's whole capacity: the buffer lives in the shell's
/// heap, and a log view that scrolls through a hundred thousand lines is not a
/// feature, it is a way to make the interface stutter. The newest
/// `POUND_LOG_SNAPSHOT_MAX` records are what a person actually looks at.
#define POUND_LOG_SNAPSHOT_MAX 256

/// Everything a page may read or change, assembled once per frame by the shell.
///
/// A structure rather than a long argument list because the list only grows: the
/// library page needed nothing but a content directory to begin with, the settings
/// page needs the config and the path to save it to, and the logs page needs the
/// ring. Threading five arguments through five functions to serve one of them is
/// how signatures end up with parameters no single page uses.
///
/// Which fields a page is allowed to write is stated on the field.
typedef struct
{
    /// The live settings. The Settings page writes through this.
    pound_config_t *POUND_RESTRICT config;

    /// The active palette. Never modified.
    const pound_theme_t *POUND_RESTRICT theme;

    /// The active type scale. Never modified.
    const pound_fonts_t *POUND_RESTRICT fonts;

    /// Where the Settings page saves to and loads from.
    const char *POUND_RESTRICT settings_path;

    /// Records the Logs page reads, and clears. Never NULL once the shell has
    /// installed it; the page degrades to a message if it is.
    pound_log_ring_t *POUND_RESTRICT log_ring;

    /// Scratch space the Logs page snapshots records into.
    ///
    /// Owned by the shell and reused every frame rather than allocated per frame:
    /// a log view that allocates a quarter of a megabyte on every render is a log
    /// view that makes the allocator visible in a profile.
    pound_log_record_t *POUND_RESTRICT log_scratch;

    /// How many records `log_scratch` can hold. Must be at least
    /// `POUND_LOG_SNAPSHOT_MAX` for the Logs page to use it.
    size_t log_scratch_capacity;

    /// Guest-memory visualisation state. The Memory page renders and advances it.
    debug_memory_tracker_t *POUND_RESTRICT memory_tracker;

    /// Scroll position and log filter. The shell owns the storage.
    pound_pages_view_t *POUND_RESTRICT view;

    /// Set to true by a page that has changed the config's appearance -- the theme
    /// or the interface scale -- in a way the running interface must pick up. The
    /// shell re-applies the palette and the type scale at the top of the next frame
    /// and clears it.
    ///
    /// Distinct from persistence: writing the file is explicit, done by the Settings
    /// page's Save button, so a colour the user is still deciding between does not
    /// get committed behind them.
    bool *POUND_RESTRICT config_dirty;
} pound_page_context_t;

typedef void (*pound_page_render_t)(const pound_page_context_t *POUND_RESTRICT context);

/// One row of the sidebar: what to call the page and how to draw it.
typedef struct
{
    const char          *name;
    const char          *subtitle;
    pound_page_render_t  render;
} pound_page_descriptor_t;

/// The registry, indexed by `pound_page_t`.
///
/// Exactly `POUND_PAGE_COUNT` entries, in enum order. Returned as a pointer to a
/// static array so the sidebar can walk it without copying; callers index it and
/// never modify it.
const pound_page_descriptor_t *pound_pages_registry(void);

/// True when `page` is a member of `pound_page_t`.
///
/// Total, and used by the shell before indexing the registry with a value that
/// may have come from a saved state file.
bool pound_page_is_valid(int page);

/// The display name of a page, or `"Unknown"` for a value that is not one.
const char *pound_page_name(int page);

/// Draws `page`'s body into the current window.
///
/// A NULL `context`, or a page that is not a member of `pound_page_t`, is logged
/// and drawn as a short message rather than left blank: a page that renders
/// nothing is indistinguishable from a page that has no content, which is the
/// exact confusion an empty state exists to avoid.
void pound_page_render(pound_page_t page, const pound_page_context_t *POUND_RESTRICT context);

/// The pages. Declared here rather than kept private to a single translation unit
/// because the registry in `pages.c` is the only place that names them, and that
/// table is the one thing that has to stay in step with the enum.
void pound_page_render_library(const pound_page_context_t *POUND_RESTRICT context);
void pound_page_render_memory(const pound_page_context_t *POUND_RESTRICT context);
void pound_page_render_logs(const pound_page_context_t *POUND_RESTRICT context);
void pound_page_render_settings(const pound_page_context_t *POUND_RESTRICT context);
void pound_page_render_about(const pound_page_context_t *POUND_RESTRICT context);

/// page furniture
///
/// The pieces every page draws in the same way. Kept here rather than duplicated
/// per page so "the heading is 1.25x and the body is at 16px" is a fact about the
/// interface rather than a fact about five files.

/// Draws the page's title block: an optional small eyebrow line, the title in the
/// title role, and an optional subtitle in muted text.
///
/// `title` must not be NULL. `eyebrow` and `subtitle` may be.
void pound_page_heading(const pound_page_context_t *POUND_RESTRICT context,
                        const char *POUND_RESTRICT               eyebrow,
                        const char *POUND_RESTRICT               title,
                        const char *POUND_RESTRICT               subtitle);

/// A section label with a rule under it, for grouping settings or list content.
void pound_page_section(const pound_page_context_t *POUND_RESTRICT context,
                        const char *POUND_RESTRICT               title);

/// Opens a card -- a raised surface with the theme's padding -- and returns
/// whether its contents should be drawn.
///
/// Like `igBegin`, the body belongs inside the `if`, and `pound_page_end_card`
/// must be called outside it regardless. `height` of 0 sizes the card to its
/// contents.
bool pound_page_begin_card(const pound_page_context_t *POUND_RESTRICT context,
                           const char *POUND_RESTRICT               id,
                           float                                    height);

/// Closes the card most recently opened by `pound_page_begin_card`.
void pound_page_end_card(void);

/// Draws a centred block of muted text, for a page or list with nothing in it.
///
/// Takes a title and a body because an empty state has to say two things: what is
/// not here, and why that is the expected situation rather than a failure.
void pound_page_empty_state(const pound_page_context_t *POUND_RESTRICT context,
                            const char *POUND_RESTRICT               title,
                            const char *POUND_RESTRICT               body);

/// Draws a label in muted text followed by a value, on one row.
///
/// The label column is fixed width so a stack of rows lines up; `percent_width`
/// is that width as a fraction of the content region.
void pound_page_labeled_value(const pound_page_context_t *POUND_RESTRICT context,
                              const char *POUND_RESTRICT               label,
                              const char *POUND_RESTRICT               value,
                              float                                    percent_width);

/// Draws a settings row's label, and its explanatory line when `help` is not NULL.
///
/// The control for the setting is drawn by the caller immediately after, on the
/// line below. Label-above-control rather than label-beside-control because the
/// help text needs the full width to stay readable, and a form where the labels
/// are all the same width beside their controls has no room for one.
void pound_page_setting_label(const pound_page_context_t *POUND_RESTRICT context,
                              const char *POUND_RESTRICT               label,
                              const char *POUND_RESTRICT               help);

/// The log levels in the order the interface offers them: loudest first.
///
/// Shared by the Logs page's filter and the Settings page's log-level control, so
/// the two cannot disagree about what order the levels go in or how they are
/// spelled. Returns a null-separated list suitable for `igCombo_Str`.
const char *pound_page_log_level_items(void);

/// The index of `level` in `pound_page_log_level_items`, or the `Warn` row when
/// `level` is not one of them.
int pound_page_log_level_index(log_level_t level);

/// The level at `index`, or `LOG_LEVEL_WARN` when `index` is not in range.
log_level_t pound_page_log_level_at(int index);

/// The lowercase name of a level, for a label or a log line. Never NULL.
const char *pound_page_log_level_label(log_level_t level);

#endif // POUND_GUI_PAGES_H

/*** end of file ***/

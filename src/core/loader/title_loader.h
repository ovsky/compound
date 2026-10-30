#ifndef POUND_LOADER_TITLE_LOADER_H
#define POUND_LOADER_TITLE_LOADER_H

//! Title format detection and the executable-image description the boot layer
//! consumes.
//!
//! Detection is by file content, never by extension. A file called `game.nro`
//! that starts with something else is not an NRO, and a real NRO without an
//! extension is still an NRO. The only two formats this build can actually map
//! are the unencrypted executable images NRO and NSO; the package formats NCA,
//! NSP and XCI are recognised so the refusal has a name, but they require
//! Nintendo keys and a filesystem stack this build does not ship.
//!
//! ## Switch 2
//!
//! The repository does not contain, and cannot contain, Nintendo's keys. A
//! Switch 2 title is an encrypted package; detecting its magic is not evidence
//! that its content can be decrypted or executed. `is_switch2_compatible` is
//! therefore always false in this build. It is a statement about Compound, not
//! about the title, and setting it from a file extension would be exactly the
//! fabricated capability the project forbids.

#include "attributes.h"
#include "errors.h"
#include "fs/fs_reader.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    POUND_TITLE_UNKNOWN = 0,
    POUND_TITLE_NRO,
    POUND_TITLE_NSO,
    POUND_TITLE_NCA,
    POUND_TITLE_NSP,
    POUND_TITLE_XCI,

    /// Legacy enum value. Decoding HAC packages is not implemented and no
    /// content is ever classified as one; the value stays only so existing
    /// switch statements keep compiling.
    POUND_TITLE_HAC,
} pound_title_type_t;

/// Number of executable segments an NRO or NSO describes: text, rodata, data.
#define POUND_TITLE_SEGMENT_COUNT 3U

/// One segment of an executable image.
///
/// `file_offset` is absolute in the title file. `memory_offset` is relative to
/// the image base the boot layer chooses. NRO has no separate memory offset, so
/// its parser copies the file offset; NSO carries both.
typedef struct
{
    uint64_t file_offset;
    uint64_t memory_offset;

    /// Uncompressed length, which is what the segment occupies in guest memory.
    uint64_t size;

    /// Bytes the file actually stores at `file_offset`. Equal to `size` for an
    /// uncompressed segment; smaller for an NSO segment whose compression flag is
    /// set. The boot layer reads this many bytes and expands them to `size`.
    uint64_t stored_size;

    /// True when `stored_size` bytes are one raw LZ4 block. NRO never sets this.
    bool compressed;

    uint8_t pad[7];
} pound_title_segment_t;

/// Everything the boot layer needs to place an executable image.
typedef struct
{
    pound_title_type_t type;
    uint32_t           version;
    uint32_t           flags;

    pound_title_segment_t segments[POUND_TITLE_SEGMENT_COUNT];
    uint32_t              bss_size;

    /// One past the last byte the image occupies, relative to the image base.
    /// This already includes `bss_size` after the data segment.
    uint64_t image_size;

    /// Offset of the first instruction, relative to the image base.
    uint64_t entry_offset;
} pound_title_layout_t;

typedef struct
{
    pound_title_type_t type;
    char               path[512];

    uint32_t format_version;

    /// The content needs a key this build does not have. True for NCA, NSP and
    /// XCI; their metadata is visible, their payload is not.
    bool requires_keys;

    /// The bytes are encrypted, whether or not a key would be enough to open
    /// them. Kept separate from `requires_keys` because an unencrypted package
    /// is still not bootable here.
    bool is_encrypted;

    /// This build can parse and map the image. True only for an uncompressed
    /// NRO or NSO.
    bool is_bootable;

    /// Always false; see the note at the top of the header.
    bool is_switch2_compatible;

    uint8_t pad[3];
} pound_title_info_t;

/// Detects the format of `path` from its contents.
///
/// Returns `POUND_SUCCESS` for a recognised format, `POUND_ERROR_UNSUPPORTED_FORMAT`
/// when the magic matches no format, and an I/O/argument error when the file
/// cannot be read. `info->path` is copied and always NUL-terminated.
error_t pound_title_detect(const char *POUND_RESTRICT path, pound_title_info_t *POUND_RESTRICT info);

/// Detects a format through an already-open reader.
///
/// `info->path` is left empty because a reader has no name. The reader is not
/// retained.
error_t pound_title_detect_reader(const fs_reader_t *POUND_RESTRICT reader,
                                  pound_title_info_t *POUND_RESTRICT info);

/// Parses and validates the executable layout of an NRO or NSO.
///
/// Refuses package formats and compressed NSO segments with
/// `POUND_ERROR_UNSUPPORTED_FEATURE`. Every offset and size is checked against
/// the reader and against 64-bit overflow before it is stored.
error_t pound_title_parse_reader(const fs_reader_t *POUND_RESTRICT reader,
                                 pound_title_type_t                 type,
                                 pound_title_layout_t *POUND_RESTRICT layout);

/// Validates a detected title without mapping it.
///
/// For NRO/NSO this parses the image and logs its entry point and extent. For
/// NCA/NSP/XCI it returns `POUND_ERROR_UNSUPPORTED_FEATURE` and says which
/// missing capability caused the refusal; it never reports success for a format
/// the boot layer cannot start.
error_t pound_title_load(const pound_title_info_t *POUND_RESTRICT info);

/// Initializes loader subsystem. Kept for callers that want to state the
/// dependency explicitly; detection and parsing need no mutable global state.
error_t pound_title_loader_init(void);

#ifdef __cplusplus
}
#endif

#endif // POUND_LOADER_TITLE_LOADER_H

/*** end of file ***/

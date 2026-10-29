#include "loader/title_loader.h"

#include "fs/file_reader.h"
#include "log.h"

#include <stdio.h>
#include <string.h>

/// NRO files start with a 16-byte `NroStart` block; the magic and the header
/// proper follow at 0x10. The header ends at 0x80.
#define NRO_MAGIC_OFFSET 0x10U
#define NRO_HEADER_SIZE  0x80U

/// NSO puts its magic first and its header is 0x100 bytes.
#define NSO_HEADER_SIZE 0x100U

/// The NCA magic sits behind the two 0x100-byte signatures.
#define NCA_MAGIC_OFFSET 0x200U

/// Largest prefix needed to identify any format this module knows.
#define TITLE_PROBE_SIZE 0x204U

static uint32_t
load_le32(const uint8_t *POUND_RESTRICT bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) | ((uint32_t)bytes[2] << 16U)
         | ((uint32_t)bytes[3] << 24U);
}

static bool
add_u64(const uint64_t left, const uint64_t right, uint64_t *POUND_RESTRICT out)
{
    if (POUND_UNLIKELY(NULL == out))
    {
        return false;
    }

    if (POUND_UNLIKELY(left > (UINT64_MAX - right)))
    {
        return false;
    }

    *out = left + right;
    return true;
}

static bool
magic_is(const uint8_t *POUND_RESTRICT bytes, const size_t offset, const char *POUND_RESTRICT magic)
{
    return (memcmp(bytes + offset, magic, 4U) == 0);
}

/// Stores one segment after checking that its file range is inside the image.
///
/// `limit` is the declared NRO extent or the reader size for an NSO. A zero
/// length segment is legal and keeps its declared offset; there is nothing to
/// read, and rejecting it would break images that omit rodata.
static error_t
store_segment(const fs_reader_t *POUND_RESTRICT reader,
              const uint64_t                      file_offset,
              const uint64_t                      memory_offset,
              const uint64_t                      size,
              const uint64_t                      limit,
              pound_title_segment_t *POUND_RESTRICT segment,
              uint64_t *POUND_RESTRICT              image_end)
{
    uint64_t file_end = 0U;
    if (false == add_u64(file_offset, size, &file_end))
    {
        POUND_LOG_ERROR(&thread_logger, "Title segment wraps the 64-bit file address space.");
        return POUND_ERROR_MALFORMED_HEADER;
    }

    uint64_t memory_end = 0U;
    if (false == add_u64(memory_offset, size, &memory_end))
    {
        POUND_LOG_ERROR(&thread_logger, "Title segment wraps the 64-bit memory address space.");
        return POUND_ERROR_MALFORMED_HEADER;
    }

    if (file_end > limit)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Title segment [%llu, %llu) is outside the %llu-byte image limit.",
                        (unsigned long long)file_offset,
                        (unsigned long long)file_end,
                        (unsigned long long)limit);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    if (false == fs_reader_range_is_valid(reader, file_offset, (size_t)size))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Title segment [%llu, %llu) is outside the %llu-byte file.",
                        (unsigned long long)file_offset,
                        (unsigned long long)file_end,
                        (unsigned long long)reader->size);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    segment->file_offset   = file_offset;
    segment->memory_offset = memory_offset;
    segment->size          = size;

    if (memory_end > *image_end)
    {
        *image_end = memory_end;
    }

    return POUND_SUCCESS;
}

static uint32_t
detected_version(const pound_title_type_t type, const uint8_t *POUND_RESTRICT probe)
{
    switch (type)
    {
        case POUND_TITLE_NRO:
            return load_le32(probe + 0x14U);
        case POUND_TITLE_NSO:
            return load_le32(probe + 0x04U);
        case POUND_TITLE_NCA:
            return (probe[NCA_MAGIC_OFFSET + 3U] - (uint8_t)'0');
        default:
            return 0U;
    }
}

error_t
pound_title_detect_reader(const fs_reader_t *POUND_RESTRICT reader,
                          pound_title_info_t *POUND_RESTRICT info)
{
    if (POUND_UNLIKELY(NULL == reader) || POUND_UNLIKELY(NULL == info))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting title detection: reader=%p, info=%p.",
                        (const void *)reader, (const void *)info);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    memset(info, 0, sizeof(*info));

    if ((NULL == reader->read_at) || (0U == reader->size))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting title detection: the reader is empty or unbound.");
        return POUND_ERROR_UNSUPPORTED_FORMAT;
    }

    uint8_t probe[TITLE_PROBE_SIZE] = {0};
    const size_t probe_size = (reader->size < TITLE_PROBE_SIZE) ? (size_t)reader->size : TITLE_PROBE_SIZE;

    const error_t read_error = fs_reader_read_at(reader, 0U, probe, probe_size);
    if (POUND_SUCCESS != read_error)
    {
        POUND_LOG_ERROR(&thread_logger, "Title detection could not read the file header.");
        return read_error;
    }

    if ((probe_size >= (NRO_MAGIC_OFFSET + 4U)) && magic_is(probe, NRO_MAGIC_OFFSET, "NRO0"))
    {
        info->type              = POUND_TITLE_NRO;
        info->format_version    = detected_version(POUND_TITLE_NRO, probe);
        info->is_bootable       = true;
        info->is_encrypted      = false;
        info->requires_keys     = false;
        info->is_switch2_compatible = false;
        return POUND_SUCCESS;
    }

    if ((probe_size >= 4U) && magic_is(probe, 0U, "NSO0"))
    {
        info->type              = POUND_TITLE_NSO;
        info->format_version    = detected_version(POUND_TITLE_NSO, probe);
        info->is_bootable       = true;
        info->is_encrypted      = false;
        info->requires_keys     = false;
        info->is_switch2_compatible = false;
        return POUND_SUCCESS;
    }

    if ((probe_size >= (NCA_MAGIC_OFFSET + 4U)) && magic_is(probe, NCA_MAGIC_OFFSET, "NCA"))
    {
        info->type              = POUND_TITLE_NCA;
        info->format_version    = detected_version(POUND_TITLE_NCA, probe);
        info->is_bootable       = false;
        info->is_encrypted      = true;
        info->requires_keys     = true;
        info->is_switch2_compatible = false;
        return POUND_SUCCESS;
    }

    if ((probe_size >= 4U) && magic_is(probe, 0U, "PFS0"))
    {
        info->type              = POUND_TITLE_NSP;
        info->is_bootable       = false;
        info->is_encrypted      = true;
        info->requires_keys     = true;
        info->is_switch2_compatible = false;
        return POUND_SUCCESS;
    }

    if ((probe_size >= 4U) && magic_is(probe, 0U, "HEAD"))
    {
        info->type              = POUND_TITLE_XCI;
        info->is_bootable       = false;
        info->is_encrypted      = true;
        info->requires_keys     = true;
        info->is_switch2_compatible = false;
        return POUND_SUCCESS;
    }

    POUND_LOG_WARN(&thread_logger, "Title header matches no format Compound recognises.");
    return POUND_ERROR_UNSUPPORTED_FORMAT;
}

error_t
pound_title_detect(const char *POUND_RESTRICT path, pound_title_info_t *POUND_RESTRICT info)
{
    if (POUND_UNLIKELY(NULL == path) || POUND_UNLIKELY(NULL == info) || POUND_UNLIKELY('\0' == path[0]))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting title detection: path=%p, info=%p.",
                        (const void *)path, (const void *)info);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (strlen(path) >= sizeof(info->path))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting title path: it is longer than %zu bytes.", sizeof(info->path) - 1U);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    fs_file_t file;
    error_t   result = fs_file_open(&file, path);
    if (POUND_SUCCESS != result)
    {
        memset(info, 0, sizeof(*info));
        return result;
    }

    fs_reader_t reader;
    result = fs_file_reader(&file, &reader);
    if (POUND_SUCCESS == result)
    {
        result = pound_title_detect_reader(&reader, info);
    }

    fs_file_close(&file);

    if (POUND_SUCCESS == result)
    {
        (void)snprintf(info->path, sizeof(info->path), "%s", path);
    }
    else
    {
        memset(info, 0, sizeof(*info));
    }

    return result;
}

static error_t
parse_nro(const fs_reader_t *POUND_RESTRICT reader, pound_title_layout_t *POUND_RESTRICT layout)
{
    uint8_t header[NRO_HEADER_SIZE] = {0};
    const error_t read_error        = fs_reader_read_at(reader, 0U, header, sizeof(header));
    if (POUND_SUCCESS != read_error)
    {
        return read_error;
    }

    if (false == magic_is(header, NRO_MAGIC_OFFSET, "NRO0"))
    {
        POUND_LOG_ERROR(&thread_logger, "NRO header magic is missing at 0x%X.", NRO_MAGIC_OFFSET);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    const uint32_t declared_size = load_le32(header + 0x18U);
    if ((declared_size < NRO_HEADER_SIZE) || ((uint64_t)declared_size > reader->size))
    {
        POUND_LOG_ERROR(&thread_logger, "NRO declares a %u-byte image inside a %llu-byte file.",
                        declared_size, (unsigned long long)reader->size);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    layout->type    = POUND_TITLE_NRO;
    layout->version = load_le32(header + 0x14U);
    layout->flags   = load_le32(header + 0x1CU);

    uint64_t image_end = 0U;
    for (uint32_t index = 0U; index < POUND_TITLE_SEGMENT_COUNT; ++index)
    {
        const size_t field_offset = 0x20U + ((size_t)index * 8U);
        const uint64_t file_offset = load_le32(header + field_offset);
        const uint64_t size        = load_le32(header + field_offset + 4U);

        const error_t segment_error = store_segment(reader,
                                                    file_offset,
                                                    file_offset,
                                                    size,
                                                    declared_size,
                                                    &layout->segments[index],
                                                    &image_end);
        if (POUND_SUCCESS != segment_error)
        {
            return segment_error;
        }
    }

    layout->bss_size     = load_le32(header + 0x38U);
    layout->entry_offset = layout->segments[0].memory_offset;

    uint64_t data_end = 0U;
    if (false == add_u64(layout->segments[2].memory_offset, layout->segments[2].size, &data_end))
    {
        return POUND_ERROR_MALFORMED_HEADER;
    }

    uint64_t bss_end = 0U;
    if (false == add_u64(data_end, layout->bss_size, &bss_end))
    {
        POUND_LOG_ERROR(&thread_logger, "NRO BSS wraps the 64-bit memory address space.");
        return POUND_ERROR_MALFORMED_HEADER;
    }

    if (bss_end > image_end)
    {
        image_end = bss_end;
    }

    layout->image_size = image_end;
    return POUND_SUCCESS;
}

static error_t
parse_nso(const fs_reader_t *POUND_RESTRICT reader, pound_title_layout_t *POUND_RESTRICT layout)
{
    uint8_t header[NSO_HEADER_SIZE] = {0};
    const error_t read_error        = fs_reader_read_at(reader, 0U, header, sizeof(header));
    if (POUND_SUCCESS != read_error)
    {
        return read_error;
    }

    if (false == magic_is(header, 0U, "NSO0"))
    {
        POUND_LOG_ERROR(&thread_logger, "NSO header magic is missing.");
        return POUND_ERROR_MALFORMED_HEADER;
    }

    layout->type    = POUND_TITLE_NSO;
    layout->version = load_le32(header + 0x04U);
    layout->flags   = load_le32(header + 0x0CU);

    // Bits 0, 1 and 2 say text, rodata and data are LZ4-compressed. Decompression
    // is a separate subsystem; pretending the compressed bytes are code would
    // hand the interpreter garbage.
    if ((layout->flags & 0x7U) != 0U)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "NSO uses LZ4-compressed segments (flags=0x%X); this build does not decompress them.",
                        layout->flags);
        return POUND_ERROR_UNSUPPORTED_FEATURE;
    }

    struct
    {
        size_t file_offset_field;
        size_t memory_offset_field;
        size_t size_field;
        size_t stored_size_field;
    } const fields[POUND_TITLE_SEGMENT_COUNT] = {
        {0x10U, 0x14U, 0x18U, 0x60U},
        {0x20U, 0x24U, 0x28U, 0x64U},
        {0x30U, 0x34U, 0x38U, 0x68U},
    };

    uint64_t image_end = 0U;
    for (uint32_t index = 0U; index < POUND_TITLE_SEGMENT_COUNT; ++index)
    {
        const uint64_t file_offset   = load_le32(header + fields[index].file_offset_field);
        const uint64_t memory_offset = load_le32(header + fields[index].memory_offset_field);
        const uint64_t size          = load_le32(header + fields[index].size_field);
        const uint64_t stored_size   = load_le32(header + fields[index].stored_size_field);

        if ((0U != stored_size) && (stored_size != size))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "NSO segment %u declares size %llu but stores %llu bytes without the compressed flag.",
                            index,
                            (unsigned long long)size,
                            (unsigned long long)stored_size);
            return POUND_ERROR_MALFORMED_HEADER;
        }

        const error_t segment_error = store_segment(reader,
                                                    file_offset,
                                                    memory_offset,
                                                    size,
                                                    reader->size,
                                                    &layout->segments[index],
                                                    &image_end);
        if (POUND_SUCCESS != segment_error)
        {
            return segment_error;
        }
    }

    layout->bss_size     = load_le32(header + 0x3CU);
    layout->entry_offset = layout->segments[0].memory_offset;

    uint64_t data_end = 0U;
    if (false == add_u64(layout->segments[2].memory_offset, layout->segments[2].size, &data_end))
    {
        return POUND_ERROR_MALFORMED_HEADER;
    }

    uint64_t bss_end = 0U;
    if (false == add_u64(data_end, layout->bss_size, &bss_end))
    {
        POUND_LOG_ERROR(&thread_logger, "NSO BSS wraps the 64-bit memory address space.");
        return POUND_ERROR_MALFORMED_HEADER;
    }

    if (bss_end > image_end)
    {
        image_end = bss_end;
    }

    layout->image_size = image_end;
    return POUND_SUCCESS;
}

error_t
pound_title_parse_reader(const fs_reader_t *POUND_RESTRICT reader,
                         const pound_title_type_t           type,
                         pound_title_layout_t *POUND_RESTRICT layout)
{
    if (POUND_UNLIKELY(NULL == reader) || POUND_UNLIKELY(NULL == layout))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting title parse: reader=%p, layout=%p.",
                        (const void *)reader, (const void *)layout);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    memset(layout, 0, sizeof(*layout));

    switch (type)
    {
        case POUND_TITLE_NRO:
            return parse_nro(reader, layout);

        case POUND_TITLE_NSO:
            return parse_nso(reader, layout);

        case POUND_TITLE_NCA:
        case POUND_TITLE_NSP:
        case POUND_TITLE_XCI:
            POUND_LOG_ERROR(&thread_logger,
                            "Refusing to boot this title: package formats need Nintendo keys and "
                            "the encrypted-content stack, neither of which this build implements.");
            return POUND_ERROR_UNSUPPORTED_FEATURE;

        case POUND_TITLE_UNKNOWN:
        case POUND_TITLE_HAC:
        default:
            POUND_LOG_ERROR(&thread_logger, "Refusing to parse a title with no recognised format.");
            return POUND_ERROR_UNSUPPORTED_FORMAT;
    }
}

error_t
pound_title_load(const pound_title_info_t *POUND_RESTRICT info)
{
    if (POUND_UNLIKELY(NULL == info) || POUND_UNLIKELY('\0' == info->path[0]))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting title load: the info has no path.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    fs_file_t file;
    error_t   result = fs_file_open(&file, info->path);
    if (POUND_SUCCESS != result)
    {
        return result;
    }

    fs_reader_t reader;
    result = fs_file_reader(&file, &reader);
    if (POUND_SUCCESS != result)
    {
        fs_file_close(&file);
        return result;
    }

    pound_title_info_t detected;
    result = pound_title_detect_reader(&reader, &detected);
    if (POUND_SUCCESS != result)
    {
        fs_file_close(&file);
        return result;
    }

    if (detected.type != info->type)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Title type changed between detection and load: %d != %d.",
                        (int)detected.type,
                        (int)info->type);
        fs_file_close(&file);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    pound_title_layout_t layout;
    result = pound_title_parse_reader(&reader, detected.type, &layout);
    fs_file_close(&file);

    if (POUND_SUCCESS == result)
    {
        POUND_LOG_INFO(&thread_logger,
                       "Title %s is a %s image: entry +0x%llX, image %llu bytes, bss %u bytes.",
                       info->path,
                       (POUND_TITLE_NRO == detected.type) ? "NRO" : "NSO",
                       (unsigned long long)layout.entry_offset,
                       (unsigned long long)layout.image_size,
                       layout.bss_size);
    }

    return result;
}

error_t
pound_title_loader_init(void)
{
    return POUND_SUCCESS;
}

/*** end of file ***/

//! Tests for title detection, NRO/NSO parsing, and the boot state machine.
//!
//! The images are built byte by byte in this file. They are deliberately not
//! real titles: what matters is that detection follows magic rather than a file
//! extension, that malformed sizes are refused before a host pointer is handed
//! to them, and that a successful boot maps code read/execute, data read/write,
//! and places SP at the top of the stack.

#include "pound_test.h"

#include "fs/file_reader.h"
#include "guest_memory.h"
#include "hos/memory_layout.h"
#include "loader/boot/boot.h"
#include "loader/title_loader.h"

#include <stdio.h>
#include <string.h>

#define TEST_IMAGE_SIZE   0x6000U
#define TEST_NRO_SIZE     0x5000U
#define TEST_NSO_SIZE     0x3100U
#define TEST_TEXT_SIZE    0x1000U
#define TEST_RODATA_OFF   0x1000U
#define TEST_DATA_OFF     0x2000U
#define TEST_BSS_SIZE     0x2000U
#define TEST_BOOT_BASE    UINT64_C(0x80000000)
#define TEST_BOOT_SIZE    UINT64_C(0x200000)
#define TEST_STACK_SIZE   UINT64_C(0x10000)

static unsigned g_loader_counter = 0U;

static void
put_le32(uint8_t *POUND_RESTRICT bytes, const size_t offset, const uint32_t value)
{
    bytes[offset + 0U] = (uint8_t)(value & 0xFFU);
    bytes[offset + 1U] = (uint8_t)((value >> 8U) & 0xFFU);
    bytes[offset + 2U] = (uint8_t)((value >> 16U) & 0xFFU);
    bytes[offset + 3U] = (uint8_t)((value >> 24U) & 0xFFU);
}

static void
make_path(char *POUND_RESTRICT path, const size_t capacity, const char *POUND_RESTRICT suffix)
{
    (void)snprintf(path, capacity, "compound_loader_%u_%s", g_loader_counter++, suffix);
}

static error_t
write_image(const char *POUND_RESTRICT path, const uint8_t *POUND_RESTRICT image, const size_t size)
{
    FILE *stream = fopen(path, "wb");
    if (NULL == stream)
    {
        return POUND_ERROR_IO;
    }

    const size_t written = fwrite(image, 1U, size, stream);
    const int    closed  = fclose(stream);

    if ((written != size) || (0 != closed))
    {
        (void)remove(path);
        return POUND_ERROR_IO;
    }

    return POUND_SUCCESS;
}

/// Builds an NRO with three page-sized segments and a 0x2000-byte BSS.
static void
build_nro(uint8_t *POUND_RESTRICT image, const uint32_t declared_size)
{
    memset(image, 0, TEST_IMAGE_SIZE);

    memcpy(image + 0x10U, "NRO0", 4U);
    put_le32(image, 0x14U, 0U);
    put_le32(image, 0x18U, declared_size);
    put_le32(image, 0x1CU, 0U);

    put_le32(image, 0x20U, 0U);             // text file offset
    put_le32(image, 0x24U, TEST_TEXT_SIZE); // text size
    put_le32(image, 0x28U, TEST_RODATA_OFF); // rodata file offset
    put_le32(image, 0x2CU, TEST_TEXT_SIZE); // rodata size
    put_le32(image, 0x30U, TEST_DATA_OFF);  // data file offset
    put_le32(image, 0x34U, TEST_TEXT_SIZE); // data size
    put_le32(image, 0x38U, TEST_BSS_SIZE);  // bss size

    // AArch64 NOP at the first instruction, so the entry can be checked by
    // reading the mapped bytes rather than only by comparing the PC.
    put_le32(image, 0U, 0xD503201FU);
}

/// Writes a raw LZ4 block that expands to `size` copies of `value` at `destination`.
///
/// The block is one literal followed by a match at offset one, which is the shape a
/// compressed run of identical bytes has and which exercises the decoder's
/// byte-at-a-time overlap copy rather than a `memcpy`.
static size_t
write_repeat_lz4(uint8_t *POUND_RESTRICT destination, const size_t size, const uint8_t value)
{
    if (size < 5U)
    {
        return 0U;
    }

    size_t cursor = 0U;
    destination[cursor++] = 0x1FU; // one literal; match length needs extensions
    destination[cursor++] = value;

    size_t extension = (size - 1U) - 4U - 15U;

    while (extension >= 255U)
    {
        destination[cursor++] = 0xFFU;
        extension -= 255U;
    }

    destination[cursor++] = (uint8_t)extension;
    destination[cursor++] = 0x01U;
    destination[cursor++] = 0x00U;

    return cursor;
}

/// Builds an NSO with page-aligned segments. A flag bit makes that segment one raw
/// LZ4 block whose expansion is a repeated byte, not the dummy header bytes.
static void
build_nso(uint8_t *POUND_RESTRICT image, const uint32_t flags)
{
    memset(image, 0, TEST_IMAGE_SIZE);

    memcpy(image, "NSO0", 4U);
    put_le32(image, 0x04U, 0U);
    put_le32(image, 0x08U, 0U);
    put_le32(image, 0x0CU, flags);

    put_le32(image, 0x10U, 0x100U);        // text file offset
    put_le32(image, 0x14U, 0U);            // text memory offset
    put_le32(image, 0x18U, TEST_TEXT_SIZE); // text size
    put_le32(image, 0x1CU, 0x100U);        // module name offset

    put_le32(image, 0x20U, 0x1100U);       // rodata file offset
    put_le32(image, 0x24U, TEST_RODATA_OFF); // rodata memory offset
    put_le32(image, 0x28U, TEST_TEXT_SIZE); // rodata size
    put_le32(image, 0x2CU, 0U);             // module name size

    put_le32(image, 0x30U, 0x2100U);       // data file offset
    put_le32(image, 0x34U, TEST_DATA_OFF); // data memory offset
    put_le32(image, 0x38U, TEST_TEXT_SIZE); // data size
    put_le32(image, 0x3CU, 0x1000U);       // bss size

    put_le32(image, 0x60U, TEST_TEXT_SIZE);
    put_le32(image, 0x64U, TEST_TEXT_SIZE);
    put_le32(image, 0x68U, TEST_TEXT_SIZE);

    put_le32(image, 0x100U, 0xD503201FU);

    if (0U != (flags & 1U))
    {
        const size_t stored = write_repeat_lz4(image + 0x100U, TEST_TEXT_SIZE, 'A');
        put_le32(image, 0x60U, (uint32_t)stored);
    }

    if (0U != (flags & 2U))
    {
        const size_t stored = write_repeat_lz4(image + 0x1100U, TEST_TEXT_SIZE, 'B');
        put_le32(image, 0x64U, (uint32_t)stored);
    }

    if (0U != (flags & 4U))
    {
        const size_t stored = write_repeat_lz4(image + 0x2100U, TEST_TEXT_SIZE, 'C');
        put_le32(image, 0x68U, (uint32_t)stored);
    }
}

static void
boot_config(pound_boot_config_t *POUND_RESTRICT config)
{
    config->guest_base         = TEST_BOOT_BASE;
    config->address_space_size = TEST_BOOT_SIZE;
    config->stack_size         = TEST_STACK_SIZE;
}

POUND_TEST(loader, detection_uses_content_not_extension)
{
    char path[128];
    make_path(path, sizeof(path), "header.bin");

    uint8_t image[TEST_IMAGE_SIZE];
    build_nro(image, TEST_NRO_SIZE);
    POUND_REQUIRE(POUND_SUCCESS == write_image(path, image, TEST_NRO_SIZE));

    pound_title_info_t info;
    POUND_REQUIRE(POUND_SUCCESS == pound_title_detect(path, &info));
    POUND_CHECK_EQ_I64(info.type, POUND_TITLE_NRO);
    POUND_CHECK(info.is_bootable);
    POUND_CHECK(!info.is_encrypted);
    POUND_CHECK(!info.requires_keys);
    POUND_CHECK(!info.is_switch2_compatible);
    POUND_CHECK_STR_EQ(info.path, path);

    (void)remove(path);

    make_path(path, sizeof(path), "not-really.nro");
    const char *text = "this is not an executable image";
    POUND_REQUIRE(POUND_SUCCESS == write_image(path, (const uint8_t *)text, strlen(text)));
    POUND_CHECK_EQ_I64(pound_title_detect(path, &info), POUND_ERROR_UNSUPPORTED_FORMAT);
    (void)remove(path);
}

POUND_TEST(loader, packages_are_recognised_but_never_reported_bootable)
{
    char path[128];
    make_path(path, sizeof(path), "package.nsp");

    uint8_t image[0x20];
    memset(image, 0, sizeof(image));
    memcpy(image, "PFS0", 4U);
    POUND_REQUIRE(POUND_SUCCESS == write_image(path, image, sizeof(image)));

    pound_title_info_t info;
    POUND_REQUIRE(POUND_SUCCESS == pound_title_detect(path, &info));
    POUND_CHECK_EQ_I64(info.type, POUND_TITLE_NSP);
    POUND_CHECK(!info.is_bootable);
    POUND_CHECK(info.requires_keys);
    POUND_CHECK(info.is_encrypted);
    POUND_CHECK(!info.is_switch2_compatible);

    POUND_CHECK_EQ_I64(pound_title_load(&info), POUND_ERROR_UNSUPPORTED_FEATURE);

    (void)remove(path);
}

POUND_TEST(loader, nro_boot_maps_text_data_bss_and_stack)
{
    char path[128];
    make_path(path, sizeof(path), "program.nro");

    uint8_t image[TEST_IMAGE_SIZE];
    build_nro(image, TEST_NRO_SIZE);
    POUND_REQUIRE(POUND_SUCCESS == write_image(path, image, TEST_NRO_SIZE));

    pound_title_info_t info;
    POUND_REQUIRE(POUND_SUCCESS == pound_title_detect(path, &info));

    pound_boot_config_t config;
    boot_config(&config);

    pound_boot_state_t state;
    POUND_REQUIRE(POUND_SUCCESS == pound_boot_init_with_config(&state, &config));
    POUND_REQUIRE(POUND_SUCCESS == pound_boot_load_title(&state, &info));

    POUND_CHECK_EQ_I64(state.phase, POUND_BOOT_PHASE_READY);
    POUND_CHECK_EQ_U64(state.guest_state.pc, TEST_BOOT_BASE);
    POUND_CHECK_EQ_U64(state.guest_state.x[31], TEST_BOOT_BASE + (TEST_BOOT_SIZE / 2U) - 16U);
    POUND_CHECK_EQ_U64(state.guest_state.x[31] & 0xFU, 0U);
    POUND_CHECK_EQ_U64(state.image_end, TEST_BOOT_BASE + TEST_NRO_SIZE);

    size_t executable_bytes = 0U;
    const uint8_t *code = guest_memory_translate_execute(&state.guest_memory,
                                                         state.guest_state.pc,
                                                         &executable_bytes);
    POUND_REQUIRE_PTR_NON_NULL(code);
    POUND_CHECK(executable_bytes >= 4U);
    POUND_CHECK_EQ_U64(code[0], 0x1FU);
    POUND_CHECK_EQ_U64(code[3], 0xD5U);

    // Text must not be writable.
    size_t writable_bytes = 0U;
    POUND_CHECK_PTR_NULL(guest_memory_translate_write(&state.guest_memory, state.guest_state.pc, &writable_bytes));

    // Data is writable and the copy landed at its memory offset.
    uint8_t *data = guest_memory_translate_write(&state.guest_memory, TEST_BOOT_BASE + TEST_DATA_OFF,
                                                 &writable_bytes);
    POUND_REQUIRE_PTR_NON_NULL(data);
    POUND_CHECK(writable_bytes >= 4U);
    data[0] = 0xABU;
    POUND_CHECK_EQ_U64(data[0], 0xABU);

    const guest_memory_region_t *text_region = hos_memory_layout_find(&state.memory_layout, TEST_BOOT_BASE);
    POUND_REQUIRE_PTR_NON_NULL(text_region);
    POUND_CHECK_EQ_U64(text_region->state, HOS_MEMORY_STATE_CODE);
    POUND_CHECK_EQ_U64(text_region->permission, GUEST_MEMORY_PERM_READ | GUEST_MEMORY_PERM_EXEC);

    const guest_memory_region_t *stack_region
        = hos_memory_layout_find(&state.memory_layout, state.stack_base + 0x100U);
    POUND_REQUIRE_PTR_NON_NULL(stack_region);
    POUND_CHECK_EQ_U64(stack_region->state, HOS_MEMORY_STATE_STACK);

    POUND_REQUIRE(POUND_SUCCESS == pound_boot_start(&state));
    POUND_CHECK(pound_boot_is_running(&state));
    POUND_CHECK_EQ_I64(pound_boot_start(&state), POUND_ERROR_INVALID_STATE);

    pound_boot_shutdown(&state);
    POUND_CHECK(!state.initialized);
    POUND_CHECK_PTR_NULL(state.host_base);
    POUND_CHECK(!pound_boot_is_running(&state));

    (void)remove(path);
}

POUND_TEST(loader, nso_boot_copies_segments_at_their_memory_offsets)
{
    char path[128];
    make_path(path, sizeof(path), "program.nso");

    uint8_t image[TEST_IMAGE_SIZE];
    build_nso(image, 0U);
    POUND_REQUIRE(POUND_SUCCESS == write_image(path, image, TEST_NSO_SIZE));

    pound_title_info_t info;
    POUND_REQUIRE(POUND_SUCCESS == pound_title_detect(path, &info));
    POUND_CHECK_EQ_I64(info.type, POUND_TITLE_NSO);

    pound_boot_config_t config;
    boot_config(&config);

    pound_boot_state_t state;
    POUND_REQUIRE(POUND_SUCCESS == pound_boot_init_with_config(&state, &config));
    POUND_REQUIRE(POUND_SUCCESS == pound_boot_load_title(&state, &info));

    POUND_CHECK_EQ_U64(state.guest_state.pc, TEST_BOOT_BASE);
    POUND_CHECK_EQ_U64(state.image_end, TEST_BOOT_BASE + 0x4000U);

    size_t code_bytes = 0U;
    const uint8_t *code = guest_memory_translate_execute(&state.guest_memory, TEST_BOOT_BASE, &code_bytes);
    POUND_REQUIRE_PTR_NON_NULL(code);
    POUND_CHECK_EQ_U64(code[0], 0x1FU);
    POUND_CHECK_EQ_U64(code[3], 0xD5U);

    const guest_state_t *guest_state = pound_boot_get_guest_state(&state);
    POUND_REQUIRE_PTR_NON_NULL(guest_state);
    POUND_CHECK_EQ_U64(guest_state->pc, TEST_BOOT_BASE);

    pound_boot_shutdown(&state);
    (void)remove(path);
}

POUND_TEST(loader, compressed_nso_expands_its_segments)
{
    char path[128];
    make_path(path, sizeof(path), "compressed.nso");

    uint8_t image[TEST_IMAGE_SIZE];
    build_nso(image, 0x1U);
    POUND_REQUIRE(POUND_SUCCESS == write_image(path, image, TEST_NSO_SIZE));

    pound_title_info_t info;
    POUND_REQUIRE(POUND_SUCCESS == pound_title_detect(path, &info));
    POUND_CHECK_EQ_I64(info.type, POUND_TITLE_NSO);

    fs_file_t file;
    POUND_REQUIRE(POUND_SUCCESS == fs_file_open(&file, path));
    fs_reader_t reader;
    POUND_REQUIRE(POUND_SUCCESS == fs_file_reader(&file, &reader));

    pound_title_layout_t layout;
    POUND_REQUIRE(POUND_SUCCESS == pound_title_parse_reader(&reader, POUND_TITLE_NSO, &layout));
    POUND_CHECK(layout.segments[0].compressed);
    POUND_CHECK_EQ_U64(layout.segments[0].size, TEST_TEXT_SIZE);
    POUND_CHECK(layout.segments[0].stored_size < layout.segments[0].size);
    fs_file_close(&file);

    pound_boot_config_t config;
    boot_config(&config);
    pound_boot_state_t state;
    POUND_REQUIRE(POUND_SUCCESS == pound_boot_init_with_config(&state, &config));
    POUND_REQUIRE(POUND_SUCCESS == pound_boot_load_title(&state, &info));

    size_t code_bytes = 0U;
    const uint8_t *code = guest_memory_translate_execute(&state.guest_memory, TEST_BOOT_BASE, &code_bytes);
    POUND_REQUIRE_PTR_NON_NULL(code);
    POUND_CHECK(code_bytes >= TEST_TEXT_SIZE);
    POUND_CHECK_EQ_U64(code[0], 'A');
    POUND_CHECK_EQ_U64(code[TEST_TEXT_SIZE - 1U], 'A');

    pound_boot_shutdown(&state);
    (void)remove(path);
}

POUND_TEST(loader, a_corrupt_compressed_segment_fails_the_boot)
{
    char path[128];
    make_path(path, sizeof(path), "corrupt-compressed.nso");

    uint8_t image[TEST_IMAGE_SIZE];
    build_nso(image, 0x1U);

    // Keep the compressed flag and stored length but break the match offset, so the
    // failure comes from the decoder rather than from the layout checks.
    image[0x104U] = 0x00U;

    POUND_REQUIRE(POUND_SUCCESS == write_image(path, image, TEST_NSO_SIZE));

    pound_title_info_t info;
    POUND_REQUIRE(POUND_SUCCESS == pound_title_detect(path, &info));

    pound_boot_config_t config;
    boot_config(&config);
    pound_boot_state_t state;
    POUND_REQUIRE(POUND_SUCCESS == pound_boot_init_with_config(&state, &config));
    POUND_CHECK_EQ_I64(pound_boot_load_title(&state, &info), POUND_ERROR_CORRUPTED);
    POUND_CHECK_PTR_NULL(state.host_base);
    pound_boot_shutdown(&state);

    (void)remove(path);
}

POUND_TEST(loader, malformed_nro_size_is_refused_before_mapping)
{
    char path[128];
    make_path(path, sizeof(path), "oversized.nro");

    uint8_t image[TEST_IMAGE_SIZE];
    build_nro(image, TEST_NRO_SIZE + 0x1000U);
    POUND_REQUIRE(POUND_SUCCESS == write_image(path, image, TEST_NRO_SIZE));

    pound_title_info_t info;
    POUND_REQUIRE(POUND_SUCCESS == pound_title_detect(path, &info));

    pound_boot_config_t config;
    boot_config(&config);
    pound_boot_state_t state;
    POUND_REQUIRE(POUND_SUCCESS == pound_boot_init_with_config(&state, &config));
    POUND_CHECK_EQ_I64(pound_boot_load_title(&state, &info), POUND_ERROR_MALFORMED_HEADER);
    POUND_CHECK_PTR_NULL(state.host_base);

    pound_boot_shutdown(&state);
    (void)remove(path);
}

POUND_TEST(loader, boot_start_requires_a_loaded_title)
{
    pound_boot_state_t state;
    POUND_REQUIRE(POUND_SUCCESS == pound_boot_init(&state));
    POUND_CHECK_EQ_I64(pound_boot_start(&state), POUND_ERROR_INVALID_STATE);
    POUND_CHECK(!pound_boot_is_running(&state));
    pound_boot_shutdown(&state);
}

POUND_TEST_SUITE(loader,
                 POUND_TEST_CASE(loader, detection_uses_content_not_extension),
                 POUND_TEST_CASE(loader, packages_are_recognised_but_never_reported_bootable),
                 POUND_TEST_CASE(loader, nro_boot_maps_text_data_bss_and_stack),
                 POUND_TEST_CASE(loader, nso_boot_copies_segments_at_their_memory_offsets),
                 POUND_TEST_CASE(loader, compressed_nso_expands_its_segments),
                 POUND_TEST_CASE(loader, a_corrupt_compressed_segment_fails_the_boot),
                 POUND_TEST_CASE(loader, malformed_nro_size_is_refused_before_mapping),
                 POUND_TEST_CASE(loader, boot_start_requires_a_loaded_title))

//! Unit tests for `guest_memory.h`.
//!
//! `guest_memory_t` is the sole translation layer between guest virtual
//! addresses (the Nintendo Switch 2's Horizon OS address space) and host
//! pointers. Every JIT backend and every system module reads through it, so a
//! bounds mistake here is a guest-controlled out-of-bounds access on the host.

#include "pound_test.h"

#include "guest_memory.h"
#include <string.h>

/// A realistic Nintendo Switch 2 application address base, used here purely as
/// a test input.
#define GUEST_TEST_APP_BASE  UINT64_C(0x0000000100000000)
#define GUEST_TEST_HOST_SIZE 4096U

/// The 16-byte alignment requirement of `guest_memory_init` means the backing
/// store cannot be an ordinary char array.
static _Alignas(64) uint8_t g_host_buffer[GUEST_TEST_HOST_SIZE];

/// Initialises `region` over `g_host_buffer` and reports whether it succeeded.
///
/// Returns an `error_t` rather than using POUND_REQUIRE, because that macro
/// performs a `void` return and this helper is not `void`.
static error_t
init_region(guest_memory_t *region, const uint64_t guest_base)
{
    memset(g_host_buffer, 0, sizeof(g_host_buffer));
    return guest_memory_init(region, g_host_buffer, GUEST_TEST_HOST_SIZE, guest_base);
}

// -----------------------------------------------------------------------------
// guest_memory_init
// -----------------------------------------------------------------------------

POUND_TEST(guest_memory, init_rejects_invalid_arguments)
{
    guest_memory_t region = { 0 };

    pound_test_log_reset();

    POUND_CHECK_EQ_I64(guest_memory_init(NULL, g_host_buffer, GUEST_TEST_HOST_SIZE, 0U),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(guest_memory_init(&region, NULL, GUEST_TEST_HOST_SIZE, 0U),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(guest_memory_init(&region, g_host_buffer, 0U, 0U),
                       POUND_ERROR_INVALID_ARGUMENT);

    // A typed error code alone would leave the caller unable to tell which of
    // the three arguments was at fault, so each rejection is also reported.
    POUND_CHECK_EQ_U64(pound_test_log_count_at(LOG_LEVEL_ERROR), 3U);
    POUND_CHECK(pound_test_log_contains("Rejecting guest memory region"));
}

POUND_TEST(guest_memory, init_rejects_a_misaligned_host_base)
{
    guest_memory_t region = { 0 };

    // g_host_buffer is 64-byte aligned, so +1 is guaranteed to break the
    // 16-byte contract without depending on allocator behaviour.
    uint8_t *misaligned = g_host_buffer + 1U;

    POUND_CHECK_EQ_I64(guest_memory_init(&region, misaligned, GUEST_TEST_HOST_SIZE, 0U),
                       POUND_ERROR_MEMORY_ALIGNMENT);

    // ... and an alignment that *is* valid must still be accepted.
    POUND_CHECK_EQ_I64(
        guest_memory_init(&region, g_host_buffer + 16U, GUEST_TEST_HOST_SIZE - 16U, 0U),
        POUND_SUCCESS);
}

POUND_TEST(guest_memory, init_rejects_a_guest_range_that_wraps)
{
    guest_memory_t region = { 0 };

    // guest_base + host_size must not exceed UINT64_MAX.
    POUND_CHECK_EQ_I64(
        guest_memory_init(&region, g_host_buffer, GUEST_TEST_HOST_SIZE, UINT64_MAX),
        POUND_ERROR_GUEST_ADDRESS_OVERFLOW);

    // The largest base that still leaves room for the whole window: `guest_end`
    // is `guest_base + host_size`, so one more than this would wrap `guest_end`
    // to zero and make the window look empty.
    POUND_CHECK_EQ_I64(guest_memory_init(&region,
                                        g_host_buffer,
                                        GUEST_TEST_HOST_SIZE,
                                        UINT64_MAX - GUEST_TEST_HOST_SIZE),
                       POUND_SUCCESS);
    POUND_CHECK_EQ_U64(region.guest_end, UINT64_MAX);
}

POUND_TEST(guest_memory, init_populates_the_window)
{
    guest_memory_t region = { 0 };

    POUND_REQUIRE(POUND_SUCCESS == init_region(&region, GUEST_TEST_APP_BASE));

    POUND_CHECK_PTR_EQ(region.host_base, g_host_buffer);
    POUND_CHECK_EQ_U64(region.host_size, GUEST_TEST_HOST_SIZE);
    POUND_CHECK_EQ_U64(region.guest_base, GUEST_TEST_APP_BASE);
    POUND_CHECK_EQ_U64(region.guest_end, GUEST_TEST_APP_BASE + GUEST_TEST_HOST_SIZE);
}

// -----------------------------------------------------------------------------
// guest_memory_contains
// -----------------------------------------------------------------------------

POUND_TEST(guest_memory, contains_is_half_open)
{
    guest_memory_t region = { 0 };
    POUND_REQUIRE(POUND_SUCCESS == init_region(&region, GUEST_TEST_APP_BASE));

    POUND_CHECK(guest_memory_contains(&region, GUEST_TEST_APP_BASE));
    POUND_CHECK(guest_memory_contains(&region, GUEST_TEST_APP_BASE + 1U));
    POUND_CHECK(guest_memory_contains(&region, region.guest_end - 1U));

    // Half-open: the end address itself is out.
    POUND_CHECK(false == guest_memory_contains(&region, region.guest_end));
    POUND_CHECK(false == guest_memory_contains(&region, region.guest_end + 1U));
    POUND_CHECK(false == guest_memory_contains(&region, GUEST_TEST_APP_BASE - 1U));
    POUND_CHECK(false == guest_memory_contains(&region, 0U));
}

POUND_TEST(guest_memory, contains_rejects_null)
{
    pound_test_log_reset();

    POUND_CHECK(false == guest_memory_contains(NULL, GUEST_TEST_APP_BASE));
    POUND_CHECK_EQ_U64(pound_test_log_count_at(LOG_LEVEL_ERROR), 1U);
    POUND_CHECK(pound_test_log_contains("region is NULL"));
}

POUND_TEST(guest_memory, contains_does_not_wrap_a_high_window)
{
    // A region whose window touches the top of the address space must not
    // accidentally admit low addresses through unsigned wraparound.
    guest_memory_t region = { 0 };
    POUND_REQUIRE(POUND_SUCCESS
                  == guest_memory_init(&region,
                                       g_host_buffer,
                                       GUEST_TEST_HOST_SIZE,
                                       UINT64_MAX - GUEST_TEST_HOST_SIZE));

    POUND_CHECK(guest_memory_contains(&region, UINT64_MAX - GUEST_TEST_HOST_SIZE));

    // guest_end is UINT64_MAX itself, and the window is half-open, so the very
    // top of the address space is one past the last mapped byte.
    POUND_CHECK(guest_memory_contains(&region, UINT64_MAX - 1U));
    POUND_CHECK(false == guest_memory_contains(&region, UINT64_MAX));

    POUND_CHECK(false == guest_memory_contains(&region, 0U));
    POUND_CHECK(false == guest_memory_contains(&region, UINT64_MAX - GUEST_TEST_HOST_SIZE - 1U));
}

// -----------------------------------------------------------------------------
// Translation
// -----------------------------------------------------------------------------

POUND_TEST(guest_memory, translate_read_reports_the_remaining_span)
{
    guest_memory_t region = { 0 };
    POUND_REQUIRE(POUND_SUCCESS == init_region(&region, GUEST_TEST_APP_BASE));

    size_t               readable = 0U;
    const uint8_t *const host    = guest_memory_translate_read(&region,
                                                           GUEST_TEST_APP_BASE,
                                                           &readable);

    POUND_CHECK_PTR_EQ(host, g_host_buffer);
    POUND_CHECK_EQ_U64(readable, GUEST_TEST_HOST_SIZE);

    const uint8_t *const offset_host
        = guest_memory_translate_read(&region, GUEST_TEST_APP_BASE + 16U, &readable);
    POUND_CHECK_PTR_EQ(offset_host, g_host_buffer + 16U);
    POUND_CHECK_EQ_U64(readable, GUEST_TEST_HOST_SIZE - 16U);

    // The final byte leaves exactly one byte readable.
    const uint8_t *const last_host
        = guest_memory_translate_read(&region, region.guest_end - 1U, &readable);
    POUND_CHECK_PTR_EQ(last_host, g_host_buffer + GUEST_TEST_HOST_SIZE - 1U);
    POUND_CHECK_EQ_U64(readable, 1U);
}

POUND_TEST(guest_memory, translate_write_reports_the_remaining_span)
{
    guest_memory_t region = { 0 };
    POUND_REQUIRE(POUND_SUCCESS == init_region(&region, GUEST_TEST_APP_BASE));

    size_t         writable = 0U;
    uint8_t *const host    = guest_memory_translate_write(&region, GUEST_TEST_APP_BASE + 8U,
                                                     &writable);

    POUND_CHECK_PTR_EQ(host, g_host_buffer + 8U);
    POUND_CHECK_EQ_U64(writable, GUEST_TEST_HOST_SIZE - 8U);
}

POUND_TEST(guest_memory, translation_reaches_the_expected_host_bytes)
{
    guest_memory_t region = { 0 };
    POUND_REQUIRE(POUND_SUCCESS == init_region(&region, GUEST_TEST_APP_BASE));

    size_t         writable = 0U;
    uint8_t *const host    = guest_memory_translate_write(&region, GUEST_TEST_APP_BASE + 32U,
                                                     &writable);

    POUND_REQUIRE(NULL != host);
    POUND_REQUIRE(writable >= 4U);

    host[0] = 0xDE;
    host[1] = 0xAD;
    host[2] = 0xBE;
    host[3] = 0xEF;

    // The write must be visible through the host buffer itself.
    POUND_CHECK_EQ_U64(g_host_buffer[32], 0xDE);
    POUND_CHECK_EQ_U64(g_host_buffer[33], 0xAD);
    POUND_CHECK_EQ_U64(g_host_buffer[34], 0xBE);
    POUND_CHECK_EQ_U64(g_host_buffer[35], 0xEF);

    size_t               readable = 0U;
    const uint8_t *const reader
        = guest_memory_translate_read(&region, GUEST_TEST_APP_BASE + 32U, &readable);

    POUND_REQUIRE(NULL != reader);
    POUND_CHECK_EQ_U64(reader[3], 0xEF);
}

POUND_TEST(guest_memory, translation_rejects_out_of_window_addresses)
{
    guest_memory_t region = { 0 };
    POUND_REQUIRE(POUND_SUCCESS == init_region(&region, GUEST_TEST_APP_BASE));

    pound_test_log_reset();

    size_t readable = 0U;
    POUND_CHECK_PTR_NULL(guest_memory_translate_read(&region, region.guest_end, &readable));
    POUND_CHECK_PTR_NULL(guest_memory_translate_read(&region, 0U, &readable));

    size_t writable = 0U;
    POUND_CHECK_PTR_NULL(guest_memory_translate_write(&region, region.guest_end, &writable));
    POUND_CHECK_PTR_NULL(guest_memory_translate_write(&region, 0U, &writable));

    // A guest-controlled address falling outside the window is the single most
    // security-relevant event in this module, so each refusal names the
    // address and the window it missed.
    POUND_CHECK_EQ_U64(pound_test_log_count_at(LOG_LEVEL_ERROR), 4U);
    POUND_CHECK(pound_test_log_contains("Read fault"));
    POUND_CHECK(pound_test_log_contains("Write fault"));
}

POUND_TEST(guest_memory, translation_rejects_null_arguments)
{
    guest_memory_t region = { 0 };
    POUND_REQUIRE(POUND_SUCCESS == init_region(&region, GUEST_TEST_APP_BASE));

    pound_test_log_reset();

    size_t readable = 0U;
    POUND_CHECK_PTR_NULL(guest_memory_translate_read(NULL, GUEST_TEST_APP_BASE, &readable));
    POUND_CHECK_PTR_NULL(guest_memory_translate_read(&region, GUEST_TEST_APP_BASE, NULL));
    POUND_CHECK_PTR_NULL(guest_memory_translate_read(NULL, GUEST_TEST_APP_BASE, NULL));

    size_t writable = 0U;
    POUND_CHECK_PTR_NULL(guest_memory_translate_write(NULL, GUEST_TEST_APP_BASE, &writable));
    POUND_CHECK_PTR_NULL(guest_memory_translate_write(&region, GUEST_TEST_APP_BASE, NULL));
    POUND_CHECK_PTR_NULL(guest_memory_translate_write(NULL, GUEST_TEST_APP_BASE, NULL));

    POUND_CHECK_EQ_U64(pound_test_log_count_at(LOG_LEVEL_ERROR), 6U);
    POUND_CHECK(pound_test_log_contains("Rejecting read translation"));
    POUND_CHECK(pound_test_log_contains("Rejecting write translation"));
}

POUND_TEST_SUITE(guest_memory,
    POUND_TEST_CASE(guest_memory, init_rejects_invalid_arguments),
    POUND_TEST_CASE(guest_memory, init_rejects_a_misaligned_host_base),
    POUND_TEST_CASE(guest_memory, init_rejects_a_guest_range_that_wraps),
    POUND_TEST_CASE(guest_memory, init_populates_the_window),
    POUND_TEST_CASE(guest_memory, contains_is_half_open),
    POUND_TEST_CASE(guest_memory, contains_rejects_null),
    POUND_TEST_CASE(guest_memory, contains_does_not_wrap_a_high_window),
    POUND_TEST_CASE(guest_memory, translate_read_reports_the_remaining_span),
    POUND_TEST_CASE(guest_memory, translate_write_reports_the_remaining_span),
    POUND_TEST_CASE(guest_memory, translation_reaches_the_expected_host_bytes),
    POUND_TEST_CASE(guest_memory, translation_rejects_out_of_window_addresses),
    POUND_TEST_CASE(guest_memory, translation_rejects_null_arguments))
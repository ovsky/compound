//! Tests for host lifecycle, timing, and capability reporting.

#include "pound_test.h"

#include "platform/services.h"

POUND_TEST(platform_services, init_reports_a_clean_service_set)
{
    pound_platform_services_t services;
    POUND_REQUIRE(POUND_SUCCESS == pound_platform_services_init(&services, 1000U, 60U));

    POUND_CHECK(services.initialized);
    POUND_CHECK(!pound_platform_services_should_shutdown(&services));
    POUND_CHECK_EQ_U64(services.target_fps, 60U);
    POUND_CHECK_EQ_U64(services.gamepad_count, 0U);
    POUND_CHECK_EQ_U64(services.frame_count, 0U);
    POUND_CHECK_EQ_U64(services.start_ms, 1000U);
    POUND_CHECK_EQ_U64(services.last_frame_ms, 1000U);
    POUND_CHECK_EQ_U64(pound_platform_services_frame_interval_ms(&services), 17U);

    for (int capability = 0; capability < POUND_PLATFORM_CAPABILITY_COUNT; ++capability)
    {
        POUND_CHECK(!pound_platform_services_has_capability(&services,
                                                           (pound_platform_capability_t)capability));
    }

    pound_platform_services_shutdown(&services);
    POUND_CHECK(!services.initialized);
}

POUND_TEST(platform_services, invalid_init_arguments_are_rejected)
{
    pound_platform_services_t services;

    POUND_CHECK_EQ_I64(pound_platform_services_init(NULL, 0U, 60U), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(pound_platform_services_init(&services, 0U, 0U), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_I64(pound_platform_services_init(&services, 0U, 1001U),
                       POUND_ERROR_INVALID_ARGUMENT);
}

POUND_TEST(platform_services, capabilities_are_set_only_when_valid_and_initialized)
{
    pound_platform_services_t services;
    POUND_REQUIRE(POUND_SUCCESS == pound_platform_services_init(&services, 0U, 60U));

    POUND_REQUIRE(POUND_SUCCESS == pound_platform_services_set_capability(
                                      &services, POUND_PLATFORM_CAPABILITY_AUDIO_OUTPUT, true));
    POUND_CHECK(pound_platform_services_has_capability(&services,
                                                       POUND_PLATFORM_CAPABILITY_AUDIO_OUTPUT));
    POUND_CHECK(!pound_platform_services_has_capability(&services,
                                                        POUND_PLATFORM_CAPABILITY_KEYBOARD));

    POUND_REQUIRE(POUND_SUCCESS == pound_platform_services_set_capability(
                                      &services, POUND_PLATFORM_CAPABILITY_AUDIO_OUTPUT, false));
    POUND_CHECK(!pound_platform_services_has_capability(&services,
                                                        POUND_PLATFORM_CAPABILITY_AUDIO_OUTPUT));

    POUND_CHECK_EQ_I64(pound_platform_services_set_capability(
                           &services, POUND_PLATFORM_CAPABILITY_COUNT, true),
                       POUND_ERROR_INVALID_ARGUMENT);

    pound_platform_services_shutdown(&services);
    POUND_CHECK_EQ_I64(pound_platform_services_set_capability(
                           &services, POUND_PLATFORM_CAPABILITY_GAMEPADS, true),
                       POUND_ERROR_NOT_INITIALIZED);
}

POUND_TEST(platform_services, gamepad_count_is_bounded)
{
    pound_platform_services_t services;
    POUND_REQUIRE(POUND_SUCCESS == pound_platform_services_init(&services, 0U, 60U));

    POUND_REQUIRE(POUND_SUCCESS == pound_platform_services_set_gamepad_count(&services, 2U));
    POUND_CHECK_EQ_U64(services.gamepad_count, 2U);

    POUND_REQUIRE(POUND_SUCCESS == pound_platform_services_set_gamepad_count(&services, 0U));
    POUND_CHECK_EQ_U64(services.gamepad_count, 0U);

    POUND_CHECK_EQ_I64(pound_platform_services_set_gamepad_count(
                           &services, POUND_PLATFORM_MAX_GAMEPADS + 1U),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(services.gamepad_count, 0U);

    pound_platform_services_shutdown(&services);
}

POUND_TEST(platform_services, ticks_are_monotonic_and_counted)
{
    pound_platform_services_t services;
    POUND_REQUIRE(POUND_SUCCESS == pound_platform_services_init(&services, 100U, 30U));

    POUND_CHECK_EQ_U64(pound_platform_services_elapsed_ms(&services, 250U), 150U);
    POUND_CHECK_EQ_U64(pound_platform_services_elapsed_ms(&services, 99U), 0U);

    POUND_REQUIRE(POUND_SUCCESS == pound_platform_services_tick(&services, 100U));
    POUND_REQUIRE(POUND_SUCCESS == pound_platform_services_tick(&services, 133U));
    POUND_CHECK_EQ_U64(services.frame_count, 2U);
    POUND_CHECK_EQ_U64(services.last_frame_ms, 133U);

    POUND_CHECK_EQ_I64(pound_platform_services_tick(&services, 132U), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(services.frame_count, 2U);
    POUND_CHECK_EQ_U64(services.last_frame_ms, 133U);

    POUND_CHECK_EQ_U64(pound_platform_services_frame_interval_ms(&services), 34U);

    pound_platform_services_shutdown(&services);
    POUND_CHECK_EQ_I64(pound_platform_services_tick(&services, 200U), POUND_ERROR_NOT_INITIALIZED);
}

POUND_TEST(platform_services, shutdown_request_is_idempotent)
{
    pound_platform_services_t services;
    POUND_REQUIRE(POUND_SUCCESS == pound_platform_services_init(&services, 0U, 60U));

    POUND_CHECK(!pound_platform_services_should_shutdown(&services));
    pound_platform_services_request_shutdown(&services);
    pound_platform_services_request_shutdown(&services);
    POUND_CHECK(pound_platform_services_should_shutdown(&services));

    pound_platform_services_shutdown(&services);
    POUND_CHECK(!pound_platform_services_should_shutdown(&services));
}

POUND_TEST_SUITE(platform_services,
                 POUND_TEST_CASE(platform_services, init_reports_a_clean_service_set),
                 POUND_TEST_CASE(platform_services, invalid_init_arguments_are_rejected),
                 POUND_TEST_CASE(platform_services, capabilities_are_set_only_when_valid_and_initialized),
                 POUND_TEST_CASE(platform_services, gamepad_count_is_bounded),
                 POUND_TEST_CASE(platform_services, ticks_are_monotonic_and_counted),
                 POUND_TEST_CASE(platform_services, shutdown_request_is_idempotent))

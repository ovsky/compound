#include "platform/services.h"

#include "log.h"

#include <string.h>

static bool
capability_is_valid(const pound_platform_capability_t capability)
{
    return ((int)capability >= 0) && (capability < POUND_PLATFORM_CAPABILITY_COUNT);
}

error_t
pound_platform_services_init(pound_platform_services_t *POUND_RESTRICT services,
                             const uint64_t                              now_ms,
                             const uint32_t                              target_fps)
{
    if (POUND_UNLIKELY(NULL == services))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting platform init: services is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    memset(services, 0, sizeof(*services));

    if ((target_fps < 1U) || (target_fps > 1000U))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting platform init: target FPS %u is out of range.", target_fps);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    services->initialized    = true;
    services->target_fps     = target_fps;
    services->start_ms       = now_ms;
    services->last_frame_ms  = now_ms;
    services->frame_count    = 0U;
    services->gamepad_count  = 0U;
    services->shutdown_requested = false;
    memset(services->capabilities, 0, sizeof(services->capabilities));

    POUND_LOG_INFO(&thread_logger, "Platform services ready: target %u FPS at %llu ms.", target_fps,
                   (unsigned long long)now_ms);
    return POUND_SUCCESS;
}

void
pound_platform_services_shutdown(pound_platform_services_t *POUND_RESTRICT services)
{
    if (NULL != services)
    {
        memset(services, 0, sizeof(*services));
    }
}

error_t
pound_platform_services_set_capability(pound_platform_services_t *POUND_RESTRICT services,
                                       const pound_platform_capability_t         capability,
                                       const bool                                available)
{
    if (POUND_UNLIKELY(NULL == services) || POUND_UNLIKELY(!services->initialized))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting capability update: the service set is not initialized.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    if (!capability_is_valid(capability))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting capability update: %d is out of range.", (int)capability);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    services->capabilities[capability] = available;
    return POUND_SUCCESS;
}

bool
pound_platform_services_has_capability(const pound_platform_services_t *POUND_RESTRICT services,
                                       const pound_platform_capability_t         capability)
{
    if ((NULL == services) || !services->initialized || !capability_is_valid(capability))
    {
        return false;
    }

    return services->capabilities[capability];
}

error_t
pound_platform_services_set_gamepad_count(pound_platform_services_t *POUND_RESTRICT services,
                                           const uint32_t                              count)
{
    if (POUND_UNLIKELY(NULL == services) || POUND_UNLIKELY(!services->initialized))
    {
        return POUND_ERROR_NOT_INITIALIZED;
    }

    if (count > POUND_PLATFORM_MAX_GAMEPADS)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting gamepad count %u: the limit is %u.",
                        count,
                        POUND_PLATFORM_MAX_GAMEPADS);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    services->gamepad_count = count;
    return POUND_SUCCESS;
}

error_t
pound_platform_services_tick(pound_platform_services_t *POUND_RESTRICT services, const uint64_t now_ms)
{
    if (POUND_UNLIKELY(NULL == services) || POUND_UNLIKELY(!services->initialized))
    {
        return POUND_ERROR_NOT_INITIALIZED;
    }

    if (now_ms < services->last_frame_ms)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting frame timestamp %llu: the last frame was at %llu.",
                        (unsigned long long)now_ms,
                        (unsigned long long)services->last_frame_ms);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    services->last_frame_ms = now_ms;
    ++services->frame_count;
    return POUND_SUCCESS;
}

uint64_t
pound_platform_services_elapsed_ms(const pound_platform_services_t *POUND_RESTRICT services,
                                   const uint64_t                              now_ms)
{
    if ((NULL == services) || !services->initialized || (now_ms < services->start_ms))
    {
        return 0U;
    }

    return now_ms - services->start_ms;
}

uint32_t
pound_platform_services_frame_interval_ms(const pound_platform_services_t *POUND_RESTRICT services)
{
    if ((NULL == services) || !services->initialized || (0U == services->target_fps))
    {
        return 0U;
    }

    return (1000U + services->target_fps - 1U) / services->target_fps;
}

void
pound_platform_services_request_shutdown(pound_platform_services_t *POUND_RESTRICT services)
{
    if ((NULL != services) && services->initialized)
    {
        services->shutdown_requested = true;
    }
}

bool
pound_platform_services_should_shutdown(const pound_platform_services_t *POUND_RESTRICT services)
{
    return (NULL != services) && services->initialized && services->shutdown_requested;
}

/*** end of file ***/

#include "gui_layout.h"
#include "log.h"
#include "safe_math.h"
#include <math.h>

static bool is_region_valid(gui_layout_region_t dock);
static bool is_dimensions_valid(float width, float height);
static bool is_rectangle_valid(const gui_layout_rectangle_t *rectangle);

gui_plugin_error_t
gui_layout_add_panel(gui_layout_t *layout, const gui_layout_region_t region, const float fraction)
{
    if (POUND_UNLIKELY(NULL == layout))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: layout is NULL.");
        return GUI_PLUGIN_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(false == is_region_valid(region)))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: region %d is invalid.", (int)region);
        return GUI_PLUGIN_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY((false == isfinite(fraction)) || (fraction < 0.0F) || (fraction > 1.0F)))
    {
        POUND_LOG_ERROR(
            &thread_logger, "Aborting function: fraction %f is outside [0, 1].", fraction);
        return GUI_PLUGIN_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY((layout->panel_count < 0)
                       || (layout->panel_count > GUI_LAYOUT_PANEL_CAPACITY)))
    {
        POUND_LOG_ERROR(
            &thread_logger, "Aborting function: panel_count %d is corrupted.", layout->panel_count);
        return GUI_PLUGIN_ERROR_INVALID_ARGUMENT;
    }

    int                     next_count = 0;
    const safe_math_error_t math_error = safe_math_add_i32(layout->panel_count, 1, &next_count);

    if (POUND_UNLIKELY(POUND_MATH_SUCCESS != math_error))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: panel count overflow because %s.",
                        safe_math_error_string(math_error));
        return GUI_PLUGIN_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(next_count > GUI_LAYOUT_PANEL_CAPACITY))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: panel capacity (%d) exhausted.",
                        GUI_LAYOUT_PANEL_CAPACITY);
        return GUI_PLUGIN_ERROR_PANEL_REGISTRY_FULL;
    }

    const int index              = layout->panel_count;
    layout->regions[index]       = region;
    layout->fractions[index]     = fraction;
    layout->panels[index].x      = 0.0f;
    layout->panels[index].y      = 0.0f;
    layout->panels[index].width  = 0.0f;
    layout->panels[index].height = 0.0f;
    layout->panel_count          = next_count;
    layout->is_computed          = false;

    return GUI_PLUGIN_SUCCESS;
}

gui_plugin_error_t
gui_layout_compute(gui_layout_t           *layout,
                   float                   screen_width,
                   float                   screen_height,
                   gui_layout_rectangle_t *out_viewport)
{
    if (POUND_UNLIKELY(NULL == layout))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: layout is NULL.");
        return GUI_PLUGIN_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(NULL == out_viewport))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: out_viewport is NULL.");
        return GUI_PLUGIN_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(false == is_dimensions_valid(screen_width, screen_height)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: screen size (%f x %f) must be finite and "
                        "non-negative.",
                        screen_width,
                        screen_height);
        return GUI_PLUGIN_ERROR_INVALID_ARGUMENT;
    }

    const int count = layout->panel_count;

    if (POUND_UNLIKELY((count < 0) || (count > GUI_LAYOUT_PANEL_CAPACITY)))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: panel_count %d is corrupted.", count);
        return GUI_PLUGIN_ERROR_INVALID_ARGUMENT;
    }

    if (0 == count)
    {
        // No docked panels: the main viewport is the entire screen.
        //
        out_viewport->x      = 0.0f;
        out_viewport->y      = 0.0f;
        out_viewport->width  = screen_width;
        out_viewport->height = screen_height;
        layout->is_computed  = true;
        return GUI_PLUGIN_SUCCESS;
    }

    float left_fraction   = 0.0F;
    float right_fraction  = 0.0F;
    float top_fraction    = 0.0F;
    float bottom_fraction = 0.0F;
    int   left_count      = 0;
    int   right_count     = 0;
    int   top_count       = 0;
    int   bottom_count    = 0;

    const gui_layout_region_t *POUND_RESTRICT region_cursor   = layout->regions;
    const float *POUND_RESTRICT               fraction_cursor = layout->fractions;

    for (int i = 0; i < count; ++i)
    {
        const float               fraction = *fraction_cursor++;
        const gui_layout_region_t region   = *region_cursor++;

        switch (region)
        {
            case GUI_LAYOUT_REGION_LEFT: {
                ++left_count;

                if (fraction > left_fraction)
                {
                    left_fraction = fraction;
                }

                break;
            }
            case GUI_LAYOUT_REGION_RIGHT: {
                ++right_count;

                if (fraction > right_fraction)
                {
                    right_fraction = fraction;
                }

                break;
            }
            case GUI_LAYOUT_REGION_TOP: {
                ++top_count;

                if (fraction > top_fraction)
                {
                    top_fraction = fraction;
                }

                break;
            }
            case GUI_LAYOUT_REGION_BOTTOM: {
                ++bottom_count;

                if (fraction > bottom_fraction)
                {
                    bottom_fraction = fraction;
                }

                break;
            }
            default: {
                POUND_LOG_ERROR(&thread_logger,
                                "Aborting function: stored region %d is corrupted.",
                                (int)region);
                return GUI_PLUGIN_ERROR_INVALID_ARGUMENT;
            }
        }
    }

    float left_width    = screen_width * left_fraction;
    float right_width   = screen_width * right_fraction;
    float top_height    = screen_height * top_fraction;
    float bottom_height = screen_height * bottom_fraction;

    const float column_total = left_width + right_width;

    if (POUND_UNLIKELY(column_total > screen_width))
    {
        const float scale = screen_width / column_total;
        left_width *= scale;
        right_width *= scale;
    }
    const float row_total = top_height + bottom_height;

    if (POUND_UNLIKELY(row_total > screen_height))
    {
        const float scale = screen_height / row_total;
        top_height *= scale;
        bottom_height *= scale;
    }

    float center_width  = screen_width - left_width - right_width;
    float center_height = screen_height - top_height - bottom_height;

    if (POUND_UNLIKELY(center_width < 0.0f))
    {
        center_width = 0.0f;
    }

    if (POUND_UNLIKELY(center_height < 0.0f))
    {
        center_height = 0.0f;
    }

    const gui_layout_rectangle_t left_rectangle
        = { .x = 0.0f, .y = 0.0f, .width = left_width, .height = screen_height };
    const gui_layout_rectangle_t right_rectangle = {
        .x = screen_width - right_width, .y = 0.0f, .width = right_width, .height = screen_height
    };
    const gui_layout_rectangle_t top_rectangle
        = { .x = left_width, .y = 0.0f, .width = center_width, .height = top_height };
    const gui_layout_rectangle_t bottom_rectangle = { .x      = left_width,
                                                      .y      = screen_height - bottom_height,
                                                      .width  = center_width,
                                                      .height = bottom_height };
    const gui_layout_rectangle_t viewport
        = { .x = left_width, .y = top_height, .width = center_width, .height = center_height };

    // Split each region evenly among its panels.
    region_cursor                                           = layout->regions;
    gui_layout_rectangle_t *POUND_RESTRICT rectangle_cursor = layout->panels;
    int                                    left_index       = 0;
    int                                    right_index      = 0;
    int                                    top_index        = 0;
    int                                    bottom_index     = 0;

    for (int i = 0; i < count; ++i)
    {
        gui_layout_rectangle_t    rectangle = { 0 };
        const gui_layout_region_t region    = *region_cursor++;

        switch (region)
        {
            case GUI_LAYOUT_REGION_LEFT: {
                const float slot = left_rectangle.height / (float)left_count;
                rectangle.x      = left_rectangle.x;
                rectangle.y      = left_rectangle.y + slot * (float)left_index;
                rectangle.width  = left_rectangle.width;
                rectangle.height = slot;
                ++left_index;
                break;
            }
            case GUI_LAYOUT_REGION_RIGHT: {
                const float slot = right_rectangle.height / (float)right_count;
                rectangle.x      = right_rectangle.x;
                rectangle.y      = right_rectangle.y + slot * (float)right_index;
                rectangle.width  = right_rectangle.width;
                rectangle.height = slot;
                ++right_index;
                break;
            }
            case GUI_LAYOUT_REGION_TOP: {
                const float slot = top_rectangle.width / (float)top_count;

                // Newest panel joins at the left end.
                rectangle.x      = top_rectangle.x + slot * (float)(top_count - 1 - top_index);
                rectangle.y      = top_rectangle.y;
                rectangle.width  = slot;
                rectangle.height = top_rectangle.height;
                ++top_index;
                break;
            }
            case GUI_LAYOUT_REGION_BOTTOM: {
                const float slot = bottom_rectangle.width / (float)bottom_count;

                // Newest panel joins at the left end.
                rectangle.x = bottom_rectangle.x + slot * (float)(bottom_count - 1 - bottom_index);
                rectangle.y = bottom_rectangle.y;
                rectangle.width  = slot;
                rectangle.height = bottom_rectangle.height;
                ++bottom_index;
                break;
            }
            default: {
                POUND_LOG_ERROR(&thread_logger,
                                "Aborting function: stored region %d is corrupted.",
                                (int)region);
                return GUI_PLUGIN_ERROR_INVALID_ARGUMENT;
            }
        }
        *rectangle_cursor++ = rectangle;
    }

    // The centre viewport is the single most important output: panels are
    // positioned relative to it. Validate it explicitly rather than letting a
    // NaN escape into ImGui.
    if (POUND_UNLIKELY(false == is_rectangle_valid(&viewport)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: computed viewport (%f, %f, %f, %f) is invalid.",
                        viewport.x,
                        viewport.y,
                        viewport.width,
                        viewport.height);
        layout->is_computed = false;
        return GUI_PLUGIN_ERROR_INVALID_ARGUMENT;
    }

    layout->is_computed = true;
    *out_viewport       = viewport;
    return GUI_PLUGIN_SUCCESS;
}

static bool
is_region_valid(const gui_layout_region_t dock)
{
    const bool valid = (dock >= GUI_LAYOUT_REGION_LEFT) && (dock <= GUI_LAYOUT_REGION_BOTTOM);
    return valid;
}

static bool
is_dimensions_valid(const float width, const float height)
{
    const bool valid = isfinite(width) && isfinite(height) && (width >= 0.0F) && (height >= 0.0F);
    return valid;
}

static bool
is_rectangle_valid(const gui_layout_rectangle_t *rectangle)
{
    if (POUND_UNLIKELY(NULL == rectangle))
    {
        return false;
    }

    if (POUND_UNLIKELY(false == isfinite(rectangle->x) || false == isfinite(rectangle->y)
                       || false == isfinite(rectangle->width) || false == isfinite(rectangle->height)))
    {
        return false;
    }

    const bool valid = (rectangle->x >= 0.0F) && (rectangle->y >= 0.0F)
                       && (rectangle->width >= 0.0F) && (rectangle->height >= 0.0F);
    return valid;
}

/*** end of file ***/

#include "pages.h"

/// The theme role a level's text is drawn in.
///
/// Errors and warnings carry colour because they are the two a reader scans for;
/// everything else is body text, so the colour keeps meaning something.
static int
pound_logs_level_role(const log_level_t level)
{
    switch (level)
    {
        case LOG_LEVEL_ERROR:
            return POUND_THEME_STATUS_ERROR;
        case LOG_LEVEL_WARN:
            return POUND_THEME_STATUS_WARNING;
        case LOG_LEVEL_DEBUG:
        case LOG_LEVEL_TRACE:
            return POUND_THEME_TEXT_SECONDARY;
        case LOG_LEVEL_INFO:
        case LOG_LEVEL_NONE:
            return POUND_THEME_TEXT_PRIMARY;
    }

    return POUND_THEME_TEXT_PRIMARY;
}

/// True when `level` is at least as severe as `minimum`.
///
/// The enum orders severity as `NONE = -1` upward to `TRACE = 4`, so a record
/// shows when its value is *less than or equal to* the chosen floor. Stating the
/// comparison once here keeps several call sites from each getting the direction
/// wrong in their own way.
static bool
pound_logs_level_passes(const log_level_t level, const log_level_t minimum)
{
    return (int)level <= (int)minimum;
}

void
pound_page_render_logs(const pound_page_context_t *POUND_RESTRICT context)
{
    if (POUND_UNLIKELY(NULL == context))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: context is NULL.");
        return;
    }

    pound_page_heading(context,
                       "DIAGNOSTICS",
                       "Logs",
                       "Everything Pound has reported, newest last. Errors and warnings carry "
                       "colour so they are easy to find.");

    if ((NULL == context->log_ring) || (NULL == context->log_scratch)
        || (context->log_scratch_capacity < (size_t)POUND_LOG_SNAPSHOT_MAX))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "The Logs page has no ring or scratch buffer, so it cannot show records.");
        igTextDisabled("The in-process log is unavailable this frame; see the console output.");
        return;
    }

    pound_pages_view_t *const view = context->view;

    if (NULL == view)
    {
        POUND_LOG_ERROR(&thread_logger, "The Logs page has no view state.");
        igTextDisabled("The Logs page state is unavailable this frame.");
        return;
    }

    // The controls. `##` suffixes keep the labels unique as ImGui identifiers while
    // still reading as sentences beside their widget.
    int filter_index = pound_page_log_level_index(view->log_min_level);

    igSetNextItemWidth(160.0F);

    if (igCombo_Str("##log_level_filter", &filter_index, pound_page_log_level_items(), 6))
    {
        view->log_min_level = pound_page_log_level_at(filter_index);
    }

    igSameLine(0.0F, 12.0F);
    igCheckbox("Follow newest", &view->log_auto_scroll);

    igSameLine(0.0F, 12.0F);

    if (igButton("Clear", (ImVec2){ 0.0F, 0.0F }))
    {
        pound_log_ring_clear(context->log_ring);
    }

    const size_t total = pound_log_ring_snapshot(
        context->log_ring, context->log_scratch, (size_t)POUND_LOG_SNAPSHOT_MAX);

    size_t visible = 0;

    for (size_t i = 0; i < total; i++)
    {
        if (pound_logs_level_passes(context->log_scratch[i].level, view->log_min_level))
        {
            ++visible;
        }
    }

    igPushStyleColor_Vec4(
        ImGuiCol_Text, pound_theme_imgui_color(context->theme, POUND_THEME_TEXT_SECONDARY));
    const bool count_font_pushed = pound_fonts_push(context->fonts, POUND_FONT_ROLE_SMALL);
    igText("Showing %zu of %zu captured record(s); filter set to %s.",
           visible,
           total,
           pound_page_log_level_label(view->log_min_level));
    pound_fonts_pop(count_font_pushed);
    igPopStyleColor(1);

    const float list_height = igGetContentRegionAvail().y;

    if (pound_page_begin_card(context, "##log_records", list_height))
    {
        if (0U == total)
        {
            igTextDisabled("Nothing has been logged yet.");
        }
        else if (0U == visible)
        {
            igTextDisabled("No records are at or above the chosen level. Lower the filter to see "
                           "more.");
        }
        else
        {
            for (size_t i = 0; i < total; i++)
            {
                const pound_log_record_t *const record = &context->log_scratch[i];

                if (false == pound_logs_level_passes(record->level, view->log_min_level))
                {
                    continue;
                }

                const ImVec4 color
                    = pound_theme_imgui_color(context->theme, pound_logs_level_role(record->level));

                // The sequence number is shown beside the level so a reader can see
                // that records were dropped -- a gap in the numbers -- rather than
                // reading two non-adjacent entries as if they were neighbours. The
                // level is padded so the messages line up in a column.
                igTextColored(color,
                              "%6llu  %6s  %s",
                              (unsigned long long)record->sequence,
                              pound_page_log_level_label(record->level),
                              record->text);
            }
        }

        if (true == view->log_auto_scroll)
        {
            igSetScrollY_Float(igGetScrollMaxY());
        }
    }

    pound_page_end_card();
}

/*** end of file ***/

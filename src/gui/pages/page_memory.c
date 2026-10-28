#include "pages.h"

/// The Memory page.
///
/// The address-space map is not drawn here. It lives in a floating window the
/// shell owns (`debug_memory_render`, driven from `gui.c`), because ImGui does
/// not support `Begin` inside a child window and the map used to be clipped to a
/// sliver when this page tried to draw it inside its card. What is left for the
/// page is the heading, an explanation of what the window shows, and a way to
/// bring the window back after its own close button hid it.
void
pound_page_render_memory(const pound_page_context_t *POUND_RESTRICT context)
{
    if (POUND_UNLIKELY(NULL == context))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: context is NULL.");
        return;
    }

    pound_page_heading(context,
                       "DIAGNOSTICS",
                       "Memory",
                       "The guest address space as mapped so far, from the code segment down to "
                       "the framebuffer.");

    if (NULL == context->memory_tracker)
    {
        POUND_LOG_ERROR(&thread_logger, "The Memory page has no tracker to render.");
        igTextDisabled("The memory tracker is unavailable this frame; see the Logs page.");
        return;
    }

    if (pound_page_begin_card(context, "##memory_map_card", 0.0F))
    {
        const bool *const window_open = context->memory_window_open;

        if ((NULL != window_open) && (true == *window_open))
        {
            igTextWrapped("The Guest Address Space window is open on the left. It stays there "
                          "while you move between pages; close it with its own close button.");
        }
        else
        {
            igTextWrapped("The Guest Address Space window is closed. Choose Memory in the sidebar "
                          "again to bring it back, or use the button below.");
            igSpacing();

            if ((NULL != window_open)
                && (true == igButton("Show Guest Address Space", (ImVec2){ 0.0F, 0.0F })))
            {
                *context->memory_window_open = true;
            }
        }
    }

    pound_page_end_card();
}

/*** end of file ***/

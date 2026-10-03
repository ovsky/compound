#include "pages.h"

/// The Memory page.
///
/// A thin frame around `debug_memory_render`, which already owns the drawing of
/// the address-space map, its boxes and its detail panel. The only thing this page
/// adds is the heading, and the refusal to draw when the tracker is not there --
/// passing NULL into the renderer would be a crash rather than an empty state.
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
        debug_memory_render(context->memory_tracker);
    }

    pound_page_end_card();
}

/*** end of file ***/

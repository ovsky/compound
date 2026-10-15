//! Unit tests for `memory_layout.h` and the region gate it installs.
//!
//! ## What this module is for
//!
//! `guest_memory_t` is a flat window with no structure: every address translates to
//! every byte. Horizon is not flat. A process's address space is a table of regions
//! whose state and permission decide what the guest may do where, and two things read
//! that table: the access gate inside `guest_memory.c`, and `svcQueryMemory`. These
//! cases pin down that the two cannot disagree -- that the same `Free` page which
//! faults on a read is the one reported as `Inaccessible` by a query, and the same
//! `Code` page which runs is the one refused a store.
//!
//! ## Why the window is deliberately large
//!
//! `svcSetHeapSize` works in multiples of 2MB and a real heap region is much larger
//! still, so the window here is 16MB. A window that only held a page or two could not
//! tell a heap that grows from one that silently refuses, which is the distinction the
//! heap cases exist to hold down.

#include "pound_test.h"

#include "cpu/interpreter.h"
#include "guest_state.h"
#include "hos/memory_layout.h"
#include "log.h"
#include "memory/guest_memory.h"

#include <stddef.h>
#include <string.h>

// -----------------------------------------------------------------------------
// The window
// -----------------------------------------------------------------------------

#define LAYOUT_GUEST_BASE 0x40000000ULL
#define LAYOUT_GUEST_SIZE 0x01000000ULL // 16 MiB, split into two 8 MiB halves

/// Convenience spellings for the rights a region in a case wants.
#define LAYOUT_RW (GUEST_MEMORY_PERM_READ | GUEST_MEMORY_PERM_WRITE)
#define LAYOUT_RX (GUEST_MEMORY_PERM_READ | GUEST_MEMORY_PERM_EXEC)

/// The window itself. `guest_memory_init` requires 16-byte alignment; 64 matches what
/// the struct itself asks for and is the alignment a real mapping would have.
static _Alignas(64) uint8_t g_memory[LAYOUT_GUEST_SIZE];

static guest_memory_t      g_memory_region;
static hos_memory_layout_t g_layout;

/// The base of the heap region, which `hos_memory_layout_init` places at the midpoint.
static uint64_t
heap_base(void)
{
    return LAYOUT_GUEST_BASE + (LAYOUT_GUEST_SIZE / 2U);
}

/// Brings up a guest window and lays out an address space over it.
static void
layout_machine_reset(void)
{
    memset(g_memory, 0, sizeof(g_memory));
    memset(&g_memory_region, 0, sizeof(g_memory_region));
    memset(&g_layout, 0, sizeof(g_layout));

    POUND_REQUIRE(POUND_SUCCESS ==
                  guest_memory_init(&g_memory_region, g_memory, sizeof(g_memory), LAYOUT_GUEST_BASE));

    POUND_REQUIRE(POUND_SUCCESS == hos_memory_layout_init(&g_layout,
                                                          &g_memory_region,
                                                          LAYOUT_GUEST_BASE,
                                                          LAYOUT_GUEST_BASE + LAYOUT_GUEST_SIZE));
}

/// Maps one page with `state` and `permission`, reporting the error code.
///
/// A helper rather than a macro so the cases below read as the sequence of mappings they
/// are; `POUND_REQUIRE` cannot be used inside it, because it returns `void`, so callers
/// that need the mapping to have worked use `POUND_REQUIRE(POUND_SUCCESS == map_page(...))`.
static error_t
map_page(const uint64_t base, const uint32_t state, const uint32_t permission)
{
    return hos_memory_layout_map(&g_layout, base, 0x1000U, state, permission, 0U);
}

// -----------------------------------------------------------------------------
// hos_memory_layout_init
// -----------------------------------------------------------------------------

POUND_TEST(hos_memory_layout, init_splits_the_window_into_two_free_halves)
{
    layout_machine_reset();

    POUND_CHECK_EQ_U64(g_layout.address_space_start, LAYOUT_GUEST_BASE);
    POUND_CHECK_EQ_U64(g_layout.address_space_end, LAYOUT_GUEST_BASE + LAYOUT_GUEST_SIZE);
    POUND_CHECK_EQ_U64(g_layout.heap_base, heap_base());
    POUND_CHECK_EQ_U64(g_layout.heap_region_size, LAYOUT_GUEST_SIZE / 2U);
    POUND_CHECK_EQ_U64(g_layout.heap_size, 0U);
    POUND_CHECK_EQ_U64(g_layout.region_count, 2U);

    // The map region, then the heap region: both Free, both without any permission.
    POUND_CHECK_EQ_U64(g_layout.regions[0].base, LAYOUT_GUEST_BASE);
    POUND_CHECK_EQ_U64(g_layout.regions[0].size, LAYOUT_GUEST_SIZE / 2U);
    POUND_CHECK_EQ_U64(g_layout.regions[0].state, HOS_MEMORY_STATE_FREE);
    POUND_CHECK_EQ_U64(g_layout.regions[0].permission, 0U);

    POUND_CHECK_EQ_U64(g_layout.regions[1].base, heap_base());
    POUND_CHECK_EQ_U64(g_layout.regions[1].size, LAYOUT_GUEST_SIZE / 2U);
    POUND_CHECK_EQ_U64(g_layout.regions[1].state, HOS_MEMORY_STATE_FREE);
    POUND_CHECK_EQ_U64(g_layout.regions[1].permission, 0U);

    // The bind: the gate is now reading the table this layout owns.
    POUND_CHECK_PTR_EQ(g_memory_region.regions, g_layout.regions);
    POUND_CHECK_EQ_U64(g_memory_region.region_count, 2U);

    // Free means no access, which is what makes "laid out but not yet loaded" different
    // from "flat and readable". A read, a write and a fetch all fault.
    size_t bytes = 0U;
    POUND_CHECK_PTR_NULL(guest_memory_translate_read(&g_memory_region, LAYOUT_GUEST_BASE, &bytes));
    POUND_CHECK_PTR_NULL(guest_memory_translate_write(&g_memory_region, LAYOUT_GUEST_BASE + 0x1000U, &bytes));
    POUND_CHECK_PTR_NULL(guest_memory_translate_execute(&g_memory_region, heap_base(), &bytes));
}

POUND_TEST(hos_memory_layout, init_rejects_what_it_cannot_partition)
{
    guest_memory_t      region;
    hos_memory_layout_t layout;

    memset(g_memory, 0, sizeof(g_memory));
    POUND_REQUIRE(POUND_SUCCESS == guest_memory_init(&region, g_memory, sizeof(g_memory), LAYOUT_GUEST_BASE));

    memset(&layout, 0, sizeof(layout));
    pound_test_log_reset();

    const uint64_t end = LAYOUT_GUEST_BASE + LAYOUT_GUEST_SIZE;

    POUND_CHECK_EQ_U64(hos_memory_layout_init(NULL, &region, LAYOUT_GUEST_BASE, end), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_memory_layout_init(&layout, NULL, LAYOUT_GUEST_BASE, end), POUND_ERROR_INVALID_ARGUMENT);

    // Not page-aligned, at either end.
    POUND_CHECK_EQ_U64(hos_memory_layout_init(&layout, &region, LAYOUT_GUEST_BASE + 1U, end),
                       POUND_ERROR_MEMORY_ALIGNMENT);
    POUND_CHECK_EQ_U64(hos_memory_layout_init(&layout, &region, LAYOUT_GUEST_BASE, end - 1U),
                       POUND_ERROR_MEMORY_ALIGNMENT);

    // Too small to split into two non-empty halves.
    POUND_CHECK_EQ_U64(hos_memory_layout_init(&layout, &region, LAYOUT_GUEST_BASE, LAYOUT_GUEST_BASE + 0x1000U),
                       POUND_ERROR_INVALID_ARGUMENT);

    // Outside the window it is supposed to describe, below and above.
    POUND_CHECK_EQ_U64(hos_memory_layout_init(&layout, &region, LAYOUT_GUEST_BASE - 0x1000U, end),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_memory_layout_init(&layout, &region, LAYOUT_GUEST_BASE, end + 0x1000U),
                       POUND_ERROR_INVALID_ARGUMENT);

    // Every refusal is reported, and none of them touched the layout.
    POUND_CHECK_EQ_U64(layout.magic, 0U);
    POUND_CHECK(pound_test_log_contains("Rejecting memory layout init"));

    // A second successful init is refused rather than silently re-splitting the space.
    POUND_REQUIRE(POUND_SUCCESS == hos_memory_layout_init(&layout, &region, LAYOUT_GUEST_BASE, end));
    POUND_CHECK_EQ_U64(hos_memory_layout_init(&layout, &region, LAYOUT_GUEST_BASE, end),
                       POUND_ERROR_ALREADY_INITIALIZED);
    POUND_CHECK(pound_test_log_contains("already initialised"));
}

// -----------------------------------------------------------------------------
// hos_memory_layout_find
// -----------------------------------------------------------------------------

POUND_TEST(hos_memory_layout, find_tiles_the_address_space_and_nothing_outside_it)
{
    layout_machine_reset();

    // The first and last byte of each half, so both boundaries are exercised on both sides.
    POUND_CHECK_PTR_EQ(hos_memory_layout_find(&g_layout, LAYOUT_GUEST_BASE), &g_layout.regions[0]);
    POUND_CHECK_PTR_EQ(hos_memory_layout_find(&g_layout, heap_base() - 1U), &g_layout.regions[0]);
    POUND_CHECK_PTR_EQ(hos_memory_layout_find(&g_layout, heap_base()), &g_layout.regions[1]);
    POUND_CHECK_PTR_EQ(hos_memory_layout_find(&g_layout, LAYOUT_GUEST_BASE + LAYOUT_GUEST_SIZE - 1U),
                       &g_layout.regions[1]);

    // Outside the address space, on either side: no region at all. `svcQueryMemory` is what
    // turns "none" into the fabricated `Inaccessible` block, so this is the case that keeps
    // the two halves of that contract honest.
    POUND_CHECK_PTR_NULL(hos_memory_layout_find(&g_layout, LAYOUT_GUEST_BASE - 1U));
    POUND_CHECK_PTR_NULL(hos_memory_layout_find(&g_layout, LAYOUT_GUEST_BASE + LAYOUT_GUEST_SIZE));
    POUND_CHECK_PTR_NULL(hos_memory_layout_find(&g_layout, UINT64_MAX));

    // A layout that was never initialised, and no layout at all.
    hos_memory_layout_t blank;
    memset(&blank, 0, sizeof(blank));
    POUND_CHECK_PTR_NULL(hos_memory_layout_find(&blank, LAYOUT_GUEST_BASE));
    POUND_CHECK_PTR_NULL(hos_memory_layout_find(NULL, LAYOUT_GUEST_BASE));
}

// -----------------------------------------------------------------------------
// hos_memory_layout_map
// -----------------------------------------------------------------------------

POUND_TEST(hos_memory_layout, map_carves_free_space_and_splits_around_it)
{
    layout_machine_reset();

    // A page in the middle of the map region: the Free region splits on both sides, so two
    // entries are added.
    const uint64_t normal_base = LAYOUT_GUEST_BASE + 0x1000U;
    POUND_REQUIRE(POUND_SUCCESS ==
                  map_page(normal_base, HOS_MEMORY_STATE_NORMAL, LAYOUT_RW));

    POUND_CHECK_EQ_U64(g_layout.region_count, 4U);
    POUND_CHECK_EQ_U64(g_layout.regions[0].base, LAYOUT_GUEST_BASE);
    POUND_CHECK_EQ_U64(g_layout.regions[0].size, 0x1000U);
    POUND_CHECK_EQ_U64(g_layout.regions[0].state, HOS_MEMORY_STATE_FREE);
    POUND_CHECK_EQ_U64(g_layout.regions[1].base, normal_base);
    POUND_CHECK_EQ_U64(g_layout.regions[1].size, 0x1000U);
    POUND_CHECK_EQ_U64(g_layout.regions[1].state, HOS_MEMORY_STATE_NORMAL);
    POUND_CHECK_EQ_U64(g_layout.regions[1].permission, LAYOUT_RW);
    POUND_CHECK_EQ_U64(g_layout.regions[2].base, normal_base + 0x1000U);
    POUND_CHECK_EQ_U64(g_layout.regions[2].state, HOS_MEMORY_STATE_FREE);

    // The gate agrees with the table: writable and readable, not executable.
    size_t bytes = 0U;
    POUND_CHECK_PTR_NON_NULL(guest_memory_translate_read(&g_memory_region, normal_base, &bytes));
    POUND_CHECK_PTR_NON_NULL(guest_memory_translate_write(&g_memory_region, normal_base, &bytes));
    POUND_CHECK_PTR_NULL(guest_memory_translate_execute(&g_memory_region, normal_base, &bytes));

    // The byte count stops at the region's end: an access at the last byte sees one byte,
    // not the Free page after it.
    POUND_CHECK_PTR_NON_NULL(guest_memory_translate_write(&g_memory_region, normal_base + 0xFFFU, &bytes));
    POUND_CHECK_EQ_U64(bytes, 1U);

    // Mapping the Free page to its left exactly adds no entry: nothing is left over on
    // either side, so the growth is zero.
    POUND_REQUIRE(POUND_SUCCESS == map_page(LAYOUT_GUEST_BASE, HOS_MEMORY_STATE_CODE, LAYOUT_RX));
    POUND_CHECK_EQ_U64(g_layout.region_count, 4U);
    POUND_CHECK_EQ_U64(g_layout.regions[0].base, LAYOUT_GUEST_BASE);
    POUND_CHECK_EQ_U64(g_layout.regions[0].state, HOS_MEMORY_STATE_CODE);

    // ... and the code page is executable and readable but not writable.
    POUND_CHECK_PTR_NON_NULL(guest_memory_translate_execute(&g_memory_region, LAYOUT_GUEST_BASE, &bytes));
    POUND_CHECK_PTR_NULL(guest_memory_translate_write(&g_memory_region, LAYOUT_GUEST_BASE, &bytes));
}

POUND_TEST(hos_memory_layout, map_refuses_what_it_must)
{
    layout_machine_reset();

    // A layout that was never initialised, and no layout at all.
    hos_memory_layout_t blank;
    memset(&blank, 0, sizeof(blank));
    POUND_CHECK_EQ_U64(hos_memory_layout_map(&blank, LAYOUT_GUEST_BASE, 0x1000U, HOS_MEMORY_STATE_NORMAL, LAYOUT_RW, 0U),
                       POUND_ERROR_NOT_INITIALIZED);
    POUND_CHECK_EQ_U64(hos_memory_layout_map(NULL, LAYOUT_GUEST_BASE, 0x1000U, HOS_MEMORY_STATE_NORMAL, LAYOUT_RW, 0U),
                       POUND_ERROR_INVALID_ARGUMENT);

    // Zero size, and misaligned bounds. None of these mutate the table.
    POUND_CHECK_EQ_U64(hos_memory_layout_map(&g_layout, LAYOUT_GUEST_BASE + 0x1000U, 0U, HOS_MEMORY_STATE_NORMAL, LAYOUT_RW, 0U),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_memory_layout_map(&g_layout, LAYOUT_GUEST_BASE + 0x1001U, 0x1000U, HOS_MEMORY_STATE_NORMAL, LAYOUT_RW, 0U),
                       POUND_ERROR_MEMORY_ALIGNMENT);
    POUND_CHECK_EQ_U64(hos_memory_layout_map(&g_layout, LAYOUT_GUEST_BASE + 0x1000U, 0x1001U, HOS_MEMORY_STATE_NORMAL, LAYOUT_RW, 0U),
                       POUND_ERROR_MEMORY_ALIGNMENT);

    // Outside the address space, below and above.
    POUND_CHECK_EQ_U64(hos_memory_layout_map(&g_layout, LAYOUT_GUEST_BASE - 0x1000U, 0x1000U, HOS_MEMORY_STATE_NORMAL, LAYOUT_RW, 0U),
                       POUND_ERROR_GUEST_ADDRESS_OUT_OF_BOUNDS);
    POUND_CHECK_EQ_U64(hos_memory_layout_map(&g_layout, LAYOUT_GUEST_BASE + LAYOUT_GUEST_SIZE - 0x1000U, 0x2000U,
                                             HOS_MEMORY_STATE_NORMAL, LAYOUT_RW, 0U),
                       POUND_ERROR_GUEST_ADDRESS_OUT_OF_BOUNDS);

    // `base + size` wrapping is its own refusal, not a bounds one. The base is page-aligned
    // so that this reaches the overflow check rather than stopping at alignment.
    POUND_CHECK_EQ_U64(hos_memory_layout_map(&g_layout, UINT64_MAX - 0xFFFU, 0x1000U, HOS_MEMORY_STATE_NORMAL, LAYOUT_RW, 0U),
                       POUND_ERROR_GUEST_ADDRESS_OVERFLOW);

    // The heap region belongs to `svcSetHeapSize`, so the loader may not carve it.
    POUND_CHECK_EQ_U64(map_page(heap_base(), HOS_MEMORY_STATE_NORMAL, LAYOUT_RW), POUND_ERROR_INVALID_ARGUMENT);

    // Now one page is mapped, leaving a Free page to its left. A target that is not Free,
    // and one that spans a region boundary, both need regions to look at, so they come
    // after the map rather than before it.
    POUND_REQUIRE(POUND_SUCCESS == map_page(LAYOUT_GUEST_BASE + 0x1000U, HOS_MEMORY_STATE_NORMAL, LAYOUT_RW));

    // Not Free: the very page that was just mapped.
    POUND_CHECK_EQ_U64(map_page(LAYOUT_GUEST_BASE + 0x1000U, HOS_MEMORY_STATE_NORMAL, LAYOUT_RW), POUND_ERROR_INVALID_ARGUMENT);

    // Spans a region boundary: it starts in the Free page to the left of the mapped one and
    // runs into it.
    POUND_CHECK_EQ_U64(hos_memory_layout_map(&g_layout, LAYOUT_GUEST_BASE, 0x2000U, HOS_MEMORY_STATE_NORMAL, LAYOUT_RW, 0U),
                       POUND_ERROR_INVALID_ARGUMENT);

    POUND_CHECK(pound_test_log_contains("Rejecting memory layout map"));
}

POUND_TEST(hos_memory_layout, map_fills_the_table_and_then_refuses)
{
    layout_machine_reset();

    uint32_t successes = 0U;
    error_t  error     = POUND_SUCCESS;

    // One page at a time from the start of the map region. Each map takes the front of the
    // remaining Free region, so each adds exactly one entry until the table is full.
    for (uint64_t offset = 0U; offset < (LAYOUT_GUEST_SIZE / 2U); offset += 0x1000U)
    {
        error = map_page(LAYOUT_GUEST_BASE + offset, HOS_MEMORY_STATE_NORMAL, LAYOUT_RW);

        if (error != POUND_SUCCESS)
        {
            break;
        }

        successes++;
    }

    // Two entries at the start plus one per successful map reaches exactly the cap.
    POUND_CHECK_EQ_U64((uint64_t)successes + 2U, HOS_MEMORY_LAYOUT_MAX_REGIONS);
    POUND_CHECK_EQ_U64(error, POUND_ERROR_ALLOCATION_FAILED);
    POUND_CHECK_EQ_U64(g_layout.region_count, HOS_MEMORY_LAYOUT_MAX_REGIONS);
    POUND_CHECK(pound_test_log_contains("region table is full"));
}

// -----------------------------------------------------------------------------
// hos_memory_layout_set_heap_size
// -----------------------------------------------------------------------------

POUND_TEST(hos_memory_layout, set_heap_size_commits_normal_bytes_and_frees_the_rest)
{
    layout_machine_reset();

    const uint64_t base        = heap_base();
    const uint64_t region_size = g_layout.heap_region_size;

    // Zero is legitimate: it commits nothing and leaves the whole heap region Free.
    POUND_REQUIRE(POUND_SUCCESS == hos_memory_layout_set_heap_size(&g_layout, 0U));
    POUND_CHECK_EQ_U64(g_layout.heap_size, 0U);
    POUND_CHECK_EQ_U64(g_layout.regions[1].state, HOS_MEMORY_STATE_FREE);

    // Grow to 2MB: the committed part is Normal and read|write, the rest stays Free.
    POUND_REQUIRE(POUND_SUCCESS == hos_memory_layout_set_heap_size(&g_layout, 0x200000U));
    POUND_CHECK_EQ_U64(g_layout.heap_size, 0x200000U);

    const guest_memory_region_t *region = hos_memory_layout_find(&g_layout, base);
    POUND_REQUIRE(region != NULL);
    POUND_CHECK_EQ_U64(region->base, base);
    POUND_CHECK_EQ_U64(region->size, 0x200000U);
    POUND_CHECK_EQ_U64(region->state, HOS_MEMORY_STATE_NORMAL);
    POUND_CHECK_EQ_U64(region->permission, LAYOUT_RW);

    size_t bytes = 0U;
    POUND_CHECK_PTR_NON_NULL(guest_memory_translate_write(&g_memory_region, base, &bytes));
    POUND_CHECK_PTR_NULL(guest_memory_translate_execute(&g_memory_region, base, &bytes));

    // The byte just past the committed heap is Free, so it refuses.
    POUND_CHECK_PTR_NULL(guest_memory_translate_read(&g_memory_region, base + 0x200000U, &bytes));

    // Grow again: the committed region gets longer.
    POUND_REQUIRE(POUND_SUCCESS == hos_memory_layout_set_heap_size(&g_layout, 0x400000U));
    region = hos_memory_layout_find(&g_layout, base + 0x200000U);
    POUND_REQUIRE(region != NULL);
    POUND_CHECK_EQ_U64(region->state, HOS_MEMORY_STATE_NORMAL);

    // Shrink back: the bytes past the new size are Free again, and the last committed byte
    // still works.
    POUND_REQUIRE(POUND_SUCCESS == hos_memory_layout_set_heap_size(&g_layout, 0x200000U));
    POUND_CHECK_PTR_NULL(guest_memory_translate_read(&g_memory_region, base + 0x200000U, &bytes));
    POUND_CHECK_PTR_NON_NULL(guest_memory_translate_read(&g_memory_region, base + 0x1FFFFFU, &bytes));

    // The whole region at once: the heap is one Normal entry and the table is back to its
    // two-entry shape.
    POUND_REQUIRE(POUND_SUCCESS == hos_memory_layout_set_heap_size(&g_layout, region_size));
    POUND_CHECK_EQ_U64(g_layout.region_count, 2U);
    POUND_CHECK_EQ_U64(g_layout.heap_size, region_size);

    region = hos_memory_layout_find(&g_layout, base + region_size - 1U);
    POUND_REQUIRE(region != NULL);
    POUND_CHECK_EQ_U64(region->size, region_size);

    // And back to nothing.
    POUND_REQUIRE(POUND_SUCCESS == hos_memory_layout_set_heap_size(&g_layout, 0U));
    POUND_CHECK_EQ_U64(g_layout.region_count, 2U);
    POUND_CHECK_EQ_U64(g_layout.regions[1].base, base);
    POUND_CHECK_EQ_U64(g_layout.regions[1].size, region_size);
    POUND_CHECK_EQ_U64(g_layout.regions[1].state, HOS_MEMORY_STATE_FREE);
}

POUND_TEST(hos_memory_layout, set_heap_size_refuses_bad_sizes)
{
    layout_machine_reset();

    hos_memory_layout_t blank;
    memset(&blank, 0, sizeof(blank));

    POUND_CHECK_EQ_U64(hos_memory_layout_set_heap_size(NULL, 0U), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(hos_memory_layout_set_heap_size(&blank, 0U), POUND_ERROR_NOT_INITIALIZED);

    // Not page-aligned.
    POUND_CHECK_EQ_U64(hos_memory_layout_set_heap_size(&g_layout, 0x800U), POUND_ERROR_MEMORY_ALIGNMENT);

    // Larger than the heap region.
    POUND_CHECK_EQ_U64(hos_memory_layout_set_heap_size(&g_layout, g_layout.heap_region_size + 0x1000U),
                       POUND_ERROR_INVALID_ARGUMENT);

    // A refused resize changed nothing.
    POUND_CHECK_EQ_U64(g_layout.heap_size, 0U);
    POUND_CHECK_EQ_U64(g_layout.regions[1].base, heap_base());
    POUND_CHECK_EQ_U64(g_layout.regions[1].size, g_layout.heap_region_size);
    POUND_CHECK_EQ_U64(g_layout.regions[1].state, HOS_MEMORY_STATE_FREE);
}

// -----------------------------------------------------------------------------
// The gate the layout installs, seen through the interpreter's fetch
// -----------------------------------------------------------------------------

POUND_TEST(hos_memory_layout, instruction_fetch_needs_execute_permission)
{
    layout_machine_reset();

    // One page readable and writable, the next readable and executable. They are mapped
    // before anything runs, because there is no set-permission call yet to change one into
    // the other after the fact.
    POUND_REQUIRE(POUND_SUCCESS == map_page(LAYOUT_GUEST_BASE, HOS_MEMORY_STATE_NORMAL, LAYOUT_RW));
    POUND_REQUIRE(POUND_SUCCESS == map_page(LAYOUT_GUEST_BASE + 0x1000U, HOS_MEMORY_STATE_CODE, LAYOUT_RX));

    // A `ret`, little-endian, staged straight into the host buffer. Staging bytes is not a
    // guest access, so it is not gated; what is gated is the fetch of it.
    const uint32_t ret  = 0xD65F03C0U;
    uint8_t       *code = &g_memory[0x1000U];
    code[0] = (uint8_t)ret;
    code[1] = (uint8_t)(ret >> 8);
    code[2] = (uint8_t)(ret >> 16);
    code[3] = (uint8_t)(ret >> 24);

    guest_state_t state;
    POUND_REQUIRE(POUND_SUCCESS == guest_state_init(&state));

    interpreter_trap_t trap;
    uint32_t           executed = 0U;

    // Fetching from the read|write page is a memory fault: readable is not executable.
    state.pc = LAYOUT_GUEST_BASE;
    POUND_CHECK_EQ_U64(interpreter_run(&state, &g_memory_region, 1U, &executed, &trap),
                       POUND_ERROR_MEMORY_FAULT);
    POUND_CHECK_EQ_U64(executed, 0U);

    // The same fetch from the read|execute page runs.
    state.pc = LAYOUT_GUEST_BASE + 0x1000U;
    POUND_CHECK_EQ_U64(interpreter_run(&state, &g_memory_region, 1U, &executed, &trap), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(executed, 1U);

    // And a fetch into the Free page beyond the code faults, because Free permits nothing.
    state.pc = LAYOUT_GUEST_BASE + 0x2000U;
    POUND_CHECK_EQ_U64(interpreter_run(&state, &g_memory_region, 1U, &executed, &trap),
                       POUND_ERROR_MEMORY_FAULT);
}

// -----------------------------------------------------------------------------

POUND_TEST_SUITE(hos_memory_layout,
                 POUND_TEST_CASE(hos_memory_layout, init_splits_the_window_into_two_free_halves),
                 POUND_TEST_CASE(hos_memory_layout, init_rejects_what_it_cannot_partition),
                 POUND_TEST_CASE(hos_memory_layout, find_tiles_the_address_space_and_nothing_outside_it),
                 POUND_TEST_CASE(hos_memory_layout, map_carves_free_space_and_splits_around_it),
                 POUND_TEST_CASE(hos_memory_layout, map_refuses_what_it_must),
                 POUND_TEST_CASE(hos_memory_layout, map_fills_the_table_and_then_refuses),
                 POUND_TEST_CASE(hos_memory_layout, set_heap_size_commits_normal_bytes_and_frees_the_rest),
                 POUND_TEST_CASE(hos_memory_layout, set_heap_size_refuses_bad_sizes),
                 POUND_TEST_CASE(hos_memory_layout, instruction_fetch_needs_execute_permission))

/*** end of file ***/

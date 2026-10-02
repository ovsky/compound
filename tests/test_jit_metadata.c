//! Tests for the JIT metadata manager.
//!
//! The manager is the one part of Pound whose correctness is a *concurrency*
//! argument rather than a computation, so most of these cases are not "does this
//! return the right index" -- they are "does this leave the table in a state a
//! second thread could still navigate".
//!
//! The three properties that matter, and where they are covered:
//!
//! - **A probe always terminates, and always finds the right key.** Linear probing
//!   with deletion silently strands entries, and the symptom is a block that has been
//!   translated and then cannot be found -- which in a JIT looks exactly like a game
//!   that got slower for no reason.
//!   `every_interned_address_is_still_findable_after_its_own_slot_is_invalidated`
//!   walks more than half a table's worth of addresses through the invalidation of
//!   every one of them and checks all of them afterwards; if any probe chain had been
//!   split, some address would come back on a different slot.
//! - **Invalidation cannot pull code out from under a running block.** The lease
//!   handshake is the only thing standing between a self-modifying-code guest and a
//!   jump into freed memory, so
//!   `an_invalidation_that_would_catch_a_running_block_is_refused_whole` checks both
//!   halves: that the refusal names the leased block, and that the *unleased* block
//!   in the same range was not cleared either.
//! - **A failed claim is distinguishable from a contended one.** They return the same
//!   status but mean opposite things, and the reported previous state is the only
//!   thing telling them apart.

#include "pound_test.h"

#include "jit/jit_metadata.h"
#include "log.h"
#include <string.h>

/// Bytes of "host code" a case pretends to have emitted.
///
/// Never executed and never dereferenced: the manager stores the address, and these
/// cases are about the bookkeeping. A local array keeps the addresses distinct and
/// gives them a lifetime that ends after the manager does, which is the direction
/// that would fault if anything held on to a host address too long.
#define FAKE_CODE_BYTES 64U

/// A block's simulated host code.
typedef struct
{
    uint8_t bytes[FAKE_CODE_BYTES];
} fake_code_t;

/// Brings up a manager with the smallest legal table, so a case can fill it.
///
/// 256 slots is the documented minimum, and `max_blocks` is left at its default of
/// three quarters, which is 192. Small enough that filling the table in a test is a
/// loop over a couple of hundred addresses rather than tens of thousands, and large
/// enough that linear probing has to do real work.
static error_t
make_manager(jit_metadata_t *POUND_RESTRICT manager)
{
    jit_metadata_config_t config;

    memset(&config, 0, sizeof(config));

    config.capacity = JIT_METADATA_MIN_CAPACITY;

    return jit_metadata_init(manager, &config);
}

/// Interns `guest_pc`, claims it, publishes `code` over it, and reports the slot.
///
/// The whole translate-a-block sequence in one step, because almost every case needs
/// a `READY` block and spelling out three calls each time would bury the thing being
/// tested. Returns the status of the first step that failed.
static error_t
publish_block(jit_metadata_t *POUND_RESTRICT manager,
              const uint64_t                       guest_pc,
              void *POUND_RESTRICT                  code,
              const size_t                         code_bytes,
              const uint64_t                       guest_size)
{
    size_t index = 0U;

    error_t status = jit_metadata_intern(manager, guest_pc, &index);

    if (POUND_SUCCESS != status)
    {
        return status;
    }

    jit_metadata_state_t previous = JIT_METADATA_STATE_EMPTY;

    status = jit_metadata_try_begin(manager, index, &previous);

    if (POUND_SUCCESS != status)
    {
        return status;
    }

    return jit_metadata_publish(manager, index, code, code_bytes, guest_size, 0U);
}

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

POUND_TEST(metadata, a_default_manager_is_usable_and_reports_its_own_shape)
{
    jit_metadata_t manager;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_init(&manager, NULL));

    jit_metadata_resolved_t resolved;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_describe(&manager, &resolved));

    POUND_CHECK(JIT_METADATA_DEFAULT_CAPACITY == resolved.capacity);
    POUND_CHECK(resolved.capacity - 1U == resolved.capacity_mask);
    POUND_CHECK(JIT_METADATA_ENTRY_BYTES == resolved.entry_bytes);
    POUND_CHECK_MSG(resolved.capacity * resolved.entry_bytes == resolved.table_bytes,
                    "A table of %zu slots of %zu bytes was reported as %zu bytes.",
                    resolved.capacity, resolved.entry_bytes, resolved.table_bytes);
    POUND_CHECK(JIT_METADATA_MAP_SLOTS == resolved.map_slots);
    POUND_CHECK(resolved.account);

    // The load factor is the reason a probe terminates, so it is asserted rather than
    // assumed: a table that could be filled to capacity would make every lookup
    // after the last free slot a spin.
    POUND_CHECK_MSG((resolved.max_blocks * JIT_METADATA_LOAD_DENOMINATOR)
                        <= (resolved.capacity * JIT_METADATA_LOAD_NUMERATOR),
                    "The default ceiling of %zu addresses is above %u/%u of %zu slots.",
                    resolved.max_blocks,
                    JIT_METADATA_LOAD_NUMERATOR,
                    JIT_METADATA_LOAD_DENOMINATOR,
                    resolved.capacity);
    POUND_CHECK_MSG(resolved.max_blocks < resolved.capacity,
                    "The default ceiling of %zu addresses equals the %zu slots, so a probe has "
                    "no unoccupied slot left to terminate on.",
                    resolved.max_blocks, resolved.capacity);

    // Non-zero, so a zeroed snapshot cannot be mistaken for a real one.
    POUND_CHECK_MSG(0U != resolved.generation, "The manager starts at generation zero.");

    // And the table really is empty, so the reported shape describes a usable table
    // rather than a plausible-looking one.
    size_t index = 0U;

    POUND_CHECK(POUND_ERROR_NOT_FOUND == jit_metadata_lookup(&manager, 0x1000U, &index));

    jit_metadata_destroy(&manager);
}

POUND_TEST(metadata, a_configuration_that_could_not_produce_a_usable_table_is_refused)
{
    jit_metadata_t manager;

    jit_metadata_config_t config;

    memset(&config, 0, sizeof(config));

    // Not a power of two, so a probe index cannot be masked out of the table.
    config.capacity = 1000U;

    POUND_CHECK_MSG(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_init(&manager, &config),
                    "A table of 1000 slots was accepted; a probe index is masked with "
                    "`capacity - 1`, and 999 has no useful mask.");

    // Below the documented minimum.
    config.capacity = JIT_METADATA_MIN_CAPACITY - 1U;

    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_init(&manager, &config));

    // A power of two, but a ceiling above three quarters. This is the one that
    // matters: a table that could be filled to capacity has no unoccupied slot left to
    // end a probe chain.
    config.capacity = JIT_METADATA_MIN_CAPACITY;
    config.max_blocks = config.capacity;

    POUND_CHECK_MSG(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_init(&manager, &config),
                    "A ceiling equal to the capacity was accepted, so once the table fills a "
                    "probe has no unoccupied slot to terminate on and the dispatch loop hangs.");

    // Exactly three quarters is allowed; one more is not. The boundary has to be in the
    // right place, or the refusal above is just refusing something harmless.
    config.max_blocks = (config.capacity * JIT_METADATA_LOAD_NUMERATOR) / JIT_METADATA_LOAD_DENOMINATOR;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_init(&manager, &config));

    POUND_CHECK((config.capacity * JIT_METADATA_LOAD_NUMERATOR) / JIT_METADATA_LOAD_DENOMINATOR
                == manager.max_blocks);

    // A ceiling *below* three quarters is allowed, because a caller that wants a small
    // working set should not have to fill the table to get it.
    config.max_blocks = 8U;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_init(&manager, &config));
    POUND_CHECK(8U == manager.max_blocks);

    jit_metadata_destroy(&manager);

    // A NULL manager.
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_init(NULL, &config));

    // A config with every field left at zero takes every default, statistics included.
    // This is the shape `make_manager` builds, so getting it wrong here would silently
    // turn off the counters for most of this suite -- and the suite would still pass,
    // because every other counter check reads zero and zero is what it expects.
    memset(&config, 0, sizeof(config));

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_init(&manager, &config));

    POUND_CHECK_MSG(JIT_METADATA_DEFAULT_CAPACITY == manager.capacity,
                    "A zero-filled configuration produced %zu slots rather than the default %u.",
                    manager.capacity, (unsigned)JIT_METADATA_DEFAULT_CAPACITY);
    POUND_CHECK_MSG(manager.account,
                    "A zero-filled configuration came up with its counters switched off. `no_accounting` "
                    "is a disable flag so that zero means the default; a positive `account` field would "
                    "have meant the opposite of every other field here and nothing would have said so.");
}

POUND_TEST(metadata, a_rejected_init_leaves_a_manager_that_reports_not_initialised)
{
    jit_metadata_t manager;

    // Poisoned, so a rejected `init` that merely returned early -- rather than
    // clearing the struct first -- would leave every entry point dereferencing the
    // caller's garbage.
    memset(&manager, 0xA5, sizeof(manager));

    jit_metadata_config_t config;

    memset(&config, 0, sizeof(config));

    config.capacity = 3U;

    POUND_REQUIRE(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_init(&manager, &config));

    // Every entry point must now report the manager is not up, rather than acting on
    // the 0xA5 the struct was filled with. Index 0 is included deliberately: an
    // uninitialised manager has zero slots, so an in-range test on its own would call
    // index 0 valid.
    size_t index = 0U;
    jit_metadata_state_t previous = JIT_METADATA_STATE_EMPTY;
    jit_metadata_info_t info;
    jit_metadata_stats_t stats;
    jit_metadata_resolved_t resolved;

    POUND_CHECK(POUND_ERROR_NOT_INITIALIZED == jit_metadata_lookup(&manager, 0x1000U, &index));
    POUND_CHECK(POUND_ERROR_NOT_INITIALIZED == jit_metadata_find_ready(&manager, 0x1000U, &index));
    POUND_CHECK(POUND_ERROR_NOT_INITIALIZED == jit_metadata_intern(&manager, 0x1000U, &index));
    POUND_CHECK(POUND_ERROR_NOT_INITIALIZED == jit_metadata_describe(&manager, &resolved));
    POUND_CHECK(POUND_ERROR_NOT_INITIALIZED == jit_metadata_get_stats(&manager, &stats));

    // The index-taking entry points have no manager-level checks of their own, so
    // they are the ones that depend on the index check noticing the manager is down.
    // Every one of them, at index 0 and at a wildly out-of-range one.
    POUND_CHECK(POUND_ERROR_NOT_INITIALIZED == jit_metadata_try_begin(&manager, 0U, &previous));
    POUND_CHECK(POUND_ERROR_NOT_INITIALIZED == jit_metadata_publish(&manager, 0U, NULL, 0U, 0U, 0U));
    POUND_CHECK(POUND_ERROR_NOT_INITIALIZED
                == jit_metadata_fail(&manager, 0U, POUND_ERROR_TRANSLATION_FAILED, "x"));
    POUND_CHECK(POUND_ERROR_NOT_INITIALIZED == jit_metadata_acquire(&manager, 0U, &info));
    POUND_CHECK(POUND_ERROR_NOT_INITIALIZED == jit_metadata_peek(&manager, 0U, &info));
    POUND_CHECK(POUND_ERROR_NOT_INITIALIZED == jit_metadata_set_label(&manager, 0U, "x"));
    POUND_CHECK(POUND_ERROR_NOT_INITIALIZED == jit_metadata_set_map(&manager, 0U, NULL, 0U));

    // A wildly out-of-range index on a manager that is not up reports the *lifecycle*
    // error, not the range error. Both are true and the manager reports the one the
    // caller has to fix first: there is no such slot in a manager that has no slots,
    // so complaining about the index would send the caller looking in the wrong place.
    POUND_CHECK(POUND_ERROR_NOT_INITIALIZED == jit_metadata_try_begin(&manager, SIZE_MAX, &previous));
    POUND_CHECK(POUND_ERROR_NOT_INITIALIZED == jit_metadata_peek(&manager, SIZE_MAX, &info));

    // The count-returning entry points report zero, which is the safe answer: the
    // caller is asking about a block it has already left.
    POUND_CHECK_MSG(0U == jit_metadata_generation(&manager),
                    "An uninitialised manager reported generation %llu rather than zero.",
                    (unsigned long long)jit_metadata_generation(&manager));
    POUND_CHECK(0U == jit_metadata_leases(&manager, 0U));
    POUND_CHECK(0U == jit_metadata_release(&manager, 0U));

    // And the teardown an aborted start-up would still call is harmless rather than a
    // fault or a double free.
    jit_metadata_destroy(&manager);
    jit_metadata_reset(&manager);
    jit_metadata_log_summary(&manager);
}

// -----------------------------------------------------------------------------
// Interning and lookup
// -----------------------------------------------------------------------------

POUND_TEST(metadata, an_interned_address_gets_a_slot_and_keeps_it)
{
    jit_metadata_t manager;

    POUND_REQUIRE(POUND_SUCCESS == make_manager(&manager));

    size_t first = 0U;
    size_t second = 0U;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_intern(&manager, 0x1000U, &first));
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_intern(&manager, 0x2000U, &second));

    POUND_CHECK_MSG(first != second, "Two different guest addresses were given the same slot.");

    // Interning again must be the same slot, not a second one: the dispatcher calls
    // this on every dispatch to a block it has not published, and a table that grew
    // per dispatch is exactly what the fixed-size design exists to prevent.
    size_t again = SIZE_MAX;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_intern(&manager, 0x1000U, &again));
    POUND_CHECK_MSG(first == again, "Re-interning an address moved it from slot %zu to %zu.",
                    first, again);

    // And a lookup finds it at that slot.
    size_t found = SIZE_MAX;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_lookup(&manager, 0x1000U, &found));
    POUND_CHECK(first == found);

    // Guest address 0 is a legal address, not a sentinel, and must be treated like any
    // other. A table that treated 0 as "no key" would lose the first page of any
    // guest that mapped its code there.
    size_t zero_slot = SIZE_MAX;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_intern(&manager, 0U, &zero_slot));
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_lookup(&manager, 0U, &found));
    POUND_CHECK(zero_slot == found);
    POUND_CHECK_MSG(zero_slot != first, "Guest address 0 and 0x1000 were given the same slot.");

    jit_metadata_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_get_stats(&manager, &stats));
    POUND_CHECK(3U == stats.interned);

    jit_metadata_destroy(&manager);
}

POUND_TEST(metadata, a_lookup_that_finds_nothing_is_a_miss_and_not_a_failure)
{
    jit_metadata_t manager;

    POUND_REQUIRE(POUND_SUCCESS == make_manager(&manager));

    // Left at a value that is neither a valid index nor a plausible slot, so the
    // checks can tell "the miss wrote nothing" from "the miss happened to write what
    // was already there".
    const size_t untouched = 12345U;
    size_t index = untouched;

    POUND_CHECK(POUND_ERROR_NOT_FOUND == jit_metadata_lookup(&manager, 0x1000U, &index));
    POUND_CHECK_MSG(untouched == index, "A lookup miss wrote to the caller's index.");

    POUND_CHECK(POUND_ERROR_NOT_FOUND == jit_metadata_find_ready(&manager, 0x1000U, &index));
    POUND_CHECK(untouched == index);

    // An address that *is* interned but not translated is found by `lookup` and not
    // by `find_ready`. The two answer different questions, and collapsing them would
    // leave the dispatcher unable to tell "never seen" from "seen, not yet translated"
    // without going back and reading the state.
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_intern(&manager, 0x1000U, &index));

    size_t ready_index = untouched;

    POUND_CHECK(POUND_ERROR_NOT_FOUND == jit_metadata_find_ready(&manager, 0x1000U, &ready_index));
    POUND_CHECK(untouched == ready_index);

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_lookup(&manager, 0x1000U, &ready_index));
    POUND_CHECK(index == ready_index);

    // A miss is an ordinary answer, not a failure. Every block is a miss exactly once,
    // so a record per miss would bury the records that matter.
    POUND_CHECK_MSG(0U == pound_test_log_count_at(LOG_LEVEL_ERROR),
                    "A lookup that found nothing logged %u error record(s).",
                    pound_test_log_count_at(LOG_LEVEL_ERROR));

    jit_metadata_destroy(&manager);
}

POUND_TEST(metadata, a_full_table_refuses_new_addresses_and_says_so)
{
    jit_metadata_t manager;

    POUND_REQUIRE(POUND_SUCCESS == make_manager(&manager));

    // Exactly the ceiling, which must all succeed.
    for (size_t i = 0U; i < manager.max_blocks; ++i)
    {
        size_t index = 0U;

        const error_t status = jit_metadata_intern(&manager, (uint64_t)(0x10000U + (i * 4U)), &index);

        POUND_REQUIRE_MSG(POUND_SUCCESS == status,
                          "Interning address %zu of %zu failed: %s",
                          i, manager.max_blocks, pound_error_to_string(status));
    }

    // One more must be refused, and refused *loudly*. A JIT that has stopped growing is
    // a condition an operator needs to see, because from that moment nothing new runs
    // at native speed.
    pound_test_log_reset();

    size_t index = 0U;

    POUND_CHECK(POUND_ERROR_ALLOCATION_FAILED
                == jit_metadata_intern(&manager, UINT64_C(0x7FFFFFFF0000), &index));

    POUND_CHECK_MSG(pound_test_log_contains("The metadata table is full"),
                    "A full table refused an address without saying so.");

    // The refusal changed nothing: every address already interned is still there, and
    // the refused one is not.
    jit_metadata_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_get_stats(&manager, &stats));
    POUND_CHECK(manager.max_blocks == stats.interned);
    POUND_CHECK(POUND_ERROR_NOT_FOUND
                == jit_metadata_lookup(&manager, UINT64_C(0x7FFFFFFF0000), &index));

    // A lookup that probes a three-quarters-full table still terminates. That is the
    // property the load factor buys, and it is only observable at the ceiling, so it
    // can only be asserted here.
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_lookup(&manager, 0x10000U, &index));

    // And the table is still usable for the addresses it does have, which is the whole
    // point of stopping rather than failing.
    fake_code_t code;

    POUND_REQUIRE(POUND_SUCCESS == publish_block(&manager, 0x10000U, code.bytes, 8U, 4U));

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_find_ready(&manager, 0x10000U, &index));

    jit_metadata_destroy(&manager);
}

// -----------------------------------------------------------------------------
// The claim protocol
// -----------------------------------------------------------------------------

POUND_TEST(metadata, a_block_is_translated_by_exactly_one_claimant)
{
    jit_metadata_t manager;

    POUND_REQUIRE(POUND_SUCCESS == make_manager(&manager));

    size_t index = 0U;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_intern(&manager, 0x1000U, &index));

    jit_metadata_state_t previous = JIT_METADATA_STATE_EMPTY;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_try_begin(&manager, index, &previous));

    POUND_CHECK_MSG(JIT_METADATA_STATE_EMPTY == previous,
                    "A successful claim reported previous state \"%s\" rather than the state it "
                    "must have started from.",
                    jit_metadata_state_to_string(previous));

    // The second claimant loses, and is told it lost to a *racer* rather than to a
    // block that is already done. That distinction is the entire point of reporting the
    // previous state: "wait for it" and "run the interpreter" are different responses.
    jit_metadata_state_t second_previous = JIT_METADATA_STATE_VACANT;

    POUND_CHECK(POUND_ERROR_ALREADY_INITIALIZED
                == jit_metadata_try_begin(&manager, index, &second_previous));

    POUND_CHECK_MSG(JIT_METADATA_STATE_CLAIMED == second_previous,
                    "A second claim reported previous state \"%s\" rather than \"%s\".",
                    jit_metadata_state_to_string(second_previous),
                    jit_metadata_state_to_string(JIT_METADATA_STATE_CLAIMED));

    // A contended claim is a race, not a failure, so it must not raise an error record.
    // Two cores reaching the same cold block is the ordinary case, and a record per
    // occurrence would drown the log for the whole session.
    POUND_CHECK_MSG(0U == pound_test_log_count_at(LOG_LEVEL_ERROR),
                    "A contended claim logged %u error record(s).",
                    pound_test_log_count_at(LOG_LEVEL_ERROR));

    fake_code_t code;

    POUND_REQUIRE(POUND_SUCCESS
                  == jit_metadata_publish(&manager, index, code.bytes, sizeof(code.bytes), 64U, 0U));

    // And now it is already done, which is a third previous state again.
    POUND_CHECK(POUND_ERROR_ALREADY_INITIALIZED
                == jit_metadata_try_begin(&manager, index, &second_previous));
    POUND_CHECK(JIT_METADATA_STATE_READY == second_previous);

    // A block that was never claimed cannot be published over either -- but that is a
    // different entry point with its own case, so here the check is only that the
    // loser of the race left the winner's block intact.
    jit_metadata_info_t info;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_acquire(&manager, index, &info));
    POUND_CHECK_PTR_EQ(code.bytes, info.host_code);
    POUND_CHECK(0U == jit_metadata_release(&manager, index));

    jit_metadata_destroy(&manager);
}

POUND_TEST(metadata, a_block_that_was_never_claimed_cannot_be_published_into)
{
    jit_metadata_t manager;

    POUND_REQUIRE(POUND_SUCCESS == make_manager(&manager));

    fake_code_t code;

    size_t interned = 0U;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_intern(&manager, 0x1000U, &interned));

    // Publishing into a block nobody claimed would hand out code nobody emitted, and
    // the dispatcher's only defence is the state -- so the refusal has to be here.
    POUND_CHECK_MSG(POUND_ERROR_ALREADY_INITIALIZED
                        == jit_metadata_publish(&manager, interned, code.bytes,
                                                sizeof(code.bytes), 64U, 0U),
                    "An unclaimed block accepted a publication, so a dispatcher could have jumped "
                    "to machine code no translator emitted.");

    // Even after the block has been published once, a second publication is refused: it
    // is `READY`, not `CLAIMED`.
    POUND_REQUIRE(POUND_SUCCESS
                  == publish_block(&manager, 0x1000U, code.bytes, sizeof(code.bytes), 64U));

    POUND_CHECK(POUND_ERROR_ALREADY_INITIALIZED
                == jit_metadata_publish(&manager, interned, code.bytes, sizeof(code.bytes), 64U, 0U));

    // And neither refusal clobbered the payload.
    jit_metadata_info_t info;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_acquire(&manager, interned, &info));

    POUND_CHECK(sizeof(code.bytes) == info.host_size);
    POUND_CHECK(64U == info.guest_size);
    POUND_CHECK_PTR_EQ(code.bytes, info.host_code);

    POUND_CHECK(0U == jit_metadata_release(&manager, interned));

    jit_metadata_destroy(&manager);
}

POUND_TEST(metadata, a_publication_that_could_not_be_described_is_refused)
{
    jit_metadata_t manager;

    POUND_REQUIRE(POUND_SUCCESS == make_manager(&manager));

    fake_code_t code;

    size_t index = 0U;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_intern(&manager, 0x1000U, &index));

    jit_metadata_state_t previous = JIT_METADATA_STATE_EMPTY;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_try_begin(&manager, index, &previous));

    // A block with bytes of host code at no address. Storing it would give the
    // dispatcher a `READY` block it would jump to NULL.
    POUND_CHECK_MSG(POUND_ERROR_INVALID_ARGUMENT
                        == jit_metadata_publish(&manager, index, NULL, sizeof(code.bytes), 64U, 0U),
                    "A block with %u bytes of host code at no address was accepted.",
                    (unsigned)sizeof(code.bytes));

    // No address *and* no bytes is fine: an interpreter block has no host code, and
    // refusing it would mean the dispatcher could not record that a block is running
    // through the interpreter.
    POUND_REQUIRE(POUND_SUCCESS
                  == jit_metadata_publish(&manager, index, NULL, 0U, 0U, JIT_METADATA_FLAG_INTERPRETED));

    // A flag bit nobody defined. Storing it would be a behaviour change in the
    // translator that no reader could see, which is the worst kind of typo.
    size_t other = 0U;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_intern(&manager, 0x2000U, &other));
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_try_begin(&manager, other, &previous));

    POUND_CHECK_MSG(POUND_ERROR_INVALID_ARGUMENT
                        == jit_metadata_publish(&manager, other, code.bytes, 8U, 8U, 0x80000000U),
                    "An undefined flag bit was accepted, so a typo in the translator would become "
                    "an invisible behaviour change.");

    // Every defined flag together, which must be accepted -- otherwise the check is
    // just refusing everything.
    size_t third = 0U;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_intern(&manager, 0x3000U, &third));
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_try_begin(&manager, third, &previous));

    const uint32_t all_flags = JIT_METADATA_FLAG_SYSCALL | JIT_METADATA_FLAG_ENTRY_POINT |
                               JIT_METADATA_FLAG_INTERPRETED | JIT_METADATA_FLAG_COLD;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_publish(&manager, third, code.bytes, 8U, 8U, all_flags));

    jit_metadata_info_t info;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_peek(&manager, third, &info));
    POUND_CHECK(all_flags == info.flags);

    jit_metadata_destroy(&manager);
}

// -----------------------------------------------------------------------------
// Failure
// -----------------------------------------------------------------------------

POUND_TEST(metadata, a_block_that_cannot_be_translated_says_why_and_stays_that_way)
{
    jit_metadata_t manager;

    POUND_REQUIRE(POUND_SUCCESS == make_manager(&manager));

    size_t index = 0U;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_intern(&manager, 0x1000U, &index));

    // A diagnostic not terminated within the field would be cut off exactly where the
    // interesting part is, so it is refused.
    char unterminated[JIT_METADATA_LABEL_MAX];

    memset(unterminated, 'x', sizeof(unterminated));

    jit_metadata_state_t previous = JIT_METADATA_STATE_EMPTY;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_try_begin(&manager, index, &previous));

    POUND_CHECK_MSG(POUND_ERROR_INVALID_ARGUMENT
                        == jit_metadata_fail(&manager, index,
                                             POUND_ERROR_UNSUPPORTED_INSTRUCTION, unterminated),
                    "An unterminated diagnostic was accepted, so the part that said what went "
                    "wrong would have been the part that got cut.");

    // The refusal did not record a failure, so the claim is still owned and a real
    // failure can still be recorded against it. This is the property that makes
    // "refuse" different from "accept and truncate".
    POUND_REQUIRE(POUND_SUCCESS
                  == jit_metadata_fail(&manager, index, POUND_ERROR_UNSUPPORTED_INSTRUCTION,
                                       "encoding 0x1f2e3d4c"));

    error_t reason = POUND_SUCCESS;
    char detail[JIT_METADATA_LABEL_MAX];

    memset(detail, 0, sizeof(detail));

    POUND_REQUIRE(POUND_SUCCESS
                  == jit_metadata_failure_reason(&manager, index, &reason, detail, sizeof(detail)));

    POUND_CHECK(POUND_ERROR_UNSUPPORTED_INSTRUCTION == reason);
    POUND_CHECK_STR_EQ("encoding 0x1f2e3d4c", detail);

    // It is not `READY`, so the dispatcher will not run it.
    size_t ready_index = 0U;

    POUND_CHECK(POUND_ERROR_NOT_FOUND == jit_metadata_find_ready(&manager, 0x1000U, &ready_index));

    // And it stays that way. A claim against it is refused and reported at *error*
    // level, because a dispatcher that keeps asking for a block it already knows is
    // untranslatable has a bug of its own -- the opposite of a contended claim.
    pound_test_log_reset();

    POUND_CHECK(POUND_ERROR_ALREADY_INITIALIZED == jit_metadata_try_begin(&manager, index, &previous));
    POUND_CHECK(JIT_METADATA_STATE_FAILED == previous);
    POUND_CHECK_MSG(0U < pound_test_log_count_at(LOG_LEVEL_ERROR),
                    "Claiming a block already recorded as untranslatable logged nothing; the "
                    "dispatcher has asked for a block it should have remembered.");

    // A block that is not failed has no reason, and asking for one is a mistake worth
    // a record.
    size_t healthy = 0U;

    POUND_REQUIRE(POUND_SUCCESS == publish_block(&manager, 0x2000U, NULL, 0U, 0U));
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_lookup(&manager, 0x2000U, &healthy));

    POUND_CHECK(POUND_ERROR_NOT_INITIALIZED
                == jit_metadata_failure_reason(&manager, healthy, &reason, NULL, 0U));

    // A NULL detail is legitimate and records the reason with no further detail.
    size_t terse = 0U;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_intern(&manager, 0x3000U, &terse));
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_try_begin(&manager, terse, &previous));
    POUND_REQUIRE(POUND_SUCCESS
                  == jit_metadata_fail(&manager, terse, POUND_ERROR_TRANSLATION_FAILED, NULL));

    memset(detail, 'z', sizeof(detail));

    POUND_REQUIRE(POUND_SUCCESS
                  == jit_metadata_failure_reason(&manager, terse, &reason, detail, sizeof(detail)));

    POUND_CHECK(POUND_ERROR_TRANSLATION_FAILED == reason);
    POUND_CHECK_STR_EQ("", detail);

    // A one-byte detail buffer still gets a terminator. An unterminated string handed
    // back to a caller is worse than a truncated one.
    char tiny[2] = {'x', 'x'};

    POUND_REQUIRE(POUND_SUCCESS
                  == jit_metadata_failure_reason(&manager, terse, &reason, tiny, sizeof(tiny)));

    POUND_CHECK_MSG('\0' == tiny[0], "A two-byte detail buffer was not terminated.");

    // A NULL detail buffer with a non-zero size is a caller mistake, not something to
    // quietly ignore.
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT
                == jit_metadata_failure_reason(&manager, terse, &reason, NULL, 8U));

    // And a failure recorded against a block nobody claimed would silence a
    // translation that is about to succeed.
    size_t unclaimed = 0U;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_intern(&manager, 0x4000U, &unclaimed));
    POUND_CHECK(POUND_ERROR_ALREADY_INITIALIZED
                == jit_metadata_fail(&manager, unclaimed, POUND_ERROR_TRANSLATION_FAILED, "no"));

    jit_metadata_destroy(&manager);
}

// -----------------------------------------------------------------------------
// Leases
// -----------------------------------------------------------------------------

POUND_TEST(metadata, a_lease_is_held_for_exactly_as_long_as_the_block_is_running)
{
    jit_metadata_t manager;

    POUND_REQUIRE(POUND_SUCCESS == make_manager(&manager));

    fake_code_t code;

    POUND_REQUIRE(POUND_SUCCESS
                  == publish_block(&manager, 0x1000U, code.bytes, sizeof(code.bytes), 64U));

    // The slot the block landed in, which is not slot 0 -- that is the whole reason the
    // index-taking entry points cannot be exercised with a hard-coded one.
    size_t index = 0U;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_lookup(&manager, 0x1000U, &index));

    POUND_CHECK(0U == jit_metadata_leases(&manager, index));

    // Two threads inside the same block. Each snapshot counts the thread that took
    // it, so an uncontended block reports 1 rather than 0 -- a dispatcher reading
    // `leases == 1` means "nobody else is here" without having to know the
    // convention and subtract one.
    jit_metadata_info_t first;
    jit_metadata_info_t second;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_acquire(&manager, index, &first));

    POUND_CHECK_MSG(1U == first.leases, "The first acquire reported %u lease(s) rather than only "
                                       "its own.",
                    first.leases);

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_acquire(&manager, index, &second));

    POUND_CHECK_MSG(2U == second.leases,
                    "The second acquire reported %u lease(s) rather than the two threads that are "
                    "now inside the block.",
                    second.leases);

    POUND_CHECK(2U == jit_metadata_leases(&manager, index));

    jit_metadata_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_get_stats(&manager, &stats));
    POUND_CHECK_MSG(2U == stats.leases_outstanding,
                    "Two leases were taken but the manager counted %zu.",
                    stats.leases_outstanding);

    // Each release reports what is left, so a dispatcher can spot a block that never
    // drains.
    POUND_CHECK(1U == jit_metadata_release(&manager, index));
    POUND_CHECK(0U == jit_metadata_release(&manager, index));
    POUND_CHECK(0U == jit_metadata_leases(&manager, index));

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_get_stats(&manager, &stats));
    POUND_CHECK_MSG(0U == stats.leases_outstanding,
                    "%zu lease(s) are still counted after both were released.",
                    stats.leases_outstanding);

    // The snapshot is the one the dispatcher uses to decide where to jump, so its
    // fields are what the block is actually made of.
    POUND_CHECK_PTR_EQ(code.bytes, first.host_code);
    POUND_CHECK(sizeof(code.bytes) == first.host_size);
    POUND_CHECK(64U == first.guest_size);
    POUND_CHECK(0x1000U == first.guest_pc);
    POUND_CHECK((uint32_t)JIT_METADATA_STATE_READY == first.state);
    POUND_CHECK(0U == first.flags);

    // An empty register map, and not whatever the allocator left in the entry. The
    // table is zeroed once at start-up rather than on each `intern`, so this is the
    // case that proves it: a block that was never given a map has to report none,
    // because `get_map` memcpy's `map_used` entries out of the entry and an
    // uninitialised count there overflows the caller's array.
    POUND_CHECK_MSG(0U == first.map_used,
                    "A block that was never given a register map reports %u entries in use.",
                    first.map_used);

    // Releasing a block that was never entered means the dispatcher is tracking blocks
    // it never entered, which makes the counts invalidation gates on wrong. That is a
    // bug worth an error record.
    pound_test_log_reset();

    POUND_CHECK(0U == jit_metadata_release(&manager, index));

    POUND_CHECK_MSG(0U < pound_test_log_count_at(LOG_LEVEL_ERROR),
                    "A release with no matching acquire logged nothing; the lease counts "
                    "invalidation depends on are already wrong.");
}

POUND_TEST(metadata, a_block_that_is_not_ready_cannot_be_acquired)
{
    jit_metadata_t manager;

    POUND_REQUIRE(POUND_SUCCESS == make_manager(&manager));

    size_t index = 0U;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_intern(&manager, 0x1000U, &index));

    jit_metadata_info_t info;

    // `EMPTY`: interned, nothing there yet. This is the dispatch miss, and it must not
    // raise an error record.
    POUND_CHECK(POUND_ERROR_NOT_INITIALIZED == jit_metadata_acquire(&manager, index, &info));
    POUND_CHECK(0U == pound_test_log_count_at(LOG_LEVEL_ERROR));

    // `CLAIMED`: somebody is writing it, so its bytes are not readable yet.
    jit_metadata_state_t previous = JIT_METADATA_STATE_EMPTY;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_try_begin(&manager, index, &previous));

    POUND_CHECK_MSG(POUND_ERROR_NOT_INITIALIZED == jit_metadata_acquire(&manager, index, &info),
                    "A block being translated was acquirable, so a dispatcher could have executed "
                    "machine code that was still being emitted.");

    // `FAILED`.
    POUND_REQUIRE(POUND_SUCCESS
                  == jit_metadata_fail(&manager, index, POUND_ERROR_TRANSLATION_FAILED, "nope"));

    POUND_CHECK(POUND_ERROR_NOT_INITIALIZED == jit_metadata_acquire(&manager, index, &info));

    // A slot that was never interned at all. The manager has 256 slots, so 0 is in
    // range but holds no key; 256 is out of range. Both are refused, and for different
    // reasons.
    POUND_CHECK(POUND_ERROR_NOT_INITIALIZED == jit_metadata_acquire(&manager, 0U, &info));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT
                == jit_metadata_acquire(&manager, JIT_METADATA_MIN_CAPACITY, &info));

    // None of those refusals moved a lease count, so invalidation is not being blocked
    // by leases that were never taken.
    POUND_CHECK(0U == jit_metadata_leases(&manager, index));

    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_acquire(&manager, index, NULL));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_acquire(NULL, 0U, &info));

    jit_metadata_destroy(&manager);
}

// -----------------------------------------------------------------------------
// Invalidation
// -----------------------------------------------------------------------------

POUND_TEST(metadata, an_invalidation_clears_the_blocks_the_guest_overwrote)
{
    jit_metadata_t manager;

    POUND_REQUIRE(POUND_SUCCESS == make_manager(&manager));

    fake_code_t code;

    // Five adjacent 128-byte blocks from 0x1000 to 0x1200.
    for (size_t i = 0U; i < 5U; ++i)
    {
        POUND_REQUIRE(POUND_SUCCESS
                      == publish_block(&manager, (uint64_t)(0x1000U + (i * 0x80U)),
                                       code.bytes, sizeof(code.bytes), 0x80U));
    }

    const uint64_t generation_before = jit_metadata_generation(&manager);

    // Half-open, and exactly one block wide: [0x1080, 0x1100) is the whole of the
    // 0x1080 block and neither byte of either neighbour.
    size_t cleared = 0U;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_invalidate_range(&manager, 0x1080U, 0x1100U, &cleared));

    POUND_CHECK_MSG(1U == cleared,
                    "Invalidating a range that covers exactly the 0x1080 block cleared %zu "
                    "block(s) rather than one.",
                    cleared);

    // The neighbour below and the two above survived, so the range is not "a block or
    // two either side of what you asked for". A manager that cleared too much is worse
    // than one that cleared too little: it frees code the guest is still running.
    size_t ready_index = 0U;

    POUND_CHECK_MSG(POUND_SUCCESS == jit_metadata_find_ready(&manager, 0x1000U, &ready_index),
                    "The block below the invalidated range was cleared as well.");
    POUND_CHECK_MSG(POUND_ERROR_NOT_FOUND == jit_metadata_find_ready(&manager, 0x1080U, &ready_index),
                    "The block inside the invalidated range survived.");
    POUND_CHECK_MSG(POUND_SUCCESS == jit_metadata_find_ready(&manager, 0x1100U, &ready_index),
                    "The block above the invalidated range was cleared as well.");
    POUND_CHECK(POUND_SUCCESS == jit_metadata_find_ready(&manager, 0x1180U, &ready_index));
    POUND_CHECK(POUND_SUCCESS == jit_metadata_find_ready(&manager, 0x1200U, &ready_index));

    // A partial overlap at either end still counts, because the guest overwrote part of
    // the block and the rest of it is now the wrong bytes too.
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_invalidate_range(&manager, 0x11C0U, 0x11D0U, &cleared));
    POUND_CHECK_MSG(1U == cleared, "A range inside the last block cleared %zu rather than 1.",
                    cleared);

    // The generation moved, so a dispatcher holding a cached `(index, generation)` pair
    // can tell its cache went stale.
    POUND_CHECK_MSG(jit_metadata_generation(&manager) != generation_before,
                    "Invalidations that cleared %zu block(s) left the generation at %llu.",
                    cleared, (unsigned long long)generation_before);

    jit_metadata_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_get_stats(&manager, &stats));

    // Five blocks published, two cleared -- the one inside [0x1080, 0x1100) and the one
    // the 16-byte range at 0x11C0 landed in.
    POUND_CHECK_MSG(3U == stats.ready, "%zu of the 3 surviving blocks are ready.", stats.ready);
    POUND_CHECK(0U == stats.claimed);
    POUND_CHECK(0U == stats.failed);

    // Cleared blocks no longer count as interned, because they are not blocks any
    // more. They do keep their slots, which is the price of not splitting the table's
    // chains.
    POUND_CHECK_MSG(3U == stats.interned, "%zu of the 5 published blocks still count as interned.",
                    stats.interned);

    // The block at 0x1080 is put back. Reclaiming a vacated slot is exactly the path
    // where the running counter and the recount can diverge: the slot's key was
    // already there, so nothing *new* was interned, yet the entry is live again and
    // has to be counted.
    //
    // Expressed as a round trip rather than by reaching into the struct, because the
    // symptom of getting it wrong is not a wrong number here -- it is a table that
    // reports itself full a few operations later.
    size_t reused = 0U;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_intern(&manager, 0x1080U, &reused));

    jit_metadata_stats_t after_reuse;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_get_stats(&manager, &after_reuse));

    POUND_CHECK_MSG(4U == after_reuse.interned,
                    "Re-interning an invalidated address left the manager counting %zu address(es) "
                    "rather than 4.",
                    after_reuse.interned);

    // And with the table's count correct, new addresses still fit. A counter that had
    // wrapped would report the table full here and refuse, long after the entry that
    // wrapped it had been cleared.
    size_t fresh = 0U;

    POUND_CHECK_MSG(POUND_SUCCESS == jit_metadata_intern(&manager, 0xE000U, &fresh),
                    "The table reported itself full with %zu of %zu addresses in it.",
                    after_reuse.interned, manager.max_blocks);

    jit_metadata_destroy(&manager);
}

POUND_TEST(metadata, every_interned_address_is_still_findable_after_its_own_slot_is_invalidated)
{
    jit_metadata_t manager;

    POUND_REQUIRE(POUND_SUCCESS == make_manager(&manager));

    fake_code_t code;

    // Three quarters of the table, so the address count pushes the load factor to the
    // ceiling. A probe chain is only worth testing if there is one, and at 192 of 256
    // slots there are certainly chains longer than a single slot -- which the probe
    // statistics below confirm rather than assume.
    enum
    {
        ADDRESS_COUNT = 192U,
    };

    size_t slots[ADDRESS_COUNT];

    for (size_t i = 0U; i < ADDRESS_COUNT; ++i)
    {
        const uint64_t guest_pc = 0x10000000ULL + ((uint64_t)i * 0x1000ULL);

        const error_t status = jit_metadata_intern(&manager, guest_pc, &slots[i]);

        POUND_REQUIRE_MSG(POUND_SUCCESS == status,
                          "Interning address %zu of %u failed: %s",
                          i, (unsigned)ADDRESS_COUNT, pound_error_to_string(status));
    }

    // The addresses really are spread over chains longer than one slot, or this case is
    // not testing what it claims to.
    jit_metadata_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_get_stats(&manager, &stats));

    POUND_CHECK_MSG(stats.probe_worst > 1U,
                    "The longest probe was one slot in a table filled to %u/%u, so there is no "
                    "chain here for the invalidation to break.",
                    (unsigned)ADDRESS_COUNT, (unsigned)manager.capacity);

    // Publish them all, so the table is full of real blocks rather than empty keys.
    for (size_t i = 0U; i < ADDRESS_COUNT; ++i)
    {
        jit_metadata_state_t previous = JIT_METADATA_STATE_EMPTY;

        const size_t slot = slots[i];

        POUND_REQUIRE(POUND_SUCCESS == jit_metadata_try_begin(&manager, slot, &previous));
        POUND_REQUIRE(POUND_SUCCESS == jit_metadata_publish(&manager, slot, code.bytes, 8U, 0x10U, 0U));
    }

    // Invalidate every single one of them, in a handful of ranges.
    size_t cleared = 0U;

    POUND_REQUIRE(POUND_SUCCESS
                  == jit_metadata_invalidate_range(&manager, 0x10000000ULL, 0x20000000ULL, &cleared));

    POUND_CHECK_MSG(ADDRESS_COUNT == cleared, "Clearing everything cleared %zu of %u blocks.",
                    cleared, (unsigned)ADDRESS_COUNT);

    // Every one must now be findable by key again, and on *its own* slot. This is the
    // invariant the whole fixed-size design rests on: if invalidation had cleared the
    // slots outright, the entries behind each split chain would be unreachable from
    // their home slot and would come back on a different one -- or not at all.
    for (size_t i = 0U; i < ADDRESS_COUNT; ++i)
    {
        const uint64_t guest_pc = 0x10000000ULL + ((uint64_t)i * 0x1000ULL);
        size_t found = SIZE_MAX;

        POUND_CHECK_MSG(POUND_SUCCESS == jit_metadata_intern(&manager, guest_pc, &found),
                        "Address 0x%llx could not be re-interned after every slot was invalidated, "
                        "so its probe chain was split by the invalidation.",
                        (unsigned long long)guest_pc);

        POUND_CHECK_MSG(slots[i] == found,
                        "Address 0x%llx was given slot %zu after invalidation rather than its own "
                        "slot %zu.",
                        (unsigned long long)guest_pc, found, slots[i]);
    }

    // And they are all translatable again, which is what a self-modifying guest does on
    // the very next dispatch. A single failure here means some slot could not be
    // re-claimed even though it could be re-found.
    for (size_t i = 0U; i < ADDRESS_COUNT; ++i)
    {
        const size_t slot = slots[i];
        jit_metadata_state_t previous = JIT_METADATA_STATE_EMPTY;

        POUND_CHECK_MSG(POUND_SUCCESS == jit_metadata_try_begin(&manager, slot, &previous),
                        "Slot %zu could be found but not claimed after a full invalidation.",
                        slot);
    }

    jit_metadata_destroy(&manager);
}

// The invariant that makes a fixed-size table work at all: invalidating one block
// must not hide the blocks that were stored *behind* it.
//
// A linear-probe slot is only safe to clear outright if nothing can ever probe past
// it. It can, so invalidation keeps the key in a `VACANT` slot instead. Clearing the
// slot to `UNOCCUPIED` instead does not merely lose the key -- it turns that slot into
// a wall. Every address whose probe passed through it now terminates there and reports
// "no such address", for a block that is published, leased and running.
POUND_TEST(metadata, an_invalidated_slot_does_not_hide_the_blocks_behind_it)
{
    jit_metadata_t manager;

    POUND_REQUIRE(POUND_SUCCESS == make_manager(&manager));

    fake_code_t code;

    enum
    {
        ADDRESS_COUNT = 192U,
    };

    size_t slots[ADDRESS_COUNT];
    size_t homes[ADDRESS_COUNT];

    jit_metadata_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_get_stats(&manager, &stats));

    for (size_t i = 0U; i < ADDRESS_COUNT; ++i)
    {
        const uint64_t guest_pc = 0x10000000ULL + ((uint64_t)i * 0x1000ULL);
        const uint_least64_t before = stats.probe_total;

        POUND_REQUIRE(POUND_SUCCESS == jit_metadata_intern(&manager, guest_pc, &slots[i]));
        POUND_REQUIRE(POUND_SUCCESS == jit_metadata_get_stats(&manager, &stats));

        // `probe_total` advances by the number of slots the probe walked, so the slot an
        // address *started* at is recoverable from the public statistics: a probe that
        // walked n slots ended n - 1 slots after its home. Without that, there is no way
        // from outside the module to know which slots a given address's chain covers --
        // and which slots a chain covers is the whole subject of this case.
        const size_t walked = (size_t)(stats.probe_total - before);

        POUND_REQUIRE_MSG(walked >= 1U, "Interning address %zu walked no slots at all.", i);
        POUND_REQUIRE_MSG((walked - 1U) <= slots[i],
                          "Address %zu walked %zu slot(s) but landed on slot %zu, so its home slot "
                          "would be negative.",
                          i,
                          walked,
                          slots[i]);

        homes[i] = slots[i] - (walked - 1U);

        jit_metadata_state_t previous = JIT_METADATA_STATE_EMPTY;

        POUND_REQUIRE(POUND_SUCCESS == jit_metadata_try_begin(&manager, slots[i], &previous));
        POUND_REQUIRE(POUND_SUCCESS == jit_metadata_publish(&manager, slots[i], code.bytes, 8U, 0x10U, 0U));
    }

    POUND_REQUIRE_MSG(stats.probe_worst > 1U,
                      "The longest probe was one slot across %u addresses, so no chain exists here "
                      "and this case would pass whatever the invalidation did.",
                      (unsigned)ADDRESS_COUNT);

    // A victim that something else probes *past*. Found rather than chosen, because a
    // slot nothing probes through cannot demonstrate anything: clearing it splits no
    // chain, which is exactly why a weaker version of this case passes against a
    // broken implementation.
    size_t victim = ADDRESS_COUNT;
    size_t witness = ADDRESS_COUNT;

    for (size_t i = 0U; i < ADDRESS_COUNT; ++i)
    {
        for (size_t j = 0U; j < ADDRESS_COUNT; ++j)
        {
            if ((i != j) && (homes[j] <= slots[i]) && (slots[i] <= slots[j]))
            {
                victim = i;
                witness = j;

                break;
            }
        }

        if (victim < ADDRESS_COUNT)
        {
            break;
        }
    }

    POUND_REQUIRE_MSG(victim < ADDRESS_COUNT,
                      "No slot in a table of %u addresses is probed past by another address, so there "
                      "is no chain here for the invalidation to break.",
                      (unsigned)ADDRESS_COUNT);

    const uint64_t victim_pc = 0x10000000ULL + ((uint64_t)victim * 0x1000ULL);
    const uint64_t witness_pc = 0x10000000ULL + ((uint64_t)witness * 0x1000ULL);

    size_t cleared = 0U;

    POUND_REQUIRE(POUND_SUCCESS
                  == jit_metadata_invalidate_range(&manager, victim_pc, victim_pc + 0x10U, &cleared));

    POUND_CHECK_MSG(1U == cleared, "Invalidating one block's range cleared %zu.", cleared);

    // The witness: stored at a slot at or past the one that was just vacated, so its
    // probe now walks the vacated slot on its way to its own. Its block was not
    // invalidated, was never claimed by anybody else, and is still published.
    size_t found = SIZE_MAX;

    POUND_CHECK_MSG(POUND_SUCCESS == jit_metadata_lookup(&manager, witness_pc, &found),
                    "Address 0x%llx probes through slot %zu, which was just vacated, and cannot be "
                    "found any more. Vacating the slot turned it into the end of a chain it was "
                    "never the end of.",
                    (unsigned long long)witness_pc,
                    slots[victim]);

    POUND_CHECK_MSG(slots[witness] == found,
                    "Address 0x%llx came back on slot %zu rather than its own slot %zu, so the "
                    "invalidation moved it.",
                    (unsigned long long)witness_pc,
                    found,
                    slots[witness]);

    // Not only the witness: nothing that survives an invalidation may become
    // unreachable. One address per chain position would be a sample of the symptom;
    // this is the whole table.
    for (size_t i = 0U; i < ADDRESS_COUNT; ++i)
    {
        if (i == victim)
        {
            continue;
        }

        const uint64_t guest_pc = 0x10000000ULL + ((uint64_t)i * 0x1000ULL);

        found = SIZE_MAX;

        POUND_CHECK_MSG(POUND_SUCCESS == jit_metadata_find_ready(&manager, guest_pc, &found),
                        "Address 0x%llx became unfindable after an unrelated block at 0x%llx was "
                        "invalidated.",
                        (unsigned long long)guest_pc,
                        (unsigned long long)victim_pc);
    }

    // And the vacated address is reclaimable on its own slot, by the same address only.
    found = SIZE_MAX;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_intern(&manager, victim_pc, &found));
    POUND_CHECK_MSG(slots[victim] == found,
                    "The invalidated address came back on slot %zu rather than its own slot %zu.",
                    found,
                    slots[victim]);

    jit_metadata_destroy(&manager);
}

POUND_TEST(metadata, an_invalidation_that_would_catch_a_running_block_is_refused_whole)
{
    jit_metadata_t manager;

    POUND_REQUIRE(POUND_SUCCESS == make_manager(&manager));

    fake_code_t code;

    // Two adjacent blocks, both inside the range to be invalidated.
    size_t running = 0U;
    size_t idle = 0U;

    POUND_REQUIRE(POUND_SUCCESS
                  == publish_block(&manager, 0x1000U, code.bytes, sizeof(code.bytes), 0x80U));
    POUND_REQUIRE(POUND_SUCCESS
                  == publish_block(&manager, 0x1080U, code.bytes, sizeof(code.bytes), 0x80U));

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_lookup(&manager, 0x1000U, &running));
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_lookup(&manager, 0x1080U, &idle));

    // The CPU thread is inside the first one.
    jit_metadata_info_t info;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_acquire(&manager, running, &info));

    size_t cleared = 12345U;

    pound_test_log_reset();

    POUND_CHECK_MSG(POUND_ERROR_BUSY
                        == jit_metadata_invalidate_range(&manager, 0x1000U, 0x1100U, &cleared),
                    "An invalidation caught a block a thread was executing, so its host code "
                    "could have been freed underneath that thread.");

    POUND_CHECK_MSG(pound_test_log_contains("lease"),
                    "The refusal did not say a lease was the reason.");
    POUND_CHECK_MSG(pound_test_log_contains("0x1000"),
                    "The refusal did not name the block that was holding the lease.");

    // All or nothing. Clearing the second block and refusing the first would leave a
    // guest running a mixture of blocks translated against the old bytes and the new --
    // a state no title is prepared for.
    POUND_CHECK_MSG(0U == cleared, "A refused invalidation still reported %zu block(s) cleared.",
                    cleared);

    size_t ready_index = 0U;

    POUND_CHECK_MSG(POUND_SUCCESS == jit_metadata_find_ready(&manager, 0x1080U, &ready_index),
                    "The unleased block in the refused range was cleared anyway, so the "
                    "invalidation was not all-or-nothing.");

    // Once the thread leaves, the same call succeeds.
    POUND_CHECK(0U == jit_metadata_release(&manager, running));

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_invalidate_range(&manager, 0x1000U, 0x1100U, &cleared));
    POUND_CHECK(2U == cleared);

    POUND_CHECK(POUND_ERROR_NOT_FOUND == jit_metadata_find_ready(&manager, 0x1080U, &ready_index));

    // And the now-stale pointer is gone from the snapshot too, which is what lets the
    // caller release the memory.
    POUND_CHECK(POUND_ERROR_NOT_INITIALIZED == jit_metadata_peek(&manager, running, &info));

    jit_metadata_destroy(&manager);
}

POUND_TEST(metadata, an_invalidation_range_is_half_open_and_may_reach_the_top_of_the_address_space)
{
    jit_metadata_t manager;

    POUND_REQUIRE(POUND_SUCCESS == make_manager(&manager));

    fake_code_t code;

    // A range that ends exactly where the block begins does not touch it.
    POUND_REQUIRE(POUND_SUCCESS
                  == publish_block(&manager, 0x1000U, code.bytes, sizeof(code.bytes), 0x80U));

    size_t cleared = 99U;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_invalidate_range(&manager, 0x0F00U, 0x1000U, &cleared));

    POUND_CHECK_MSG(0U == cleared, "A range ending exactly at a block's start cleared %zu block(s).",
                    cleared);

    // A range covering exactly the block does.
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_invalidate_range(&manager, 0x1000U, 0x1080U, &cleared));
    POUND_CHECK(1U == cleared);

    // Re-publishing it, to test the end-boundary from the other side.
    POUND_REQUIRE(POUND_SUCCESS
                  == publish_block(&manager, 0x1000U, code.bytes, sizeof(code.bytes), 0x80U));

    // A range that starts where the block ends does not touch it: `end` is exclusive, and
    // [0x1080, 0x2000) begins one byte past the block's last byte.
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_invalidate_range(&manager, 0x1080U, 0x2000U, &cleared));
    POUND_CHECK_MSG(0U == cleared, "A range starting at a block's end cleared %zu block(s).",
                    cleared);

    // And a range that stops one byte short of the block's *start* does not touch it
    // either. The byte the block's last byte is not the test -- that range overlaps the
    // block, and overlapping is what matters: the guest wrote the block's first byte,
    // so the rest of the block is stale too and has to go.
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_invalidate_range(&manager, 0x0000U, 0x1000U, &cleared));
    POUND_CHECK_MSG(0U == cleared, "A range stopping at a block's start cleared %zu block(s).",
                    cleared);

    // One byte into the block and it goes, because the rest of it is now the wrong
    // bytes as well. This is the case that a block-aligned invalidation gets wrong in
    // the *unsafe* direction: keeping a block the guest has partly rewritten means
    // running machine code translated from a mixture of old and new bytes.
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_invalidate_range(&manager, 0x0000U, 0x1001U, &cleared));
    POUND_CHECK_MSG(1U == cleared,
                    "A range overlapping one byte of the block cleared %zu block(s) rather than "
                    "one.",
                    cleared);

    // An empty range is not a request, but it is not an error either: a guest that
    // stores zero bytes has still executed a store, and the caller should not have to
    // special-case it.
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_invalidate_range(&manager, 0x1000U, 0x1000U, &cleared));
    POUND_CHECK(0U == cleared);

    // A range that ends before it starts is a caller's bug.
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT
                == jit_metadata_invalidate_range(&manager, 0x2000U, 0x1000U, &cleared));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT
                == jit_metadata_invalidate_range(&manager, 0U, 0U, NULL));

    // A block at the very top of the address space, invalidated by a range that runs to
    // `UINT64_MAX`. A guest can write past the last page it mapped, and the range that
    // covers that has to be expressible -- a range that had to stop below the top
    // because of an overflow check would leave the last page of the address space
    // permanently un-invalidation-able.
    //
    // Placed so the block's last byte *is* the top one: it covers
    // [UINT64_MAX - 0x80, UINT64_MAX), which does not wrap.
    const uint64_t top_pc = UINT64_MAX - 0x80U;

    POUND_REQUIRE(POUND_SUCCESS == publish_block(&manager, top_pc, code.bytes, 8U, 0x80U));

    size_t ready_index = 0U;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_find_ready(&manager, top_pc, &ready_index));

    POUND_REQUIRE_MSG(POUND_SUCCESS
                          == jit_metadata_invalidate_range(&manager, top_pc, UINT64_MAX, &cleared),
                      "A range running to the top of the address space was refused, so a guest that "
                      "writes past its last mapped page could never be serviced.");
    POUND_CHECK_MSG(1U == cleared,
                    "A range covering the block at the top of the address space cleared %zu block(s) "
                    "rather than one.",
                    cleared);

    // `end` is exclusive, and here that is the difference between servicing the write
    // and not: the range must *reach* the block's last byte.
    POUND_REQUIRE(POUND_SUCCESS == publish_block(&manager, top_pc, code.bytes, 8U, 0x80U));

    POUND_REQUIRE(POUND_SUCCESS
                  == jit_metadata_invalidate_range(&manager, top_pc, UINT64_MAX - 0x80U, &cleared));

    POUND_CHECK_MSG(0U == cleared,
                    "A range ending before the top of the address space cleared %zu block(s), but it "
                    "did not reach the block's last byte.",
                    cleared);

    // One byte further and it is serviced.
    POUND_REQUIRE(POUND_SUCCESS
                  == jit_metadata_invalidate_range(&manager, top_pc, UINT64_MAX, &cleared));
    POUND_CHECK(1U == cleared);

    // A block whose declared size would push its end past the top of the address space
    // is *not* claimed to overlap anything, because claiming so would authorise freeing
    // code something might be running. The conservative direction is the safe one.
    size_t wrapping = 0U;
    jit_metadata_state_t previous = JIT_METADATA_STATE_EMPTY;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_intern(&manager, UINT64_MAX - 0x10U, &wrapping));
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_try_begin(&manager, wrapping, &previous));
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_publish(&manager, wrapping, code.bytes, 8U, 0x100U, 0U));

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_invalidate_range(&manager, 0U, UINT64_MAX, &cleared));

    POUND_CHECK_MSG(0U == cleared,
                    "A block whose guest range wraps past the top of the address space was reported "
                    "as overlapping, which would have authorised freeing code that might be running.");

    // A block that covers no guest bytes -- an interpreter block -- is still dropped when
    // the guest writes at its address, because anything decoded from the old bytes is
    // stale even though there is no host code to free.
    size_t interpreted = 0U;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_intern(&manager, 0x8000U, &interpreted));
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_try_begin(&manager, interpreted, &previous));
    POUND_REQUIRE(POUND_SUCCESS
                  == jit_metadata_publish(&manager, interpreted, NULL, 0U, 0U,
                                          JIT_METADATA_FLAG_INTERPRETED));

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_invalidate_range(&manager, 0x8000U, 0x8001U, &cleared));

    POUND_CHECK_MSG(1U == cleared,
                    "A block covering no guest bytes survived a one-byte write at its own address.");

    jit_metadata_destroy(&manager);
}

POUND_TEST(metadata, an_invalidated_block_can_be_translated_again)
{
    jit_metadata_t manager;

    POUND_REQUIRE(POUND_SUCCESS == make_manager(&manager));

    fake_code_t original;
    fake_code_t replacement;

    size_t index = 0U;

    POUND_REQUIRE(POUND_SUCCESS
                  == publish_block(&manager, 0x1000U, original.bytes, sizeof(original.bytes), 0x80U));

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_lookup(&manager, 0x1000U, &index));

    size_t cleared = 0U;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_invalidate_range(&manager, 0x1000U, 0x1080U, &cleared));
    POUND_CHECK(1U == cleared);

    // The address is interned again and gets its own slot back -- not a new one, and
    // not nothing.
    size_t again = SIZE_MAX;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_intern(&manager, 0x1000U, &again));

    POUND_CHECK_MSG(index == again, "The address was given slot %zu after invalidation rather than "
                                    "its own slot %zu.",
                    again, index);

    // It is claimable, and the *replacement* code is what comes back -- so a guest that
    // rewrote itself really does run the new bytes.
    jit_metadata_state_t previous = JIT_METADATA_STATE_EMPTY;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_try_begin(&manager, again, &previous));
    POUND_CHECK(JIT_METADATA_STATE_EMPTY == previous);
    POUND_REQUIRE(POUND_SUCCESS
                  == jit_metadata_publish(&manager, again, replacement.bytes,
                                          sizeof(replacement.bytes), 0x80U, 0U));

    jit_metadata_info_t info;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_acquire(&manager, again, &info));

    POUND_CHECK_PTR_EQ(replacement.bytes, info.host_code);
    POUND_CHECK_MSG(original.bytes != info.host_code,
                    "The block still reports its old host code, so the guest would run the bytes it "
                    "had already overwritten.");

    POUND_CHECK(0U == jit_metadata_release(&manager, again));

    jit_metadata_destroy(&manager);
}

// -----------------------------------------------------------------------------
// Reset
// -----------------------------------------------------------------------------

POUND_TEST(metadata, a_reset_empties_the_table_and_keeps_it_usable)
{
    jit_metadata_t manager;

    POUND_REQUIRE(POUND_SUCCESS == make_manager(&manager));

    fake_code_t code;

    for (size_t i = 0U; i < 8U; ++i)
    {
        POUND_REQUIRE(POUND_SUCCESS
                      == publish_block(&manager, (uint64_t)(0x1000U + (i * 0x100U)),
                                       code.bytes, sizeof(code.bytes), 0x100U));
    }

    // One failure, so the reset has something in every state.
    size_t doomed = 0U;
    jit_metadata_state_t previous = JIT_METADATA_STATE_EMPTY;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_intern(&manager, 0x9000U, &doomed));
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_try_begin(&manager, doomed, &previous));
    POUND_REQUIRE(POUND_SUCCESS
                  == jit_metadata_fail(&manager, doomed, POUND_ERROR_TRANSLATION_FAILED, "no"));

    // One claim in flight, which a reset has to leave alone.
    size_t claimed = 0U;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_intern(&manager, 0xA000U, &claimed));
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_try_begin(&manager, claimed, &previous));

    const uint64_t before = jit_metadata_generation(&manager);

    jit_metadata_reset(&manager);

    POUND_CHECK_MSG(jit_metadata_generation(&manager) != before, "A reset left the generation at %llu.",
                    (unsigned long long)before);

    jit_metadata_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_get_stats(&manager, &stats));

    // A reset does not keep a claim. A claim owns no resource -- only an intent to
    // publish -- and that intent is against a game the reset has just unloaded, so
    // letting it survive would mean a translator from the old title publishing into the
    // new one's table. Clearing it is also the harmless direction: the translator's
    // publication is refused and its output discarded, which costs a retranslate.
    //
    // A *lease* is the opposite, and is what the companion case covers.
    POUND_CHECK_MSG(0U == stats.claimed,
                    "%zu claim(s) survived a reset, so a translator from the unloaded title could "
                    "still publish into this table.",
                    stats.claimed);
    POUND_CHECK_MSG(0U == stats.ready, "%zu published block(s) survived a reset.", stats.ready);
    POUND_CHECK_MSG(0U == stats.failed, "%zu failed block(s) survived a reset.", stats.failed);

    // The keys are dropped too, unlike an invalidation's. A reset that left `VACANT`
    // slots would leave the new game unable to intern addresses the old one used --
    // which, at the ceiling, means a title that cannot start.
    POUND_CHECK_MSG(0U == stats.interned, "%zu address(es) survived a reset.", stats.interned);

    // Counters cleared as well, so a debug window does not mix two sessions' numbers.
    POUND_CHECK(0U == stats.publications);
    POUND_CHECK(0U == stats.probe_total);
    POUND_CHECK(0U == stats.probe_worst);

    // The manager is immediately usable: same table, empty contents. A reset that had
    // dropped the table would show up here as an allocation failure.
    size_t index = SIZE_MAX;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_intern(&manager, 0x1000U, &index));
    POUND_REQUIRE(POUND_SUCCESS == publish_block(&manager, 0x1000U, code.bytes, 8U, 8U));

    size_t ready_index = 0U;

    POUND_CHECK(POUND_SUCCESS == jit_metadata_find_ready(&manager, 0x1000U, &ready_index));

    // And the translator whose claim was dropped is told so rather than left believing
    // it still owns the block. It finds the slot unoccupied and the publication is
    // refused, which is the outcome it has to handle by discarding its output.
    POUND_CHECK_MSG(POUND_ERROR_ALREADY_INITIALIZED
                        == jit_metadata_publish(&manager, claimed, code.bytes, 8U, 8U, 0U),
                    "A slot the reset emptied still accepted a publication.");

    // A second reset of an already-empty table is not a failure and not a leak.
    jit_metadata_reset(&manager);

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_get_stats(&manager, &stats));
    POUND_CHECK_MSG(0U == stats.interned, "A second reset left %zu address(es) counted.",
                    stats.interned);

    jit_metadata_destroy(&manager);
}

POUND_TEST(metadata, a_reset_keeps_a_block_a_thread_is_still_running_and_says_it_did)
{
    jit_metadata_t manager;

    POUND_REQUIRE(POUND_SUCCESS == make_manager(&manager));

    fake_code_t code;

    size_t running = 0U;

    POUND_REQUIRE(POUND_SUCCESS
                  == publish_block(&manager, 0x1000U, code.bytes, sizeof(code.bytes), 0x80U));

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_lookup(&manager, 0x1000U, &running));

    jit_metadata_info_t info;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_acquire(&manager, running, &info));

    pound_test_log_reset();

    jit_metadata_reset(&manager);

    // Kept, and announced. Clearing it would make the host code unreachable while a
    // thread is inside it, which is a use-after-free; keeping it means the guest keeps
    // running code built from bytes it may just have rewritten. Neither is what the
    // caller wanted, so the only correct answer is to do the survivable thing and say
    // so -- an operator reading the log can then retry the invalidation.
    POUND_CHECK_MSG(0U < pound_test_log_count_at(LOG_LEVEL_WARN),
                    "A reset that kept a running block logged no warning, so an operator would not "
                    "learn that a self-modifying guest's rewrite was not applied.");

    jit_metadata_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_get_stats(&manager, &stats));
    POUND_CHECK_MSG(1U == stats.ready, "The running block was cleared by the reset.");

    // Still callable, so the thread inside it is fine.
    POUND_CHECK_PTR_EQ(code.bytes, info.host_code);
    POUND_CHECK(0U == jit_metadata_release(&manager, running));

    // And once it is out, the next reset takes it.
    jit_metadata_reset(&manager);

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_get_stats(&manager, &stats));
    POUND_CHECK(0U == stats.ready);

    // A reset of a manager that is already empty is not a failure and not a leak.
    jit_metadata_reset(&manager);

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_get_stats(&manager, &stats));
    POUND_CHECK(0U == stats.interned);

    jit_metadata_destroy(&manager);
}

// -----------------------------------------------------------------------------
// The register map
// -----------------------------------------------------------------------------

POUND_TEST(metadata, a_blocks_register_map_is_published_and_read_back_exactly)
{
    jit_metadata_t manager;

    POUND_REQUIRE(POUND_SUCCESS == make_manager(&manager));

    size_t index = 0U;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_intern(&manager, 0x1000U, &index));

    jit_metadata_state_t previous = JIT_METADATA_STATE_EMPTY;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_try_begin(&manager, index, &previous));

    jit_register_slot_t slots[JIT_METADATA_MAP_SLOTS];

    slots[0].guest_index = 0U;
    slots[0].host_index = 19U;
    slots[0].frame_offset = 0U;
    slots[0].location = (uint16_t)JIT_REGISTER_LOCATION_HOST_GPR;

    slots[1].guest_index = 1U;
    slots[1].host_index = 20U;
    slots[1].frame_offset = 0U;
    slots[1].location = (uint16_t)JIT_REGISTER_LOCATION_HOST_GPR;

    slots[2].guest_index = 31U;
    slots[2].host_index = 3U;
    slots[2].frame_offset = 0U;
    slots[2].location = (uint16_t)JIT_REGISTER_LOCATION_HOST_VEC;

    slots[3].guest_index = 2U;
    slots[3].host_index = 0U;
    slots[3].frame_offset = 48U;
    slots[3].location = (uint16_t)JIT_REGISTER_LOCATION_STACK;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_set_map(&manager, index, slots, 4U));

    jit_register_slot_t read_back[JIT_METADATA_MAP_SLOTS];

    memset(read_back, 0, sizeof(read_back));

    size_t used = 0U;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_get_map(&manager, index, read_back, &used));

    POUND_CHECK_MSG(4U == used, "The map reported %zu entries rather than the 4 that were set.",
                    used);

    for (size_t i = 0U; i < used; ++i)
    {
        POUND_CHECK_MSG(slots[i].guest_index == read_back[i].guest_index
                            && slots[i].host_index == read_back[i].host_index
                            && slots[i].frame_offset == read_back[i].frame_offset
                            && slots[i].location == read_back[i].location,
                        "Register-map entry %zu came back as guest %u / host %u / frame %u / "
                        "location %u rather than guest %u / host %u / frame %u / location %u.",
                        i,
                        (unsigned)read_back[i].guest_index,
                        (unsigned)read_back[i].host_index,
                        (unsigned)read_back[i].frame_offset,
                        (unsigned)read_back[i].location,
                        (unsigned)slots[i].guest_index,
                        (unsigned)slots[i].host_index,
                        (unsigned)slots[i].frame_offset,
                        (unsigned)slots[i].location);
    }

    // The count travels with the block snapshot, so a dispatcher does not have to ask
    // twice.
    jit_metadata_info_t info;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_peek(&manager, index, &info));
    POUND_CHECK(4U == info.map_used);

    // Every location kind the dispatcher understands, in one map, so the reader is not
    // only ever exercised against the first enum value.
    jit_register_slot_t every_location[4];

    every_location[0].guest_index = 0U;
    every_location[0].host_index = 10U;
    every_location[0].frame_offset = 0U;
    every_location[0].location = (uint16_t)JIT_REGISTER_LOCATION_HOST_GPR;

    every_location[1].guest_index = 1U;
    every_location[1].host_index = 11U;
    every_location[1].frame_offset = 0U;
    every_location[1].location = (uint16_t)JIT_REGISTER_LOCATION_HOST_VEC;

    every_location[2].guest_index = 2U;
    every_location[2].host_index = 12U;
    every_location[2].frame_offset = 0U;
    every_location[2].location = (uint16_t)JIT_REGISTER_LOCATION_HOST_FPR;

    every_location[3].guest_index = 3U;
    every_location[3].host_index = 0U;
    every_location[3].frame_offset = 64U;
    every_location[3].location = (uint16_t)JIT_REGISTER_LOCATION_STACK;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_set_map(&manager, index, every_location, 4U));

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_get_map(&manager, index, read_back, &used));

    for (size_t i = 0U; i < used; ++i)
    {
        POUND_CHECK_MSG(every_location[i].location == read_back[i].location,
                        "Location kind %u was stored as %u.", (unsigned)every_location[i].location,
                        (unsigned)read_back[i].location);
    }

    jit_metadata_destroy(&manager);
}

POUND_TEST(metadata, a_register_map_the_dispatcher_would_misread_is_refused)
{
    jit_metadata_t manager;

    POUND_REQUIRE(POUND_SUCCESS == make_manager(&manager));

    size_t index = 0U;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_intern(&manager, 0x1000U, &index));

    jit_metadata_state_t previous = JIT_METADATA_STATE_EMPTY;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_try_begin(&manager, index, &previous));

    jit_register_slot_t bad[JIT_METADATA_MAP_SLOTS];

    // A location nobody defined. The dispatcher would act on it by whatever its default
    // case is, which is the definition of reading a register that was never written.
    bad[0].guest_index = 0U;
    bad[0].host_index = 19U;
    bad[0].frame_offset = 0U;
    bad[0].location = 99U;

    POUND_CHECK_MSG(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_set_map(&manager, index, bad, 1U),
                    "A register map naming location 99 was accepted.");

    // A frame offset on a register that is not spilled. One of the two fields is always
    // wrong here, and the dispatcher cannot tell which.
    bad[0].location = (uint16_t)JIT_REGISTER_LOCATION_HOST_GPR;
    bad[0].frame_offset = 16U;

    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_set_map(&manager, index, bad, 1U));

    // A host index on a spilled register, where it means nothing and a stale value
    // would be believed.
    bad[0].location = (uint16_t)JIT_REGISTER_LOCATION_STACK;
    bad[0].frame_offset = 16U;
    bad[0].host_index = 7U;

    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_set_map(&manager, index, bad, 1U));

    // A valid map, so the refusals above were not just refusing everything.
    bad[0].host_index = 0U;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_set_map(&manager, index, bad, 1U));

    // More entries than a block holds. The answer is to split the block, and the log has
    // to say that rather than truncating and letting the dispatcher spill the rest
    // somewhere it never reserved.
    for (size_t i = 0U; i < JIT_METADATA_MAP_SLOTS; ++i)
    {
        bad[i].guest_index = (uint16_t)i;
        bad[i].host_index = 0U;
        bad[i].frame_offset = (uint16_t)(i * 8U);
        bad[i].location = (uint16_t)JIT_REGISTER_LOCATION_STACK;
    }

    pound_test_log_reset();

    POUND_CHECK_MSG(POUND_ERROR_ALLOCATION_FAILED
                        == jit_metadata_set_map(&manager, index, bad, JIT_METADATA_MAP_SLOTS + 1U),
                    "An oversized register map was accepted rather than refused.");

    POUND_CHECK_MSG(pound_test_log_contains("Split the block"),
                    "The refusal did not tell the translator what to do instead.");

    // Exactly the cap is fine, so the check is not simply refusing everything.
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_set_map(&manager, index, bad, JIT_METADATA_MAP_SLOTS));

    // A NULL array with a non-zero count.
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_set_map(&manager, index, NULL, 3U));

    // A NULL array with a zero count publishes an empty map, which is what a block with
    // nothing to preserve wants.
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_set_map(&manager, index, NULL, 0U));

    size_t used = 99U;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_get_map(&manager, index, NULL, &used));
    POUND_CHECK_MSG(0U == used, "An empty map reported %zu entries.", used);

    // A NULL count destination is a caller mistake.
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT
                == jit_metadata_get_map(&manager, index, bad, NULL));

    jit_metadata_destroy(&manager);
}

POUND_TEST(metadata, a_register_map_cannot_be_written_after_the_block_is_callable)
{
    jit_metadata_t manager;

    POUND_REQUIRE(POUND_SUCCESS == make_manager(&manager));

    fake_code_t code;

    size_t index = 0U;

    POUND_REQUIRE(POUND_SUCCESS == publish_block(&manager, 0x1000U, code.bytes, 8U, 8U));

    jit_register_slot_t slots[1];

    slots[0].guest_index = 0U;
    slots[0].host_index = 19U;
    slots[0].frame_offset = 0U;
    slots[0].location = (uint16_t)JIT_REGISTER_LOCATION_HOST_GPR;

    // The map is what makes a block callable. Rewriting it while a dispatcher might be
    // reading it would be a torn read of a table the dispatcher trusts absolutely.
    POUND_CHECK_MSG(POUND_ERROR_ALREADY_INITIALIZED
                        == jit_metadata_set_map(&manager, index, slots, 1U),
                    "A published block's register map was rewritable, so a dispatcher could have "
                    "read half of one map and half of another.");

    // The same for a block nobody has claimed, and for one that failed.
    jit_metadata_state_t previous = JIT_METADATA_STATE_EMPTY;

    size_t fresh = 0U;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_intern(&manager, 0x2000U, &fresh));
    POUND_CHECK(POUND_ERROR_ALREADY_INITIALIZED == jit_metadata_set_map(&manager, fresh, slots, 1U));

    size_t doomed = 0U;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_intern(&manager, 0x3000U, &doomed));
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_try_begin(&manager, doomed, &previous));
    POUND_REQUIRE(POUND_SUCCESS
                  == jit_metadata_fail(&manager, doomed, POUND_ERROR_TRANSLATION_FAILED, "no"));
    POUND_CHECK(POUND_ERROR_ALREADY_INITIALIZED == jit_metadata_set_map(&manager, doomed, slots, 1U));

    // A slot the table has, holding no block, is "not initialised"; a slot past the end
    // is "invalid argument". Two different caller mistakes -- the second means the
    // caller's index came from the wrong place, the first that it asked about an
    // address nobody translated -- so they must not report the same thing.
    //
    // Which slots hold blocks depends on the hash, so the case finds an empty one
    // rather than assuming one: it asserts there *is* one, which is what makes the
    // later check meaningful.
    size_t empty = JIT_METADATA_MIN_CAPACITY;
    size_t used = 0U;

    for (size_t scan = 0U; scan < JIT_METADATA_MIN_CAPACITY; ++scan)
    {
        if (POUND_ERROR_NOT_INITIALIZED == jit_metadata_get_map(&manager, scan, slots, &used))
        {
            empty = scan;

            break;
        }
    }

    POUND_REQUIRE_MSG(empty < JIT_METADATA_MIN_CAPACITY,
                      "Every one of the %u slots reported a block, but only three were published.",
                      (unsigned)JIT_METADATA_MIN_CAPACITY);

    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT
                == jit_metadata_get_map(&manager, JIT_METADATA_MIN_CAPACITY, slots, &used));

    jit_metadata_destroy(&manager);
}

// -----------------------------------------------------------------------------
// Labels
// -----------------------------------------------------------------------------

POUND_TEST(metadata, a_label_is_stored_exactly_or_refused)
{
    jit_metadata_t manager;

    POUND_REQUIRE(POUND_SUCCESS == make_manager(&manager));

    fake_code_t code;

    POUND_REQUIRE(POUND_SUCCESS == publish_block(&manager, 0x1000U, code.bytes, 8U, 8U));

    // Which slot the hash chose is not this case's concern, but the slot is: an
    // index-taking entry point has to be given the address's real slot, and slot 0 is
    // not it.
    size_t index = SIZE_MAX;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_lookup(&manager, 0x1000U, &index));

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_set_label(&manager, index, "arm64_main_loop"));

    jit_metadata_info_t info;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_peek(&manager, index, &info));
    POUND_CHECK_STR_EQ("arm64_main_loop", info.label);

    // The longest label that fits, terminator included. If this were refused the field
    // would be smaller than it is documented to be.
    char longest[JIT_METADATA_LABEL_MAX];

    memset(longest, 'a', sizeof(longest) - 1U);
    longest[sizeof(longest) - 1U] = '\0';

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_set_label(&manager, index, longest));
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_peek(&manager, index, &info));
    POUND_CHECK_STR_EQ(longest, info.label);

    // One byte more is refused rather than cut, because a label is read by a person
    // reading a log and a shortened one looks complete.
    char too_long[JIT_METADATA_LABEL_MAX + 1U];

    memset(too_long, 'b', sizeof(too_long) - 1U);
    too_long[sizeof(too_long) - 1U] = '\0';

    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_set_label(&manager, index, too_long));

    // The refused label did not land, which is the other half of "refused rather than
    // truncated": a refusal that had already overwritten the old label would have lost
    // the information it was protecting.
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_peek(&manager, index, &info));
    POUND_CHECK_STR_EQ(longest, info.label);

    // The empty string is a legitimate label and means "no label", which is different
    // from a slot that has never been labelled.
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_set_label(&manager, index, ""));
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_peek(&manager, index, &info));
    POUND_CHECK_STR_EQ("", info.label);

    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_set_label(&manager, index, NULL));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_set_label(&manager, SIZE_MAX, "x"));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_set_label(NULL, 0U, "x"));

    jit_metadata_destroy(&manager);
}

// -----------------------------------------------------------------------------
// Statistics
// -----------------------------------------------------------------------------

POUND_TEST(metadata, the_counters_report_what_the_table_did)
{
    jit_metadata_t manager;

    POUND_REQUIRE(POUND_SUCCESS == make_manager(&manager));

    fake_code_t code;

    // Two published blocks and one failure, so the state counts cover every value.
    for (size_t i = 0U; i < 2U; ++i)
    {
        POUND_REQUIRE(POUND_SUCCESS
                      == publish_block(&manager, (uint64_t)(0x1000U + (i * 0x100U)),
                                       code.bytes, 8U, 0x100U));
    }

    size_t doomed = 0U;
    jit_metadata_state_t previous = JIT_METADATA_STATE_EMPTY;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_intern(&manager, 0x9000U, &doomed));
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_try_begin(&manager, doomed, &previous));
    POUND_REQUIRE(POUND_SUCCESS
                  == jit_metadata_fail(&manager, doomed, POUND_ERROR_TRANSLATION_FAILED, "no"));

    // Two hits and two misses.
    size_t index = 0U;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_find_ready(&manager, 0x1000U, &index));
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_find_ready(&manager, 0x1100U, &index));
    POUND_CHECK(POUND_ERROR_NOT_FOUND == jit_metadata_find_ready(&manager, 0x7777U, &index));
    POUND_CHECK(POUND_ERROR_NOT_FOUND == jit_metadata_find_ready(&manager, 0x7778U, &index));

    jit_metadata_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_get_stats(&manager, &stats));

    POUND_CHECK_MSG(2U == stats.hits, "Two `find_ready` calls succeeded but %llu were counted.",
                    (unsigned long long)stats.hits);
    POUND_CHECK(2U == stats.misses);
    POUND_CHECK_MSG(3U == stats.claims, "Three blocks were translated but %llu claims were counted.",
                    (unsigned long long)stats.claims);
    POUND_CHECK(2U == stats.publications);
    POUND_CHECK(1U == stats.failures);
    POUND_CHECK(3U == stats.interned);
    POUND_CHECK(2U == stats.ready);
    POUND_CHECK(1U == stats.failed);
    POUND_CHECK(0U == stats.claimed);
    POUND_CHECK(0U == stats.leases_outstanding);
    POUND_CHECK(manager.capacity == stats.capacity);
    POUND_CHECK(manager.max_blocks == stats.max_blocks);

    // The probe statistics are the table's health report, and a table that is working
    // does not need more than a handful of slots to find a key.
    POUND_CHECK_MSG(stats.probe_worst >= 1U, "No lookup recorded a probe length.");
    POUND_CHECK_MSG(stats.probe_worst <= 8U,
                    "The longest probe was %zu slots in a table of %zu, so the hash is clustering "
                    "keys badly.",
                    stats.probe_worst, stats.capacity);

    // Invalidation *calls* and *blocks cleared* are counted separately, because the two
    // answer different questions: how often the guest rewrote itself, and how much work
    // that cost. The second call here clears nothing, so the two numbers must differ or
    // one of them is counting the other's thing.
    size_t cleared = 0U;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_invalidate_range(&manager, 0x1000U, 0x1080U, &cleared));
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_invalidate_range(&manager, 0x5000U, 0x5080U, &cleared));

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_get_stats(&manager, &stats));

    POUND_CHECK_MSG(2U == stats.invalidations, "%llu invalidation call(s) were counted rather than 2.",
                    (unsigned long long)stats.invalidations);
    POUND_CHECK_MSG(1U == stats.invalidated_blocks,
                    "%llu block(s) were counted as cleared; one call cleared one block and the "
                    "other cleared none.",
                    (unsigned long long)stats.invalidated_blocks);

    // A refused invalidation is neither, because nothing was cleared.
    fake_code_t more;

    size_t running = 0U;

    POUND_REQUIRE(POUND_SUCCESS == publish_block(&manager, 0x7000U, more.bytes, 8U, 0x80U));
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_lookup(&manager, 0x7000U, &running));
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_acquire(&manager, running, &(jit_metadata_info_t){0}));
    POUND_CHECK(POUND_ERROR_BUSY == jit_metadata_invalidate_range(&manager, 0x7000U, 0x7080U, &cleared));
    POUND_CHECK(0U == jit_metadata_release(&manager, running));

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_get_stats(&manager, &stats));

    POUND_CHECK(2U == stats.invalidations);
    POUND_CHECK(1U == stats.invalidated_blocks);

    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_get_stats(&manager, NULL));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_get_stats(NULL, &stats));

    // The summary logs counts and no code addresses, because a log file ends up in a
    // bug report and an address there is meaningless without the build it came from.
    pound_test_log_reset();

    jit_metadata_log_summary(&manager);

    POUND_CHECK_MSG(pound_test_log_contains("Metadata:"), "The summary logged no state line.");
    POUND_CHECK(pound_test_log_contains("Metadata counters:"));

    // A summary of a manager that is not up says so rather than printing zeroes that
    // look like measurements.
    pound_test_log_reset();

    jit_metadata_log_summary(NULL);

    POUND_CHECK_MSG(0U < pound_test_log_count_at(LOG_LEVEL_WARN),
                    "A summary of a NULL manager logged nothing.");

    jit_metadata_destroy(&manager);
}

POUND_TEST(metadata, counters_can_be_switched_off_and_the_snapshot_says_so)
{
    jit_metadata_t manager;

    jit_metadata_config_t config;

    memset(&config, 0, sizeof(config));

    config.capacity = JIT_METADATA_MIN_CAPACITY;
    config.no_accounting = true;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_init(&manager, &config));

    // A lookup as well as a publish, so the probe counters are exercised too. Those
    // run on the dispatch path, which is exactly why "switched off" has to mean
    // switched off: a caller on a 32-bit host turns them off to stop paying for a
    // locked 64-bit increment on every dispatch, and a gate that missed them would
    // leave them paying while reporting zeroes.
    fake_code_t code;

    POUND_REQUIRE(POUND_SUCCESS == publish_block(&manager, 0x1000U, code.bytes, 8U, 8U));

    size_t index = 0U;
    size_t miss = 0U;

    POUND_CHECK(POUND_ERROR_NOT_FOUND == jit_metadata_lookup(&manager, 0xBEEFU, &miss));
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_find_ready(&manager, 0x1000U, &index));
    POUND_CHECK(POUND_SUCCESS == jit_metadata_lookup(&manager, 0x1000U, &miss));

    jit_metadata_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_get_stats(&manager, &stats));

    // The snapshot has to say so, or a reader would take the zeroes for measurements
    // that were never taken.
    POUND_CHECK_MSG(!stats.account, "A snapshot did not report that its counters are switched off.");
    POUND_CHECK(0U == stats.hits);
    POUND_CHECK(0U == stats.misses);
    POUND_CHECK(0U == stats.publications);
    POUND_CHECK_MSG(0U == stats.probe_total,
                    "%llu probe(s) were recorded even though the counters are switched off, so the "
                    "dispatch path is paying for diagnostics nobody asked for.",
                    (unsigned long long)stats.probe_total);
    POUND_CHECK(0U == stats.probe_worst);

    // The block counts are not counters -- they are the table's actual contents -- so
    // they are still right, and a caller can still tell an empty table from a full one
    // with accounting off.
    POUND_CHECK(1U == stats.ready);
    POUND_CHECK(1U == stats.interned);

    jit_metadata_resolved_t resolved;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_describe(&manager, &resolved));
    POUND_CHECK(!resolved.account);

    // And the summary says it too, rather than printing zeroes with no explanation.
    pound_test_log_reset();

    jit_metadata_log_summary(&manager);

    POUND_CHECK_MSG(pound_test_log_contains("switched off"),
                    "The summary did not say its counters were switched off.");

    jit_metadata_destroy(&manager);
}

// -----------------------------------------------------------------------------
// Argument validation
// -----------------------------------------------------------------------------

POUND_TEST(metadata, an_index_the_table_does_not_have_is_refused_everywhere)
{
    jit_metadata_t manager;

    POUND_REQUIRE(POUND_SUCCESS == make_manager(&manager));

    fake_code_t code;

    size_t index = 0U;

    POUND_REQUIRE(POUND_SUCCESS == publish_block(&manager, 0x1000U, code.bytes, 8U, 8U));

    // The table has 256 slots, so 256 is the first invalid index and `SIZE_MAX` is the
    // other shape a bad index arrives in. Every index-taking entry point has to refuse
    // both, and none of them may act on one.
    const size_t past_the_end = JIT_METADATA_MIN_CAPACITY;

    jit_metadata_state_t previous = JIT_METADATA_STATE_EMPTY;
    jit_metadata_info_t info;
    jit_metadata_stats_t stats;
    jit_register_slot_t slots[JIT_METADATA_MAP_SLOTS];
    size_t used = 0U;
    error_t reason = POUND_SUCCESS;
    size_t cleared = 0U;

    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_try_begin(&manager, past_the_end, &previous));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT
                == jit_metadata_publish(&manager, past_the_end, code.bytes, 8U, 8U, 0U));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT
                == jit_metadata_fail(&manager, past_the_end, POUND_ERROR_TRANSLATION_FAILED, "x"));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_acquire(&manager, past_the_end, &info));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_peek(&manager, past_the_end, &info));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT
                == jit_metadata_failure_reason(&manager, past_the_end, &reason, NULL, 0U));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_set_map(&manager, past_the_end, slots, 1U));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_get_map(&manager, past_the_end, slots, &used));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_set_label(&manager, past_the_end, "x"));

    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_peek(&manager, SIZE_MAX, &info));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_acquire(&manager, SIZE_MAX, &info));

    // A NULL manager is the same class of mistake, and every entry point has to
    // recognise it rather than dereference it.
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_lookup(NULL, 0x1000U, &index));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_lookup(&manager, 0x1000U, NULL));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_find_ready(NULL, 0x1000U, &index));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_find_ready(&manager, 0x1000U, NULL));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_intern(NULL, 0x1000U, &index));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_intern(&manager, 0x1000U, NULL));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_try_begin(NULL, 0U, &previous));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_try_begin(&manager, 0U, NULL));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_publish(NULL, 0U, code.bytes, 8U, 8U, 0U));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_fail(NULL, 0U, POUND_ERROR_IO, "x"));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_acquire(NULL, 0U, &info));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_acquire(&manager, 0U, NULL));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_peek(NULL, 0U, &info));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_peek(&manager, 0U, NULL));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_failure_reason(NULL, 0U, &reason, NULL, 0U));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT
                == jit_metadata_failure_reason(&manager, 0U, NULL, NULL, 0U));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT
                == jit_metadata_invalidate_range(NULL, 0U, 1U, &cleared));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_invalidate_range(&manager, 0U, 1U, NULL));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_set_map(NULL, 0U, slots, 1U));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_get_map(NULL, 0U, slots, &used));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_get_map(&manager, 0U, slots, NULL));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_set_label(NULL, 0U, "x"));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_describe(NULL, &(jit_metadata_resolved_t){0}));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_describe(&manager, NULL));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_get_stats(NULL, &stats));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_metadata_get_stats(&manager, NULL));

    // Every one of those refusals logged a record, so a caller that ignored a return
    // value would still have left a trace.
    POUND_CHECK_MSG(0U < pound_test_log_count_at(LOG_LEVEL_ERROR),
                    "%u of the refusals logged no record.",
                    pound_test_log_count_at(LOG_LEVEL_ERROR));

    // And the one block the table did have is untouched by any of it. Slot 0 is not
    // necessarily that block, so it is found again by address rather than by index.
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_find_ready(&manager, 0x1000U, &index));
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_acquire(&manager, index, &info));
    POUND_CHECK_PTR_EQ(code.bytes, info.host_code);
    POUND_CHECK(0U == jit_metadata_release(&manager, index));

    jit_metadata_destroy(&manager);
}

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

POUND_TEST(metadata, a_manager_can_be_built_taken_down_and_built_again)
{
    jit_metadata_t manager;

    fake_code_t code;

    for (size_t round = 0U; round < 3U; ++round)
    {
        POUND_REQUIRE(POUND_SUCCESS == make_manager(&manager));

        const uint64_t guest_pc = 0x1000U + ((uint64_t)round * 0x1000U);

        POUND_REQUIRE(POUND_SUCCESS
                      == publish_block(&manager, guest_pc, code.bytes, 8U, 0x80U));

        size_t index = SIZE_MAX;

        POUND_REQUIRE(POUND_SUCCESS == jit_metadata_find_ready(&manager, guest_pc, &index));

        POUND_CHECK_MSG(index < JIT_METADATA_MIN_CAPACITY, "Slot %zu is outside the table.", index);

        jit_metadata_destroy(&manager);

        // Destroying twice is what a teardown path that runs on both the error and the
        // success branch would do, and it must not double-free the table.
        jit_metadata_destroy(&manager);

        POUND_CHECK_PTR_NULL(manager.table);
        POUND_CHECK_MSG(0U == manager.capacity, "A destroyed manager still reports %zu slots.",
                        manager.capacity);
    }

    // A name for every state, and one for a value that is not a state -- the last is
    // what keeps a corrupted state word from rendering as a NULL into a log format.
    POUND_CHECK_STR_EQ("unoccupied", jit_metadata_state_to_string(JIT_METADATA_STATE_UNOCCUPIED));
    POUND_CHECK_STR_EQ("ready", jit_metadata_state_to_string(JIT_METADATA_STATE_READY));
    POUND_CHECK_PTR_NON_NULL(jit_metadata_state_to_string(JIT_METADATA_STATE_EMPTY));
    POUND_CHECK_PTR_NON_NULL(jit_metadata_state_to_string(JIT_METADATA_STATE_CLAIMED));
    POUND_CHECK_PTR_NON_NULL(jit_metadata_state_to_string(JIT_METADATA_STATE_FAILED));
    POUND_CHECK_PTR_NON_NULL(jit_metadata_state_to_string(JIT_METADATA_STATE_VACANT));
    POUND_CHECK_STR_EQ("unrecognised", jit_metadata_state_to_string((jit_metadata_state_t)999));
}

POUND_TEST_SUITE(metadata,
                POUND_TEST_CASE(metadata, a_default_manager_is_usable_and_reports_its_own_shape),
                POUND_TEST_CASE(metadata, a_configuration_that_could_not_produce_a_usable_table_is_refused),
                POUND_TEST_CASE(metadata, a_rejected_init_leaves_a_manager_that_reports_not_initialised),
                POUND_TEST_CASE(metadata, an_interned_address_gets_a_slot_and_keeps_it),
                POUND_TEST_CASE(metadata, a_lookup_that_finds_nothing_is_a_miss_and_not_a_failure),
                POUND_TEST_CASE(metadata, a_full_table_refuses_new_addresses_and_says_so),
                POUND_TEST_CASE(metadata, a_block_is_translated_by_exactly_one_claimant),
                POUND_TEST_CASE(metadata, a_block_that_was_never_claimed_cannot_be_published_into),
                POUND_TEST_CASE(metadata, a_publication_that_could_not_be_described_is_refused),
                POUND_TEST_CASE(metadata, a_block_that_cannot_be_translated_says_why_and_stays_that_way),
                POUND_TEST_CASE(metadata, a_lease_is_held_for_exactly_as_long_as_the_block_is_running),
                POUND_TEST_CASE(metadata, a_block_that_is_not_ready_cannot_be_acquired),
                POUND_TEST_CASE(metadata, an_invalidation_clears_the_blocks_the_guest_overwrote),
                POUND_TEST_CASE(metadata,
                                every_interned_address_is_still_findable_after_its_own_slot_is_invalidated),
                POUND_TEST_CASE(metadata,
                                an_invalidated_slot_does_not_hide_the_blocks_behind_it),
                POUND_TEST_CASE(metadata, an_invalidation_that_would_catch_a_running_block_is_refused_whole),
                POUND_TEST_CASE(metadata,
                                an_invalidation_range_is_half_open_and_may_reach_the_top_of_the_address_space),
                POUND_TEST_CASE(metadata, an_invalidated_block_can_be_translated_again),
                POUND_TEST_CASE(metadata, a_reset_empties_the_table_and_keeps_it_usable),
                POUND_TEST_CASE(metadata, a_reset_keeps_a_block_a_thread_is_still_running_and_says_it_did),
                POUND_TEST_CASE(metadata, a_blocks_register_map_is_published_and_read_back_exactly),
                POUND_TEST_CASE(metadata, a_register_map_the_dispatcher_would_misread_is_refused),
                POUND_TEST_CASE(metadata, a_register_map_cannot_be_written_after_the_block_is_callable),
                POUND_TEST_CASE(metadata, a_label_is_stored_exactly_or_refused),
                POUND_TEST_CASE(metadata, the_counters_report_what_the_table_did),
                POUND_TEST_CASE(metadata, counters_can_be_switched_off_and_the_snapshot_says_so),
                POUND_TEST_CASE(metadata, an_index_the_table_does_not_have_is_refused_everywhere),
                POUND_TEST_CASE(metadata, a_manager_can_be_built_taken_down_and_built_again))

/*** end of line ***/

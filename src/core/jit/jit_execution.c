//! Implementation of the execution loop dispatcher.
//!
//! See `jit_execution.h` for the contract. The two structural commitments made
//! here are worth restating:
//!
//! - **One block per step.** A miss step interns, claims, translates, publishes
//!   and *then runs* the block it built, so `step` always ends with a block
//!   executed or an exit reason produced -- never with a translation that the
//!   caller has to remember to finish.
//! - **The loop owns the code it publishes.** Blocks are allocated from the
//!   cache, pushed onto the ownership registry, and handed back to the cache by
//!   `invalidate_range` / `reset`. The metadata manager's cleared-block *count*
//!   is not enough to free with, so the registry is what honours the
//!   "invalidate, then free, never reordered" contract.

#include "jit_execution.h"

#include "log.h"
#include "memory/memory.h"
#include <string.h>

/// Increments `counter` when accounting is on.
///
/// Relaxed, exactly like the metadata manager's counters: the increment gates
/// nothing and losing one costs a measurement, not a translation.
static void
count(jit_execution_t *POUND_RESTRICT exec, atomic_uint_least64_t *POUND_RESTRICT counter)
{
    if (exec->account)
    {
        (void)atomic_fetch_add_explicit(counter, (uint_least64_t)1, memory_order_relaxed);
    }
}

/// True when the counters would be lock-free on this host.
static bool
counters_are_lock_free(void)
{
    atomic_uint_least64_t probe_counter;

    atomic_init(&probe_counter, (uint_least64_t)0);

    return atomic_is_lock_free(&probe_counter);
}

/// Clears every counter.
static void
clear_counters(jit_execution_t *POUND_RESTRICT exec)
{
    atomic_store_explicit(&exec->hits, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&exec->misses, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&exec->claims, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&exec->claim_conflicts, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&exec->publications, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&exec->blocks_run, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&exec->blocks_interpreted, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&exec->failures, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&exec->syscall_exits, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&exec->stops, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&exec->quota_hits, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&exec->deferrals, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&exec->contention_interpreted, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&exec->spin_exhausted, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&exec->stale_acquires, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&exec->lease_refusals, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&exec->dispatch_failures, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&exec->invalidations, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&exec->blocks_freed, (uint_least64_t)0, memory_order_relaxed);
}

/// Returns `[a_begin, a_begin + a_size)` overlapping `[b_begin, b_begin + b_size)`.
///
/// This is the metadata manager's `ranges_overlap` predicate, reproduced here
/// so that the ownership registry's free pass asks exactly the question the
/// metadata table's invalidation pass asks. Both ranges must be non-empty; the
/// zero-sized-block case is handled by `owner_is_cleared`. A range whose end
/// would wrap is reported as *not* overlapping, the conservative direction,
/// exactly as in `jit_metadata.c`.
static bool
ranges_overlap(const uint64_t a_begin,
               const uint64_t a_size,
               const uint64_t b_begin,
               const uint64_t b_size)
{
    if ((0U == a_size) || (0U == b_size))
    {
        return false;
    }

    if ((a_begin > (UINT64_MAX - a_size)) || (b_begin > (UINT64_MAX - b_size)))
    {
        return false;
    }

    return (a_begin < (b_begin + b_size)) && (b_begin < (a_begin + a_size));
}

/// Whether invalidation of `[begin, begin + span)` would clear block `owner`.
///
/// Mirrors `entry_in_range` in `jit_metadata.c`: a block that covers no guest
/// bytes is cleared when its start address is inside the range, and any other
/// block is cleared when its guest range overlaps it. The caller passes the
/// validated `end - begin` as `span`, exactly as the metadata manager computes
/// it, so the two modules reach the same decision for the same block.
static bool
owner_is_cleared(const jit_execution_owner_t *POUND_RESTRICT owner,
                 const uint64_t                             begin,
                 const uint64_t                             span)
{
    if (0U == owner->guest_size)
    {
        return (begin <= owner->guest_pc) && (owner->guest_pc < (begin + span));
    }

    return ranges_overlap(owner->guest_pc, owner->guest_size, begin, span);
}

/// Frees and forgets every owned code block that `[begin, begin + span)` clears.
static void
registry_free_overlap(jit_execution_t *POUND_RESTRICT exec, const uint64_t begin, const uint64_t span)
{
    size_t keep = 0U;

    for (size_t index = 0U; index < exec->owner_count; ++index)
    {
        if (owner_is_cleared(&exec->owners[index], begin, span))
        {
            jit_cache_free_executable(exec->cache, exec->owners[index].code);

            count(exec, &exec->blocks_freed);

            continue;
        }

        if (keep != index)
        {
            exec->owners[keep] = exec->owners[index];
        }

        ++keep;
    }

    exec->owner_count = keep;
}

/// Removes and returns the owned block with address `code`, or NULL.
///
/// The rollback path for a publication that failed after the registry was
/// written: the block is still ours to free, it just never became callable.
static void *
registry_remove(jit_execution_t *POUND_RESTRICT exec, const void *POUND_RESTRICT code)
{
    for (size_t index = exec->owner_count; index > 0U; --index)
    {
        if (exec->owners[index - 1U].code == code)
        {
            void *POUND_RESTRICT removed = exec->owners[index - 1U].code;

            --exec->owner_count;

            if (index < exec->owner_count)
            {
                exec->owners[index - 1U] = exec->owners[exec->owner_count];
            }

            return removed;
        }
    }

    return NULL;
}

/// Records that this dispatcher owns the executable block `code`, growing the
/// registry when it is full.
static error_t
registry_push(jit_execution_t *POUND_RESTRICT exec,
              const uint64_t                 guest_pc,
              const uint64_t                 guest_size,
              void *POUND_RESTRICT           code)
{
    if (exec->owner_count == exec->owner_capacity)
    {
        const size_t old_capacity = exec->owner_capacity;
        const size_t new_capacity = (0U == old_capacity)
                                        ? JIT_EXECUTION_OWNER_REGISTRY_INITIAL
                                        : old_capacity * 2U;

        if ((0U != old_capacity) && (new_capacity < old_capacity))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "The dispatcher's block registry cannot grow past %zu entries.",
                            old_capacity);
            return POUND_ERROR_ALLOCATION_FAILED;
        }

        if (new_capacity > (SIZE_MAX / sizeof(jit_execution_owner_t)))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "A block registry of %zu entries of %zu bytes each cannot be addressed.",
                            new_capacity,
                            sizeof(jit_execution_owner_t));
            return POUND_ERROR_ALLOCATION_FAILED;
        }

        jit_execution_owner_t *POUND_RESTRICT new_owners =
            (jit_execution_owner_t *)memory_subsystem_allocate(8U, new_capacity * sizeof(jit_execution_owner_t));

        if (NULL == new_owners)
        {
            POUND_LOG_ERROR(&thread_logger,
                            "Could not grow the dispatcher's block registry to %zu entries.",
                            new_capacity);
            return POUND_ERROR_ALLOCATION_FAILED;
        }

        if (NULL != exec->owners)
        {
            memcpy(new_owners, exec->owners, exec->owner_count * sizeof(jit_execution_owner_t));

            memory_subsystem_free(exec->owners);
        }

        exec->owners = new_owners;
        exec->owner_capacity = new_capacity;
    }

    exec->owners[exec->owner_count].guest_pc  = guest_pc;
    exec->owners[exec->owner_count].guest_size = guest_size;
    exec->owners[exec->owner_count].code      = code;

    ++exec->owner_count;

    return POUND_SUCCESS;
}

/// Records that slot `index` cannot be translated, as the metadata contract
/// requires of a failed claim.
static error_t
record_bad_translation(jit_execution_t *POUND_RESTRICT exec,
                       const size_t                    index,
                       const error_t                   reason,
                       const char *POUND_RESTRICT      detail)
{
    const error_t recorded = jit_metadata_fail(exec->metadata, index, reason, detail);

    // The dispatch failed whether or not the table accepted the record, so the
    // failure counter is the first thing counted.
    count(exec, &exec->dispatch_failures);

    if (POUND_SUCCESS == recorded)
    {
        count(exec, &exec->failures);
    }

    // The caller sees the substantive error -- the refusal itself -- not the
    // metadata accountant's all-clear.
    return (POUND_SUCCESS == recorded) ? reason : recorded;
}

/// What one dispatch attempt concluded, for the stepper to turn into an exit.
///
/// `error` is `POUND_SUCCESS` when the step concluded normally; otherwise the
/// hard failure. `exit` classifies the outcome either way. `reload` asks the
/// stepper to re-dispatch from scratch -- a block vanished between `find_ready`
/// and `acquire`, or a freshly interned slot was vacated before it could be
/// claimed -- and is bounded by `JIT_EXECUTION_MAX_RELOADS` in the stepper.
typedef struct
{
    error_t              error;
    jit_execution_exit_t exit;
    bool                 reload;
} dispatch_result_t;

/// Runs the block in READY slot `index` to completion.
///
/// Takes and drops the lease around the execution, so the block's host code is
/// pinned for exactly as long as it is inside it -- the guarantee invalidation
/// depends on. Sets `reload` when the block vanished between `find_ready` and
/// `acquire`, which is an invalidation racing the dispatch.
static dispatch_result_t
run_entry(jit_execution_t *POUND_RESTRICT exec, const size_t index)
{
    jit_metadata_info_t info;

    const error_t err = jit_metadata_acquire(exec->metadata, index, &info);

    if (POUND_SUCCESS != err)
    {
        if (POUND_ERROR_NOT_INITIALIZED == err)
        {
            // Never logs (the metadata contract says so) and means the block was
            // invalidated out from under the dispatch. Re-dispatch.
            count(exec, &exec->stale_acquires);

            return (dispatch_result_t){.error = POUND_SUCCESS,
                                       .exit = JIT_EXECUTION_EXIT_NONE,
                                       .reload = true};
        }

        // The only other fail is the lease ceiling.
        count(exec, &exec->lease_refusals);

        return (dispatch_result_t){.error = err, .exit = JIT_EXECUTION_EXIT_ERROR, .reload = false};
    }

    dispatch_result_t result;

    if (0U != (info.flags & JIT_METADATA_FLAG_INTERPRETED))
    {
        if (NULL == exec->interpret)
        {
            POUND_LOG_ERROR(&thread_logger,
                            "Guest address 0x%llx was published as interpreted but no interpret() "
                            "hook is installed, so it cannot be run.",
                            (unsigned long long)info.guest_pc);

            count(exec, &exec->dispatch_failures);

            result = (dispatch_result_t){.error = POUND_ERROR_NOT_INITIALIZED,
                                          .exit = JIT_EXECUTION_EXIT_FAILED,
                                          .reload = false};
        }
        else
        {
            // Only an interpretation that actually runs is a "block
            // interpreted"; a block refused because the hook is missing is a
            // dispatch failure and nothing else.
            count(exec, &exec->blocks_interpreted);

            const error_t interpreted = exec->interpret(exec, exec->context, info.guest_pc);

            if (POUND_SUCCESS != interpreted)
            {
                POUND_LOG_ERROR(&thread_logger,
                                "The interpreter failed guest address 0x%llx (%s).",
                                (unsigned long long)info.guest_pc,
                                pound_error_to_string(interpreted));

                count(exec, &exec->dispatch_failures);

                result = (dispatch_result_t){.error = interpreted,
                                              .exit = JIT_EXECUTION_EXIT_FAILED,
                                              .reload = false};
            }
            else
            {
                result = (dispatch_result_t){.error = POUND_SUCCESS,
                                              .exit = (0U != (info.flags & JIT_METADATA_FLAG_SYSCALL))
                                                          ? JIT_EXECUTION_EXIT_SYSCALL
                                                          : JIT_EXECUTION_EXIT_NONE,
                                              .reload = false};

                if (JIT_EXECUTION_EXIT_SYSCALL == result.exit)
                {
                    count(exec, &exec->syscall_exits);
                }
            }
        }
    }
    else
    {
        if (NULL == info.host_code)
        {
            // A READY block with no host code and no INTERPRETED flag is a
            // corrupted entry: `publish` refuses NULL code with non-zero size,
            // so only a memory fault could have produced this.
            POUND_LOG_ERROR(&thread_logger,
                            "Guest address 0x%llx in slot %zu is READY with no host code and no "
                            "INTERPRETED flag; the entry is corrupt.",
                            (unsigned long long)info.guest_pc,
                            index);

            count(exec, &exec->dispatch_failures);

            result = (dispatch_result_t){.error = POUND_ERROR_CORRUPTED,
                                          .exit = JIT_EXECUTION_EXIT_ERROR,
                                          .reload = false};
        }
        else
        {
            // Function pointers and object pointers are not interchangeable in
            // ISO C, and a pedantic build warns about a direct cast. The
            // round trip through `memcpy` is the portable spelling, and it is
            // exactly what the emulator itself will do with a compiled block.
            count(exec, &exec->blocks_run);

            jit_execution_block_entry_fn entry;

            memcpy(&entry, &info.host_code, sizeof(entry));

            entry(exec->state);

            result = (dispatch_result_t){.error = POUND_SUCCESS,
                                          .exit = (0U != (info.flags & JIT_METADATA_FLAG_SYSCALL))
                                                      ? JIT_EXECUTION_EXIT_SYSCALL
                                                      : JIT_EXECUTION_EXIT_NONE,
                                          .reload = false};

            if (JIT_EXECUTION_EXIT_SYSCALL == result.exit)
            {
                count(exec, &exec->syscall_exits);
            }
        }
    }

    jit_metadata_release(exec->metadata, index);

    return result;
}

/// Declared in and driven by `dispatch_miss`.
static dispatch_result_t resolve_contention(jit_execution_t *POUND_RESTRICT exec, const uint64_t guest_pc);

/// Translates and publishes the block claimed at `index`, then runs it.
///
/// The guest is stopped on `guest_pc`, so the block being built is the block
/// that address is waiting for: the step not only fills the miss, it completes
/// the dispatch. Every code block is pushed onto the ownership registry *before*
/// publication, so the "invalidate then free, never reordered" contract cannot
/// lose track of it.
static dispatch_result_t
translate_and_dispatch(jit_execution_t *POUND_RESTRICT exec, const size_t index, const uint64_t guest_pc)
{
    void *POUND_RESTRICT buffer = jit_cache_alloc_executable(exec->cache, exec->block_code_bytes);

    if (NULL == buffer)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "The code cache could not provide %zu executable bytes for guest address "
                        "0x%llx; the block is recorded as failed.",
                        exec->block_code_bytes,
                        (unsigned long long)guest_pc);

        const error_t recorded = record_bad_translation(exec, index, POUND_ERROR_ALLOCATION_FAILED,
                                                        "the executable code cache refused a block");

        return (dispatch_result_t){.error = recorded, .exit = JIT_EXECUTION_EXIT_FAILED, .reload = false};
    }

    const size_t capacity = jit_cache_usable_size(exec->cache, buffer);

    size_t   host_size  = 0U;
    uint64_t guest_size = 0U;
    uint32_t flags      = 0U;
    char     detail[JIT_METADATA_LABEL_MAX];

    detail[0] = '\0';

    const error_t translated = exec->translate(exec,
                                               exec->context,
                                               guest_pc,
                                               index,
                                               buffer,
                                               capacity,
                                               &host_size,
                                               &guest_size,
                                               &flags,
                                               detail,
                                               sizeof(detail));

    if (POUND_SUCCESS != translated)
    {
        const error_t recorded = record_bad_translation(exec, index, translated, detail);

        jit_cache_free_executable(exec->cache, buffer);

        return (dispatch_result_t){.error = recorded, .exit = JIT_EXECUTION_EXIT_FAILED, .reload = false};
    }

    if (0U == host_size)
    {
        // An interpreter block: there is no host code to keep. Record the
        // decision in the table so it is made once, not on every dispatch.
        flags |= JIT_METADATA_FLAG_INTERPRETED;

        const error_t published = jit_metadata_publish(exec->metadata, index, NULL, 0U, guest_size, flags);

        jit_cache_free_executable(exec->cache, buffer);

        if (POUND_SUCCESS != published)
        {
            POUND_LOG_ERROR(&thread_logger,
                            "Interpreter block at 0x%llx could not be published (%s), despite holding "
                            "the claim; the publication protocol was violated.",
                            (unsigned long long)guest_pc,
                            pound_error_to_string(published));

            return (dispatch_result_t){.error = published, .exit = JIT_EXECUTION_EXIT_ERROR, .reload = false};
        }

        count(exec, &exec->publications);

        return run_entry(exec, index);
    }

    const error_t protected_rx = jit_cache_protect_rx(exec->cache, buffer);

    if (POUND_SUCCESS != protected_rx)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "The %zu byte block at 0x%llx could not be made executable (%s); the "
                        "translation is discarded and the block recorded as failed.",
                        host_size,
                        (unsigned long long)guest_pc,
                        pound_error_to_string(protected_rx));

        const error_t recorded = record_bad_translation(exec, index, protected_rx,
                                                        "the code cache refused to make the block executable");

        jit_cache_free_executable(exec->cache, buffer);

        return (dispatch_result_t){.error = recorded, .exit = JIT_EXECUTION_EXIT_FAILED, .reload = false};
    }

    // Register ownership before the block becomes callable, so that an
    // invalidation arriving after publication can find it to free.
    const error_t owned = registry_push(exec, guest_pc, guest_size, buffer);

    if (POUND_SUCCESS != owned)
    {
        const error_t recorded = record_bad_translation(exec, index, owned,
                                                        "the dispatcher's usage ledger could not record the block");

        jit_cache_free_executable(exec->cache, buffer);

        return (dispatch_result_t){.error = recorded, .exit = JIT_EXECUTION_EXIT_FAILED, .reload = false};
    }

    const error_t published = jit_metadata_publish(exec->metadata, index, buffer, host_size, guest_size, flags);

    if (POUND_SUCCESS != published)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Host block at 0x%llx could not be published (%s), despite holding the "
                        "claim; the publication protocol was violated.",
                        (unsigned long long)guest_pc,
                        pound_error_to_string(published));

        (void)registry_remove(exec, buffer);

        jit_cache_free_executable(exec->cache, buffer);

        return (dispatch_result_t){.error = published, .exit = JIT_EXECUTION_EXIT_ERROR, .reload = false};
    }

    count(exec, &exec->publications);

    return run_entry(exec, index);
}

/// Fills a dispatch miss: intern, claim, translate, publish, run.
static dispatch_result_t
dispatch_miss(jit_execution_t *POUND_RESTRICT exec, const uint64_t guest_pc)
{
    size_t index = 0U;

    const error_t interned = jit_metadata_intern(exec->metadata, guest_pc, &index);

    if (POUND_SUCCESS != interned)
    {
        // The table is full, or the lone other hard failure. The metadata
        // contract says a full table means "the JIT has stopped growing"; the
        // caller sees this as a typed error rather than a hang.
        return (dispatch_result_t){.error = interned, .exit = JIT_EXECUTION_EXIT_ERROR, .reload = false};
    }

    jit_metadata_state_t previous_state = JIT_METADATA_STATE_UNOCCUPIED;

    const error_t claimed = jit_metadata_try_begin(exec->metadata, index, &previous_state);

    if (POUND_SUCCESS == claimed)
    {
        count(exec, &exec->claims);
        count(exec, &exec->misses);

        return translate_and_dispatch(exec, index, guest_pc);
    }

    if (POUND_ERROR_ALREADY_INITIALIZED != claimed)
    {
        return (dispatch_result_t){.error = claimed, .exit = JIT_EXECUTION_EXIT_ERROR, .reload = false};
    }

    switch (previous_state)
    {
        case JIT_METADATA_STATE_CLAIMED:
            count(exec, &exec->claim_conflicts);

            return resolve_contention(exec, guest_pc);

        case JIT_METADATA_STATE_READY:
            // Another thread published between our intern and our try_begin.
            // The block is there now; run it.
            count(exec, &exec->claim_conflicts);

            return run_entry(exec, index);

        case JIT_METADATA_STATE_FAILED:
            // Known untranslatable, and the dispatcher asked for it anyway.
            {
                count(exec, &exec->claim_conflicts);

                error_t reason = POUND_ERROR_UNSUPPORTED_INSTRUCTION;
                char    why[JIT_METADATA_LABEL_MAX];

                why[0] = '\0';

                (void)jit_metadata_failure_reason(exec->metadata, index, &reason, why, sizeof(why));

                POUND_LOG_ERROR(&thread_logger,
                                "Dispatch asked for guest address 0x%llx, which is recorded as "
                                "untranslatable (%s): %s.",
                                (unsigned long long)guest_pc,
                                pound_error_to_string(reason),
                                ('\0' == why[0]) ? "no further detail" : why);

                count(exec, &exec->dispatch_failures);

                return (dispatch_result_t){.error = reason, .exit = JIT_EXECUTION_EXIT_FAILED, .reload = false};
            }

        case JIT_METADATA_STATE_VACANT:
            // The freshly interned address was invalidated back out between our
            // intern and our claim. Re-dispatch; the reload is bounded.
            return (dispatch_result_t){.error = POUND_SUCCESS,
                                        .exit = JIT_EXECUTION_EXIT_NONE,
                                        .reload = true};

        default:
            // EMPTY and UNOCCUPIED cannot refuse try_begin, so a refusal with
            // either as the previous state means the table was corrupted.
            POUND_LOG_ERROR(&thread_logger,
                            "Slot %zu refused a claim from %s, which it cannot be in; the table "
                            "is corrupt.",
                            index,
                            jit_metadata_state_to_string(previous_state));

            return (dispatch_result_t){.error = POUND_ERROR_CORRUPTED,
                                        .exit = JIT_EXECUTION_EXIT_ERROR,
                                        .reload = false};
    }
}

/// Resolves a claim held by another thread, per the configured policy.
static dispatch_result_t
resolve_contention(jit_execution_t *POUND_RESTRICT exec, const uint64_t guest_pc)
{
    switch (exec->contention_policy)
    {
        case JIT_EXECUTION_CONTENTION_SPIN:
        {
            uint32_t spin = 1U;

            for (; spin <= exec->claim_spin_budget; ++spin)
            {
                size_t index = 0U;

                if (POUND_SUCCESS == jit_metadata_find_ready(exec->metadata, guest_pc, &index))
                {
                    count(exec, &exec->hits);

                    // The other thread's publication made the block callable.
                    return run_entry(exec, index);
                }
            }

            POUND_LOG_WARN(&thread_logger,
                           "Guest address 0x%llx stayed claimed for the whole %u spin budget; "
                           "refusing to wait on a translation that is not coming.",
                           (unsigned long long)guest_pc,
                           exec->claim_spin_budget);

            count(exec, &exec->spin_exhausted);

            return (dispatch_result_t){.error = POUND_ERROR_BUSY,
                                        .exit = JIT_EXECUTION_EXIT_CONTENTION,
                                        .reload = false};
        }

        case JIT_EXECUTION_CONTENTION_INTERPRET:
            if (NULL == exec->interpret)
            {
                POUND_LOG_WARN(&thread_logger,
                               "The INTERPRET contention policy cannot run guest address 0x%llx "
                               "interpretively: no interpret() hook is installed. Deferring instead.",
                               (unsigned long long)guest_pc);

                count(exec, &exec->deferrals);

                return (dispatch_result_t){.error = POUND_SUCCESS,
                                            .exit = JIT_EXECUTION_EXIT_CONTENTION,
                                            .reload = false};
            }

            count(exec, &exec->contention_interpreted);
            count(exec, &exec->blocks_interpreted);

            {
                const error_t interpreted = exec->interpret(exec, exec->context, guest_pc);

                if (POUND_SUCCESS != interpreted)
                {
                    POUND_LOG_ERROR(&thread_logger,
                                    "The interpreter failed contended guest address 0x%llx (%s).",
                                    (unsigned long long)guest_pc,
                                    pound_error_to_string(interpreted));

                    count(exec, &exec->dispatch_failures);

                    return (dispatch_result_t){.error = interpreted,
                                                .exit = JIT_EXECUTION_EXIT_FAILED,
                                                .reload = false};
                }
            }

            return (dispatch_result_t){.error = POUND_SUCCESS,
                                        .exit = JIT_EXECUTION_EXIT_NONE,
                                        .reload = false};

        case JIT_EXECUTION_CONTENTION_DEFER:
            count(exec, &exec->deferrals);

            return (dispatch_result_t){.error = POUND_SUCCESS,
                                        .exit = JIT_EXECUTION_EXIT_CONTENTION,
                                        .reload = false};

        default:
            POUND_LOG_ERROR(&thread_logger,
                            "The dispatcher carries contention policy %d, which is not defined.",
                            (int)exec->contention_policy);

            return (dispatch_result_t){.error = POUND_ERROR_CORRUPTED,
                                        .exit = JIT_EXECUTION_EXIT_ERROR,
                                        .reload = false};
    }
}

/// Dispatches one guest block at `state->pc`.
///
/// Returns a concise outcome; `step` and the `run` loop turn it into their
/// contracts. A block that keeps vanishing mid-dispatch reloads up to
/// `JIT_EXECUTION_MAX_RELOADS` times before the loop refuses.
static dispatch_result_t
dispatch_once(jit_execution_t *POUND_RESTRICT exec)
{
    const uint64_t pc = exec->state->pc;

    if ((0U != exec->halt_pc) && (pc == exec->halt_pc))
    {
        return (dispatch_result_t){.error = POUND_SUCCESS,
                                    .exit = JIT_EXECUTION_EXIT_HALTED,
                                    .reload = false};
    }

    for (unsigned reloads = 0U; reloads < JIT_EXECUTION_MAX_RELOADS; ++reloads)
    {
        size_t index = 0U;

        const error_t found = jit_metadata_find_ready(exec->metadata, pc, &index);

        if (POUND_SUCCESS == found)
        {
            count(exec, &exec->hits);

            const dispatch_result_t result = run_entry(exec, index);

            if (result.reload)
            {
                continue;
            }

            return result;
        }

        if (POUND_ERROR_NOT_FOUND != found)
        {
            return (dispatch_result_t){.error = found, .exit = JIT_EXECUTION_EXIT_ERROR, .reload = false};
        }

        const dispatch_result_t result = dispatch_miss(exec, pc);

        if (result.reload)
        {
            continue;
        }

        return result;
    }

    POUND_LOG_WARN(&thread_logger,
                   "Guest address 0x%llx has vanished %u times in a row while being dispatched; "
                   "refusing to spin on an invalidation storm.",
                   (unsigned long long)pc,
                   JIT_EXECUTION_MAX_RELOADS);

    return (dispatch_result_t){.error = POUND_ERROR_BUSY,
                                .exit = JIT_EXECUTION_EXIT_CONTENTION,
                                .reload = false};
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

error_t
jit_execution_init(jit_execution_t *POUND_RESTRICT exec, const jit_execution_config_t *POUND_RESTRICT config)
{
    if (NULL == exec)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the dispatcher is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (NULL == config)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the dispatcher configuration is NULL. A dispatcher with "
                        "no manager, cache, state or translator has nothing to run.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (exec->initialised)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the dispatcher is already initialised; destroy it first.");
        return POUND_ERROR_ALREADY_INITIALIZED;
    }

    // Zeroed first, so a rejected `init` leaves a dispatcher whose every entry
    // point reports "not initialised" rather than one whose pointers are
    // whatever the caller's stack happened to hold.
    memset(exec, 0, sizeof(*exec));

    if (NULL == config->metadata)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the dispatcher configuration has no metadata manager.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (NULL == config->cache)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the dispatcher configuration has no code cache.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (NULL == config->state)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the dispatcher configuration has no guest state.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (NULL == config->ops)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the dispatcher configuration has no translator ops.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (NULL == config->ops->translate)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the dispatcher configuration has no translate callback. "
                        "A loop that cannot fill a miss can only run blocks somebody else published.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if ((config->contention_policy < JIT_EXECUTION_CONTENTION_SPIN) ||
        (config->contention_policy > JIT_EXECUTION_CONTENTION_DEFER))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: contention policy %d is not defined.",
                        (int)config->contention_policy);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    exec->metadata            = config->metadata;
    exec->cache               = config->cache;
    exec->state               = config->state;
    exec->contention_policy   = config->contention_policy;
    exec->claim_spin_budget   = (0U == config->claim_spin_budget) ? JIT_EXECUTION_DEFAULT_SPIN_BUDGET
                                                                  : config->claim_spin_budget;
    exec->block_code_bytes    = (0U == config->block_code_bytes) ? JIT_EXECUTION_DEFAULT_BLOCK_CODE_BYTES
                                                                 : config->block_code_bytes;
    exec->max_blocks_per_run  = config->max_blocks_per_run;
    exec->halt_pc             = config->halt_pc;
    exec->context             = config->ops->context;
    exec->translate           = config->ops->translate;
    exec->interpret           = config->ops->interpret;
    exec->account             = !config->no_accounting;
    exec->counters_lock_free  = counters_are_lock_free();

    // The ownership registry is sized at init rather than on the first push, so
    // that its shape is visible to `describe` and a broken allocation fails the
    // dispatcher's construction instead of its first translation.
    jit_execution_owner_t *POUND_RESTRICT owners =
        (jit_execution_owner_t *)memory_subsystem_allocate(
            8U, (size_t)JIT_EXECUTION_OWNER_REGISTRY_INITIAL * sizeof(jit_execution_owner_t));

    if (NULL == owners)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Could not allocate the dispatcher's %u-entry block ownership registry.",
                        (unsigned)JIT_EXECUTION_OWNER_REGISTRY_INITIAL);

        return POUND_ERROR_ALLOCATION_FAILED;
    }

    exec->owners         = owners;
    exec->owner_capacity = JIT_EXECUTION_OWNER_REGISTRY_INITIAL;

    atomic_init(&exec->stop_requested, false);

    clear_counters(exec);

    exec->initialised = true;

    if (exec->account && !exec->counters_lock_free)
    {
        POUND_LOG_WARN(&thread_logger,
                       "The dispatcher's 64-bit counters are not lock-free on this host, so every "
                       "step pays for a locked increment. They are still maintained because they "
                       "were asked for; set jit_execution_config_t::no_accounting to stop paying "
                       "for them.");
    }

    POUND_LOG_INFO(&thread_logger,
                   "Dispatcher initialised against metadata %p, cache %p, guest state %p with the "
                   "%s contention policy.",
                   (const void *)exec->metadata,
                   (const void *)exec->cache,
                   (const void *)exec->state,
                   (JIT_EXECUTION_CONTENTION_SPIN == exec->contention_policy)
                       ? "spin"
                       : ((JIT_EXECUTION_CONTENTION_INTERPRET == exec->contention_policy) ? "interpret"
                                                                                           : "defer"));

    return POUND_SUCCESS;
}

void
jit_execution_destroy(jit_execution_t *POUND_RESTRICT exec)
{
    if (NULL == exec)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the dispatcher is NULL.");
        return;
    }

    if (!exec->initialised)
    {
        POUND_LOG_INFO(&thread_logger, "The dispatcher is not initialised; nothing to tear down.");
        return;
    }

    if (0U != exec->owner_count)
    {
        POUND_LOG_WARN(&thread_logger,
                       "The dispatcher was torn down while still owning %zu code block(s). Their "
                       "executable bytes are not freed: the code cache outlives this loop, and "
                       "whose cache this is is the caller's to order. Reset or invalidate before "
                       "destroy to avoid leaking them.",
                       exec->owner_count);
    }

    if (NULL != exec->owners)
    {
        memory_subsystem_free(exec->owners);
    }

    memset(exec, 0, sizeof(*exec));
}

error_t
jit_execution_describe(const jit_execution_t *POUND_RESTRICT exec, jit_execution_resolved_t *POUND_RESTRICT out)
{
    if (NULL == exec)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the dispatcher is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (NULL == out)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the resolved-configuration destination is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!exec->initialised)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the dispatcher is not initialised.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    memset(out, 0, sizeof(*out));

    out->contention_policy  = exec->contention_policy;
    out->spin_budget        = exec->claim_spin_budget;
    out->block_code_bytes   = exec->block_code_bytes;
    out->max_blocks_per_run = exec->max_blocks_per_run;
    out->halt_pc            = exec->halt_pc;
    out->account            = exec->account;
    out->counters_lock_free = exec->counters_lock_free;
    out->owner_count        = exec->owner_count;
    out->owner_capacity     = exec->owner_capacity;

    return POUND_SUCCESS;
}

error_t
jit_execution_step(jit_execution_t *POUND_RESTRICT exec, jit_execution_exit_t *POUND_RESTRICT out_exit)
{
    if (NULL == exec)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the dispatcher is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (NULL == out_exit)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the exit-reason destination is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!exec->initialised)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the dispatcher is not initialised.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    if (atomic_exchange_explicit(&exec->stop_requested, false, memory_order_relaxed))
    {
        count(exec, &exec->stops);

        *out_exit = JIT_EXECUTION_EXIT_STOPPED;

        return POUND_SUCCESS;
    }

    const dispatch_result_t result = dispatch_once(exec);

    *out_exit = result.exit;

    return result.error;
}

error_t
jit_execution_run(jit_execution_t *POUND_RESTRICT exec, jit_execution_exit_t *POUND_RESTRICT out_exit)
{
    if (NULL == exec)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the dispatcher is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (NULL == out_exit)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the exit-reason destination is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!exec->initialised)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the dispatcher is not initialised.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    uint64_t executed = 0U;

    for (;;)
    {
        if ((0U != exec->max_blocks_per_run) && (executed >= exec->max_blocks_per_run))
        {
            count(exec, &exec->quota_hits);

            *out_exit = JIT_EXECUTION_EXIT_QUOTA;

            return POUND_SUCCESS;
        }

        if (atomic_exchange_explicit(&exec->stop_requested, false, memory_order_relaxed))
        {
            count(exec, &exec->stops);

            *out_exit = JIT_EXECUTION_EXIT_STOPPED;

            return POUND_SUCCESS;
        }

        const dispatch_result_t result = dispatch_once(exec);

        if (POUND_SUCCESS != result.error)
        {
            *out_exit = result.exit;

            return result.error;
        }

        switch (result.exit)
        {
            case JIT_EXECUTION_EXIT_NONE:
                ++executed;
                continue;

            case JIT_EXECUTION_EXIT_SYSCALL:
                ++executed;
                *out_exit = JIT_EXECUTION_EXIT_SYSCALL;
                return POUND_SUCCESS;

            case JIT_EXECUTION_EXIT_HALTED:
            case JIT_EXECUTION_EXIT_CONTENTION:
                *out_exit = result.exit;
                return POUND_SUCCESS;

            default:
                // FAILED and ERROR travel as errors, caught above; STOPPED and
                // QUOTA are produced only by this loop itself. Neither is
                // reachable here, and returning is safer than looping.
                *out_exit = result.exit;
                return POUND_SUCCESS;
        }
    }
}

void
jit_execution_stop(jit_execution_t *POUND_RESTRICT exec)
{
    if (NULL == exec)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the dispatcher is NULL.");
        return;
    }

    if (!exec->initialised)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the dispatcher is not initialised.");
        return;
    }

    atomic_store_explicit(&exec->stop_requested, true, memory_order_relaxed);
}

error_t
jit_execution_invalidate_range(jit_execution_t *POUND_RESTRICT exec,
                               const uint64_t                 begin,
                               const uint64_t                 end,
                               size_t *POUND_RESTRICT         out_invalidated)
{
    if (NULL == exec)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the dispatcher is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!exec->initialised)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the dispatcher is not initialised.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    // The metadata pass comes first and decides everything. On success -- and
    // only on success -- the freed half runs, because between the two calls the
    // cleared blocks are unreachable but their memory is still mapped. On
    // POUND_ERROR_BUSY nothing is cleared, so nothing is freed.
    const error_t invalidated = jit_metadata_invalidate_range(exec->metadata, begin, end, out_invalidated);

    if (POUND_SUCCESS == invalidated)
    {
        count(exec, &exec->invalidations);

        if (end != begin)
        {
            registry_free_overlap(exec, begin, end - begin);
        }
    }

    return invalidated;
}

error_t
jit_execution_reset(jit_execution_t *POUND_RESTRICT exec)
{
    if (NULL == exec)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the dispatcher is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!exec->initialised)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the dispatcher is not initialised.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    // Invalidate the whole address space first: that refuses with BUSY if any
    // thread is inside a block, which is exactly when a reset must not proceed.
    size_t invalidated = 0U;

    const error_t cleared = jit_metadata_invalidate_range(exec->metadata, 0U, UINT64_MAX, &invalidated);

    if (POUND_SUCCESS != cleared)
    {
        return cleared;
    }

    registry_free_overlap(exec, 0U, UINT64_MAX);

    count(exec, &exec->invalidations);

    // The metadata table and the code cache reset underneath us; every block is
    // gone, so neither has live blocks to keep.
    jit_metadata_reset(exec->metadata);
    jit_cache_reset(exec->cache);

    clear_counters(exec);

    POUND_LOG_INFO(&thread_logger,
                   "Dispatcher reset: cleared %zu block(s), freed %zu code block(s); the metadata "
                   "table, the code cache and the counters are as fresh as at first init.",
                   invalidated,
                   exec->owner_count);

    return POUND_SUCCESS;
}

error_t
jit_execution_get_stats(const jit_execution_t *POUND_RESTRICT exec, jit_execution_stats_t *POUND_RESTRICT out)
{
    if (NULL == exec)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the dispatcher is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (NULL == out)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the statistics destination is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!exec->initialised)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the dispatcher is not initialised.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    memset(out, 0, sizeof(*out));

    out->hits                  = (uint64_t)atomic_load_explicit(&exec->hits, memory_order_relaxed);
    out->misses                = (uint64_t)atomic_load_explicit(&exec->misses, memory_order_relaxed);
    out->claims                = (uint64_t)atomic_load_explicit(&exec->claims, memory_order_relaxed);
    out->claim_conflicts       = (uint64_t)atomic_load_explicit(&exec->claim_conflicts, memory_order_relaxed);
    out->publications          = (uint64_t)atomic_load_explicit(&exec->publications, memory_order_relaxed);
    out->blocks_run            = (uint64_t)atomic_load_explicit(&exec->blocks_run, memory_order_relaxed);
    out->blocks_interpreted    = (uint64_t)atomic_load_explicit(&exec->blocks_interpreted, memory_order_relaxed);
    out->failures              = (uint64_t)atomic_load_explicit(&exec->failures, memory_order_relaxed);
    out->syscall_exits         = (uint64_t)atomic_load_explicit(&exec->syscall_exits, memory_order_relaxed);
    out->stops                 = (uint64_t)atomic_load_explicit(&exec->stops, memory_order_relaxed);
    out->quota_hits            = (uint64_t)atomic_load_explicit(&exec->quota_hits, memory_order_relaxed);
    out->deferrals             = (uint64_t)atomic_load_explicit(&exec->deferrals, memory_order_relaxed);
    out->contention_interpreted = (uint64_t)atomic_load_explicit(&exec->contention_interpreted,
                                                                memory_order_relaxed);
    out->spin_exhausted        = (uint64_t)atomic_load_explicit(&exec->spin_exhausted, memory_order_relaxed);
    out->stale_acquires        = (uint64_t)atomic_load_explicit(&exec->stale_acquires, memory_order_relaxed);
    out->lease_refusals        = (uint64_t)atomic_load_explicit(&exec->lease_refusals, memory_order_relaxed);
    out->dispatch_failures     = (uint64_t)atomic_load_explicit(&exec->dispatch_failures, memory_order_relaxed);
    out->invalidations         = (uint64_t)atomic_load_explicit(&exec->invalidations, memory_order_relaxed);
    out->blocks_freed          = (uint64_t)atomic_load_explicit(&exec->blocks_freed, memory_order_relaxed);
    out->account               = exec->account;
    out->counters_lock_free    = exec->counters_lock_free;

    return POUND_SUCCESS;
}

void
jit_execution_log_summary(const jit_execution_t *POUND_RESTRICT exec)
{
    if (NULL == exec)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the dispatcher is NULL.");
        return;
    }

    if (!exec->initialised)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the dispatcher is not initialised.");
        return;
    }

    jit_execution_stats_t stats;

    if (POUND_SUCCESS != jit_execution_get_stats(exec, &stats))
    {
        return;
    }

    POUND_LOG_INFO(&thread_logger,
                   "Dispatcher: %llu block(s) run as host code, %llu interpreted, %llu "
                   "published, %llu hit(s), %llu miss(es), %llu contention conflict(s), %llu "
                   "block(s) freed by %llu invalidation(s); %zu code block(s) owned.",
                   (unsigned long long)stats.blocks_run,
                   (unsigned long long)stats.blocks_interpreted,
                   (unsigned long long)stats.publications,
                   (unsigned long long)stats.hits,
                   (unsigned long long)stats.misses,
                   (unsigned long long)stats.claim_conflicts,
                   (unsigned long long)stats.blocks_freed,
                   (unsigned long long)stats.invalidations,
                   exec->owner_count);
}

const char *
jit_execution_exit_to_string(const jit_execution_exit_t exit_code)
{
    switch (exit_code)
    {
        case JIT_EXECUTION_EXIT_NONE:
            return "none";

        case JIT_EXECUTION_EXIT_SYSCALL:
            return "syscall";

        case JIT_EXECUTION_EXIT_HALTED:
            return "halted";

        case JIT_EXECUTION_EXIT_STOPPED:
            return "stopped";

        case JIT_EXECUTION_EXIT_QUOTA:
            return "quota";

        case JIT_EXECUTION_EXIT_CONTENTION:
            return "contention";

        case JIT_EXECUTION_EXIT_FAILED:
            return "failed";

        case JIT_EXECUTION_EXIT_ERROR:
            return "error";

        default:
            return "unrecognised";
    }
}

size_t
jit_execution_owner_count(const jit_execution_t *POUND_RESTRICT exec)
{
    if ((NULL == exec) || (!exec->initialised))
    {
        return 0U;
    }

    return exec->owner_count;
}

bool
jit_execution_is_owner(const jit_execution_t *POUND_RESTRICT exec, const void *POUND_RESTRICT code)
{
    if ((NULL == exec) || (!exec->initialised) || (NULL == code) || (NULL == exec->owners))
    {
        return false;
    }

    for (size_t index = 0U; index < exec->owner_count; ++index)
    {
        if (exec->owners[index].code == code)
        {
            return true;
        }
    }

    return false;
}

/*** end of file ***/
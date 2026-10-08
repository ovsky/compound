//! Tests for the execution loop: the dispatcher that binds the metadata
//! manager, the code cache and the guest register file together.
//!
//! The loop is the first module whose subject is *behaviour* rather than a data
//! structure -- it calls functions, runs blocks and advances the guest -- so
//! most of these cases are "did the right thing happen to the state", not "is
//! the table still navigable".
//!
//! Two fakes stand in for the translator that will eventually bind Ballistic
//! underneath this module:
//!
//! - `fake_translate` produces host code -- a single `ret` instruction, which is
//!   one byte on x86-64 and four on AArch64 -- or, with a zero host size, an
//!   interpreter block. Executing `ret` on a freshly protected cache page is
//!   real code generation territory and it is safe: the block returns
//!   immediately and changes nothing.
//! - `fake_interpret` advances the guest PC and records what it was asked to
//!   interpret.
//!
//! The other blocks under test are *pre-published* straight into the metadata
//! table with the address of a real C function as their "host code", which
//! exercises the dispatch-and-call path without compiling anything.

#include "jit/jit_execution.h"

#include "pound_test.h"
#include <stdio.h>
#include <string.h>

/// Whether `fake_translate` can produce a safe block of actual machine code on
/// this host. Every lane in the build matrix (x86-64 and AArch64, Windows and
/// POSIX) can; the fallback is for a host that cannot, on which the host-code
/// tests would visibly fail rather than execute garbage.
#if defined(__x86_64__) || defined(__aarch64__)
#define HOST_BLOCK_BYTES_AVAILABLE 1
#else
#define HOST_BLOCK_BYTES_AVAILABLE 0
#endif

/// Knobs the fake translator reads, and the log it writes so the tests can
/// assert what the dispatcher asked of it.
typedef struct
{
    /// What translation returns. `POUND_SUCCESS` translates; anything else
    /// refuses the block.
    error_t result;

    /// Guest PC at which the translator refuses, regardless of `result`.
    uint64_t refused_pc;

    /// Bytes of host code to produce. Zero publishes an interpreter block.
    size_t host_size;

    /// Guest bytes the translated block covers.
    uint64_t guest_size;

    /// Flags to publish with.
    uint32_t flags;

    /// Detail recorded on refusal.
    char detail[JIT_METADATA_LABEL_MAX];

    /// How many times the translator was called.
    uint32_t calls;

    /// Code capacity the translator was offered, for the allocation-shape test.
    size_t last_code_capacity;

    /// Guest PC of the most recent call.
    uint64_t last_guest_pc;
} translate_knobs_t;

/// Knobs the fake interpreter reads, and the log it writes.
typedef struct
{
    /// What interpretation returns.
    error_t result;

    /// How far the guest PC advances per interpretation.
    uint64_t advance;

    /// How many times the interpreter was called.
    uint32_t calls;

    /// Guest PCs the interpreter was asked about (bounded, for assertions).
    uint64_t pcs[16];
    uint32_t pc_count;
} interpret_knobs_t;

/// Everything one test needs, wired so that both callbacks share one context.
typedef struct
{
    jit_execution_t exec;
    jit_metadata_t  metadata;
    jit_cache_t     cache;
    guest_state_t   state;

    translate_knobs_t translate;
    interpret_knobs_t interpret;
} harness_t;

/// The fake translator. See the file comment for what the produced bytes are.
static error_t
fake_translate(jit_execution_t *POUND_RESTRICT exec,
               void *POUND_RESTRICT           context,
               uint64_t                       guest_pc,
               size_t                         slot_index,
               void *POUND_RESTRICT           code_buffer,
               size_t                         code_capacity,
               size_t *POUND_RESTRICT         out_host_size,
               uint64_t *POUND_RESTRICT       out_guest_size,
               uint32_t *POUND_RESTRICT       out_flags,
               char *POUND_RESTRICT           out_fail_detail,
               size_t                         fail_detail_capacity)
{
    (void)exec;
    (void)slot_index;

    // `context` is the harness (see `harness_new`); the knobs are its member.
    translate_knobs_t *knobs = &((harness_t *)context)->translate;

    ++knobs->calls;
    knobs->last_guest_pc = guest_pc;
    knobs->last_code_capacity = code_capacity;

    *out_host_size  = 0U;
    *out_guest_size = 0U;
    *out_flags      = 0U;

    // Refuse for a specific PC, even when `result` is success (a per-address
    // failure used by the retry test), and refuse globally when `result` is a
    // failure. On a refusal the diagnostics are recorded and the block is
    // marked FAILED.
    if ((guest_pc == knobs->refused_pc) || (POUND_SUCCESS != knobs->result))
    {
        if (guest_pc == knobs->refused_pc)
        {
            (void)snprintf(out_fail_detail, fail_detail_capacity, "%s", knobs->detail);
        }

        return knobs->result;
    }

    *out_guest_size = knobs->guest_size;
    *out_flags      = knobs->flags;

    if (0U == knobs->host_size)
    {
        return POUND_SUCCESS;
    }

#if defined(__x86_64__)
    if (code_capacity < 1U)
    {
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    ((unsigned char *)code_buffer)[0] = 0xC3U; /* ret */

    *out_host_size = 1U;

    return POUND_SUCCESS;
#elif defined(__aarch64__)
    if (code_capacity < 4U)
    {
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    ((unsigned char *)code_buffer)[0] = 0xC0U; /* ret, little-endian */
    ((unsigned char *)code_buffer)[1] = 0x03U;
    ((unsigned char *)code_buffer)[2] = 0x5FU;
    ((unsigned char *)code_buffer)[3] = 0xD6U;

    *out_host_size = 4U;

    return POUND_SUCCESS;
#else
    (void)code_buffer;
    return POUND_ERROR_UNSUPPORTED_INSTRUCTION;
#endif
}

/// The fake interpreter.
static error_t
fake_interpret(jit_execution_t *POUND_RESTRICT exec,
               void *POUND_RESTRICT           context,
               uint64_t                       guest_pc)
{
    // `context` is the harness (see `harness_new`); the knobs are its member.
    interpret_knobs_t *knobs = &((harness_t *)context)->interpret;

    ++knobs->calls;

    if (knobs->pc_count < 16U)
    {
        knobs->pcs[knobs->pc_count] = guest_pc;
        ++knobs->pc_count;
    }

    if (POUND_SUCCESS != knobs->result)
    {
        return knobs->result;
    }

    exec->state->pc += knobs->advance;

    return POUND_SUCCESS;
}

/// A real function, published into the metadata table as a block's "host code".
///
/// The point is that the dispatcher does not care whether its `host_code` is
/// the output of a compiler or the address of a C function: it calls it with
/// the guest state and continues.
static void
dispatch_target(guest_state_t *POUND_RESTRICT state)
{
    ++state->x[0];
    state->pc += 4U;
}

/// The address of `dispatch_target` as an object pointer.
///
/// ISO C does not let a function pointer be cast to `void *` on every ABI, so
/// the address travels through a stored function-pointer object whose bytes are
/// copied. Copying from `&dispatch_target` directly would copy the first bytes
/// of the function's machine code, not its address.
static void *
dispatch_target_entry(void)
{
    jit_execution_block_entry_fn block = dispatch_target;

    void *storage = NULL;

    memcpy(&storage, &block, sizeof(storage));

    return storage;
}

/// Zeroes a harness and binds its pieces together.
///
/// `exec_config` and `metadata_config` may be NULL for defaults. The translate
/// and interpret callbacks are always installed; the tests that need no
/// interpreter pass a config whose ops have a NULL `interpret` via
/// `harness_new_no_interpreter`.
static error_t
harness_new(harness_t *POUND_RESTRICT h,
            const jit_execution_config_t *POUND_RESTRICT exec_config,
            const jit_metadata_config_t *POUND_RESTRICT metadata_config,
            const translate_knobs_t *POUND_RESTRICT translate,
            const interpret_knobs_t *POUND_RESTRICT interpret)
{
    memset(h, 0, sizeof(*h));

    if (NULL != translate)
    {
        memcpy(&h->translate, translate, sizeof(h->translate));
    }
    else
    {
        h->translate.result    = POUND_SUCCESS;
        h->translate.guest_size = 4U;
    }

    if (NULL != interpret)
    {
        memcpy(&h->interpret, interpret, sizeof(h->interpret));
    }
    else
    {
        h->interpret.result  = POUND_SUCCESS;
        h->interpret.advance = 4U;
    }

    error_t err = jit_metadata_init(&h->metadata, metadata_config);

    if (POUND_SUCCESS != err)
    {
        return err;
    }

    err = jit_cache_init(&h->cache, NULL);

    if (POUND_SUCCESS != err)
    {
        jit_metadata_destroy(&h->metadata);
        return err;
    }

    err = guest_state_init(&h->state);

    if (POUND_SUCCESS != err)
    {
        jit_cache_destroy(&h->cache);
        jit_metadata_destroy(&h->metadata);
        return err;
    }

    jit_execution_config_t defaults;

    if (NULL != exec_config)
    {
        defaults = *exec_config;
    }
    else
    {
        memset(&defaults, 0, sizeof(defaults));
    }

    jit_execution_ops_t ops;

    memset(&ops, 0, sizeof(ops));

    ops.context   = h;
    ops.translate = fake_translate;
    ops.interpret = fake_interpret;

    defaults.metadata = &h->metadata;
    defaults.cache    = &h->cache;
    defaults.state    = &h->state;
    defaults.ops      = &ops;

    err = jit_execution_init(&h->exec, &defaults);

    if (POUND_SUCCESS != err)
    {
        jit_cache_destroy(&h->cache);
        jit_metadata_destroy(&h->metadata);
        return err;
    }

    return POUND_SUCCESS;
}

/// Tears the harness down in dependency order, resetting first so neither the
/// dispatcher nor the cache has a leak warning to log.
static void
harness_destroy(harness_t *POUND_RESTRICT h)
{
    if (h->exec.initialised)
    {
        (void)jit_execution_reset(&h->exec);
        jit_execution_destroy(&h->exec);
    }

    jit_cache_destroy(&h->cache);
    jit_metadata_destroy(&h->metadata);
}

/// Publishes a block directly into the metadata table.
static error_t
prepublish(harness_t *POUND_RESTRICT h,
           const uint64_t           guest_pc,
           void *POUND_RESTRICT     host_code,
           const size_t             host_size,
           const uint64_t           guest_size,
           const uint32_t           flags)
{
    size_t index = 0U;
    error_t err = jit_metadata_intern(&h->metadata, guest_pc, &index);

    if (POUND_SUCCESS != err)
    {
        return err;
    }

    jit_metadata_state_t previous = JIT_METADATA_STATE_UNOCCUPIED;

    err = jit_metadata_try_begin(&h->metadata, index, &previous);

    if (POUND_SUCCESS != err)
    {
        return err;
    }

    return jit_metadata_publish(&h->metadata, index, host_code, host_size, guest_size, flags);
}

/// Claims a slot (intern + try_begin) and leaves it CLAIMED, as a second
/// dispatcher thread would while it translates.
static error_t
preclaim(harness_t *POUND_RESTRICT h, const uint64_t guest_pc, size_t *POUND_RESTRICT out_index)
{
    size_t index = 0U;
    error_t err = jit_metadata_intern(&h->metadata, guest_pc, &index);

    if (POUND_SUCCESS != err)
    {
        return err;
    }

    jit_metadata_state_t previous = JIT_METADATA_STATE_UNOCCUPIED;

    err = jit_metadata_try_begin(&h->metadata, index, &previous);

    if (POUND_SUCCESS == err)
    {
        if (NULL != out_index)
        {
            *out_index = index;
        }
    }

    return err;
}

/// Acquires the READY block at `guest_pc` via the metadata manager (no lease is
/// taken on the dispatcher side), for inspecting the block the dispatcher built.
static error_t
read_block(harness_t *POUND_RESTRICT h,
           const uint64_t           guest_pc,
           size_t *POUND_RESTRICT   out_index,
           jit_metadata_info_t *POUND_RESTRICT out_info)
{
    size_t index = 0U;
    error_t err = jit_metadata_find_ready(&h->metadata, guest_pc, &index);

    if (POUND_SUCCESS != err)
    {
        return err;
    }

    err = jit_metadata_acquire(&h->metadata, index, out_info);

    if (POUND_SUCCESS == err)
    {
        if (NULL != out_index)
        {
            *out_index = index;
        }

        (void)jit_metadata_release(&h->metadata, index);
    }

    return err;
}

typedef jit_execution_exit_t exit_t;

// ---------------------------------------------------------------------------
// Init, teardown, validation
// ---------------------------------------------------------------------------

POUND_TEST(execution, a_dispatcher_with_no_dependencies_is_refused)
{
    harness_t h;

    memset(&h, 0, sizeof(h));

    jit_execution_config_t config;

    memset(&config, 0, sizeof(config));

    jit_execution_ops_t ops;

    memset(&ops, 0, sizeof(ops));
    ops.translate = fake_translate;
    config.ops    = &ops;

    // NULL dispatcher and NULL config.
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_execution_init(NULL, &config));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_execution_init(&h.exec, NULL));

    // Missing every required dependency, one at a time, in construction order.
    jit_execution_t uninit;

    // No metadata manager.
    memset(&uninit, 0, sizeof(uninit));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_execution_init(&uninit, &config));

    // No code cache.
    config.metadata = &h.metadata;
    memset(&uninit, 0, sizeof(uninit));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_execution_init(&uninit, &config));

    // No guest state.
    config.cache = &h.cache;
    memset(&uninit, 0, sizeof(uninit));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_execution_init(&uninit, &config));

    // No ops, and therefore no translator.
    config.state = &h.state;
    config.ops   = NULL;
    memset(&uninit, 0, sizeof(uninit));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_execution_init(&uninit, &config));

    // Ops present but no translate callback.
    config.ops            = &ops;
    ops.translate         = NULL;
    memset(&uninit, 0, sizeof(uninit));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_execution_init(&uninit, &config));

    // An undefined contention policy.
    ops.translate         = fake_translate;
    config.contention_policy = (jit_execution_contention_policy_t)99;
    memset(&uninit, 0, sizeof(uninit));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_execution_init(&uninit, &config));

    // A rejected init leaves a dispatcher that reports not-initialised.
    config.contention_policy = JIT_EXECUTION_CONTENTION_SPIN;
    memset(&uninit, 0, sizeof(uninit));
    POUND_CHECK(POUND_SUCCESS == jit_execution_init(&uninit, &config));

    jit_execution_resolved_t resolved;

    POUND_CHECK(POUND_ERROR_ALREADY_INITIALIZED == jit_execution_init(&uninit, &config));
    POUND_CHECK(POUND_SUCCESS == jit_execution_describe(&uninit, &resolved));
    jit_execution_destroy(&uninit);
}

POUND_TEST(execution, an_uninitialised_dispatcher_refuses_every_entry_point)
{
    harness_t h;

    memset(&h, 0, sizeof(h));

    exit_t exit_code = JIT_EXECUTION_EXIT_NONE;
    error_t err;

    // Init the pieces so the "dispatcher not initialised" paths are the only
    // thing refusing; a zeroed dispatcher with no dependencies refuses anyway.
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_init(&h.metadata, NULL));
    POUND_REQUIRE(POUND_SUCCESS == jit_cache_init(&h.cache, NULL));
    POUND_REQUIRE(POUND_SUCCESS == guest_state_init(&h.state));

    jit_execution_t uninit;

    memset(&uninit, 0, sizeof(uninit));

    jit_execution_resolved_t resolved;
    jit_execution_stats_t    stats;
    size_t                   count = 0U;

    POUND_CHECK(POUND_ERROR_NOT_INITIALIZED == jit_execution_step(&uninit, &exit_code));
    POUND_CHECK(POUND_ERROR_NOT_INITIALIZED == jit_execution_run(&uninit, &exit_code));
    POUND_CHECK(POUND_ERROR_NOT_INITIALIZED == jit_execution_describe(&uninit, &resolved));
    POUND_CHECK(POUND_ERROR_NOT_INITIALIZED == jit_execution_get_stats(&uninit, &stats));
    POUND_CHECK(POUND_ERROR_NOT_INITIALIZED == jit_execution_invalidate_range(&uninit, 0U, 1U, &count));
    POUND_CHECK(POUND_ERROR_NOT_INITIALIZED == jit_execution_reset(&uninit));

    // NULL outputs are refused even on a configured dispatcher, and NULL
    // dispatchers are refused everywhere.
    jit_execution_config_t config;

    memset(&config, 0, sizeof(config));
    config.metadata = &h.metadata;
    config.cache    = &h.cache;
    config.state    = &h.state;

    jit_execution_ops_t ops;

    memset(&ops, 0, sizeof(ops));
    ops.translate = fake_translate;
    ops.interpret = fake_interpret;
    config.ops    = &ops;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_init(&uninit, &config));

    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_execution_step(NULL, &exit_code));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_execution_run(NULL, &exit_code));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_execution_step(&uninit, NULL));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_execution_run(&uninit, NULL));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_execution_describe(NULL, &resolved));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_execution_get_stats(NULL, &stats));
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == jit_execution_invalidate_range(NULL, 0U, 1U, &count));

    jit_execution_destroy(&uninit);
    jit_execution_destroy(&uninit); // must not double-free anything

    jit_cache_destroy(&h.cache);
    jit_metadata_destroy(&h.metadata);
}

POUND_TEST(execution, a_default_dispatcher_reports_its_own_shape)
{
    harness_t h;

    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, NULL, NULL, NULL, NULL));

    jit_execution_resolved_t resolved;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_describe(&h.exec, &resolved));

    POUND_CHECK(JIT_EXECUTION_CONTENTION_SPIN == resolved.contention_policy);
    POUND_CHECK(JIT_EXECUTION_DEFAULT_SPIN_BUDGET == resolved.spin_budget);
    POUND_CHECK(JIT_EXECUTION_DEFAULT_BLOCK_CODE_BYTES == resolved.block_code_bytes);
    POUND_CHECK(0U == resolved.max_blocks_per_run);
    POUND_CHECK(0U == resolved.halt_pc);
    POUND_CHECK(resolved.account);
    POUND_CHECK(resolved.counters_lock_free);
    POUND_CHECK(0U == resolved.owner_count);
    POUND_CHECK(JIT_EXECUTION_OWNER_REGISTRY_INITIAL == resolved.owner_capacity);

    POUND_CHECK(0U == jit_execution_owner_count(&h.exec));
    POUND_CHECK(!jit_execution_is_owner(&h.exec, &h.exec));

    // A configured dispatcher echoes its own configuration.
    jit_execution_t other;

    memset(&other, 0, sizeof(other));

    jit_execution_config_t config;

    memset(&config, 0, sizeof(config));
    config.metadata          = &h.metadata;
    config.cache             = &h.cache;
    config.state             = &h.state;
    config.contention_policy = JIT_EXECUTION_CONTENTION_DEFER;
    config.claim_spin_budget = 16U;
    config.block_code_bytes  = 8192U;
    config.max_blocks_per_run = 4U;
    config.halt_pc           = 0xDEAD0U;

    jit_execution_ops_t ops;

    memset(&ops, 0, sizeof(ops));
    ops.translate = fake_translate;
    config.ops    = &ops;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_init(&other, &config));

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_describe(&other, &resolved));
    POUND_CHECK(JIT_EXECUTION_CONTENTION_DEFER == resolved.contention_policy);
    POUND_CHECK(16U == resolved.spin_budget);
    POUND_CHECK(8192U == resolved.block_code_bytes);
    POUND_CHECK(4U == resolved.max_blocks_per_run);
    POUND_CHECK(0xDEAD0U == resolved.halt_pc);

    jit_execution_destroy(&other);
    harness_destroy(&h);
}

POUND_TEST(execution, a_name_exists_for_every_exit_and_for_garbage)
{
    POUND_CHECK_STR_EQ("none", jit_execution_exit_to_string(JIT_EXECUTION_EXIT_NONE));
    POUND_CHECK_STR_EQ("syscall", jit_execution_exit_to_string(JIT_EXECUTION_EXIT_SYSCALL));
    POUND_CHECK_STR_EQ("halted", jit_execution_exit_to_string(JIT_EXECUTION_EXIT_HALTED));
    POUND_CHECK_STR_EQ("stopped", jit_execution_exit_to_string(JIT_EXECUTION_EXIT_STOPPED));
    POUND_CHECK_STR_EQ("quota", jit_execution_exit_to_string(JIT_EXECUTION_EXIT_QUOTA));
    POUND_CHECK_STR_EQ("contention", jit_execution_exit_to_string(JIT_EXECUTION_EXIT_CONTENTION));
    POUND_CHECK_STR_EQ("failed", jit_execution_exit_to_string(JIT_EXECUTION_EXIT_FAILED));
    POUND_CHECK_STR_EQ("error", jit_execution_exit_to_string(JIT_EXECUTION_EXIT_ERROR));
    POUND_CHECK_STR_EQ("unrecognised", jit_execution_exit_to_string((jit_execution_exit_t)999));
}

// ---------------------------------------------------------------------------
// The hit path: dispatch and call
// ---------------------------------------------------------------------------

POUND_TEST(execution, a_pre_published_host_block_is_dispatched_and_called)
{
    harness_t h;

    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, NULL, NULL, NULL, NULL));

    void *entry = NULL;

    entry = dispatch_target_entry();

    POUND_REQUIRE(POUND_SUCCESS == prepublish(&h, 0x1000U, entry, 1U, 4U, 0U));

    h.state.pc = 0x1000U;

    exit_t exit_code = JIT_EXECUTION_EXIT_NONE;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_step(&h.exec, &exit_code));
    POUND_CHECK(JIT_EXECUTION_EXIT_NONE == exit_code);
    POUND_CHECK(1U == h.state.x[0]);
    POUND_CHECK(0x1004U == h.state.pc);

    // A second dispatch of the same address is a hit, no translation involved.
    h.state.pc = 0x1000U;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_step(&h.exec, &exit_code));
    POUND_CHECK(JIT_EXECUTION_EXIT_NONE == exit_code);
    POUND_CHECK(2U == h.state.x[0]);

    jit_execution_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_get_stats(&h.exec, &stats));
    POUND_CHECK(2U == stats.hits);
    POUND_CHECK(0U == stats.misses);
    POUND_CHECK(2U == stats.blocks_run);
    POUND_CHECK(0U == stats.publications);

    harness_destroy(&h);
}

POUND_TEST(execution, a_pre_published_host_block_ending_in_a_syscall_exits_to_the_runtime)
{
    harness_t h;

    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, NULL, NULL, NULL, NULL));

    void *entry = NULL;

    entry = dispatch_target_entry();

    POUND_REQUIRE(POUND_SUCCESS == prepublish(&h, 0x1000U, entry, 1U, 4U, JIT_METADATA_FLAG_SYSCALL));

    h.state.pc = 0x1000U;

    exit_t exit_code = JIT_EXECUTION_EXIT_NONE;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_step(&h.exec, &exit_code));
    POUND_CHECK(JIT_EXECUTION_EXIT_SYSCALL == exit_code);
    POUND_CHECK(1U == h.state.x[0]);

    jit_execution_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_get_stats(&h.exec, &stats));
    POUND_CHECK(1U == stats.syscall_exits);

    harness_destroy(&h);
}

// ---------------------------------------------------------------------------
// The miss path: translate, publish, run
// ---------------------------------------------------------------------------

POUND_TEST(execution, a_translated_host_block_is_protected_published_and_run)
{
    if (0 == HOST_BLOCK_BYTES_AVAILABLE)
    {
        return;
    }

    harness_t h;

    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, NULL, NULL, NULL, NULL));

    h.translate.host_size  = 1U;
    h.translate.guest_size = 4U;

    h.state.pc = 0x2000U;

    exit_t exit_code = JIT_EXECUTION_EXIT_ERROR;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_step(&h.exec, &exit_code));
    POUND_CHECK(JIT_EXECUTION_EXIT_NONE == exit_code);

    // The block the loop built: one `ret` that changes nothing, so the PC is
    // untouched -- matching what a translated block that does not branch does.
    POUND_CHECK(0x2000U == h.state.pc);
    POUND_CHECK(1U == h.translate.calls);
    POUND_CHECK(0x2000U == h.translate.last_guest_pc);
    POUND_CHECK(h.translate.last_code_capacity >= JIT_EXECUTION_DEFAULT_BLOCK_CODE_BYTES);

    // It was made executable and recorded in the dispatcher's ledger.
    jit_metadata_info_t info;
    size_t              index = 0U;

    POUND_REQUIRE(POUND_SUCCESS == read_block(&h, 0x2000U, &index, &info));
    POUND_CHECK_PTR_NON_NULL(info.host_code);
    POUND_CHECK(jit_cache_is_executable(&h.cache, info.host_code));
    POUND_CHECK(JIT_BLOCK_STATE_READ_EXECUTE == jit_cache_block_state(&h.cache, info.host_code));
    POUND_CHECK(jit_execution_is_owner(&h.exec, info.host_code));
    POUND_CHECK(1U == jit_execution_owner_count(&h.exec));

    jit_execution_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_get_stats(&h.exec, &stats));
    POUND_CHECK(1U == stats.misses);
    POUND_CHECK(1U == stats.claims);
    POUND_CHECK(1U == stats.publications);
    POUND_CHECK(1U == stats.blocks_run);
    POUND_CHECK(0U == stats.hits);

    // The second dispatch is a hit on the block the loop published.
    POUND_REQUIRE(POUND_SUCCESS == jit_execution_step(&h.exec, &exit_code));
    POUND_CHECK(JIT_EXECUTION_EXIT_NONE == exit_code);
    POUND_CHECK(1U == h.translate.calls);

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_get_stats(&h.exec, &stats));
    POUND_CHECK(1U == stats.hits);
    POUND_CHECK(2U == stats.blocks_run);
    POUND_CHECK(1U == stats.publications);

    harness_destroy(&h);
}

POUND_TEST(execution, an_interpreter_block_runs_through_the_interpret_hook)
{
    harness_t h;

    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, NULL, NULL, NULL, NULL));

    h.translate.host_size  = 0U; // interpreter block
    h.translate.guest_size = 8U;
    h.interpret.advance    = 8U; // move the PC over the block's guest bytes

    h.state.pc = 0x3000U;

    exit_t exit_code = JIT_EXECUTION_EXIT_ERROR;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_step(&h.exec, &exit_code));
    POUND_CHECK(JIT_EXECUTION_EXIT_NONE == exit_code);
    POUND_CHECK(0x3008U == h.state.pc);
    POUND_CHECK(1U == h.interpret.calls);
    POUND_CHECK(0x3000U == h.interpret.pcs[0]);

    // The published decision is recorded: the dispatcher runs the interpreter
    // block without asking the translator again.
    h.state.pc = 0x3000U;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_step(&h.exec, &exit_code));
    POUND_CHECK(JIT_EXECUTION_EXIT_NONE == exit_code);
    POUND_CHECK(1U == h.translate.calls);
    POUND_CHECK(2U == h.interpret.calls);

    jit_execution_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_get_stats(&h.exec, &stats));
    POUND_CHECK(1U == stats.misses);
    POUND_CHECK(1U == stats.hits);
    POUND_CHECK(2U == stats.blocks_interpreted);
    POUND_CHECK(0U == stats.blocks_run);
    POUND_CHECK(1U == stats.publications);
    POUND_CHECK(0U == stats.blocks_freed);

    harness_destroy(&h);
}

POUND_TEST(execution, an_interpreter_block_ending_in_a_syscall_exits_syscall)
{
    harness_t h;

    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, NULL, NULL, NULL, NULL));

    h.translate.flags      = JIT_METADATA_FLAG_SYSCALL;
    h.translate.guest_size = 4U;

    h.state.pc = 0x3100U;

    exit_t exit_code = JIT_EXECUTION_EXIT_NONE;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_step(&h.exec, &exit_code));
    POUND_CHECK(JIT_EXECUTION_EXIT_SYSCALL == exit_code);

    jit_execution_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_get_stats(&h.exec, &stats));
    POUND_CHECK(1U == stats.syscall_exits);
    POUND_CHECK(1U == stats.blocks_interpreted);

    harness_destroy(&h);
}

POUND_TEST(execution, an_interpreter_block_without_an_interpreter_is_refused)
{
    harness_t h;

    // The ops here have no interpret hook: set one up, then clear it after the
    // default harness has wired the callbacks.
    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, NULL, NULL, NULL, NULL));

    h.exec.interpret = NULL;

    h.state.pc = 0x3200U;

    exit_t exit_code = JIT_EXECUTION_EXIT_NONE;

    const error_t err = jit_execution_step(&h.exec, &exit_code);

    POUND_CHECK(POUND_ERROR_NOT_INITIALIZED == err);
    POUND_CHECK(JIT_EXECUTION_EXIT_FAILED == exit_code);

    jit_execution_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_get_stats(&h.exec, &stats));
    POUND_CHECK(1U == stats.dispatch_failures);
    POUND_CHECK(0U == stats.blocks_interpreted);

    harness_destroy(&h);
}

POUND_TEST(execution, a_translation_refusal_is_recorded_and_the_block_is_retried_after_invalidation)
{
    harness_t h;

    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, NULL, NULL, NULL, NULL));

    h.translate.result    = POUND_ERROR_UNSUPPORTED_INSTRUCTION;
    h.translate.refused_pc = 0x4000U;
    (void)snprintf(h.translate.detail, sizeof(h.translate.detail), "stub instruction");

    h.state.pc = 0x4000U;

    exit_t exit_code = JIT_EXECUTION_EXIT_NONE;

    const error_t err = jit_execution_step(&h.exec, &exit_code);

    POUND_CHECK(POUND_ERROR_UNSUPPORTED_INSTRUCTION == err);
    POUND_CHECK(JIT_EXECUTION_EXIT_FAILED == exit_code);

    jit_execution_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_get_stats(&h.exec, &stats));
    POUND_CHECK(1U == stats.failures);
    POUND_CHECK(1U == stats.dispatch_failures);

    // The failure is recorded with the translator's own diagnostic. find_ready
    // does not return a FAILED slot, so locate the entry the way intern does and
    // ask it why.
    error_t reason = POUND_SUCCESS;
    char    why[JIT_METADATA_LABEL_MAX];

    {
        size_t slot = 0U;

        POUND_REQUIRE(POUND_SUCCESS == jit_metadata_intern(&h.metadata, 0x4000U, &slot));

        POUND_REQUIRE(POUND_SUCCESS
                      == jit_metadata_failure_reason(&h.metadata, slot, &reason, why, sizeof(why)));
        POUND_CHECK(POUND_ERROR_UNSUPPORTED_INSTRUCTION == reason);
        POUND_CHECK_STR_EQ(why, "stub instruction");
    }

    // An invalidation un-fails it; the next dispatch translates it for real and
    // the block runs.
    {
        size_t cleared = 0U;

        POUND_REQUIRE(POUND_SUCCESS
                      == jit_execution_invalidate_range(&h.exec, 0x4000U, 0x4001U, &cleared));
        POUND_CHECK(1U == cleared);
    }

    h.translate.result    = POUND_SUCCESS;
    h.translate.refused_pc = 0U; // the refusal is over: the address is healthy again

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_step(&h.exec, &exit_code));
    POUND_CHECK(JIT_EXECUTION_EXIT_NONE == exit_code);
    POUND_CHECK(2U == h.translate.calls);

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_get_stats(&h.exec, &stats));
    POUND_CHECK(2U == stats.misses);
    POUND_CHECK(1U == stats.publications);
    POUND_CHECK(1U == stats.invalidations);

    harness_destroy(&h);
}

POUND_TEST(execution, an_untranslatable_block_is_not_translated_twice)
{
    harness_t h;

    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, NULL, NULL, NULL, NULL));

    h.translate.result = POUND_ERROR_TRANSLATION_FAILED;

    h.state.pc = 0x4100U;

    exit_t exit_code = JIT_EXECUTION_EXIT_NONE;

    POUND_CHECK(POUND_ERROR_TRANSLATION_FAILED == jit_execution_step(&h.exec, &exit_code));
    POUND_CHECK(JIT_EXECUTION_EXIT_FAILED == exit_code);

    // Dispatch again: same constant, no translation attempted -- the failure is
    // remembered until invalidation or reset.
    POUND_CHECK(POUND_ERROR_TRANSLATION_FAILED == jit_execution_step(&h.exec, &exit_code));
    POUND_CHECK(1U == h.translate.calls);

    jit_execution_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_get_stats(&h.exec, &stats));
    POUND_CHECK(2U == stats.dispatch_failures);
    POUND_CHECK(1U == stats.failures);
    POUND_CHECK(1U == stats.misses);

    harness_destroy(&h);
}

// ---------------------------------------------------------------------------
// Contention: what happens when another thread holds the claim
// ---------------------------------------------------------------------------

POUND_TEST(execution, a_claimed_slot_defers_under_the_defer_policy)
{
    harness_t h;

    jit_execution_config_t config;

    memset(&config, 0, sizeof(config));
    config.contention_policy = JIT_EXECUTION_CONTENTION_DEFER;

    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, &config, NULL, NULL, NULL));

    POUND_REQUIRE(POUND_SUCCESS == preclaim(&h, 0x6000U, NULL));

    h.state.pc = 0x6000U;

    exit_t exit_code = JIT_EXECUTION_EXIT_ERROR;

    const error_t err = jit_execution_step(&h.exec, &exit_code);

    POUND_CHECK(POUND_SUCCESS == err);
    POUND_CHECK(JIT_EXECUTION_EXIT_CONTENTION == exit_code);

    jit_execution_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_get_stats(&h.exec, &stats));
    POUND_CHECK(1U == stats.claim_conflicts);
    POUND_CHECK(1U == stats.deferrals);
    POUND_CHECK(0U == stats.blocks_run);

    harness_destroy(&h);
}

POUND_TEST(execution, a_claimed_slot_runs_interpretively_under_the_interpret_policy)
{
    harness_t h;

    jit_execution_config_t config;

    memset(&config, 0, sizeof(config));
    config.contention_policy = JIT_EXECUTION_CONTENTION_INTERPRET;

    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, &config, NULL, NULL, NULL));

    POUND_REQUIRE(POUND_SUCCESS == preclaim(&h, 0x6100U, NULL));

    h.state.pc = 0x6100U;

    exit_t exit_code = JIT_EXECUTION_EXIT_ERROR;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_step(&h.exec, &exit_code));
    POUND_CHECK(JIT_EXECUTION_EXIT_NONE == exit_code);
    POUND_CHECK(0x6104U == h.state.pc);
    POUND_CHECK(0x6100U == h.interpret.pcs[0]);

    jit_execution_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_get_stats(&h.exec, &stats));
    POUND_CHECK(1U == stats.claim_conflicts);
    POUND_CHECK(1U == stats.contention_interpreted);
    POUND_CHECK(1U == stats.blocks_interpreted);

    harness_destroy(&h);
}

POUND_TEST(execution, a_claimed_slot_spins_until_the_budget_then_gives_up)
{
    harness_t h;

    jit_execution_config_t config;

    memset(&config, 0, sizeof(config));
    config.contention_policy = JIT_EXECUTION_CONTENTION_SPIN;
    config.claim_spin_budget = 8U;

    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, &config, NULL, NULL, NULL));

    POUND_REQUIRE(POUND_SUCCESS == preclaim(&h, 0x6200U, NULL));

    h.state.pc = 0x6200U;

    exit_t exit_code = JIT_EXECUTION_EXIT_NONE;

    const error_t err = jit_execution_step(&h.exec, &exit_code);

    POUND_CHECK(POUND_ERROR_BUSY == err);
    POUND_CHECK(JIT_EXECUTION_EXIT_CONTENTION == exit_code);

    jit_execution_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_get_stats(&h.exec, &stats));
    POUND_CHECK(1U == stats.claim_conflicts);
    POUND_CHECK(1U == stats.spin_exhausted);

    harness_destroy(&h);
}

POUND_TEST(execution, a_failed_slot_the_dispatcher_asked_for_reports_why)
{
    harness_t h;

    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, NULL, NULL, NULL, NULL));

    size_t index = 0U;

    POUND_REQUIRE(POUND_SUCCESS == preclaim(&h, 0x6300U, &index));
    POUND_REQUIRE(POUND_SUCCESS
                  == jit_metadata_fail(&h.metadata, index, POUND_ERROR_TRANSLATION_FAILED,
                                       "no register allocation"));

    h.state.pc = 0x6300U;

    exit_t exit_code = JIT_EXECUTION_EXIT_NONE;

    const error_t err = jit_execution_step(&h.exec, &exit_code);

    POUND_CHECK(POUND_ERROR_TRANSLATION_FAILED == err);
    POUND_CHECK(JIT_EXECUTION_EXIT_FAILED == exit_code);

    jit_execution_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_get_stats(&h.exec, &stats));
    POUND_CHECK(1U == stats.claim_conflicts);
    POUND_CHECK(1U == stats.dispatch_failures);

    harness_destroy(&h);
}

// ---------------------------------------------------------------------------
// Stopping, halting, quota
// ---------------------------------------------------------------------------

POUND_TEST(execution, a_stop_request_is_consumed_once_and_stops_the_dispatch)
{
    harness_t h;

    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, NULL, NULL, NULL, NULL));

    void *entry = NULL;

    entry = dispatch_target_entry();

    POUND_REQUIRE(POUND_SUCCESS == prepublish(&h, 0x10000U, entry, 1U, 4U, 0U));

    h.state.pc = 0x10000U;

    jit_execution_stop(&h.exec);

    exit_t exit_code = JIT_EXECUTION_EXIT_NONE;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_step(&h.exec, &exit_code));
    POUND_CHECK(JIT_EXECUTION_EXIT_STOPPED == exit_code);
    POUND_CHECK(0U == h.state.x[0]); // the block did not run

    // The request was consumed; the next step runs.
    POUND_REQUIRE(POUND_SUCCESS == jit_execution_step(&h.exec, &exit_code));
    POUND_CHECK(JIT_EXECUTION_EXIT_NONE == exit_code);
    POUND_CHECK(1U == h.state.x[0]);

    jit_execution_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_get_stats(&h.exec, &stats));
    POUND_CHECK(1U == stats.stops);
    POUND_CHECK(1U == stats.blocks_run);

    harness_destroy(&h);
}

POUND_TEST(execution, run_consumes_a_stop_request_up_front)
{
    harness_t h;

    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, NULL, NULL, NULL, NULL));

    jit_execution_stop(&h.exec);

    exit_t exit_code = JIT_EXECUTION_EXIT_NONE;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_run(&h.exec, &exit_code));
    POUND_CHECK(JIT_EXECUTION_EXIT_STOPPED == exit_code);

    harness_destroy(&h);
}

POUND_TEST(execution, a_halted_pc_stops_without_executing_anything)
{
    harness_t h;

    jit_execution_config_t config;

    memset(&config, 0, sizeof(config));
    config.halt_pc = 0x9000U;

    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, &config, NULL, NULL, NULL));

    h.state.pc = 0x9000U;

    exit_t exit_code = JIT_EXECUTION_EXIT_ERROR;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_step(&h.exec, &exit_code));
    POUND_CHECK(JIT_EXECUTION_EXIT_HALTED == exit_code);

    jit_execution_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_get_stats(&h.exec, &stats));
    POUND_CHECK(0U == stats.misses);
    POUND_CHECK(0U == stats.blocks_run);

    harness_destroy(&h);
}

POUND_TEST(execution, run_stops_when_the_pc_reaches_the_halt_address)
{
    harness_t h;

    jit_execution_config_t config;

    memset(&config, 0, sizeof(config));
    config.halt_pc = 0xB004U;
    config.max_blocks_per_run = 0U; // unlimited: only the halt stops this run

    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, &config, NULL, NULL, NULL));

    h.translate.guest_size = 4U;

    h.state.pc = 0xB000U;

    exit_t exit_code = JIT_EXECUTION_EXIT_NONE;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_run(&h.exec, &exit_code));
    POUND_CHECK(JIT_EXECUTION_EXIT_HALTED == exit_code);
    POUND_CHECK(0xB004U == h.state.pc);
    POUND_CHECK(1U == h.interpret.calls);

    harness_destroy(&h);
}

POUND_TEST(execution, run_executes_up_to_the_configured_quota_then_returns)
{
    harness_t h;

    jit_execution_config_t config;

    memset(&config, 0, sizeof(config));
    config.max_blocks_per_run = 3U;

    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, &config, NULL, NULL, NULL));

    h.state.pc = 0xA000U;

    exit_t exit_code = JIT_EXECUTION_EXIT_NONE;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_run(&h.exec, &exit_code));
    POUND_CHECK(JIT_EXECUTION_EXIT_QUOTA == exit_code);
    POUND_CHECK(0xA00CU == h.state.pc);
    POUND_CHECK(3U == h.interpret.calls);

    jit_execution_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_get_stats(&h.exec, &stats));
    POUND_CHECK(1U == stats.quota_hits);
    POUND_CHECK(3U == stats.blocks_interpreted);

    harness_destroy(&h);
}

POUND_TEST(execution, run_propagates_a_translation_failure_as_failed)
{
    harness_t h;

    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, NULL, NULL, NULL, NULL));

    h.translate.result = POUND_ERROR_UNSUPPORTED_INSTRUCTION;

    h.state.pc = 0xA100U;

    exit_t exit_code = JIT_EXECUTION_EXIT_NONE;

    const error_t err = jit_execution_run(&h.exec, &exit_code);

    POUND_CHECK(POUND_ERROR_UNSUPPORTED_INSTRUCTION == err);
    POUND_CHECK(JIT_EXECUTION_EXIT_FAILED == exit_code);

    harness_destroy(&h);
}

// ---------------------------------------------------------------------------
// Invalidation, reset, the ownership ledger
// ---------------------------------------------------------------------------

POUND_TEST(execution, invalidation_frees_the_code_this_dispatcher_owns)
{
    if (0 == HOST_BLOCK_BYTES_AVAILABLE)
    {
        return;
    }

    harness_t h;

    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, NULL, NULL, NULL, NULL));

    h.translate.host_size  = 1U;
    h.translate.guest_size = 4U;

    h.state.pc = 0xC000U;
    {
        exit_t e = JIT_EXECUTION_EXIT_ERROR;
        POUND_REQUIRE(POUND_SUCCESS == jit_execution_step(&h.exec, &e));
    }

    h.state.pc = 0xC010U;
    {
        exit_t e = JIT_EXECUTION_EXIT_ERROR;
        POUND_REQUIRE(POUND_SUCCESS == jit_execution_step(&h.exec, &e));
    }

    POUND_CHECK(2U == jit_execution_owner_count(&h.exec));

    jit_metadata_info_t first;
    jit_metadata_info_t second;

    POUND_REQUIRE(POUND_SUCCESS == read_block(&h, 0xC000U, NULL, &first));
    POUND_REQUIRE(POUND_SUCCESS == read_block(&h, 0xC010U, NULL, &second));

    POUND_CHECK(jit_execution_is_owner(&h.exec, first.host_code));
    POUND_CHECK(jit_execution_is_owner(&h.exec, second.host_code));

    // Invalidate exactly the first block's four bytes. Half-open: a range that
    // ends where the block ends clears it, one that starts there does not.
    {
        size_t cleared = 0U;

        POUND_REQUIRE(POUND_SUCCESS
                      == jit_execution_invalidate_range(&h.exec, 0xC000U, 0xC004U, &cleared));
        POUND_CHECK(1U == cleared);
        POUND_CHECK(1U == jit_execution_owner_count(&h.exec));
        POUND_CHECK(!jit_execution_is_owner(&h.exec, first.host_code));
        POUND_CHECK(jit_execution_is_owner(&h.exec, second.host_code));

        // The freed block is no longer backed by the cache...
        POUND_CHECK(JIT_BLOCK_STATE_FREE == jit_cache_block_state(&h.cache, first.host_code));

        // The metadata table agrees the address is gone.
        size_t gone = 0U;

        POUND_CHECK(POUND_ERROR_NOT_FOUND == jit_metadata_find_ready(&h.metadata, 0xC000U, &gone));
    }

    // The surviving block is untouched and still dispatches.
    {
        size_t cleared = 0U;

        POUND_REQUIRE(POUND_SUCCESS
                      == jit_execution_invalidate_range(&h.exec, 0xC004U, 0xC010U, &cleared));
        POUND_CHECK(0U == cleared);
        POUND_CHECK(1U == jit_execution_owner_count(&h.exec));
    }

    h.state.pc = 0xC010U;
    {
        exit_t e = JIT_EXECUTION_EXIT_ERROR;
        POUND_REQUIRE(POUND_SUCCESS == jit_execution_step(&h.exec, &e));
        POUND_CHECK(JIT_EXECUTION_EXIT_NONE == e);
    }

    jit_execution_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_get_stats(&h.exec, &stats));
    POUND_CHECK(2U == stats.invalidations);
    POUND_CHECK(1U == stats.blocks_freed);

    harness_destroy(&h);
}

POUND_TEST(execution, a_range_that_ends_before_it_starts_is_refused)
{
    harness_t h;

    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, NULL, NULL, NULL, NULL));

    size_t cleared = 0U;

    const error_t err = jit_execution_invalidate_range(&h.exec, 0x40U, 0x20U, &cleared);

    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT == err);
    POUND_CHECK(0U == cleared);

    jit_execution_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_get_stats(&h.exec, &stats));
    POUND_CHECK(0U == stats.invalidations);

    harness_destroy(&h);
}

POUND_TEST(execution, an_invalidation_that_would_catch_a_leased_block_is_refused_whole)
{
    if (0 == HOST_BLOCK_BYTES_AVAILABLE)
    {
        return;
    }

    harness_t h;

    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, NULL, NULL, NULL, NULL));

    h.translate.host_size  = 1U;
    h.translate.guest_size = 4U;

    h.state.pc = 0xD000U;
    {
        exit_t e = JIT_EXECUTION_EXIT_ERROR;
        POUND_REQUIRE(POUND_SUCCESS == jit_execution_step(&h.exec, &e));
    }

    POUND_CHECK(1U == jit_execution_owner_count(&h.exec));

    // Hold a lease on the block, the way a thread inside host code would.
    size_t             index = 0U;
    jit_metadata_info_t info;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_find_ready(&h.metadata, 0xD000U, &index));
    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_acquire(&h.metadata, index, &info));

    size_t cleared = 0U;

    const error_t err = jit_execution_invalidate_range(&h.exec, 0xD000U, 0xD004U, &cleared);

    POUND_CHECK(POUND_ERROR_BUSY == err);
    POUND_CHECK(0U == cleared);

    // Nothing was cleared and nothing was freed.
    POUND_CHECK(1U == jit_execution_owner_count(&h.exec));
    POUND_CHECK(jit_execution_is_owner(&h.exec, info.host_code));

    size_t still_ready = 0U;

    POUND_CHECK(POUND_SUCCESS == jit_metadata_find_ready(&h.metadata, 0xD000U, &still_ready));

    jit_metadata_release(&h.metadata, index);

    // With the lease gone the same range invalidates cleanly.
    POUND_REQUIRE(POUND_SUCCESS == jit_execution_invalidate_range(&h.exec, 0xD000U, 0xD004U, &cleared));
    POUND_CHECK(1U == cleared);
    POUND_CHECK(0U == jit_execution_owner_count(&h.exec));

    harness_destroy(&h);
}

POUND_TEST(execution, an_invalidated_block_is_translated_again_and_reflowed_through_the_ledger)
{
    if (0 == HOST_BLOCK_BYTES_AVAILABLE)
    {
        return;
    }

    harness_t h;

    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, NULL, NULL, NULL, NULL));

    h.translate.host_size  = 1U;
    h.translate.guest_size = 4U;

    h.state.pc = 0xE000U;
    {
        exit_t e = JIT_EXECUTION_EXIT_ERROR;
        POUND_REQUIRE(POUND_SUCCESS == jit_execution_step(&h.exec, &e));
    }

    POUND_CHECK(1U == jit_execution_owner_count(&h.exec));
    POUND_CHECK(1U == h.translate.calls);

    size_t cleared = 0U;

    POUND_REQUIRE(POUND_SUCCESS
                  == jit_execution_invalidate_range(&h.exec, 0xE000U, 0xE004U, &cleared));
    POUND_CHECK(0U == jit_execution_owner_count(&h.exec));

    // The invalidated slot re-forks into a *new* code block on the next miss.
    h.state.pc = 0xE000U;
    {
        exit_t e = JIT_EXECUTION_EXIT_ERROR;
        POUND_REQUIRE(POUND_SUCCESS == jit_execution_step(&h.exec, &e));
    }

    POUND_CHECK(2U == h.translate.calls);
    POUND_CHECK(1U == jit_execution_owner_count(&h.exec));

    jit_metadata_info_t info;

    POUND_REQUIRE(POUND_SUCCESS == read_block(&h, 0xE000U, NULL, &info));
    POUND_CHECK(jit_execution_is_owner(&h.exec, info.host_code));

    harness_destroy(&h);
}

POUND_TEST(execution, a_reset_clears_every_block_and_starts_clean)
{
    if (0 == HOST_BLOCK_BYTES_AVAILABLE)
    {
        return;
    }

    harness_t h;

    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, NULL, NULL, NULL, NULL));

    // One host block and one interpreter block.
    h.translate.host_size  = 1U;
    h.translate.guest_size = 4U;

    {
        exit_t e = JIT_EXECUTION_EXIT_ERROR;
        h.state.pc = 0xF000U;
        POUND_REQUIRE(POUND_SUCCESS == jit_execution_step(&h.exec, &e));
    }

    h.translate.host_size = 0U;

    {
        exit_t e = JIT_EXECUTION_EXIT_ERROR;
        h.state.pc = 0xF100U;
        POUND_REQUIRE(POUND_SUCCESS == jit_execution_step(&h.exec, &e));
    }

    POUND_CHECK(1U == jit_execution_owner_count(&h.exec));

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_reset(&h.exec));

    POUND_CHECK(0U == jit_execution_owner_count(&h.exec));

    // The metadata table forgot the keys...
    jit_metadata_stats_t meta;

    POUND_REQUIRE(POUND_SUCCESS == jit_metadata_get_stats(&h.metadata, &meta));
    POUND_CHECK(0U == meta.interned);

    // The dispatcher's counters restarted as well.
    jit_execution_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_get_stats(&h.exec, &stats));
    POUND_CHECK(0U == stats.blocks_run);
    POUND_CHECK(0U == stats.publications);
    POUND_CHECK(0U == stats.blocks_freed);
    POUND_CHECK(0U == stats.invalidations);

    // A PC that had a block before is a miss again.
    h.state.pc = 0xF000U;
    {
        exit_t e = JIT_EXECUTION_EXIT_ERROR;
        POUND_REQUIRE(POUND_SUCCESS == jit_execution_step(&h.exec, &e));
        POUND_CHECK(JIT_EXECUTION_EXIT_NONE == e);
        // Two translations before the reset, one after it re-translates 0xF000.
        POUND_CHECK(3U == h.translate.calls);
    }

    harness_destroy(&h);
}

POUND_TEST(execution, the_ownership_registry_grows_past_its_initial_capacity)
{
    if (0 == HOST_BLOCK_BYTES_AVAILABLE)
    {
        return;
    }

    harness_t h;

    jit_metadata_config_t metadata_config;

    memset(&metadata_config, 0, sizeof(metadata_config));
    metadata_config.capacity = 256U;

    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, NULL, &metadata_config, NULL, NULL));

    h.translate.host_size  = 1U;
    h.translate.guest_size = 4U;

    const size_t blocks = JIT_EXECUTION_OWNER_REGISTRY_INITIAL + 40U;

    for (size_t i = 0U; i < blocks; ++i)
    {
        h.state.pc = 0xF0000U + (uint64_t)i * 4U;

        exit_t e = JIT_EXECUTION_EXIT_ERROR;

        POUND_REQUIRE(POUND_SUCCESS == jit_execution_step(&h.exec, &e));
        POUND_CHECK(JIT_EXECUTION_EXIT_NONE == e);
    }

    POUND_CHECK(blocks == jit_execution_owner_count(&h.exec));

    jit_execution_resolved_t resolved;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_describe(&h.exec, &resolved));
    POUND_CHECK((JIT_EXECUTION_OWNER_REGISTRY_INITIAL * 2U) == resolved.owner_capacity); // 128 doubled past 168 blocks

    jit_execution_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_get_stats(&h.exec, &stats));
    POUND_CHECK(blocks == stats.publications);
    POUND_CHECK(blocks == stats.blocks_run);

    // Every block is still individually owned and executable.
    for (size_t i = 0U; i < blocks; i += 16U)
    {
        jit_metadata_info_t info;

        if (POUND_SUCCESS == read_block(&h, 0xF0000U + (uint64_t)i * 4U, NULL, &info))
        {
            POUND_CHECK(jit_execution_is_owner(&h.exec, info.host_code));
        }
    }

    harness_destroy(&h);
}

POUND_TEST(execution, a_table_that_cannot_grow_reports_allocation_failed)
{
    harness_t h;

    jit_metadata_config_t metadata_config;

    memset(&metadata_config, 0, sizeof(metadata_config));
    metadata_config.capacity = 256U;

    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, NULL, &metadata_config, NULL, NULL));

    // Fill the table to its intern ceiling by hand.
    size_t filled = 0U;

    for (size_t i = 0U; i < 256U; ++i)
    {
        size_t index = 0U;

        if (POUND_SUCCESS == jit_metadata_intern(&h.metadata, (uint64_t)(0x10000000U + i * 4U), &index))
        {
            ++filled;
        }
        else
        {
            break;
        }
    }

    POUND_CHECK(filled > 0U);

    {
        jit_metadata_resolved_t meta;

        POUND_REQUIRE(POUND_SUCCESS == jit_metadata_describe(&h.metadata, &meta));
        POUND_CHECK(filled == meta.max_blocks);
    }

    // Dispatch to a fresh address: the table cannot intern it.
    h.state.pc = 0x55555550U;

    exit_t exit_code = JIT_EXECUTION_EXIT_NONE;

    const error_t err = jit_execution_step(&h.exec, &exit_code);

    POUND_CHECK(POUND_ERROR_ALLOCATION_FAILED == err);
    POUND_CHECK(JIT_EXECUTION_EXIT_ERROR == exit_code);

    harness_destroy(&h);
}

POUND_TEST(execution, disabled_accounting_switches_the_counters_off)
{
    harness_t h;

    jit_execution_config_t config;

    memset(&config, 0, sizeof(config));
    config.no_accounting = true;

    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, &config, NULL, NULL, NULL));

    void *entry = NULL;

    entry = dispatch_target_entry();

    POUND_REQUIRE(POUND_SUCCESS == prepublish(&h, 0x7000U, entry, 1U, 4U, 0U));

    h.state.pc = 0x7000U;

    {
        exit_t e = JIT_EXECUTION_EXIT_ERROR;
        POUND_REQUIRE(POUND_SUCCESS == jit_execution_step(&h.exec, &e));
        POUND_CHECK(JIT_EXECUTION_EXIT_NONE == e);
    }

    jit_execution_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_get_stats(&h.exec, &stats));
    POUND_CHECK(!stats.account);
    POUND_CHECK(0U == stats.hits);
    POUND_CHECK(0U == stats.blocks_run);

    jit_execution_resolved_t resolved;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_describe(&h.exec, &resolved));
    POUND_CHECK(!resolved.account);

    harness_destroy(&h);
}

POUND_TEST(execution, the_dispatch_survives_a_non_advancing_block)
{
    // A translated `ret` block leaves the PC alone, which is precisely what a
    // block that does not branch does. The dispatcher must not spin on it:
    // each call is one block.
    if (0 == HOST_BLOCK_BYTES_AVAILABLE)
    {
        return;
    }

    harness_t h;

    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, NULL, NULL, NULL, NULL));

    h.translate.host_size  = 1U;
    h.translate.guest_size = 4U;
    h.state.pc             = 0x8000U;

    for (unsigned i = 0U; i < 4U; ++i)
    {
        exit_t e = JIT_EXECUTION_EXIT_ERROR;

        POUND_REQUIRE(POUND_SUCCESS == jit_execution_step(&h.exec, &e));
        POUND_CHECK(JIT_EXECUTION_EXIT_NONE == e);
    }

    POUND_CHECK(1U == h.translate.calls); // one translation, four dispatches

    jit_execution_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_get_stats(&h.exec, &stats));
    POUND_CHECK(4U == stats.blocks_run);
    POUND_CHECK(3U == stats.hits);

    harness_destroy(&h);
}

POUND_TEST(execution, the_ownership_queries_behave_on_a_null_or_unowned_dispatcher)
{
    harness_t h;

    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, NULL, NULL, NULL, NULL));

    POUND_CHECK(0U == jit_execution_owner_count(NULL));
    POUND_CHECK(!jit_execution_is_owner(NULL, &h));
    POUND_CHECK(!jit_execution_is_owner(&h.exec, NULL));

    jit_execution_t uninit;

    memset(&uninit, 0, sizeof(uninit));

    POUND_CHECK(0U == jit_execution_owner_count(&uninit));
    POUND_CHECK(!jit_execution_is_owner(&uninit, &h));

    harness_destroy(&h);
}

POUND_TEST(execution, a_dispatcher_can_be_torn_down_and_built_again)
{
    harness_t h;

    POUND_REQUIRE(POUND_SUCCESS == harness_new(&h, NULL, NULL, NULL, NULL));

    jit_execution_destroy(&h.exec);
    jit_execution_destroy(&h.exec);

    POUND_CHECK(!h.exec.initialised);
    POUND_CHECK(0U == h.exec.owner_count);

    // A zero configuration has nothing to drive and is refused without
    // disturbing the dispatcher's clean state.
    POUND_CHECK(POUND_ERROR_INVALID_ARGUMENT
                == jit_execution_init(&h.exec, &(jit_execution_config_t){0}));

    // The rebuild needs its dependencies.
    jit_execution_config_t config;

    memset(&config, 0, sizeof(config));
    config.metadata = &h.metadata;
    config.cache    = &h.cache;
    config.state    = &h.state;

    jit_execution_ops_t ops;

    memset(&ops, 0, sizeof(ops));
    ops.translate = fake_translate;
    ops.interpret = fake_interpret;
    config.ops    = &ops;

    // A rebuild with its dependencies back init_ises cleanly: destroy leaves the
    // dispatcher in a state that can be initialised again.
    POUND_CHECK(POUND_SUCCESS == jit_execution_init(&h.exec, &config));

    jit_execution_destroy(&h.exec);

    POUND_CHECK(POUND_SUCCESS == jit_execution_init(&h.exec, &config));

    void *entry = NULL;

    entry = dispatch_target_entry();

    POUND_REQUIRE(POUND_SUCCESS == prepublish(&h, 0x1800U, entry, 1U, 4U, 0U));

    h.state.pc = 0x1800U;

    exit_t e = JIT_EXECUTION_EXIT_ERROR;

    POUND_REQUIRE(POUND_SUCCESS == jit_execution_step(&h.exec, &e));
    POUND_CHECK(JIT_EXECUTION_EXIT_NONE == e);
    POUND_CHECK(1U == h.state.x[0]);

    harness_destroy(&h);
}

POUND_TEST(execution, log_summary_and_stop_are_safe_on_null_or_uninitialised)
{
    jit_execution_log_summary(NULL);
    jit_execution_stop(NULL);

    jit_execution_t uninit;

    memset(&uninit, 0, sizeof(uninit));

    jit_execution_log_summary(&uninit);
    jit_execution_stop(&uninit);
}

POUND_TEST_SUITE(execution,
                 POUND_TEST_CASE(execution, a_dispatcher_with_no_dependencies_is_refused),
                 POUND_TEST_CASE(execution, an_uninitialised_dispatcher_refuses_every_entry_point),
                 POUND_TEST_CASE(execution, a_default_dispatcher_reports_its_own_shape),
                 POUND_TEST_CASE(execution, a_name_exists_for_every_exit_and_for_garbage),
                 POUND_TEST_CASE(execution, a_pre_published_host_block_is_dispatched_and_called),
                 POUND_TEST_CASE(execution,
                                 a_pre_published_host_block_ending_in_a_syscall_exits_to_the_runtime),
                 POUND_TEST_CASE(execution, a_translated_host_block_is_protected_published_and_run),
                 POUND_TEST_CASE(execution, an_interpreter_block_runs_through_the_interpret_hook),
                 POUND_TEST_CASE(execution, an_interpreter_block_ending_in_a_syscall_exits_syscall),
                 POUND_TEST_CASE(execution, an_interpreter_block_without_an_interpreter_is_refused),
                 POUND_TEST_CASE(execution,
                                 a_translation_refusal_is_recorded_and_the_block_is_retried_after_invalidation),
                 POUND_TEST_CASE(execution, an_untranslatable_block_is_not_translated_twice),
                 POUND_TEST_CASE(execution, a_claimed_slot_defers_under_the_defer_policy),
                 POUND_TEST_CASE(execution, a_claimed_slot_runs_interpretively_under_the_interpret_policy),
                 POUND_TEST_CASE(execution, a_claimed_slot_spins_until_the_budget_then_gives_up),
                 POUND_TEST_CASE(execution, a_failed_slot_the_dispatcher_asked_for_reports_why),
                 POUND_TEST_CASE(execution, a_stop_request_is_consumed_once_and_stops_the_dispatch),
                 POUND_TEST_CASE(execution, run_consumes_a_stop_request_up_front),
                 POUND_TEST_CASE(execution, a_halted_pc_stops_without_executing_anything),
                 POUND_TEST_CASE(execution, run_stops_when_the_pc_reaches_the_halt_address),
                 POUND_TEST_CASE(execution, run_executes_up_to_the_configured_quota_then_returns),
                 POUND_TEST_CASE(execution, run_propagates_a_translation_failure_as_failed),
                 POUND_TEST_CASE(execution, invalidation_frees_the_code_this_dispatcher_owns),
                 POUND_TEST_CASE(execution, a_range_that_ends_before_it_starts_is_refused),
                 POUND_TEST_CASE(execution,
                                 an_invalidation_that_would_catch_a_leased_block_is_refused_whole),
                 POUND_TEST_CASE(execution,
                                 an_invalidated_block_is_translated_again_and_reflowed_through_the_ledger),
                 POUND_TEST_CASE(execution, a_reset_clears_every_block_and_starts_clean),
                 POUND_TEST_CASE(execution, the_ownership_registry_grows_past_its_initial_capacity),
                 POUND_TEST_CASE(execution, a_table_that_cannot_grow_reports_allocation_failed),
                 POUND_TEST_CASE(execution, disabled_accounting_switches_the_counters_off),
                 POUND_TEST_CASE(execution, the_dispatch_survives_a_non_advancing_block),
                 POUND_TEST_CASE(execution, the_ownership_queries_behave_on_a_null_or_unowned_dispatcher),
                 POUND_TEST_CASE(execution, a_dispatcher_can_be_torn_down_and_built_again),
                 POUND_TEST_CASE(execution, log_summary_and_stop_are_safe_on_null_or_uninitialised))

/*** end of line ***/
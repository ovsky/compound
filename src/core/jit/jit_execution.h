//! The execution loop: the user of every other JIT module.
//!
//! `jit_metadata` answers "is there host code for this address, and where".
//! `jit_cache` answers "where do I put machine code" and owns its bytes. This
//! module is what actually *runs* the guest: it reads the guest PC, finds or
//! translates the block that PC starts, hands the guest register file to the
//! block, and puts the result back where the runtime can act on it.
//!
//! It is the piece that binds a translator to the two caches, and it is the
//! piece README's roadmap calls "execution loop integration".
//!
//! # The block ABI
//!
//! A published block is one host function pointer:
//!
//! ```text
//!   void (*)(guest_state_t *state)
//! ```
//!
//! The guest register file is a single object -- `guest_state_t` -- passed by
//! address, which is exactly the shape Ballistic's `bal_jit_block_t` has. That
//! is deliberate: when the ARM64 translator is bound below this module, the
//! blocks it publishes are the address of the compiled host code, and this
//! loop's `step` is the dispatcher that calls them.
//!
//! # What one step is
//!
//! `jit_execution_step` runs **one guest block**:
//!
//! 1. Consume any pending stop request.
//! 2. Read `state->pc`; if a `halt_pc` is configured and `pc == halt_pc`,
//!    stop without executing anything.
//! 3. Look the PC up in the metadata table (`jit_metadata_find_ready`). On a
//!    hit, acquire a lease, run the block, drop the lease.
//! 4. On a miss, intern the address, claim the slot for translation
//!    (`jit_metadata_intern` + `jit_metadata_try_begin`), call the configured
//!    `translate` callback into a freshly allocated cache block, make the block
//!    executable and publish it, then run it -- the guest was stopped on this
//!    address, so the block that now exists is the block that address was
//!    waiting for.
//!
//! A step therefore ends after exactly one block has executed, or after an exit
//! reason (`SYSCALL`, `HALTED`, `STOPPED`, `CONTENTION`) has been produced.
//! The exit reason travels in `jit_execution_exit_t`; hard failures travel as
//! the return value. `jit_execution_run` is `step` in a loop: it executes
//! blocks until an exit reason or the configured block quota, and returns
//! `POUND_SUCCESS` with the reason in `*out_exit`.
//!
//! # The claim is the only source of blocking
//!
//! Two dispatcher threads can reach the same cold address. `jit_metadata` calls
//! the winner's translation and reports the loser with the *previous state*, and
//! resolving that is this module's policy decision -- `jit_execution_contention_policy_t`:
//!
//! - **Spin.** Re-dispatch on the address for `claim_spin_budget` iterations.
//!   The other thread's publication makes the block `READY` and the spin wins.
//!   If the budget runs out the loop refuses to wait on a translator that may
//!   never finish, and reports `POUND_ERROR_BUSY` with exit `CONTENTION`.
//! - **Interpret.** Run one block interpretively through the `interpret` hook
//!   (the next block is dispatched fresh, by which time the claim usually
//!   publishes). When no `interpret` hook is installed this degrades to Defer
//!   with a warning.
//! - **Defer.** Return control to the caller immediately with exit `CONTENTION`,
//!   and let the runtime decide when to come back.
//!
//! # Invalidation is two steps, and this module owns the second one
//!
//! `jit_metadata_invalidate_range` moves cleared blocks to `VACANT` but does
//! **not** free their host code -- that is the documented contract that makes a
//! raced invalidation safe (see `jit_metadata.h`). The code space belongs to
//! `jit_cache`. Who calls `jit_cache_free_executable` for each cleared block?
//! In a design with one translator this loop does, which is why this module
//! keeps a small registry of every block it published: `{guest_pc,
//! guest_size, code}`. `jit_execution_invalidate_range` runs the metadata
//! invalidation first and, only when it returns `POUND_SUCCESS`, frees and
//! forgets every registered block that the same range would have cleared.
//!
//! The register map is set by the *translator*, through the metadata manager
//! exposed on this loop (`exec->metadata`) for the slot it was handed. This
//! module stores and passes it through; it is what a future fast-path dispatcher
//! will read, and the map's presence is part of what `translate` may legitimately
//! manipulate during its claim.

#ifndef POUND_JIT_EXECUTION_H
#define POUND_JIT_EXECUTION_H

#include "attributes.h"
#include "errors.h"
#include "guest_state.h"
#include "jit_cache.h"
#include "jit_metadata.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Forward declaration, so the callback and block signatures can name the
/// dispatcher before its definition below.
typedef struct jit_execution jit_execution_t;

/// Default spins tolerated on a claim held by another thread.
///
/// 4096 iterations is nanoseconds of re-dispatch on a contended slot and a
/// visible stall on one that never publishes; the budget's job is to fall into
/// the wait column quickly when it is clear the translation is not coming.
#define JIT_EXECUTION_DEFAULT_SPIN_BUDGET 4096U

/// Default bytes requested from the code cache for one translated block.
///
/// A page-sized request, because executable blocks own whole pages of the cache.
/// The buffer the translator actually gets can be larger:
/// `jit_cache_usable_size` reports the block's true capacity.
#define JIT_EXECUTION_DEFAULT_BLOCK_CODE_BYTES 4096U

/// Initial capacity of the block-ownership registry.
///
/// 128 blocks covers a cold-start burst with no realloc; a long session grows
/// the registry by doubling, which is rare and never on the hit path.
#define JIT_EXECUTION_OWNER_REGISTRY_INITIAL 128U

/// Consecutive re-dispatches tolerated while a block keeps vanishing between
/// `find_ready` and `acquire`.
///
/// In a single-dispatcher process this cannot happen at all. On a multi-core
/// title a fast invalidation loop has to be able to race the dispatch, so the
/// loop reloads rather than failing -- but reloading forever on a hostile
/// invalidation storm is also a hang, and the loop refuses after this many.
#define JIT_EXECUTION_MAX_RELOADS 4U

/// The block entry ABI.
///
/// A published block is callable with the guest register file and nothing else,
/// matching Ballistic's `bal_jit_block_t(bal_cpu_t *)`. Blocks that end in a
/// syscall set `JIT_METADATA_FLAG_SYSCALL`; the dispatcher returns after such a
/// block so the runtime can service the call.
typedef void (*jit_execution_block_entry_fn)(guest_state_t *POUND_RESTRICT state);

/// How a dispatcher that lost a claim to another thread proceeds.
typedef enum
{
    /// Re-dispatch on the contended address for up to
    /// `jit_execution_config_t::claim_spin_budget` iterations.
    JIT_EXECUTION_CONTENTION_SPIN = 0,

    /// Run one block interpretively through `interpret` and continue.
    JIT_EXECUTION_CONTENTION_INTERPRET,

    /// Return control to the caller with exit `JIT_EXECUTION_EXIT_CONTENTION`.
    JIT_EXECUTION_CONTENTION_DEFER,
} jit_execution_contention_policy_t;

/// Translates one guest block into a caller-supplied buffer.
///
/// Called on the dispatch miss path, after the loop has claimed `slot_index`
/// and allocated `code_buffer` from the code cache (writable, not yet
/// executable). `code_capacity` is the block's usable size -- the translator
/// may write up to that many bytes.
///
/// On success the translator writes `*out_host_size` bytes of machine code into
/// `code_buffer`, the number of guest bytes the block covers into
/// `*out_guest_size`, and a bitwise OR of `JIT_METADATA_FLAG_*` into
/// `*out_flags`. A `*out_host_size` of zero turns the block into an interpreter
/// block: the loop publishes it with `JIT_METADATA_FLAG_INTERPRETED` set (the
/// translator need not set that bit itself) and `*out_flags == 0` may be
/// written to mean "plain block". The block then runs through the `interpret`
/// hook rather than as host code.
///
/// While it holds the claim the translator may use `exec->metadata` to attach
/// the block's register map (`jit_metadata_set_map`) and label
/// (`jit_metadata_set_label`) for `slot_index`; both must happen before
/// publication, which is what this callback's place in the sequence guarantees.
///
/// On failure the translator returns an `error_t` and may write a NUL-terminated
/// diagnostic into `out_fail_detail` (at most `fail_detail_capacity` bytes,
/// including the terminator). The loop records the block as failed and never
/// retries it until it is invalidated or the manager is reset.
typedef error_t (*jit_execution_translate_fn)(jit_execution_t *POUND_RESTRICT exec,
                                              void *POUND_RESTRICT           context,
                                              uint64_t                       guest_pc,
                                              size_t                         slot_index,
                                              void *POUND_RESTRICT           code_buffer,
                                              size_t                         code_capacity,
                                              size_t *POUND_RESTRICT         out_host_size,
                                              uint64_t *POUND_RESTRICT       out_guest_size,
                                              uint32_t *POUND_RESTRICT       out_flags,
                                              char *POUND_RESTRICT           out_fail_detail,
                                              size_t                         fail_detail_capacity);

/// Runs one guest block interpretively and returns.
///
/// `guest_pc` is the address of the block about to run. The hook must advance
/// `state->pc` past everything it executed, exactly as host code would, leaving
/// the machine one block further along. On success it returns `POUND_SUCCESS`;
/// any other `error_t` aborts the dispatch and is reported as exit `FAILED`.
///
/// For a block whose published flags carry `JIT_METADATA_FLAG_SYSCALL`, the
/// hook must leave the machine at the same boundary the host code would (the
/// syscall visible to the runtime through `state`); the dispatcher then exits
/// with `JIT_EXECUTION_EXIT_SYSCALL` exactly as it would for a host block.
typedef error_t (*jit_execution_interpret_fn)(jit_execution_t *POUND_RESTRICT exec,
                                              void *POUND_RESTRICT           context,
                                              uint64_t                       guest_pc);

/// The translator and interpreter the dispatcher fills misses with.
typedef struct
{
    /// Opaque caller data handed to every callback.
    void *context;

    /// Translates one guest block. Required: a loop with no translator can
    /// only ever run blocks somebody else published.
    jit_execution_translate_fn translate;

    /// Runs one guest block interpretively. Optional; without it, published
    /// interpreter blocks are refused at dispatch with exit `FAILED`.
    jit_execution_interpret_fn interpret;
} jit_execution_ops_t;

/// Construction parameters for the dispatcher.
///
/// `metadata`, `cache`, `state` and `ops` are required; every other field takes
/// a default when zero, so a zero-filled struct plus the three pointers is the
/// ordinary way to build one.
typedef struct
{
    /// The metadata manager that owns the address-to-block table. Required.
    jit_metadata_t *metadata;

    /// The W^X code cache that supplies and reclaims block storage. Required.
    jit_cache_t *cache;

    /// The guest register file. Required: the loop reads `pc` from it and
    /// passes it to every block.
    guest_state_t *state;

    /// The callbacks that fill misses. Required (the struct and `translate`
    /// inside it must be non-NULL).
    const jit_execution_ops_t *ops;

    /// How to resolve a claim held by another thread. Zero is `SPIN`.
    jit_execution_contention_policy_t contention_policy;

    /// Spins allowed before a `SPIN` resolution gives up. Zero takes
    /// `JIT_EXECUTION_DEFAULT_SPIN_BUDGET`.
    uint32_t claim_spin_budget;

    /// Bytes requested from the code cache for one translated block. Zero takes
    /// `JIT_EXECUTION_DEFAULT_BLOCK_CODE_BYTES`.
    size_t block_code_bytes;

    /// Blocks `jit_execution_run` may execute before returning exit `QUOTA`.
    /// Zero, which is what a zero-filled struct holds, means unlimited.
    uint64_t max_blocks_per_run;

    /// Guest PC at which dispatch stops and reports `HALTED` without executing.
    /// Zero disables the check.
    uint64_t halt_pc;

    /// Suppress the dispatcher's own counters, mirroring
    /// `jit_metadata_config_t::no_accounting`.
    ///
    /// Negative on purpose, for the same reason as the metadata flag: every
    /// other field here treats zero as "take the default", so a positive flag
    /// would be read backwards.
    bool no_accounting;

    /// Explicit so the struct's size does not depend on `bool`'s width.
    char pad[7];
} jit_execution_config_t;

/// Why `jit_execution_step` or `jit_execution_run` handed control back.
typedef enum
{
    /// A block ran and execution may continue. `run` continues on this.
    JIT_EXECUTION_EXIT_NONE = 0,

    /// The block that ran ends in a syscall; the runtime must service it
    /// (through `state`) and then resume.
    JIT_EXECUTION_EXIT_SYSCALL,

    /// The configured `halt_pc` was reached. Nothing was executed.
    JIT_EXECUTION_EXIT_HALTED,

    /// A stop was requested. Nothing was executed.
    JIT_EXECUTION_EXIT_STOPPED,

    /// `max_blocks_per_run` blocks executed. Returned only by `run`.
    JIT_EXECUTION_EXIT_QUOTA,

    /// Another thread held the claim and the configured policy handed control
    /// back instead of continuing.
    JIT_EXECUTION_EXIT_CONTENTION,

    /// The target block is recorded as `FAILED` (untranslatable), or an
    /// interpreter block was reached with no interpreter installed.
    JIT_EXECUTION_EXIT_FAILED,

    /// The dispatcher itself failed; the `error_t` return value says how. This
    /// exit is a classification, not a reason.
    JIT_EXECUTION_EXIT_ERROR,
} jit_execution_exit_t;

/// One block this dispatcher published and therefore owns the code of.
///
/// The registry exists so that `jit_execution_invalidate_range` can honour the
/// metadata contract's "invalidate, then free through the cache, never
/// reordered" by knowing exactly which cache blocks belong to which guest
/// range -- the metadata manager only reports a *count* of cleared blocks.
typedef struct
{
    /// The guest address the block is keyed on.
    uint64_t guest_pc;

    /// Bytes of guest code the block covers. Zero for a block with no host
    /// code, which is never registered here.
    uint64_t guest_size;

    /// The executable cache block, as returned by
    /// `jit_cache_alloc_executable` and made RX by `jit_cache_protect_rx`.
    void *code;
} jit_execution_owner_t;

/// The dispatcher's resolved configuration, as reported by
/// `jit_execution_describe`.
typedef struct
{
    /// The contention policy in effect.
    jit_execution_contention_policy_t contention_policy;

    /// Spins tolerated before a `SPIN` resolution gives up, after defaults.
    uint32_t spin_budget;

    /// Bytes requested per translated block, after defaults.
    size_t block_code_bytes;

    /// Blocks `run` executes before `QUOTA`, after defaults (0 = unlimited).
    uint64_t max_blocks_per_run;

    /// PC at which dispatch halts, after defaults (0 = never).
    uint64_t halt_pc;

    /// Whether the dispatcher's counters are maintained.
    bool account;

    /// Whether the counters are lock-free on this host. Meaningless when
    /// `account` is false.
    bool counters_lock_free;

    /// Registered code blocks currently owned.
    size_t owner_count;

    /// Capacity of the ownership registry.
    size_t owner_capacity;

    /// Explicit padding.
    char pad[4];
} jit_execution_resolved_t;

/// Snapshot of dispatcher activity.
///
/// Counted exactly like the metadata manager's: relaxed atomics on the dispatch
/// path, snapshotted for the caller and therefore lossy under concurrency.
typedef struct
{
    /// Dispatches that found a ready block.
    uint64_t hits;

    /// Dispatches that had to translate.
    uint64_t misses;

    /// Misses won: claims taken.
    uint64_t claims;

    /// Misses lost: the slot was already claimed or had just been published.
    uint64_t claim_conflicts;

    /// Blocks published, host-backed or interpreted.
    uint64_t publications;

    /// Blocks executed as host code.
    uint64_t blocks_run;

    /// Blocks executed interpretively (both published interpreter blocks and
    /// `INTERPRET` contention fallbacks).
    uint64_t blocks_interpreted;

    /// Blocks marked `FAILED` (untranslatable).
    uint64_t failures;

    /// Blocks that ended in a syscall, host or interpreted.
    uint64_t syscall_exits;

    /// Stop requests consumed.
    uint64_t stops;

    /// Times `run` returned for reaching the block quota.
    uint64_t quota_hits;

    /// Claims resolved by handing control back (`DEFER`, or `INTERPRET` with no
    /// interpreter installed).
    uint64_t deferrals;

    /// Claims resolved by running one interpretive block.
    uint64_t contention_interpreted;

    /// `SPIN` resolutions that exhausted the budget and returned `BUSY`.
    uint64_t spin_exhausted;

    /// Dispatches reloaded because a block vanished between `find_ready` and
    /// `acquire`.
    uint64_t stale_acquires;

    /// Acquires refused for want of room in the lease counter.
    uint64_t lease_refusals;

    /// Dispatches abandoned with exit `FAILED`: a known-untranslatable block,
    /// a failed interpretation, or a missing interpreter.
    uint64_t dispatch_failures;

    /// Completed calls to `jit_execution_invalidate_range`.
    uint64_t invalidations;

    /// Executable blocks returned to the code cache by invalidation or reset.
    uint64_t blocks_freed;

    /// Whether counters are maintained.
    bool account;

    /// Whether the counters are lock-free on this host.
    bool counters_lock_free;

    /// Explicit padding.
    char pad[6];
} jit_execution_stats_t;

/// The dispatcher.
///
/// Defined here rather than in the translation unit so a caller can put one on
/// the stack, matching `jit_metadata` and `jit_cache`, and so the test suite
/// can exercise it without its own host allocator. The ownership registry is
/// the one heap allocation, and it grows only on the miss path.
struct jit_execution
{
    /// The metadata manager this loop dispatches through.
    jit_metadata_t *metadata;

    /// The code cache this loop allocates and frees blocks through.
    jit_cache_t *cache;

    /// The guest register file this loop drives.
    guest_state_t *state;

    /// The contention policy in effect.
    jit_execution_contention_policy_t contention_policy;

    /// Spins tolerated before a `SPIN` resolution gives up.
    uint32_t claim_spin_budget;

    /// Bytes requested from the code cache per translated block.
    size_t block_code_bytes;

    /// Blocks `run` executes before `QUOTA` (0 = unlimited).
    uint64_t max_blocks_per_run;

    /// PC at which dispatch halts (0 = never).
    uint64_t halt_pc;

    /// Opaque caller data for the callbacks.
    void *context;

    /// The translator installed at init.
    jit_execution_translate_fn translate;

    /// The interpreter installed at init, or NULL.
    jit_execution_interpret_fn interpret;

    /// The block-ownership registry: every block this dispatcher published.
    jit_execution_owner_t *owners;

    /// Live entries in `owners`.
    size_t owner_count;

    /// Allocated entries in `owners`.
    size_t owner_capacity;

    /// Counters taken on the dispatch path. Relaxed atomics, like the metadata
    /// manager's: they gate nothing, and `account` switches them off.
    atomic_uint_least64_t hits;
    atomic_uint_least64_t misses;
    atomic_uint_least64_t claims;
    atomic_uint_least64_t claim_conflicts;
    atomic_uint_least64_t publications;
    atomic_uint_least64_t blocks_run;
    atomic_uint_least64_t blocks_interpreted;
    atomic_uint_least64_t failures;
    atomic_uint_least64_t syscall_exits;
    atomic_uint_least64_t stops;
    atomic_uint_least64_t quota_hits;
    atomic_uint_least64_t deferrals;
    atomic_uint_least64_t contention_interpreted;
    atomic_uint_least64_t spin_exhausted;
    atomic_uint_least64_t stale_acquires;
    atomic_uint_least64_t lease_refusals;
    atomic_uint_least64_t dispatch_failures;
    atomic_uint_least64_t invalidations;
    atomic_uint_least64_t blocks_freed;

    /// Set by `jit_execution_stop`; consumed by `step` and `run`.
    atomic_bool stop_requested;

    /// Whether counters are maintained.
    bool account;

    /// Whether the counters are lock-free on this host.
    bool counters_lock_free;

    /// Whether `init` completed.
    bool initialised;

    /// Explicit padding so the struct's size does not depend on `bool` width.
    char pad[5];
};

/// Initialises `exec`.
///
/// `config` is required: a dispatcher with no metadata manager, cache, guest
/// state or translator has nothing to run. Tunable fields may be zero for their
/// defaults. Returns `POUND_SUCCESS`, or a typed error with a log record:
///
/// - `POUND_ERROR_INVALID_ARGUMENT` for a NULL `exec` or `config`, any missing
///   required dependency, an `ops` without `translate`, or a contention policy
///   that is not one of the defined values.
/// - `POUND_ERROR_ALREADY_INITIALIZED` when `exec` is already initialised.
///
/// A warning is logged when accounting is on and the counters would not be
/// lock-free, mirroring the metadata manager.
error_t jit_execution_init(jit_execution_t *POUND_RESTRICT exec,
                           const jit_execution_config_t *POUND_RESTRICT config);

/// Tears the dispatcher down.
///
/// Releases the ownership registry. Does **not** free registered code blocks,
/// because the code bytes belong to the cache and the cache's lifetime is the
/// caller's to order; instead a warning names the count of blocks still owned,
/// which is the caller's leak to fix before tearing the caches down. Logs and
/// ignores a NULL or uninitialised dispatcher.
void jit_execution_destroy(jit_execution_t *POUND_RESTRICT exec);

/// Writes the resolved configuration to `out`.
error_t jit_execution_describe(const jit_execution_t *POUND_RESTRICT exec,
                               jit_execution_resolved_t *POUND_RESTRICT out);

/// Runs exactly one guest block.
///
/// This is the dispatcher. It consumes a pending stop request, reads
/// `state->pc`, then dispatches: a hit runs the block, a miss interns, claims,
/// translates, publishes and then runs it. On `POUND_SUCCESS`, `*out_exit`
/// says why the step returned: `NONE` or `SYSCALL` mean a block ran,
/// `HALTED`/`STOPPED`/`CONTENTION` mean it did not and gave a reason.
///
/// Returns a typed error with a log record when the dispatch itself failed:
///
/// - `POUND_ERROR_INVALID_ARGUMENT` for a NULL `exec` or `out_exit`.
/// - `POUND_ERROR_NOT_INITIALIZED` when `exec` is not initialised, or when an
///   interpreter block was reached with no interpreter installed (exit
///   `FAILED`).
/// - `POUND_ERROR_BUSY` when a `SPIN` contention budget was exhausted (exit
///   `CONTENTION`) or a block kept vanishing mid-dispatch.
/// - `POUND_ERROR_ALLOCATION_FAILED` when the metadata table is full, the code
///   cache cannot supply a block, the lease counter is exhausted, or the
///   ownership registry cannot grow.
/// - the translator's own `error_t`, when translation failed and the block was
///   recorded as `FAILED` (exit `FAILED`).
error_t jit_execution_step(jit_execution_t *POUND_RESTRICT exec,
                           jit_execution_exit_t *POUND_RESTRICT out_exit);

/// Runs blocks until an exit reason or the quota.
///
/// Repeatedly dispatches, continuing on `NONE`, until it reaches `halt_pc`
/// (`HALTED`), a stop request is consumed (`STOPPED`), a block ends in a
/// syscall (`SYSCALL`), a contention policy defers (`CONTENTION`), a block is
/// known-untranslatable (`FAILED`), or `max_blocks_per_run` blocks have
/// executed (`QUOTA`). The smaller contract of `step` applies to every
/// iteration: hard failures return the `error_t` with `*out_exit` classifying
/// them.
error_t jit_execution_run(jit_execution_t *POUND_RESTRICT exec,
                          jit_execution_exit_t *POUND_RESTRICT out_exit);

/// Requests that the next `step`/`run` return exit `STOPPED`.
///
/// Each stop request is consumed exactly once. Safe to call from another
/// thread while one is dispatching. Logs and ignores a NULL or uninitialised
/// dispatcher.
void jit_execution_stop(jit_execution_t *POUND_RESTRICT exec);

/// Clears every block whose guest range overlaps `[begin, end)`.
///
/// Runs `jit_metadata_invalidate_range` first; only when it returns
/// `POUND_SUCCESS` are the corresponding code blocks returned to the cache and
/// forgotten -- the two halves may not be reordered, exactly as documented in
/// `jit_metadata.h`. `out_invalidated` receives the metadata count.
///
/// Returns `POUND_ERROR_BUSY` when any block the range would clear holds a
/// lease; nothing is cleared and nothing is freed in that case.
error_t jit_execution_invalidate_range(jit_execution_t *POUND_RESTRICT exec,
                                       uint64_t                             begin,
                                       uint64_t                             end,
                                       size_t *POUND_RESTRICT                out_invalidated);

/// Clears every block, drops every key and every chunk.
///
/// The full "new game / reload save state" reset: invalidates the whole
/// address space (frees every code block this dispatcher owns), then resets
/// the metadata manager and the code cache, and clears the dispatcher's
/// counters. Requires that no lease is outstanding and returns
/// `POUND_ERROR_BUSY` otherwise. A pending stop request survives the reset.
error_t jit_execution_reset(jit_execution_t *POUND_RESTRICT exec);

/// Writes a snapshot of dispatcher activity to `out`.
error_t jit_execution_get_stats(const jit_execution_t *POUND_RESTRICT exec,
                                jit_execution_stats_t *POUND_RESTRICT out);

/// Logs a one-line summary of the dispatcher's state at info level.
void jit_execution_log_summary(const jit_execution_t *POUND_RESTRICT exec);

/// Human-readable name for `exit_code`. Never returns NULL.
const char *jit_execution_exit_to_string(jit_execution_exit_t exit_code);

/// Returns the number of code blocks this dispatcher currently owns.
size_t jit_execution_owner_count(const jit_execution_t *POUND_RESTRICT exec);

/// Returns true when `code` is a block this dispatcher currently owns.
bool jit_execution_is_owner(const jit_execution_t *POUND_RESTRICT exec, const void *POUND_RESTRICT code);

#endif // POUND_JIT_EXECUTION_H

/*** end of line ***/
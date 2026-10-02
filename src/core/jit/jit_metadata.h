//! Guest-address-to-host-code metadata for the translator.
//!
//! The code cache (`jit_cache.h`) answers "where do I put this machine code". This
//! module answers the question the translator asks on every single dispatch: "I am
//! standing on guest address `pc`; is there host code for it, and if so where, how
//! big is it, and which registers does it expect to find where". A JIT is a cache
//! with a lookup table in front of it, and this is that table.
//!
//! # Why the table is fixed size and never grows
//!
//! The dispatch path must run on every guest basic block, so it must not take a
//! lock: at 100 MHz of guest code a contended mutex is not a bottleneck to be
//! optimised later, it is the thing that makes the emulation slower than the
//! interpreter. So `jit_metadata_lookup`, `jit_metadata_acquire` and
//! `jit_metadata_release` are lock-free, and they can only stay that way if the
//! table's *shape* never changes underneath a reader.
//!
//! That is why the table is sized once at init and never resized, and why a guest
//! address, once interned, is never removed from it:
//!
//! - **No resize.** Rehashing would move entries to new addresses, so a reader
//!   walking the old table would keep probing a chain the writer had already
//!   dissolved. There is no generation-safe way to do this to a table that
//!   lock-free readers walk without either blocking them or retiring a copy of
//!   every table ever allocated.
//! - **No removal.** Linear probing is only correct if every occupied slot is
//!   followed by the rest of its own probe chain until it reaches an unoccupied
//!   slot. Deleting an entry in place truncates that chain and makes every later
//!   entry behind it unreachable. The usual repair is a tombstone, but a
//!   tombstone cannot express "this guest address is interned and has no host
//!   code yet", which is the state a block is in for most of its life.
//!
//! Both problems are solved by never letting a key leave its slot.
//! `JIT_METADATA_STATE_UNOCCUPIED` means the slot has never held an address and
//! terminates a probe; every other state means it holds a key, whatever the code
//! behind that key currently is. So re-translating an invalidated block reuses its
//! own slot, and no probe chain is ever split. A guest address that is interned
//! keeps its slot until `jit_metadata_reset`; the table fills up and stays full, and
//! when it is full `jit_metadata_intern` says so and the dispatcher runs what it
//! already has.
//!
//! That is the same policy every production JIT uses: the cache stops growing and
//! keeps running, rather than growing until the process dies.
//!
//! The third slot state, `JIT_METADATA_STATE_VACANT`, is what makes "no removal"
//! true. Vacating a slot outright would split every probe chain running through it
//! and strand the entries behind the split, so invalidation moves `READY` to
//! `VACANT`: the key stays and the payload is cleared. A `VACANT` slot is therefore
//! reusable only by the address it already holds, which is the only address that
//! will ever probe to it.
//!
//! # The claim protocol
//!
//! Three threads can reach the same untranslated guest address -- the CPU thread
//! and, in a multi-core title, the other cores' dispatchers. Exactly one of them
//! may translate. The manager arbitrates with a single compare-exchange on the
//! entry's state word:
//!
//! ```text
//!   EMPTY  --CAS-->  CLAIMED  --store(rel)-->  READY
//!     ^                   |
//!     |                   +--store(rel)-->  FAILED
//!     |
//!     +--CAS(invalidate or reset)--  READY
//! ```
//!
//! `CLAIMED -> READY` publishes a payload that a lock-free reader is about to
//! read, so it is a release store, and `get`'s state load is an acquire load. A
//! reader that observes `CLAIMED` never observes the payload, so a block cannot be
//! half-visible.
//!
//! A failed claim is **not** an error the manager resolves, because the right
//! answer depends on the caller: the CPU thread may spin until the other thread
//! finishes, or fall back to the interpreter for one block, or refuse to run the
//! title at all. `jit_metadata_try_begin` reports `POUND_ERROR_ALREADY_INITIALIZED`
//! and the *previous state* in `out_previous_state`, and the dispatcher decides.
//! Building a wait primitive here would mean committing to one of those policies
//! for every caller, and picking the wrong one is worse than making the caller say.
//!
//! # Invalidation, leases and self-modifying code
//!
//! A guest that writes to its own code makes already-translated host code wrong.
//! `jit_metadata_invalidate_range` clears every block whose guest range overlaps
//! `[begin, end)` and bumps the manager's generation.
//!
//! It does **not** free the host code, and this is the load-bearing part of the
//! design. The CPU thread may be a few instructions away from jumping into a block
//! that invalidation is clearing right now. So:
//!
//! - `jit_metadata_acquire` takes a lease on an entry as it reads it, and
//!   `jit_metadata_release` drops it. The dispatcher holds a lease for exactly as
//!   long as it is inside the block's host code.
//! - `jit_metadata_invalidate_range` refuses with `POUND_ERROR_BUSY` if any entry
//!   it would clear holds a lease, and clears **nothing** in that case. Partly
//!   invalidating a range would leave the guest with a mixture of blocks
//!   translated against the old code and blocks translated against the new, which
//!   is not a state any guest is prepared for.
//! - The acquire path re-checks the state *after* taking the lease. A reader that
//!   raced an invalidation backs its lease out and reports the block gone, so a
//!   caller that sees a successful invalidate can rely on the host code being dead
//!   as of that moment even if a reader was mid-acquire.
//!
//! So the sequence for the guest's code writer is: invalidate, and if it returns
//! `POUND_SUCCESS`, release the host code through `jit_cache`. The two halves
//! cannot be reordered, because between them the entry is unreachable but its
//! memory is still mapped.
//!
//! # The register map
//!
//! A block's host code is only callable from a dispatcher that already holds the
//! guest's registers in the places that block expects. That agreement is the
//! register map: a small table of guest register to host location, written by the
//! translator before publication and read by the dispatcher after.
//!
//! The map is inline in the entry and capped at `JIT_METADATA_MAP_SLOTS`. A block
//! that needs more than that is **not** silently truncated -- the translator is
//! expected to split it. That is not a limitation worked around, it is the right
//! answer: a basic block that has to preserve an unbounded number of host
//! registers is a block that has grown past what a basic block should be, and
//! splitting it also shrinks its icache footprint.
//!
//! # Statistics are relaxed atomics, and that has a cost
//!
//! Hit and miss counters are updated on the dispatch path, so they are relaxed
//! 64-bit atomics. On a 64-bit host that is a single instruction and costs
//! nothing measurable. On a 32-bit host it may not be lock-free, and then the
//! counter becomes the most expensive thing in the dispatch loop. `jit_metadata_config_t::no_accounting`
//! turns the counters off, `jit_metadata_describe` reports whether they would have
//! been lock-free, and a warning is logged at init when they are wanted but are not
//! cheap. `jit_metadata_get_stats` reports the same two flags, so a snapshot is
//! never mistaken for measurements that were actually taken.

#ifndef POUND_JIT_METADATA_H
#define POUND_JIT_METADATA_H

#include "attributes.h"
#include "errors.h"
#include "sync/mutex.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Default number of table slots.
///
/// A power of two, because the probe index is a mask of the mixed guest address.
/// 32768 slots is 5 MiB of table at the default map size, which is a working set
/// a commercial title fills only after a long session; the alternative -- sizing
/// for the worst case up front -- charges every user, including the ones who quit
/// after the tutorial, for blocks they never reached.
#define JIT_METADATA_DEFAULT_CAPACITY 32768U

/// Smallest table the manager will accept.
///
/// 256 slots is enough to be useful in a test and small enough that a mistake in
/// the load-factor arithmetic shows up as a probe-length regression rather than as
/// an allocation failure.
#define JIT_METADATA_MIN_CAPACITY 256U

/// Fraction of the table that may be occupied at once.
///
/// Three quarters. Linear probing degrades sharply past that: the mean probe length
/// climbs into the tens and a miss walks most of the table, which would put the
/// miss path -- the *common* path early in a session -- back on the critical path.
#define JIT_METADATA_LOAD_NUMERATOR 3U

/// Fraction denominator for `JIT_METADATA_LOAD_NUMERATOR`.
#define JIT_METADATA_LOAD_DENOMINATOR 4U

/// Host registers a block's register map may describe.
///
/// Eight is the size of a preserved-register save area on the targets Pound runs on
/// (x19-x28 plus frame pointer and link register), which is the set a block that
/// honours the ABI has to account for. See the module comment for why exceeding it
/// means the block should be split rather than the cap raised.
#define JIT_METADATA_MAP_SLOTS 8U

/// Bytes of human-readable label stored per block, including the terminator.
#define JIT_METADATA_LABEL_MAX 32U

/// Bytes of block metadata.
///
/// Reported by `jit_metadata_describe` so a caller can compute the table's cost
/// without duplicating this module's layout arithmetic.
#define JIT_METADATA_ENTRY_BYTES (sizeof(void *) * 6U + 16U + JIT_METADATA_LABEL_MAX + (JIT_METADATA_MAP_SLOTS * 8U))

/// Lifecycle of one guest address.
///
/// The numeric order matters: `UNOCCUPIED` is zero so a zeroed table is a valid empty
/// table, and every other state is distinct so a wrong-state write is visible.
typedef enum
{
    /// The slot has never held a guest address. Terminates a probe chain; there is no
    /// key here, and `intern` may take it.
    JIT_METADATA_STATE_UNOCCUPIED = 0,

    /// A guest address is interned here and there is no host code for it. The state a
    /// freshly interned address is in, and the only state `try_begin` succeeds from.
    JIT_METADATA_STATE_EMPTY,

    /// A translator owns this entry and is between `try_begin` and `publish`.
    /// Other threads see the reservation and must not touch the payload.
    JIT_METADATA_STATE_CLAIMED,

    /// Host code is published and the entry may be acquired.
    JIT_METADATA_STATE_READY,

    /// Translation failed. The entry stays in this state so the dispatcher does not
    /// retry a block that has already been shown to be untranslatable.
    JIT_METADATA_STATE_FAILED,

    /// An interned address whose host code has been invalidated. Its key is kept and
    /// its payload is cleared, so the slot is reusable *only* by that same address.
    ///
    /// The state between `READY` and `UNOCCUPIED`, and it is what makes the
    /// never-shrink the table rule safe. Vacating a slot entirely -- the obvious
    /// reading of "invalidate this block" -- splits every probe chain that passes
    /// through it, and the entries behind the split become unreachable from their own
    /// home slot. Keeping the key costs 160 bytes per invalidated block and repairs
    /// the chain; clearing the slot costs a lookup that silently misses a block that
    /// is ready to run.
    JIT_METADATA_STATE_VACANT,
} jit_metadata_state_t;

/// The block ends in a call the translator could not inline and the dispatcher must
/// handle by exiting to the runtime.
#define JIT_METADATA_FLAG_SYSCALL 0x00000001U

/// The block is an indirect branch target and its address may be the target of a
/// jump from anywhere, so its code outlives the instruction that reached it.
#define JIT_METADATA_FLAG_ENTRY_POINT 0x00000002U

/// The block executes through the interpreter rather than as host code. It still
/// occupies a slot, so that the dispatcher's decision is recorded rather than
/// re-made on every entry.
#define JIT_METADATA_FLAG_INTERPRETED 0x00000004U

/// The block has not been entered since the last reset. A cold block's code is the
/// first a reclaim pass should drop.
#define JIT_METADATA_FLAG_COLD 0x00000008U

/// Where one guest register lives at the moment host code is entered.
typedef enum
{
    /// The slot is unused. Meaningless in a map returned to a caller, which is
    /// always `map_used` entries long.
    JIT_REGISTER_LOCATION_NONE = 0,

    /// In a host general-purpose register.
    JIT_REGISTER_LOCATION_HOST_GPR,

    /// In a host vector register, whole.
    JIT_REGISTER_LOCATION_HOST_VEC,

    /// In a host floating-point/SIMD register as a scalar.
    JIT_REGISTER_LOCATION_HOST_FPR,

    /// Spilled to the stack. `frame_offset` is the byte offset from the dispatch
    /// frame's base.
    JIT_REGISTER_LOCATION_STACK,
} jit_register_location_t;

/// One guest register's expected location.
///
/// Eight bytes, no padding: a map is scanned linearly and an entry that straddled
/// a cache line for the sake of a field that was never large would be a waste.
typedef struct
{
    /// Guest register number, in the guest's own numbering.
    uint16_t guest_index;

    /// Host register number, for every location except `JIT_REGISTER_LOCATION_STACK`.
    uint16_t host_index;

    /// Byte offset from the dispatch frame's base. Must be zero for every location
    /// other than `JIT_REGISTER_LOCATION_STACK`, so a slot has exactly one meaning.
    uint16_t frame_offset;

    /// A `jit_register_location_t` value.
    uint16_t location;
} jit_register_slot_t;

/// Construction parameters for the manager.
///
/// Distinct from every per-block request so that a block's fields cannot be passed
/// where the manager's were meant.
typedef struct
{
    /// Table slots, a power of two. Zero takes `JIT_METADATA_DEFAULT_CAPACITY`.
    size_t capacity;

    /// Interned addresses at which `intern` refuses. Zero takes three quarters of
    /// the capacity.
    ///
    /// Cannot exceed three quarters of the capacity, whatever the caller asks for.
    /// That is not a tuning knob: a fuller table has no unoccupied slot left to end
    /// a probe chain, and a probe that does not terminate is a hang inside the
    /// dispatch loop rather than a slow lookup.
    size_t max_blocks;

    /// Suppress the hit, miss and publication counters. Zero, which is what a
    /// zero-filled struct holds, keeps them on.
    ///
    /// Negative rather than positive on purpose. Every other field here treats zero as
    /// "take the default", so a positive `account` would have to be read the other way
    /// round from all of them -- and a caller who zero-filled the struct and set one
    /// field would silently get a manager with no statistics and no indication of it.
    /// As a "disable" flag, forgetting to set it costs nothing and setting it does
    /// exactly one thing.
    ///
    /// Set on a 32-bit host where a 64-bit relaxed atomic increment is not lock-free;
    /// see the module comment.
    bool no_accounting;

    /// Explicit so the struct's size does not depend on `bool`'s width.
    bool pad[7];
} jit_metadata_config_t;

/// The manager's resolved configuration, as reported by `jit_metadata_describe`.
typedef struct
{
    /// Table slots.
    size_t capacity;

    /// `capacity - 1`, the probe mask.
    size_t capacity_mask;

    /// Interned addresses at which `intern` refuses.
    size_t max_blocks;

    /// Bytes of one block's metadata, from `JIT_METADATA_ENTRY_BYTES`.
    size_t entry_bytes;

    /// Bytes of table, `capacity * entry_bytes`.
    size_t table_bytes;

    /// Generation the manager starts at. Never zero, so a zeroed snapshot is
    /// distinguishable from a real one.
    uint64_t generation;

    /// The compile-time register-map cap.
    size_t map_slots;

    /// Whether counters are maintained.
    bool account;

    /// Whether the counters are lock-free on this host. Meaningless when `account`
    /// is false.
    bool counters_lock_free;

    /// Explicit padding.
    char pad[6];
} jit_metadata_resolved_t;

/// A consistent copy of one block's metadata.
typedef struct
{
    /// The guest address this entry is keyed on.
    uint64_t guest_pc;

    /// Bytes of guest code the block covers. Zero until published.
    uint64_t guest_size;

    /// The manager generation this payload was published in.
    ///
    /// Compared against `jit_metadata_generation` to detect a block that has been
    /// invalidated since the caller cached its address.
    uint64_t generation;

    /// Where the host code is. Only meaningful while a lease is held, or while the
    /// dispatcher is quiescent.
    void *host_code;

    /// Bytes of host code.
    size_t host_size;

    /// Bitwise OR of `JIT_METADATA_FLAG_*`.
    uint32_t flags;

    /// A `jit_metadata_state_t` value.
    uint32_t state;

    /// Leases outstanding when the snapshot was taken.
    uint32_t leases;

    /// Entries in the register map that are in use. Zero until published.
    uint32_t map_used;

    /// Human-readable label, possibly empty.
    char label[JIT_METADATA_LABEL_MAX];
} jit_metadata_info_t;

/// Snapshot of manager activity, sampled under the manager lock.
typedef struct
{
    /// Table slots.
    size_t capacity;

    /// Interned-address ceiling.
    size_t max_blocks;

    /// Slots currently holding a key and a block: `EMPTY`, `CLAIMED`, `READY` or
    /// `FAILED`. A `VACANT` slot is not counted, because its key still holds the
    /// address but no block is behind it.
    size_t interned;

    /// Slots currently `READY`.
    size_t ready;

    /// Slots currently `CLAIMED`.
    size_t claimed;

    /// Slots currently `FAILED`.
    size_t failed;

    /// Sum of the probe lengths of every lookup since the last reset.
    size_t probe_total;

    /// Longest probe length of any single lookup since the last reset. The number to
    /// watch: it is the table's worst case, and it is what a load factor too high
    /// looks like.
    size_t probe_worst;

    /// Leases taken and not yet dropped, across every block.
    size_t leases_outstanding;

    /// Current generation.
    uint64_t generation;

    /// Lookups that found a `READY` entry.
    uint64_t hits;

    /// Lookups that did not.
    uint64_t misses;

    /// Successful claims.
    uint64_t claims;

    /// Claims refused because another thread held the entry. A dispatcher that spins
    /// on these is behaving as designed; one that grows them is not.
    uint64_t claim_conflicts;

    /// Successful publications.
    uint64_t publications;

    /// Blocks marked `FAILED`.
    uint64_t failures;

    /// Completed calls to `invalidate_range`.
    uint64_t invalidations;

    /// Blocks cleared by those calls.
    uint64_t invalidated_blocks;

    /// Register-map publications refused for exceeding `JIT_METADATA_MAP_SLOTS`.
    uint64_t map_refusals;

    /// Label writes refused for not fitting `JIT_METADATA_LABEL_MAX`.
    uint64_t label_refusals;

    /// Acquire attempts refused for want of room in the lease counter.
    uint64_t lease_refusals;

    /// Whether counters are maintained.
    bool account;

    /// Whether the counters are lock-free on this host.
    bool counters_lock_free;

    /// Explicit padding.
    char pad[6];
} jit_metadata_stats_t;

/// The manager.
///
/// Defined here rather than in the translation unit so a caller can put one on the
/// stack, which is what lets the test suite exercise the manager without a host
/// allocator of its own and keeps single ownership obvious.
struct jit_metadata
{
    /// Guards table mutation: `intern`, `reset`, and every counter update. The
    /// dispatch path deliberately does not take it.
    mutex_t lock;

    /// The table. `capacity` slots, never resized, never freed except by
    /// `destroy`.
    void *table;

    /// Bytes of table, for logging.
    size_t table_bytes;

    /// Table slots.
    size_t capacity;

    /// `capacity - 1`.
    size_t capacity_mask;

    /// Interned-address ceiling.
    size_t max_blocks;

    /// Slots currently holding a key: `EMPTY`, `CLAIMED`, `READY`, `FAILED` or
    /// `VACANT`.
    size_t interned;

    /// Current generation. Bumped by every invalidation and by every reset.
    uint64_t generation;

    /// Sum of every lookup's probe length since the last reset.
    atomic_uint_least64_t probe_total;

    /// Longest probe length of any single lookup since the last reset. The number to
    /// watch: it is the table's worst case, and it is what a load factor too high
    /// looks like.
    atomic_uint_least64_t probe_worst;

    /// Counters taken on the dispatch path.
    ///
    /// Relaxed atomics: they are read and written without `lock`, so they must be
    /// atomic. They gate nothing -- losing an increment costs a measurement, not a
    /// translation -- which is what makes `relaxed` the right order and also what
    /// makes turning them off a lossless-enough option on a 32-bit host.
    ///
    /// They are atomic rather than plain `size_t` because the dispatch path updates
    /// them from several threads at once. A plain increment there would be a data
    /// race, and a data race is undefined behaviour rather than a lost count, so
    /// "a torn value costs only a bad average" is not a trade this code may make.
    atomic_uint_least64_t hits;
    atomic_uint_least64_t misses;
    atomic_uint_least64_t claims;
    atomic_uint_least64_t claim_conflicts;
    atomic_uint_least64_t publications;
    atomic_uint_least64_t failures;
    atomic_uint_least64_t invalidations;
    atomic_uint_least64_t invalidated_blocks;
    atomic_uint_least64_t map_refusals;
    atomic_uint_least64_t label_refusals;
    atomic_uint_least64_t lease_refusals;

    /// Leases taken and not yet dropped, across every block.
    atomic_uint_least64_t leases_outstanding;

    /// Whether `lock` holds a usable mutex and `table` is mapped.
    bool initialised;

    /// Whether counters are maintained.
    bool account;

    /// Explicit padding so the struct's size does not depend on `bool` width.
    bool pad[6];
};

typedef struct jit_metadata jit_metadata_t;

/// Initialises `manager`.
///
/// `config` may be NULL, which takes every default. Returns `POUND_SUCCESS`, or a
/// typed error with a log record:
///
/// - `POUND_ERROR_INVALID_ARGUMENT` for a NULL manager, or a capacity that is not
///   a power of two, is below `JIT_METADATA_MIN_CAPACITY`, or is so large that
///   `capacity * JIT_METADATA_ENTRY_BYTES` overflows.
/// - `POUND_ERROR_ALLOCATION_FAILED` when the table could not be mapped, or when
///   `max_blocks` exceeds the capacity and so could never be reached.
///
/// Initialisation allocates the table. A 5 MiB default reservation is the price of
/// the lock-free dispatch path and is paid once, at start-up, not per block.
///
/// A warning is logged when `account` is set and the counters would not be
/// lock-free on this host; the counters are still maintained, because dropping
/// measurements the caller asked for without saying so would be worse than paying
/// for them.
error_t jit_metadata_init(jit_metadata_t *POUND_RESTRICT manager,
                          const jit_metadata_config_t *POUND_RESTRICT config);

/// Releases the table and the manager's mutex.
///
/// Logs and ignores a NULL or uninitialised manager. Does not require that every
/// lease has been dropped, but logs a warning naming the count when one remains.
void jit_metadata_destroy(jit_metadata_t *POUND_RESTRICT manager);

/// Clears every block and bumps the generation, keeping the configuration and the
/// table.
///
/// Unlike `destroy` this keeps the mutex and the table, so the manager is usable
/// immediately. This is the reset a guest gets when it changes game or reloads a
/// save state. Every slot goes to `UNOCCUPIED` -- the keys are dropped too, unlike
/// an invalidation -- because a reset is not racing anything and a table full of
/// `VACANT` slots would be a table that cannot intern a new game.
///
/// Requires that no lease is outstanding: a thread inside host code whose entry is
/// being cleared would be left executing against a table that no longer knows it
/// exists. Entries with leases are *not* cleared, they are left `READY`, and a
/// warning names the count -- so a reset during an active dispatch degrades to "the
/// blocks still running are kept", which is correct, rather than to a use-after-free.
void jit_metadata_reset(jit_metadata_t *POUND_RESTRICT manager);

/// Writes the resolved configuration to `out`.
error_t jit_metadata_describe(const jit_metadata_t *POUND_RESTRICT manager,
                              jit_metadata_resolved_t *POUND_RESTRICT out);

/// Returns the manager's current generation.
///
/// A dispatcher that caches `(index, generation)` pairs compares against this to
/// notice that its cache went stale. Cheap enough for a hot path: one relaxed load.
uint64_t jit_metadata_generation(const jit_metadata_t *POUND_RESTRICT manager);

/// Returns the index of `guest_pc` if it is interned, and `POUND_ERROR_NOT_FOUND`
/// if it is not.
///
/// Never logs: this is called on the dispatch path, and "not translated yet" is the
/// normal answer early in a session. `out_index` is written only on success.
///
/// Does not take the lock. The table's shape never changes, so a probe needs none.
///
/// Takes the manager non-const because it updates the probe-length counters through
/// them. Those counters are relaxed atomics, so the update is still lock-free; the
/// parameter is not const only because C will not let a const object's atomic
/// members be modified, and casting that away would be a promise the function
/// breaks.
error_t jit_metadata_lookup(jit_metadata_t *POUND_RESTRICT manager,
                            uint64_t                      guest_pc,
                            size_t *POUND_RESTRICT        out_index);

/// Returns the index of the `READY` block at `guest_pc`, and `POUND_ERROR_NOT_FOUND`
/// if there is none.
///
/// The dispatch hot path: one hash, one probe, one acquire load. Never logs.
error_t jit_metadata_find_ready(jit_metadata_t *POUND_RESTRICT manager,
                                uint64_t                      guest_pc,
                                size_t *POUND_RESTRICT        out_index);

/// Returns the index for `guest_pc`, interning the address if it is not already.
///
/// Takes the lock: this writes the table. Returns `POUND_SUCCESS`, or a typed error
/// with a log record:
///
/// - `POUND_ERROR_INVALID_ARGUMENT` for a NULL manager or a NULL `out_index`.
/// - `POUND_ERROR_ALLOCATION_FAILED` when `max_blocks` addresses are already
///   interned. Logged, because it means the JIT has stopped growing and the
///   dispatcher will keep running the blocks it already has.
error_t jit_metadata_intern(jit_metadata_t *POUND_RESTRICT manager,
                            uint64_t                              guest_pc,
                            size_t *POUND_RESTRICT                 out_index);

/// Claims `index` for translation, on behalf of the calling thread.
///
/// Succeeds only from `JIT_METADATA_STATE_EMPTY`, which is the state
/// `jit_metadata_intern` leaves a new address in. Returns `POUND_SUCCESS`, or
/// `POUND_ERROR_INVALID_ARGUMENT` for a NULL manager, an out-of-range index, or a
/// NULL `out_previous_state`; `POUND_ERROR_NOT_INITIALIZED` when the manager or the
/// entry is not initialised; and `POUND_ERROR_ALREADY_INITIALIZED` when the entry is
/// `CLAIMED`, `READY` or `FAILED`, in which case `out_previous_state` says which.
///
/// A refusal from `CLAIMED` is logged at debug level, not at error level: it is the
/// ordinary outcome of two dispatchers racing to the same cold block, and an error
/// record for it would bury the records that matter. A refusal from `FAILED` is
/// logged at error level, because it means the dispatcher asked for a block that
/// has already been shown to be untranslatable and did not remember.
///
/// Does not take the lock.
error_t jit_metadata_try_begin(jit_metadata_t *POUND_RESTRICT manager,
                               size_t                                  index,
                               jit_metadata_state_t *POUND_RESTRICT     out_previous_state);

/// Publishes host code into a claimed entry.
///
/// `guest_size` and `host_size` may be zero, which is legitimate for a block the
/// interpreter will run; `host_code` may be NULL only when `host_size` is zero, so
/// that a non-empty block cannot claim to have no code.
///
/// `flags` is a bitwise OR of `JIT_METADATA_FLAG_*`, and a flag that is not defined
/// is refused rather than stored, so a typo in the translator does not become an
/// invisible behaviour change.
///
/// Returns `POUND_SUCCESS`, or `POUND_ERROR_INVALID_ARGUMENT` for a NULL manager or
/// out-of-range index, `POUND_ERROR_NOT_INITIALIZED` when the manager is not
/// initialised, and `POUND_ERROR_ALREADY_INITIALIZED` when the caller does not own
/// the entry -- publishing into a block another thread is translating would hand out
/// code nobody emitted.
///
/// Does not take the lock. The caller owns the entry by contract, established by
/// `try_begin`.
error_t jit_metadata_publish(jit_metadata_t *POUND_RESTRICT manager,
                             size_t                                  index,
                             void *POUND_RESTRICT                    host_code,
                             size_t                                  host_size,
                             uint64_t                                guest_size,
                             uint32_t                                flags);

/// Records that a claimed entry cannot be translated.
///
/// `reason` is the `error_t` the translator produced and is stored so the dispatcher
/// can report why a guest address will never run rather than only that it will not.
/// A NULL `detail` stores an empty detail; a `detail` that is not NUL-terminated
/// within `JIT_METADATA_LABEL_MAX` is refused rather than truncated, because the
/// detail is a translator's own diagnostic and silently cutting it off hides the
/// part that said what went wrong.
///
/// The entry stays `FAILED` until `jit_metadata_reset` or an invalidation clears
/// it, so a block that cannot be translated is not retried on every dispatch.
///
/// Returns `POUND_SUCCESS`, `POUND_ERROR_INVALID_ARGUMENT` for a NULL manager,
/// out-of-range index or unterminated detail, and `POUND_ERROR_ALREADY_INITIALIZED`
/// when the caller does not own the entry.
error_t jit_metadata_fail(jit_metadata_t *POUND_RESTRICT manager,
                          size_t                                  index,
                          error_t                                 reason,
                          const char *POUND_RESTRICT              detail);

/// Reads a block's metadata and takes a lease on it.
///
/// Succeeds only from `JIT_METADATA_STATE_READY`. The lease must be dropped with
/// `jit_metadata_release`, and the caller must hold it for as long as it is using
/// `out_info->host_code` -- that is the whole guarantee that makes invalidation
/// safe.
///
/// `out_info->leases` is the count *before* this call's increment, so a caller can
/// see whether the block is contended.
///
/// This is the one dispatch-path entry point that uses sequentially consistent
/// ordering, because it implements the increment-then-re-check handshake that
/// invalidation depends on. It costs a barrier on the architectures that need one,
/// which is why it runs once per block *entry* rather than once per guest
/// instruction; `jit_metadata_find_ready`, which runs on every dispatch and is far
/// hotter, needs only an acquire load.
///
/// Returns `POUND_SUCCESS`, `POUND_ERROR_INVALID_ARGUMENT` for a NULL manager or
/// out-of-range index, `POUND_ERROR_ALLOCATION_FAILED` when the block already holds
/// the maximum number of leases, and `POUND_ERROR_NOT_INITIALIZED` when the block is
/// not `READY`. The latter never logs: "not ready" is the dispatch miss, and it is
/// not a failure.
///
/// Does not take the lock.
error_t jit_metadata_acquire(jit_metadata_t *POUND_RESTRICT manager,
                             size_t                                  index,
                             jit_metadata_info_t *POUND_RESTRICT     out_info);

/// Drops a lease taken by `jit_metadata_acquire` and returns the remaining count.
///
/// Returns the number of leases still outstanding on the block, or `0` for a NULL
/// manager or an out-of-range index -- neither of which is a failure, because the
/// caller has already left the host code by the time it can know. A lease count
/// that has already reached zero is refused by `jit_metadata_acquire`, so this
/// cannot underflow.
///
/// A release with no matching acquire is a dispatcher bug and is logged at error
/// level: the lease counts are what invalidation gates on, so a spurious release
/// makes those counts wrong.
///
/// Does not take the lock.
size_t jit_metadata_release(jit_metadata_t *POUND_RESTRICT manager, size_t index);

/// Returns the number of leases outstanding on `index`, or `0` for an index the
/// manager does not have.
size_t jit_metadata_leases(const jit_metadata_t *POUND_RESTRICT manager, size_t index);

/// Writes a block's metadata without taking a lease.
///
/// For diagnostics, tests and teardown only. The payload may be stale or torn if
/// another thread is invalidating concurrently, which is precisely why the lease-
/// taking `jit_metadata_acquire` exists; a caller that uses `host_code` from a
/// `peek` must be quiescent. Returns `POUND_ERROR_NOT_INITIALIZED` for a slot that
/// holds no key, or for one whose key was vacated by an invalidation.
error_t jit_metadata_peek(const jit_metadata_t *POUND_RESTRICT manager,
                          size_t                                  index,
                          jit_metadata_info_t *POUND_RESTRICT     out_info);

/// Returns why `index` is `FAILED`.
///
/// `out_detail` receives the translator's diagnostic, NUL-terminated, or an empty
/// string when there is none. Returns `POUND_SUCCESS`, or
/// `POUND_ERROR_NOT_INITIALIZED` when the block is not `FAILED`.
error_t jit_metadata_failure_reason(const jit_metadata_t *POUND_RESTRICT manager,
                                    size_t                                  index,
                                    error_t *POUND_RESTRICT                out_reason,
                                    char *POUND_RESTRICT                   out_detail,
                                    size_t                                 detail_size);

/// Clears every block whose guest range overlaps `[begin, end)`.
///
/// The range is half-open, so a block covering exactly `[begin, end)` is cleared
/// and one ending exactly at `begin` is not. A block that covers no guest bytes --
/// an interpreter block, or one whose translation failed -- is cleared when its
/// start address is inside the range, because anything decoded from the old bytes
/// is stale even though there is no host code to free. An empty range clears
/// nothing and succeeds, because "invalidate no code" is not a request.
///
/// Cleared blocks keep their slot, as `JIT_METADATA_STATE_VACANT`, so the probe
/// chains through them stay intact. They do not count against `max_blocks`
/// afterwards, but they do occupy memory, which is the price of not splitting the
/// table's chains.
///
/// This does **not** free the host code, and that is the load-bearing part of the
/// contract. On success the caller is entitled to release each cleared block's code
/// through `jit_cache_free_executable`, and the two calls may not be reordered,
/// because between them the block is unreachable but its memory is still mapped.
///
/// `end` may be `UINT64_MAX` meaning "to the end of the address space", because a
/// guest can write past the last page it mapped.
///
/// Returns `POUND_SUCCESS` with `out_invalidated` set to the number of blocks
/// cleared, or a typed error with a log record:
///
/// - `POUND_ERROR_INVALID_ARGUMENT` for a NULL manager or a NULL `out_invalidated`.
/// - `POUND_ERROR_BUSY` when any block that would be cleared holds a lease. Nothing
///   is cleared in that case.
///
/// Bumps the generation even when nothing matched, so a caller can tell "the range
/// was clean" from "the call did not run".
///
/// Takes the lock: it walks the whole table, and it must not interleave with
/// `intern`.
error_t jit_metadata_invalidate_range(jit_metadata_t *POUND_RESTRICT manager,
                                      uint64_t                             begin,
                                      uint64_t                             end,
                                      size_t *POUND_RESTRICT                out_invalidated);

/// Publishes a block's register map.
///
/// Must be called on a claimed entry, before `jit_metadata_publish`, because the
/// map is what makes the block callable and a caller must never see a `READY` block
/// whose map is still being written.
///
/// `slots` may be NULL when `count` is zero, which publishes an empty map.
///
/// Returns `POUND_SUCCESS`, or `POUND_ERROR_INVALID_ARGUMENT` for a NULL manager, a
/// NULL `slots` with a non-zero `count`, an out-of-range index, or a slot that is
/// malformed: an undefined location, a host index for a `STACK` location, or a
/// non-zero `frame_offset` for a location that is not `STACK`. Every one of those
/// would make the dispatcher read a register that was never written or spill to an
/// offset that was never reserved.
///
/// Returns `POUND_ERROR_ALLOCATION_FAILED` when `count` exceeds
/// `JIT_METADATA_MAP_SLOTS`, and logs it: the translator must split the block
/// rather than publish a map the dispatcher would misread.
///
/// Takes the lock, so a partially written map can never be observed.
error_t jit_metadata_set_map(jit_metadata_t *POUND_RESTRICT manager,
                             size_t                                       index,
                             const jit_register_slot_t *POUND_RESTRICT    slots,
                             size_t                                       slot_count);

/// Writes the register map of `index` to `out`.
///
/// `out_count` receives the number of slots. Returns `POUND_SUCCESS`,
/// `POUND_ERROR_INVALID_ARGUMENT` for a NULL manager, a NULL `out_count` or a NULL
/// `out` with a non-zero `*out_count`, and `POUND_ERROR_NOT_INITIALIZED` when the
/// block has no map.
error_t jit_metadata_get_map(const jit_metadata_t *POUND_RESTRICT manager,
                             size_t                                  index,
                             jit_register_slot_t *POUND_RESTRICT      out,
                             size_t                                 *POUND_RESTRICT out_count);

/// Sets a block's label.
///
/// A `label` that is not NUL-terminated within `JIT_METADATA_LABEL_MAX` is refused
/// rather than truncated, and logged. A label is read by a human reading a log; a
/// silently shortened one produces a line that looks complete and is not.
///
/// Takes the lock.
error_t jit_metadata_set_label(jit_metadata_t *POUND_RESTRICT manager,
                               size_t                                  index,
                               const char *POUND_RESTRICT              label);

/// Writes a snapshot of manager activity to `out`.
///
/// Takes the lock, so the snapshot is self-consistent: the state counts cannot
/// describe a moment that never existed.
error_t jit_metadata_get_stats(jit_metadata_t *POUND_RESTRICT manager, jit_metadata_stats_t *POUND_RESTRICT out);

/// Logs a one-line summary of the manager's state at info level.
///
/// Names counts and never any host code address, because a log file is routinely
/// attached to a bug report and a code address is only meaningful with the build it
/// came from.
void jit_metadata_log_summary(jit_metadata_t *POUND_RESTRICT manager);

/// Human-readable name for `state`. Never returns NULL.
const char *jit_metadata_state_to_string(jit_metadata_state_t state);

#endif // POUND_JIT_METADATA_H

/*** end of line ***/

#include "jit_metadata.h"

#include "log.h"
#include "memory/memory.h"
#include <string.h>

/// Alignment of the table.
///
/// One cache line, so two entries never share one. A probe walks adjacent slots
/// together, and a shared line turns each probe into two cache misses on exactly
/// the path -- the lookup miss -- that is most common early in a session.
#define JIT_METADATA_TABLE_ALIGNMENT 64U

/// Highest lease count one entry will hold.
///
/// Far enough below the point where the increment would wrap that reaching it means
/// the dispatcher is not releasing leases, rather than that it is busy. The acquire
/// path uses a compare-exchange loop so a refused lease never corrupts the count;
/// this is the value the loop refuses at.
#define JIT_METADATA_LEASE_CEILING 0xFFFFFFFFULL

/// One guest address's metadata.
///
/// Exactly `JIT_METADATA_ENTRY_BYTES`, checked by the static assertion below. Every
/// field after `state` is written only by the thread that owns the entry, between
/// `try_begin` and `publish`, and read only after an acquire load has observed
/// `READY` -- so none of them needs to be atomic, and making them atomic would cost
/// the dispatch path a fence per field for nothing.
typedef struct
{
    /// The key. Meaningful once `state` is not `UNUSED`, and never cleared
    /// afterwards: keeping it is what keeps linear probing correct.
    uint64_t guest_pc;

    /// Bytes of guest code covered. Zero until published.
    uint64_t guest_size;

    /// The manager generation this payload was published in.
    uint64_t generation;

    /// Bytes of host code at `host_code`.
    size_t host_size;

    /// The block's machine code.
    void *POUND_RESTRICT host_code;

    /// Threads that are inside this block's host code right now.
    atomic_uint_least64_t leases;

    /// A `jit_metadata_state_t` value. The only field written concurrently.
    atomic_uint_least32_t state;

    /// Bitwise OR of `JIT_METADATA_FLAG_*`.
    uint32_t flags;

    /// Entries of `map` in use.
    uint32_t map_used;

    /// For a `FAILED` entry, the `error_t` the translator produced.
    uint32_t failed_with;

    /// For a `READY` entry, the block's label. For a `FAILED` entry, the
    /// translator's diagnostic. A block is only ever one or the other, so the two
    /// share a buffer rather than the entry paying for 64 bytes it never uses.
    char text[JIT_METADATA_LABEL_MAX];

    /// The block's register map, `map_used` entries in use.
    jit_register_slot_t map[JIT_METADATA_MAP_SLOTS];
} jit_metadata_entry_t;

_Static_assert(sizeof(jit_metadata_entry_t) == JIT_METADATA_ENTRY_BYTES,
               "JIT_METADATA_ENTRY_BYTES no longer describes the entry; jit_metadata_describe "
               "would report a table size the manager does not use.");

/// Returns true when `value` is a non-zero power of two.
static bool
is_power_of_two(const size_t value)
{
    return (0U != value) && (0U == (value & (value - 1U)));
}

/// MurmurHash3's 64-bit finalizer (Austin Appleby, public domain).
///
/// The standard avalanche step for a table indexed by an address. Guest addresses
/// arrive already aligned and clustered -- a translated region is thousands of
/// values a few hundred bytes apart -- so the low bits alone would put every block
/// of a hot function in one probe chain.
static uint64_t
mix64(uint64_t value)
{
    value ^= (value >> 33);
    value *= UINT64_C(0xFF51AFD7ED558CCD);
    value ^= (value >> 33);
    value *= UINT64_C(0xC4CEB9FE1A85EC53);
    value ^= (value >> 33);

    return value;
}

/// Returns `[a_begin, a_begin + a_size)` overlapping `[b_begin, b_begin + b_size)`.
///
/// Both ranges must be non-empty; `entry_in_range` handles the empty case
/// separately, because "the block's bytes overlap the written bytes" and "the written
/// bytes start inside the block's address" are different questions for a block that
/// covers no bytes.
///
/// A range whose end would wrap is reported as *not* overlapping. That is the
/// conservative direction: refusing to claim an overlap leaves code mapped that a
/// caller may then not free, which costs memory, whereas claiming one would free
/// memory something is still executing. An end that wraps also means the guest
/// address and size in the table are already nonsense, which is a corruption report
/// elsewhere rather than something to reason about here.
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

/// Returns the slot holding `guest_pc`, or the first `UNOCCUPIED` slot where it would
/// go, and writes the probe length to `out_probes` when that is not NULL.
///
/// Terminates at `UNOCCUPIED`, and that is the only state it terminates at. Linear
/// probing is correct only if every occupied slot is followed by the rest of its own
/// chain, so a key may never leave its slot. Invalidation therefore leaves a
/// `VACANT` marker holding the key rather than clearing the slot, and `intern` of a
/// vacated address reuses that slot instead of extending a chain past it.
///
/// A `VACANT` slot's key does not match the address being probed unless it *is* that
/// address, which is the point: a `VACANT` slot is invisible to every probe but its
/// own, so it terminates other chains' probes correctly without being adopted by
/// them.
///
/// Returns NULL if the probe does not terminate, which the three-quarter load factor
/// makes unreachable; hitting it means the table has been corrupted by something this
/// module cannot see, and the right answer is a typed error and a log record rather
/// than a spin inside an emulator's dispatch loop.
static jit_metadata_entry_t *
probe(jit_metadata_t *POUND_RESTRICT manager, const uint64_t guest_pc, size_t *POUND_RESTRICT out_probes)
{
    jit_metadata_entry_t *POUND_RESTRICT table = (jit_metadata_entry_t *)manager->table;

    size_t index = (size_t)(mix64(guest_pc) & (uint64_t)manager->capacity_mask);

    for (size_t step = 1U; step <= manager->capacity; ++step)
    {
        jit_metadata_entry_t *POUND_RESTRICT entry = &table[index];

        const uint32_t state = atomic_load_explicit(&entry->state, memory_order_acquire);

        if ((JIT_METADATA_STATE_UNOCCUPIED == state) || (entry->guest_pc == guest_pc))
        {
            if (NULL != out_probes)
            {
                *out_probes = step;
            }

            return entry;
        }

        index = (index + 1U) & manager->capacity_mask;
    }

    return NULL;
}

/// Returns the slot for `index`, or NULL when the manager or the index is bad.
///
/// Every index-taking entry point goes through here, so an out-of-range index is one
/// check rather than one per function, and none of them can forget it.
static jit_metadata_entry_t *
entry_at(jit_metadata_t *POUND_RESTRICT manager, const size_t index)
{
    if ((NULL == manager) || !manager->initialised || (index >= manager->capacity))
    {
        return NULL;
    }

    jit_metadata_entry_t *POUND_RESTRICT table = (jit_metadata_entry_t *)manager->table;

    return &table[index];
}

/// Records one lookup's probe length.
///
/// Gated on `account` like every other counter. It is the one that matters most to get
/// right: this runs on the dispatch path, so a caller who switched accounting off in
/// order to stop paying for diagnostics would otherwise still pay a read-modify-write
/// on the hottest path in the module, and the snapshot would report zeros that were
/// never measured.
static void
account_probe(jit_metadata_t *POUND_RESTRICT manager, const size_t probes)
{
    if (!manager->account)
    {
        return;
    }

    (void)atomic_fetch_add_explicit(&manager->probe_total, (uint_least64_t)probes, memory_order_relaxed);

    const uint_least64_t worst = atomic_load_explicit(&manager->probe_worst, memory_order_relaxed);

    if ((uint_least64_t)probes > worst)
    {
        // Not a compare-exchange loop: the counter is a diagnostic, and two threads
        // racing to record the same longest probe both write the same value.
        atomic_store_explicit(&manager->probe_worst, (uint_least64_t)probes, memory_order_relaxed);
    }
}

/// Adds `amount` to a counter, or does nothing when accounting is off.
///
/// `count` is this with an amount of one. The two are separate because most
/// counters count *events* and a call site that counts events reads better with a
/// name that says so, while a counter that accumulates a quantity -- blocks cleared,
/// not invalidations performed -- must be given the quantity or it reports a
/// different number from the one its own documentation promises.
///
/// Neither parameter is `restrict`: callers pass a field *of* the manager, so two
/// restrict-qualified pointers would be required to describe disjoint objects and
/// the call would be undefined behaviour at every call site.
static void
count_by(jit_metadata_t *manager, atomic_uint_least64_t *field, const uint_least64_t amount)
{
    if (manager->account)
    {
        (void)atomic_fetch_add_explicit(field, amount, memory_order_relaxed);
    }
}

/// Increments a counter, or does nothing when accounting is off.
static void
count(jit_metadata_t *manager, atomic_uint_least64_t *field)
{
    count_by(manager, field, (uint_least64_t)1);
}

/// Takes a lease on `entry`, or reports that it could not.
///
/// A compare-exchange loop rather than `fetch_add`, so that refusing at the ceiling
/// cannot leave the count one past it.
static bool
take_lease(jit_metadata_entry_t *POUND_RESTRICT entry)
{
    uint_least64_t current = atomic_load_explicit(&entry->leases, memory_order_relaxed);

    for (;;)
    {
        if (current >= JIT_METADATA_LEASE_CEILING)
        {
            return false;
        }

        if (atomic_compare_exchange_weak_explicit(&entry->leases,
                                                  &current,
                                                  current + (uint_least64_t)1,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire))
        {
            return true;
        }

        // A failed exchange refreshes `current` with what is actually there.
    }
}

/// Copies `entry`'s payload into `out`.
///
/// Called only once an acquire load has observed `READY`, so every field read here
/// was published before the state store the caller observed.
static void
fill_info(const jit_metadata_entry_t *POUND_RESTRICT entry, jit_metadata_info_t *POUND_RESTRICT out)
{
    memset(out, 0, sizeof(*out));

    out->guest_pc = entry->guest_pc;
    out->guest_size = entry->guest_size;
    out->generation = entry->generation;
    out->host_code = entry->host_code;
    out->host_size = entry->host_size;
    out->flags = entry->flags;
    out->map_used = entry->map_used;

    // Sampled relaxed: it is a diagnostic, and the caller is told it is the count
    // from before its own acquire.
    out->state = atomic_load_explicit(&entry->state, memory_order_relaxed);
    out->leases = (uint32_t)atomic_load_explicit(&entry->leases, memory_order_relaxed);

    memcpy(out->label, entry->text, sizeof(out->label));
    out->label[sizeof(out->label) - 1U] = '\0';
}

/// Returns true when `entry`'s guest bytes fall inside the half-open `[begin, end)`.
///
/// `end` is exclusive and `UINT64_MAX` is permitted, because a guest can write past
/// the last page it mapped and the range that covers it still has to be reachable.
static bool
entry_in_range(const jit_metadata_entry_t *POUND_RESTRICT entry, const uint64_t begin, const uint64_t end)
{
    const uint32_t state = atomic_load_explicit(&entry->state, memory_order_relaxed);

    if ((JIT_METADATA_STATE_UNOCCUPIED == state) || (JIT_METADATA_STATE_VACANT == state))
    {
        return false;
    }

    if (0U == entry->guest_size)
    {
        // A block that covers no guest bytes -- an interpreter block, or an entry
        // whose translation failed -- still has to be dropped when the guest writes
        // at its address, because anything the interpreter decoded from the old
        // bytes is stale even though there is no host code to free.
        return (begin <= entry->guest_pc) && (entry->guest_pc < end);
    }

    return ranges_overlap(entry->guest_pc, entry->guest_size, begin, end - begin);
}

/// Whether every bit of `flag` is a flag this module defines.
static bool
flags_are_defined(const uint32_t flags)
{
    const uint32_t defined = JIT_METADATA_FLAG_SYSCALL | JIT_METADATA_FLAG_ENTRY_POINT |
                             JIT_METADATA_FLAG_INTERPRETED | JIT_METADATA_FLAG_COLD;

    return (0U == (flags & ~defined));
}

/// Whether `slot` describes a location the dispatcher could act on.
static bool
slot_is_valid(const jit_register_slot_t *POUND_RESTRICT slot)
{
    if (slot->location > (uint16_t)JIT_REGISTER_LOCATION_STACK)
    {
        return false;
    }

    if (JIT_REGISTER_LOCATION_STACK == (jit_register_location_t)slot->location)
    {
        // The host index is meaningless for a spilled register, and leaving a stale
        // one in place would let a caller read it and believe it.
        return 0U == slot->host_index;
    }

    // A frame offset on a register that is not spilled is the dangerous direction:
    // the dispatcher would treat the host index as the register and the frame offset
    // as noise, or the reverse, and one of the two is always wrong.
    return 0U == slot->frame_offset;
}

/// Returns `POUND_SUCCESS` when `index` names a slot this manager has.
///
/// The two failure modes get different codes on purpose, because they are different
/// mistakes: a manager that is not up is a lifecycle error the caller fixes by
/// initialising, and an index past the end is a caller that computed a slot wrong.
/// Collapsing them into one code would leave the caller guessing which.
///
/// A subtlety this function exists to get right: an uninitialised manager has
/// `capacity == 0`, so "is this index in range" cannot be the test on its own --
/// index 0 would test as in range for a table that has no slots at all. Testing
/// `initialised` first is what makes index 0 of an uninitialised manager an error
/// rather than a NULL dereference.
static error_t
check_index(const jit_metadata_t *POUND_RESTRICT manager, const size_t index)
{
    if (NULL == manager)
    {
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!manager->initialised)
    {
        return POUND_ERROR_NOT_INITIALIZED;
    }

    return (index < manager->capacity) ? POUND_SUCCESS : POUND_ERROR_INVALID_ARGUMENT;
}

/// Writes a NULL-terminated copy of `entry->text` into `out`, up to `out_size`.
static void
copy_text(const char *POUND_RESTRICT text, char *POUND_RESTRICT out, const size_t out_size)
{
    if ((NULL == out) || (0U == out_size))
    {
        return;
    }

    const size_t limit = (out_size - 1U < JIT_METADATA_LABEL_MAX) ? (out_size - 1U) : JIT_METADATA_LABEL_MAX;

    memcpy(out, text, limit);

    out[limit] = '\0';
}

/// Returns `manager`'s bounded length of `text`, or `JIT_METADATA_LABEL_MAX` when
/// `text` is not terminated within it.
///
/// Bounded by hand rather than with `strnlen`, which is POSIX 2008 and not in C11's
/// `<string.h>`, so on a strict-conformance target it is a declaration error rather
/// than a link error.
static size_t
bounded_length(const char *POUND_RESTRICT text, const size_t limit)
{
    size_t length = 0U;

    while ((length < limit) && ('\0' != text[length]))
    {
        ++length;
    }

    return length;
}

/// True when the counters would be lock-free on this host.
static bool
counters_are_lock_free(void)
{
    atomic_uint_least64_t probe_counter;

    atomic_init(&probe_counter, (uint_least64_t)0);

    return atomic_is_lock_free(&probe_counter);
}

/// Resets an entry to the state a freshly interned address has, keeping its key.
static void
clear_payload(jit_metadata_entry_t *POUND_RESTRICT entry)
{
    entry->guest_size = 0U;
    entry->generation = 0U;
    entry->host_code = NULL;
    entry->host_size = 0U;
    entry->flags = 0U;
    entry->map_used = 0U;
    entry->failed_with = (uint32_t)POUND_SUCCESS;
    entry->text[0] = '\0';

    for (size_t slot = 0U; slot < JIT_METADATA_MAP_SLOTS; ++slot)
    {
        entry->map[slot].guest_index = 0U;
        entry->map[slot].host_index = 0U;
        entry->map[slot].frame_offset = 0U;
        entry->map[slot].location = (uint16_t)JIT_REGISTER_LOCATION_NONE;
    }
}

/// Clears every counter.
static void
clear_counters(jit_metadata_t *POUND_RESTRICT manager)
{
    atomic_store_explicit(&manager->hits, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&manager->misses, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&manager->claims, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&manager->claim_conflicts, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&manager->publications, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&manager->failures, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&manager->invalidations, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&manager->invalidated_blocks, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&manager->map_refusals, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&manager->label_refusals, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&manager->lease_refusals, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&manager->leases_outstanding, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&manager->probe_total, (uint_least64_t)0, memory_order_relaxed);
    atomic_store_explicit(&manager->probe_worst, (uint_least64_t)0, memory_order_relaxed);
}

error_t
jit_metadata_init(jit_metadata_t *POUND_RESTRICT manager, const jit_metadata_config_t *POUND_RESTRICT config)
{
    if (NULL == manager)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the metadata manager is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    // Zeroed first, so a rejected `init` leaves a manager whose every entry point
    // reports "not initialised" rather than one whose `table` is whatever the
    // caller's stack happened to hold.
    memset(manager, 0, sizeof(*manager));

    size_t capacity = JIT_METADATA_DEFAULT_CAPACITY;
    size_t max_blocks = 0U;
    bool account = true;

    if (NULL != config)
    {
        capacity = config->capacity;
        max_blocks = config->max_blocks;
        account = !config->no_accounting;
    }

    if (0U == capacity)
    {
        capacity = JIT_METADATA_DEFAULT_CAPACITY;
    }

    if (capacity < JIT_METADATA_MIN_CAPACITY)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "A metadata table of %zu slots is below the minimum of %u.",
                        capacity,
                        JIT_METADATA_MIN_CAPACITY);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!is_power_of_two(capacity))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "A metadata table of %zu slots is not a power of two, so a probe index "
                        "cannot be masked out of it.",
                        capacity);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (capacity > (SIZE_MAX / sizeof(jit_metadata_entry_t)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "A metadata table of %zu slots of %zu bytes each cannot be addressed.",
                        capacity,
                        sizeof(jit_metadata_entry_t));
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    // The ceiling is not a tunable. It is what guarantees a quarter of the table is
    // never occupied, which is what guarantees a probe terminates. A caller that
    // could raise `max_blocks` to the capacity could turn a lookup into a hang.
    const size_t ceiling = (capacity * JIT_METADATA_LOAD_NUMERATOR) / JIT_METADATA_LOAD_DENOMINATOR;

    if (0U == max_blocks)
    {
        max_blocks = ceiling;
    }

    if (max_blocks > ceiling)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "A ceiling of %zu interned addresses is above the %zu that %zu slots "
                        "allow, which is %u/%u of them: a fuller table has no unoccupied slot "
                        "left to end a probe chain.",
                        max_blocks,
                        ceiling,
                        capacity,
                        JIT_METADATA_LOAD_NUMERATOR,
                        JIT_METADATA_LOAD_DENOMINATOR);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const size_t table_bytes = capacity * sizeof(jit_metadata_entry_t);

    void *POUND_RESTRICT table = memory_subsystem_allocate(JIT_METADATA_TABLE_ALIGNMENT, table_bytes);

    if (NULL == table)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Could not allocate the %zu-byte metadata table for %zu slots.",
                        table_bytes,
                        capacity);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    const error_t status = mutex_init(&manager->lock);

    if (POUND_SUCCESS != status)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Could not initialise the metadata manager's mutex (%s); the %zu-byte "
                        "table is released and nothing was set up.",
                        pound_error_to_string(status),
                        table_bytes);

        memory_subsystem_free(table);

        return status;
    }

    jit_metadata_entry_t *POUND_RESTRICT entries = (jit_metadata_entry_t *)table;

    for (size_t index = 0U; index < capacity; ++index)
    {
        atomic_init(&entries[index].state, (uint_least32_t)JIT_METADATA_STATE_UNOCCUPIED);
        atomic_init(&entries[index].leases, (uint_least64_t)0);
    }

    manager->table = table;
    manager->table_bytes = table_bytes;
    manager->capacity = capacity;
    manager->capacity_mask = capacity - 1U;
    manager->max_blocks = max_blocks;
    manager->interned = 0U;

    // Never zero, so a zeroed snapshot is distinguishable from a real one.
    manager->generation = 1U;
    manager->account = account;
    manager->initialised = true;

    if (account && !counters_are_lock_free())
    {
        POUND_LOG_WARN(&thread_logger,
                       "The metadata manager's 64-bit counters are not lock-free on this host, "
                       "so every dispatch pays for a locked increment. They are still maintained "
                       "because they were asked for; set "
                       "jit_metadata_config_t::no_accounting to stop paying for them.");
    }

    POUND_LOG_INFO(&thread_logger,
                   "Metadata manager ready: %zu slots, %zu addresses, %zu bytes per block, "
                   "%zu bytes of table, generation %llu, %zu register-map slots per block.",
                   capacity,
                   max_blocks,
                   sizeof(jit_metadata_entry_t),
                   table_bytes,
                   (unsigned long long)manager->generation,
                   JIT_METADATA_MAP_SLOTS);

    return POUND_SUCCESS;
}

void
jit_metadata_destroy(jit_metadata_t *POUND_RESTRICT manager)
{
    if ((NULL == manager) || !manager->initialised)
    {
        return;
    }

    const uint_least64_t leases = atomic_load_explicit(&manager->leases_outstanding, memory_order_relaxed);

    if (0U != leases)
    {
        POUND_LOG_WARN(&thread_logger,
                       "Releasing the metadata manager while %llu block lease(s) are outstanding. "
                       "The blocks are unreachable afterwards, so whatever held those leases has "
                       "already lost its table.",
                       (unsigned long long)leases);
    }

    memory_subsystem_free(manager->table);

    mutex_destroy(&manager->lock);

    manager->table = NULL;
    manager->table_bytes = 0U;
    manager->capacity = 0U;
    manager->capacity_mask = 0U;
    manager->max_blocks = 0U;
    manager->interned = 0U;
    manager->generation = 0U;
    manager->account = false;
    manager->initialised = false;
}

void
jit_metadata_reset(jit_metadata_t *POUND_RESTRICT manager)
{
    if ((NULL == manager) || !manager->initialised)
    {
        POUND_LOG_WARN(&thread_logger,
                       "Ignoring a metadata manager reset: the manager is %s.",
                       (NULL == manager) ? "NULL" : "not initialised.");
        return;
    }

    size_t kept = 0U;

    // The live entries that survive the walk. Collected rather than assumed to be
    // zero: a reset that kept leased blocks has to keep counting them, or the ceiling
    // it enforces afterwards stops including them and the table can be keyed past
    // `max_blocks` -- which is past the load factor the probe's termination depends on.
    // A `CLAIMED` entry does *not* survive: a claim owns no resource, only an intent to
    // publish, and that intent is against a game that reset has just unloaded. The
    // translator that holds it finds its publication refused and discards its output,
    // which is the correct outcome.
    size_t live = 0U;

    jit_metadata_entry_t *POUND_RESTRICT entries = (jit_metadata_entry_t *)manager->table;

    // The lock is held for the whole walk so `intern` cannot hand out a slot in the
    // middle of it. It does not exclude the lock-free `acquire`, and cannot: a thread
    // inside host code does not take the manager's lock. That is safe because of the
    // re-check in `jit_metadata_acquire` -- an acquirer that raced this reset sees
    // `UNOCCUPIED` afterwards, backs its lease out, and never touches the payload.
    mutex_lock(&manager->lock);

    for (size_t index = 0U; index < manager->capacity; ++index)
    {
        jit_metadata_entry_t *POUND_RESTRICT entry = &entries[index];

        const uint32_t state = atomic_load_explicit(&entry->state, memory_order_relaxed);

        if ((JIT_METADATA_STATE_UNOCCUPIED == state) || (JIT_METADATA_STATE_VACANT == state))
        {
            continue;
        }

        if (0U != atomic_load_explicit(&entry->leases, memory_order_relaxed))
        {
            // Kept rather than cleared, because clearing it would make the payload
            // unreachable while a thread is inside it. The block stays callable, so
            // the guest keeps working with the old code -- which is wrong only if the
            // guest just rewrote it, and noticing that is the caller's own SMC path.
            ++kept;
            ++live;
            continue;
        }

        uint32_t expected = state;

        (void)atomic_compare_exchange_strong_explicit(&entry->state,
                                                      &expected,
                                                      (uint_least32_t)JIT_METADATA_STATE_UNOCCUPIED,
                                                      memory_order_acq_rel,
                                                      memory_order_relaxed);

        clear_payload(entry);
    }

    manager->interned = live;
    manager->generation = manager->generation + 1U;

    clear_counters(manager);

    mutex_unlock(&manager->lock);

    if (0U != kept)
    {
        POUND_LOG_WARN(&thread_logger,
                       "Metadata reset kept %zu block(s) that a thread was still executing. They "
                       "stay callable; a guest that rewrote their addresses has to invalidate "
                       "them again once the dispatch loop is idle.",
                       kept);
    }

    POUND_LOG_DEBUG(&thread_logger,
                    "Metadata manager reset; generation is now %llu.",
                    (unsigned long long)manager->generation);
}

error_t
jit_metadata_describe(const jit_metadata_t *POUND_RESTRICT manager, jit_metadata_resolved_t *POUND_RESTRICT out)
{
    if (NULL == manager)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the metadata manager is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (NULL == out)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the description destination is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!manager->initialised)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the metadata manager is not initialised.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    memset(out, 0, sizeof(*out));

    out->capacity = manager->capacity;
    out->capacity_mask = manager->capacity_mask;
    out->max_blocks = manager->max_blocks;
    out->entry_bytes = sizeof(jit_metadata_entry_t);
    out->table_bytes = manager->table_bytes;
    out->generation = manager->generation;
    out->map_slots = JIT_METADATA_MAP_SLOTS;
    out->account = manager->account;
    out->counters_lock_free = counters_are_lock_free();

    return POUND_SUCCESS;
}

uint64_t
jit_metadata_generation(const jit_metadata_t *POUND_RESTRICT manager)
{
    if ((NULL == manager) || !manager->initialised)
    {
        return 0U;
    }

    return manager->generation;
}

error_t
jit_metadata_lookup(jit_metadata_t *POUND_RESTRICT manager, const uint64_t guest_pc, size_t *POUND_RESTRICT out_index)
{
    if (NULL == manager)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the metadata manager is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (NULL == out_index)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the index destination for guest address 0x%llx is NULL.",
                        (unsigned long long)guest_pc);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!manager->initialised)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the metadata manager is not initialised.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    size_t probes = 0U;

    jit_metadata_entry_t *POUND_RESTRICT entry = probe(manager, guest_pc, &probes);

    if (NULL == entry)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "The metadata table's probe for guest address 0x%llx did not terminate in "
                        "%zu slots, so the table has been corrupted by something outside this "
                        "module.",
                        (unsigned long long)guest_pc,
                        manager->capacity);
        return POUND_ERROR_CORRUPTED;
    }

    account_probe(manager, probes);

    if (JIT_METADATA_STATE_UNOCCUPIED == atomic_load_explicit(&entry->state, memory_order_relaxed))
    {
        // A miss, not a failure: the address has never been translated. No log record,
        // because this is the dispatch path and every block is a miss exactly once.
        return POUND_ERROR_NOT_FOUND;
    }

    *out_index = (size_t)(entry - (jit_metadata_entry_t *)manager->table);

    return POUND_SUCCESS;
}

error_t
jit_metadata_find_ready(jit_metadata_t *POUND_RESTRICT manager, const uint64_t guest_pc, size_t *POUND_RESTRICT out_index)
{
    if (NULL == manager)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the metadata manager is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (NULL == out_index)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the index destination for guest address 0x%llx is NULL.",
                        (unsigned long long)guest_pc);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!manager->initialised)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the metadata manager is not initialised.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    size_t probes = 0U;

    jit_metadata_entry_t *POUND_RESTRICT entry = probe(manager, guest_pc, &probes);

    if (NULL == entry)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "The metadata table's probe for guest address 0x%llx did not terminate in "
                        "%zu slots, so the table has been corrupted by something outside this "
                        "module.",
                        (unsigned long long)guest_pc,
                        manager->capacity);
        return POUND_ERROR_CORRUPTED;
    }

    account_probe(manager, probes);

    if (JIT_METADATA_STATE_READY != atomic_load_explicit(&entry->state, memory_order_acquire))
    {
        count(manager, &manager->misses);

        return POUND_ERROR_NOT_FOUND;
    }

    *out_index = (size_t)(entry - (jit_metadata_entry_t *)manager->table);

    count(manager, &manager->hits);

    return POUND_SUCCESS;
}

error_t
jit_metadata_intern(jit_metadata_t *POUND_RESTRICT manager, const uint64_t guest_pc, size_t *POUND_RESTRICT out_index)
{
    if (NULL == manager)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the metadata manager is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (NULL == out_index)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the index destination for guest address 0x%llx is NULL.",
                        (unsigned long long)guest_pc);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!manager->initialised)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the metadata manager is not initialised.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    mutex_lock(&manager->lock);

    size_t probes = 0U;

    jit_metadata_entry_t *POUND_RESTRICT entry = probe(manager, guest_pc, &probes);

    if (NULL == entry)
    {
        mutex_unlock(&manager->lock);

        POUND_LOG_ERROR(&thread_logger,
                        "The metadata table's probe for guest address 0x%llx did not terminate in "
                        "%zu slots, so the table has been corrupted by something outside this "
                        "module.",
                        (unsigned long long)guest_pc,
                        manager->capacity);

        return POUND_ERROR_CORRUPTED;
    }

    account_probe(manager, probes);

    const uint32_t state = atomic_load_explicit(&entry->state, memory_order_relaxed);

    if ((JIT_METADATA_STATE_UNOCCUPIED != state) && (JIT_METADATA_STATE_VACANT != state))
    {
        // Already interned and still holding its slot. This is the path an
        // invalidated block takes on re-translation, which is why invalidation
        // leaves a `VACANT` marker rather than an unoccupied slot.
        *out_index = (size_t)(entry - (jit_metadata_entry_t *)manager->table);

        mutex_unlock(&manager->lock);

        return POUND_SUCCESS;
    }

    if (JIT_METADATA_STATE_VACANT == state)
    {
        // Reclaiming a vacated slot. Its key is still the one we are looking up, so the
        // chain it sits in stays intact and no *new* key is added.
        //
        // The count does move, though, and the ceiling check is deliberately not
        // repeated: `interned` counts live entries, and after `clear_payload` this is a
        // live entry again. The invariant that makes skipping the check safe is that the
        // number of keyed slots never exceeds `max_blocks` -- a new key is only ever
        // added while `interned < max_blocks`, and reclaiming one adds a key to nothing.
        // So `interned` cannot pass `max_blocks` here.
        //
        // Leaving it alone instead is not a "harmless off-by-one": the next
        // invalidation of this block decrements a count that never included it, and for
        // a `size_t` that is not a wrong number but a wrapped one. The symptom is a
        // table that reports itself full with four entries in it.
        clear_payload(entry);

        ++manager->interned;
    }
    else
    {
        if (manager->interned >= manager->max_blocks)
        {
            mutex_unlock(&manager->lock);

            POUND_LOG_ERROR(&thread_logger,
                            "The metadata table is full: all %zu of its addresses are interned, so "
                            "guest address 0x%llx has nowhere to go. Existing blocks keep running; "
                            "no new address will be translated until a reset.",
                            manager->max_blocks,
                            (unsigned long long)guest_pc);

            return POUND_ERROR_ALLOCATION_FAILED;
        }

        ++manager->interned;
    }

    // The key first, then a release store of the state. A reader that observes the
    // state through its acquire load is guaranteed to see the key.
    entry->guest_pc = guest_pc;
    entry->generation = manager->generation;

    atomic_store_explicit(&entry->state, (uint_least32_t)JIT_METADATA_STATE_EMPTY, memory_order_release);

    *out_index = (size_t)(entry - (jit_metadata_entry_t *)manager->table);

    const size_t interned = manager->interned;
    const size_t ceiling = manager->max_blocks;

    mutex_unlock(&manager->lock);

    POUND_LOG_DEBUG(&thread_logger,
                    "Interned guest address 0x%llx at slot %zu after %zu probe(s); %zu of %zu "
                    "addresses are now in use.",
                    (unsigned long long)guest_pc,
                    *out_index,
                    probes,
                    interned,
                    ceiling);

    return POUND_SUCCESS;
}

error_t
jit_metadata_try_begin(jit_metadata_t *POUND_RESTRICT manager,
                       const size_t                       index,
                       jit_metadata_state_t *POUND_RESTRICT out_previous_state)
{
    if (NULL == manager)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the metadata manager is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (NULL == out_previous_state)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the state destination for slot %zu is NULL.",
                        index);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const error_t unavailable = check_index(manager, index);

    if (POUND_SUCCESS != unavailable)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Slot %zu cannot be used: %s. The manager has %zu slot(s).",
                        index,
                        pound_error_to_string(unavailable),
                        (NULL == manager) ? 0U : manager->capacity);
        return unavailable;
    }

    jit_metadata_entry_t *POUND_RESTRICT entry = entry_at(manager, index);

    uint32_t expected = (uint32_t)JIT_METADATA_STATE_EMPTY;

    *out_previous_state = JIT_METADATA_STATE_EMPTY;

    if (atomic_compare_exchange_strong_explicit(&entry->state,
                                                &expected,
                                                (uint_least32_t)JIT_METADATA_STATE_CLAIMED,
                                                memory_order_acq_rel,
                                                memory_order_acquire))
    {
        count(manager, &manager->claims);

        return POUND_SUCCESS;
    }

    const jit_metadata_state_t previous = (jit_metadata_state_t)expected;

    *out_previous_state = previous;

    count(manager, &manager->claim_conflicts);

    if (JIT_METADATA_STATE_CLAIMED == previous)
    {
        // Two dispatchers reached the same cold block. The ordinary outcome of a
        // multi-core title, so it is not an error: the losing dispatcher spins,
        // interprets, or gives up, and all three are its decision to make.
        POUND_LOG_DEBUG(&thread_logger,
                        "Guest address 0x%llx is already being translated into slot %zu; the "
                        "claim is refused.",
                        (unsigned long long)entry->guest_pc,
                        index);
    }
    else if (JIT_METADATA_STATE_FAILED == previous)
    {
        // The dispatcher asked for a block already recorded as untranslatable, which
        // means the failure was not remembered. A bug in the dispatcher, and an error
        // record is the right level for it.
        POUND_LOG_ERROR(&thread_logger,
                        "Guest address 0x%llx in slot %zu was already recorded as untranslatable "
                        "(%s: %s), so the claim is refused. The dispatcher asked for a block it "
                        "should have recognised as failed.",
                        (unsigned long long)entry->guest_pc,
                        index,
                        pound_error_to_string((error_t)entry->failed_with),
                        entry->text);
    }
    else
    {
        POUND_LOG_DEBUG(&thread_logger,
                        "Guest address 0x%llx in slot %zu is %s, so there is nothing to "
                        "translate; the claim is refused.",
                        (unsigned long long)entry->guest_pc,
                        index,
                        jit_metadata_state_to_string(previous));
    }

    return POUND_ERROR_ALREADY_INITIALIZED;
}

error_t
jit_metadata_publish(jit_metadata_t *POUND_RESTRICT manager,
                     const size_t       index,
                     void *POUND_RESTRICT host_code,
                     const size_t       host_size,
                     const uint64_t     guest_size,
                     const uint32_t     flags)
{
    if (NULL == manager)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the metadata manager is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const error_t unavailable = check_index(manager, index);

    if (POUND_SUCCESS != unavailable)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Slot %zu cannot be used: %s. The manager has %zu slot(s).",
                        index,
                        pound_error_to_string(unavailable),
                        (NULL == manager) ? 0U : manager->capacity);
        return unavailable;
    }

    if ((NULL == host_code) && (0U != host_size))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Slot %zu is to be published with %zu bytes of host code at no address.",
                        index,
                        host_size);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!flags_are_defined(flags))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Slot %zu is to be published with flags 0x%08x, which include bits this "
                        "module does not define. Publishing them would store a behaviour change "
                        "nobody can read.",
                        index,
                        flags);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    jit_metadata_entry_t *POUND_RESTRICT entry = entry_at(manager, index);

    const uint32_t state = atomic_load_explicit(&entry->state, memory_order_acquire);

    if (JIT_METADATA_STATE_CLAIMED != state)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Slot %zu is %s, not %s, so it was not claimed for publication. Publishing "
                        "into a block another thread is translating would hand out code nobody "
                        "emitted.",
                        index,
                        jit_metadata_state_to_string((jit_metadata_state_t)state),
                        jit_metadata_state_to_string(JIT_METADATA_STATE_CLAIMED));

        return POUND_ERROR_ALREADY_INITIALIZED;
    }

    entry->host_code = host_code;
    entry->host_size = host_size;
    entry->guest_size = guest_size;
    entry->flags = flags;
    entry->generation = manager->generation;

    // The release store is what makes the payload above visible to every reader that
    // observes `READY` with an acquire load. Without it a dispatcher could execute
    // host code whose size and flags it had not yet seen.
    atomic_store_explicit(&entry->state, (uint_least32_t)JIT_METADATA_STATE_READY, memory_order_release);

    count(manager, &manager->publications);

    POUND_LOG_DEBUG(&thread_logger,
                    "Published %zu byte(s) of host code for guest address 0x%llx covering %llu "
                    "guest byte(s) in slot %zu, flags 0x%08x.",
                    host_size,
                    (unsigned long long)entry->guest_pc,
                    (unsigned long long)guest_size,
                    index,
                    flags);

    return POUND_SUCCESS;
}

error_t
jit_metadata_fail(jit_metadata_t *POUND_RESTRICT manager,
                  const size_t       index,
                  const error_t      reason,
                  const char *POUND_RESTRICT detail)
{
    if (NULL == manager)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the metadata manager is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const error_t unavailable = check_index(manager, index);

    if (POUND_SUCCESS != unavailable)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Slot %zu cannot be used: %s. The manager has %zu slot(s).",
                        index,
                        pound_error_to_string(unavailable),
                        (NULL == manager) ? 0U : manager->capacity);
        return unavailable;
    }

    if (NULL != detail)
    {
        if (bounded_length(detail, JIT_METADATA_LABEL_MAX) >= JIT_METADATA_LABEL_MAX)
        {
            POUND_LOG_ERROR(&thread_logger,
                            "The translator's diagnostic for slot %zu is not NUL-terminated within "
                            "%u bytes. It is refused rather than cut short, because the part that "
                            "said what went wrong is the part that gets cut.",
                            index,
                            JIT_METADATA_LABEL_MAX);

            return POUND_ERROR_INVALID_ARGUMENT;
        }
    }

    jit_metadata_entry_t *POUND_RESTRICT entry = entry_at(manager, index);

    const uint32_t state = atomic_load_explicit(&entry->state, memory_order_acquire);

    if (JIT_METADATA_STATE_CLAIMED != state)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Slot %zu is %s, not %s, so it was not claimed. A failure recorded against "
                        "a block nobody was translating would silence a translation that is about "
                        "to succeed.",
                        index,
                        jit_metadata_state_to_string((jit_metadata_state_t)state),
                        jit_metadata_state_to_string(JIT_METADATA_STATE_CLAIMED));

        return POUND_ERROR_ALREADY_INITIALIZED;
    }

    entry->failed_with = (uint32_t)reason;

    if (NULL == detail)
    {
        entry->text[0] = '\0';
    }
    else
    {
        copy_text(detail, entry->text, sizeof(entry->text));
    }

    atomic_store_explicit(&entry->state, (uint_least32_t)JIT_METADATA_STATE_FAILED, memory_order_release);

    count(manager, &manager->failures);

    POUND_LOG_ERROR(&thread_logger,
                    "Guest address 0x%llx in slot %zu cannot be translated (%s): %s. It stays "
                    "untranslatable until the manager is reset or the address is invalidated.",
                    (unsigned long long)entry->guest_pc,
                    index,
                    pound_error_to_string(reason),
                    (NULL == detail) ? "no further detail" : entry->text);

    return POUND_SUCCESS;
}

error_t
jit_metadata_acquire(jit_metadata_t *POUND_RESTRICT manager,
                     const size_t       index,
                     jit_metadata_info_t *POUND_RESTRICT out_info)
{
    if (NULL == manager)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the metadata manager is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (NULL == out_info)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the block snapshot destination for slot %zu is NULL.",
                        index);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const error_t unavailable = check_index(manager, index);

    if (POUND_SUCCESS != unavailable)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Slot %zu cannot be used: %s. The manager has %zu slot(s).",
                        index,
                        pound_error_to_string(unavailable),
                        (NULL == manager) ? 0U : manager->capacity);
        return unavailable;
    }

    jit_metadata_entry_t *POUND_RESTRICT entry = entry_at(manager, index);

    // Sequentially consistent, and deliberately so. This is the standard
    // increment-then-re-check handshake: the first state load must not be reordered
    // past the lease increment, and the second must not be reordered before it, or
    // invalidation could observe zero leases, free the host code, and have this
    // thread go on to execute it. Acquire/release alone does not order two plain
    // loads around a read-modify-write on every architecture Pound targets.
    //
    // It is also why the cost is acceptable: this runs once per block entry, not once
    // per guest instruction, and `jit_metadata_find_ready` -- which runs on every
    // dispatch and is far hotter -- needs only an acquire load.
    if (JIT_METADATA_STATE_READY != atomic_load_explicit(&entry->state, memory_order_seq_cst))
    {
        return POUND_ERROR_NOT_INITIALIZED;
    }

    if (!take_lease(entry))
    {
        count(manager, &manager->lease_refusals);

        POUND_LOG_ERROR(&thread_logger,
                        "Slot %zu already holds the maximum of %llu lease(s). A dispatcher that "
                        "reaches this has not been releasing them, so the counts cannot be "
                        "trusted and no block can be invalidated safely.",
                        index,
                        (unsigned long long)JIT_METADATA_LEASE_CEILING);

        return POUND_ERROR_ALLOCATION_FAILED;
    }

    if (JIT_METADATA_STATE_READY != atomic_load_explicit(&entry->state, memory_order_seq_cst))
    {
        // Raced an invalidation, which saw no lease because the increment had not
        // landed yet. Backing the lease out is what makes that safe: the caller never
        // sees the payload, so it never uses host code the invalidation was entitled
        // to free.
        (void)atomic_fetch_sub_explicit(&entry->leases, (uint_least64_t)1, memory_order_relaxed);

        return POUND_ERROR_NOT_INITIALIZED;
    }

    // Sampled after the increment above has published, so `leases` is the count from
    // before this call's own increment and a caller can see contention.
    fill_info(entry, out_info);

    (void)atomic_fetch_add_explicit(&manager->leases_outstanding, (uint_least64_t)1, memory_order_relaxed);

    return POUND_SUCCESS;
}

size_t
jit_metadata_release(jit_metadata_t *POUND_RESTRICT manager, const size_t index)
{
    if (POUND_SUCCESS != check_index(manager, index))
    {
        // Already out of the host code by the time this can be reached, so there is
        // nothing left to report. A warning here would fire on the error path of a
        // block that was never entered.
        return 0U;
    }

    jit_metadata_entry_t *POUND_RESTRICT entry = entry_at(manager, index);

    const uint_least64_t current = atomic_load_explicit(&entry->leases, memory_order_relaxed);

    if (0U == current)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Slot %zu was released with no lease held. A release without a matching "
                        "acquire means a dispatcher is tracking blocks it never entered, and the "
                        "lease counts invalidation gates on are already wrong.",
                        index);

        return 0U;
    }

    (void)atomic_fetch_sub_explicit(&entry->leases, (uint_least64_t)1, memory_order_relaxed);

    const uint_least64_t manager_current = atomic_load_explicit(&manager->leases_outstanding,
                                                               memory_order_relaxed);

    if (0U != manager_current)
    {
        (void)atomic_fetch_sub_explicit(&manager->leases_outstanding, (uint_least64_t)1, memory_order_relaxed);
    }

    return (size_t)atomic_load_explicit(&entry->leases, memory_order_relaxed);
}

size_t
jit_metadata_leases(const jit_metadata_t *POUND_RESTRICT manager, const size_t index)
{
    if ((NULL == manager) || !manager->initialised || (index >= manager->capacity))
    {
        return 0U;
    }

    const jit_metadata_entry_t *POUND_RESTRICT table = (const jit_metadata_entry_t *)manager->table;

    return (size_t)atomic_load_explicit(&table[index].leases, memory_order_relaxed);
}

error_t
jit_metadata_peek(const jit_metadata_t *POUND_RESTRICT manager,
                  const size_t       index,
                  jit_metadata_info_t *POUND_RESTRICT out_info)
{
    if (NULL == manager)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the metadata manager is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (NULL == out_info)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the block snapshot destination for slot %zu is NULL.",
                        index);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const error_t unavailable = check_index(manager, index);

    if (POUND_SUCCESS != unavailable)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Slot %zu cannot be used: %s. The manager has %zu slot(s).",
                        index,
                        pound_error_to_string(unavailable),
                        (NULL == manager) ? 0U : manager->capacity);
        return unavailable;
    }

    const jit_metadata_entry_t *POUND_RESTRICT table = (const jit_metadata_entry_t *)manager->table;
    const jit_metadata_entry_t *POUND_RESTRICT entry = &table[index];

    const uint32_t state = atomic_load_explicit(&entry->state, memory_order_relaxed);

    if ((JIT_METADATA_STATE_UNOCCUPIED == state) || (JIT_METADATA_STATE_VACANT == state))
    {
        // The two states are reported differently because they mean different things to
        // whoever is reading the log: one address was never seen, the other was
        // translated and has since been invalidated.
        POUND_LOG_ERROR(&thread_logger,
                        "Slot %zu is %s, so there is no block metadata to read.",
                        index,
                        jit_metadata_state_to_string((jit_metadata_state_t)state));
        return POUND_ERROR_NOT_INITIALIZED;
    }

    fill_info(entry, out_info);

    return POUND_SUCCESS;
}

error_t
jit_metadata_failure_reason(const jit_metadata_t *POUND_RESTRICT manager,
                            const size_t                   index,
                            error_t *POUND_RESTRICT        out_reason,
                            char *POUND_RESTRICT           out_detail,
                            const size_t                   detail_size)
{
    if (NULL == manager)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the metadata manager is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (NULL == out_reason)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the reason destination for slot %zu is NULL.",
                        index);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if ((NULL == out_detail) && (0U != detail_size))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the detail destination for slot %zu is NULL but its "
                        "size is %zu.",
                        index,
                        detail_size);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const error_t unavailable = check_index(manager, index);

    if (POUND_SUCCESS != unavailable)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Slot %zu cannot be used: %s. The manager has %zu slot(s).",
                        index,
                        pound_error_to_string(unavailable),
                        (NULL == manager) ? 0U : manager->capacity);
        return unavailable;
    }

    const jit_metadata_entry_t *POUND_RESTRICT table = (const jit_metadata_entry_t *)manager->table;
    const jit_metadata_entry_t *POUND_RESTRICT entry = &table[index];

    const uint32_t state = atomic_load_explicit(&entry->state, memory_order_relaxed);

    if (JIT_METADATA_STATE_FAILED != state)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Slot %zu is %s, so it has no recorded translation failure.",
                        index,
                        jit_metadata_state_to_string((jit_metadata_state_t)state));
        return POUND_ERROR_NOT_INITIALIZED;
    }

    *out_reason = (error_t)entry->failed_with;

    copy_text(entry->text, out_detail, detail_size);

    return POUND_SUCCESS;
}

error_t
jit_metadata_invalidate_range(jit_metadata_t *POUND_RESTRICT manager,
                              const uint64_t       begin,
                              const uint64_t       end,
                              size_t *POUND_RESTRICT out_invalidated)
{
    if (NULL == manager)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the metadata manager is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (NULL == out_invalidated)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the cleared-block destination for range "
                        "[0x%llx, 0x%llx) is NULL.",
                        (unsigned long long)begin,
                        (unsigned long long)end);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!manager->initialised)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the metadata manager is not initialised.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    if (end < begin)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "The invalidation range [0x%llx, 0x%llx) ends before it starts.",
                        (unsigned long long)begin,
                        (unsigned long long)end);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    *out_invalidated = 0U;

    if (begin == end)
    {
        // "Invalidate no code" is not a request. Not an error either: a guest that
        // writes zero bytes has still executed a store, and the caller should not have
        // to special-case it.
        count(manager, &manager->invalidations);

        return POUND_SUCCESS;
    }

    jit_metadata_entry_t *POUND_RESTRICT entries = (jit_metadata_entry_t *)manager->table;

    mutex_lock(&manager->lock);

    // Two passes, and the first one only reads. Clearing an entry a thread is inside
    // would make its host code unreachable while it is running, so the decision to
    // clear has to be made before anything changes. Partly clearing is not an option
    // either: a guest running a mixture of blocks translated against the old code and
    // blocks translated against the new is in a state no title is prepared for.
    size_t matches = 0U;

    for (size_t index = 0U; index < manager->capacity; ++index)
    {
        const jit_metadata_entry_t *POUND_RESTRICT entry = &entries[index];

        if (!entry_in_range(entry, begin, end))
        {
            continue;
        }

        ++matches;

        const uint_least64_t leases = atomic_load_explicit(&entry->leases, memory_order_relaxed);

        if (0U != leases)
        {
            POUND_LOG_ERROR(&thread_logger,
                            "Guest address 0x%llx in slot %zu overlaps [0x%llx, 0x%llx) and has "
                            "%llu lease(s) outstanding, so nothing is invalidated. Freeing its "
                            "host code now would pull it out from under a thread executing it.",
                            (unsigned long long)entry->guest_pc,
                            index,
                            (unsigned long long)begin,
                            (unsigned long long)end,
                            (unsigned long long)leases);

            mutex_unlock(&manager->lock);

            return POUND_ERROR_BUSY;
        }
    }

    size_t cleared = 0U;

    for (size_t index = 0U; index < manager->capacity; ++index)
    {
        jit_metadata_entry_t *POUND_RESTRICT entry = &entries[index];

        if (!entry_in_range(entry, begin, end))
        {
            continue;
        }

        // `VACANT`, not `UNOCCUPIED`: the key stays, so the probe chains that pass
        // through this slot still terminate on it, and a later `intern` of the same
        // guest address reuses it rather than extending a chain past the slot every
        // other chain also passes through.
        uint32_t expected = (uint32_t)atomic_load_explicit(&entry->state, memory_order_relaxed);

        if (!atomic_compare_exchange_strong_explicit(&entry->state,
                                                     &expected,
                                                     (uint_least32_t)JIT_METADATA_STATE_VACANT,
                                                     memory_order_acq_rel,
                                                     memory_order_relaxed))
        {
            // A racer moved the state between the two passes. The only writer that
            // can do that without the lock is `try_begin`, which needs `EMPTY`, and
            // this entry was `READY` or `FAILED` when the first pass read it.
            // Reaching here means the table is not being used the way its contract
            // says, so it is reported rather than counted as a clear.
            POUND_LOG_ERROR(&thread_logger,
                            "Slot %zu changed from %s to %s between the two passes of an "
                            "invalidation of [0x%llx, 0x%llx), so it was not cleared.",
                            index,
                            jit_metadata_state_to_string(
                                (jit_metadata_state_t)atomic_load_explicit(&entry->state,
                                                                          memory_order_relaxed)),
                            jit_metadata_state_to_string((jit_metadata_state_t)expected),
                            (unsigned long long)begin,
                            (unsigned long long)end);

            mutex_unlock(&manager->lock);

            return POUND_ERROR_CORRUPTED;
        }

        clear_payload(entry);

        ++cleared;
    }

    // Every cleared entry was a live entry, so this cannot underflow -- and because
    // underflowing a `size_t` turns a wrong count into a count that looks enormous
    // (a table reporting itself full with four entries in it), the invariant is
    // asserted rather than assumed. A violation here means some other path cleared an
    // entry without decrementing, and continuing would hide that behind every
    // subsequent symptom instead of reporting it once, here.
    if (cleared > manager->interned)
    {
        mutex_unlock(&manager->lock);

        POUND_LOG_ERROR(&thread_logger,
                        "An invalidation of [0x%llx, 0x%llx) cleared %zu block(s) but the manager "
                        "believed it held %zu, so its entry count is corrupt.",
                        (unsigned long long)begin,
                        (unsigned long long)end,
                        cleared,
                        manager->interned);

        return POUND_ERROR_CORRUPTED;
    }

    manager->interned -= cleared;
    manager->generation = manager->generation + 1U;

    mutex_unlock(&manager->lock);

    *out_invalidated = cleared;

    count(manager, &manager->invalidations);

    // The quantity, not the call: "how often did the guest rewrite itself" and "how
    // much work did that cost" are different questions and the header documents this
    // counter as the answer to the second one.
    count_by(manager, &manager->invalidated_blocks, (uint_least64_t)cleared);

    POUND_LOG_DEBUG(&thread_logger,
                    "Invalidation of [0x%llx, 0x%llx) cleared %zu of %zu matching block(s); "
                    "generation is now %llu.",
                    (unsigned long long)begin,
                    (unsigned long long)end,
                    cleared,
                    matches,
                    (unsigned long long)manager->generation);

    return POUND_SUCCESS;
}

error_t
jit_metadata_set_map(jit_metadata_t *POUND_RESTRICT manager,
                     const size_t                          index,
                     const jit_register_slot_t *POUND_RESTRICT slots,
                     const size_t                          slot_count)
{
    if (NULL == manager)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the metadata manager is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if ((NULL == slots) && (0U != slot_count))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Slot %zu is to be given a %zu-entry register map from a NULL array.",
                        index,
                        slot_count);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const error_t unavailable = check_index(manager, index);

    if (POUND_SUCCESS != unavailable)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Slot %zu cannot be used: %s. The manager has %zu slot(s).",
                        index,
                        pound_error_to_string(unavailable),
                        (NULL == manager) ? 0U : manager->capacity);
        return unavailable;
    }

    if (slot_count > JIT_METADATA_MAP_SLOTS)
    {
        count(manager, &manager->map_refusals);

        POUND_LOG_ERROR(&thread_logger,
                        "Guest address 0x%llx wants a %zu-entry register map but a block holds at "
                        "most %u. Split the block: a basic block that has to preserve this many "
                        "host registers has grown past what a basic block should be, and "
                        "splitting it also shrinks its instruction footprint.",
                        (unsigned long long)((const jit_metadata_entry_t *)manager->table)[index].guest_pc,
                        slot_count,
                        JIT_METADATA_MAP_SLOTS);

        return POUND_ERROR_ALLOCATION_FAILED;
    }

    for (size_t slot = 0U; slot < slot_count; ++slot)
    {
        if (!slot_is_valid(&slots[slot]))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "Entry %zu of the register map for slot %zu is malformed: guest %u, "
                            "host %u, frame offset %u, location %u. A dispatcher acting on it would "
                            "read a register that was never written or spill to an offset that was "
                            "never reserved.",
                            slot,
                            index,
                            (unsigned)slots[slot].guest_index,
                            (unsigned)slots[slot].host_index,
                            (unsigned)slots[slot].frame_offset,
                            (unsigned)slots[slot].location);

            return POUND_ERROR_INVALID_ARGUMENT;
        }
    }

    jit_metadata_entry_t *POUND_RESTRICT entry = entry_at(manager, index);

    mutex_lock(&manager->lock);

    const uint32_t state = atomic_load_explicit(&entry->state, memory_order_relaxed);

    if (JIT_METADATA_STATE_CLAIMED != state)
    {
        mutex_unlock(&manager->lock);

        POUND_LOG_ERROR(&thread_logger,
                        "Slot %zu is %s, not %s, so its register map cannot be set. A map is what "
                        "makes a block callable, and a caller must never see a %s block whose map "
                        "is still being written.",
                        index,
                        jit_metadata_state_to_string((jit_metadata_state_t)state),
                        jit_metadata_state_to_string(JIT_METADATA_STATE_CLAIMED),
                        jit_metadata_state_to_string(JIT_METADATA_STATE_READY));

        return POUND_ERROR_ALREADY_INITIALIZED;
    }

    for (size_t slot = 0U; slot < JIT_METADATA_MAP_SLOTS; ++slot)
    {
        if (slot < slot_count)
        {
            entry->map[slot] = slots[slot];
        }
        else
        {
            entry->map[slot].guest_index = 0U;
            entry->map[slot].host_index = 0U;
            entry->map[slot].frame_offset = 0U;
            entry->map[slot].location = (uint16_t)JIT_REGISTER_LOCATION_NONE;
        }
    }

    entry->map_used = (uint32_t)slot_count;

    mutex_unlock(&manager->lock);

    return POUND_SUCCESS;
}

error_t
jit_metadata_get_map(const jit_metadata_t *POUND_RESTRICT manager,
                     const size_t                     index,
                     jit_register_slot_t *POUND_RESTRICT out,
                     size_t *POUND_RESTRICT            out_count)
{
    if (NULL == manager)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the metadata manager is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (NULL == out_count)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the register-map count destination for slot %zu is NULL.",
                        index);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const error_t unavailable = check_index(manager, index);

    if (POUND_SUCCESS != unavailable)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Slot %zu cannot be used: %s. The manager has %zu slot(s).",
                        index,
                        pound_error_to_string(unavailable),
                        (NULL == manager) ? 0U : manager->capacity);
        return unavailable;
    }

    const jit_metadata_entry_t *POUND_RESTRICT table = (const jit_metadata_entry_t *)manager->table;
    const jit_metadata_entry_t *POUND_RESTRICT entry = &table[index];

    const uint32_t state = atomic_load_explicit(&entry->state, memory_order_relaxed);

    if ((JIT_METADATA_STATE_UNOCCUPIED == state) || (JIT_METADATA_STATE_VACANT == state))
    {
        *out_count = 0U;

        POUND_LOG_ERROR(&thread_logger,
                        "Slot %zu is %s, so it has no register map.",
                        index,
                        jit_metadata_state_to_string((jit_metadata_state_t)state));

        return POUND_ERROR_NOT_INITIALIZED;
    }

    const uint32_t used = entry->map_used;

    *out_count = (size_t)used;

    if (0U == used)
    {
        return POUND_SUCCESS;
    }

    if (NULL == out)
    {
        *out_count = 0U;

        POUND_LOG_ERROR(&thread_logger,
                        "Slot %zu has a %zu-entry register map but the destination is NULL.",
                        index,
                        (size_t)used);

        return POUND_ERROR_INVALID_ARGUMENT;
    }

    memcpy(out, entry->map, (size_t)used * sizeof(jit_register_slot_t));

    return POUND_SUCCESS;
}

error_t
jit_metadata_set_label(jit_metadata_t *POUND_RESTRICT manager,
                       const size_t       index,
                       const char *POUND_RESTRICT label)
{
    if (NULL == manager)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the metadata manager is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (NULL == label)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the label for slot %zu is NULL.",
                        index);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const error_t unavailable = check_index(manager, index);

    if (POUND_SUCCESS != unavailable)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Slot %zu cannot be used: %s. The manager has %zu slot(s).",
                        index,
                        pound_error_to_string(unavailable),
                        (NULL == manager) ? 0U : manager->capacity);
        return unavailable;
    }

    if (bounded_length(label, JIT_METADATA_LABEL_MAX) >= JIT_METADATA_LABEL_MAX)
    {
        count(manager, &manager->label_refusals);

        POUND_LOG_ERROR(&thread_logger,
                        "The label for slot %zu is not NUL-terminated within %u bytes. It is "
                        "refused rather than cut short, because a label is read by a person "
                        "reading a log and a shortened one looks complete.",
                        index,
                        JIT_METADATA_LABEL_MAX);

        return POUND_ERROR_INVALID_ARGUMENT;
    }

    jit_metadata_entry_t *POUND_RESTRICT entry = entry_at(manager, index);

    mutex_lock(&manager->lock);

    copy_text(label, entry->text, sizeof(entry->text));

    mutex_unlock(&manager->lock);

    return POUND_SUCCESS;
}

error_t
jit_metadata_get_stats(jit_metadata_t *POUND_RESTRICT manager, jit_metadata_stats_t *POUND_RESTRICT out)
{
    if (NULL == manager)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the metadata manager is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (NULL == out)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the statistics destination is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!manager->initialised)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the metadata manager is not initialised.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    // Counted under the lock even though the states are atomic, because a snapshot
    // that reads "3 ready, 2 claimed, 1 failed" has to be a state that existed. The
    // counters themselves are read relaxed: they are already defined to be lossy.
    mutex_lock(&manager->lock);

    const jit_metadata_entry_t *POUND_RESTRICT entries = (const jit_metadata_entry_t *)manager->table;

    size_t ready = 0U;
    size_t claimed = 0U;
    size_t failed = 0U;
    size_t interned = 0U;

    for (size_t index = 0U; index < manager->capacity; ++index)
    {
        switch (atomic_load_explicit(&entries[index].state, memory_order_relaxed))
        {
            case JIT_METADATA_STATE_EMPTY:
                ++interned;
                break;

            case JIT_METADATA_STATE_CLAIMED:
                ++interned;
                ++claimed;
                break;

            case JIT_METADATA_STATE_READY:
                ++interned;
                ++ready;
                break;

            case JIT_METADATA_STATE_FAILED:
                ++interned;
                ++failed;
                break;

            case JIT_METADATA_STATE_UNOCCUPIED:
            case JIT_METADATA_STATE_VACANT:
            default:
                break;
        }
    }

    memset(out, 0, sizeof(*out));

    out->capacity = manager->capacity;
    out->max_blocks = manager->max_blocks;
    out->interned = interned;
    out->ready = ready;
    out->claimed = claimed;
    out->failed = failed;
    out->probe_total = (size_t)atomic_load_explicit(&manager->probe_total, memory_order_relaxed);
    out->probe_worst = (size_t)atomic_load_explicit(&manager->probe_worst, memory_order_relaxed);
    out->leases_outstanding = (size_t)atomic_load_explicit(&manager->leases_outstanding,
                                                           memory_order_relaxed);
    out->generation = manager->generation;
    out->hits = (uint64_t)atomic_load_explicit(&manager->hits, memory_order_relaxed);
    out->misses = (uint64_t)atomic_load_explicit(&manager->misses, memory_order_relaxed);
    out->claims = (uint64_t)atomic_load_explicit(&manager->claims, memory_order_relaxed);
    out->claim_conflicts = (uint64_t)atomic_load_explicit(&manager->claim_conflicts, memory_order_relaxed);
    out->publications = (uint64_t)atomic_load_explicit(&manager->publications, memory_order_relaxed);
    out->failures = (uint64_t)atomic_load_explicit(&manager->failures, memory_order_relaxed);
    out->invalidations = (uint64_t)atomic_load_explicit(&manager->invalidations, memory_order_relaxed);
    out->invalidated_blocks = (uint64_t)atomic_load_explicit(&manager->invalidated_blocks, memory_order_relaxed);
    out->map_refusals = (uint64_t)atomic_load_explicit(&manager->map_refusals, memory_order_relaxed);
    out->label_refusals = (uint64_t)atomic_load_explicit(&manager->label_refusals, memory_order_relaxed);
    out->lease_refusals = (uint64_t)atomic_load_explicit(&manager->lease_refusals, memory_order_relaxed);
    out->account = manager->account;
    out->counters_lock_free = counters_are_lock_free();

    mutex_unlock(&manager->lock);

    return POUND_SUCCESS;
}

void
jit_metadata_log_summary(jit_metadata_t *POUND_RESTRICT manager)
{
    if ((NULL == manager) || !manager->initialised)
    {
        POUND_LOG_WARN(&thread_logger,
                       "Not logging a metadata summary: the manager is %s.",
                       (NULL == manager) ? "NULL" : "not initialised");
        return;
    }

    jit_metadata_stats_t stats;

    if (POUND_SUCCESS != jit_metadata_get_stats(manager, &stats))
    {
        // `get_stats` has already logged the reason.
        return;
    }

    POUND_LOG_INFO(&thread_logger,
                   "Metadata: %zu of %zu addresses interned in %zu slots (%zu ready, %zu being "
                   "translated, %zu untranslatable, %zu lease(s) out), generation %llu.",
                   stats.interned,
                   stats.max_blocks,
                   stats.capacity,
                   stats.ready,
                   stats.claimed,
                   stats.failed,
                   stats.leases_outstanding,
                   (unsigned long long)stats.generation);

    POUND_LOG_INFO(&thread_logger,
                   "Metadata counters: %llu hit(s), %llu miss(es), %llu claim(s), %llu claim "
                   "conflict(s), %llu publication(s), %llu failure(s), %llu invalidation(s) "
                   "clearing %llu block(s). Longest probe %zu of %zu.",
                   (unsigned long long)stats.hits,
                   (unsigned long long)stats.misses,
                   (unsigned long long)stats.claims,
                   (unsigned long long)stats.claim_conflicts,
                   (unsigned long long)stats.publications,
                   (unsigned long long)stats.failures,
                   (unsigned long long)stats.invalidations,
                   (unsigned long long)stats.invalidated_blocks,
                   stats.probe_worst,
                   stats.capacity);

    if (!stats.account)
    {
        POUND_LOG_INFO(&thread_logger,
                       "Metadata counters are switched off, so every figure above is zero.");
    }
    else if (!stats.counters_lock_free)
    {
        POUND_LOG_INFO(&thread_logger,
                       "Metadata counters are not lock-free on this host, so they cost more than "
                       "they are worth on the dispatch path.");
    }
}

const char *
jit_metadata_state_to_string(const jit_metadata_state_t state)
{
    switch (state)
    {
        case JIT_METADATA_STATE_UNOCCUPIED:
            return "unoccupied";
        case JIT_METADATA_STATE_EMPTY:
            return "interned but not translated";
        case JIT_METADATA_STATE_CLAIMED:
            return "being translated";
        case JIT_METADATA_STATE_READY:
            return "ready";
        case JIT_METADATA_STATE_FAILED:
            return "untranslatable";
        default:
            return "unrecognised";
    }
}

/*** end of line ***/

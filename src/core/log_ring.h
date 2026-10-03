#ifndef POUND_CORE_LOG_RING_H
#define POUND_CORE_LOG_RING_H

/// An in-process ring of recent log records.
///
/// `log.h` dispatches every record to whatever callback the thread's logger
/// holds, and the default one prints it and forgets it. That is the right
/// behaviour for a console and useless for an interface: the Logs page needs to
/// read back what was just logged, and a bug report needs the last hundred lines
/// without asking a user to copy them out of a terminal.
///
/// This is the smallest thing that provides both. It installs itself as the
/// thread's log callback, formats each record into a fixed-size slot, keeps the
/// most recent `capacity` of them, and passes every record on to the sink that was
/// there before -- so nothing that used to be logged stops being logged.
///
/// Two properties worth stating rather than discovering:
///
///  * It is *per-thread*, because `thread_logger` is thread-local. Installing it
///    captures the thread that installed it and no other. The GUI installs it on
///    the thread it renders on, which is the thread whose records a user is
///    looking at; a core worker's records still reach the console. Capturing every
///    thread would mean a registry of loggers and a way to reach them, which is a
///    larger piece of machinery than the interface needs.
///
///  * It is bounded and lossy by design. The oldest record is overwritten when the
///    ring is full, and each record carries a monotonic `sequence`, so a reader can
///    tell that records were dropped rather than silently reading a gap as
///    contiguous.

#include "attributes.h"
#include "errors.h"
#include "log.h"
#include "sync/mutex.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Longest formatted record the ring stores, including the terminator.
///
/// A slot is written with `vsnprintf` and truncated at this length; a longer
/// message keeps its beginning, which is where the identifying part (a filename,
/// an error sentence) lives. 320 bytes holds every message the codebase currently
/// emits with room to spare, and 1024 slots of them is a third of a megabyte --
/// small enough to keep resident without thinking about it.
#define POUND_LOG_RING_TEXT_MAX 320

/// The default number of records a ring keeps.
#define POUND_LOG_RING_DEFAULT_CAPACITY 1024

/// One stored record.
typedef struct
{
    /// Monotonic across the ring's lifetime, starting at 1.
    ///
    /// A reader that remembers the sequence of the last record it saw can use a
    /// jump of more than one to detect that the ring wrapped past it.
    uint64_t sequence;

    log_level_t level;

    /// The formatted message, always terminated.
    char text[POUND_LOG_RING_TEXT_MAX];

    /// Explicit padding so the struct has no *implicit* padding, which the build
    /// treats as a portability defect worth failing on.
    char pad[4];
} pound_log_record_t;

typedef struct
{
    pound_log_record_t *records;

    /// How many records the allocation holds.
    size_t capacity;

    /// How many of them are valid; saturates at `capacity`.
    size_t count;

    /// Where the next record is written.
    size_t next;

    /// The sequence number the next record will carry.
    uint64_t next_sequence;

    /// The sink that was installed before this ring, and its context.
    ///
    /// Kept so the ring can be removed without silencing logging, and so every
    /// record still reaches the console while the ring is installed.
    log_function_t previous_log;
    void          *previous_user_data;

    /// Guards `records`, `count`, `next` and `next_sequence`.
    ///
    /// A log call can come from any thread that installed the ring, and the GUI
    /// renders on a different one from the core workers that also log.
    mutex_t lock;

    bool initialized;
    bool installed;
    char pad[6];
} pound_log_ring_t;

/// Allocates a ring of `capacity` records and initialises it.
///
/// `capacity` of 0 is refused rather than promoted to the default: a caller that
/// computes a capacity from a setting and gets zero has a bug, and an empty ring
/// would present it as "logging is broken".
///
/// Returns `POUND_ERROR_ALLOCATION_FAILED` when the record array cannot be
/// obtained and `POUND_ERROR_INVALID_ARGUMENT` for a NULL or zero-capacity ring,
/// logging the reason either way.
error_t pound_log_ring_init(pound_log_ring_t *POUND_RESTRICT ring, size_t capacity);

/// Frees the ring, uninstalling it first if it is still installed.
///
/// Safe to call on a ring that was never initialised and safe to call twice; in
/// both cases it returns without doing anything, because a shutdown path that has
/// to know whether init ran is a shutdown path that will get it wrong.
void pound_log_ring_shutdown(pound_log_ring_t *POUND_RESTRICT ring);

/// Installs the ring as the calling thread's log callback.
///
/// The sink already installed is remembered and still called for every record, so
/// console output continues. Installing a ring that is already installed is
/// refused rather than treated as a no-op: it would overwrite the remembered sink
/// with the ring itself and produce unbounded recursion on the next record.
error_t pound_log_ring_install(pound_log_ring_t *POUND_RESTRICT ring);

/// Restores the sink that was installed before this ring.
///
/// Returns without changing anything if the ring is not installed, or if the
/// thread's callback is no longer this ring -- the latter because unconditionally
/// restoring would undo somebody else's install.
void pound_log_ring_uninstall(pound_log_ring_t *POUND_RESTRICT ring);

/// How many valid records the ring currently holds.
size_t pound_log_ring_count(pound_log_ring_t *POUND_RESTRICT ring);

/// The sequence number of the newest record, or 0 when the ring is empty.
uint64_t pound_log_ring_newest_sequence(pound_log_ring_t *POUND_RESTRICT ring);

/// Drops every record, leaving the sequence counter alone.
///
/// The counter is not reset: a reader holding an old sequence must still be able
/// to tell that what it is looking at is newer, and restarting at 1 would make a
/// cleared ring indistinguishable from a fresh one.
void pound_log_ring_clear(pound_log_ring_t *POUND_RESTRICT ring);

/// Copies the newest records into `out`, oldest first.
///
/// Copies at most `capacity` of them and returns how many were written. When the
/// caller's capacity is smaller than the ring's, the *newest* `capacity` records
/// are the ones copied, because that is what a log view shows.
///
/// Returns 0 for a NULL or uninitialised ring, or a NULL `out` with non-zero
/// capacity.
size_t pound_log_ring_snapshot(pound_log_ring_t *POUND_RESTRICT ring,
                               pound_log_record_t *POUND_RESTRICT out,
                               size_t capacity);

/// Logs how many records the ring holds and how many it can hold, at INFO.
void pound_log_ring_log_summary(pound_log_ring_t *POUND_RESTRICT ring);

#endif // POUND_CORE_LOG_RING_H

/*** end of file ***/
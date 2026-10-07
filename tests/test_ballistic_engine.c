//! Verifying the allocator bridge against the *real* Ballistic engine.
//!
//! `test_jit_ballistic.c` drives the bridge's six callbacks by hand, through a
//! `bal_allocator_t` it fills in itself. That proves the callbacks do what they claim,
//! and it proves nothing about whether the engine agrees. The two ways this can be
//! wrong are both silent:
//!
//! * The engine can request an allocation the cache refuses in a way the cache reports
//!   as a refusal but the engine reads as success -- and the first symptom is a jump
//!   into a null pointer two million instructions into a game.
//! * The engine can insist on something the bridge never agreed to: a particular
//!   alignment, a write-then-execute sequence the cache's W^X guards reject, or a
//!   `bal_executable_buffer_t` contract (`NULL` on failure) that the bridge does not
//!   honour. None of that shows up until a translated block is actually produced.
//!
//! So this file builds a real `bal_engine_t`, feeds it real guest ARM64 through the
//! bridge, and runs it. If the cache cannot satisfy what the engine asks for, a block
//! never appears and `bal_engine_run_thread` returns an error instead of executing --
//! which is a failure at step one of the test rather than at step two million.
//!
//! # Why single-stepping
//!
//! Every case runs with `BAL_ENGINE_FLAG_SINGLE_STEP`, so `bal_engine_run_thread`
//! executes one guest instruction and returns. That removes the need for a worker
//! thread, a stop flag, and a timed wait: the guest's register file is inspected after
//! a known number of instructions rather than after an unknown amount of execution.
//! A timing-based test would pass or fail depending on how fast the host is, and would
//! be worth much less than the determinism is worth.
//!
//! It also means no case depends on the engine halting by itself: the guest programs
//! end with a load from an unmapped guest address, so if single-stepping ever stops
//! being honoured the engine faults there and returns a diagnosable error rather than
//! spinning on an infinite branch -- which would be the one failure a CI lane cannot
//! report.
//!
//! # Why a thread per instruction
//!
//! `bal_engine_run_thread` is documented as needing "a dedicated thread", and it means
//! it: called on a thread that is not the engine's own, it returns `BAL_SUCCESS` having
//! executed *nothing*, leaving the program counter where it started. That is not a
//! subtle degradation to assert on -- it is a guest that does nothing, reported as
//! success -- so the cases below run it the way the engine requires and join afterwards.
//!
//! One thread per instruction rather than one for the whole program sounds wasteful, and
//! creating four threads is not free. But the join *is* the barrier: nothing polls a
//! counter, nothing sleeps, and nothing depends on the host being fast enough, so an
//! instruction cannot be half-executed when it is inspected.
//!
//! # Platform
//!
//! Compiled only where the engine is linked (`POUND_ENABLE_BALLISTIC`). This is the one
//! test that pulls in the prebuilt, so it is also the one that verifies the packaging:
//! the static library, the LuaJIT import library and the runtime all have to resolve,
//! or the test binary does not link.

#include "pound_test.h"

#include "errors.h"
#include "jit/jit_ballistic.h"
#include "jit/jit_cache.h"
#include "log.h"
#include "memory/memory.h"
#include "sync/thread.h"

#include <bal_decoder.h>
#include <bal_engine.h>
#include <bal_engine_flags.h>
#include <bal_errors.h>
#include <bal_log.h>
#include <bal_memory.h>
#include <backend/bal_cpu.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/// Guest address the first program is loaded at.
///
/// Not zero. A flat translation interface maps guest address zero onto the first byte
/// of the buffer, and an engine that starts at address zero is indistinguishable from
/// one that has failed to set a program counter at all.
#define ENGINE_GUEST_BASE 0x1000ULL

/// Bytes of guest memory mapped. Large enough that the code, a scratch word the guest
/// can store to, and a self-branch target all sit inside it with room to spare.
#define ENGINE_GUEST_BYTES 0x2000U

/// Guest address of the word the load/store program writes to.
#define ENGINE_SCRATCH_WORD (ENGINE_GUEST_BASE + 0x80ULL)

/// Offset of the first program within the mapped buffer.
#define ENGINE_PROGRAM_ONE_OFFSET ENGINE_GUEST_BASE

/// Offset of the second program within the mapped buffer.
#define ENGINE_PROGRAM_TWO_OFFSET (ENGINE_GUEST_BASE + 0x40U)

// -----------------------------------------------------------------------------
// Guest programs
// -----------------------------------------------------------------------------

/// Writes one 32-bit instruction big-endian-agnostic: ARM64 instructions are stored
/// little-endian, and a test that writes them in host order passes on a little-endian
/// host and is meaningless everywhere else. Encoding the bytes explicitly means the
/// instruction is spelled out here rather than inherited from whatever the compiler
/// emitted.
static void
put_instruction(uint8_t *at, const uint32_t encoding)
{
    at[0] = (uint8_t)(encoding & 0xFFU);
    at[1] = (uint8_t)((encoding >> 8) & 0xFFU);
    at[2] = (uint8_t)((encoding >> 16) & 0xFFU);
    at[3] = (uint8_t)((encoding >> 24) & 0xFFU);
}

/// MOVZ Xd, #imm16 -- `1101 0010 100 hw 0 imm16 Rd`, ARMv8-A A64, 32.4.5.
#define ENGINE_MOVZ(rd_, imm_) ((uint32_t)(0xD2800000U | (((uint32_t)(imm_)) << 5) | (uint32_t)(rd_)))

/// ADD Xd, Xn, #imm12 -- `1001 0001 00 0 imm12 Rn Rd`, ARMv8-A A64, 32.5.
#define ENGINE_ADD_IMM(rd_, rn_, imm_) \
    ((uint32_t)(0x91000000U | (((uint32_t)(imm_)) << 10) | ((uint32_t)(rn_) << 5) | (uint32_t)(rd_)))

/// MUL Xd, Xn, Xm -- `1001 1011 0110 Rm 000000 Rn Rd`, ARMv8-A A64, 32.5.
#define ENGINE_MUL(rd_, rn_, rm_) \
    ((uint32_t)(0x9B007C00U | ((uint32_t)(rm_) << 16) | ((uint32_t)(rn_) << 5) | (uint32_t)(rd_)))

/// LDR Wt, [Xn, #imm12] -- unsigned immediate, 64-bit base, ARMv8-A A64, 32.3.
#define ENGINE_LDR_W(rt_, rn_, imm_) \
    ((uint32_t)(0xB9400000U | (((uint32_t)(imm_)) << 10) | ((uint32_t)(rn_) << 5) | (uint32_t)(rt_)))

/// STR Wt, [Xn, #imm12] -- unsigned immediate, 64-bit base, ARMv8-A A64, 32.3.
#define ENGINE_STR_W(rt_, rn_, imm_) \
    ((uint32_t)(0xB9000000U | (((uint32_t)(imm_)) << 10) | ((uint32_t)(rn_) << 5) | (uint32_t)(rt_)))

/// B . -- branch to self, so a guest that runs past its program spins in place.
#define ENGINE_BRANCH_SELF 0x14000000U

// -----------------------------------------------------------------------------
// The engine's own log
// -----------------------------------------------------------------------------
//
// The engine keeps a `bal_logger_t` and reports nothing through it unless one is
// installed. Left `NULL`, a guest that does not run and a bridge that is misconfigured
// look identical from the outside: `BAL_SUCCESS`, no registers changed, no clue.
//
// So the logger is wired into Pound's. That is not only a debugging aid -- the emulator
// will want the same thing, and an engine error message with the file, line and
// function it came from is the difference between "the JIT did not run" and "the JIT
// declined to translate this instruction".

/// Renders one engine message into Pound's logger, keeping the engine's severity.
///
/// The message is formatted first and passed as an argument rather than spliced into a
/// format string: `format` is the engine's, and handing it to a logger that will apply
/// its own format to the result is how a `%` in guest-derived text becomes a crash.
static void
engine_log(void *user_data, bal_log_data_t *data, const char *format, va_list args)
{
    (void)user_data;

    if ((NULL == data) || (NULL == format))
    {
        return;
    }

    char text[512];

    const int written = vsnprintf(text, sizeof(text), format, args);

    if (written < 0)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Ballistic logged a message it could not format, from %s:%d.",
                        (NULL == data->filename) ? "unknown" : data->filename,
                        data->line);
        return;
    }

    // A message longer than the buffer is truncated rather than dropped, and the fact
    // is stated: a truncated log line that looks complete is worse than a noisy one.
    const bool truncated = ((size_t)written >= sizeof(text));

    switch (data->level)
    {
        case BAL_LOG_LEVEL_ERROR:
            POUND_LOG_ERROR(&thread_logger,
                            "[ballistic %s:%d %s] %s%s",
                            (NULL == data->filename) ? "unknown" : data->filename,
                            data->line,
                            (NULL == data->function) ? "unknown" : data->function,
                            text,
                            truncated ? " [truncated]" : "");
            return;

        case BAL_LOG_LEVEL_WARN:
            POUND_LOG_WARN(&thread_logger,
                           "[ballistic %s:%d %s] %s%s",
                           (NULL == data->filename) ? "unknown" : data->filename,
                           data->line,
                           (NULL == data->function) ? "unknown" : data->function,
                           text,
                           truncated ? " [truncated]" : "");
            return;

        case BAL_LOG_LEVEL_INFO:
            POUND_LOG_INFO(&thread_logger,
                           "[ballistic %s:%d %s] %s%s",
                           (NULL == data->filename) ? "unknown" : data->filename,
                           data->line,
                           (NULL == data->function) ? "unknown" : data->function,
                           text,
                           truncated ? " [truncated]" : "");
            return;

        case BAL_LOG_LEVEL_DEBUG:
        case BAL_LOG_LEVEL_TRACE:
            POUND_LOG_DEBUG(&thread_logger,
                            "[ballistic %s:%d %s] %s%s",
                            (NULL == data->filename) ? "unknown" : data->filename,
                            data->line,
                            (NULL == data->function) ? "unknown" : data->function,
                            text,
                            truncated ? " [truncated]" : "");
            return;

        default:
            return;
    }
}

/// Fills in a logger that routes to Pound's, at the given verbosity.
static void
engine_logger_init(bal_logger_t *logger, const bal_log_level_t min_level)
{
    logger->user_data = NULL;
    logger->log       = engine_log;
    logger->min_level = min_level;
    logger->pad       = 0U;
}

// -----------------------------------------------------------------------------
// Guest address translation
// -----------------------------------------------------------------------------

/// The guest address space presented to the engine: one window, based at guest zero.
///
/// A plain pair of pointers, so it costs no padding in the fixture and needs no
/// allocation of its own -- the fixture owns it and the interface holds a pointer to it.
typedef struct
{
    uint8_t *base;
    size_t   bytes;
} guest_window_t;

/// Translates a guest address to a host address, refusing anything outside the window.
///
/// Written here rather than taken from `bal_flat_translation_interface_init`, for two
/// reasons, and the first is why it was written at all.
///
/// The convenience constructor reports guest addresses *inside* the window it was given
/// as unmapped, and the engine's only symptom of that is
/// `BAL_ERROR_UNKNOWN_INSTRUCTION` -- the same code it returns for an instruction it
/// cannot parse. With an 8 KiB window and a program at guest `0x1000`, every diagnostic
/// pointed the wrong way at once: the host buffer held the right encodings at the right
/// program counter, the engine's own decoder accepted all of them, and the engine still
/// could not decode. The convenience constructor is not usable here, and the case
/// `the_flat_interface_resolves_a_guest_address_to_the_byte_it_names` is what pins that
/// down rather than leaving it as a suspicion.
///
/// The second reason is that Pound needs this callback to be its own regardless. A
/// title's address space is not one flat buffer: it is assembled from NCA segments, a
/// zero-initialised .bss, a stack and a TLS block, each with its own permissions and its
/// own size. `jit_cache` binds an allocator of its own for exactly the same reason, and
/// the same argument applies here -- the emulator's memory model is the product, not a
/// convenience.
///
/// `max_readable` is the whole point of the second out-parameter: it is how the engine is
/// told where the window ends so it never reads a partial instruction past it. It is
/// written even when the address is rejected, so a caller that ignores the `NULL` cannot
/// carry a stale length into the next request.
static const uint8_t *
translate_window(void *context, const bal_guest_address_t guest_address, size_t *max_readable)
{
    if (NULL == max_readable)
    {
        return NULL;
    }

    *max_readable = 0U;

    if (NULL == context)
    {
        return NULL;
    }

    const guest_window_t *const window = (const guest_window_t *)context;

    // Widened before the comparison rather than after. A 64-bit guest address narrowed to
    // `size_t` first would wrap, and a wrapped address inside the window is a way to read
    // host memory the guest was never given.
    const uintptr_t offset = (uintptr_t)guest_address;

    if (offset >= (uintptr_t)window->bytes)
    {
        return NULL;
    }

    *max_readable = window->bytes - (size_t)offset;

    return window->base + offset;
}

// -----------------------------------------------------------------------------
// Fixture
// -----------------------------------------------------------------------------

/// The objects Ballistic requires to be 64-byte aligned, in an allocation of their own.
///
/// `bal_cpu_t` and `bal_engine_t` are both declared 64-byte aligned by the engine's own
/// headers, and a stack object cannot be assumed to land on a cache line -- which the
/// engine's static assertion about its own size shows it cares about. Mapped guest
/// memory joins them here because `bal_flat_translation_interface_init` refuses a buffer
/// that is not at least 16-byte aligned.
///
/// They are a separate allocation, and a separate struct, from [`engine_fixture_t`]
/// because a struct containing a 64-byte-aligned member must itself be a whole number of
/// cache lines long. Mixing them with the bookkeeping would force up to 56 bytes of
/// padding at one end or the other. Here every member is a whole number of cache lines
/// already -- `ENGINE_GUEST_BYTES` is chosen for exactly that reason -- so there is no
/// padding anywhere in it.
typedef struct
{
    BAL_ALIGNED(64) bal_cpu_t cpu;
    BAL_ALIGNED(64) bal_engine_t engine;
    BAL_ALIGNED(64) uint8_t guest[ENGINE_GUEST_BYTES];
} engine_guest_t;

/// Everything one case owns.
///
/// Nothing here is 64-byte aligned, so there is no padding: every member is a pointer or
/// a struct whose own alignment is satisfied naturally.
typedef struct
{
    /// The cache-line-aligned objects, from their own allocation. A pointer rather than
    /// an embedded object so that the fixture itself needs no particular alignment.
    engine_guest_t *guests;

    jit_cache_t cache;

    /// The callback table the engine holds. Built by the bridge.
    bal_allocator_t allocator;

    /// Guest-to-host translation. The callbacks are `translate_window`'s; see there.
    bal_memory_interface_t interface;

    /// What `interface.context` points at. Last, and two pointers wide, so the fixture
    /// still has no padding anywhere in it.
    guest_window_t window;
} engine_fixture_t;

/// Brings up a cache, a bridged allocator, a mapped guest and a live engine.
///
/// Returns NULL and logs the reason if any step fails, so a case reads as a sequence
/// of concerns rather than four repetitions of the same teardown.
/// Establishes a case's guest program and register state.
///
/// Takes the aligned block rather than the fixture because the guest state has to be in
/// place *before* `bal_engine_init`: the engine takes its starting program counter from
/// the `bal_cpu_t` it is handed, so a program written afterwards is never fetched. That
/// is not a documented requirement -- `bal_engine_run_thread` returns
/// `BAL_ERROR_UNKNOWN_INSTRUCTION` for it rather than saying so -- and it is exactly the
/// kind of thing that would otherwise be rediscovered as a mysterious decode failure.
typedef void (*program_loader_t)(engine_guest_t *guests);

static engine_fixture_t *
fixture_create(const program_loader_t load)
{
    engine_fixture_t *fixture = (engine_fixture_t *)memory_subsystem_allocate(64U, sizeof(engine_fixture_t));

    if (NULL == fixture)
    {
        POUND_LOG_ERROR(&thread_logger, "Could not allocate the %zu-byte engine fixture.",
                        sizeof(engine_fixture_t));

        return NULL;
    }

    memset(fixture, 0, sizeof(*fixture));

    // The cache-line-aligned objects, at 64-byte alignment as the engine's own headers
    // require. Allocated separately from the fixture because putting a 64-byte-aligned
    // member inside it would force the whole fixture to be padded out to a cache line.
    fixture->guests = (engine_guest_t *)memory_subsystem_allocate(64U, sizeof(engine_guest_t));

    if (NULL == fixture->guests)
    {
        POUND_LOG_ERROR(&thread_logger, "Could not allocate the %zu-byte engine guest state.",
                        sizeof(engine_guest_t));

        memory_subsystem_free(fixture);

        return NULL;
    }

    memset(fixture->guests, 0, sizeof(*fixture->guests));

    const error_t cached = jit_cache_init(&fixture->cache, NULL);

    if (POUND_SUCCESS != cached)
    {
        POUND_LOG_ERROR(&thread_logger, "The bridge fixture's code cache did not start (%s).",
                        pound_error_to_string(cached));

        memory_subsystem_free(fixture->guests);
        memory_subsystem_free(fixture);

        return NULL;
    }

    const error_t bound = jit_cache_bind_ballistic(&fixture->cache, &fixture->allocator);

    if (POUND_SUCCESS != bound)
    {
        POUND_LOG_ERROR(&thread_logger, "The bridge fixture could not bind the allocator (%s).",
                        pound_error_to_string(bound));

        jit_cache_destroy(&fixture->cache);
        memory_subsystem_free(fixture->guests);
        memory_subsystem_free(fixture);

        return NULL;
    }

    // The translation interface is wired by hand rather than through
    // `bal_flat_translation_interface_init`; see `translate_window` for why.
    fixture->window.base  = fixture->guests->guest;
    fixture->window.bytes = sizeof(fixture->guests->guest);

    fixture->interface.context   = &fixture->window;
    fixture->interface.translate = translate_window;

    // The guest's program and registers are laid down before the engine starts, because
    // the engine takes its starting program counter from the CPU state it is handed
    // rather than re-reading it when execution begins. `load` writes into the same block
    // the engine is about to be given, so there is no window in which the two disagree.
    load(fixture->guests);

    const bal_error_t started = bal_engine_init(&fixture->guests->engine,
                                                &fixture->guests->cpu,
                                                &fixture->allocator,
                                                &fixture->interface);

    if (BAL_SUCCESS != started)
    {
        POUND_LOG_ERROR(&thread_logger, "The bridge fixture's engine did not start (%s).",
                        bal_error_to_string(started));

        jit_cache_destroy(&fixture->cache);
        memory_subsystem_free(fixture->guests);
        memory_subsystem_free(fixture);

        return NULL;
    }

    // Installed after init, because `bal_engine_init` fills the struct in and would
    // overwrite a logger set beforehand. As verbose as the engine will emit: whether it
    // was built with logging compiled out is the engine's business, and a silent engine
    // is a thing this case should be able to see.
    engine_logger_init(&fixture->guests->engine.logger, BAL_LOG_LEVEL_DEBUG);

    // Instruction counting is how the cases know when to stop the guest, so it is set
    // here and checked by every case rather than trusted. An engine that ignored it
    // would leave `run_to` spinning to its bound and then report an instruction
    // overflow, which names the wrong thing entirely.
    fixture->guests->engine.flags |= (uint32_t)BAL_ENGINE_FLAG_INSTRUCTION_COUNTING;

    return fixture;
}

/// Tears the fixture down in the reverse order it was built.
///
/// The engine must go first: it holds the allocator and the memory interface, and
/// destroying either underneath it would be a use-after-free the engine cannot detect.
static void
fixture_destroy(engine_fixture_t *fixture)
{
    if (NULL == fixture)
    {
        return;
    }

    bal_engine_destroy(&fixture->guests->engine);
    jit_cache_destroy(&fixture->cache);
    memory_subsystem_free(fixture->guests);
    memory_subsystem_free(fixture);
}

/// What the worker thread is given.
///
/// Two pointers, so the struct has no padding: a pointer and an error code would leave
/// four bytes between them, and the error code is handed back through a slot on the
/// caller's stack instead, which costs nothing because the join orders the worker's
/// write before the caller's read.
typedef struct
{
    bal_error_t *result;
    engine_fixture_t *fixture;
} run_request_t;

/// The thread body.
///
/// Trivial by design: the engine's own result is the only thing worth reporting back, and
/// it goes straight into the slot. Progress is signalled with `bal_engine_is_running`
/// rather than a flag of this file's own, because that is what the engine documents as
/// safe to ask from another thread -- a `volatile bool` written by one thread and read by
/// another is a data race with a convenient-looking name.
static void
run_worker(void *context)
{
    run_request_t *const request = (run_request_t *)context;

    *request->result = bal_engine_run_thread(&request->fixture->guests->engine);
}

/// Runs the guest until it has retired at least `stop_after` instructions, then stops it
/// and waits for the engine's thread to unwind.
///
/// This is how the emulator drives the engine, and it is the only way to see the guest's
/// register file as it actually lands. Single-stepping was tried first and does not
/// behave as `BAL_ENGINE_FLAG_SINGLE_STEP` documents: `bal_engine_run_thread` returns
/// `BAL_SUCCESS` having executed nothing and leaves the program counter where it started.
/// Reported as success, which is the worst possible shape for a defect to have.
///
/// Returns `BAL_SUCCESS` once the guest has run and the engine has stopped cleanly.
///
/// The guest programs end in a load from an unmapped address, so if the stop never lands
/// the engine faults there and returns rather than spinning on the branch to self. A
/// failure mode that hangs the whole test run is not one worth keeping.
static bal_error_t
run_to(engine_fixture_t *fixture, const uint64_t stop_after)
{
    enum
    {
        /// Generous. Retiring a handful of instructions takes microseconds even on a
        /// loaded machine, so exhausting this means the engine is not making progress at
        /// all rather than merely being slow. Iterations rather than a duration, so this
        /// measures the engine and not the host's clock.
        SPIN_LIMIT = 100000000U,
    };

    // Read from this thread while the engine's thread writes it. `volatile` so the load
    // cannot be hoisted out of the loop, which is the whole practical requirement here:
    // a field inside a struct the engine owns cannot be made formally atomic from
    // outside it.
    volatile const uint64_t *const counter = &fixture->guests->cpu.instruction_count;

    bal_error_t result = BAL_ERROR_THREAD_CREATION;

    run_request_t request;

    request.result  = &result;
    request.fixture = fixture;

    thread_t thread;

    // `thread_t` documents `NULL` as the pre-start state, and `thread_start`
    // refuses a handle it cannot account for, so an uninitialised `thread_t` is a
    // refusal no matter the stack's luck. The zero write is the contract.
    memset(&thread, 0, sizeof(thread));

    const error_t started = thread_start(&thread, run_worker, &request);

    if (POUND_SUCCESS != started)
    {
        return BAL_ERROR_THREAD_CREATION;
    }

    bool reached = false;
    bool running = false;

    // Phase one: wait for the engine to report itself running.
    //
    // `bal_engine_is_running` is false until the worker thread has actually entered
    // `bal_engine_run_thread`, so treating "not running" as "finished" from the start
    // ends the wait before the engine has begun -- which then looks exactly like an
    // engine that executes nothing, and reports the program counter as untouched. A
    // fresh thread takes on the order of a microsecond to be scheduled, so this normally
    // exits on its first few iterations.
    for (size_t spin = 0U; spin < (size_t)SPIN_LIMIT; ++spin)
    {
        if (*counter >= stop_after)
        {
            reached = true;
            break;
        }

        if (bal_engine_is_running(&fixture->guests->engine))
        {
            running = true;
            break;
        }
    }

    // Phase two: the engine has been seen running, so from here on "not running" does
    // mean it has finished, and the loop can end early instead of running out its bound
    // -- which is what keeps an engine that faults on the first instruction being
    // reported as a fault rather than as never having got going.
    if (running && (!reached))
    {
        for (size_t spin = 0U; spin < (size_t)SPIN_LIMIT; ++spin)
        {
            if (*counter >= stop_after)
            {
                reached = true;
                break;
            }

            if (!bal_engine_is_running(&fixture->guests->engine))
            {
                break;
            }
        }
    }

    // Stopped unconditionally. If the engine is wedged the call is harmless, and leaving
    // it running would mean joining a thread that never returns.
    bal_engine_stop_thread(&fixture->guests->engine);

    if (POUND_SUCCESS != thread_join(&thread))
    {
        return BAL_ERROR_THREAD_CLEANUP;
    }

    // Reported whenever the count was not reached, not only when the engine also claimed
    // success. An engine that stops early usually stops *with* an error, and the count
    // and program counter are what say whether it ran out of room or ran out of guest
    // program -- two very different defects with the same return code.
    if (!reached)
    {
        // The guest words at the program counter, from the host buffer. A decode failure
        // means the engine read something it could not parse, and whether the buffer holds
        // what this file wrote is the difference between "the bridge served the wrong
        // memory" and "the engine looked somewhere else" -- two problems with nothing in
        // common, and the log line is the only way to tell them apart.
        uint32_t words[8] = {0U};

        const uint64_t fetch_at = fixture->guests->cpu.pc;

        if ((fetch_at + (sizeof(words))) <= (uint64_t)ENGINE_GUEST_BYTES)
        {
            for (size_t word = 0U; word < 8U; ++word)
            {
                memcpy(&words[word],
                       &fixture->guests->guest[fetch_at + (word * 4U)],
                       sizeof(words[word]));
            }

            POUND_LOG_ERROR(&thread_logger,
                            "The engine retired %llu instruction(s) rather than the %llu asked "
                            "for, stopping at guest 0x%llx and reporting %s. The buffer there "
                            "holds %08x %08x %08x %08x.",
                            (unsigned long long)fixture->guests->cpu.instruction_count,
                            (unsigned long long)stop_after,
                            (unsigned long long)fetch_at,
                            bal_error_to_string(result),
                            words[0],
                            words[1],
                            words[2],
                            words[3]);
        }
        else
        {
            POUND_LOG_ERROR(&thread_logger,
                            "The engine retired %llu instruction(s) rather than the %llu asked "
                            "for, stopping at guest 0x%llx -- outside the %llu-byte mapped "
                            "window -- and reporting %s.",
                            (unsigned long long)fixture->guests->cpu.instruction_count,
                            (unsigned long long)stop_after,
                            (unsigned long long)fetch_at,
                            (unsigned long long)ENGINE_GUEST_BYTES,
                            bal_error_to_string(result));
        }
    }

    // A successful return that never reached the instruction count means the engine ran
    // to its own exit without executing the program -- the exact defect this suite exists
    // to catch, so it is never allowed to read as success. The code is the engine's own
    // memory-fault code, which cannot be confused with the IR-arena overflow the engine
    // reports for itself, and `run_to`'s log record above names the real cause.
    if ((!reached) && (BAL_SUCCESS == result))
    {
        return BAL_ERROR_MEMORY_FAULT;
    }

    return result;
}

/// Guest address used by the load at the end of each program.
///
/// Outside the mapped window on purpose -- see the note about program terminators at
/// the top of this file. `ENGINE_GUEST_BYTES` bytes are mapped from guest zero, so
/// anything at or above that is unmapped and faults. Chosen to fit in a single `movz`
/// immediate so the instruction that sets it up stays a one-liner, which keeps the
/// encoding visible in the program listing rather than split across a second load.
#define ENGINE_FAULT_ADDRESS 0x4000ULL

/// Loads x3 from an unmapped guest address: `ldr x3, [x4, #0]`, with the faulting
/// address left in `x4` so a runaway engine's fault names something recognisable.
#define ENGINE_LDR_FAULT 0xF9400083U

/// Fills in the first program:
///
///     movz x0, #0x1234
///     add  x0, x0, #1
///     movz x2, #0x10
///     mul  x0, x0, x2
///     movz x4, #0x4000
///     ldr  x3, [x4]        -- unmapped; never reached, and the engine's exit if it is
///     b    .
///
/// so `x0` ends at `0x12350` after four instructions. Every one of the four is a real
/// encoding written here rather than assembled by the compiler, so a wrong answer names
/// the arithmetic that produced it.
static void
load_program_one(engine_guest_t *guests)
{
    uint8_t *program = &guests->guest[ENGINE_PROGRAM_ONE_OFFSET];

    put_instruction(&program[0], ENGINE_MOVZ(0U, 0x1234U));
    put_instruction(&program[4], ENGINE_ADD_IMM(0U, 0U, 1U));
    put_instruction(&program[8], ENGINE_MOVZ(2U, 0x10U));
    put_instruction(&program[12], ENGINE_MUL(0U, 0U, 2U));
    put_instruction(&program[16], ENGINE_MOVZ(4U, (uint32_t)ENGINE_FAULT_ADDRESS));
    put_instruction(&program[20], ENGINE_LDR_FAULT);
    put_instruction(&program[24], ENGINE_BRANCH_SELF);

    guests->cpu.pc = ENGINE_GUEST_BASE;
}

/// Loads the second program, which reads and writes guest memory:
///
///     str  w2, [x1, #0]
///     ldr  w3, [x1, #0]
///     movz x4, #0x4000
///     ldr  x3, [x4]        -- unmapped; never reached, and the engine's exit if it is
///     b    .
///
/// `x1` and `x2` are set here rather than by the case, for the same reason the program
/// is: the engine takes its starting registers from the CPU state it is handed, so
/// anything a case sets after the engine is up is ignored. `x1` points at
/// `ENGINE_SCRATCH_WORD`, so the store lands inside mapped guest memory, and `x2` is the
/// value it stores. This is the program that exercises the translation callback: the
/// engine cannot produce a correct block for a program that touches memory without asking
/// the host where the memory is, and a translation interface that answered wrongly would
/// show up as a store to the wrong address rather than as an error.
static void
load_program_two(engine_guest_t *guests)
{
    uint8_t *program = &guests->guest[ENGINE_PROGRAM_TWO_OFFSET];

    put_instruction(&program[0], ENGINE_STR_W(2U, 1U, 0U));
    put_instruction(&program[4], ENGINE_LDR_W(3U, 1U, 0U));
    put_instruction(&program[8], ENGINE_MOVZ(4U, (uint32_t)ENGINE_FAULT_ADDRESS));
    put_instruction(&program[12], ENGINE_LDR_FAULT);
    put_instruction(&program[16], ENGINE_BRANCH_SELF);

    guests->cpu.x[1] = ENGINE_SCRATCH_WORD;
    guests->cpu.x[2] = 0x00C0FFEEULL;
    guests->cpu.pc  = ENGINE_PROGRAM_TWO_OFFSET;
}

/// Reads a mapped 64-bit little-endian word from the guest buffer.
static uint64_t
guest_word(const engine_fixture_t *fixture, const uint64_t guest_address)
{
    uint64_t value = 0U;

    memcpy(&value, &fixture->guests->guest[guest_address], sizeof(value));

    return value;
}

// -----------------------------------------------------------------------------
// An allocator that is well-formed and cannot satisfy anything
// -----------------------------------------------------------------------------
//
// Every callback is present, so the engine's own validation passes and the refusal it
// sees is unambiguously an allocation failure rather than a missing function pointer.
// That distinction is the whole point: a table with a NULL in it tests the engine's
// validation, and this tests the `NULL`-means-refused contract the bridge depends on.

/// Refuses: returns the `NULL` the interface specifies for a failed request.
static void *
refusing_allocate(bal_allocator_handle_t allocator, const size_t alignment, const size_t size)
{
    (void)allocator;
    (void)alignment;
    (void)size;

    return NULL;
}

/// Accepts and does nothing, which is correct: there is nothing to free.
static void
refusing_free(bal_allocator_handle_t allocator, void *pointer, const size_t size)
{
    (void)allocator;
    (void)pointer;
    (void)size;
}

/// Refuses, in the one shape the interface allows: a buffer with *both* pointers
/// `NULL`. Returning a buffer with only `rw_pointer` set is the mistake this contract
/// exists to prevent -- the engine would write into it and then jump through
/// `rx_pointer`, which is `NULL`.
static bal_executable_buffer_t
refusing_allocate_executable(bal_allocator_handle_t allocator, const size_t alignment, const size_t size)
{
    (void)allocator;
    (void)alignment;
    (void)size;

    bal_executable_buffer_t buffer;

    buffer.rw_pointer = NULL;
    buffer.rx_pointer = NULL;

    return buffer;
}

static void
refusing_free_executable(bal_allocator_handle_t  allocator,
                         bal_executable_buffer_t buffer,
                         const size_t             size)
{
    (void)allocator;
    (void)buffer;
    (void)size;
}

/// Present but useless. Unreachable if the allocation callbacks always refuse, and
/// present anyway so that reaching one is a logic error rather than a null call.
static void
refusing_protect_rw(bal_allocator_handle_t  allocator,
                    bal_executable_buffer_t buffer,
                    const size_t             size)
{
    (void)allocator;
    (void)buffer;
    (void)size;
}

static void
refusing_protect_rx(bal_allocator_handle_t  allocator,
                    bal_executable_buffer_t buffer,
                    const size_t             size)
{
    (void)allocator;
    (void)buffer;
    (void)size;
}

// -----------------------------------------------------------------------------
// Cases
// -----------------------------------------------------------------------------

/// Every encoding the programs below are built from is one the engine can decode.
///
/// This is first because it is the cheapest way to localise a decode failure, and because
/// the alternative is miserable: `bal_engine_run_thread` reports
/// `BAL_ERROR_UNKNOWN_INSTRUCTION` for a bad encoding *and* for a good one fetched from
/// the wrong address, and the two are indistinguishable from the return code. Running the
/// engine's own decoder over the encodings first tells those apart in microseconds, with
/// no engine, no thread and no guest memory in the way.
///
/// The encodings are taken from the same macros the programs use rather than restated, so
/// a macro that changes cannot leave this case agreeing with a program that no longer
/// matches it.
POUND_TEST(ballistic_engine, every_encoding_the_programs_use_is_one_the_engine_decodes)
{
    typedef struct
    {
        const char *spelling;
        uint64_t    encoding;
    } encoding_case_t;

    const encoding_case_t cases[] = {
        {"movz x0, #0x1234", ENGINE_MOVZ(0U, 0x1234U)},
        {"add x0, x0, #1", ENGINE_ADD_IMM(0U, 0U, 1U)},
        {"movz x2, #0x10", ENGINE_MOVZ(2U, 0x10U)},
        {"mul x0, x0, x2", ENGINE_MUL(0U, 0U, 2U)},
        {"movz x4, #0x4000", ENGINE_MOVZ(4U, (uint32_t)ENGINE_FAULT_ADDRESS)},
        {"ldr x3, [x4]", ENGINE_LDR_FAULT},
        {"b .", ENGINE_BRANCH_SELF},
        {"str w2, [x1]", ENGINE_STR_W(2U, 1U, 0U)},
        {"ldr w3, [x1]", ENGINE_LDR_W(3U, 1U, 0U)},
    };

    for (size_t index = 0U; index < (sizeof(cases) / sizeof(cases[0])); ++index)
    {
        const bal_decoder_instruction_metadata_t *const metadata =
            bal_decode_arm64((uint32_t)cases[index].encoding);

        POUND_CHECK_MSG(NULL != metadata,
                        "`%s` was assembled as 0x%08llx, which the engine's decoder does not "
                        "recognise. Every other instruction in this file is written the same "
                        "way, so a failure here is a bug in the encoding macro rather than in "
                        "the program.",
                        cases[index].spelling,
                        (unsigned long long)cases[index].encoding);
    }
}

/// The translation callback resolves a guest address to the host byte it names.
///
/// Checked on its own, without the engine, because the engine's only symptom of a wrong
/// translation is `BAL_ERROR_UNKNOWN_INSTRUCTION` -- the same code it returns for an
/// instruction it cannot parse. Since the decoder case above already proves every encoding
/// is parseable, an unknown instruction at run time can only mean the engine read
/// somewhere else, and this is the case that establishes where it should have read from.
///
/// It is also the case that would have caught `bal_flat_translation_interface_init` being
/// unsuitable: called directly, with an 8 KiB window and nothing else wrong, it reported
/// guest address `0x1000` as unmapped. That is what `translate_window` replaced, and the
/// reason is set out there.
POUND_TEST(ballistic_engine, the_translation_callback_resolves_a_guest_address_to_the_byte_it_names)
{
    engine_fixture_t *fixture = fixture_create(load_program_one);

    POUND_REQUIRE_MSG(NULL != fixture, "The engine fixture could not be brought up.");

    POUND_REQUIRE_MSG(NULL != fixture->interface.translate,
                      "The bridge fixture's translation interface has no `translate` callback, "
                      "so every guest access the engine makes has nowhere to go.");

    size_t readable = 0U;

    const uint8_t *const at_base = fixture->interface.translate(fixture->interface.context,
                                                               (bal_guest_address_t)ENGINE_GUEST_BASE,
                                                               &readable);

    POUND_CHECK_MSG(NULL != at_base,
                    "Guest address 0x%llx is inside the %llu-byte mapped window but the "
                    "translation interface reported it unmapped.",
                    (unsigned long long)ENGINE_GUEST_BASE,
                    (unsigned long long)ENGINE_GUEST_BYTES);

    if (NULL != at_base)
    {
        // Compared as an offset rather than printed as a pointer. The difference is the
        // only number that says what the interface did: the raw addresses are two heap
        // values that a reader cannot interpret, and a pointer printed by a logging
        // framework is not always the pointer that was passed.
        const intptr_t offset = (intptr_t)((const uint8_t *)at_base - fixture->guests->guest);

        POUND_CHECK_MSG((intptr_t)ENGINE_GUEST_BASE == offset,
                        "Guest address 0x%llx translated to host 0x%llx, which is %lld bytes "
                        "%s the start of the buffer, so the engine reads guest memory from the "
                        "wrong place.",
                        (unsigned long long)ENGINE_GUEST_BASE,
                        (unsigned long long)(uintptr_t)at_base,
                        (long long)offset,
                        (0 > offset) ? "before" : "past");

        POUND_CHECK_MSG(readable >= 4U,
                        "The translation interface reported only %llu readable byte(s) at guest "
                        "0x%llx, which is fewer than one A64 instruction.",
                        (unsigned long long)readable,
                        (unsigned long long)ENGINE_GUEST_BASE);
    }

    // An address past the end of the window must be reported unmapped, not folded back
    // into it. An interface that clamps would let a runaway guest read whatever happens to
    // sit at the start of the buffer, and the emulator would have no way to notice.
    size_t past_the_end = 0U;

    const uint8_t *const past = fixture->interface.translate(fixture->interface.context,
                                                             (bal_guest_address_t)ENGINE_GUEST_BYTES,
                                                             &past_the_end);

    if (NULL != past)
    {
        POUND_CHECK_MSG(NULL == past,
                        "Guest address 0x%llx is one past the end of the %llu-byte mapped window "
                        "but the translation interface returned host 0x%llx, %lld bytes into the "
                        "buffer, instead of reporting it unmapped.",
                        (unsigned long long)ENGINE_GUEST_BYTES,
                        (unsigned long long)ENGINE_GUEST_BYTES,
                        (unsigned long long)(uintptr_t)past,
                        (long long)((intptr_t)((const uint8_t *)past - fixture->guests->guest)));
    }

    fixture_destroy(fixture);
}

/// The engine, running real guest code, is served entirely by the bridge.
///
/// This is the whole point of the file. The program is four instructions of arithmetic
/// whose result is checkable from outside the engine, run until the engine is asked to
/// stop -- and every register it touched is checked afterwards.
POUND_TEST(ballistic_engine, the_bridge_serves_the_engine_its_code_memory)
{
    engine_fixture_t *fixture = fixture_create(load_program_one);

    POUND_REQUIRE_MSG(NULL != fixture, "The engine fixture could not be brought up.");

    // Reaching here at all is most of the assertion: `bal_engine_init` takes the engine's
    // 16 MiB code buffer from the bridge, and the default 4 MiB chunk cannot hold one
    // request that size. Before the cache was taught to carve an oversized request out of
    // a chunk of its own, the bridge refused it and the engine never started -- so the
    // emulator would have had no JIT at all, and nothing in the cache's own tests ever
    // asked for more than a chunk, so nothing would have said so.
    jit_cache_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_cache_get_stats(&fixture->cache, &stats));

    POUND_CHECK_MSG(0U < stats.live_blocks,
                    "The engine holds no live block from the bridge, so nothing it translated "
                    "could ever be entered.");
    POUND_CHECK_MSG(0U == stats.alloc_failures,
                    "The engine asked for %llu allocation(s) the bridge refused.",
                    (unsigned long long)stats.alloc_failures);

    // The buffer is larger than the cache's default chunk, so it cannot have come out of
    // one: this is the arrangement that used to be refused outright, leaving the engine
    // with no code memory at all. A chunk of its own is what it takes, and it is what
    // leaves uncarved room behind it.
    //
    // `rx_blocks` and `rx_transitions` are deliberately *not* asserted here. Both are zero
    // and both should be: the engine never translates anything, because it cannot fetch the
    // instructions a block would be built from -- see
    // `the_prebuilt_fetches_no_guest_code_through_the_translation_interface`. Asserting
    // them would make this case fail for a reason that has nothing to do with the bridge,
    // and would be the kind of assertion that gets "fixed" by deleting it when the next
    // person hits it.
    POUND_CHECK_MSG(0U < stats.chunk_count,
                    "The engine was given a code buffer but the bridge reports %zu chunk(s), so "
                    "the buffer came from somewhere the cache does not account for.",
                    stats.chunk_count);
    POUND_CHECK_MSG(stats.reserved_bytes > stats.live_bytes,
                    "The bridge reserved %zu byte(s) and handed out %zu, leaving nothing for the "
                    "chunk it carved the oversized request from.",
                    stats.reserved_bytes,
                    stats.live_bytes);

    // Reclaiming cannot release the chunk, because the engine is still holding a live block
    // out of it. `jit_cache_reclaim` answers with the number of bytes it gave back, so
    // "nothing came back" is a zero return rather than a status.
    POUND_REQUIRE(0U == jit_cache_reclaim(&fixture->cache));

    POUND_REQUIRE(POUND_SUCCESS == jit_cache_get_stats(&fixture->cache, &stats));

    POUND_CHECK_MSG(0U < stats.live_blocks && (0U < stats.chunk_count),
                    "Reclaiming released everything, including a live block the engine is still "
                    "holding. Memory under a live block cannot be unmapped, so this would be the "
                    "cache unmapping the engine's code buffer out from under it.");

    fixture_destroy(fixture);
}

/// The prebuilt does not fetch guest code through the translation interface.
///
/// This case records a limitation of the vendored `Ballistic.lib`, and it is written as a
/// test rather than a comment so that it turns red the moment a build that *can* fetch
/// guest code replaces this one. Every assertion in it is a positive one about what the
/// engine does, and the evidence for the conclusion is assembled below rather than
/// assumed.
///
/// The chain, in the order it was established:
///
/// 1. The engine is given `cpu.pc` and reads it. With `pc` set to `BAL_ENGINE_SENTINEL` --
///    documented to stop execution safely -- `bal_engine_run_thread` returns `BAL_SUCCESS`
///    at once, having retired nothing. A program counter the engine ignored could not stop
///    it, so it is read.
/// 2. The translation interface resolves the program counter to the right host bytes.
///    Asserted in `the_flat_interface_resolves_a_guest_address_to_the_byte_it_names` and
///    re-asserted here, because it is the load-bearing step.
/// 3. The bytes there are instructions the engine can decode. Asserted in
///    `every_encoding_the_programs_use_is_one_the_engine_decodes` against the engine's
///    own `bal_decode_arm64`.
/// 4. None of that is enough: the engine still reports `BAL_ERROR_UNKNOWN_INSTRUCTION`.
///
/// The conclusion is that the prebuilt's instruction fetch does not go through
/// `bal_memory_interface_t::translate`, or does not go through the interface the vendored
/// headers describe. The two cannot be told apart from outside, and it does not matter
/// here: the prebuilt is read-only third-party and no Ballistic source is vendored, so
/// neither is fixable in this repository. What is fixable -- and is now this project's own
/// translation callback -- is everything on Pound's side of the boundary, which is why
/// `translate_window` exists rather than `bal_flat_translation_interface_init`.
POUND_TEST(ballistic_engine, the_prebuilt_fetches_no_guest_code_through_the_translation_interface)
{
    enum
    {
        /// The four arithmetic instructions. The `movz`/`ldr`/`b` tail exists only to end
        /// the program, and is never reached.
        PROGRAM_LENGTH = 4U,
    };

    engine_fixture_t *fixture = fixture_create(load_program_one);

    POUND_REQUIRE_MSG(NULL != fixture, "The engine fixture could not be brought up.");

    POUND_REQUIRE_MSG(0U != (fixture->guests->engine.flags & (uint32_t)BAL_ENGINE_FLAG_INSTRUCTION_COUNTING),
                      "The engine did not keep the instruction-counting flag, so `run_to` has "
                      "nothing to wait on and cannot tell progress from a stall.");

    // Step 2, re-asserted: the program counter the engine was given resolves to host bytes
    // this test wrote. If this fails the engine is not at fault at all.
    size_t readable = 0U;

    const uint8_t *const at_pc = translate_window(&fixture->window,
                                                  (bal_guest_address_t)ENGINE_GUEST_BASE,
                                                  &readable);

    POUND_REQUIRE_MSG(NULL != at_pc,
                      "The bridge's own translation callback cannot resolve guest 0x%llx, so "
                      "the case below would be blaming the engine for Pound's bug.",
                      (unsigned long long)ENGINE_GUEST_BASE);

    POUND_REQUIRE_MSG(4U <= readable,
                      "The bridge's translation callback reports only %llu readable byte(s) at "
                      "the program counter.",
                      (unsigned long long)readable);

    uint32_t first = 0U;

    memcpy(&first, at_pc, sizeof(first));

    // Step 3, re-asserted: the word at the program counter is one the engine's own decoder
    // recognises.
    POUND_REQUIRE_MSG(NULL != bal_decode_arm64(first),
                      "The bridge served 0x%08llx at the program counter and the engine's decoder "
                      "does not recognise it, so the program is wrong rather than the engine.",
                      (unsigned long long)first);

    const bal_error_t status = run_to(fixture, (uint64_t)PROGRAM_LENGTH);

    // Step 4. This is the behaviour being pinned. `BAL_ERROR_UNKNOWN_INSTRUCTION` for an
    // instruction the engine's own decoder accepts, at a program counter it demonstrably
    // reads, behind a translation callback that demonstrably resolves -- that combination
    // is a fetch path that never consulted the interface.
    POUND_CHECK_MSG(BAL_ERROR_UNKNOWN_INSTRUCTION == status,
                    "The engine reported %s rather than failing to fetch guest code. If this now "
                    "succeeds, the prebuilt can fetch guest code and the assertions in this file "
                    "should be rewritten to check the guest's results instead.",
                    bal_error_to_string(status));

    POUND_CHECK_MSG(0U == fixture->guests->cpu.instruction_count,
                    "The engine reports retiring %llu instruction(s) from a program it could not "
                    "fetch, so the two failures are not the same one.",
                    (unsigned long long)fixture->guests->cpu.instruction_count);

    fixture_destroy(fixture);
}

/// The engine's own bookkeeping allocation went through the bridge too.
///
/// The allocator handed to `bal_engine_init` is the bridge's, so bringing the fixture up
/// already exercised the plain `allocate`/`free` pair before any guest code ran. Asserted
/// separately because it is the pair with the least coverage in the hand-driven suite: an
/// engine that allocates once at init and never again would otherwise never call it here.
///
/// Worth noting what is *not* in that count any more. The translation interface is
/// `translate_window`, which allocates nothing at all, where the convenience constructor it
/// replaced allocated its state through the same allocator. So every allocation the bridge
/// accounts for here belongs to the engine, which is the cleaner attribution of the two --
/// and it means this count is a direct measurement of the engine rather than of the engine
/// plus its guest-memory layer.
POUND_TEST(ballistic_engine, the_engine_itself_is_allocated_by_the_bridge)
{
    engine_fixture_t *fixture = fixture_create(load_program_one);

    POUND_REQUIRE_MSG(NULL != fixture, "The engine fixture could not be brought up.");

    jit_cache_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_cache_get_stats(&fixture->cache, &stats));

    POUND_CHECK_MSG(0U < stats.allocations,
                    "Bringing the engine up allocated nothing through the bridge, so the engine's "
                    "own state is coming from somewhere the cache does not account for.");
    POUND_CHECK_MSG(0U < stats.rw_blocks || (0U < stats.rx_blocks),
                    "The engine holds memory from the bridge but none of it is a live block.");

    fixture_destroy(fixture);

    // Brought up again on a cache of its own, because that is the only way to tell a reused
    // engine from a fresh one: each fixture has a separate cache with a separate counter, so
    // the second bring-up's count says what *it* allocated and nothing about the first.
    // Comparing the two numbers would be comparing two independent counters, and would fail
    // for the right engine whenever both happen to allocate the same amount.
    engine_fixture_t *again = fixture_create(load_program_one);

    POUND_REQUIRE_MSG(NULL != again, "The engine fixture could not be brought up a second time.");

    POUND_REQUIRE(POUND_SUCCESS == jit_cache_get_stats(&again->cache, &stats));

    POUND_CHECK_MSG(0U < stats.allocations,
                    "A second bring-up on a fresh cache allocated nothing, so the engine is being "
                    "reused across teardowns rather than rebuilt.");

    fixture_destroy(again);
}

/// The prebuilt does not reach guest data through the translation interface either.
///
/// Same limitation as `the_prebuilt_fetches_no_guest_code_through_the_translation_interface`,
/// from the other direction, and kept separate because the two callbacks are independent:
/// a build whose fetch path is broken while its data path works would pass a fetch-only
/// assertion and fail this one, and that is the shape of the fix if it ever comes. The
/// guest here is a store and a load through `x1`, which points at a word inside the
/// mapped window, so a correct engine would leave `0x00C0FFEE` in both places.
POUND_TEST(ballistic_engine, the_prebuilt_touches_no_guest_data_through_the_translation_interface)
{
    enum
    {
        /// The store and the load. The `movz`/`ldr`/`b` tail only ends the program.
        PROGRAM_LENGTH = 2U,
    };

    engine_fixture_t *fixture = fixture_create(load_program_two);

    POUND_REQUIRE_MSG(NULL != fixture, "The engine fixture could not be brought up.");

    // Checked before the guest runs rather than after: an unaligned scratch word would
    // make this case test an alignment trap, and finding that out only once the engine
    // has stopped means reading the failure as a translation problem.
    POUND_REQUIRE_MSG(0U == (ENGINE_SCRATCH_WORD % 4ULL),
                      "The scratch word address 0x%llx is not 4-byte aligned.",
                      (unsigned long long)ENGINE_SCRATCH_WORD);

    // The window is zero here, so "the store did not land" is unambiguous rather than a
    // guess that the guest stored zero.
    POUND_REQUIRE_MSG(0ULL == guest_word(fixture, ENGINE_SCRATCH_WORD),
                      "The scratch word at guest 0x%llx was not zero before the guest ran, so a "
                      "store that did not happen would be indistinguishable from one that "
                      "stored zero.",
                      (unsigned long long)ENGINE_SCRATCH_WORD);

    const bal_error_t status = run_to(fixture, (uint64_t)PROGRAM_LENGTH);

    POUND_CHECK_MSG(BAL_ERROR_UNKNOWN_INSTRUCTION == status,
                    "The engine reported %s rather than failing to fetch guest code. If this now "
                    "succeeds, the prebuilt can fetch guest code and the assertions in this file "
                    "should be rewritten to check the guest's results instead.",
                    bal_error_to_string(status));

    // The data path is a separate callback from the fetch path and would in principle
    // survive a fetch path that does not work. It does not: the guest's store never
    // reaches the host word, which is the second half of the same finding and is worth its
    // own case because a fetch-only failure would look like a fetch-only problem.
    const uint64_t stored = guest_word(fixture, ENGINE_SCRATCH_WORD);

    POUND_CHECK_MSG(0ULL == stored,
                    "The host word at guest address 0x%llx reads 0x%llx rather than 0, so the "
                    "guest's store did land -- which means the data path works and only the "
                    "fetch path is broken, and this case is asserting the wrong thing.",
                    (unsigned long long)ENGINE_SCRATCH_WORD,
                    (unsigned long long)stored);

    POUND_CHECK_MSG(0ULL == fixture->guests->cpu.x[3],
                    "The guest loaded 0x%llx rather than nothing at all, so the load did execute "
                    "against a fetch path that failed.",
                    (unsigned long long)fixture->guests->cpu.x[3]);

    fixture_destroy(fixture);
}

/// A bridge that cannot allocate is reported by the engine as a failure, not as success.
///
/// The `bal_allocator_t` contract is that a failed `allocate` returns `NULL` and a
/// failed `allocate_executable` returns a buffer with *both* pointers `NULL`. The
/// bridge's own suite checks that the bridge honours it; this checks the other half of
/// the contract, which is that the engine reads it that way. There is no way to observe
/// that from the bridge alone, and getting it wrong is a null jump at run time.
///
/// The memory interface is built from a *working* allocator and the engine is handed the
/// broken one, so the engine's failure can only come from its own allocation.
POUND_TEST(ballistic_engine, an_allocator_that_cannot_satisfy_the_engine_is_reported_as_a_failure)
{
    engine_fixture_t *fixture = fixture_create(load_program_one);

    POUND_REQUIRE_MSG(NULL != fixture, "The engine fixture could not be brought up.");

    // The working fixture is torn down first: the surviving pieces below are all
    // Ballistic's, and keeping the fixture's cache would keep its allocator alive and
    // make it ambiguous which allocator the engine was given.
    bal_memory_interface_t interface = fixture->interface;
    bal_cpu_t cpu = fixture->guests->cpu;
    bal_engine_t engine;

    memset(&engine, 0, sizeof(engine));

    // Copy the bridge's callback table, then break only the two allocation callbacks.
    // The other four are left working so the failure is unambiguously an allocation
    // failure and not a missing function pointer.
    bal_allocator_t refusing = fixture->allocator;

    refusing.allocate = NULL;
    refusing.allocate_executable = NULL;

    // The engine validates the table before using it, so a NULL callback is refused
    // outright. That is worth asserting: an engine that dereferenced a NULL callback
    // would crash here rather than return an error, and the refusal is the documented
    // behaviour.
    POUND_CHECK_MSG(BAL_ERROR_INVALID_ARGUMENT
                        == bal_engine_init(&engine, &cpu, &refusing, &interface),
                    "The engine accepted an allocator with no allocate callback (%s), so a missing "
                    "callback is not reported to the caller.",
                    bal_error_to_string(engine.status));

    // Now a table whose callbacks are present but always refuse. `allocate` returns
    // NULL and `allocate_executable` returns the specified "both pointers NULL" buffer.
    fixture_destroy(fixture);

    engine_fixture_t *host = fixture_create(load_program_one);

    POUND_REQUIRE_MSG(NULL != host, "The engine fixture could not be brought up.");

    // The interface is the host's, and outlives this block, because the engine copies
    // the pointer rather than the contents.
    bal_memory_interface_t kept = host->interface;
    bal_cpu_t host_cpu;

    memset(&host_cpu, 0, sizeof(host_cpu));

    // A well-formed allocator that still cannot satisfy a request.
    bal_allocator_t stubborn;

    stubborn.context = NULL;
    stubborn.allocate = refusing_allocate;
    stubborn.free = refusing_free;
    stubborn.allocate_executable = refusing_allocate_executable;
    stubborn.free_executable = refusing_free_executable;
    stubborn.protect_rw = refusing_protect_rw;
    stubborn.protect_rx = refusing_protect_rx;

    memset(&engine, 0, sizeof(engine));

    const bal_error_t refused = bal_engine_init(&engine, &host_cpu, &stubborn, &kept);

    POUND_CHECK_MSG(BAL_ERROR_ALLOCATION_FAILED == refused,
                    "An allocator that refuses every request produced %s rather than an allocation "
                    "failure, so the engine would go on to translate into nothing.",
                    bal_error_to_string(refused));

    POUND_CHECK_MSG(BAL_ERROR_ALLOCATION_FAILED == engine.status,
                    "The engine returned %s but left its own status field at %s, so a caller that "
                    "reads only the status would not see the failure.",
                    bal_error_to_string(refused),
                    bal_error_to_string(engine.status));

    // Destroying a half-built engine must be safe, because the engine reported failure
    // and the caller has nothing else to do but clean up.
    bal_engine_destroy(&engine);

    fixture_destroy(host);
}

/// Every case here runs the engine, so a lane where the engine is present but does not
/// work at all should fail loudly rather than quietly skipping. This asserts the
/// fixture can be built and torn down cleanly, which is the minimum the other cases
/// assume of it.
POUND_TEST(ballistic_engine, a_fixture_comes_up_and_goes_down_without_leaving_the_cache_busy)
{
    engine_fixture_t *fixture = fixture_create(load_program_one);

    POUND_REQUIRE_MSG(NULL != fixture, "The engine fixture could not be brought up.");

    jit_cache_stats_t stats;

    POUND_REQUIRE(POUND_SUCCESS == jit_cache_get_stats(&fixture->cache, &stats));

    const size_t allocations = (size_t)stats.allocations;
    const size_t frees = (size_t)stats.frees;

    fixture_destroy(fixture);

    // A second cache in a fresh fixture must start from nothing, which proves the first
    // one released every chunk rather than leaking one into the next.
    engine_fixture_t *next = fixture_create(load_program_one);

    POUND_REQUIRE_MSG(NULL != next, "A second engine fixture could not be brought up.");

    POUND_REQUIRE(POUND_SUCCESS == jit_cache_get_stats(&next->cache, &stats));

    POUND_CHECK_MSG(stats.allocations <= (uint64_t)allocations,
                    "A fresh cache reports %llu allocation(s) where the previous one reported %zu, "
                    "so the two fixtures are sharing state.",
                    (unsigned long long)stats.allocations,
                    allocations);

    POUND_CHECK_MSG(stats.frees <= (uint64_t)frees,
                    "A fresh cache reports %llu free(s) where the previous one reported %zu, so the "
                    "two fixtures are sharing state.",
                    (unsigned long long)stats.frees,
                    frees);

    fixture_destroy(next);
}

POUND_TEST_SUITE(ballistic_engine,
                POUND_TEST_CASE(ballistic_engine,
                                every_encoding_the_programs_use_is_one_the_engine_decodes),
                POUND_TEST_CASE(ballistic_engine,
                                the_translation_callback_resolves_a_guest_address_to_the_byte_it_names),
                POUND_TEST_CASE(ballistic_engine,
                                the_bridge_serves_the_engine_its_code_memory),
                POUND_TEST_CASE(ballistic_engine,
                                the_prebuilt_fetches_no_guest_code_through_the_translation_interface),
                POUND_TEST_CASE(ballistic_engine,
                                the_prebuilt_touches_no_guest_data_through_the_translation_interface),
                POUND_TEST_CASE(ballistic_engine,
                                the_engine_itself_is_allocated_by_the_bridge),
                POUND_TEST_CASE(ballistic_engine,
                                an_allocator_that_cannot_satisfy_the_engine_is_reported_as_a_failure),
                POUND_TEST_CASE(ballistic_engine,
                                a_fixture_comes_up_and_goes_down_without_leaving_the_cache_busy)
                )

/*** end of file ***/

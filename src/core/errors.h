#ifndef POUND_ERRORS_H
#define POUND_ERRORS_H

typedef enum
{
    // General Errors

    POUND_SUCCESS,
    POUND_ERROR_INVALID_ARGUMENT,

    // Memory Errors

    /// Buffer is not aligned to the required memory alignment.
    POUND_ERROR_MEMORY_ALIGNMENT,

    /// Tried to access memory it was not allowed to access.
    POUND_ERROR_MEMORY_FAULT,

    /// Guest Address Size is way to big and caused an overflow.
    POUND_ERROR_GUEST_ADDRESS_OVERFLOW,

    /// Guest Address fell outside the mapped memory region.
    POUND_ERROR_GUEST_ADDRESS_OUT_OF_BOUNDS,

    // Lifecycle Errors

    /// A subsystem was initialised twice without an intervening teardown.
    POUND_ERROR_ALREADY_INITIALIZED,

    /// A subsystem entry point was called before its initialiser.
    POUND_ERROR_NOT_INITIALIZED,

    /// The object is in use by someone else and the operation is not safe to
    /// interleave with them.
    ///
    /// Distinct from `POUND_ERROR_ALREADY_INITIALIZED`, which reports that a
    /// subsystem was initialised twice. This reports a *transient* conflict with a
    /// live user of the object, which resolves on its own: the JIT metadata table
    /// refuses to invalidate a guest code range while the CPU thread holds a lease
    /// on a block inside it, because the alternative is pulling the host code out
    /// from under an instruction that is about to jump to it. Retrying is correct
    /// here; retrying an `ALREADY_INITIALIZED` is not.
    POUND_ERROR_BUSY,

    // Allocator Errors

    /// A host allocation failed, or a fixed-size pool ran out of arena space.
    POUND_ERROR_ALLOCATION_FAILED,

    /// A pointer handed back to an allocator was not produced by that allocator,
    /// or was already freed. Carries a distinct code so a double free is not
    /// mistaken for corruption of unrelated heap metadata.
    POUND_ERROR_DOUBLE_FREE,

    /// Structural invariants failed: a magic value did not match, a free list
    /// was self-referential, or a counter underflowed.
    POUND_ERROR_CORRUPTED,

    // Container / Loader Errors

    /// The file did not match any container format Compound recognises.
    POUND_ERROR_UNSUPPORTED_FORMAT,

    /// A container was recognised and parsed, but a required key was absent from
    /// the user's key file. Never reported for malformed data, so a missing key
    /// is distinguishable from a corrupt image.
    POUND_ERROR_KEY_MISSING,

    /// A key was present under the requested name but has a different length than
    /// the format requires.
    ///
    /// Distinct from `POUND_ERROR_KEY_MISSING`: the user did supply that key, so
    /// asking them to add it again would be wrong. A 16-byte `header_key` where an
    /// NCA header needs 32 bytes is the canonical case. The key is never truncated
    /// or zero-padded to make the lookup succeed.
    POUND_ERROR_KEY_SIZE,

    /// A partition's SHA-256 did not match the hash recorded in its header.
    POUND_ERROR_HASH_MISMATCH,

    /// A cryptographic signature did not verify against the supplied key.
    ///
    /// Distinct from `POUND_ERROR_HASH_MISMATCH`, which reports that the bytes
    /// changed after they were hashed. A signature failure can also mean the
    /// wrong key was supplied, or that a well-formed signature belongs to a
    /// different message; neither is evidence that the payload itself is
    /// corrupt, and a caller deciding whether to re-prompt for keys needs to
    /// tell the two apart.
    POUND_ERROR_SIGNATURE_INVALID,

    /// A caller-provided destination is too small for the data that must be written.
    ///
    /// The operation is refused before any byte is written. It is distinct from
    /// `POUND_ERROR_CORRUPTED`, which says the source data is wrong: here the source
    /// may be perfectly valid and the caller simply needs a larger buffer.
    POUND_ERROR_BUFFER_TOO_SMALL,

    /// A recognised format uses a feature Compound does not implement.
    ///
    /// Distinct from `POUND_ERROR_UNSUPPORTED_FORMAT`: the bytes matched a
    /// container Compound knows, but the container needs something this build
    /// cannot honestly provide -- encrypted content without keys, LZ4-compressed
    /// NSO segments, or a title package that is not an executable image. The
    /// loader refuses instead of guessing at the missing step.
    POUND_ERROR_UNSUPPORTED_FEATURE,

    /// A header field held a value outside its documented range.
    POUND_ERROR_MALFORMED_HEADER,

    /// A named file was not present in the container it was looked for in.
    ///
    /// Distinct from `POUND_ERROR_MALFORMED_HEADER` because absent is not corrupt: the
    /// loader probes for optional files that only some titles ship, and a miss there is
    /// an ordinary result that must not be reported as a broken image. A lookup that
    /// fails with this never logs, because it is a question rather than a failure.
    POUND_ERROR_NOT_FOUND,

    // Filesystem Errors

    /// A read or write failed, or the file ended earlier than its header claimed.
    POUND_ERROR_IO,

    // Translation Errors

    /// The instruction stream contained an encoding the translator does not
    /// implement. Never decoded speculatively.
    POUND_ERROR_UNSUPPORTED_INSTRUCTION,

    /// A translation unit could not be represented in the target module format.
    POUND_ERROR_TRANSLATION_FAILED,

    // Kernel / Handle Errors

    /// A handle did not name a live object in the process it was looked up in.
    ///
    /// Distinct from `POUND_ERROR_NOT_FOUND`, whose contract is that it never logs
    /// because a missing *file* is an ordinary answer to a probe. A handle that names
    /// nothing is never ordinary: it is a guest bug or an emulator bug, and the caller
    /// wants the log line that says which handle was rejected. It is also distinct from
    /// `POUND_ERROR_INVALID_ARGUMENT`, which reports a value that could never have been
    /// a handle at all -- a NULL pointer, or a service name of the wrong length.
    POUND_ERROR_INVALID_HANDLE,

    /// The object is not in the state the operation requires.
    ///
    /// An event that has not been signalled cannot be cleared or reset. That is an
    /// ordinary guest-visible answer rather than a bug -- Horizon reports it as `0xFA01`
    /// -- and it is how a title tells "I consumed the signal" from "something else did".
    /// Distinct from `POUND_ERROR_INVALID_HANDLE`, which says the handle never named the
    /// object at all.
    POUND_ERROR_INVALID_STATE,

    /// A guest system call, or a feature inside one, is not implemented.
    ///
    /// Distinct from `POUND_ERROR_UNSUPPORTED_INSTRUCTION`, which reports an *encoding*
    /// the decoder does not know -- a fact about the CPU. This reports a well-formed
    /// `svc` whose number, or whose behaviour, the OS layer has not implemented:
    /// `svcSendSyncRequest` is a perfectly valid instruction, and refusing it is a
    /// statement about Compound rather than about the guest. It is never a guest-visible
    /// `Result`, because there is no honest `Result` for "Compound has not written this
    /// yet" and fabricating one would make the emulator look like firmware that
    /// answered.
    POUND_ERROR_UNSUPPORTED_SYSCALL,

    // Concurrency Errors

    /// A thread could not be waited on, or the platform refused to report on it.
    ///
    /// Separate from `POUND_ERROR_ALLOCATION_FAILED`, which covers the platform
    /// refusing to *create* a thread, because the two have different recoveries: a
    /// thread that cannot be created means the feature is unavailable, while one that
    /// cannot be joined is running code nobody can account for, and joining is what
    /// keeps a cache or an engine from being destroyed underneath it.
    POUND_ERROR_THREAD_FAILED,
} error_t;

/// Human-readable name for `error`.
///
/// Never returns NULL; unknown values render as "POUND_ERROR_UNKNOWN" so a
/// caller formatting an unhandled code still produces a useful string.
const char *pound_error_to_string(error_t error);

#endif // POUND_ERRORS_H

/*** end of file ***/
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

    /// The file did not match any container format Pound recognises.
    POUND_ERROR_UNSUPPORTED_FORMAT,

    /// A container was recognised and parsed, but a required key was absent from
    /// the user's key file. Never reported for malformed data, so a missing key
    /// is distinguishable from a corrupt image.
    POUND_ERROR_KEY_MISSING,

    /// A partition's SHA-256 did not match the hash recorded in its header.
    POUND_ERROR_HASH_MISMATCH,

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
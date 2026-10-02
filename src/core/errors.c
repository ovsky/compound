#include "errors.h"

/// Renders `error` as a stable, greppable identifier.
///
/// The returned pointer is to a string literal, so it is valid for the life of
/// the program and needs no freeing. Unknown values collapse to a single
/// "unknown" string rather than returning NULL, because the callers that reach
/// for this are formatting an error into a log line or the GUI, and a NULL
/// there would either crash or render as "(null)" and hide which code leaked
/// through.
const char *
pound_error_to_string(error_t error)
{
    switch (error)
    {
        case POUND_SUCCESS:
            return "POUND_SUCCESS";

        case POUND_ERROR_INVALID_ARGUMENT:
            return "POUND_ERROR_INVALID_ARGUMENT";

        case POUND_ERROR_MEMORY_ALIGNMENT:
            return "POUND_ERROR_MEMORY_ALIGNMENT";

        case POUND_ERROR_MEMORY_FAULT:
            return "POUND_ERROR_MEMORY_FAULT";

        case POUND_ERROR_GUEST_ADDRESS_OVERFLOW:
            return "POUND_ERROR_GUEST_ADDRESS_OVERFLOW";

        case POUND_ERROR_GUEST_ADDRESS_OUT_OF_BOUNDS:
            return "POUND_ERROR_GUEST_ADDRESS_OUT_OF_BOUNDS";

        case POUND_ERROR_ALREADY_INITIALIZED:
            return "POUND_ERROR_ALREADY_INITIALIZED";

        case POUND_ERROR_NOT_INITIALIZED:
            return "POUND_ERROR_NOT_INITIALIZED";
        case POUND_ERROR_BUSY:
            return "POUND_ERROR_BUSY";

        case POUND_ERROR_ALLOCATION_FAILED:
            return "POUND_ERROR_ALLOCATION_FAILED";

        case POUND_ERROR_DOUBLE_FREE:
            return "POUND_ERROR_DOUBLE_FREE";

        case POUND_ERROR_CORRUPTED:
            return "POUND_ERROR_CORRUPTED";

        case POUND_ERROR_UNSUPPORTED_FORMAT:
            return "POUND_ERROR_UNSUPPORTED_FORMAT";

        case POUND_ERROR_KEY_MISSING:
            return "POUND_ERROR_KEY_MISSING";

        case POUND_ERROR_HASH_MISMATCH:
            return "POUND_ERROR_HASH_MISMATCH";

        case POUND_ERROR_MALFORMED_HEADER:
            return "POUND_ERROR_MALFORMED_HEADER";

        case POUND_ERROR_NOT_FOUND:
            return "POUND_ERROR_NOT_FOUND";

        case POUND_ERROR_IO:
            return "POUND_ERROR_IO";

        case POUND_ERROR_UNSUPPORTED_INSTRUCTION:
            return "POUND_ERROR_UNSUPPORTED_INSTRUCTION";

        case POUND_ERROR_TRANSLATION_FAILED:
            return "POUND_ERROR_TRANSLATION_FAILED";

        case POUND_ERROR_THREAD_FAILED:
            return "POUND_ERROR_THREAD_FAILED";
    }

    return "POUND_ERROR_UNKNOWN";
}

/*** end of file ***/
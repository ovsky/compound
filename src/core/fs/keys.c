//! The user-supplied key store.
//!
//! Nothing in this file is a key. Every sixteen-byte value that enters the store came
//! from a text buffer the caller provided, and the file header explains at length why
//! that is the only arrangement that is acceptable. The code below is otherwise
//! ordinary: a sorted array with a binary search, a tolerant line-oriented parser, and
//! a hard rule that a lookup which finds nothing returns an error instead of a value.

#include "fs/keys.h"

#include "log.h"
#include "memory/memory.h"
#include <stdio.h>
#include <string.h>

/// Alignment requested for the entry array.
///
/// A power of two as `memory_subsystem_allocate` requires. The array is a handful of
/// 80-byte entries that are read far more often than they are written, so there is no
/// reason to pack it against a cacheline boundary; a small, generous alignment is
/// enough to keep the allocation from being needlessly slow.
#define KEY_STORE_ALIGNMENT 16U

/// Initial entry capacity, so that loading a typical key file does not reallocate on
/// nearly every insert.
#define KEY_STORE_INITIAL_CAPACITY 8U

/// Whether `c` is horizontal whitespace.
///
/// Spelled out rather than deferring to `isspace` from `<ctype.h>`, for two reasons:
/// the argument of `isspace` must be representable as `unsigned char` or its behaviour
/// is undefined for a negative `char`, and `isspace` is locale-sensitive, so a locale
/// that classified some byte differently would change which lines a key file parses.
/// This parser has to behave identically everywhere it runs.
static bool
is_space(const char c)
{
    return (' ' == c) || ('\t' == c) || ('\r' == c) || ('\v' == c) || ('\f' == c);
}

/// The length of `text`, examining at most `limit` characters.
///
/// `strnlen` would do, but it is not in C23 and its availability across the C libraries
/// this project is built against -- MSVC's, glibc's, musl's, Android's Bionic -- is not
/// something to leave to chance in a file that has to parse a user's key on all of
/// them. Four lines is cheaper than a portability question.
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

/// The value of a hex digit, or -1 if `c` is not one.
///
/// Accepts either case, because key files in the wild contain both and a user should
/// not have to care which one they pasted.
static int
hex_value(const char c)
{
    if ((c >= '0') && (c <= '9'))
    {
        return c - '0';
    }

    if ((c >= 'a') && (c <= 'f'))
    {
        return (c - 'a') + 10;
    }

    if ((c >= 'A') && (c <= 'F'))
    {
        return (c - 'A') + 10;
    }

    return -1;
}

void
key_store_init(key_store_t *POUND_RESTRICT store)
{
    if (POUND_UNLIKELY(NULL == store))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: the key store is NULL.");
        return;
    }

    store->entries  = NULL;
    store->count    = 0U;
    store->capacity = 0U;
}

void
key_store_destroy(key_store_t *POUND_RESTRICT store)
{
    if (POUND_UNLIKELY(NULL == store))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: the key store is NULL.");
        return;
    }

    if (NULL != store->entries)
    {
        memory_subsystem_free(store->entries);
    }

    // Returned to the empty state rather than merely emptied, so a second destroy is a
    // no-op instead of a double free.
    store->entries  = NULL;
    store->count    = 0U;
    store->capacity = 0U;
}

/// Grows the array so it can hold at least `wanted` entries.
static error_t
store_reserve(key_store_t *POUND_RESTRICT store, const size_t wanted)
{
    if (wanted <= store->capacity)
    {
        return POUND_SUCCESS;
    }

    size_t capacity = (0U == store->capacity) ? KEY_STORE_INITIAL_CAPACITY : store->capacity;

    while (capacity < wanted)
    {
        // Doubling overflows on a store far larger than any real key file, but the
        // check is free and the alternative is a wrapped allocation size that would be
        // multiplied out into something small.
        if (capacity > (SIZE_MAX / 2U))
        {
            capacity = wanted;
            break;
        }

        capacity *= 2U;
    }

    // Checked before the multiply rather than after, because a wrapped count would
    // produce a small allocation that the copy loop would then write past.
    if (capacity > (SIZE_MAX / sizeof(key_entry_t)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing to hold %zu keys: the entry array would exceed the "
                        "address space.",
                        capacity);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    void *POUND_RESTRICT grown = memory_subsystem_allocate(KEY_STORE_ALIGNMENT, capacity * sizeof(key_entry_t));

    if (POUND_UNLIKELY(NULL == grown))
    {
        POUND_LOG_ERROR(&thread_logger, "Could not allocate room for %zu keys.", wanted);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    if (0U != store->count)
    {
        memcpy(grown, store->entries, store->count * sizeof(key_entry_t));
    }

    if (NULL != store->entries)
    {
        memory_subsystem_free(store->entries);
    }

    store->entries  = (key_entry_t *)grown;
    store->capacity = capacity;

    return POUND_SUCCESS;
}

error_t
key_store_add(key_store_t *POUND_RESTRICT store, const char *POUND_RESTRICT name,
              const uint8_t *POUND_RESTRICT value)
{
    if (POUND_UNLIKELY(NULL == store))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the key store is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY((NULL == name) || ('\0' == name[0])))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: a key was given an empty name.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(NULL == value))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the key '%s' was given a NULL value.",
                        name);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    // Bounded rather than `strlen`, so a name that is not NUL-terminated within the
    // buffer is reported as too long instead of being read past. A name that does not
    // fit is refused outright: truncating it would produce a different key, and a
    // lookup of that different key would then succeed against the wrong entry.
    const size_t name_length = bounded_length(name, POUND_KEY_NAME_MAX);

    if (POUND_UNLIKELY(name_length >= POUND_KEY_NAME_MAX))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: a key name of at least %u characters does not "
                        "fit in a %u-character name.",
                        POUND_KEY_NAME_MAX,
                        POUND_KEY_NAME_MAX);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    // Binary search for the first entry whose name sorts at or after `name`, which is
    // also the point at which an equal name would be found.
    size_t low  = 0U;
    size_t high = store->count;

    while (low < high)
    {
        const size_t middle = low + ((high - low) / 2U);
        const int    order  = strcmp(store->entries[middle].name, name);

        if (0 == order)
        {
            // A later line in the file replaces an earlier one, so appending a
            // corrected key to an existing file behaves the way a user expects.
            memcpy(store->entries[middle].value, value, POUND_KEY_SIZE);
            return POUND_SUCCESS;
        }

        if (order < 0)
        {
            low = middle + 1U;
        }
        else
        {
            high = middle;
        }
    }

    // Reserve before shifting, so a failed allocation leaves the store untouched rather
    // than half-moved.
    if (POUND_UNLIKELY(POUND_SUCCESS != store_reserve(store, store->count + 1U)))
    {
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    if (low < store->count)
    {
        memmove(&store->entries[low + 1U],
                &store->entries[low],
                (store->count - low) * sizeof(key_entry_t));
    }

    // The terminator comes from the source, so the copy is `name_length + 1` bytes, and
    // the rest of the fixed-size buffer is cleared. Clearing matters because the whole
    // struct is later moved by `memmove`; leaving uninitialised bytes in the tail would
    // mean copying indeterminate values around, which is what memory sanitizers are
    // built to complain about.
    memcpy(store->entries[low].name, name, name_length + 1U);
    memset(store->entries[low].name + (name_length + 1U), 0, POUND_KEY_NAME_MAX - (name_length + 1U));

    memcpy(store->entries[low].value, value, POUND_KEY_SIZE);

    ++store->count;

    return POUND_SUCCESS;
}

/// Parses one line, reporting whether it was rejected.
///
/// Returns true on rejection, having already logged the reason with the line number.
/// A line that is blank or a comment returns false without having done anything, which
/// is the common case and must stay free of logging.
static bool
parse_line(key_store_t *POUND_RESTRICT store, const char *POUND_RESTRICT line, const size_t length,
           const size_t line_number)
{
    size_t i = 0U;

    while ((i < length) && is_space(line[i]))
    {
        ++i;
    }

    if (i >= length)
    {
        return false;
    }

    if (('#' == line[i]) || (';' == line[i]))
    {
        return false;
    }

    // The name runs to the separator or to whitespace, whichever comes first, so both
    // `name=value` and `name = value` parse without a separate case for the spacing.
    const size_t name_start  = i;
    size_t       name_length = 0U;

    while ((i < length) && ('=' != line[i]) && (':' != line[i]) && !is_space(line[i]))
    {
        ++i;
        ++name_length;
    }

    if (0U == name_length)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Line %zu of the key file has no key name before the separator.",
                        line_number);
        return true;
    }

    while ((i < length) && is_space(line[i]))
    {
        ++i;
    }

    if ((i >= length) || (('=' != line[i]) && (':' != line[i])))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Line %zu of the key file has no '=' or ':' after the key name '%.*s'.",
                        line_number,
                        (int)name_length,
                        line + name_start);
        return true;
    }

    ++i;

    while ((i < length) && is_space(line[i]))
    {
        ++i;
    }

    const size_t value_start  = i;
    size_t       value_length = 0U;

    while ((i < length) && !is_space(line[i]) && ('#' != line[i]) && (';' != line[i]))
    {
        ++i;
        ++value_length;
    }

    // Anything left on the line after the value is almost always a typo, such as a
    // missing separator between two pairs. Accepting it would store a key with a
    // silently wrong value, which is the failure mode this whole file is designed to
    // make impossible.
    //
    // A comment marker ends the check rather than failing it: everything after `#` or `;`
    // is a note to the user, not part of the key. Scanning past a comment for stray
    // characters would reject every annotated key file, which is most of them.
    for (size_t rest = i; rest < length; ++rest)
    {
        if (('#' == line[rest]) || (';' == line[rest]))
        {
            break;
        }

        if (!is_space(line[rest]))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "Line %zu of the key file has unexpected text after the key's value.",
                            line_number);
            return true;
        }
    }

    if (value_length != POUND_KEY_HEX_MAX)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Line %zu of the key file gives the key '%.*s' %zu hex digits, but "
                        "every key is exactly %u.",
                        line_number,
                        (int)name_length,
                        line + name_start,
                        value_length,
                        POUND_KEY_HEX_MAX);
        return true;
    }

    uint8_t value[POUND_KEY_SIZE];

    for (size_t byte = 0U; byte < POUND_KEY_SIZE; ++byte)
    {
        const int high = hex_value(line[value_start + (byte * 2U)]);
        const int low  = hex_value(line[value_start + (byte * 2U) + 1U]);

        if (POUND_UNLIKELY((high < 0) || (low < 0)))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "Line %zu of the key file gives the key '%.*s' a value that is "
                            "not made of hex digits.",
                            line_number,
                            (int)name_length,
                            line + name_start);
            return true;
        }

        // Both nibbles are known to be in 0..15, so the shift and the mask are exact.
        value[byte] = (uint8_t)(((unsigned int)high << 4) | (unsigned int)low);
    }

    // The name is copied out before `key_store_add` is called because the store keeps
    // the string, and `line` is a view into the caller's buffer that must not outlive
    // this call.
    char name[POUND_KEY_NAME_MAX];

    if (POUND_UNLIKELY(name_length >= sizeof(name)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Line %zu of the key file gives a key name of %zu characters, which "
                        "does not fit in %zu.",
                        line_number,
                        name_length,
                        sizeof(name));
        return true;
    }

    memcpy(name, line + name_start, name_length);
    name[name_length] = '\0';

    const error_t status = key_store_add(store, name, value);

    if (POUND_UNLIKELY(POUND_SUCCESS != status))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Line %zu of the key file defines the key '%s', which could not be "
                        "stored (%s).",
                        line_number,
                        name,
                        pound_error_to_string(status));
        return true;
    }

    return false;
}

error_t
key_store_load_text(key_store_t *POUND_RESTRICT store, const char *POUND_RESTRICT text, const size_t size)
{
    if (POUND_UNLIKELY(NULL == store))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the key store is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY((NULL == text) && (0U != size)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: a %zu-byte key file was described with a NULL pointer.",
                        size);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    bool        any_rejected = false;
    size_t      line_number  = 0U;
    size_t      cursor       = 0U;
    const char *POUND_RESTRICT text_data = text;

    while (cursor < size)
    {
        // Find the end of this line without walking past the buffer, so a file whose
        // last line has no terminator is handled the same as one that does.
        size_t line_end = cursor;

        while ((line_end < size) && ('\n' != text_data[line_end]))
        {
            ++line_end;
        }

        ++line_number;

        if (parse_line(store, text_data + cursor, line_end - cursor, line_number))
        {
            any_rejected = true;
        }

        // A file not ending in a newline leaves `cursor == size` and the loop ends; one
        // that does advances past the terminator.
        cursor = (line_end < size) ? (line_end + 1U) : size;
    }

    if (POUND_UNLIKELY(any_rejected))
    {
        // One summary line naming the count, so a user who fixed nothing still learns
        // from the summary that the file did not load cleanly rather than only from
        // the individual line messages.
        POUND_LOG_ERROR(&thread_logger,
                        "The key file was not read cleanly. %zu key(s) loaded, and the "
                        "line(s) above were skipped.",
                        store->count);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    return POUND_SUCCESS;
}

error_t
key_store_get(const key_store_t *POUND_RESTRICT store, const char *POUND_RESTRICT name,
              uint8_t out[POUND_KEY_SIZE])
{
    if (POUND_UNLIKELY(NULL == store))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the key store is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY((NULL == name) || ('\0' == name[0])))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: a key lookup was given an empty name.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(NULL == out))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the key '%s' was looked up with no destination.",
                        name);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    size_t low  = 0U;
    size_t high = store->count;

    while (low < high)
    {
        const size_t middle = low + ((high - low) / 2U);
        const int    order  = strcmp(store->entries[middle].name, name);

        if (0 == order)
        {
            memcpy(out, store->entries[middle].value, POUND_KEY_SIZE);
            return POUND_SUCCESS;
        }

        if (order < 0)
        {
            low = middle + 1U;
        }
        else
        {
            high = middle;
        }
    }

    // The single most important log in the loader. It names the key, because the user
    // has to go and add that specific line to their file, and it says the key was not
    // substituted with anything, because the natural wrong reaction to a decryption
    // failure is to try a different key.
    POUND_LOG_ERROR(&thread_logger,
                    "The key '%s' is not in the key file. Pound ships no keys and will not "
                    "substitute one; add it to your own key file to load this title.",
                    name);

    return POUND_ERROR_KEY_MISSING;
}

bool
key_store_has(const key_store_t *POUND_RESTRICT store, const char *POUND_RESTRICT name)
{
    if ((NULL == store) || (NULL == name) || ('\0' == name[0]))
    {
        return false;
    }

    size_t low  = 0U;
    size_t high = store->count;

    while (low < high)
    {
        const size_t middle = low + ((high - low) / 2U);
        const int    order  = strcmp(store->entries[middle].name, name);

        if (0 == order)
        {
            return true;
        }

        if (order < 0)
        {
            low = middle + 1U;
        }
        else
        {
            high = middle;
        }
    }

    return false;
}

size_t
key_store_count(const key_store_t *POUND_RESTRICT store)
{
    if (POUND_UNLIKELY(NULL == store))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the key store is NULL.");
        return 0U;
    }

    return store->count;
}

error_t
key_store_name_at(const key_store_t *POUND_RESTRICT store, const size_t index, char out[POUND_KEY_NAME_MAX])
{
    if (POUND_UNLIKELY(NULL == store))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: the key store is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(NULL == out))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: a key name was requested with no destination.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (POUND_UNLIKELY(index >= store->count))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: key %zu was requested, but the store holds only %zu.",
                        index,
                        store->count);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    memcpy(out, store->entries[index].name, POUND_KEY_NAME_MAX);

    return POUND_SUCCESS;
}

/// Looks up a member of a numbered key family, building its name from `prefix` and
/// `suffix`.
///
/// The families are assembled here rather than held as a table of literals so that the
/// bound and the name pattern cannot drift apart: there is one place where
/// `master_key_` and `02` are written next to the bound of 3.
static error_t
key_store_get_family(const key_store_t *POUND_RESTRICT store, const char *POUND_RESTRICT prefix,
                     const char *POUND_RESTRICT suffix, const unsigned int index, const unsigned int count,
                     const char *POUND_RESTRICT family, uint8_t out[POUND_KEY_SIZE])
{
    if (POUND_UNLIKELY(index >= count))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: %s index %u was requested, but the family has only "
                        "%u entries.",
                        family,
                        index,
                        count);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    char name[POUND_KEY_NAME_MAX];

    // The name is built into a stack buffer that is exactly the size of a stored name,
    // so a prefix or suffix that ever grew would be caught here by the truncation
    // check rather than by producing a name that silently matches nothing.
    const int written = snprintf(name, sizeof(name), "%s%02u%s", prefix, index, suffix);

    if (POUND_UNLIKELY((written < 0) || ((size_t)written >= sizeof(name))))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting function: the name of %s index %u does not fit in %zu characters.",
                        family,
                        index,
                        sizeof(name));
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    return key_store_get(store, name, out);
}

error_t
key_store_get_master_key(const key_store_t *POUND_RESTRICT store, const unsigned int index,
                         uint8_t out[POUND_KEY_SIZE])
{
    return key_store_get_family(store,
                                "master_key_",
                                "",
                                index,
                                POUND_KEY_MASTER_KEY_COUNT,
                                "the master key family",
                                out);
}

error_t
key_store_get_application_key_area(const key_store_t *POUND_RESTRICT store, const unsigned int index,
                                   uint8_t out[POUND_KEY_SIZE])
{
    return key_store_get_family(store,
                                "key_area_key_application_",
                                "",
                                index,
                                POUND_KEY_APPLICATION_KEY_AREA_COUNT,
                                "the application key area family",
                                out);
}

error_t
key_store_get_titlekek(const key_store_t *POUND_RESTRICT store, const unsigned int index, uint8_t out[POUND_KEY_SIZE])
{
    return key_store_get_family(store,
                                "titlekek_",
                                "",
                                index,
                                POUND_KEY_TITLEKEK_COUNT,
                                "the title key encryption key family",
                                out);
}

void
key_store_log_summary(const key_store_t *POUND_RESTRICT store)
{
    if (POUND_UNLIKELY(NULL == store))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: the key store is NULL.");
        return;
    }

    POUND_LOG_INFO(&thread_logger,
                   "The key file supplied %zu key(s). Their names are listed below; their values "
                   "are deliberately not logged.",
                   store->count);

    char name[POUND_KEY_NAME_MAX];

    for (size_t i = 0U; i < store->count; ++i)
    {
        // The index is known to be in range by the loop bound and `key_store_name_at`
        // only fails on a NULL store or destination, neither of which can be true here.
        // The status is still checked rather than discarded, because a silent failure
        // would leave a key unlisted in a summary whose entire job is to be complete.
        if (POUND_SUCCESS == key_store_name_at(store, i, name))
        {
            POUND_LOG_INFO(&thread_logger, "  key %zu: %s", i, name);
        }
    }
}

/*** end of file ***/

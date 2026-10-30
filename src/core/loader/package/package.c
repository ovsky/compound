//! NSP, XCI and standalone-NCA executable extraction.

#include "loader/package/package.h"

#include "fs/hfs0.h"
#include "fs/pfs0.h"
#include "loader/package/nca.h"
#include "loader/package/ticket.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>

/// Offset of the XCI root HFS0 in the gamecard image.
#define XCI_ROOT_OFFSET 0x10000U

/// A selected program NCA inside its container.
typedef struct
{
    const fs_reader_t *source;
    uint64_t           base;
    uint64_t           size;
    pound_nca_info_t   info;
} package_nca_t;

typedef struct
{
    pound_ticket_t tickets[POUND_PACKAGE_MAX_TICKETS];
    uint32_t       count;
} package_tickets_t;

static bool
ends_with_ci(const char *POUND_RESTRICT name, const char *POUND_RESTRICT suffix)
{
    const size_t name_length   = strlen(name);
    const size_t suffix_length = strlen(suffix);

    if (name_length < suffix_length)
    {
        return false;
    }

    const char *POUND_RESTRICT tail = name + (name_length - suffix_length);

    for (size_t i = 0U; i < suffix_length; ++i)
    {
        char left  = tail[i];
        char right = suffix[i];

        if ((left >= 'A') && (left <= 'Z'))
        {
            left = (char)(left - 'A' + 'a');
        }

        if ((right >= 'A') && (right <= 'Z'))
        {
            right = (char)(right - 'A' + 'a');
        }

        if (left != right)
        {
            return false;
        }
    }

    return true;
}

static unsigned int
package_key_generation(const uint8_t crypto_type, const uint8_t crypto_type2)
{
    uint8_t generation = (crypto_type2 > crypto_type) ? crypto_type2 : crypto_type;

    if (0U != generation)
    {
        --generation;
    }

    return generation;
}

static const pound_ticket_t *
find_ticket(const package_tickets_t *POUND_RESTRICT tickets, const uint8_t rights_id[16])
{
    for (uint32_t i = 0U; i < tickets->count; ++i)
    {
        if (0 == memcmp(tickets->tickets[i].rights_id, rights_id, 16U))
        {
            return &tickets->tickets[i];
        }
    }

    return NULL;
}

static error_t
extract_nca(const package_nca_t *POUND_RESTRICT selected, const key_store_t *POUND_RESTRICT keys,
            const package_tickets_t *POUND_RESTRICT tickets, pound_package_t *POUND_RESTRICT package)
{
    uint8_t title_key[16];
    const uint8_t *title_key_pointer = NULL;

    bool has_rights_id = false;
    for (size_t i = 0U; i < sizeof(selected->info.rights_id); ++i)
    {
        if (0U != selected->info.rights_id[i])
        {
            has_rights_id = true;
            break;
        }
    }

    if (has_rights_id)
    {
        const pound_ticket_t *POUND_RESTRICT ticket = find_ticket(tickets, selected->info.rights_id);

        if (NULL == ticket)
        {
            POUND_LOG_ERROR(&thread_logger,
                            "The program NCA needs rights id %02X%02X%02X%02X..., but the package has "
                            "no matching ticket.",
                            selected->info.rights_id[0],
                            selected->info.rights_id[1],
                            selected->info.rights_id[2],
                            selected->info.rights_id[3]);
            return POUND_ERROR_KEY_MISSING;
        }

        const error_t key_error
            = pound_ticket_decrypt_title_key(ticket,
                                             keys,
                                             package_key_generation(selected->info.crypto_type,
                                                                    selected->info.crypto_type2),
                                             title_key);
        if (POUND_SUCCESS != key_error)
        {
            return key_error;
        }

        title_key_pointer = title_key;
    }

    pound_nca_t nca;
    error_t     status = pound_nca_open(&nca,
                                        selected->source,
                                        selected->base,
                                        selected->size,
                                        keys,
                                        title_key_pointer);
    if (POUND_SUCCESS != status)
    {
        memset(title_key, 0, sizeof(title_key));
        return status;
    }

    uint8_t *main_data = NULL;
    size_t   main_size = 0U;
    status = pound_nca_extract_main(&nca, &main_data, &main_size);

    pound_nca_log_summary(&nca);
    pound_nca_close(&nca);
    memset(title_key, 0, sizeof(title_key));

    if (POUND_SUCCESS != status)
    {
        return status;
    }

    if ((main_size < 4U) || (0 != memcmp(main_data, "NSO0", 4U)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "The ExeFS main file is not an NSO. This build cannot execute it.");
        memset(main_data, 0, main_size);
        free(main_data);
        return POUND_ERROR_UNSUPPORTED_FORMAT;
    }

    package->main_data  = main_data;
    package->main_size  = main_size;
    package->program_id = selected->info.program_id;

    return POUND_SUCCESS;
}

static error_t
read_tickets(pfs0_t *POUND_RESTRICT pfs0, package_tickets_t *POUND_RESTRICT tickets)
{
    for (uint32_t index = 0U; index < pfs0_count(pfs0); ++index)
    {
        const pfs0_entry_t *POUND_RESTRICT entry = pfs0_entry_at(pfs0, index);
        if ((NULL == entry) || !ends_with_ci(entry->name, ".tik"))
        {
            continue;
        }

        if (tickets->count >= POUND_PACKAGE_MAX_TICKETS)
        {
            POUND_LOG_ERROR(&thread_logger,
                            "The NSP contains more than %u tickets; this loader will not choose among "
                            "them.",
                            POUND_PACKAGE_MAX_TICKETS);
            return POUND_ERROR_UNSUPPORTED_FEATURE;
        }

        if (entry->size > POUND_PACKAGE_MAX_TICKET_SIZE)
        {
            POUND_LOG_ERROR(&thread_logger, "Ticket '%s' is %llu bytes, beyond the %u-byte limit.",
                            entry->name,
                            (unsigned long long)entry->size,
                            POUND_PACKAGE_MAX_TICKET_SIZE);
            return POUND_ERROR_MALFORMED_HEADER;
        }

        uint8_t *ticket_data = (uint8_t *)malloc((size_t)entry->size);
        if (NULL == ticket_data)
        {
            POUND_LOG_ERROR(&thread_logger, "Allocating %llu bytes for a ticket failed.",
                            (unsigned long long)entry->size);
            return POUND_ERROR_ALLOCATION_FAILED;
        }

        const error_t read_error = pfs0_read(pfs0, index, 0U, ticket_data, (size_t)entry->size);
        if (POUND_SUCCESS != read_error)
        {
            free(ticket_data);
            return read_error;
        }

        const error_t parse_error
            = pound_ticket_parse(&tickets->tickets[tickets->count], ticket_data, (size_t)entry->size);

        memset(ticket_data, 0, (size_t)entry->size);
        free(ticket_data);

        if (POUND_SUCCESS != parse_error)
        {
            return parse_error;
        }

        ++tickets->count;
    }

    return POUND_SUCCESS;
}

/// Picks the program NCA from a PFS0/NSP and extracts it.
static error_t
load_nsp(const fs_reader_t *POUND_RESTRICT reader, const key_store_t *POUND_RESTRICT keys,
         pound_package_t *POUND_RESTRICT package)
{
    pfs0_t pfs0;
    error_t status = pfs0_open(&pfs0, reader);
    if (POUND_SUCCESS != status)
    {
        return status;
    }

    package_tickets_t tickets;
    memset(&tickets, 0, sizeof(tickets));

    status = read_tickets(&pfs0, &tickets);
    if (POUND_SUCCESS != status)
    {
        pfs0_close(&pfs0);
        return status;
    }

    bool          found = false;
    package_nca_t selected;
    memset(&selected, 0, sizeof(selected));

    for (uint32_t index = 0U; index < pfs0_count(&pfs0); ++index)
    {
        const pfs0_entry_t *POUND_RESTRICT entry = pfs0_entry_at(&pfs0, index);
        if ((NULL == entry) || !ends_with_ci(entry->name, ".nca"))
        {
            continue;
        }

        pound_nca_info_t info;
        status = pound_nca_inspect(reader, entry->offset, entry->size, keys, &info);
        if (POUND_SUCCESS != status)
        {
            // A non-program NCA that cannot be inspected is not necessarily fatal:
            // packages may carry metadata encrypted for a key this build does not need.
            // Only the program NCA is required to open here.
            continue;
        }

        if (0U != info.content_type)
        {
            continue;
        }

        if (!found)
        {
            selected.source = reader;
            selected.base   = entry->offset;
            selected.size   = entry->size;
            selected.info   = info;
            found           = true;
        }
    }

    if (!found)
    {
        POUND_LOG_ERROR(&thread_logger, "The NSP contains no program NCA this build could inspect.");
        pfs0_close(&pfs0);
        return POUND_ERROR_NOT_FOUND;
    }

    status = extract_nca(&selected, keys, &tickets, package);
    pfs0_close(&pfs0);
    return status;
}

/// Picks the program NCA from an XCI's secure partition and extracts it.
static error_t
load_xci(const fs_reader_t *POUND_RESTRICT reader, const key_store_t *POUND_RESTRICT keys,
         pound_package_t *POUND_RESTRICT package)
{
    if ((reader->size < (XCI_ROOT_OFFSET + HFS0_HEADER_SIZE))
        || (reader->size - XCI_ROOT_OFFSET > SIZE_MAX))
    {
        POUND_LOG_ERROR(&thread_logger, "The XCI is too small to hold a root HFS0 at 0x%X.",
                        XCI_ROOT_OFFSET);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    fs_reader_slice_t root_slice;
    error_t status = fs_reader_slice_init(&root_slice,
                                          reader,
                                          XCI_ROOT_OFFSET,
                                          (size_t)(reader->size - XCI_ROOT_OFFSET));
    if (POUND_SUCCESS != status)
    {
        return status;
    }

    hfs0_t root;
    status = hfs0_open(&root, &root_slice.reader);
    if (POUND_SUCCESS != status)
    {
        return status;
    }

    uint32_t secure_index = 0U;
    if (POUND_SUCCESS != hfs0_index_of(&root, "secure", &secure_index))
    {
        POUND_LOG_ERROR(&thread_logger, "The XCI root partition has no 'secure' entry.");
        hfs0_close(&root);
        return POUND_ERROR_NOT_FOUND;
    }

    hfs0_slice_t secure_slice;
    status = hfs0_entry_reader(&root, secure_index, &secure_slice);
    hfs0_close(&root);
    if (POUND_SUCCESS != status)
    {
        return status;
    }

    hfs0_t secure;
    status = hfs0_open(&secure, &secure_slice.reader);
    if (POUND_SUCCESS != status)
    {
        hfs0_slice_close(&secure_slice);
        return status;
    }

    bool          found = false;
    package_nca_t selected;
    memset(&selected, 0, sizeof(selected));

    for (uint32_t index = 0U; index < hfs0_count(&secure); ++index)
    {
        const hfs0_entry_t *POUND_RESTRICT entry = hfs0_entry_at(&secure, index);
        if ((NULL == entry) || !ends_with_ci(entry->name, ".nca"))
        {
            continue;
        }

        pound_nca_info_t info;
        status = pound_nca_inspect(&secure.reader, entry->offset, entry->size, keys, &info);
        if (POUND_SUCCESS != status)
        {
            continue;
        }

        if (0U != info.content_type)
        {
            continue;
        }

        if (!found)
        {
            selected.source = &secure.reader;
            selected.base   = entry->offset;
            selected.size   = entry->size;
            selected.info   = info;
            found           = true;
        }
    }

    if (!found)
    {
        POUND_LOG_ERROR(&thread_logger, "The XCI secure partition has no program NCA this build could inspect.");
        hfs0_close(&secure);
        hfs0_slice_close(&secure_slice);
        return POUND_ERROR_NOT_FOUND;
    }

    package_tickets_t no_tickets;
    memset(&no_tickets, 0, sizeof(no_tickets));

    status = extract_nca(&selected, keys, &no_tickets, package);
    if (POUND_SUCCESS == status)
    {
        package->from_gamecard = true;
    }

    hfs0_close(&secure);
    hfs0_slice_close(&secure_slice);
    return status;
}

error_t
pound_package_load(const fs_reader_t *POUND_RESTRICT reader, const key_store_t *POUND_RESTRICT keys,
                   pound_package_t *POUND_RESTRICT package)
{
    if (POUND_UNLIKELY(NULL == reader) || POUND_UNLIKELY(NULL == keys) || POUND_UNLIKELY(NULL == package))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: package loading was handed a NULL pointer.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    memset(package, 0, sizeof(*package));

    uint8_t magic[4];
    if ((reader->size < sizeof(magic)) || (POUND_SUCCESS != fs_reader_read_at(reader, 0U, magic, sizeof(magic))))
    {
        POUND_LOG_ERROR(&thread_logger, "The package is too small to identify.");
        return POUND_ERROR_UNSUPPORTED_FORMAT;
    }

    if (0 == memcmp(magic, "PFS0", 4U))
    {
        return load_nsp(reader, keys, package);
    }

    if (0 == memcmp(magic, "HEAD", 4U))
    {
        return load_xci(reader, keys, package);
    }

    pound_nca_info_t info;
    const error_t inspect_error = pound_nca_inspect(reader, 0U, reader->size, keys, &info);
    if (POUND_SUCCESS == inspect_error)
    {
        if (0U != info.content_type)
        {
            POUND_LOG_ERROR(&thread_logger, "The NCA is content type %u, not a program.", info.content_type);
            return POUND_ERROR_UNSUPPORTED_FORMAT;
        }

        package_nca_t selected;
        selected.source = reader;
        selected.base   = 0U;
        selected.size   = reader->size;
        selected.info   = info;

        package_tickets_t no_tickets;
        memset(&no_tickets, 0, sizeof(no_tickets));

        return extract_nca(&selected, keys, &no_tickets, package);
    }

    if ((POUND_ERROR_KEY_MISSING == inspect_error) || (POUND_ERROR_KEY_SIZE == inspect_error)
        || (POUND_ERROR_UNSUPPORTED_FEATURE == inspect_error))
    {
        return inspect_error;
    }

    POUND_LOG_ERROR(&thread_logger,
                    "The file is neither an NSP, an XCI, nor an NCA3 this build can inspect.");
    return POUND_ERROR_UNSUPPORTED_FORMAT;
}

void
pound_package_close(pound_package_t *POUND_RESTRICT package)
{
    if (NULL == package)
    {
        return;
    }

    if (NULL != package->main_data)
    {
        memset(package->main_data, 0, package->main_size);
        free(package->main_data);
    }

    memset(package, 0, sizeof(*package));
}

void
pound_package_log_summary(const pound_package_t *POUND_RESTRICT package)
{
    if ((NULL == package) || (NULL == package->main_data))
    {
        return;
    }

    POUND_LOG_INFO(&thread_logger,
                   "Loaded %s package for program %016llX: %zu-byte ExeFS main.",
                   package->from_gamecard ? "gamecard" : "download",
                   (unsigned long long)package->program_id,
                   package->main_size);
}

/*** end of file ***/

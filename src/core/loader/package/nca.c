//! NCA3 header, key-area and section decryption.
//!
//! The byte offsets in this file are the documented offsets of a decrypted NCA
//! header, not guesses. The XTS tweak construction and the CTR initial counter are
//! the ones hactool uses; both differ from a naive reading of the format, so they
//! are written out explicitly below and covered by tests rather than left to a
//! comment elsewhere.

#include "loader/package/nca.h"

#include "crypto/aes.h"
#include "fs/pfs0.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>

/// Offsets in the decrypted NCA header.
#define NCA_MAGIC_OFFSET          0x200U
#define NCA_DISTRIBUTION_OFFSET   0x204U
#define NCA_CONTENT_TYPE_OFFSET   0x205U
#define NCA_CRYPTO_TYPE_OFFSET    0x206U
#define NCA_KAEK_INDEX_OFFSET     0x207U
#define NCA_SIZE_OFFSET           0x208U
#define NCA_PROGRAM_ID_OFFSET     0x210U
#define NCA_CRYPTO_TYPE2_OFFSET   0x220U
#define NCA_RIGHTS_ID_OFFSET      0x230U
#define NCA_SECTIONS_OFFSET       0x240U
#define NCA_ENCRYPTED_KEYS_OFFSET 0x300U
#define NCA_FS_HEADERS_OFFSET     0x400U

/// Offsets inside an NCA section entry.
#define NCA_SECTION_START_OFFSET 0U
#define NCA_SECTION_END_OFFSET   4U

/// Offsets inside an NCA fs header.
#define NCA_FS_PARTITION_OFFSET 0x2U
#define NCA_FS_TYPE_OFFSET      0x3U
#define NCA_FS_CRYPT_OFFSET     0x4U
#define NCA_FS_PFS0_OFFSET      0x40U
#define NCA_FS_PFS0_SIZE        0x48U
#define NCA_FS_SECTION_CTR      0x140U

static uint32_t
load_le32(const uint8_t *POUND_RESTRICT bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) | ((uint32_t)bytes[2] << 16U)
         | ((uint32_t)bytes[3] << 24U);
}

static uint64_t
load_le64(const uint8_t *POUND_RESTRICT bytes)
{
    return (uint64_t)load_le32(bytes) | ((uint64_t)load_le32(bytes + 4U) << 32U);
}

static bool
range_is_inside(const uint64_t offset, const uint64_t size, const uint64_t limit)
{
    return (offset <= limit) && (size <= (limit - offset));
}

static error_t
nca_read(const pound_nca_t *POUND_RESTRICT nca, const uint64_t offset, void *POUND_RESTRICT destination,
         const size_t size)
{
    if (POUND_UNLIKELY(offset > (UINT64_MAX - nca->base)))
    {
        POUND_LOG_ERROR(&thread_logger, "NCA read at 0x%llX wraps the parent reader.",
                        (unsigned long long)offset);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    if (false == range_is_inside(offset, size, nca->size))
    {
        POUND_LOG_ERROR(&thread_logger, "NCA read [0x%llX, 0x%llX) leaves the %llu-byte content.",
                        (unsigned long long)offset,
                        (unsigned long long)(offset + size),
                        (unsigned long long)nca->size);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    return fs_reader_read_at(nca->source, nca->base + offset, destination, size);
}

/// Builds the XTS tweak for NCA sector `sector`.
///
/// Nintendo passes the sector number as a big-endian integer in the low eight
/// bytes of the data unit. The data unit is otherwise zero.
static void
nca_sector_tweak(uint8_t tweak[POUND_AES_BLOCK_SIZE], const uint64_t sector)
{
    memset(tweak, 0, POUND_AES_BLOCK_SIZE);

    for (unsigned int byte = 0U; byte < 8U; ++byte)
    {
        tweak[15U - byte] = (uint8_t)(sector >> (byte * 8U));
    }
}

/// Builds the initial CTR block exactly as hactool does for an NCA section.
///
/// The eight-byte `section_ctr` field is reversed into the first half of the
/// counter, and the section's byte offset divided by sixteen is written big-endian
/// into the second half. Neither half is the field in its stored order.
static void
nca_section_counter(uint8_t counter[POUND_AES_BLOCK_SIZE], const uint8_t section_ctr[8],
                    const uint64_t section_offset)
{
    uint64_t offset = section_offset >> 4U;

    for (unsigned int byte = 0U; byte < 8U; ++byte)
    {
        counter[byte]        = section_ctr[7U - byte];
        counter[15U - byte] = (uint8_t)(offset & 0xFFU);
        offset >>= 8U;
    }
}

/// Selects the key-area key generation the way the format does.
///
/// `crypto_type` and `crypto_type2` are both key generations; the higher one wins,
/// and generations 0 and 1 both use the first key. A value beyond the user's key
/// family is left to the key store to report as missing.
static unsigned int
nca_key_generation(const uint8_t crypto_type, const uint8_t crypto_type2)
{
    uint8_t generation = (crypto_type2 > crypto_type) ? crypto_type2 : crypto_type;

    if (0U != generation)
    {
        --generation;
    }

    return generation;
}

static bool
rights_id_is_present(const uint8_t rights_id[16])
{
    for (size_t i = 0U; i < 16U; ++i)
    {
        if (0U != rights_id[i])
        {
            return true;
        }
    }

    return false;
}

/// Reads `[base, base + size)` and leaves a decrypted NCA3 header in `header`.
static error_t
nca_decode_header(const fs_reader_t *POUND_RESTRICT source, const uint64_t base, const uint64_t size,
                  const key_store_t *POUND_RESTRICT keys, uint8_t header[POUND_NCA_HEADER_SIZE],
                  bool *POUND_RESTRICT out_plaintext)
{
    *out_plaintext = false;

    if (size < POUND_NCA_HEADER_SIZE)
    {
        POUND_LOG_ERROR(&thread_logger, "A %llu-byte NCA is smaller than its 0x%X-byte header.",
                        (unsigned long long)size, POUND_NCA_HEADER_SIZE);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    if (!fs_reader_range_is_valid(source, base, (size_t)size))
    {
        POUND_LOG_ERROR(&thread_logger, "The NCA range [0x%llX, 0x%llX) leaves its source reader.",
                        (unsigned long long)base,
                        (unsigned long long)(base + size));
        return POUND_ERROR_IO;
    }

    error_t status = fs_reader_read_at(source, base, header, POUND_NCA_HEADER_SIZE);
    if (POUND_SUCCESS != status)
    {
        return status;
    }

    bool looks_plain = false;
    if ((0 == memcmp(header + NCA_MAGIC_OFFSET, "NCA3", 4U))
        || (0 == memcmp(header + NCA_MAGIC_OFFSET, "NCA2", 4U)))
    {
        looks_plain = true;

        for (size_t i = 0x340U; i < 0x400U; ++i)
        {
            if (0U != header[i])
            {
                looks_plain = false;
                break;
            }
        }
    }

    if (!looks_plain)
    {
        uint8_t header_key[POUND_KEY_MAX_SIZE];
        status = key_store_get_sized(keys, POUND_KEY_HEADER_KEY, header_key, sizeof(header_key));
        if (POUND_SUCCESS != status)
        {
            return status;
        }

        uint8_t raw[POUND_NCA_HEADER_SIZE];
        memcpy(raw, header, sizeof(raw));

        for (uint64_t sector = 0U; sector < (POUND_NCA_HEADER_SIZE / POUND_NCA_SECTOR_SIZE); ++sector)
        {
            uint8_t tweak[POUND_AES_BLOCK_SIZE];
            nca_sector_tweak(tweak, sector);

            status = pound_aes_xts_decrypt(header_key,
                                           sizeof(header_key),
                                           tweak,
                                           raw + (sector * POUND_NCA_SECTOR_SIZE),
                                           header + (sector * POUND_NCA_SECTOR_SIZE),
                                           POUND_NCA_SECTOR_SIZE,
                                           POUND_NCA_SECTOR_SIZE);
            if (POUND_SUCCESS != status)
            {
                memset(header_key, 0, sizeof(header_key));
                memset(raw, 0, sizeof(raw));
                return status;
            }
        }

        memset(header_key, 0, sizeof(header_key));
        memset(raw, 0, sizeof(raw));
    }

    if (0 != memcmp(header + NCA_MAGIC_OFFSET, "NCA3", 4U))
    {
        if (0 == memcmp(header + NCA_MAGIC_OFFSET, "NCA2", 4U))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "This is an NCA2 archive. NCA2 header decryption is not implemented.");
            return POUND_ERROR_UNSUPPORTED_FEATURE;
        }

        POUND_LOG_ERROR(&thread_logger,
                        "The decrypted NCA header has no NCA3 magic. The header_key is wrong or the "
                        "file is not an NCA.");
        return POUND_ERROR_MALFORMED_HEADER;
    }

    *out_plaintext = looks_plain;
    return POUND_SUCCESS;
}

error_t
pound_nca_inspect(const fs_reader_t *POUND_RESTRICT source, const uint64_t base, const uint64_t size,
                  const key_store_t *POUND_RESTRICT keys, pound_nca_info_t *POUND_RESTRICT info)
{
    if (POUND_UNLIKELY(NULL == source) || POUND_UNLIKELY(NULL == keys) || POUND_UNLIKELY(NULL == info))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: NCA inspection was handed a NULL pointer.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    memset(info, 0, sizeof(*info));

    uint8_t header[POUND_NCA_HEADER_SIZE];
    error_t status = nca_decode_header(source, base, size, keys, header, &info->is_plaintext);
    if (POUND_SUCCESS != status)
    {
        return status;
    }

    info->distribution_type = header[NCA_DISTRIBUTION_OFFSET];
    info->content_type      = header[NCA_CONTENT_TYPE_OFFSET];
    info->crypto_type       = header[NCA_CRYPTO_TYPE_OFFSET];
    info->crypto_type2      = header[NCA_CRYPTO_TYPE2_OFFSET];
    info->kaek_ind          = header[NCA_KAEK_INDEX_OFFSET];
    info->program_id        = load_le64(header + NCA_PROGRAM_ID_OFFSET);
    info->nca_size          = load_le64(header + NCA_SIZE_OFFSET);

    memcpy(info->rights_id, header + NCA_RIGHTS_ID_OFFSET, sizeof(info->rights_id));

    memset(header, 0, sizeof(header));
    return POUND_SUCCESS;
}

error_t
pound_nca_open(pound_nca_t *POUND_RESTRICT nca, const fs_reader_t *POUND_RESTRICT source,
               const uint64_t base, const uint64_t size, const key_store_t *POUND_RESTRICT keys,
               const uint8_t title_key[16])
{
    if (POUND_UNLIKELY(NULL == nca) || POUND_UNLIKELY(NULL == source) || POUND_UNLIKELY(NULL == keys))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: NCA open was handed a NULL pointer.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    memset(nca, 0, sizeof(*nca));
    nca->source = source;
    nca->base   = base;
    nca->size   = size;

    error_t status = nca_decode_header(source, base, size, keys, nca->header, &nca->is_plaintext);
    if (POUND_SUCCESS != status)
    {
        return status;
    }

    nca->distribution_type = nca->header[NCA_DISTRIBUTION_OFFSET];
    nca->content_type      = nca->header[NCA_CONTENT_TYPE_OFFSET];
    nca->crypto_type       = nca->header[NCA_CRYPTO_TYPE_OFFSET];
    nca->kaek_ind           = nca->header[NCA_KAEK_INDEX_OFFSET];
    nca->program_id        = load_le64(nca->header + NCA_PROGRAM_ID_OFFSET);

    memcpy(nca->rights_id, nca->header + NCA_RIGHTS_ID_OFFSET, sizeof(nca->rights_id));
    nca->has_rights_id = rights_id_is_present(nca->rights_id);

    const uint64_t declared_size = load_le64(nca->header + NCA_SIZE_OFFSET);
    if ((declared_size < POUND_NCA_HEADER_SIZE) || (declared_size > size))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "NCA declares a size of %llu bytes, but %llu bytes were supplied.",
                        (unsigned long long)declared_size,
                        (unsigned long long)size);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    nca->size = declared_size;

    memcpy(nca->fs_headers, nca->header + NCA_FS_HEADERS_OFFSET, sizeof(nca->fs_headers));

    for (uint32_t index = 0U; index < POUND_NCA_SECTION_COUNT; ++index)
    {
        const uint8_t *POUND_RESTRICT entry = nca->header + NCA_SECTIONS_OFFSET + (index * 16U);

        const uint64_t start = (uint64_t)load_le32(entry + NCA_SECTION_START_OFFSET) * POUND_NCA_SECTOR_SIZE;
        const uint64_t end   = (uint64_t)load_le32(entry + NCA_SECTION_END_OFFSET) * POUND_NCA_SECTOR_SIZE;

        if (0U == start && 0U == end)
        {
            continue;
        }

        if ((end < start) || !range_is_inside(start, end - start, nca->size))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "NCA section %u declares [0x%llX, 0x%llX), outside the %llu-byte content.",
                            index,
                            (unsigned long long)start,
                            (unsigned long long)end,
                            (unsigned long long)nca->size);
            return POUND_ERROR_MALFORMED_HEADER;
        }

        const uint8_t crypt_type = nca->fs_headers[index][NCA_FS_CRYPT_OFFSET];
        if ((POUND_NCA_CRYPT_BKTR == crypt_type) && !nca->has_rights_id)
        {
            POUND_LOG_ERROR(&thread_logger,
                            "NCA section %u uses BKTR encryption, which this build does not implement.",
                            index);
            return POUND_ERROR_UNSUPPORTED_FEATURE;
        }

        nca->sections[index].offset = start;
        nca->sections[index].size   = end - start;
    }

    if (nca->has_rights_id)
    {
        if (NULL == title_key)
        {
            POUND_LOG_ERROR(&thread_logger,
                            "NCA rights id %02X%02X%02X%02X... needs a ticket title key, which was "
                            "not supplied.",
                            nca->rights_id[0],
                            nca->rights_id[1],
                            nca->rights_id[2],
                            nca->rights_id[3]);
            return POUND_ERROR_KEY_MISSING;
        }

        memcpy(nca->title_key, title_key, sizeof(nca->title_key));
    }
    else
    {
        if (0U != nca->kaek_ind)
        {
            POUND_LOG_ERROR(&thread_logger,
                            "NCA uses key-area index %u; this build has only the index-0 application "
                            "key-area key layout.",
                            nca->kaek_ind);
            return POUND_ERROR_UNSUPPORTED_FEATURE;
        }

        uint8_t key_area_key[POUND_KEY_SIZE];
        status = key_store_get_application_key_area(keys,
                                                    nca_key_generation(nca->crypto_type,
                                                                       nca->header[NCA_CRYPTO_TYPE2_OFFSET]),
                                                    key_area_key);
        if (POUND_SUCCESS != status)
        {
            return status;
        }

        pound_aes_t aes;
        status = pound_aes_init(&aes, key_area_key, sizeof(key_area_key));
        if (POUND_SUCCESS == status)
        {
            status = pound_aes_ecb_decrypt(&aes,
                                           nca->header + NCA_ENCRYPTED_KEYS_OFFSET,
                                           nca->decrypted_keys,
                                           sizeof(nca->decrypted_keys));
        }

        memset(&aes, 0, sizeof(aes));
        memset(key_area_key, 0, sizeof(key_area_key));

        if (POUND_SUCCESS != status)
        {
            return status;
        }
    }

    nca->initialized = true;
    return POUND_SUCCESS;
}

void
pound_nca_close(pound_nca_t *POUND_RESTRICT nca)
{
    if (NULL != nca)
    {
        memset(nca, 0, sizeof(*nca));
    }
}

error_t
pound_nca_read_section(const pound_nca_t *POUND_RESTRICT nca, const uint32_t index,
                       uint8_t **POUND_RESTRICT out, size_t *POUND_RESTRICT out_size)
{
    if (POUND_UNLIKELY(NULL == nca) || POUND_UNLIKELY(NULL == out) || POUND_UNLIKELY(NULL == out_size))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: NCA section read was handed a NULL pointer.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    *out      = NULL;
    *out_size = 0U;

    if (!nca->initialized)
    {
        return POUND_ERROR_NOT_INITIALIZED;
    }

    if (index >= POUND_NCA_SECTION_COUNT)
    {
        POUND_LOG_ERROR(&thread_logger, "NCA section %u is outside the four sections an NCA has.", index);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const pound_nca_section_t *POUND_RESTRICT section = &nca->sections[index];
    if (0U == section->size)
    {
        POUND_LOG_ERROR(&thread_logger, "NCA section %u is not present.", index);
        return POUND_ERROR_NOT_FOUND;
    }

    if (section->size > POUND_NCA_MAX_SECTION_SIZE)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "NCA section %u is %llu bytes, beyond the %llu-byte loader limit.",
                        index,
                        (unsigned long long)section->size,
                        (unsigned long long)POUND_NCA_MAX_SECTION_SIZE);
        return POUND_ERROR_UNSUPPORTED_FEATURE;
    }

    if (section->size > SIZE_MAX)
    {
        POUND_LOG_ERROR(&thread_logger, "NCA section %u does not fit a host size_t.", index);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    const size_t size = (size_t)section->size;
    uint8_t *POUND_RESTRICT buffer = (uint8_t *)malloc(size);
    if (NULL == buffer)
    {
        POUND_LOG_ERROR(&thread_logger, "Allocating %zu bytes for NCA section %u failed.", size, index);
        return POUND_ERROR_ALLOCATION_FAILED;
    }

    error_t status = nca_read(nca, section->offset, buffer, size);
    if (POUND_SUCCESS != status)
    {
        free(buffer);
        return status;
    }

    const uint8_t crypt_type = nca->fs_headers[index][NCA_FS_CRYPT_OFFSET];

    if ((POUND_NCA_CRYPT_NONE == crypt_type) || nca->is_plaintext)
    {
        // The section is already plaintext. A plaintext NCA may still carry a
        // crypt type in its header; the absence of an initialized key area is what
        // makes the bytes trustworthy here.
    }
    else if (nca->has_rights_id)
    {
        uint8_t counter[POUND_AES_BLOCK_SIZE];
        nca_section_counter(counter,
                            nca->fs_headers[index] + NCA_FS_SECTION_CTR,
                            section->offset);

        pound_aes_t aes;
        status = pound_aes_init(&aes, nca->title_key, sizeof(nca->title_key));
        if (POUND_SUCCESS == status)
        {
            status = pound_aes_ctr_xcrypt(&aes, counter, buffer, buffer, size);
        }

        memset(&aes, 0, sizeof(aes));
        memset(counter, 0, sizeof(counter));
    }
    else if (POUND_NCA_CRYPT_CTR == crypt_type)
    {
        uint8_t counter[POUND_AES_BLOCK_SIZE];
        nca_section_counter(counter,
                            nca->fs_headers[index] + NCA_FS_SECTION_CTR,
                            section->offset);

        pound_aes_t aes;
        status = pound_aes_init(&aes, nca->decrypted_keys + 0x20U, POUND_KEY_SIZE);
        if (POUND_SUCCESS == status)
        {
            status = pound_aes_ctr_xcrypt(&aes, counter, buffer, buffer, size);
        }

        memset(&aes, 0, sizeof(aes));
        memset(counter, 0, sizeof(counter));
    }
    else if (POUND_NCA_CRYPT_XTS == crypt_type)
    {
        if (0U != (size % POUND_NCA_SECTOR_SIZE))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "NCA section %u is %zu bytes, not a whole number of 0x200-byte XTS sectors.",
                            index, size);
            free(buffer);
            return POUND_ERROR_MALFORMED_HEADER;
        }

        for (size_t offset = 0U; offset < size; offset += POUND_NCA_SECTOR_SIZE)
        {
            uint8_t tweak[POUND_AES_BLOCK_SIZE];
            nca_sector_tweak(tweak, offset / POUND_NCA_SECTOR_SIZE);

            status = pound_aes_xts_decrypt(nca->decrypted_keys,
                                           POUND_AES_MAX_KEY_SIZE,
                                           tweak,
                                           buffer + offset,
                                           buffer + offset,
                                           POUND_NCA_SECTOR_SIZE,
                                           POUND_NCA_SECTOR_SIZE);
            if (POUND_SUCCESS != status)
            {
                break;
            }
        }
    }
    else if (POUND_NCA_CRYPT_BKTR == crypt_type)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "NCA section %u uses BKTR encryption, which this build does not implement.",
                        index);
        status = POUND_ERROR_UNSUPPORTED_FEATURE;
    }
    else
    {
        POUND_LOG_ERROR(&thread_logger, "NCA section %u has unknown crypt type %u.", index, crypt_type);
        status = POUND_ERROR_UNSUPPORTED_FEATURE;
    }

    if (POUND_SUCCESS != status)
    {
        memset(buffer, 0, size);
        free(buffer);
        return status;
    }

    *out      = buffer;
    *out_size = size;
    return POUND_SUCCESS;
}

error_t
pound_nca_extract_main(const pound_nca_t *POUND_RESTRICT nca, uint8_t **POUND_RESTRICT out,
                       size_t *POUND_RESTRICT out_size)
{
    if (POUND_UNLIKELY(NULL == nca) || POUND_UNLIKELY(NULL == out) || POUND_UNLIKELY(NULL == out_size))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: ExeFS extraction was handed a NULL pointer.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    *out      = NULL;
    *out_size = 0U;

    if (!nca->initialized)
    {
        return POUND_ERROR_NOT_INITIALIZED;
    }

    for (uint32_t index = 0U; index < POUND_NCA_SECTION_COUNT; ++index)
    {
        if (0U == nca->sections[index].size)
        {
            continue;
        }

        const uint8_t *POUND_RESTRICT fs_header = nca->fs_headers[index];
        if ((POUND_NCA_PARTITION_PFS0 != fs_header[NCA_FS_PARTITION_OFFSET])
            || (POUND_NCA_FS_TYPE_PFS0 != fs_header[NCA_FS_TYPE_OFFSET]))
        {
            continue;
        }

        uint8_t *section       = NULL;
        size_t   section_size  = 0U;
        error_t  status        = pound_nca_read_section(nca, index, &section, &section_size);
        if (POUND_SUCCESS != status)
        {
            return status;
        }

        const uint64_t pfs0_offset = load_le64(fs_header + NCA_FS_PFS0_OFFSET);
        const uint64_t pfs0_size   = load_le64(fs_header + NCA_FS_PFS0_SIZE);

        if (!range_is_inside(pfs0_offset, pfs0_size, section_size) || (0U == pfs0_size))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "ExeFS section %u declares PFS0 [0x%llX, 0x%llX), outside its %zu bytes.",
                            index,
                            (unsigned long long)pfs0_offset,
                            (unsigned long long)(pfs0_offset + pfs0_size),
                            section_size);
            memset(section, 0, section_size);
            free(section);
            return POUND_ERROR_MALFORMED_HEADER;
        }

        fs_reader_t pfs0_reader;
        fs_reader_from_buffer(&pfs0_reader, section + pfs0_offset, (size_t)pfs0_size);

        pfs0_t partition;
        status = pfs0_open(&partition, &pfs0_reader);
        if (POUND_SUCCESS != status)
        {
            memset(section, 0, section_size);
            free(section);
            return status;
        }

        uint32_t main_index = 0U;
        status = pfs0_index_of(&partition, "main", &main_index);
        if (POUND_ERROR_NOT_FOUND == status)
        {
            pfs0_close(&partition);
            memset(section, 0, section_size);
            free(section);
            continue;
        }

        if (POUND_SUCCESS != status)
        {
            pfs0_close(&partition);
            memset(section, 0, section_size);
            free(section);
            return status;
        }

        const pfs0_entry_t *POUND_RESTRICT entry = pfs0_entry_at(&partition, main_index);
        if ((NULL == entry) || (entry->size > SIZE_MAX) || (0U == entry->size))
        {
            POUND_LOG_ERROR(&thread_logger, "The ExeFS main entry has an unusable size.");
            pfs0_close(&partition);
            memset(section, 0, section_size);
            free(section);
            return POUND_ERROR_MALFORMED_HEADER;
        }

        uint8_t *main_data = (uint8_t *)malloc((size_t)entry->size);
        if (NULL == main_data)
        {
            POUND_LOG_ERROR(&thread_logger, "Allocating %llu bytes for ExeFS main failed.",
                            (unsigned long long)entry->size);
            pfs0_close(&partition);
            memset(section, 0, section_size);
            free(section);
            return POUND_ERROR_ALLOCATION_FAILED;
        }

        status = pfs0_read(&partition, main_index, 0U, main_data, (size_t)entry->size);

        pfs0_close(&partition);
        memset(section, 0, section_size);
        free(section);

        if (POUND_SUCCESS != status)
        {
            memset(main_data, 0, (size_t)entry->size);
            free(main_data);
            return status;
        }

        *out      = main_data;
        *out_size = (size_t)entry->size;
        return POUND_SUCCESS;
    }

    POUND_LOG_ERROR(&thread_logger, "This NCA has no PFS0 ExeFS section containing a main file.");
    return POUND_ERROR_NOT_FOUND;
}

void
pound_nca_log_summary(const pound_nca_t *POUND_RESTRICT nca)
{
    if ((NULL == nca) || !nca->initialized)
    {
        return;
    }

    POUND_LOG_INFO(&thread_logger,
                   "NCA%s: program %016llX, %llu bytes, %s, %s, %s.",
                   nca->is_plaintext ? " (plaintext header)" : "",
                   (unsigned long long)nca->program_id,
                   (unsigned long long)nca->size,
                   (0U == nca->distribution_type) ? "download" : "gamecard",
                   (0U == nca->content_type) ? "program" : "non-program",
                   nca->has_rights_id ? "rights-id/title-key" : "key-area");
}

/*** end of file ***/

#include "loader/boot/boot.h"

#include "compression/lz4.h"
#include "fs/file_reader.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool
add_u64(const uint64_t left, const uint64_t right, uint64_t *POUND_RESTRICT out)
{
    if (POUND_UNLIKELY(NULL == out) || POUND_UNLIKELY(left > (UINT64_MAX - right)))
    {
        return false;
    }

    *out = left + right;
    return true;
}

/// Rounds `value` up to `alignment`, which is a power of two. Returns false on
/// overflow rather than wrapping to a small page count.
static bool
align_up_u64(const uint64_t value, const uint64_t alignment, uint64_t *POUND_RESTRICT out)
{
    const uint64_t mask = alignment - 1U;

    if (POUND_UNLIKELY(value > (UINT64_MAX - mask)))
    {
        return false;
    }

    *out = (value + mask) & ~mask;
    return true;
}

static uint64_t
align_down_u64(const uint64_t value, const uint64_t alignment)
{
    return value & ~(alignment - 1U);
}

void
pound_boot_config_default(pound_boot_config_t *POUND_RESTRICT config)
{
    if (NULL == config)
    {
        return;
    }

    config->guest_base         = POUND_BOOT_DEFAULT_GUEST_BASE;
    config->address_space_size = POUND_BOOT_DEFAULT_ADDRESS_SPACE_SIZE;
    config->stack_size         = POUND_BOOT_DEFAULT_STACK_SIZE;
}

error_t
pound_boot_init_with_config(pound_boot_state_t *POUND_RESTRICT state,
                            const pound_boot_config_t *POUND_RESTRICT config)
{
    if (POUND_UNLIKELY(NULL == state) || POUND_UNLIKELY(NULL == config))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting boot init: state=%p, config=%p.",
                        (const void *)state, (const void *)config);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const uint64_t page = POUND_BOOT_PAGE_SIZE;

    if ((0U != (config->guest_base & (page - 1U)))
        || (0U != (config->address_space_size & (page - 1U)))
        || (0U != (config->stack_size & (page - 1U))))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting boot config: base=0x%llX, size=0x%llX and stack=0x%llX must be page-aligned.",
                        (unsigned long long)config->guest_base,
                        (unsigned long long)config->address_space_size,
                        (unsigned long long)config->stack_size);
        return POUND_ERROR_MEMORY_ALIGNMENT;
    }

    if ((config->address_space_size < (page * 4U)) || (config->stack_size == 0U)
        || (config->stack_size > (config->address_space_size / 4U)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Rejecting boot config: address space 0x%llX is too small for stack 0x%llX.",
                        (unsigned long long)config->address_space_size,
                        (unsigned long long)config->stack_size);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    uint64_t guest_end = 0U;
    if (false == add_u64(config->guest_base, config->address_space_size, &guest_end))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting boot config: the guest address space wraps.");
        return POUND_ERROR_GUEST_ADDRESS_OVERFLOW;
    }

    if (config->address_space_size > (uint64_t)SIZE_MAX)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting boot config: the address space does not fit the host.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    memset(state, 0, sizeof(*state));
    state->guest_base   = config->guest_base;
    state->guest_end    = guest_end;
    state->host_size    = (size_t)config->address_space_size;
    state->stack_size   = config->stack_size;
    state->phase        = POUND_BOOT_PHASE_IDLE;
    state->initialized  = true;
    state->switch2_mode = false;

    POUND_LOG_INFO(&thread_logger,
                   "Boot initialized for guest [0x%llX, 0x%llX), stack 0x%llX bytes.",
                   (unsigned long long)state->guest_base,
                   (unsigned long long)state->guest_end,
                   (unsigned long long)state->stack_size);
    return POUND_SUCCESS;
}

error_t
pound_boot_init(pound_boot_state_t *POUND_RESTRICT state)
{
    pound_boot_config_t config;
    pound_boot_config_default(&config);
    return pound_boot_init_with_config(state, &config);
}

static void
boot_release_buffer(pound_boot_state_t *POUND_RESTRICT state)
{
    if ((NULL != state) && state->owns_host_buffer)
    {
        free(state->host_base);
    }

    if (NULL != state)
    {
        state->host_base        = NULL;
        state->owns_host_buffer = false;
        memset(&state->guest_memory, 0, sizeof(state->guest_memory));
        memset(&state->memory_layout, 0, sizeof(state->memory_layout));
    }
}

static error_t
boot_map_segment(pound_boot_state_t *POUND_RESTRICT state,
                 const fs_reader_t *POUND_RESTRICT reader,
                 const pound_title_segment_t *POUND_RESTRICT segment,
                 const uint32_t memory_state,
                 const uint32_t permission,
                 uint64_t *POUND_RESTRICT mapped_end)
{
    if (0U == segment->size)
    {
        return POUND_SUCCESS;
    }

    if (0U != (segment->memory_offset & (POUND_BOOT_PAGE_SIZE - 1U)))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing image: segment at +0x%llX is not page-aligned.",
                        (unsigned long long)segment->memory_offset);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    uint64_t memory_end = 0U;
    if (false == add_u64(segment->memory_offset, segment->size, &memory_end))
    {
        return POUND_ERROR_MALFORMED_HEADER;
    }

    uint64_t page_end = 0U;
    if (false == align_up_u64(memory_end, POUND_BOOT_PAGE_SIZE, &page_end))
    {
        return POUND_ERROR_MALFORMED_HEADER;
    }

    if (memory_end > (state->image_end - state->guest_base))
    {
        POUND_LOG_ERROR(&thread_logger, "Refusing image: a segment extends past the declared image size.");
        return POUND_ERROR_MALFORMED_HEADER;
    }

    if (segment->memory_offset < *mapped_end)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing image: segment at +0x%llX overlaps the previous segment ending at +0x%llX.",
                        (unsigned long long)segment->memory_offset,
                        (unsigned long long)*mapped_end);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    const uint64_t map_size = page_end - segment->memory_offset;
    const uint64_t map_base = state->guest_base + segment->memory_offset;

    const error_t map_error = hos_memory_layout_map(&state->memory_layout,
                                                    map_base,
                                                    map_size,
                                                    memory_state,
                                                    permission,
                                                    0U);
    if (POUND_SUCCESS != map_error)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Mapping image segment [0x%llX, 0x%llX) failed: %s.",
                        (unsigned long long)map_base,
                        (unsigned long long)(map_base + map_size),
                        pound_error_to_string(map_error));
        return map_error;
    }

    uint8_t *POUND_RESTRICT destination = state->host_base + (size_t)segment->memory_offset;

    if (!segment->compressed)
    {
        const error_t read_error
            = fs_reader_read_at(reader, segment->file_offset, destination, (size_t)segment->size);
        if (POUND_SUCCESS != read_error)
        {
            POUND_LOG_ERROR(&thread_logger, "Reading image segment at file offset 0x%llX failed.",
                            (unsigned long long)segment->file_offset);
            return read_error;
        }
    }
    else
    {
        if ((segment->stored_size > SIZE_MAX) || (segment->size > SIZE_MAX))
        {
            POUND_LOG_ERROR(&thread_logger, "Refusing image: a compressed segment does not fit a host size_t.");
            return POUND_ERROR_MALFORMED_HEADER;
        }

        uint8_t *compressed = (uint8_t *)malloc((size_t)segment->stored_size);
        if (NULL == compressed)
        {
            POUND_LOG_ERROR(&thread_logger, "Allocating %llu bytes for a compressed segment failed.",
                            (unsigned long long)segment->stored_size);
            return POUND_ERROR_ALLOCATION_FAILED;
        }

        error_t read_error
            = fs_reader_read_at(reader, segment->file_offset, compressed, (size_t)segment->stored_size);
        if (POUND_SUCCESS == read_error)
        {
            size_t decoded_size = 0U;
            read_error = pound_lz4_decompress_block(compressed,
                                                    (size_t)segment->stored_size,
                                                    destination,
                                                    (size_t)segment->size,
                                                    &decoded_size);
        }

        free(compressed);

        if (POUND_SUCCESS != read_error)
        {
            POUND_LOG_ERROR(&thread_logger,
                            "Decompressing the segment at file offset 0x%llX failed: %s.",
                            (unsigned long long)segment->file_offset,
                            pound_error_to_string(read_error));
            return read_error;
        }
    }

    *mapped_end = page_end;
    return POUND_SUCCESS;
}

static error_t
boot_load_reader(pound_boot_state_t *POUND_RESTRICT state, const pound_title_info_t *POUND_RESTRICT info,
                 const fs_reader_t *POUND_RESTRICT reader)
{

    if (!state->initialized)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting boot load: boot state is not initialized.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    if (POUND_BOOT_PHASE_IDLE != state->phase)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting boot load: state is not idle (%d).", (int)state->phase);
        return POUND_ERROR_INVALID_STATE;
    }

    if (info->is_switch2_compatible)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing the title: this build has no Switch 2 execution or decryption layer.");
        return POUND_ERROR_UNSUPPORTED_FEATURE;
    }

    if ((POUND_TITLE_NRO != info->type) && (POUND_TITLE_NSO != info->type))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing title type %d: only uncompressed NRO and NSO images are bootable here.",
                        (int)info->type);
        return POUND_ERROR_UNSUPPORTED_FEATURE;
    }

    pound_title_info_t detected;
    error_t result = pound_title_detect_reader(reader, &detected);
    if (POUND_SUCCESS != result)
    {
        return result;
    }

    if (detected.type != info->type)
    {
        POUND_LOG_ERROR(&thread_logger, "Title type changed between detection and boot.");
        return POUND_ERROR_MALFORMED_HEADER;
    }

    pound_title_layout_t layout;
    result = pound_title_parse_reader(reader, detected.type, &layout);
    if (POUND_SUCCESS != result)
    {
        return result;
    }

    if ((layout.segments[0].size < 4U) || (layout.entry_offset >= layout.image_size))
    {
        POUND_LOG_ERROR(&thread_logger, "Refusing image: it has no executable text or no entry point.");
        return POUND_ERROR_MALFORMED_HEADER;
    }

    uint64_t image_extent = 0U;
    if (false == align_up_u64(layout.image_size, POUND_BOOT_PAGE_SIZE, &image_extent))
    {
        POUND_LOG_ERROR(&thread_logger, "Refusing image: its extent wraps the address space.");
        return POUND_ERROR_MALFORMED_HEADER;
    }

    // `hos_memory_layout_init` splits the range at its midpoint, rounded up to a
    // page. Both bounds and the size are page-aligned, so that midpoint is
    // exactly half of this (even-page) range.
    const uint64_t map_limit = (state->guest_end - state->guest_base) / 2U;

    if (state->stack_size >= map_limit)
    {
        POUND_LOG_ERROR(&thread_logger, "Refusing image: the stack reservation leaves no map area.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const uint64_t stack_base = (state->guest_base + map_limit) - state->stack_size;
    if (image_extent > (stack_base - state->guest_base))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing image: 0x%llX bytes do not fit below the stack at 0x%llX.",
                        (unsigned long long)image_extent,
                        (unsigned long long)stack_base);
        return POUND_ERROR_MALFORMED_HEADER;
    }

    // Allocate and zero the window only once a real image has passed every
    // format check. A title that is refused above costs no guest memory.
    uint8_t *host_base = (uint8_t *)malloc(state->host_size);
    if (NULL == host_base)
    {
        POUND_LOG_ERROR(&thread_logger, "Allocating %zu bytes for guest memory failed.", state->host_size);
        return POUND_ERROR_ALLOCATION_FAILED;
    }
    memset(host_base, 0, state->host_size);

    state->host_base        = host_base;
    state->owns_host_buffer = true;

    result = guest_memory_init(&state->guest_memory, host_base, state->host_size, state->guest_base);
    if (POUND_SUCCESS == result)
    {
        result = hos_memory_layout_init(&state->memory_layout,
                                        &state->guest_memory,
                                        state->guest_base,
                                        state->guest_end);
    }

    if (POUND_SUCCESS != result)
    {
        POUND_LOG_ERROR(&thread_logger, "Preparing the guest address space failed: %s.",
                        pound_error_to_string(result));
        boot_release_buffer(state);
        state->phase = POUND_BOOT_PHASE_FAILED;
        return result;
    }

    state->image_base = state->guest_base;
    state->image_end  = state->guest_base + image_extent;
    state->stack_base = stack_base;
    state->stack_top  = state->guest_base + map_limit;

    uint64_t mapped_end = 0U;

    // Text: read and execute. The header is part of the segment, exactly as the
    // format says, so the entry point is the first byte after the NRO start block
    // or the NSO text memory offset.
    result = boot_map_segment(state,
                              reader,
                              &layout.segments[0],
                              HOS_MEMORY_STATE_CODE,
                              GUEST_MEMORY_PERM_READ | GUEST_MEMORY_PERM_EXEC,
                              &mapped_end);

    // Rodata: read-only, never executable.
    if (POUND_SUCCESS == result)
    {
        result = boot_map_segment(state,
                                  reader,
                                  &layout.segments[1],
                                  HOS_MEMORY_STATE_CODE,
                                  GUEST_MEMORY_PERM_READ,
                                  &mapped_end);
    }

    // Data: writable, never executable.
    if (POUND_SUCCESS == result)
    {
        result = boot_map_segment(state,
                                  reader,
                                  &layout.segments[2],
                                  HOS_MEMORY_STATE_NORMAL,
                                  GUEST_MEMORY_PERM_READ | GUEST_MEMORY_PERM_WRITE,
                                  &mapped_end);
    }

    if (POUND_SUCCESS == result)
    {
        uint64_t data_end = 0U;
        if (false == add_u64(layout.segments[2].memory_offset, layout.segments[2].size, &data_end))
        {
            result = POUND_ERROR_MALFORMED_HEADER;
        }
        else
        {
            uint64_t bss_payload_end = 0U;
            if (false == add_u64(data_end, layout.bss_size, &bss_payload_end))
            {
                result = POUND_ERROR_MALFORMED_HEADER;
            }
            else
            {
                uint64_t bss_start = mapped_end;
                if (bss_start < align_down_u64(data_end, POUND_BOOT_PAGE_SIZE))
                {
                    bss_start = align_down_u64(data_end, POUND_BOOT_PAGE_SIZE);
                }

                uint64_t bss_end = 0U;
                if (false == align_up_u64(bss_payload_end, POUND_BOOT_PAGE_SIZE, &bss_end))
                {
                    result = POUND_ERROR_MALFORMED_HEADER;
                }
                else if (bss_end > bss_start)
                {
                    result = hos_memory_layout_map(&state->memory_layout,
                                                   state->guest_base + bss_start,
                                                   bss_end - bss_start,
                                                   HOS_MEMORY_STATE_NORMAL,
                                                   GUEST_MEMORY_PERM_READ | GUEST_MEMORY_PERM_WRITE,
                                                   0U);
                }
            }
        }
    }

    if (POUND_SUCCESS == result)
    {
        result = hos_memory_layout_map(&state->memory_layout,
                                       state->stack_base,
                                       state->stack_size,
                                       HOS_MEMORY_STATE_STACK,
                                       GUEST_MEMORY_PERM_READ | GUEST_MEMORY_PERM_WRITE,
                                       0U);
    }

    if (POUND_SUCCESS != result)
    {
        POUND_LOG_ERROR(&thread_logger, "Mapping the title image failed: %s.", pound_error_to_string(result));
        boot_release_buffer(state);
        state->phase = POUND_BOOT_PHASE_FAILED;
        return result;
    }


    memset(&state->guest_state, 0, sizeof(state->guest_state));
    state->guest_state.pc     = state->guest_base + layout.entry_offset;
    state->guest_state.x[31]  = state->stack_top - 16U;
    state->guest_state.x[30]  = 0U;
    state->guest_state.x[29]  = 0U;

    state->title  = *info;
    state->layout = layout;
    state->switch2_mode = false;
    state->phase  = POUND_BOOT_PHASE_READY;

    POUND_LOG_INFO(&thread_logger,
                   "Boot ready: entry 0x%llX, SP 0x%llX, image [0x%llX, 0x%llX), stack [0x%llX, 0x%llX).",
                   (unsigned long long)state->guest_state.pc,
                   (unsigned long long)state->guest_state.x[31],
                   (unsigned long long)state->image_base,
                   (unsigned long long)state->image_end,
                   (unsigned long long)state->stack_base,
                   (unsigned long long)state->stack_top);
    return POUND_SUCCESS;
}

error_t
pound_boot_load_title(pound_boot_state_t *POUND_RESTRICT state, const pound_title_info_t *POUND_RESTRICT info)
{
    if (POUND_UNLIKELY(NULL == state) || POUND_UNLIKELY(NULL == info))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting boot load: state=%p, info=%p.",
                        (const void *)state, (const void *)info);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!state->initialized)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting boot load: boot state is not initialized.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    fs_file_t file;
    error_t   result = fs_file_open(&file, info->path);
    if (POUND_SUCCESS != result)
    {
        return result;
    }

    fs_reader_t reader;
    result = fs_file_reader(&file, &reader);
    if (POUND_SUCCESS == result)
    {
        result = boot_load_reader(state, info, &reader);
    }

    fs_file_close(&file);
    return result;
}

error_t
pound_boot_load_memory(pound_boot_state_t *POUND_RESTRICT state, const char *POUND_RESTRICT label,
                       const uint8_t *POUND_RESTRICT data, const size_t size)
{
    if (POUND_UNLIKELY(NULL == state) || POUND_UNLIKELY(NULL == data) || (0U == size))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting boot load: memory image is empty or NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    fs_reader_t reader;
    fs_reader_from_buffer(&reader, data, size);

    pound_title_info_t info;
    memset(&info, 0, sizeof(info));
    info.type             = POUND_TITLE_NSO;
    info.format_version   = 1U;
    info.is_bootable      = true;
    info.is_encrypted     = false;
    info.requires_keys    = false;

    if (NULL != label)
    {
        snprintf(info.path, sizeof(info.path), "%s", label);
    }

    return boot_load_reader(state, &info, &reader);
}

error_t
pound_boot_start(pound_boot_state_t *POUND_RESTRICT state)
{
    if (POUND_UNLIKELY(NULL == state))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting boot start: state is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!state->initialized)
    {
        return POUND_ERROR_NOT_INITIALIZED;
    }

    if (POUND_BOOT_PHASE_READY != state->phase)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting boot start: the title is not ready (%d).", (int)state->phase);
        return POUND_ERROR_INVALID_STATE;
    }

    state->phase = POUND_BOOT_PHASE_RUNNING;
    POUND_LOG_INFO(&thread_logger, "Boot: starting guest execution at 0x%llX.",
                   (unsigned long long)state->guest_state.pc);
    return POUND_SUCCESS;
}

error_t
pound_boot_run(pound_boot_state_t *POUND_RESTRICT state,
               hos_syscall_context_t *POUND_RESTRICT context,
               const uint32_t budget,
               uint32_t *POUND_RESTRICT executed,
               interpreter_trap_t *POUND_RESTRICT trap_out)
{
    if (POUND_UNLIKELY(NULL == state))
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting boot run: state is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!state->initialized)
    {
        return POUND_ERROR_NOT_INITIALIZED;
    }

    if (POUND_BOOT_PHASE_RUNNING != state->phase)
    {
        POUND_LOG_ERROR(&thread_logger, "Rejecting boot run: the title is not running (%d).", (int)state->phase);
        return POUND_ERROR_INVALID_STATE;
    }

    uint32_t total_executed = 0U;

    while (total_executed < budget)
    {
        uint32_t              slice_executed = 0U;
        interpreter_trap_t    trap;

        const error_t run_error = interpreter_run(&state->guest_state,
                                                 &state->guest_memory,
                                                 budget - total_executed,
                                                 &slice_executed,
                                                 &trap);

        total_executed += slice_executed;

        if (POUND_SUCCESS != run_error)
        {
            if (NULL != executed)
            {
                *executed = total_executed;
            }

            state->phase = POUND_BOOT_PHASE_FAILED;
            POUND_LOG_ERROR(&thread_logger, "Guest execution failed at 0x%llX: %s.",
                            (unsigned long long)state->guest_state.pc,
                            pound_error_to_string(run_error));
            return run_error;
        }

        if (INTERPRETER_TRAP_NONE == trap.kind)
        {
            if (0U == slice_executed)
            {
                // A zero-budget slice that reports no trap would otherwise spin
                // forever. It should be impossible, but refusing is cheaper than
                // making a runaway emulator the way this is discovered.
                state->phase = POUND_BOOT_PHASE_FAILED;
                return POUND_ERROR_INVALID_STATE;
            }

            break;
        }

        if (INTERPRETER_TRAP_SVC == trap.kind)
        {
            if (NULL == context)
            {
                if (NULL != trap_out)
                {
                    *trap_out = trap;
                }

                if (NULL != executed)
                {
                    *executed = total_executed;
                }

                return POUND_SUCCESS;
            }

            const error_t call_error = hos_syscall_dispatch(context, (uint32_t)trap.imm, &state->guest_state);

            if (POUND_SUCCESS != call_error)
            {
                if (NULL != executed)
                {
                    *executed = total_executed;
                }

                if (NULL != trap_out)
                {
                    *trap_out = trap;
                }

                state->phase = POUND_BOOT_PHASE_FAILED;
                POUND_LOG_ERROR(&thread_logger, "System call 0x%llX failed: %s.",
                                (unsigned long long)trap.imm,
                                pound_error_to_string(call_error));
                return call_error;
            }

            if (HOS_SYSCALL_COMPLETED == context->outcome)
            {
                continue;
            }

            state->phase = POUND_BOOT_PHASE_STOPPED;

            if (NULL != trap_out)
            {
                *trap_out = trap;
            }

            if (NULL != executed)
            {
                *executed = total_executed;
            }

            return POUND_SUCCESS;
        }

        if (INTERPRETER_TRAP_UNDEFINED == trap.kind)
        {
            if (NULL != executed)
            {
                *executed = total_executed;
            }

            if (NULL != trap_out)
            {
                *trap_out = trap;
            }

            state->phase = POUND_BOOT_PHASE_FAILED;
            POUND_LOG_ERROR(&thread_logger, "Guest executed an instruction the interpreter does not implement at 0x%llX.",
                            (unsigned long long)trap.pc);
            return POUND_ERROR_UNSUPPORTED_INSTRUCTION;
        }

        // BRK and HLT are deliberate halts. They stop the boot but are not a
        // failure: a debugger or a test may be waiting for exactly this trap.
        state->phase = POUND_BOOT_PHASE_STOPPED;

        if (NULL != trap_out)
        {
            *trap_out = trap;
        }

        if (NULL != executed)
        {
            *executed = total_executed;
        }

        return POUND_SUCCESS;
    }

    if (NULL != executed)
    {
        *executed = total_executed;
    }

    if (NULL != trap_out)
    {
        memset(trap_out, 0, sizeof(*trap_out));
    }

    return POUND_SUCCESS;
}

bool
pound_boot_is_running(const pound_boot_state_t *POUND_RESTRICT state)
{
    return (NULL != state) && state->initialized && (POUND_BOOT_PHASE_RUNNING == state->phase);
}

void
pound_boot_shutdown(pound_boot_state_t *POUND_RESTRICT state)
{
    if (NULL == state)
    {
        return;
    }

    boot_release_buffer(state);
    memset(state, 0, sizeof(*state));
}

const guest_state_t *
pound_boot_get_guest_state(const pound_boot_state_t *POUND_RESTRICT state)
{
    if ((NULL == state) || !state->initialized)
    {
        return NULL;
    }

    return &state->guest_state;
}

/*** end of file ***/

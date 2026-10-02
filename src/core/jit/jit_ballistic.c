#include "jit_ballistic.h"

#include "log.h"
#include <string.h>

/// Recovers the cache from Ballistic's opaque handle.
///
/// `bal_allocator_handle_t` is an opaque pointer the engine only ever passes back to
/// the callbacks it was given, and the cache is what was installed there, so the
/// round trip is exact.
static jit_cache_t *
handle_cache(bal_allocator_handle_t handle)
{
    return (jit_cache_t *)handle;
}

/// Returns the address a `bal_executable_buffer_t` refers to.
///
/// Ballistic populates both fields and this implementation sets them equal, but the
/// callbacks are handed buffers the engine may have copied through, so preferring
/// `rw_pointer` and falling back to `rx_pointer` stays correct whether or not the
/// two alias. Returning NULL for a wholly empty buffer is the engine's own
/// convention, not an error.
static void *
buffer_address(const bal_executable_buffer_t buffer)
{
    if (NULL != buffer.rw_pointer)
    {
        return buffer.rw_pointer;
    }

    return buffer.rx_pointer;
}

/// Brings a request up to the cache's minimum alignment, where the cache's
/// minimum is above what Ballistic asked for.
///
/// Ballistic only promises a power of two, and it is free to ask for 1. The cache
/// refuses an alignment below `JIT_CACHE_MIN_ALIGNMENT` because every descriptor it
/// keeps is pointer aligned, so a sub-16 request has nowhere to go. Rounding the
/// request *up* to the cache's default is not the same as the misalignment
/// rounding the cache refuses elsewhere: a stronger guarantee always satisfies a
/// weaker request, and it is the only direction that is safe.
static size_t
bridge_alignment(const size_t alignment)
{
    if (alignment < JIT_CACHE_MIN_ALIGNMENT)
    {
        return JIT_CACHE_DEFAULT_ALIGNMENT;
    }

    return alignment;
}

static void *
bal_pound_allocate(bal_allocator_handle_t allocator, const size_t alignment, const size_t size)
{
    if (POUND_UNLIKELY(NULL == allocator))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Ballistic asked for a %zu-byte block with no allocator context.", size);
        return NULL;
    }

    return jit_cache_alloc_aligned(handle_cache(allocator), bridge_alignment(alignment), size);
}

static void
bal_pound_free(bal_allocator_handle_t allocator, void *pointer, const size_t size)
{
    if (POUND_UNLIKELY(NULL == allocator))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Ballistic asked to release %zu bytes at %p with no allocator context.",
                        size,
                        pointer);
        return;
    }

    // `size` is deliberately unused. The cache knows every block's true capacity,
    // and trusting a caller-supplied size instead would let a stale one corrupt the
    // free list. The parameter is only read to make the log line above meaningful
    // on the failure path.
    jit_cache_free(handle_cache(allocator), pointer);
}

static bal_executable_buffer_t
bal_pound_allocate_executable(bal_allocator_handle_t allocator,
                              const size_t            alignment,
                              const size_t            size)
{
    // The interface requires both pointers to be NULL on failure, so this is
    // initialised before anything can go wrong and only ever assigned on success.
    bal_executable_buffer_t buffer;
    buffer.rw_pointer = NULL;
    buffer.rx_pointer = NULL;

    if (POUND_UNLIKELY(NULL == allocator))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Ballistic asked for a %zu-byte executable block with no allocator "
                        "context.",
                        size);
        return buffer;
    }

    void *const block = jit_cache_alloc_executable_aligned(handle_cache(allocator),
                                                           bridge_alignment(alignment),
                                                           size);

    if (POUND_UNLIKELY(NULL == block))
    {
        return buffer;
    }

    // The cache returns memory that is writable and *not* executable, and that is
    // exactly what the interface asks for: the engine writes the block's machine
    // code next and then calls `protect_rx`.
    buffer.rw_pointer = block;
    buffer.rx_pointer = block;
    return buffer;
}

static void
bal_pound_free_executable(bal_allocator_handle_t allocator,
                          bal_executable_buffer_t buffer,
                          const size_t            size)
{
    if (POUND_UNLIKELY(NULL == allocator))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Ballistic asked to release a %zu-byte executable block with no "
                        "allocator context.",
                        size);
        return;
    }

    void *const block = buffer_address(buffer);

    if (POUND_UNLIKELY(NULL == block))
    {
        // Ballistic's own contract is that both pointers are NULL on a failed
        // allocation, so it should not then call this. Reporting it beats silently
        // accepting a release for memory this cache never handed out.
        POUND_LOG_ERROR(&thread_logger,
                        "Ballistic asked to release a %zu-byte executable block whose buffer has "
                        "no address in it. Either the allocation failed and the engine is "
                        "releasing it anyway, or the buffer was not one this cache produced.",
                        size);
        return;
    }

    jit_cache_free_executable(handle_cache(allocator), block);
}

static void
bal_pound_protect_rw(bal_allocator_handle_t allocator, bal_executable_buffer_t buffer, const size_t size)
{
    if (POUND_UNLIKELY(NULL == allocator))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Ballistic asked to make a %zu-byte buffer writable with no allocator "
                        "context.",
                        size);
        return;
    }

    void *const block = buffer_address(buffer);

    if (POUND_UNLIKELY(NULL == block))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Ballistic asked to make a %zu-byte buffer writable, but the buffer has no "
                        "address in it.",
                        size);
        return;
    }

    const error_t result = jit_cache_protect_rw(handle_cache(allocator), block);

    if (POUND_UNLIKELY(POUND_SUCCESS != result))
    {
        // The cache has already logged the platform-level cause. This line records
        // that the engine -- not Pound's own JIT -- is what asked, which is the
        // difference between a cache bug and an integration bug.
        POUND_LOG_ERROR(&thread_logger,
                        "Ballistic's protect_rw on the %zu-byte block at %p failed with \"%s\". The "
                        "block stays read-execute, so any patch the engine was about to write will "
                        "fault rather than silently vanish.",
                        size,
                        block,
                        pound_error_to_string(result));
    }
}

static void
bal_pound_protect_rx(bal_allocator_handle_t allocator, bal_executable_buffer_t buffer, const size_t size)
{
    if (POUND_UNLIKELY(NULL == allocator))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Ballistic asked to make a %zu-byte buffer executable with no allocator "
                        "context.",
                        size);
        return;
    }

    void *const block = buffer_address(buffer);

    if (POUND_UNLIKELY(NULL == block))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Ballistic asked to make a %zu-byte buffer executable, but the buffer has "
                        "no address in it.",
                        size);
        return;
    }

    const error_t result = jit_cache_protect_rx(handle_cache(allocator), block);

    if (POUND_UNLIKELY(POUND_SUCCESS != result))
    {
        // This is the failure that matters most. The block holds machine code that
        // the engine is about to jump to, and if it is still read-write the jump
        // succeeds on every platform Pound targets and then the CPU executes
        // whatever policy the host applied -- on a hardened Linux or Android build,
        // nothing at all.
        POUND_LOG_ERROR(&thread_logger,
                        "Ballistic's protect_rx on the %zu-byte block at %p failed with \"%s\". The "
                        "block stays read-write, so the engine must not dispatch to it.",
                        size,
                        block,
                        pound_error_to_string(result));
    }
}

error_t
jit_cache_bind_ballistic(jit_cache_t *POUND_RESTRICT cache, bal_allocator_t *POUND_RESTRICT out_allocator)
{
    if (POUND_UNLIKELY((NULL == cache) || (NULL == out_allocator)))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: cache or output allocator is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!cache->initialised)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing to bind Ballistic to a code cache that was never initialised.");
        return POUND_ERROR_NOT_INITIALIZED;
    }

    memset(out_allocator, 0, sizeof(*out_allocator));

    out_allocator->context             = (bal_allocator_handle_t)cache;
    out_allocator->allocate            = bal_pound_allocate;
    out_allocator->free                = bal_pound_free;
    out_allocator->allocate_executable = bal_pound_allocate_executable;
    out_allocator->free_executable     = bal_pound_free_executable;
    out_allocator->protect_rw          = bal_pound_protect_rw;
    out_allocator->protect_rx          = bal_pound_protect_rx;

    POUND_LOG_INFO(&thread_logger,
                   "Ballistic is bound to the W^X code cache: %zu-byte chunks under a %zu-byte "
                   "ceiling, %zu-byte pages, poison %s.",
                   cache->chunk_bytes,
                   cache->max_bytes,
                   cache->page_size,
                   cache->poison ? "on" : "off");

    return POUND_SUCCESS;
}

error_t
jit_cache_executable_buffer(const jit_cache_t *POUND_RESTRICT cache,
                            void *POUND_RESTRICT                  block,
                            bal_executable_buffer_t *POUND_RESTRICT out_buffer)
{
    if (POUND_UNLIKELY((NULL == cache) || (NULL == block) || (NULL == out_buffer)))
    {
        POUND_LOG_ERROR(&thread_logger, "Ignoring call: cache, block or output buffer is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    jit_block_info_t info;

    memset(&info, 0, sizeof(info));

    const error_t described = jit_cache_get_block_info(cache, block, &info);

    if (POUND_SUCCESS != described)
    {
        return described;
    }

    if (JIT_BLOCK_KIND_EXECUTABLE != (jit_block_kind_t)info.kind)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing to describe %p as an executable buffer: it is a data block, whose "
                        "protection is never flipped.",
                        block);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    out_buffer->rw_pointer = block;
    out_buffer->rx_pointer = block;
    return POUND_SUCCESS;
}

/*** end of file ***/

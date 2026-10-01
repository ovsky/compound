#ifndef POUND_MEMORY_H
#define POUND_MEMORY_H

#include "attributes.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Identifies the physical heap a block of memory was carved out of.
///
/// The host heap is the only heap currently implemented, but the enum is
/// retained so that Ballistic's W^X executable pools and the Horizon OS
/// slab allocator can be added without churning every call site.
typedef enum
{
    MEMORY_HEAP_TYPE_HOST,
    MEMORY_HEAP_TYPE_JIT_EXECUTABLE,
    MEMORY_HEAP_TYPE_DEBUG_PROFILING,
    MEMORY_HEAP_TYPE_COUNT,
} memory_heap_type_t;

/// Logical ownership tag used for memory accounting.
///
/// The bucket is thread-local. Every allocation is charged to the bucket that
/// is active on the calling thread at the time of the call, and every free is
/// credited back to the bucket that is active at the time of the free. Callers
/// that free across a bucket boundary (for example, a block allocated on a
/// worker thread and released on the main thread) will skew the per-bucket
/// totals; `memory_subsystem_get_memory_used_by_bucket` saturates at zero so
/// such skew can never underflow the accounting.
typedef enum
{
    MEMORY_BUCKET_NONE,
    MEMORY_BUCKET_UI,
    MEMORY_BUCKET_GUEST_MEMORY,
    MEMORY_BUCKET_JIT_RECOMPILER,
    MEMORY_BUCKET_DEBUG_PROFILING,
    MEMORY_BUCKET_COUNT,
} memory_bucket_type_t;

/// Sentinel accepted by `memory_subsystem_get_memory_used_by_bucket` to request
/// the sum across every bucket.
#define MEMORY_BUCKET_TOTAL (-1)

typedef struct memory_allocator memory_allocator_t;

/// Initialises the memory subsystem for the calling thread.
///
/// Must be called before any other `memory_subsystem_*` entry point. Calling
/// it again is harmless and simply rebinds the thread to the host allocator.
void memory_subsystem_init(void);

/// Tears the memory subsystem down for the calling thread.
///
/// Every subsequent allocation on this thread is refused (returns `NULL`) and
/// every free is ignored, rather than dereferencing a dangling allocator.
void memory_subsystem_destroy(void);

/// Swaps the calling thread's allocator, returning the previous one.
///
/// Passing `NULL` detaches the thread from the subsystem.
memory_allocator_t *memory_subsystem_set_allocator(memory_allocator_t *POUND_RESTRICT allocator);

/// Returns the calling thread's current allocator, or `NULL` when detached.
memory_allocator_t *memory_subsystem_get_allocator(void);

/// Allocates `bytes` bytes aligned to `alignment`.
///
/// `alignment` must be a power of two. Returns `NULL` on invalid arguments,
/// when the thread is detached from the subsystem, or on allocation failure.
void *memory_subsystem_allocate(size_t alignment, size_t bytes);

/// Frees a pointer previously returned by `memory_subsystem_allocate`.
///
/// Passing `NULL` is a no-op.
void memory_subsystem_free(void *POUND_RESTRICT pointer);

/// Charges subsequent allocations on this thread to `bucket`, returning the
/// previously active bucket.
///
/// Out-of-range buckets are clamped into `[0, MEMORY_BUCKET_COUNT - 1]`.
memory_bucket_type_t memory_subsystem_set_bucket(memory_bucket_type_t bucket);

/// Returns the bucket currently charged on this thread.
memory_bucket_type_t memory_subsystem_get_bucket(void);

/// Returns the usable size of `pointer`, or `0` for `NULL`.
size_t memory_subsystem_get_usable_size(const void *POUND_RESTRICT pointer);

/// Returns the bytes charged to `bucket`.
///
/// Pass `MEMORY_BUCKET_TOTAL` (or any negative value) for the sum across every
/// bucket. Out-of-range positive values return `0`.
size_t memory_subsystem_get_memory_used_by_bucket(int bucket);

/// Returns the total bytes reserved by the host heap.
size_t memory_subsystem_get_heap_size(void);

/// Returns `true` when `bucket` names a real bucket.
bool memory_bucket_is_valid(memory_bucket_type_t bucket);

extern memory_allocator_t g_host_allocator;

#endif // POUND_MEMORY_H

/*** end of file ***/

#ifndef POUND_LOADER_BOOT_BOOT_H
#define POUND_LOADER_BOOT_BOOT_H

//! Boot state: map a validated NRO/NSO image into guest memory and prepare the
//! register file for the interpreter.
//!
//! The boot layer is deliberately the only place that knows both halves of the
//! loader contract. `title_loader` says where the bytes are in the file;
//! `hos_memory_layout` says where they belong in the guest address space; this
//! module copies the bytes, maps regions with the rights the image asks for,
//! creates a stack, and writes the initial PC and SP.
//!
//! ## What is refused
//!
//! NCA, NSP and XCI are not executable images: they are packages whose content
//! is encrypted and normally compressed. This build has no Nintendo keys and no
//! content decryption, so `pound_boot_load_title` refuses them with
//! `POUND_ERROR_UNSUPPORTED_FEATURE`. It does not map the package's outer
//! filesystem and call that a boot. LZ4-compressed NSO segments are refused for
//! the same reason.
//!
//! ## Address space
//!
//! A boot owns one host buffer and partitions it into Horizon's map and heap
//! halves with `hos_memory_layout_init`. The image starts at `guest_base`; the
//! stack occupies the top of the map half. The heap half is left `Free` for a
//! later `svcSetHeapSize`.

#include "attributes.h"
#include "cpu/interpreter.h"
#include "errors.h"
#include "guest_state.h"
#include "hos/memory_layout.h"
#include "hos/syscall.h"
#include "loader/title_loader.h"
#include "memory/guest_memory.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Guest address of the first byte of a default boot's address space.
#define POUND_BOOT_DEFAULT_GUEST_BASE UINT64_C(0x80000000)

/// Default address space: 128 MiB, split evenly between the image map and heap.
#define POUND_BOOT_DEFAULT_ADDRESS_SPACE_SIZE (UINT64_C(128) * 1024U * 1024U)

/// Horizon pages are 4 KiB.
#define POUND_BOOT_PAGE_SIZE UINT64_C(0x1000)

/// Default stack reservation at the top of the map half.
#define POUND_BOOT_DEFAULT_STACK_SIZE (UINT64_C(1) * 1024U * 1024U)

typedef enum
{
    POUND_BOOT_PHASE_IDLE = 0,
    POUND_BOOT_PHASE_READY,
    POUND_BOOT_PHASE_RUNNING,
    POUND_BOOT_PHASE_STOPPED,
    POUND_BOOT_PHASE_FAILED,
} pound_boot_phase_t;

typedef struct
{
    uint64_t guest_base;
    uint64_t address_space_size;
    uint64_t stack_size;
} pound_boot_config_t;

typedef struct
{
    bool initialized;
    pound_boot_phase_t phase;

    /// Always false in this build. A future Switch 2 execution layer would set
    /// it only after it had actually decoded a Switch 2 title.
    bool switch2_mode;
    bool owns_host_buffer;

    pound_title_info_t   title;
    pound_title_layout_t layout;

    guest_memory_t      guest_memory;
    hos_memory_layout_t memory_layout;
    guest_state_t       guest_state;

    uint8_t *host_base;
    size_t   host_size;

    uint64_t guest_base;
    uint64_t guest_end;
    uint64_t image_base;
    uint64_t image_end;
    uint64_t stack_base;
    uint64_t stack_size;
    uint64_t stack_top;
} pound_boot_state_t;

/// Fills `config` with the default base, size and stack reservation.
void pound_boot_config_default(pound_boot_config_t *POUND_RESTRICT config);

/// Initializes a boot with the default configuration.
error_t pound_boot_init(pound_boot_state_t *POUND_RESTRICT state);

/// Initializes a boot with an explicit address-space layout.
///
/// `guest_base` and `address_space_size` must be page-aligned; the size must be
/// at least four pages and large enough for the stack plus an image. The caller
/// owns `state`; the function owns the host buffer it allocates.
error_t pound_boot_init_with_config(pound_boot_state_t *POUND_RESTRICT state,
                                    const pound_boot_config_t *POUND_RESTRICT config);

/// Loads and maps an NRO or NSO. The state must be freshly initialized or idle.
///
/// On failure the state is marked `POUND_BOOT_PHASE_FAILED`; reinitialize it to
/// retry, so a partially mapped image is never presented as ready.
error_t pound_boot_load_title(pound_boot_state_t *POUND_RESTRICT state,
                              const pound_title_info_t *POUND_RESTRICT info);

/// Loads and maps an NSO already held in memory.
///
/// This is the package path: decryption produces an ExeFS `main` NSO in a buffer,
/// and writing it to a temporary file just to parse it would leave decrypted code
/// on disk. The bytes are parsed exactly as `pound_boot_load_title` parses an NSO
/// file, including compressed segments. `label` is only for logs and `state->title`.
error_t pound_boot_load_memory(pound_boot_state_t *POUND_RESTRICT state, const char *POUND_RESTRICT label,
                               const uint8_t *POUND_RESTRICT data, size_t size);

/// Moves a READY state to RUNNING. It does not call the interpreter; that is
/// the caller's next step, and `pound_boot_is_running` reports the state.
error_t pound_boot_start(pound_boot_state_t *POUND_RESTRICT state);

/// Runs the guest for up to `budget` instructions.
///
/// This is the bridge between the boot state and the interpreter. It does not
/// invent a system-call layer: when the guest executes `SVC`, the call is handed
/// to `context` -- which the caller owns and which must point at a kernel, a
/// process and the same guest memory -- exactly as `hos_syscall_dispatch` defines.
/// A completed call resumes execution; a process exit or break stops the boot.
///
/// `context` may be NULL only for a caller that deliberately wants to stop on the
/// first `SVC` and inspect the trap. In that case the SVC is reported in
/// `trap_out`, no call is dispatched, and the state is left RUNNING.
///
/// A trap that is not an SVC stops the boot with `POUND_BOOT_PHASE_STOPPED` and is
/// reported truthfully in `trap_out`; it is never stepped over. A memory fault and
/// an unsupported instruction from the interpreter are returned as errors.
///
/// `executed` may be NULL; when present it always receives the number of
/// instructions retired by this call, including a call that trapped on the first
/// instruction.
error_t pound_boot_run(pound_boot_state_t *POUND_RESTRICT state,
                       hos_syscall_context_t *POUND_RESTRICT context,
                       uint32_t budget,
                       uint32_t *POUND_RESTRICT executed,
                       interpreter_trap_t *POUND_RESTRICT trap_out);

/// Returns true only while the state is RUNNING.
bool pound_boot_is_running(const pound_boot_state_t *POUND_RESTRICT state);

/// Frees the host buffer and zeroes `state`. Safe on NULL and on an
/// uninitialized state.
void pound_boot_shutdown(pound_boot_state_t *POUND_RESTRICT state);

/// Read-only access to the prepared register file, or NULL when not ready.
const guest_state_t *pound_boot_get_guest_state(const pound_boot_state_t *POUND_RESTRICT state);

#ifdef __cplusplus
}
#endif

#endif // POUND_LOADER_BOOT_BOOT_H

/*** end of file ***/

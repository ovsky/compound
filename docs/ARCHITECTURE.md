# Architecture

How Pound is put together, and why.

- [Shape](#shape)
- [Targets and dependencies](#targets-and-dependencies)
- [Core](#core)
- [Debug GUI and hot reload](#debug-gui-and-hot-reload)
- [Memory accounting](#memory-accounting)
- [Ballistic](#ballistic)
- [Android](#android)
- [Build modules](#build-modules)

---

## Shape

Pound is a C11 emulator shell with a C++ debug GUI. The guest side now reaches
from an ARM64 interpreter through a Horizon OS kernel, system call layer and
thread scheduler; a graphics backend and the rest of the OS surface are still
missing, so nothing boots a title yet.

```
                             ┌──────────────────────────┐
                             │        main.c            │
                             │  SDL window · frame loop │
                             │  GUI lifecycle · pacing  │
                             └────┬──────────────┬──────┘
                                  │              │
              ┌───────────────────┘              └─────────────┐
              │ dlopen(PoundGui)          linked directly  │
              │ (desktop)                 (Android)         │
              ▼                                            ▼
     ┌────────────────────┐                       ┌──────────────────┐
     │      PoundGui      │                       │     PoundGui     │
     │  ImGui panels      │                       │   (STATIC)       │
     │  memory tracker    │                       │                  │
     └─────────┬──────────┘                       └────────┬─────────┘
               │  via pound_core* API                        │
               ▼                                             ▼
     ┌────────────────────────────────────────────────────────────┐
     │                        PoundCore                            │
     │  log.c      thread-local logging                           │
     │  memory.c   mimalloc wrapper + bucket accounting           │
     │  guest_memory.c  guest↔host address translation            │
     │  guest_state.c    emulated register file                   │
     │  safe_math.c      checked arithmetic                       │
     └───────────────────────────┬────────────────────────────────┘
                                 │
                       ┌─────────▼──────────┐
                       │  Ballistic::Engine  │
                       │  (prebuilt, opaque) │
                       └────────────────────┘
```

---

## Targets and dependencies

| Target | Type | Notes |
| --- | --- | --- |
| `PoundCore` | OBJECT | The emulator core. Deliberately free of SDL and ImGui. |
| `PoundGui` | SHARED (desktop) / STATIC (Android) | Loads by path on desktop; linked in on Android. |
| `imgui` | SHARED (desktop) / STATIC (Android) | Built from the vendored cimgui sources. |
| `Pound` | EXE (desktop) / SHARED `libmain.so` (Android) | The emulator itself. |
| `PoundTests` | EXE | Links `PoundCore` only. |
| `Ballistic::Engine` | IMPORTED | Prebuilt JIT, or an inert INTERFACE target when unavailable. |

`PoundCore` is an OBJECT library so the core is compiled once and linked into
both `Pound` and `PoundTests` with `POSITION_INDEPENDENT_CODE` on, which it needs
anyway to be linked into `PoundGui`.

`PoundTests` deliberately does not link SDL or ImGui. The code under test does
not touch either, and pulling them in would make the suite impossible to run in
the headless CI container or under QEMU — which is exactly where these
regressions would first be caught.

---

## Core

### Logging — `src/core/log.{h,c}`

Thread-local logger (`thread_logger`) with a pluggable sink. Levels are
`ERROR`, `WARN`, `INFO`, `DEBUG`, `TRACE`.

```c
POUND_LOG_ERROR(&thread_logger, "Aborting function: %s is NULL.", "pointer");
```

File and function name are captured at the call site, so a log line is
self-locating. The basename is stripped from the filename — correctly, which the
original implementation got wrong: it dereferenced a possibly-NULL filename and
included the leading path separator.

### Checked arithmetic — `src/core/safe_math.{h,c}`

Wrappers over Clang's checked-arithmetic builtins for `u8`/`u16`/`u32`/`u64`
and `i8`/`i16`/`i32`/`i64` addition. Returns a typed `safe_math_error_t` that
distinguishes signed-positive overflow from signed-negative overflow from
unsigned overflow, because the guest cares which one occurred.

Contract: on failure the result is **zeroed**, so a caller that ignores the
status cannot accidentally use stale data.

### Guest memory — `src/core/memory/guest_memory.{h,c}`

`guest_memory_t` is the only translation layer between guest virtual addresses
and host pointers:

```
[guest_base, guest_base + host_size)  ↔  host_base + (address - guest_base)
```

The window is half-open. `host_base` must be at least 16-byte aligned, and
`guest_base + host_size` must not wrap `UINT64_MAX`; both are checked in
`guest_memory_init`.

`guest_memory_translate_read` / `_write` return the host pointer and the number
of bytes remaining to `guest_end`. They return `NULL` for anything outside the
window. Because every JIT backend and system module reads through this one
struct, a bounds mistake here is a guest-controlled out-of-bounds access on the
host — hence the strict validation and the tests pinning the boundaries.

The translation is also where the *region gate* lives. When the `guest_memory_t`
carries a region table (a `guest_memory_region_t` array installed by
`hos/memory_layout.{h,c}`), every read, write and instruction fetch is checked
against the region containing the address, and an access the region does not
permit is refused exactly as an out-of-window one is. A `guest_memory_t` with no
table has no gate and accepts any access in its window, which is what keeps the
translation layer usable on its own. `guest_memory_translate_execute` is the
fetch-side entry point the interpreter and JIT use, so a page mapped
non-executable faults on fetch rather than on load.

### Guest state — `src/core/guest_state.{h,c}`

The emulated register file:

```
x[0..31]  64-bit general registers
pc        program counter
flag_n/z/c/v
```

`POUND_ALIGNED(64)` with explicit padding: the recompiler loads and stores this
struct by field offset, so its layout is part of the ABI between the JIT and the
guest CPU. `tests/test_guest_state.c` pins every offset as a regression guard.

### ARM64 interpreter — `src/core/cpu/interpreter.{h,c}`

`interpreter_run` is the guest CPU's reference path: it decodes and executes a
budget of ARM64 instructions directly, and it is what the JIT execution loop's
`interpret` hook binds to when a block has to run without being recompiled.
`interpreter_probe` answers whether a single word is one this module can execute,
which is what lets a caller choose the interpreter over the JIT for a block.

The two share one decoder. `a64_decoded_t` is a flat struct of already-extracted
fields — no pointers, no unions — so decoding a word costs one pass and executing
it reads only resolved values; no field position is re-derived at execute time.
An encoding the decoder rejects becomes `INTERPRETER_TRAP_UNDEFINED` rather than a
silent no-op.

The supported set is deliberately closed and spelled out in the header: the
wide-immediate moves; the add/subtract and logical families in their immediate
and shifted-register forms, including the `MOV`/`MVN`/`CMP`/`CMN`/`TST` aliases;
multiply, divide, and the variable shifts; the conditional-select family (`CSEL`,
`CSINC`, `CSINV`, `CSNEG`, and the `CSET` alias); `ADR` and `ADRP`; the four
branch shapes plus `BR`/`BLR`/`RET`; and the unsigned-offset and literal loads
and stores together with the offset-form `LDP`/`STP`. Floating point, SIMD,
atomics, and system-register access are out of scope, and an encoding outside the
set is refused by both entry points.

Register 31 is role-dependent, as the architecture requires: it is the stack
pointer where the encoding treats the operand as an address (an add/subtract
immediate operand, a load or store base) and the zero register everywhere else.
`SP` lives in `x[31]`; reading the zero register yields zero and writing it is
dropped.

The return contract separates the ways a run can end. `POUND_SUCCESS` means the
loop stopped normally — either it retired the whole budget, or it hit a trap and,
if `trap_out` was given, recorded it in `interpreter_trap_t`. An unmapped fetch
or data access returns `POUND_ERROR_MEMORY_FAULT` with `pc` left on the faulting
instruction and `*executed` counting only the instructions that retired before
it. `SVC` is reported as a syscall trap, carrying its immediate, which is what
`hos_syscall_dispatch` takes — the trap and the call are joined by a number rather
than by a shared type; `BRK` and `HLT` share the breakpoint kind, being the two
deliberate halts. A `NULL` `state`, `memory`, or `executed`
is `POUND_ERROR_INVALID_ARGUMENT`; a `NULL` `trap_out` is allowed and simply
discards the reason.

`tests/test_interpreter.c` drives it with hand-verified instruction words (33
cases), covering each family plus the trap, fault, budget, and `probe` contracts.

### SM86 → SPIR-V translation — `src/core/gpu/sm86/`

The SM86 module translates NVIDIA SM86 shader microcode to SPIR-V. It is split
across decoder, opcode metadata, an intermediate representation (IR), a SPIR-V
emitter, and a translate entry point. The decoder and generated opcode tables are
ported from the upstream pound-emu reference and extended with an SoA IR and a
header-driven SPIR-V emitter. The translation produces a self-contained SPIR-V
function using CB0–CB15 Uniform blocks for constant buffers and supports a small
subset of opcodes (NOP, MOV, IADD3, IMAD, ISETP, LDC, EXIT, KILL) with predicated
forms, returning `POUND_ERROR_UNSUPPORTED_INSTRUCTION` for everything else. The
module is built into `PoundCore` and covered by `tests/test_sm86.c` (34 cases).

### Horizon OS kernel — `src/core/hos/kernel.{h,c}`

Horizon OS does not expose its kernel as calls that mutate global state. It exposes
a *namespace of handles*, and every subsystem a title talks to — the filesystem, the
applet manager, the GPU driver — is reached by connecting to a named service and then
sending that service numbered requests. A handle is the unit of authority: a title can
do exactly what its handles let it do.

This module is that layer and nothing above it. It owns three things. **Objects** are
kernel-side things a handle can name; this slice implements events, sessions and threads,
they live in one kernel-wide table, and they are reference-counted by the number of handles
that name them, so an object outlives any one process's handle and dies only when the
last one closes. **Handles** are one process's names for objects, so the same object has
a different handle in each process that holds one. **Services** are the named request
handlers a session can be opened against.

A thread is an object with a register file, a priority and a lifecycle, and it is the one
object with two names on purpose. A **handle** names it inside one process and is what a
guest may use — a title can only reach the threads its own handle table names. A **thread
identifier** names it across the whole kernel, is never reused, and outlives the handle;
that is what the scheduler runs on, because a title may close the handle of a thread that
is still running and the thread keeps running. Starting a thread takes a reference on its
own object, so closing that last handle cannot free a register file out from under a live
thread. The `hos_kernel_thread_*` calls take identifiers and enforce no per-process
authority, because they are the emulator's own view of a thread rather than anything a
guest can reach.

A thread also carries one piece of pure request state: the cancellation flag that
`svcCancelSynchronization` raises when the target is *not* in a synchronization call. It
lives on the object, not in the scheduler, because it belongs to the thread rather than to
the current wait — it survives until that thread synchronizes or dies, and it applies to a
thread the scheduler has never admitted. It is a flag and not a counter, so two requests
before the next wait interrupt one call, exactly as Horizon's `bool` does. The request that
lands on a thread which *is* parked is the scheduler's to end, and is described there.

A handle is `(generation << 20) | (index + 1)`, with zero reserved for "no object".
Closing a handle frees its slot and advances the slot's generation, so a handle that
outlived its object stops resolving instead of silently naming whatever lands in the
slot next. Without that, a copied-around handle would eventually read another title's
data through a name it thought it had given back — the worst kind of failure, because
it is not a crash.

Objects, services, and every process's handle table share one kernel mutex. Splitting
it would give handle operations a lock order the object path would have to take in
reverse, for no gain: nothing here is on the JIT dispatch path. The one place the lock
is dropped is a service handler, which is guest policy and routinely wants to signal an
event or send the next request, so it is called with the kernel free for re-entry.
Services are append-only, which is what makes that safe to promise — a session stores
its service's index, not a pointer to its entry, so nothing it holds can move or vanish.

Which thread runs, and what wakes one that is parked, is not decided here: this module
owns threads and never chooses between them. That is `hos/scheduler.{h,c}` below.
Shared-memory and port objects, and the Horizon IPC wire format, are out of scope and each
is refused rather than half-handled. The module adds `POUND_ERROR_INVALID_HANDLE` for a
handle that names nothing — always logged, unlike the `POUND_ERROR_NOT_FOUND` it sits
beside, because a missing service is an ordinary answer and a missing object never is.
`tests/test_hos_kernel.c` covers it with 41 cases.

### Horizon OS scheduler — `src/core/hos/scheduler.{h,c}`

`hos/kernel.{h,c}` owns threads and never decides which one runs; this module is that
decision, and the only thing in Pound that knows the clock. One scheduler is one guest
core. Threads are runnable, running or parked, and the pick is the runnable thread with the
lowest priority *number* — Horizon numbers priorities downwards, so zero is the most urgent
— with the oldest first among equals. It hands that thread to a caller-supplied run
function for one quantum, takes the result and repeats, which is what keeps this module
from having to know about the interpreter or the syscall layer.

A wait here is a *level*, not an edge. Most kernels deliver a signal by finding a waiter and
waking it then and there; this one re-reads: before every decision it looks at each parked
thread's wait set and the kernel's current answer, so signal-to-wake needs no callbacks and
cannot lose an edge it was never given. An auto-reset event is consumed in the scheduler's
poll, in priority order, which is what makes the higher-priority waiter the one that takes
it.

The clock is the system tick, 19.2 MHz, and the caller advances it by instructions retired.
A thread that stopped early — a trap, a syscall, a fault — keeps its place, because a
syscall is not a yield; only `hos_scheduler_yield` hands the core over early, and a thread
that used its whole quantum goes to the back of its priority. When nothing is runnable but
a deadline is pending, `hos_scheduler_step` jumps the clock to it rather than spinning,
which is the hardware-idle model: a core with nothing to run sleeps. `hos_scheduler_step`
reports `RAN`, `DEADLOCKED` (every remaining thread parked with no deadline and nothing
ready — a fact about the guest worth reporting) or `FINISHED` (no threads left).

The scheduler has no lock, and that is the concurrency model rather than an omission: one
scheduler is one guest core driven by one host thread, and stepping it from two host threads
at once is a caller's bug that a mutex here would not make correct.

Cancellation is the one wake that does not come from the wait set. `hos_scheduler_cancel`
finds a thread parked *on handles*, marks it runnable with a `HOS_WAKE_CANCELLED` wake and
reports that it did so; the run loop answers that call with Horizon's `0xEC01` and reports no
index. A thread that is running, runnable, sleeping or unknown is deliberately left alone and
the caller is told so, which is what lets `svcCancelSynchronization` fall back to the
kernel-side flag: Horizon interrupts a synchronization call that is in progress, and arms the
next one otherwise, and those two cases are exactly "the scheduler had a wait to end" and "it
did not". A sleep is not a synchronization call, so cancelling one changes nothing and its
clock still ends it. `tests/test_hos_scheduler.c` covers it with 24 cases.

### Horizon OS memory layout — `src/core/hos/memory_layout.{h,c}`

`guest_memory.{h,c}` is one flat host buffer with no inherent structure: any address in
the window translates to any byte. Horizon is the opposite. A process's address space is
carved into regions whose *state* (`Free`, `Code`, `Normal`, `Inaccessible`, …) and
*permission* decide what the guest may do where, and `svcQueryMemory` exists precisely so a
guest can ask. This module owns that table, and it is the single writer of the
`guest_memory_region_t` array that `guest_memory_t` points at — so the access gate inside
`guest_memory.c` and the `svcQueryMemory` answer built from the same structs cannot disagree
about where a region starts, ends, or what it permits.

The layout is optional, and that is deliberate. A `guest_memory_t` with no region table
accepts any access in its window, which is what the earlier slices and their tests assume;
installing a table turns the gate on and every access is checked against the region
containing it. `hos_memory_layout_init` occupies the window with two `Free` regions split at
its midpoint — a map region and a heap region — and refuses a window too small to hold both
as whole pages, because a layout that could not place its own split is not one to hand out.
`hos_memory_layout_map` carves a `Free` region into a used one, splitting around it, and
refuses anything the loader must not do: a range that is not page-aligned, one that leaves
the window or wraps, one that overlaps the heap region (which belongs to `svcSetHeapSize`),
or one that is not wholly inside a single `Free` region. A range that is refused is refused
before the table is touched, so a failed map leaves the address space exactly as it was.

`hos_memory_layout_set_heap_size` resizes the heap region, and the refusal vocabulary is
Horizon's rather than an invented one: a size that is not a whole number of 2 MiB pages or
that exceeds 4 GiB is `0xCA01`, a size larger than the heap region is `0xD001`. When the
region cannot be grown because the region table itself is full, the module reports
`POUND_ERROR_ALLOCATION_FAILED`, which the syscall layer maps to `0xCE01` — the one place a
slab-style refusal is reachable, and the only honest one, since it is a real host-side
exhaustion rather than a fabricated `Result`.

Two more operations change a region that already exists rather than carve out a new one, and
they are where the layout stops modelling only the *shape* of the address space and starts
modelling Horizon's own *rules* for changing it. `hos_memory_layout_set_permission` is
`svcSetMemoryPermission` and `hos_memory_layout_set_attribute` is `svcSetMemoryAttribute`.
Both take a half-open range and apply it exactly: a range that covers only part of a region
splits the region, so the bytes outside it are untouched, and a range that already has the
configuration asked for is answered without splitting, because Horizon short-circuits it
before allocating anything.

The rules are the kernel's `CheckMemoryState`. A range must lie inside the address space and
be *homogeneous* — every region it covers must agree with the others about state, permission
and attribute — so a range straddling a boundary between two differently-configured regions
is refused whole rather than half-changed. A region may be reprotected only if it carries
`FlagCanReprotect` (`0x100`) and no attribute: `Code` does not carry the flag, so making it
writable is refused rather than done, and a region carrying an attribute the caller is not
masking — a locked page, say — is not eligible either. An attribute change may set or clear
exactly the uncached bit (`0x8`), and only on a region carrying `FlagCanChangeAttribute`
(`0x1000000`); Pound's host memory has no separate cached view, so the bit lives in the table
and comes back out through `svcQueryMemory` exactly as it went in. Splitting for either
operation pre-checks the table and reports `POUND_ERROR_ALLOCATION_FAILED` — `0xCE01` again —
rather than half-making the change. `tests/test_hos_memory_layout.c` covers the module with
16 cases, including fetching through the gate from the interpreter.

### Horizon OS system calls — `src/core/hos/syscall.{h,c}`

An A64 `SVC` is a trap to a numbered kernel entry point: the immediate is the number,
`X0`–`X7` are the arguments, and `X0` carries the answer back — either as the `Result`
itself or as a pointer the callee writes the answer through. The interpreter stops on
one and reports `INTERPRETER_TRAP_SVC` with the immediate; this module is the only thing
in Pound that turns that number into work.

The whole design is one rule about what a failure looks like, and it is the rule that
keeps an emulator honest. A call can fail in two entirely different ways:

- **Horizon itself refuses it.** `svcSignalEvent` handed a session handle instead of a
  writable event is a real failure on hardware with a documented `Result` (`0xE401`).
  The call *completes*: `X0` gets that value, `pc` steps over the trap, and the guest
  branches on it exactly as it would on a Switch.
- **Pound has not implemented it.** `svcSendSyncRequest` needs the CMIF wire format and
  the thread-local IPC buffer, and `svcMapMemory` needs an unmap path the layout does not
  have yet. Neither exists. These do not complete: the call returns
  `POUND_ERROR_UNSUPPORTED_SYSCALL`, logs the number *and its name*
  (`svcSendSyncRequest (0x21)`), and leaves `X0` and `pc` alone so the trap is still
  standing to be looked at.

Collapsing the second into the first is the one thing this module must not do. A plausible
`Result` for a call nobody wrote makes a title take a branch on an answer Pound invented,
and the emulator looks like firmware that replied. A refusal is a fact about Pound; a
`Result` is a fact about the guest.

Switch system calls do not all answer in `X0`, and there are three places an answer can
land. The `Result` is always `W0`. But an *output parameter* has two more homes: a
**register** — `svcWaitSynchronization`'s ready index is `W1`, `svcSetHeapSize`'s heap base
is `X1`, `svcGetThreadId`'s identifier is `X1` — and, for the calls where the *kernel*
writes it, **through a pointer in `X0`**, as `svcQueryMemory`'s `MemoryInfo` is.

The register is what actually reaches a real guest: the kernel leaves the value in `W1`,
and the caller's own stub stores it through the out-pointer it passed (libnx's `svc.s` does
`str x0, [sp, #-16]!; svc …; ldr x2, [sp], #16; str w1, [x2]`). This module sets that
register. It *also* writes the slot through `X0` directly for the calls whose out-pointer
Pound's guests read that way, and where the kernel itself writes `X0`'s target — as with
`MemoryInfo` — it writes the whole structure in one piece so a bad pointer cannot
half-update it, and it does so *before* replacing `X0`, because afterwards the address is
gone. Where hardware's value in a register on a *failure* path is not established, the
register is left as the guest left it rather than given an invented value. A call that
returns nothing at all — `svcExitThread`, `svcSleepThread`, `svcFlushEntireDataCache` —
leaves `X0` exactly as the guest left it.

Blocking is where a context with a scheduler and one without differ, deliberately. A call
that can be satisfied now never parks a thread: `svcWaitSynchronization` and a positive
`svcSleepThread` poll first, and a wait that finds something ready — or a zero-timeout poll
that finds nothing — completes with the `Result` the hardware would. Only a poll that finds
nothing parks the caller and returns `HOS_SYSCALL_BLOCKED`, leaving `pc` on the `SVC` and
the pointer in `X0` intact: the call is suspended, not finished. The scheduler owns the
wake, and the run loop finishes the call with `hos_syscall_complete_block`, which writes the
ready index through the pointer and into `W1` for `HOS_WAKE_SIGNALED`, puts `0xEA01` in `X0`
for `HOS_WAKE_TIMED_OUT`, and touches nothing for `HOS_WAKE_SLEPT`, because a sleep returns
nothing. Only then does `pc` step past the `SVC`. The wake reason alone says which call was
parked, so the scheduler never has to remember a system call's registers.

A context that was never given a scheduler has no clock and no run queue, so a call that
would block — and every thread call — is refused with `POUND_ERROR_UNSUPPORTED_SYSCALL` and
a log line: a gap, exactly like any other, and refusing is what keeps a title from spinning
on a timeout that never happened. `svcCreateThread`, `svcStartThread`, `svcExitThread`,
`svcGetThreadPriority`, `svcSetThreadPriority` and `svcGetThreadId` are then implemented for
exactly the cases Horizon defines: `svcStartThread` on a thread that is not in the created
state is `0xFA01`, produced *here* rather than by the kernel, which answers a second start
with an error code and leaves this layer to know the `Result`. An out-of-range priority is
a gap rather than a `Result`, because Pound has not established which word Horizon uses and
inventing one would look like an answer.

A wait over several handles treats a session as a legitimate object that is simply never
the ready one, rather than as an error: a title waiting on a session and an event at once
is a normal shape for a main loop, and nothing queues a message in this slice. The
memory-layout calls that need an alias path (`svcMapMemory` and `svcUnmapMemory`), IPC, and
the process queries are out of scope and refused by name.

Four calls answer from the region table and so need a layout the same way a blocking call
needs a scheduler. `svcSetHeapSize` resizes the heap region at the top of the address space
and, on success, answers the heap base in `X1`; its refusals are the ones Horizon
documents — misaligned or larger than 4 GiB is `0xCA01`, a valid size larger than the heap
region is `0xD001`, and a size the underlying allocator cannot satisfy is `0xCE01`.
`svcQueryMemory` walks the same region table and, for an address inside the address space,
reports the region containing it: base, size, state, attribute, permission and the two IPC
counts. For an address *outside* the address space the kernel still succeeds, returning a
fabricated `Inaccessible` block whose base is the end of the address space and whose size
reaches the top of memory — because a title uses exactly that answer to find where its
address space ends. A context with no layout cannot answer either call, so both are refused
rather than answered from an address space that does not exist.

Two more answer from the table by changing it. `svcSetMemoryPermission` and
`svcSetMemoryAttribute` validate their arguments in the syscall layer, where Horizon does —
a misaligned address is `0xCC01`, a size that is zero or not a whole number of pages is
`0xCA01`, an unacceptable permission is `0xD801`, an attribute mask and value that do not
nest or reach outside the one changeable bit are `0xE801` — and hand an aligned range to the
layout, whose two refusals ("not in the address space" and "a region in it may not be
changed") both collapse to the kernel's single `0xD401`, and whose table-full exhaustion is
`0xCE01`. Because both calls write the same table `svcQueryMemory` reads, a change made by
one is visible in the other.

`svcCancelSynchronization` is the first call here whose *answer* lands on another thread.
Horizon documents two cases and one result: a target already inside
`svcWaitSynchronization` has that call interrupted, and a target that is not has its next
synchronization call interrupted; either way the cancelled wait reports `0xEC01`, writes no
index, and the cancelling call itself succeeds. The handler asks the scheduler to end a
parked wait first and, only when there was none, raises the flag on the thread object, so the
two cases are told apart by the layer that knows which one is true rather than guessed. A
wait consumes a pending request before it looks at its handles, which is why a cancellation
wins over a handle that was already ready and an auto-reset event is not consumed by a call
that never polled. Without a scheduler the call is refused even though setting the flag
needs no clock: the flag could never be consumed by a wait in that context, so answering
success would promise an interruption the emulator cannot deliver.

The module adds `POUND_ERROR_UNSUPPORTED_SYSCALL` for a well-formed `svc` whose number or
feature the OS layer has not implemented — distinct from
`POUND_ERROR_UNSUPPORTED_INSTRUCTION`, which is a fact about the decoder. The dispatch
context carries the kernel, the process, the guest memory, the caller's clock and core
number, and — through `hos_syscall_context_set_scheduler` and
`hos_syscall_context_set_layout` — the scheduler that parks a blocked thread and the address
space that answers the memory calls, rather than reading a global, so a test can drive any
of them. `tests/test_hos_syscall.c` covers it with 44 cases.

---

## Debug GUI and hot reload

`PoundGui` is a separate shared library on desktop so it can be rebuilt and
reloaded without restarting the emulator. It is reached exclusively through the
`gui_plugin_exports_t` vtable:

```c
typedef struct
{
    gui_plugin_error_t (*create)(...);
    void              (*destroy)(...);
    void              (*render_frame)(...);
    gui_plugin_error_t (*save)(...);
} gui_plugin_exports_t;
```

`src/main.c` never links against ImGui. It loads the plugin by path, resolves
the vtable via `getExports`, copies the image to a temp file, and reloads it
when the source's mtime changes.

`gui_plugin_exports_get` returns `GUI_PLUGIN_SUCCESS` on success — note that
returning `true` (which is `1`, and therefore
`GUI_PLUGIN_ERROR_INVALID_ARGUMENT` in that enum) was a real bug.

### Three ABI details the plugin boundary depends on

These are not stylistic; each one produced an image that could be built but not
actually loaded.

**`PoundGui` links `PoundCore`.** The plugin uses `thread_logger`,
`memory_subsystem_*` and `safe_math_*`, which live in the core OBJECT library.
Without an explicit `PoundCore` on its link line those symbols were unresolved
in the DLL. Windows happened to paper over it — `WINDOWS_EXPORT_ALL_SYMBOLS` on
`Pound.exe` published the core out of the host executable, and the DLL picked
them up at load time from the already-mapped host image — but that is a PE
loader accident with no ELF equivalent, so the same code would have failed to
link for Android or any other platform. Linking `PoundCore` into `PoundGui`
embeds its own copy of the core objects in the DLL.

On Android `PoundGui` is `STATIC`, and a static archive consumes no OBJECT
library objects, so `libmain.so` still ends up with exactly one core.

**`gui_plugin_error_to_string` is `POUND_EXPORT`.** It is the only
host-facing function that is not behind the vtable, and `src/main.c` calls it
to turn a `gui_plugin_error_t` into log text. Left unannotated it was absent
from `libPoundGui.dll`'s export table — the DLL exported 35 symbols and every
one of them resolved, and this one did not.

**`IMGUI_IMPL_API` is `POUND_IMGUI_IMPL_API`.** `cimgui.h` defines `CIMGUI_API`
as `__declspec(dllexport)` and `imgui.h` leaves `IMGUI_API` empty, falling back
to it only when `IMGUI_IMPL_API` is undefined. That exports the ImGui core and
none of the backends: `libimgui.dll` exported 1581 core symbols and no
`ImGui_ImplSDL3_*` or `ImGui_ImplOpenGL3_*` at all, which is what the GUI needs
in order to pump input and submit draw data. `POUND_IMGUI_IMPL_API` expands to
`extern "C" __declspec(dllexport)` on Windows and `extern "C"` elsewhere.

### The plugin installs its own log sink

`gui_plugin_exports_get` is the plugin's entry point, and it is also where a
plugin gets its logger. `thread_logger` is a strong per-thread global that
defaults to `{ 0 }`, and `pound_log_message` drops every record unless
`logger->log` is set *and* `log_level <= logger->min_level`. A plugin loaded
via `dlopen` has its own copy of that global, initialised to zero, so every
`POUND_LOG_*` from inside the GUI was discarded with no diagnostic.

`gui_plugin_exports_get` therefore calls `pound_logger_init_default` on its own
`thread_logger` whenever the caller passes `log == NULL`. Passing a logger
still takes precedence, so a host that wants full control keeps it.

### Android

`POUND_PLATFORM_SUPPORTS_HOT_RELOAD` is `0` on Android:
`SDL_GetBasePath()` resolves inside the read-only APK, so there is nowhere
writable to copy a reloaded image to and no way to `dlopen` an arbitrary path.

`PoundGui` is therefore built `STATIC` and `src/main.c` binds it through
`app_gui_bind_static()`, which resolves the same vtable directly. Because
`gui_plugin_destroy` already handles a `NULL` module, every downstream call site
is unchanged and the hot-reload path is bypassed rather than forked.

---

## Memory accounting

`src/core/memory/memory.{h,c}` wraps mimalloc and attributes every byte to a
bucket:

| Bucket | Owner |
| --- | --- |
| `MEMORY_BUCKET_NONE` | Unattributed |
| `MEMORY_BUCKET_UI` | Debug GUI |
| `MEMORY_BUCKET_GUEST_MEMORY` | Emulated guest allocations |
| `MEMORY_BUCKET_JIT_RECOMPILER` | Ballistic code cache |
| `MEMORY_BUCKET_DEBUG_PROFILING` | Instrumentation |

The GUI memory tracker and the JIT code-cache statistics are driven entirely by
these counters, so the bucket index must be handled correctly everywhere.

### Two properties worth stating explicitly

**Bucket indices are clamped, never masked.** The original code indexed the
accounting array with `bucket & MEMORY_BUCKET_COUNT`. Because `COUNT` is a power
of two, that collapsed `GUEST_MEMORY` (2) onto `NONE` (0) and
`JIT_RECOMPILER` (3) onto `UI` (1), and let `MEMORY_BUCKET_COUNT` itself index
one past the end of the array. Every per-bucket total except three was wrong.
It is now a clamp, and an out-of-range query returns zero with a warning rather
than a plausible wrong number.

The clamp is one-sided, and that is forced by the type rather than chosen.
`memory_bucket_type_t` declares no negative enumerator, so its underlying type is
unsigned; a caller that casts a negative `int` produces a value far *above* the
range, not below it, and there is no sign left for the implementation to
inspect. Every out-of-range value therefore clamps to `MEMORY_BUCKET_COUNT - 1`,
which keeps the index inside `memory_used_by_bucket[]` regardless. The earlier
`bucket < MEMORY_BUCKET_NONE ? MEMORY_BUCKET_NONE : ...` branch was unreachable
dead code that suggested a guarantee the type could not deliver.

**`MEMORY_BUCKET_TOTAL` is a sentinel, not an index.** It is `-1` and is
resolved before any array access; any other negative value is treated as the
total too.

### Documented limitation: the thread-local bucket model

The active bucket is **thread-local**, and a block is charged to whichever
bucket is active at `allocate` time but credited to whichever is active at
`free` time. For a block allocated and freed on the same thread — the normal
case — this is exact. Across threads, or across a bucket switch, the breakdown
skews.

The debit therefore **saturates at zero**: a cross-bucket free can zero a
bucket but must never wrap it to a value near `SIZE_MAX`, which the GUI would
render as an absurd figure. The skew is surfaced with a warning rather than
silently absorbed.

Carrying per-block provenance to make this exact is a real design change (an
allocation header, or a hash map from pointer to bucket) and has a measurable
cost in the hot path. It has not been done.

---

## Ballistic

[Ballistic](https://github.com/pound-emu/ballistic) is a purpose-built ARM64
recompiler, consumed as a **prebuilt** static library plus a LuaJIT shared
library. `cmake/Ballistic.cmake` resolves artefacts per platform.

Only x86-64 artefacts exist. When the set for the current target is incomplete,
the JIT is disabled, a warning is emitted, and an inert `Ballistic::Engine`
INTERFACE IMPORTED target is created. That placeholder is why no `#ifdef` is
needed at the call sites: `if (TARGET Ballistic::Engine)` stays correct whether
or not the JIT is present, and ARM64 builds work today while picking the JIT up
automatically once the artefacts are committed.

`Ballistic::LuaJIT` sets `IMPORTED_NO_SONAME` on non-Windows, because the
prebuilt `libluajit.so` has no SONAME — without that, CMake synthesises one and
the loader then looks for a file that does not exist next to the executable.

---

## JIT execution loop — `src/core/jit/jit_execution.{h,c}`

The execution loop is the user of every other JIT module: `jit_metadata` says
whether host code exists for a guest address, `jit_cache` owns the executable
bytes, and this module actually **runs** the guest — it reads the guest PC,
finds or translates the block that PC starts, hands the guest register file to
the block, and reports an exit reason to the runtime.

The block ABI is one host function pointer:

```c
void (*)(guest_state_t *state)
```

A single object passed by address, which is exactly the shape of Ballistic's
`bal_jit_block_t`; when the ARM64 translator is bound below this loop, the
blocks it publishes are just the addresses of compiled host code.

`jit_execution_step` runs exactly one block: consume any stop request, check the
configured `halt_pc`, then look the PC up in the metadata table
(`jit_metadata_find_ready`). On a hit it acquires a lease, runs the block, and
drops the lease. On a miss it interns the address, claims the slot for
translation (`jit_metadata_intern` + `jit_metadata_try_begin`), calls the
configured `translate` callback into a freshly allocated cache block, makes the
block executable, publishes it, and runs it. A miss that keeps vanishing
mid-dispatch reloads up to `JIT_EXECUTION_MAX_RELOADS` (4) times before the
loop refuses.

Normal exits travel in `jit_execution_exit_t` — `NONE`, `SYSCALL`, `HALTED`,
`STOPPED`, `QUOTA`, `CONTENTION` — and hard failures (`FAILED`, `ERROR`) travel
as the return value. `jit_execution_run` is `step` in a loop until an exit
reason or the configured block quota (`max_blocks_per_run`), and returns
`POUND_SUCCESS` with the reason in `*out_exit`.

The claim is the only source of blocking. When two dispatcher threads reach the
same cold address, the loser resolves the contention per
`jit_execution_contention_policy_t`:

- **Spin** — re-dispatch for `claim_spin_budget` (default 4096) iterations; the
  other thread's publication makes the block `READY` and the spin wins, else the
  loop reports `POUND_ERROR_BUSY` with exit `CONTENTION`.
- **Interpret** — run one block interpretively through the `interpret` hook;
  with no hook installed this degrades to Defer with a warning.
- **Defer** — return immediately with exit `CONTENTION` and let the runtime
  decide when to come back.

Invalidation is two steps and this module owns the second one:
`jit_metadata_invalidate_range` moves cleared blocks to `VACANT` but does not
free their host code (the documented contract that makes a raced invalidation
safe). The loop keeps a registry of every block it published — `{guest_pc,
guest_size, code}` — grown by doubling from the initial 128 entries, and
`jit_execution_invalidate_range` frees and forgets every registered block the
range would have cleared **only after** the metadata side reports success.
`jit_execution_reset` is the full tear-down order: invalidate and free every
owned block, then `jit_metadata_reset`, then reset the code cache and clear the
dispatcher's counters.

---

## Android

Pound is not an executable on Android. `SDLActivity` `dlopen()`s `libmain.so`
and calls the exported `SDL_main` through JNI; `src/main.c` produces that symbol
by including `<SDL3/SDL_main.h>`, which is **not** pulled in by `SDL.h`.

Everything links statically into one `libmain.so`: SDL3, ImGui, `PoundGui`,
mimalloc, libc++. Two copies of SDL state in one process crash
`ImGui_ImplSDL3_Init` — the same failure the desktop build avoids by using
shared SDL.

`src/main.c` also does frame pacing on Android, where there is no vsync to
block on in the same way.

`cmake/StageJniArtifacts.cmake` stages the built image after linking;
`android/app/build.gradle.kts` orders the JNI merge behind the native build and
fails loudly if no `libmain.so` was produced, rather than shipping an APK that
cannot start.

---

## Build modules

| Module | Responsibility |
| --- | --- |
| `cmake/CmakePolicies.cmake` | `cmake_minimum_required` / policy pinning |
| `cmake/CompilerFlags.cmake` | `add_compiler_flags`, `add_linker_flags`, `add_sanitizers` |
| `cmake/Platform.cmake` | Platform and architecture detection, shared-library naming, JNI staging, sanitizer downgrade |
| `cmake/Ballistic.cmake` | Per-platform JIT artefact resolution, verification, install rules |
| `cmake/StageJniArtifacts.cmake` | Post-build JNI staging (script mode) |
| `cmake/toolchains/android-arm64.cmake` | NDK r27, arm64-v8a, API 26, 16 KiB pages |
| `cmake/toolchains/linux-aarch64.cmake` | `aarch64-linux-gnu` cross build, QEMU test emulator |

There is exactly one place that knows how a platform is detected
(`Platform.cmake`), one place that knows the Ballistic layout
(`Ballistic.cmake`), and one place that knows the on-device native layout
(`StageJniArtifacts.cmake`). That is deliberate: the alternative is the same
knowledge drifting across four files.
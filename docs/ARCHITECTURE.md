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

Pound is a C11 emulator shell with a C++ debug GUI. The guest CPU is not yet
implemented; what exists today is the substrate it will run on.

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
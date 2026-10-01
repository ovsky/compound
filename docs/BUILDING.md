# Building Pound

Everything below assumes Clang, CMake ≥ 3.25 and Ninja. Pound is a Clang-only
project; GCC and MSVC are not supported, and compiler-specific flags in
`CMakePresets.json` (`-ftrivial-auto-var-init`, `-flto`, `/LTCG`) assume it.

All presets are defined in `CMakePresets.json`. Run
`cmake --list-presets`, `--list-presets=build` and `--list-presets=test` to see
them.

---

## Contents

- [Prerequisites](#prerequisites)
- [Linux](#linux)
- [Windows](#windows)
- [macOS](#macos)
- [ARM64 Linux (cross)](#arm64-linux-cross)
- [Android](#android)
- [Ballistic](#ballistic)
- [CMake options](#cmake-options)
- [Install rules](#install-rules)
- [Tests](#tests)
- [Troubleshooting](#troubleshooting)

---

## Prerequisites

### All platforms

| Tool | Version | Notes |
| --- | --- | --- |
| CMake | ≥ 3.25 | `cmake_minimum_required(VERSION 3.25..3.31)` |
| Ninja | any | Every preset selects the Ninja generator |
| Clang | ≥ 16 | `clang` and `clang++`; `clang-cl` on Windows |
| ccache / sccache | optional | Used by every preset; remove the launcher to build without it |

`extern/` is vendored in-tree. There is nothing to `git submodule update`.

### Linux

```sh
sudo apt-get install --no-install-recommends --yes \
    cmake ninja-build clang ccache mold
```

`mold` is only needed by the `debug` preset (`-fuse-ld=mold`); the `release`
preset uses `lld`, which ships with Clang.

### Windows

Install Visual Studio (for the MSVC runtime and Windows SDK), plus LLVM. The
presets compile with `clang-cl`, which links against the MSVC runtime, so the
MSVC environment must be active:

```powershell
# In the same shell, after running vcvars64.bat:
cmake --preset debug-windows
cmake --build --preset debug-windows --parallel
ctest  --preset debug-windows
```

`sccache` is the launcher on Windows; it ships with the LLVM installer.

### macOS

```sh
brew install cmake ninja ccache mold
cmake --preset release
cmake --build --preset release --parallel
```

Only the arm64 runner is exercised in CI.

---

## Linux

```sh
cmake --preset debug
cmake --build --preset debug --parallel
ctest  --preset debug
```

Artefacts land in `build/debug/bin/linux/`.

The `debug` preset enables AddressSanitizer and UndefinedBehaviorSanitizer, so
a leak or an out-of-bounds access in the emulator core fails the test run rather
than being discovered later on a user's machine.

---

## Windows

```sh
cmake --preset debug-windows
cmake --build --preset debug-windows --parallel
ctest  --preset debug-windows
```

Artefacts land in `build/debug/bin/windows/`. `libPoundGui.dll` and
`libimgui.dll` must sit next to `Pound.exe`: the GUI plugin is loaded by path at
runtime, not through the import table.

Sanitizers are not enabled on Windows — they would require shipping the ASan
runtime DLL alongside the release, which is not worth the fragility.

---

## macOS

```sh
cmake --preset release
cmake --build --preset release --parallel
ctest  --preset release
```

Artefacts land in `build/release/bin/mac/`.

---

## ARM64 Linux (cross)

ARM64 is a first-class CI target: `linux-aarch64-debug` cross-compiles with the
`aarch64-linux-gnu` toolchain and runs the test suite under QEMU user-mode
emulation, so the ARM64 code paths are actually executed rather than merely
compiled.

```sh
sudo apt-get install --no-install-recommends --yes \
    gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
    qemu-user binutils-aarch64-linux-gnu ccache

cmake --preset linux-aarch64-debug
cmake --build --preset linux-aarch64-debug --parallel

# Sanity check that this really produced an ARM binary.
file build/linux-aarch64-debug/bin/linux/Pound

ctest --preset linux-aarch64-debug
```

If `qemu-aarch64` is not installed, the toolchain file emits a warning and the
build proceeds; `ctest` will then be unable to execute the test binary.

A different sysroot or prefix can be supplied without editing the preset:

```sh
cmake --preset linux-aarch64-debug \
    -DPOUND_LINUX_CROSS_PREFIX=aarch64-unknown-linux-gnu \
    -DPOUND_LINUX_SYSROOT=/opt/sysroot
```

---

## Android

### What gets built

On Android Pound is **not** an executable. SDL's Android entry point is invoked
from Java: `SDLActivity` `dlopen()`s `libmain.so` and calls the exported
`SDL_main`, which `src/main.c` produces by including `<SDL3/SDL_main.h>`.

Everything is therefore linked statically into a single `libmain.so`:

| Component | Android type | Why |
| --- | --- | --- |
| SDL3 | `SDL3-static` | Two copies of SDL state crash `ImGui_ImplSDL3_Init` |
| `imgui` | `STATIC` | Keeps `libmain.so` self-contained |
| `PoundGui` | `STATIC` | `SDL_GetBasePath()` resolves inside the read-only APK, so there is nowhere writable to copy a reloaded image to |
| mimalloc | `mimalloc-static` | A shared mimalloc could not be guaranteed to load before its own `malloc` override takes effect |
| libc++ | `c++_static` | Avoids shipping a second `.so` |

`POUND_LINK_GUI_STATICALLY` is `ON` by default on Android. In that mode
`src/main.c` calls `gui_plugin_exports_get()` directly instead of going through
the hot-reload loader, so the plugin vtable is identical and every downstream
call site is unchanged.

### Prerequisites

```sh
# Install the NDK (27.2.12479018 is what the build pins) and set:
export ANDROID_NDK_HOME=/path/to/android-ndk/27.2.12479018
```

The toolchain file also accepts `ANDROID_NDK`, `ANDROID_NDK_ROOT` or the newest
directory under `$ANDROID_SDK_ROOT/ndk/`.

### Build the native image only

```sh
cmake --preset android-arm64-release
cmake --build --preset android-arm64-release --parallel
```

This produces `build/android-arm64-release/jniLibs/arm64-v8a/libmain.so`.

### Build an installable APK

```sh
cd android
gradle assembleRelease
# APK: android/app/build/outputs/apk/release/
```

`android/app/jni/CMakeLists.txt` is a thin wrapper that hands control to the
repository's own top-level `CMakeLists.txt`, so the APK's native image is built
by exactly the same target graph as the desktop builds rather than by a second,
drifting copy of it.

Requirements: JDK 17, the Android SDK, and Gradle 8.12. There is no committed
Gradle wrapper (it would require committing a binary `.jar`); generate one with
`gradle wrapper` if you want `./gradlew`.

Debug builds use the suffix `dev.pound.emulator.debug` and are signed with the
standard debug keystore.

### Signing a release

Signing is only attempted when these environment variables are present:

| Variable | Meaning |
| --- | --- |
| `POUND_KEYSTORE_PATH` | Path to the `.jks` file |
| `POUND_KEYSTORE_PASSWORD` | Store password |
| `POUND_KEY_ALIAS` | Key alias |
| `POUND_KEY_PASSWORD` | Key password |

Without them the release APK is unsigned. See `.github/workflows/android-apk.yml`
for a CI-only keystore generation example.

---

## Ballistic

The JIT is consumed as a **prebuilt** static library plus a LuaJIT shared
library, committed per platform:

```
extern/ballistic/
  include/                        Headers (all platforms)
  windows/lib/Ballistic.lib
  windows/lib/lua51.lib           Import library
  windows/bin/lua51.dll
  linux/lib/libBallistic.a
  linux/bin/libluajit.so
  android/lib/<abi>/libBallistic.a     (not yet supplied)
  android/lib/<abi>/libluajit.so      (not yet supplied)
  mac/lib/libBallistic.a               (not yet supplied)
  mac/lib/libluajit.dylib              (not yet supplied)
```

### Automatic resolution

`cmake/Ballistic.cmake` looks for the artefacts matching the current target. If
**any** artefact in the set is missing, it treats the whole set as absent,
disables `POUND_ENABLE_BALLISTIC`, warns, and creates an inert
`Ballistic::Engine` INTERFACE IMPORTED target.

That placeholder is deliberate: `if (TARGET Ballistic::Engine)` call sites keep
working with no preprocessor guards, so the JIT's availability never requires
`#ifdef`s scattered through the source.

This is why the ARM64 CI lanes are green today despite the missing ARM64
artefacts — and why they will pick the JIT up automatically once
`extern/ballistic/android/arm64-v8a/` and `extern/ballistic/linux-arm64/` are
populated, with no build-file edits.

### Disabling explicitly

```sh
cmake --preset debug -DPOUND_ENABLE_BALLISTIC=OFF
```

### Why the artefacts are prebuilt

The emulator links Ballistic as an opaque static library. Building it from
source requires an ARM64 cross-assembler and its own CMake project, which is
outside the scope of this repository. Committed binaries keep the ARM64 lanes
reproducible without every contributor installing an additional toolchain.

---

## CMake options

| Option | Default | Effect |
| --- | --- | --- |
| `POUND_BUILD_TESTS` | `ON` | Build the unit test suite and register it with CTest |
| `POUND_ENABLE_BALLISTIC` | `ON` | Link the JIT when artefacts exist; auto-disabled otherwise |
| `POUND_ENABLE_INSTALL_RULES` | `ON` | Emit `install()` rules |
| `POUND_LINK_GUI_STATICALLY` | Android: `ON` | Link `PoundGui` in rather than `dlopen`ing it |
| `POUND_ENABLE_LINK_TIME_OPTIMIZATION` | `ON` | LTO / `-flto=full`, downgraded with a warning if unsupported |
| `POUND_SANITIZERS` | preset | Sanitizer flags; silently dropped when cross-compiling or on Android |
| `POUND_COMPILER_FLAGS` | preset | Extra flags applied by `add_compiler_flags` |
| `POUND_LINKER_FLAGS` | preset | Extra link flags applied by `add_linker_flags` |

---

## Install rules

```sh
cmake --preset release -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build --preset release --parallel
cmake --install build/release
```

Layout:

| Platform | Binaries | Libraries |
| --- | --- | --- |
| Windows | `bin/` | `lib/` |
| Linux | `bin/` | `lib/` |
| macOS | `bin/` | `lib/` |
| Android | — | `lib/<abi>/` |

On Android the layout is flat because that is what an APK packages.

---

## Tests

```sh
ctest --preset debug                     # everything
ctest --preset debug -R guest_memory     # one suite
ctest --preset debug -R PoundTests.      # all suites
```

Five CTest entries are registered: `PoundTests` runs the whole binary, and
`PoundTests.<suite>` re-runs it with a filter per suite
(`safe_math`, `guest_memory`, `guest_state`, `memory`). Failures therefore name
the area they came from.

The harness is dependency-free (`tests/pound_test.h`) on purpose: nothing is
vendored, and a cross-compiled ASan or a framework dependency would itself have
to be cross-compiled.

```sh
./build/debug/bin/linux/PoundTests            # run directly
./build/debug/bin/linux/PoundTests memory     # filter
```

The harness also installs a capturing logger, so tests can assert that a failed
call reported failure through a log record and not only through its return
value — the project's rule is that errors are never swallowed silently.

---

## Troubleshooting

**`[Ballistic] No prebuilt engine for ...`**
Expected on ARM64. See [Ballistic](#ballistic).

**`configure` fails with "unsupported target processor"**
`cmake/Platform.cmake` accepts only `aarch64`/`arm64`/`arm64-v8a` and
`x86_64`/`amd64`/`x64`. 32-bit targets are rejected by design.

**`ctest` reports "Unable to find executable" under the aarch64 preset**
`qemu-aarch64` is missing. Install `qemu-user`; the toolchain file needs it to
set `CMAKE_CROSSCOMPILING_EMULATOR`.

**Sanitizers vanish from a cross or Android build**
Intentional. `pound_resolve_sanitizers()` drops them because a cross-compiled or
Android ASan runtime cannot be loaded, and a link failure at configure time
would be more confusing than the downgrade.

**`libPoundGui.dll` is not found at runtime (Windows)**
It must sit next to `Pound.exe`. The loader resolves it by absolute path next to
the executable, not through the import table.

**Gradle: "No libmain.so was produced"**
The native build did not run. Run the CMake preset first
(`cmake --preset android-arm64-release`) and check
`android/app/.cxx/arm64-v8a/<variant>/jniLibs/arm64-v8a/`.

**Stale native libraries end up in the APK**
`gradle clean`. The Gradle task rejects a second `libmain.so` under `.cxx` and
removes previously staged `.so` files before copying, so this should be
self-correcting.
# Continuous integration and delivery

Every change is verified on x86-64 Linux, ARM64 Linux and Windows, plus an
Android APK lane. Tagging a `v*` release produces distribution packages for
Linux, macOS and Windows, and attaches an Android APK.

- [Workflows](#workflows)
- [Lanes](#lanes)
- [Reproducing a lane locally](#reproducing-a-lane-locally)
- [Placeholder audit](#placeholder-audit)
- [Release](#release)
- [Caching](#caching)
- [Adding a lane](#adding-a-lane)
- [Troubleshooting](#troubleshooting)

---

## Workflows

| File | Triggers | Produces |
| --- | --- | --- |
| `.github/workflows/ci.yml` | push and PR to `main`/`master`, manual | Build + test evidence. No releases. |
| `.github/workflows/android-apk.yml` | push and PR, `v*` tags, manual | Debug and release APKs as artifacts; attaches to a tagged release. |
| `.github/workflows/release.yml` | `v*` tags, manual | Linux `.tar.gz`, Windows `.zip`, macOS `.tar.gz`, GitHub release. |

A superseded run for the same ref is cancelled via a `concurrency` group, so a
push that lands mid-build does not leave a stale red check behind.

---

## Lanes

### `ci.yml`

| Lane | Runner | Preset | What it proves |
| --- | --- | --- | --- |
| `linux` (debug) | `ubuntu-24.04` | `debug` | Native x86-64 build with ASan + UBSan; tests pass with no leak or UB. |
| `linux` (release) | `ubuntu-24.04` | `release` | Optimised build with LTO; tests pass. |
| `linux-aarch64` | `ubuntu-24.04` | `linux-aarch64-debug` | ARM64 code path **compiles and runs**, under QEMU user-mode emulation. |
| `windows` (debug/release) | `windows-2025` | `debug-windows`, `release-windows` | clang-cl build on the MSVC runtime; tests pass. |
| `android-arm64` | `ubuntu-24.04` | `android-arm64-release` | NDK build produces an AArch64 `libmain.so` exporting `SDL_main`. |
| `placeholder-audit` | `ubuntu-24.04` | — | No `TODO`/`FIXME`/stub markers in tracked sources; presets and workflow YAML parse. |

The two lane details that matter most:

**ARM64 tests actually execute.** `cmake/toolchains/linux-aarch64.cmake` sets
`CMAKE_CROSSCOMPILING_EMULATOR` to `qemu-aarch64`, so CTest runs the
cross-compiled binary under emulation rather than skipping it. The lane then
asserts with `file(1)` that the produced binary really is `ARM aarch64` — a
silently-native build would otherwise make the test step pass without ever
touching the ARM64 code path.

**Android verifies the JNI contract.** After staging, the lane checks that
`libmain.so` is an AArch64 shared object (`readelf -h`) and that it exports
`SDL_main` (`readelf --dyn-syms`). Those are exactly the two things that must be
true for `SDLActivity` to `dlopen` it and find the entry point, and neither is
caught by a successful link.

### `android-apk.yml`

Runs `assembleDebug` and `assembleRelease` in a matrix, then unpacks each APK
and verifies that `lib/arm64-v8a/libmain.so` is present, is AArch64, and exports
`SDL_main`.

The APK output filename carries a signing suffix (`release-unsigned`, `debug`)
that AGP changes between versions, so the APK is *located* rather than
assumed.

On a `v*` tag the release APK is attached to the GitHub release.

### Not yet covered

- **macOS builds.** `release.yml` builds macOS arm64 for releases, but `ci.yml`
  has no macOS lane, so a change can break macOS and only be discovered at tag
  time. Adding a `macos` job to `ci.yml` is the obvious next step.
- **Sanitizers on Windows or Android.** Deliberately unavailable; see
  [BUILDING.md](BUILDING.md#cmake-options).
- **ARM64 with the JIT.** Blocked on
  `extern/ballistic/android/` and `extern/ballistic/linux-arm64/` artefacts, not
  on build infrastructure.

---

## Reproducing a lane locally

```sh
# Linux debug, with sanitizers
cmake --preset debug
cmake --build --preset debug --parallel
ctest  --preset debug

# ARM64 cross-build, tests under QEMU
sudo apt-get install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu qemu-user
cmake --preset linux-aarch64-debug
cmake --build --preset linux-aarch64-debug --parallel
file build/linux-aarch64-debug/bin/linux/Pound     # expect: ARM aarch64
ctest --preset linux-aarch64-debug

# Windows (from a shell with the MSVC environment active)
cmake --preset debug-windows
cmake --build --preset debug-windows --parallel
ctest  --preset debug-windows

# Android native image
export ANDROID_NDK_HOME=/path/to/android-ndk/27.2.12479018
cmake --preset android-arm64-release
cmake --build --preset android-arm64-release --parallel
readelf --dyn-syms -W build/android-arm64-release/jniLibs/arm64-v8a/libmain.so | grep SDL_main

# APK
cd android && gradle assembleRelease
```

Package dependencies per lane are listed in
[BUILDING.md](BUILDING.md#prerequisites).

---

## Placeholder audit

`ci.yml` fails if a tracked source contains `TODO`, `FIXME`, `XXX`, `HACK`,
`// ...` or `/* ... */`:

```sh
git ls-files 'src/*.c' 'src/*.h' 'tests/*.c' 'tests/*.h' 'cmake/*.cmake' 'cmake/*/*.cmake' \
  | xargs grep -nE '\b(TODO|FIXME|XXX|HACK)\b|// *\.\.\.|/\* *\.\.\. *\*/'
```

The glob list is restricted to `src/`, `tests/` and `cmake/` on purpose: the
vendored trees under `extern/` legitimately contain their own `TODO`s, and
auditing them would produce noise nobody would act on.

The same job also runs `cmake --list-presets` for all three preset kinds and
parses every workflow YAML file, so a malformed `CMakePresets.json` or workflow
is caught before it reaches a build.

---

## Release

Tag a `vMAJOR.MINOR.PATCH`:

```sh
git tag -a v0.1.0 -m "v0.1.0"
git push origin v0.1.0
```

`release.yml` then:

1. Resolves and validates the version.
2. Builds Linux x86-64, Windows x86-64 and macOS arm64 in parallel.
3. Packages each platform's runtime image only — headers, static archives and
   CMake export files are build inputs, not part of a runnable tree.
4. Verifies each package before uploading it.
5. Creates the GitHub release with generated notes.

`android-apk.yml` runs concurrently and attaches the signed APK to the same
release.

### Verification before publication

A package that extracts cleanly and then fails on first launch is worse than no
package, so each one is checked:

| Platform | Check |
| --- | --- |
| Linux | `ldd` reports no `not found`; `Pound` exists in the staged tree. |
| Windows | `Pound.exe` and `libPoundGui.dll` both present; every other `*.dll` copied alongside. |
| macOS | `codesign --verify --deep --strict` after ad-hoc signing; `otool -L` printed. |

macOS ad-hoc signing is not cosmetic: unsigned arm64 binaries are killed by the
kernel on first `exec`, so the artefact would download and refuse to start.

### Signing

No signing material is stored in the repository. The Android workflow
*generates* a CI-only keystore with `keytool` at run time from secrets. A
store-distribution build needs a key supplied from the release environment
instead.

---

## Caching

- `ccache` (Linux) and `sccache` (Windows) are cached per lane, keyed on the
  commit with a lane-prefixed `restore-keys` fallback so a cache hit survives
  unrelated commits.
- `gradle/actions/setup-gradle@v4` caches Gradle dependencies and configuration
  cache entries.
- The Android NDK is installed at a pinned revision, not a floating `latest`.

The Android ABI image is deliberately **not** cached: it is the artefact under
test, and a cache hit would mask a broken native build.

---

## Adding a lane

1. Add a toolchain file under `cmake/toolchains/` if the target is cross-compiled.
2. Add `configure`, `build` and (if runnable) `test` presets to
   `CMakePresets.json`.
3. Extend `cmake/Platform.cmake`'s detection — one place only.
4. Add the job to `ci.yml`.
5. Document it in [BUILDING.md](BUILDING.md) and here.

A lane must assert *something specific*. "It built" is not evidence; `file(1)`
saying the binary is AArch64, or `readelf` saying `SDL_main` is exported, is.

---

## Troubleshooting

**The ARM64 lane builds but `ctest` cannot execute anything**
`qemu-aarch64` is missing. The lane installs `qemu-user`; locally,
`sudo apt-get install qemu-user`.

**The Android lane fails at the `readelf` step**
The native build linked but the entry point is wrong. Confirm that `src/main.c`
still includes `<SDL3/SDL_main.h>` under `#if POUND_PLATFORM_ANDROID` and that the
target's `OUTPUT_NAME` is still `main`.

**A Windows lane fails to find `clang-cl`**
The runner image changed. The `Locate LLVM toolchain` step fails loudly rather
than silently falling back, on purpose — a lane that quietly stopped testing
clang-cl would be worse than a red check.

**`placeholder-audit` fails on a vendored file**
It should not: the audit is restricted to `src/`, `tests/` and `cmake/`. If it
triggers on `extern/`, the glob list in `ci.yml` has been widened by mistake.
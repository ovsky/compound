<h1 align="center">
  <a href="https://github.com/pound-emu/pound/actions/workflows/ci.yml?query=branch%3Amain">
    <img src="https://github.com/pound-emu/pound/actions/workflows/ci.yml/badge.svg?branch=main" alt="CI: Linux, ARM64 and Windows">
  </a>
  <a href="https://github.com/pound-emu/pound/actions/workflows/android-apk.yml?query=branch%3Amain">
    <img src="https://github.com/pound-emu/pound/actions/workflows/android-apk.yml/badge.svg?branch=main" alt="CI: Android APK">
  </a>
  <br><br>
  Pound
</h1>

<p align="center"><em>A Nintendo Switch 2 emulator, written in C.</em></p>

---

## Status

**Pound is early-stage software.** It builds, its unit tests pass, and the
emulator shell, memory accounting, logging, checked arithmetic and debug GUI all
work — but the guest CPU, Horizon OS, and the Ballistic JIT integration are not
finished. It does not yet boot a commercial game.

The honest summary of where things stand:

| Area | State |
| --- | --- |
| Build system (CMake, presets, cross toolchains) | Complete: x86-64 and ARM64, Windows, Linux, macOS, Android |
| CI/CD | Complete: 5 lanes, plus release packaging |
| Unit tests | 49 cases across 4 suites |
| Host memory accounting | Working |
| Checked arithmetic (`safe_math`) | Working |
| Logging | Working |
| Debug GUI + live reload (desktop) | Working |
| Debug GUI (Android) | Working, statically bound |
| **Ballistic JIT** | **No ARM64 prebuilt artefact — disabled on ARM** |
| **Guest CPU (Horizon OS)** | **Not implemented** |

### About the ARM64 JIT

Performance work has shifted to [Ballistic](https://github.com/pound-emu/ballistic),
a purpose-built ARM64 recompiler. It ships as a **prebuilt** static library per
platform. Only x86-64 artefacts exist today:

```
extern/ballistic/windows/{include,lib,bin}   libBallistic.lib, lua51.lib, lua51.dll
extern/ballistic/linux/{include,lib,bin}     libBallistic.a,  libluajit.so
```

Because there is no `extern/ballistic/android/` or `extern/ballistic/linux-arm64/`,
the ARM64 lanes build the emulator without the JIT rather than failing. Drop the
ARM64 artefacts into those two directories and the same build picks the JIT up
with no further changes — see [docs/BUILDING.md](docs/BUILDING.md#ballistic).

**Compiler developers:** contributions to the recompiler are the single
highest-value thing you can bring to this project.

## Roadmap

- [ ] Translate SM86 to SPIR-V to Vulkan
- [x] Add `mimalloc` for the host allocator
- [ ] Create a custom pool / slab allocator for Horizon OS
- [ ] Create a JIT code cache memory allocator for Ballistic
- [ ] Create a JIT metadata manager
- [ ] Integrate Ballistic into Pound
- [ ] Implement the guest CPU

## Building

Requires **CMake ≥ 3.25**, **Ninja**, and **Clang** (Clang only — GCC and MSVC
are not supported).

```sh
cmake --preset debug
cmake --build --preset debug --parallel
ctest  --preset debug
```

| Preset | Target | Notes |
| --- | --- | --- |
| `debug`, `release` | Linux / macOS x86-64 | ASan + UBSan in `debug` |
| `debug-windows`, `release-windows` | Windows x86-64 | clang-cl |
| `linux-aarch64-debug`, `linux-aarch64-release` | ARM64 Linux | Cross-compiled, tested under QEMU |
| `android-arm64-debug`, `android-arm64-release` | Android arm64-v8a | Produces `libmain.so` |

Full instructions, prerequisites, and the Android APK steps are in
**[docs/BUILDING.md](docs/BUILDING.md)**.

## Contributing

Read **[CONTRIBUTING.md](CONTRIBUTING.md)** before opening a pull request. The
project holds a deliberately high bar: every line is expected to be defensible,
and undocumented behaviour is treated as a defect.

Further reading:

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — how the pieces fit together
- [docs/CI.md](docs/CI.md) — what each CI lane proves, and how to reproduce it
- [docs/PROGRAMMING_RULES.md](docs/PROGRAMMING_RULES.md) — coding conventions
- [docs/SM86_TO_SPIRV_RULES.md](docs/SM86_TO_SPIRV_RULES.md) — recompiler rules

## Repository layout

```
src/core/         Emulator core: logging, memory, checked arithmetic, guest state
src/gui/          Debug GUI (ImGui) and its hot-reload plugin loader
src/main.c        Emulator bootstrap: SDL window, frame loop, GUI lifecycle
cmake/            Build modules, cross toolchains, JNI staging
tests/            Unit test suite and its dependency-free harness
android/          Gradle project that packages the ARM64 image into an APK
extern/           Vendored third-party sources (SDL, mimalloc, cimgui, Ballistic)
```

## Licence

See [LICENSE](LICENSE).
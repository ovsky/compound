<p align="center">
  <a href="https://github.com/ovsky/compound/actions/workflows/ci.yml?query=branch%3Amain">
    <img src="https://github.com/ovsky/compound/actions/workflows/ci.yml/badge.svg?branch=main" alt="CI: Linux, ARM64 and Windows" />
  </a>
  <a href="https://github.com/ovsky/compound/actions/workflows/android-apk.yml?query=branch%3Amain">
    <img src="https://github.com/ovsky/compound/actions/workflows/android-apk.yml/badge.svg?branch=main" alt="CI: Android APK" />
  </a>
  <a href="https://github.com/ovsky/compound/blob/main/LICENSE">
    <img src="https://img.shields.io/badge/License-GPLv3-blue.svg" alt="License: GPLv3" />
  </a>
  <a href="https://github.com/ovsky/compound">
    <img src="https://img.shields.io/badge/Language-C-5E6EF2.svg" alt="Language: C" />
  </a>
</p>

<h1 align="center">Compound</h1>

<p align="center">
  <strong>Open source emulator for Nintendo Switch 1 and 2</strong><br>
  <em>Highly experimental, but steadily growing.</em>
</p>

---

## Overview

Compound is a work-in-progress emulator project focused on bringing Nintendo Switch 1 and 2 emulation to a solid, maintainable, and portable codebase. The project combines low-level system emulation, a modern CMake build, cross-platform testing, and an experimental JIT effort built around ARM64 execution.

### Highlights

- Cross-platform build and CI support
- Linux, macOS, Windows, Android, and ARM64 targets
- Defensive runtime and checked arithmetic infrastructure
- Debug GUI with live reload support
- Experimental JIT and filesystem/crypto subsystem work
- Clean project structure and architecture-first design

---

## Current Status

Compound is in an early-stage development state. The project is capable of building, has a working test suite, and has many foundational systems in place. However, it does not yet run commercial titles.

| Area | Status |
| --- | --- |
| Build system and presets | ✅ Complete |
| CI/CD | ✅ Complete |
| Unit test coverage | ✅ Working |
| Host memory accounting | ✅ Working |
| Logging and debug tooling | ✅ Working |
| Debug GUI | ✅ Working |
| Ballistic ARM64 JIT integration | 🚧 Experimental / partial |
| Guest CPU and Horizon OS | 🚧 Not implemented |
| Filesystem parsing | 🚧 Experimental work in progress |
| Cryptography support | 🚧 Experimental work in progress |
| Full Switch compatibility | 🚧 Planned |

### Architecture notes

Performance work is centered around the Ballistic ARM64 recompiler. The emulator intentionally avoids forcing ARM64 JIT support on unsupported platforms while preserving a clean build matrix.

The repository currently includes experimental branches for:

- `general-upgrade-experimental-fs-hfs0-parser`
- `general-upgrade-experimental-fs-reader`
- `general-upgrade-experimental-crypto-sha256`
- `general-upgrade-experimental-jit-memory-ballistic`
- `general-upgrade-experimental-jit-metadata`
- `general-upgrade-experimental-core-error-logs`
- `general-upgrade-experimental-tests-jit-metadata`

This reflects a staged approach: foundational runtime support first, then JIT, then guest CPU and OS emulation.

---

## Roadmap

### Phase 1: Foundation and runtime

- [x] Add `mimalloc` host allocator
- [x] Establish core build and CI infrastructure
- [x] Add unit tests and debug instrumentation
- [ ] Improve memory ownership and allocator design for guest OS memory
- [ ] Harden checked arithmetic and validation paths

### Phase 2: Filesystem and crypto

- [ ] Build HFS0 filesystem parser
- [ ] Implement filesystem reader abstraction
- [ ] Add SHA256 and cryptographic primitives
- [ ] Add verification and signature-aware file handling

### Phase 3: JIT and execution engine

- [ ] Create JIT code cache allocator
- [ ] Implement JIT metadata manager
- [ ] Add core error logging and diagnostic systems
- [ ] Expand JIT validation tests
- [ ] Integrate Ballistic into the execution loop

### Phase 4: Guest CPU and OS emulation

- [ ] Implement ARM64 guest CPU instruction dispatch
- [ ] Implement Horizon OS kernel abstraction
- [ ] Add system call handling and scheduling logic
- [ ] Integrate core OS services and memory layout

### Phase 5: Graphics and completeness

- [ ] Translate SM86 to SPIR-V
- [ ] Add Vulkan render backend
- [ ] Expand input, audio, and platform services
- [ ] Reach a bootable, self-hosted guest state
- [ ] Support a wider range of titles and software compatibility

---

## Branch Strategy

The repository is organized around a few focused development branches:

| Branch | Purpose |
| --- | --- |
| `main` | Stable baseline and release-ready state |
| `general-upgrade-dev-v1` | Main development track |
| `general-upgrade-experimental-*` | Feature-specific experimental builds |
| `origin/enhancement/project-upgrade-core-cmake` | CMake and build-system improvements |

This branch layout matches the project’s current progression: feature experiments first, then selective merging into the main development stream.

---

## Building

### Requirements

- CMake 3.25+
- Ninja
- Clang compiler

### Quick start

```bash
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug
```

### Presets

| Preset | Target | Notes |
| --- | --- | --- |
| `debug`, `release` | Linux / macOS x86-64 | ASan + UBSan in debug |
| `debug-windows`, `release-windows` | Windows x86-64 | clang-cl |
| `linux-aarch64-debug`, `linux-aarch64-release` | ARM64 Linux | Cross-compiled, tested under QEMU |
| `android-arm64-debug`, `android-arm64-release` | Android arm64-v8a | Produces `libmain.so` |

For setup steps, prerequisites, and Android packaging details, see [docs/BUILDING.md](docs/BUILDING.md).

---

## Contributing

Before contributing, read [CONTRIBUTING.md](CONTRIBUTING.md) and the project documentation.

Relevant references:

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- [docs/CI.md](docs/CI.md)
- [docs/PROGRAMMING_RULES.md](docs/PROGRAMMING_RULES.md)
- [docs/SM86_TO_SPIRV_RULES.md](docs/SM86_TO_SPIRV_RULES.md)

---

## Repository Layout

```text
src/              Emulator core, runtime, GUI, and bootstrap logic
cmake/            Build modules, toolchains, and platform configuration
tests/            Unit tests and lightweight validation harness
android/          Android packaging and JNI integration
extern/           Vendored dependencies and third-party code
docs/             Architecture, build, CI, and contributor docs
```

---

## License

Compound is licensed under the GNU General Public License v3.0. See [LICENSE](LICENSE) for details.

---

<p align="center">
  <strong>Project status:</strong> early-stage emulator development<br>
  <strong>Focus:</strong> runtime foundations, JIT, guest execution, compatibility
</p>

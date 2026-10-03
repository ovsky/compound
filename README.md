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

<div align="center">

# 🎮 Compound Emulator

**🌟 Open-Source and the World's First Nintendo Switch 2 Emulator**

It is the Nintendo Switch 1 Emulator too!

**Ultra-high-fidelity system emulation. Highly experimental, but steadily growing.**

[![Stars](https://img.shields.io/github/stars/ovsky/compound?style=flat-square)](https://github.com/ovsky/compound/stargazers)
[![License](https://img.shields.io/badge/license-GPLv3-blue?style=flat-square)](LICENSE)
[![C Language](https://img.shields.io/badge/Language-C%2F%2FP%2B%2B-5E6EF2?style=flat-square)](https://github.com/ovsky/compound)
[![Cross-Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20macOS%20%7C%20Windows%20%7C%20Android%20%7C%20ARM64-brightgreen?style=flat-square)](https://github.com/ovsky/compound)

</div>

---

## 🎬 Preview

<details open>
<summary><b>📸 Screenshots & Gameplay</b></summary>

> Click to expand and view the latest emulation screenshots

<div align="center">

![Preview 1](https://i.imgur.com/x3fik3d.png)

![Preview 2](https://i.imgur.com/60KySYs.png)

</div>

</details>

---

## ✨ What's Compound?

**Compound** is an ambitious, production-focused Nintendo Switch emulator project written in **clean C** with a relentless focus on **correctness, performance, and portability**. Unlike many emulation projects, Compound emphasizes:

- 🧬 **Defensive architecture** with checked arithmetic and runtime validation
- 🏗️ **Architecture-first design** with clear separation of concerns
- 🚀 **High-performance JIT compilation** via the Ballistic ARM64 recompiler
- 🌍 **Cross-platform excellence** – Linux, macOS, Windows, Android, and ARM64
- 📊 **Professional-grade debugging** with GUI, live reload, and comprehensive logging
- 🎯 **Staged development approach** – foundations first, then execution, then compatibility

**Perfect for:**
- 🎮 Gaming enthusiasts and emulation developers
- 🔬 Systems programming researchers and students
- 🛠️ Hardware debugging and reverse engineering

---

## 🎯 Key Highlights

| Feature | Status | Details |
|---------|--------|---------|
| 🧱 **Build & CI Infrastructure** | ✅ Complete | Presets, CMake 3.25+, cross-platform CI |
| 🏭 **Runtime & Memory System** | ✅ Complete | `mimalloc` allocator, checked arithmetic, host accounting |
| 🐛 **Debug Tooling** | ✅ Working | GUI with live reload, comprehensive logging |
| ⚡ **Ballistic ARM64 JIT** | 🚧 Experimental | In-progress recompiler integration |
| 💾 **Filesystem & Crypto** | 🚧 Experimental | HFS0 parser, SHA256, verification systems |
| 🖥️ **Guest CPU & OS** | 🔮 Planned | ARM64 dispatch, Horizon kernel, syscalls |
| 🎨 **Graphics & Audio** | 🔮 Planned | SM86→SPIR-V, Vulkan backend, multi-media |
| 🎮 **Commercial Titles** | 🔮 Planned | Compatibility layer and title support |

---

## 📊 Project Status

Compound is in **early-stage, active development**. The emulator has:

✅ A solid **foundation** with working unit tests and presets  
✅ **Defensive systems** for memory safety and correctness  
✅ **Professional debugging** tools with GUI and instrumentation  
🚧 **Experimental work** in JIT, crypto, and filesystem parsing  
🔮 **Planned**: Guest execution, compatibility, graphics, and full Switch support  

### Current Capabilities

```
✓ Build system (CMake, Ninja, Clang)
✓ CI/CD (Linux, macOS, Windows, Android, ARM64)
✓ Unit testing and validation
✓ Memory accounting and profiling
✓ Debug GUI with real-time diagnostics
✓ Logging framework
✗ Guest CPU execution (in progress)
✗ Commercial title support (planned)
```

---

## 🛣️ Development Roadmap

### 🏗️ Phase 1: Foundation & Runtime
- [x] `mimalloc` host allocator integration
- [x] Build and CI infrastructure
- [x] Unit tests and debug instrumentation
- [ ] Advanced memory ownership and allocation strategies
- [ ] Hardened validation paths

### 📁 Phase 2: Filesystem & Cryptography
- [ ] HFS0 filesystem parser
- [ ] Filesystem reader abstraction
- [ ] SHA256 and crypto primitives
- [ ] Verification and signature handling

### ⚡ Phase 3: JIT & Execution Engine
- [ ] Ballistic ARM64 code cache allocator
- [ ] JIT metadata manager
- [ ] Error logging and diagnostics
- [ ] Execution loop integration

### 🖥️ Phase 4: Guest CPU & OS Emulation
- [ ] ARM64 CPU instruction dispatch
- [ ] Horizon OS kernel abstraction
- [ ] System call handling and scheduling
- [ ] Core OS services and memory layout

### 🎨 Phase 5: Graphics & Completeness
- [ ] SM86 → SPIR-V translation
- [ ] Vulkan render backend
- [ ] Input, audio, and platform services
- [ ] Bootable guest state and title compatibility

---

## 🌳 Repository Structure

```
compound/
├── 📁 src/
│   ├── emulator core and runtime
│   ├── GUI and debug tools
│   └── bootstrap and initialization
├── 📁 cmake/
│   ├── build modules and toolchains
│   └── platform-specific configuration
├── 📁 tests/
│   ├── unit test suite
│   └── validation harness
├── 📁 android/
│   ├── Android packaging
│   └── JNI integration
├── 📁 extern/
│   ├── vendored dependencies
│   └── third-party libraries
├── 📁 docs/
│   ├── architecture and design
│   ├── build and CI documentation
│   ├── programming rules
│   └── graphics (SM86→SPIR-V) rules
└── 📁 LICENSE, README, etc.
```

---

## 🔧 Building Compound

### ✅ Requirements

- **CMake** 3.25 or later
- **Ninja** build system
- **Clang** compiler (GCC/MSVC with adjustments)
- **C++17** or later support

### 🚀 Quick Start

```bash
# Configure and build
cmake --preset debug
cmake --build --preset debug --parallel

# Run tests
ctest --preset debug

# Verbose output (optional)
cmake --build --preset debug --verbose
```

### 📋 Available Presets

| Preset | Platform | Architecture | Features |
|--------|----------|--------------|----------|
| `debug` | Linux / macOS | x86-64 | ASan + UBSan enabled |
| `release` | Linux / macOS | x86-64 | Optimized build |
| `debug-windows` | Windows | x86-64 | clang-cl compiler |
| `release-windows` | Windows | x86-64 | Optimized (clang-cl) |
| `linux-aarch64-debug` | Linux | ARM64 | Cross-compile, QEMU tested |
| `linux-aarch64-release` | Linux | ARM64 | Cross-compile, optimized |
| `android-arm64-debug` | Android | ARM64 | Produces `libmain.so` |
| `android-arm64-release` | Android | ARM64 | Optimized Android library |

For detailed setup, prerequisites, and Android packaging, see **[docs/BUILDING.md](docs/BUILDING.md)**.

---

## 📚 Documentation

Comprehensive documentation is available in the `/docs` folder:

| Document | Purpose |
|----------|---------|
| **[ARCHITECTURE.md](docs/ARCHITECTURE.md)** | System design, module breakdown, and design patterns |
| **[BUILDING.md](docs/BUILDING.md)** | Build setup, platform-specific instructions, Android packaging |
| **[CI.md](docs/CI.md)** | CI/CD workflows, automation, and testing strategies |
| **[PROGRAMMING_RULES.md](docs/PROGRAMMING_RULES.md)** | Code style, safety, and best practices |
| **[SM86_TO_SPIRV_RULES.md](docs/SM86_TO_SPIRV_RULES.md)** | Graphics shader translation rules and patterns |

---

## 🤝 Contributing

**Before contributing, please read:**

1. **[CONTRIBUTING.md](CONTRIBUTING.md)** – Contribution guidelines
2. **[PROGRAMMING_RULES.md](docs/PROGRAMMING_RULES.md)** – Code standards
3. **[ARCHITECTURE.md](docs/ARCHITECTURE.md)** – System design overview

### How to Contribute

```bash
# 1. Fork the repository
# 2. Create a feature branch
git checkout -b feature/your-amazing-feature

# 3. Commit with clear messages
git commit -m "Add: [feature description]"

# 4. Push and open a PR
git push origin feature/your-amazing-feature
```

**Good contribution areas:**
- 🐛 Bug fixes in core systems
- 📖 Documentation and examples
- ✅ Unit tests and validation
- 🚀 Performance optimizations
- 🌍 Platform support and portability

---

## 🔄 Branch Strategy

| Branch | Purpose | Status |
|--------|---------|--------|
| `main` | Stable baseline, release-ready | Production |
| `general-upgrade-dev-v1` | Primary development track | Active |
| `general-upgrade-experimental-*` | Feature-specific experimental work | In Progress |
| `origin/enhancement/project-upgrade-core-cmake` | Build system improvements | Active |

The repository uses a **staged experimental approach**: features are tested in dedicated experimental branches, then selectively merged into the main development stream.

---

## 🎓 Architecture Highlights

### Design Principles

✨ **Correctness First**  
Runtime validation, checked arithmetic, and defensive coding patterns ensure system stability.

⚡ **Performance-Conscious**  
Ballistic ARM64 JIT provides high-speed guest code execution without sacrificing clarity.

🧱 **Clean Architecture**  
Clear separation of concerns with well-defined module boundaries and interfaces.

🌍 **Cross-Platform**  
Native support for Linux, macOS, Windows, Android, and ARM64 architectures.

### Key Systems

- **Memory System**: `mimalloc` allocator with host accounting and ownership tracking
- **JIT Engine**: Ballistic ARM64 recompiler for high-performance execution
- **Debug Tools**: GUI with live reload, comprehensive logging, and profiling
- **Validation**: Extensive unit tests and runtime checks
- **Filesystem**: Experimental HFS0 parser and reader abstraction

---

## 📜 License

**Compound** is licensed under the **GNU General Public License v3.0**. See **[LICENSE](LICENSE)** for full details.

```
Copyright (C) 2024 - Emulator Contributors
Licensed under GPLv3 – You are free to use, modify, and distribute
under the terms of the GPL v3.
```

---

## 🎉 Latest Updates

### Recent Work

- 🧱 **Foundation strengthening**: Memory systems and runtime validation
- ⚡ **JIT experimentation**: Ballistic ARM64 integration progress
- 📁 **Filesystem work**: HFS0 parser development (experimental branches)
- 🔐 **Crypto primitives**: SHA256 and verification systems (experimental)
- 🐛 **Testing**: Expanded unit test coverage and CI reliability

### Next Focus

1. Guest CPU instruction dispatch (ARM64)
2. Horizon OS kernel abstraction
3. Graphics pipeline (SM86→SPIR-V)
4. Commercial title compatibility

---

<div align="center">

### 🙌 Join the Emulation Community!

**Have questions or ideas?**  
👉 Open an [issue](https://github.com/ovsky/compound/issues) or start a [discussion](https://github.com/ovsky/compound/discussions)

**Want to help?**  
👉 Check the [roadmap](#-development-roadmap) and pick a task!

---

**Made with ❤️ by the Emulation Community**

[GitHub](https://github.com/ovsky/compound) • [Issues](https://github.com/ovsky/compound/issues) • [Discussions](https://github.com/ovsky/compound/discussions)

</div>

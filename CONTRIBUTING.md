# Contributing to Pound

This project holds a deliberately high bar for code quality. Every pull request
is expected to be defensible line by line, and undocumented or silently-handled
behaviour is treated as a defect rather than a style nit. Read this document in
full before you submit anything — the review process is tedious by design, and
it is much cheaper to avoid the round trips.

- [Before you start](#before-you-start)
- [The review standard](#the-review-standard)
- [Coding rules](#coding-rules)
- [Error handling](#error-handling)
- [Adding tests](#adding-tests)
- [Build configuration](#build-configuration)
- [Platform support](#platform-support)
- [Documentation](#documentation)
- [Commit and PR hygiene](#commit-and-pr-hygiene)
- [Vendored code](#vendored-code)

---

## Before you start

Pound is early-stage. The guest CPU is not implemented, and the highest-value
contributions right now are to the [Ballistic](https://github.com/pound-emu/ballistic)
ARM64 recompiler and to the core subsystems listed in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

If you are unsure whether a change is wanted, open an issue before writing it.
Patches that reimplement existing functionality, or that add a second code path
where one already exists, will be declined.

---

## The review standard

By opening a pull request you agree to:

1. Defend every line you added or changed.
2. Explain the reasoning behind every design decision.
3. Discuss and justify your approach to solving the problem.
4. Address all feedback and make requested changes.
5. Have tested your change and be confident in its correctness.

A comment explaining *why* is always better than one restating *what*.

---

## Coding rules

Read [docs/PROGRAMMING_RULES.md](docs/PROGRAMMING_RULES.md) for the full
conventions. The essentials:

- **Strict ISO C11.** `CMAKE_C_EXTENSIONS` is `OFF`; no GNU extensions.
- **Clang only.** Do not add GCC or MSVC compatibility shims.
- **Warnings are errors in spirit.** The debug presets enable `-Wall -Wextra
  -Wconversion -Wsign-conversion -Wshadow -Wstrict-prototypes -Wpadded -Wvla`.
  A new warning is a defect, even if the code is correct.
- **No silent failure.** See [Error handling](#error-handling).
- **No placeholders.** A merged function is a complete function. `TODO`,
  stubs, and `// ... existing code` are not acceptable, and CI fails on them
  (see [docs/CI.md](docs/CI.md#placeholder-audit)).
- **Comment the *why*.** The code already says what it does. Explain the
  invariant, the reason for the ordering, and the bug being avoided.
- **Match the surrounding style.** 4-space indent, braces on their own line,
  `_Alignas(64)` on hot data structures, return type on its own line for
  multi-line signatures.

### Struct layout

Hot structures are explicitly sized and padded, because Ballistic accesses
fields by offset:

```c
POUND_ALIGNED(64) typedef struct
{
    uint8_t *host_base;
    size_t   host_size;
    ...
    char pad[32];
} guest_memory_t;

static_assert(64 == sizeof(guest_memory_t), "Struct size mismatch");
```

If you change such a struct, update the `static_assert` **and**
`tests/test_guest_state.c`, which pins the field offsets as a regression guard.

---

## Error handling

This is the rule reviewers check most often.

1. **Check every return value.** No exceptions in C, no error propagation on
   its own.
2. **Report the failure through both channels.** Return a typed status *and*
   write a log record. A function that returns `false` silently leaves the
   caller no way to tell a benign condition from a bug.
3. **Never swallow an error.** No `if (!ok) { /* ignore */ }`, no bare
   `(void)` on a fallible call without a comment explaining why it is safe.
4. **Guard every pointer and every size.** Public entry points validate their
   arguments before touching them.
5. **Use the project's log macros.** `POUND_LOG_ERROR`, `POUND_LOG_WARN`,
   `POUND_LOG_INFO`, `POUND_LOG_DEBUG`, with `&thread_logger` and a format
   string that matches its arguments exactly.
6. **Prefer a typed error over a sentinel where a typed error exists.** Use
   `error_t` or `safe_math_error_t`; do not return `-1`.

```c
/* Correct. */
if (NULL == pointer)
{
    POUND_LOG_ERROR(&thread_logger, "Aborting function: pointer is NULL.");
    return false;
}

/* Rejected: the caller cannot distinguish this from success. */
if (NULL == pointer) { return false; }
```

Bounds and overflow are checked, not assumed. Prefer the `safe_math_*`
wrappers over raw arithmetic on guest-controlled values.

---

## Adding tests

The harness lives in `tests/pound_test.h` and has no third-party dependencies.

1. Add or extend a suite in `tests/test_<area>.c`. If the area has no file yet,
   create one and add it to `POUND_TEST_SOURCES` in `tests/CMakeLists.txt`.
2. Declare the suite with `POUND_TEST_SUITE(<name>, ...)` at the bottom of the
   file. That macro generates the `pound_register_<name>_tests` symbol, which
   `tests/test_main.c` declares and calls. Adding a suite therefore means
   touching two files — that is intentional, so nothing is ever registered by
   accident.
3. Run:

```sh
cmake --preset debug
cmake --build --preset debug --parallel
ctest  --preset debug
```

### What a good test looks like

- Assert on a **delta**, not an absolute counter, when the counter is
  process-global. `POUND_CHECK_EQ_U64(after, before)` rather than `== 0`.
- Test the error path, not just the happy path. Every guard added to a public
  entry point deserves a case that proves the guard fires.
- Assert on the log record as well as the return value:

```c
POUND_TEST(example, reports_failure_loudly)
{
    pound_test_log_reset();

    POUND_CHECK_EQ_I64(example_f(NULL), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);

    pound_test_log_reset();
}
```

- Reset the captured log between cases, or counts leak into the next assertion.

---

## Build configuration

- Prefer **CMake presets** over ad-hoc `-D` invocations. If a lane needs a new
  flag combination, add a preset so CI and developers use the same one.
- New build logic belongs in a `cmake/*.cmake` module with a doc comment
  explaining what it does and why it exists, not inline in the top-level
  `CMakeLists.txt`.
- Anything platform-specific must go through `cmake/Platform.cmake`, so there
  is exactly one place that knows how a platform is detected.
- Adding a platform means: a `pound_detect_platform` branch, output-directory
  handling, a preset, a toolchain file if cross-compiled, and a CI lane.

### Cross-compiled targets

Sanitizers must be dropped explicitly (`pound_resolve_sanitizers()`), because a
cross-compiled ASan runtime cannot be loaded and failing at link time is more
confusing than a documented downgrade.

---

## Platform support

Linux, macOS, Windows and Android are supported. Anything platform-conditional in
`src/` **must** be expressed through the macros in `src/core/platform.h`:

| Macro | Meaning |
| --- | --- |
| `POUND_PLATFORM_WINDOWS` / `_MAC` / `_LINUX` / `_ANDROID` | Target OS |
| `POUND_PLATFORM_MOBILE` | Android or iOS |
| `POUND_PLATFORM_SUPPORTS_HOT_RELOAD` | `0` on Android |
| `POUND_ARCHITECTURE_ARM64` / `_X86` | Target CPU |
| `POUND_GUEST_ENDIAN_LIT` / `_BIG` | Guest byte order |
| `POUND_PLATFORM_NAME` | Human-readable name, for log messages |

`POUND_PLATFORM_ANDROID` is tested **before** `POUND_PLATFORM_LINUX`, because
the NDK reports `CMAKE_SYSTEM_NAME` as `Android` while leaving `__linux__`
defined. Do not reorder those.

Never test `__linux__` directly in `src/`.

---

## Documentation

- A behaviour change without a documentation change is an incomplete change.
- New CMake options belong in the options table in
  [docs/BUILDING.md](docs/BUILDING.md#cmake-options).
- New CI lanes belong in [docs/CI.md](docs/CI.md).
- Keep `README.md` honest. If a feature is not finished, say so — the status
  table is not marketing.

---

## Commit and PR hygiene

- One logical change per pull request.
- Write the commit subject in the imperative mood: "Fix bucket masking in
  memory accounting", not "Fixed" or "fixes".
- Explain the *why* in the body. What the diff already shows.
- Reference the issue the change fixes.
- Fill in the pull request template completely, including the breaking-changes
  section — write "None" rather than deleting it.
- Note any new dependencies, and why they are necessary.

---

## Vendored code

`extern/` contains third-party sources (SDL, mimalloc, cimgui, Ballistic).

- **Do not modify anything under `extern/`.** Upstream contributions belong
  upstream. Note in particular that SDL's own repository prohibits
  AI-generated contributions to it.
- Wrap vendored code rather than editing it. If a change requires patching a
  vendored tree, stop and find a way to configure it from CMake instead.
- Do not bump a vendored dependency in the same pull request as a feature
  change; the diff becomes unreviewable.

---

## Security

Do not commit credentials, keystores, signing keys or tokens. The CI keystore in
`.github/workflows/android-apk.yml` is generated at run time precisely so that
no signing material ever enters the repository.
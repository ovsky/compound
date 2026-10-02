# -----------------------------------------------------------------------------
# Pound unit test suite
# -----------------------------------------------------------------------------
#
# The suite deliberately links only PoundCore. None of the code under test
# touches SDL or ImGui, and pulling them in would make the suite impossible to
# run in the headless CI container or inside a cross-compiled QEMU sandbox,
# which is where most of these regressions would otherwise first be seen.
#
# Android builds skip the suite entirely: a test binary cannot be executed on
# the device from a CMake build, so producing one would only add an artefact
# nobody can run. The same code paths are covered by the native x86-64 and
# aarch64 lanes.

if (POUND_TARGET_IS_ANDROID)
    message(STATUS "[PoundTests] Skipped: not runnable on Android.")
    return()
endif ()

set(POUND_TEST_SUITES
        safe_math
        guest_memory
        guest_state
        memory
        pool_allocator
        mutex
        slab_allocator
        jit_cache
        sha256
        fs_reader
        keys
        pfs0
        metadata
        )

set(POUND_TEST_SOURCES
        pound_test.c
        test_main.c
        test_safe_math.c
        test_guest_memory.c
        test_guest_state.c
        test_memory.c
        test_pool_allocator.c
        test_slab_allocator.c
        test_jit_cache.c
        test_sha256.c
        test_fs.c
        test_jit_metadata.c
        )

# The Ballistic bridge is the only thing under test that needs the engine's headers,
# and those headers are the only reason the bridge cannot be compiled where the engine
# has no prebuild. It is compiled here, against the same source the emulator uses, so
# a change to the `bal_allocator_t` layout or a callback signature is caught by a build
# rather than by a runtime crash.
#
# The prebuilt library itself is deliberately not linked: nothing in the bridge calls
# into the engine, only fills in a callback table, so linking it would add the
# platform-specific artefact and its LuaJIT runtime to the test binary for no coverage.
# The include directory is taken straight from the tree rather than from the imported
# target, because that target's INTERFACE include only reaches a target that links it.
if (POUND_ENABLE_BALLISTIC)
    list(APPEND POUND_TEST_SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/jit/jit_ballistic.c
            test_jit_ballistic.c
            )
    list(APPEND POUND_TEST_SUITES jit_ballistic)
endif ()

add_executable(PoundTests ${POUND_TEST_SOURCES})

# `PoundCore` exports `src/core` as a PUBLIC include directory, which makes
# "attributes.h" and "memory/memory.h" resolve but not the bare
# "guest_memory.h"/"guest_state.h" that those headers themselves use internally.
# Adding `src/core/memory` reproduces the header's own view of its siblings, so
# a test can spell an include exactly the way the header under test spells it.
target_include_directories(PoundTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/memory
)

if (POUND_ENABLE_BALLISTIC)
    target_include_directories(PoundTests PRIVATE ${PROJECT_SOURCE_DIR}/extern/ballistic/include)
endif ()

# The allocator is named explicitly rather than inherited. `PoundCore` is an
# OBJECT library and therefore has no link step of its own, so its PRIVATE
# mimalloc dependency does not reach consumers through
# INTERFACE_LINK_LIBRARIES. Without this the test binary would fail to resolve
# `mi_malloc_aligned`, `mi_malloc_usable_size`, `mi_free` and `mi_stats_get`.
target_link_libraries(PoundTests PRIVATE PoundCore ${POUND_MIMALLOC_TARGET})

add_compiler_flags(PoundTests)
add_linker_flags(PoundTests)
add_sanitizers(PoundTests)

# -----------------------------------------------------------------------------
# CTest registration
# -----------------------------------------------------------------------------
#
# One entry runs everything, and one entry per suite re-runs the binary with a
# filter so `ctest -R guest_memory` narrows to a single area and a failure
# names the area it came from.

add_test(NAME PoundTests COMMAND PoundTests)

foreach (suite IN LISTS POUND_TEST_SUITES)
    add_test(NAME PoundTests.${suite} COMMAND PoundTests ${suite})
    set_tests_properties(PoundTests.${suite} PROPERTIES LABELS "${suite}")
endforeach ()

set_tests_properties(PoundTests PROPERTIES LABELS "all")

# When cross-compiling, CTest launches the binary through
# CMAKE_CROSSCOMPILING_EMULATOR (qemu-aarch64, set by the toolchain file).
# Without one the registered tests would fail with an exec error rather than
# being skipped, which would mask real failures on native lanes.
if (CMAKE_CROSSCOMPILING)
    if (NOT CMAKE_CROSSCOMPILING_EMULATOR)
        message(WARNING
                "[PoundTests] Cross-compiling to ${CMAKE_SYSTEM_PROCESSOR} with no "
                "CMAKE_CROSSCOMPILING_EMULATOR; registered tests cannot be executed.")
    endif ()
endif ()

list(LENGTH POUND_TEST_SUITES POUND_TEST_SUITE_COUNT)
math(EXPR POUND_TEST_ENTRY_COUNT "${POUND_TEST_SUITE_COUNT} + 1")
message(STATUS
        "[PoundTests] Registered ${POUND_TEST_ENTRY_COUNT} CTest entries covering "
        "${POUND_TEST_SUITE_COUNT} suites: PoundTests plus one filtered entry per suite.")
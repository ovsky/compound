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
        )

set(POUND_TEST_SOURCES
        pound_test.c
        test_main.c
        test_safe_math.c
        test_guest_memory.c
        test_guest_state.c
        test_memory.c
        )

add_executable(PoundTests ${POUND_TEST_SOURCES})

target_include_directories(PoundTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(PoundTests PRIVATE PoundCore)

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

message(STATUS "[PoundTests] Registered ${PROJECT_NAME} unit test suite.")
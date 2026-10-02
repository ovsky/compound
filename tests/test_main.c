//! Test suite entry point.
//!
//! Each suite lives in its own translation unit and exports exactly one
//! `pound_register_<suite>_tests` symbol; this file is the only place that has
//! to be touched when a new suite is added.

#include "pound_test.h"

#include "attributes.h"
#include "memory/memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void pound_register_safe_math_tests(void);
void pound_register_guest_memory_tests(void);
void pound_register_guest_state_tests(void);
void pound_register_memory_tests(void);
void pound_register_pool_allocator_tests(void);
void pound_register_mutex_tests(void);
void pound_register_slab_allocator_tests(void);
void pound_register_jit_cache_tests(void);
void pound_register_sha256_tests(void);
void pound_register_fs_reader_tests(void);
void pound_register_keys_tests(void);
void pound_register_pfs0_tests(void);
void pound_register_metadata_tests(void);

// Registered only by `tests/CMakeLists.txt` when the Ballistic engine is linked, and
// therefore only defined there. The weak declaration is what lets one `main` serve
// every configuration: a strong one here would be a duplicate symbol, and an
// unconditional call would be an unresolved one on a lane with no prebuild.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak)) void pound_register_jit_ballistic_tests(void);
__attribute__((weak)) void pound_register_ballistic_engine_tests(void);
#endif

static void
print_usage(const char *argv0)
{
    fprintf(stderr, "usage: %s [filter]\n", argv0);
    fprintf(stderr, "  filter  optional substring match against \"suite.case\"\n");
}

int
main(int argc, char **argv)
{
    // Every Pound entry point logs on its error path, and the suite
    // deliberately drives those paths. Route them into the capture buffer so
    // they are assertable rather than noise.
    pound_test_log_capture_begin();

    memory_subsystem_init();

    if (argc > 2)
    {
        print_usage(argv[0]);
        pound_test_log_capture_end();
        return EXIT_FAILURE;
    }

    if ((argc == 2) && (0 == strcmp(argv[1], "--help")))
    {
        print_usage(argv[0]);
        pound_test_log_capture_end();
        return EXIT_SUCCESS;
    }

    pound_register_safe_math_tests();
    pound_register_guest_memory_tests();
    pound_register_guest_state_tests();
    pound_register_memory_tests();
    pound_register_pool_allocator_tests();
    pound_register_mutex_tests();
    pound_register_slab_allocator_tests();
    pound_register_jit_cache_tests();
    pound_register_sha256_tests();
    pound_register_fs_reader_tests();
    pound_register_keys_tests();
    pound_register_pfs0_tests();
    pound_register_metadata_tests();
    pound_register_ballistic_engine_tests();

    // Absent on a lane with no Ballistic prebuild, in which case there is nothing to
    // register and the suites above already cover the code cache underneath it.
    if (NULL != pound_register_jit_ballistic_tests)
    {
        pound_register_jit_ballistic_tests();
    }

    const char *filter      = (argc == 2) ? argv[1] : NULL;
    const int   exit_status = pound_test_run_all(filter);

    memory_subsystem_destroy();
    pound_test_log_capture_end();

    return exit_status;
}

/*** end of file ***/
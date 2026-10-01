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

    const char *filter      = (argc == 2) ? argv[1] : NULL;
    const int   exit_status = pound_test_run_all(filter);

    memory_subsystem_destroy();
    pound_test_log_capture_end();

    return exit_status;
}

/*** end of file ***/
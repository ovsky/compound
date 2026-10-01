#include "pound_test.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// -----------------------------------------------------------------------------
// Registry
// -----------------------------------------------------------------------------

static const pound_test_t *g_cases[POUND_TEST_MAX_CASES];
static size_t             g_case_count;

// -----------------------------------------------------------------------------
// Running-case state
// -----------------------------------------------------------------------------

static const char *g_current_case;
static unsigned    g_current_case_failures;

static unsigned
run_one(const pound_test_t *test)
{
    g_current_case          = test;
    g_current_case_failures = 0;

    // Log state is per-case. Without this, a case asserting "no error was
    // logged" would observe records emitted by whichever case ran before it,
    // making the suite order-dependent.
    pound_test_log_reset();

    printf("[ RUN      ] %s.%s\n", test->suite, test->name);
    fflush(stdout);

    test->fn();

    const unsigned failures = g_current_case_failures;

    if (0 == failures)
    {
        printf("[       OK ] %s.%s\n", test->suite, test->name);
    }
    else
    {
        printf("[   FAILED ] %s.%s (%u failed check(s))\n", test->suite, test->name, failures);
    }

    fflush(stdout);
    g_current_case = NULL;
    return failures;
}

void
pound_test_register(const pound_test_t *test)
{
    if (g_case_count >= POUND_TEST_MAX_CASES)
    {
        fprintf(stderr,
                "[ FATAL    ] test registry overflow (> %d cases); "
                "raise POUND_TEST_MAX_CASES.\n",
                POUND_TEST_MAX_CASES);
        abort();
    }

    g_cases[g_case_count] = test;
    g_case_count++;
}

void
pound_test_fail(const char *file, const int line, const char *format, ...)
{
    // A failure reported outside a running case means the harness itself is
    // broken; say so loudly instead of silently counting it.
    if (NULL == g_current_case)
    {
        fprintf(stderr, "[ FATAL    ] failure reported outside a test case: %s:%d\n", file, line);
        abort();
    }

    g_current_case_failures++;

    printf("             %s:%d: ", file, line);

    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);

    printf("\n");
    fflush(stdout);
}

int
pound_test_run_all(const char *filter)
{
    const char *needle = (filter != NULL) ? filter : "";

    unsigned run    = 0;
    unsigned passed = 0;
    unsigned failed = 0;

    for (size_t i = 0; i < g_case_count; ++i)
    {
        char label[256];

        const int written = snprintf(label, sizeof(label), "%s.%s", g_cases[i]->suite, g_cases[i]->name);

        if (written < 0)
        {
            snprintf(label, sizeof(label), "<unnamed case>");
        }

        if ('\0' != needle[0] && (NULL == strstr(label, needle)))
        {
            continue;
        }

        run++;

        if (0 == run_one(g_cases[i]))
        {
            passed++;
        }
        else
        {
            failed++;
        }
    }

    printf("\n[==========] %u case(s) run: %u passed, %u failed.\n", run, passed, failed);

    if (0 == run)
    {
        printf("[  NO TESTS ] filter '%s' matched nothing.\n", needle);
        return 1;
    }

    if (0 == failed)
    {
        printf("[  PASSED   ] %u case(s).\n", passed);
        return 0;
    }

    printf("[  FAILED   ] %u of %u case(s) failed.\n", failed, run);
    return 1;
}

// -----------------------------------------------------------------------------
// Log capture
// -----------------------------------------------------------------------------
//
// Captures the fully rendered message text, not just a count, so that
// `pound_test_log_contains` can assert a *specific* diagnostic was emitted
// rather than merely that something was. Several Pound APIs are contractually
// required to report failure through both a return value and a log record, and
// this is what lets a test check both halves.

#define POUND_TEST_LOG_CAPACITY 8192

static char     g_log_text[POUND_TEST_LOG_CAPACITY];
static size_t   g_log_length;
static unsigned g_log_total;
static unsigned g_log_per_level[LOG_LEVEL_TRACE + 1];

static const char *
log_level_tag(const log_level_t level)
{
    switch (level)
    {
        case LOG_LEVEL_ERROR:
            return "ERROR";
        case LOG_LEVEL_WARN:
            return "WARN";
        case LOG_LEVEL_INFO:
            return "INFO";
        case LOG_LEVEL_DEBUG:
            return "DEBUG";
        case LOG_LEVEL_TRACE:
            return "TRACE";
        default:
            return "UNKNOWN";
    }
}

/// Appends to the capture buffer, never overflowing it.
static void
log_capture_append(const char *text)
{
    if (NULL == text)
    {
        return;
    }

    const size_t capacity = sizeof(g_log_text) - 1;

    if (g_log_length >= capacity)
    {
        return;
    }

    const size_t room     = capacity - g_log_length;
    const size_t to_copy  = strlen(text);
    const size_t copy_len = (to_copy < room) ? to_copy : room;

    memcpy(g_log_text + g_log_length, text, copy_len);
    g_log_length += copy_len;
    g_log_text[g_log_length] = '\0';
}

static void
test_log_sink(void *user_data, log_data_t *pound_data, const char *format, va_list args)
{
    POUND_UNUSED(user_data);

    if (NULL == pound_data)
    {
        return;
    }

    const log_level_t level = pound_data->level;

    if (level >= 0 && level <= LOG_LEVEL_TRACE)
    {
        g_log_per_level[level]++;
    }

    g_log_total++;

    char rendered[1024];

    va_list copy;
    va_copy(copy, args);
    const int written = vsnprintf(rendered, sizeof(rendered), format, copy);
    va_end(copy);

    if (written <= 0)
    {
        rendered[0] = '\0';
    }

    log_capture_append("[");
    log_capture_append(log_level_tag(level));
    log_capture_append("] ");
    log_capture_append(rendered);
    log_capture_append("\n");
}

void
pound_test_log_reset(void)
{
    memset(g_log_text, 0, sizeof(g_log_text));
    g_log_length = 0;
    g_log_total  = 0;
    memset(g_log_per_level, 0, sizeof(g_log_per_level));
}

void
pound_test_log_capture_begin(void)
{
    thread_logger.user_data = NULL;
    thread_logger.log       = test_log_sink;

    // Capture everything. `pound_log_message` forwards a record only when
    // `log_level <= min_level`, and the levels are ordered ERROR(0) through
    // TRACE(4), so TRACE is the only value that lets all of them through.
    // Lowering this would silently drop the WARN records that several cases
    // assert on.
    thread_logger.min_level = LOG_LEVEL_TRACE;

    pound_test_log_reset();
}

void
pound_test_log_capture_end(void)
{
    thread_logger.log = NULL;
    pound_test_log_reset();
}

unsigned
pound_test_log_count(void)
{
    return g_log_total;
}

unsigned
pound_test_log_count_at(const log_level_t level)
{
    if (level < 0 || level > LOG_LEVEL_TRACE)
    {
        return 0;
    }

    return g_log_per_level[level];
}

const char *
pound_test_log_last(void)
{
    return g_log_text;
}

bool
pound_test_log_contains(const char *needle)
{
    if ((NULL == needle) || ('\0' == needle[0]))
    {
        return false;
    }

    return (NULL != strstr(g_log_text, needle)) ? true : false;
}

/*** end of file ***/
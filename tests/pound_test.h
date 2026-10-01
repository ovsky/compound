//! Minimal, dependency-free unit-test harness.
//!
//! Pound vendors no test framework, so rather than add one this header
//! provides just enough to register cases, assert, and report. It is
//! deliberately allocation-free: the suite must stay runnable under ASan and
//! inside a cross-compiled QEMU sandbox without pulling in a dependency that
//! has not itself been cross-compiled.
//!
//! The harness also installs a capturing logger. Several Pound APIs are
//! contractually required to report failure through both a return value *and*
//! a log record; the capture lets a test assert both halves of that contract
//! rather than trusting the return value alone.

#ifndef POUND_TEST_H
#define POUND_TEST_H

#include "attributes.h"
#include "log.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef void (*pound_test_fn)(void);

typedef struct
{
    const char   *suite;
    const char   *name;
    pound_test_fn fn;
} pound_test_t;

/// Declares a test case; the body follows as a brace-enclosed block.
///
///     POUND_TEST(memory, bucket_clamping)
///     {
///         POUND_CHECK(memory_bucket_is_valid(MEMORY_BUCKET_UI));
///     }
#define POUND_TEST(suite_, name_)                          \
    static void pound_test_##suite_##_##name_(void);       \
    static void pound_test_##suite_##_##name_(void)

/// One `{ "suite", "name", fn }` initialiser for a suite table.
#define POUND_TEST_CASE(suite_, name_) \
    { #suite_, #name_, pound_test_##suite_##_##name_ }

/// Emits a suite registration function for the cases listed in `...`.
///
/// Every test translation unit ends with exactly one of these; `test_main.c`
/// declares the resulting `pound_register_<suite>_tests` symbol and calls it.
#define POUND_TEST_SUITE(suite_, ...)                                                     \
    static const pound_test_t pound_suite_table_##suite_[] = { __VA_ARGS__ };             \
    void pound_register_##suite_##_tests(void)                                             \
    {                                                                                      \
        const size_t pound_case_count_                                                     \
            = sizeof(pound_suite_table_##suite_) / sizeof(pound_suite_table_##suite_[0]);   \
        for (size_t pound_i_ = 0; pound_i_ < pound_case_count_; ++pound_i_)                \
        {                                                                                  \
            pound_test_register(&pound_suite_table_##suite_[pound_i_]);                   \
        }                                                                                  \
    }

/// Records a failure against the running case and continues.
#define POUND_CHECK(cond) \
    do \
    { \
        if (!(cond)) \
        { \
            pound_test_fail(__FILE__, __LINE__, "check failed: %s", #cond); \
        } \
    } while (0)

/// Records a failure with additional context and continues.
///
/// Use where the bare condition would not identify *which* iteration or element
/// failed. A loop that checks the same condition a hundred times is close to
/// useless without the loop variable in the message.
#define POUND_CHECK_MSG(cond, ...)                              \
    do                                                          \
    {                                                           \
        if (!(cond))                                            \
        {                                                       \
            pound_test_fail(__FILE__, __LINE__, __VA_ARGS__);   \
        }                                                       \
    } while (0)

/// Records a failure against the running case and abandons it.
///
/// Use for preconditions where continuing would dereference invalid state.
#define POUND_REQUIRE(cond) \
    do \
    { \
        if (!(cond)) \
        { \
            pound_test_fail(__FILE__, __LINE__, "required: %s", #cond); \
            return; \
        } \
    } while (0)

/// Records a failure with additional context and abandons the case.
#define POUND_REQUIRE_MSG(cond, ...)                        \
    do                                                      \
    {                                                       \
        if (!(cond))                                        \
        {                                                   \
            pound_test_fail(__FILE__, __LINE__, __VA_ARGS__); \
            return;                                         \
        }                                                   \
    } while (0)

/// Asserts a pointer is non-NULL, abandoning the case if it is not.
#define POUND_REQUIRE_PTR_NON_NULL(actual)      \
    do                                          \
    {                                           \
        if (NULL == (const void *)(actual))     \
        {                                       \
            pound_test_fail(__FILE__,           \
                            __LINE__,           \
                            "%s is NULL",       \
                            #actual);           \
            return;                             \
        }                                       \
    } while (0)

#define POUND_CHECK_EQ_U64(actual, expected)                                   \
    do \
    { \
        const uint64_t pound_a_ = (uint64_t)(actual);                          \
        const uint64_t pound_e_ = (uint64_t)(expected);                        \
        if (pound_a_ != pound_e_) \
        { \
            pound_test_fail(__FILE__, \
                            __LINE__, \
                            "%s: expected %llu, got %llu", \
                            #actual, \
                            (unsigned long long)pound_e_, \
                            (unsigned long long)pound_a_); \
        } \
    } while (0)

#define POUND_CHECK_EQ_I64(actual, expected)                                   \
    do \
    { \
        const int64_t pound_a_ = (int64_t)(actual);                            \
        const int64_t pound_e_ = (int64_t)(expected);                          \
        if (pound_a_ != pound_e_) \
        { \
            pound_test_fail(__FILE__, \
                            __LINE__, \
                            "%s: expected %lld, got %lld", \
                            #actual, \
                            (long long)pound_e_, \
                            (long long)pound_a_); \
        } \
    } while (0)

#define POUND_CHECK_PTR_EQ(actual, expected)                                   \
    do \
    { \
        const void *pound_a_ = (const void *)(actual);                         \
        const void *pound_e_ = (const void *)(expected);                       \
        if (pound_a_ != pound_e_) \
        { \
            pound_test_fail(__FILE__, \
                            __LINE__, \
                            "%s: expected %p, got %p", \
                            #actual, pound_e_, pound_a_); \
        } \
    } while (0)

#define POUND_CHECK_PTR_NULL(actual) POUND_CHECK_PTR_EQ(actual, NULL)
#define POUND_CHECK_PTR_NON_NULL(actual) \
    do \
    { \
        if (NULL == (const void *)(actual)) \
        { \
            pound_test_fail(__FILE__, __LINE__, "%s: expected non-NULL", #actual); \
        } \
    } while (0)

#define POUND_CHECK_STR_EQ(actual, expected)                                   \
    do \
    { \
        const char *pound_a_ = (actual);                                       \
        const char *pound_e_ = (expected);                                     \
        if ((pound_a_ == NULL) || (pound_e_ == NULL) || (0 != strcmp(pound_a_, pound_e_))) \
        { \
            pound_test_fail(__FILE__, \
                            __LINE__, \
                            "%s: expected \"%s\", got \"%s\"", \
                            #actual, \
                            (pound_e_ != NULL) ? pound_e_ : "(null)", \
                            (pound_a_ != NULL) ? pound_a_ : "(null)"); \
        } \
    } while (0)

// -----------------------------------------------------------------------------
// Harness API
// -----------------------------------------------------------------------------

/// Upper bound on registered cases. The suite is a fixed set of translation
/// units, so exceeding this is a programming error rather than a runtime
/// condition.
#define POUND_TEST_MAX_CASES 512

/// Registers one case with the global registry.
void pound_test_register(const pound_test_t *test);

/// Records a failure against the currently running case.
POUND_COLD void pound_test_fail(const char *file, int line, const char *format, ...);

/// Runs every registered case whose "suite.name" contains `filter`.
///
/// Pass `NULL` or `""` to run everything. Returns a process exit code.
int pound_test_run_all(const char *filter);

/// Routes `thread_logger` at the capturing sink and clears its buffers.
void pound_test_log_capture_begin(void);

/// Restores the default logger.
void pound_test_log_capture_end(void);

/// Clears the captured log without touching the logger configuration.
void pound_test_log_reset(void);

/// Total number of records captured since the last reset.
unsigned pound_test_log_count(void);

/// Number of records captured at exactly `level` since the last reset.
unsigned pound_test_log_count_at(log_level_t level);

/// Text of the most recently captured record, or `""`.
const char *pound_test_log_last(void);

/// True when any captured record contains `needle`.
bool pound_test_log_contains(const char *needle);

#endif // POUND_TEST_H

/*** end of file ***/
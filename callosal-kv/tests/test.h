// test.h — minimal unit-test harness.
//
// Design goals:
//   - Zero external dependency.
//   - One test binary that runs every test and exits non-zero on failure.
//   - Each test is a plain `void test_xxx(void)` that uses ASSERT_* macros.
//     Failures increment a global counter and return from the function so
//     subsequent tests still run.
//
// Adding a new test is two edits: define it in tests/test_<module>.c, then
// register it in the TEST_REGISTRY in tests/test_main.c.

#ifndef CALLOSAL_KV_TEST_H
#define CALLOSAL_KV_TEST_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int g_test_failures;

#define ASSERT_TRUE(cond)                                                    \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "    ASSERT failed: %s  (%s:%d)\n",              \
                    #cond, __FILE__, __LINE__);                              \
            g_test_failures++;                                               \
            return;                                                          \
        }                                                                    \
    } while (0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

#define ASSERT_EQ_INT(a, b)                                                  \
    do {                                                                     \
        long long _a = (long long)(a);                                       \
        long long _b = (long long)(b);                                       \
        if (_a != _b) {                                                      \
            fprintf(stderr,                                                  \
                    "    ASSERT_EQ_INT failed: %s == %s "                    \
                    "(%lld != %lld)  (%s:%d)\n",                             \
                    #a, #b, _a, _b, __FILE__, __LINE__);                     \
            g_test_failures++;                                               \
            return;                                                          \
        }                                                                    \
    } while (0)

#define ASSERT_EQ_U64(a, b)                                                  \
    do {                                                                     \
        unsigned long long _a = (unsigned long long)(a);                     \
        unsigned long long _b = (unsigned long long)(b);                     \
        if (_a != _b) {                                                      \
            fprintf(stderr,                                                  \
                    "    ASSERT_EQ_U64 failed: %s == %s "                    \
                    "(%llu != %llu)  (%s:%d)\n",                             \
                    #a, #b, _a, _b, __FILE__, __LINE__);                     \
            g_test_failures++;                                               \
            return;                                                          \
        }                                                                    \
    } while (0)

#define ASSERT_EQ_MEM(a, b, n)                                               \
    do {                                                                     \
        if (memcmp((a), (b), (n)) != 0) {                                    \
            fprintf(stderr,                                                  \
                    "    ASSERT_EQ_MEM failed: %s vs %s over %zu bytes "     \
                    "(%s:%d)\n",                                             \
                    #a, #b, (size_t)(n), __FILE__, __LINE__);                \
            g_test_failures++;                                               \
            return;                                                          \
        }                                                                    \
    } while (0)

#define ASSERT_NEAR(a, b, tol)                                               \
    do {                                                                     \
        double _a = (double)(a);                                             \
        double _b = (double)(b);                                             \
        double _t = (double)(tol);                                           \
        double _d = _a - _b;                                                 \
        if (_d < 0.0) { _d = -_d; }                                          \
        if (_d > _t) {                                                       \
            fprintf(stderr,                                                  \
                    "    ASSERT_NEAR failed: %s ~ %s "                       \
                    "(|%g - %g| = %g > %g)  (%s:%d)\n",                      \
                    #a, #b, _a, _b, _d, _t, __FILE__, __LINE__);             \
            g_test_failures++;                                               \
            return;                                                          \
        }                                                                    \
    } while (0)

struct test_case {
    const char *name;
    void (*fn)(void);
};

#endif  // CALLOSAL_KV_TEST_H

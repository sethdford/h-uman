#ifndef HU_TEST_FRAMEWORK_H
#define HU_TEST_FRAMEWORK_H

#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int hu__total;
extern int hu__passed;
extern int hu__failed;
extern jmp_buf hu__jmp;

#define HU_FAIL(...)                                    \
    do {                                                \
        printf("  FAIL  (%s:%d) ", __FILE__, __LINE__); \
        printf(__VA_ARGS__);                            \
        printf("\n");                                   \
        longjmp(hu__jmp, 1);                            \
    } while (0)

#define HU_ASSERT(cond)                          \
    do {                                         \
        if (!(cond))                             \
            HU_FAIL("assert failed: %s", #cond); \
    } while (0)

#define HU_ASSERT_EQ(a, b)                                               \
    do {                                                                 \
        long long _a = (long long)(a), _b = (long long)(b);              \
        if (_a != _b)                                                    \
            HU_FAIL("expected %lld == %lld (%s == %s)", _a, _b, #a, #b); \
    } while (0)

#define HU_ASSERT_NEQ(a, b)                                              \
    do {                                                                 \
        long long _a = (long long)(a), _b = (long long)(b);              \
        if (_a == _b)                                                    \
            HU_FAIL("expected %lld != %lld (%s != %s)", _a, _b, #a, #b); \
    } while (0)

#define HU_ASSERT_STR_EQ(a, b)                                                            \
    do {                                                                                  \
        const char *_a = (a), *_b = (b);                                                  \
        if (!_a || !_b || strcmp(_a, _b) != 0)                                            \
            HU_FAIL("expected \"%s\" == \"%s\"", _a ? _a : "(null)", _b ? _b : "(null)"); \
    } while (0)

#define HU_ASSERT_NULL(a)                     \
    do {                                      \
        if ((a) != NULL)                      \
            HU_FAIL("expected NULL: %s", #a); \
    } while (0)

#define HU_ASSERT_NOT_NULL(a)                     \
    do {                                          \
        if ((a) == NULL)                          \
            HU_FAIL("expected not NULL: %s", #a); \
    } while (0)

#define HU_ASSERT_FLOAT_EQ(a, b, eps)              \
    do {                                           \
        double _a = (double)(a), _b = (double)(b); \
        if (fabs(_a - _b) > (eps))                 \
            HU_FAIL("expected %f ~= %f", _a, _b);  \
    } while (0)

#define HU_ASSERT_TRUE(a)  HU_ASSERT(a)
#define HU_ASSERT_FALSE(a) HU_ASSERT(!(a))

#define HU_RUN_TEST(fn)                  \
    do {                                 \
        hu__total++;                     \
        if (setjmp(hu__jmp) == 0) {      \
            fn();                        \
            hu__passed++;                \
            printf("  PASS  %s\n", #fn); \
        } else {                         \
            hu__failed++;                \
        }                                \
        fflush(stdout);                  \
    } while (0)

#define HU_TEST_SUITE(name) printf("\n=== %s ===\n", name)

#define HU_TEST_REPORT()                                              \
    do {                                                              \
        printf("\n--- Results: %d/%d passed", hu__passed, hu__total); \
        if (hu__failed > 0)                                           \
            printf(", %d FAILED", hu__failed);                        \
        printf(" ---\n");                                             \
    } while (0)

#define HU_TEST_EXIT() return hu__failed > 0 ? 1 : 0

#endif

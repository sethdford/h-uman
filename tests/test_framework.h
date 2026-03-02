#ifndef SC_TEST_FRAMEWORK_H
#define SC_TEST_FRAMEWORK_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <setjmp.h>

extern int sc__total;
extern int sc__passed;
extern int sc__failed;
extern jmp_buf sc__jmp;

#define SC_FAIL(...) \
    do { \
        printf("  FAIL  (%s:%d) ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
        longjmp(sc__jmp, 1); \
    } while(0)

#define SC_ASSERT(cond) \
    do { if (!(cond)) SC_FAIL("assert failed: %s", #cond); } while(0)

#define SC_ASSERT_EQ(a, b) \
    do { \
        long long _a = (long long)(a), _b = (long long)(b); \
        if (_a != _b) SC_FAIL("expected %lld == %lld (%s == %s)", _a, _b, #a, #b); \
    } while(0)

#define SC_ASSERT_NEQ(a, b) \
    do { \
        long long _a = (long long)(a), _b = (long long)(b); \
        if (_a == _b) SC_FAIL("expected %lld != %lld (%s != %s)", _a, _b, #a, #b); \
    } while(0)

#define SC_ASSERT_STR_EQ(a, b) \
    do { \
        const char *_a = (a), *_b = (b); \
        if (!_a || !_b || strcmp(_a, _b) != 0) \
            SC_FAIL("expected \"%s\" == \"%s\"", _a ? _a : "(null)", _b ? _b : "(null)"); \
    } while(0)

#define SC_ASSERT_NULL(a) \
    do { if ((a) != NULL) SC_FAIL("expected NULL: %s", #a); } while(0)

#define SC_ASSERT_NOT_NULL(a) \
    do { if ((a) == NULL) SC_FAIL("expected not NULL: %s", #a); } while(0)

#define SC_ASSERT_FLOAT_EQ(a, b, eps) \
    do { \
        double _a = (double)(a), _b = (double)(b); \
        if (fabs(_a - _b) > (eps)) SC_FAIL("expected %f ~= %f", _a, _b); \
    } while(0)

#define SC_ASSERT_TRUE(a)  SC_ASSERT(a)
#define SC_ASSERT_FALSE(a) SC_ASSERT(!(a))

#define SC_RUN_TEST(fn) \
    do { \
        sc__total++; \
        if (setjmp(sc__jmp) == 0) { \
            fn(); \
            sc__passed++; \
            printf("  PASS  %s\n", #fn); \
        } else { \
            sc__failed++; \
        } \
        fflush(stdout); \
    } while(0)

#define SC_TEST_SUITE(name) \
    printf("\n=== %s ===\n", name)

#define SC_TEST_REPORT() \
    do { \
        printf("\n--- Results: %d/%d passed", sc__passed, sc__total); \
        if (sc__failed > 0) printf(", %d FAILED", sc__failed); \
        printf(" ---\n"); \
    } while(0)

#define SC_TEST_EXIT() \
    return sc__failed > 0 ? 1 : 0

#endif

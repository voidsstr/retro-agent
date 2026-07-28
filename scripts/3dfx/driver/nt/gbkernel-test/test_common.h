/*
 * test_common.h - tiny host-side harness for the gbk_* pure-logic tests.
 * Tests MAY use the CRT (stdio); the modules under ../gbk MUST NOT
 * (enforced by -nostdinc on module objects in the Makefile).
 */
#ifndef GBK_TEST_COMMON_H
#define GBK_TEST_COMMON_H

#include <stdio.h>

static int gbk_test_failures = 0;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            gbk_test_failures++; \
        } \
    } while (0)

#define CHECK_EQ(a, b) \
    do { \
        unsigned long _a = (unsigned long)(a), _b = (unsigned long)(b); \
        if (_a != _b) { \
            printf("  FAIL %s:%d: %s == %s (0x%lx != 0x%lx)\n", \
                   __FILE__, __LINE__, #a, #b, _a, _b); \
            gbk_test_failures++; \
        } \
    } while (0)

#define TEST_END(name) \
    do { \
        if (gbk_test_failures == 0) { \
            printf("PASS %s\n", name); \
            return 0; \
        } \
        printf("FAIL %s (%d checks failed)\n", name, gbk_test_failures); \
        return 1; \
    } while (0)

#endif /* GBK_TEST_COMMON_H */

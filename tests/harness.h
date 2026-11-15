#ifndef BG_TEST_HARNESS_H
#define BG_TEST_HARNESS_H

#include <stdio.h>
#include <string.h>

static int g_tests, g_failed, g_cur_failed;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            fprintf(stderr, "    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            g_cur_failed = 1;                                                           \
        }                                                                               \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                              \
    do {                                                                                \
        long long _a = (long long)(a), _b = (long long)(b);                             \
        if (_a != _b) {                                                                 \
            fprintf(stderr, "    FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__,      \
                    __LINE__, #a, #b, _a, _b);                                          \
            g_cur_failed = 1;                                                           \
        }                                                                               \
    } while (0)

#define CHECK_STR(a, b)                                                                 \
    do {                                                                                \
        if (strcmp((a), (b)) != 0) {                                                    \
            fprintf(stderr, "    FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__,   \
                    (a), (b));                                                          \
            g_cur_failed = 1;                                                           \
        }                                                                               \
    } while (0)

#define RUN(fn)                                                                         \
    do {                                                                                \
        g_cur_failed = 0;                                                               \
        fn();                                                                           \
        g_tests++;                                                                      \
        if (g_cur_failed) {                                                             \
            g_failed++;                                                                 \
            fprintf(stderr, "FAIL %s\n", #fn);                                          \
        } else {                                                                        \
            printf("ok   %s\n", #fn);                                                   \
        }                                                                               \
    } while (0)

#endif

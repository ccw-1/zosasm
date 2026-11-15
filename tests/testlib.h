/* testlib.h - tiny test harness. */
#ifndef ZA_TESTLIB_H
#define ZA_TESTLIB_H

#include <stdio.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define T_UNUSED __attribute__((unused))
#else
#define T_UNUSED
#endif

static int t_fail, t_count;

#define CHECK(cond)                                                        \
    do {                                                                   \
        t_count++;                                                         \
        if (!(cond)) {                                                     \
            t_fail++;                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                  \
    } while (0)

#define CHECK_MSG(cond, ...)                                               \
    do {                                                                   \
        t_count++;                                                         \
        if (!(cond)) {                                                     \
            t_fail++;                                                      \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                    \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
        }                                                                  \
    } while (0)

#define CHECK_STR(a, b)                                                    \
    do {                                                                   \
        const char *a_ = (a), *b_ = (b);                                   \
        t_count++;                                                         \
        if (strcmp(a_, b_)) {                                              \
            t_fail++;                                                      \
            printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, a_, b_); \
        }                                                                  \
    } while (0)

T_UNUSED static int t_hexeq(const unsigned char *a, size_t na, const unsigned char *b, size_t nb) {
    return na == nb && memcmp(a, b, na) == 0;
}

T_UNUSED static void t_hexprint(const unsigned char *a, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) printf("%02X ", a[i]);
}

#define TEST_MAIN_END()                                                    \
    do {                                                                   \
        printf("%s: %d checks, %d failed\n", __FILE__, t_count, t_fail);   \
        return t_fail ? 1 : 0;                                             \
    } while (0)

#endif

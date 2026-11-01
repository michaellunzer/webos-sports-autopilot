#pragma once
#include <stdio.h>
#include <stdlib.h>

static int g_failures = 0;
#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            g_failures++;                                                   \
            fprintf(stderr, "FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                                   \
            fprintf(stderr, "\n");                                          \
        }                                                                   \
    } while (0)
#define TEST_DONE()                                                         \
    do {                                                                    \
        if (g_failures) {                                                   \
            fprintf(stderr, "%d check(s) failed\n", g_failures);            \
            return 1;                                                       \
        }                                                                   \
        printf("all checks passed\n");                                      \
        return 0;                                                           \
    } while (0)

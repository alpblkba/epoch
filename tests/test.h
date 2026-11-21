#ifndef EPOCH_TEST_H
#define EPOCH_TEST_H

#include <stdio.h>

static int test_failures;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            test_failures++;                                                       \
        }                                                                          \
    } while (0)

#define TEST_DONE(name)                                                            \
    (test_failures ? (fprintf(stderr, "%s: %d failure(s)\n", name, test_failures), 1) \
                   : (printf("%s: all checks passed\n", name), 0))

#endif

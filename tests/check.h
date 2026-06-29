/* check.h — the shared unit-test harness: a CHECK assertion macro, the pass/fail tally,
 * and a result reporter. Each test executable is a SINGLE translation unit, so including
 * this gives it its own counters (internal linkage); call check_report() from main().
 * Replaces the per-file copies that had drifted in formatting (DRY). C and C++ compatible. */
#ifndef HARP_TESTS_CHECK_H
#define HARP_TESTS_CHECK_H

#include <stdio.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (cond) {                                                         \
            g_pass++;                                                       \
        } else {                                                            \
            g_fail++;                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                   \
    } while (0)

/* Print the tally + return a main() exit code (0 = all passed). `name`, if non-NULL, prefixes
 * the line (e.g. "harp-runtime-units-tests: 38 passed, 0 failed"); NULL gives the bare form. */
static int check_report(const char *name) {
    if (name)
        printf("%s: %d passed, %d failed\n", name, g_pass, g_fail);
    else
        printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

#endif /* HARP_TESTS_CHECK_H */

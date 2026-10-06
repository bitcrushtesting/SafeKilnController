/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Minimal host test harness.  Deliberately dependency-free: CON-04 forbids a
 * build-time network fetch, and a unit test framework for pure C logic is a few
 * macros.  One executable per component, registered with ctest (TR-13).
 *
 * Test names carry the requirement they verify, because tools/trace parses them
 * (TR-22, TR-23): KILN_TEST(sr25_latches_on_uncommanded_current).
 */
#ifndef KILN_CHECK_H
#define KILN_CHECK_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*kiln_test_fn)(void);

typedef struct {
    const char   *name;
    kiln_test_fn  fn;
} kiln_test_entry_t;

#ifndef KILN_MAX_TESTS
#define KILN_MAX_TESTS 256
#endif

extern kiln_test_entry_t kiln_tests[KILN_MAX_TESTS];
extern int               kiln_test_count;
extern int               kiln_current_failures;

/* Registration without a constructor attribute would need a manual list, which
 * rots.  __attribute__((constructor)) is available on both gcc and clang, the
 * only host compilers the CI uses. */
/* The parameter is deliberately not called `name`: a macro parameter substitutes
 * everywhere the identifier appears, including in `.name` on the next line. */
#define KILN_TEST(test_name_)                                                  \
    static void kiln_test_##test_name_(void);                                  \
    __attribute__((constructor)) static void kiln_register_##test_name_(void)  \
    {                                                                          \
        if (kiln_test_count < KILN_MAX_TESTS) {                                \
            kiln_tests[kiln_test_count].name = #test_name_;                    \
            kiln_tests[kiln_test_count].fn   = kiln_test_##test_name_;         \
            kiln_test_count++;                                                 \
        } else {                                                               \
            (void)fprintf(stderr, "KILN_MAX_TESTS exceeded\n");                \
            abort();                                                           \
        }                                                                      \
    }                                                                          \
    static void kiln_test_##test_name_(void)

/* The (void) casts on every fprintf below are not decoration: NFR-17's rule is
 * that no return value is silently dropped, and cert-err33-c enforces it with
 * no baseline.  A diagnostic that cannot be written is genuinely nothing this
 * harness can do anything about, so the cast is the handling -- stated once,
 * here, rather than suppressed across 2 100 expansion sites in the suites. */
#define KILN_FAIL(...)                                                         \
    do {                                                                       \
        (void)fprintf(stderr, "    %s:%d: ", __FILE__, __LINE__);              \
        (void)fprintf(stderr, __VA_ARGS__);                                    \
        (void)fprintf(stderr, "\n");                                           \
        kiln_current_failures++;                                               \
    } while (0)

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) KILN_FAIL("CHECK(%s) failed", #cond);                      \
    } while (0)

#define CHECK_MSG(cond, ...)                                                   \
    do {                                                                       \
        if (!(cond)) { KILN_FAIL(__VA_ARGS__); }                               \
    } while (0)

#define CHECK_EQ_INT(got, want)                                                \
    do {                                                                       \
        const long long g_ = (long long)(got), w_ = (long long)(want);         \
        if (g_ != w_) KILN_FAIL("%s: got %lld, want %lld", #got, g_, w_);       \
    } while (0)

#define CHECK_EQ_UINT(got, want)                                               \
    do {                                                                       \
        const unsigned long long g_ = (unsigned long long)(got);               \
        const unsigned long long w_ = (unsigned long long)(want);              \
        if (g_ != w_) KILN_FAIL("%s: got %llu, want %llu", #got, g_, w_);      \
    } while (0)

#define CHECK_NEAR(got, want, tol)                                             \
    do {                                                                       \
        const double g_ = (double)(got), w_ = (double)(want), t_ = (double)(tol); \
        if (!(fabs(g_ - w_) <= t_))                                            \
            KILN_FAIL("%s: got %g, want %g +/- %g", #got, g_, w_, t_);         \
    } while (0)

#define CHECK_STR_EQ(got, want)                                                \
    do {                                                                       \
        const char *g_ = (got), *w_ = (want);                                  \
        if (!g_ || !w_ || strcmp(g_, w_) != 0)                                 \
            KILN_FAIL("%s: got \"%s\", want \"%s\"", #got,                     \
                      g_ ? g_ : "(null)", w_ ? w_ : "(null)");                 \
    } while (0)

#define CHECK_OK(expr)                                                         \
    do {                                                                       \
        const kiln_err_t e_ = (expr);                                          \
        if (e_ != KILN_OK) KILN_FAIL("%s: %s", #expr, kiln_err_str(e_));        \
    } while (0)

#define CHECK_ERR(expr, want)                                                  \
    do {                                                                       \
        const kiln_err_t e_ = (expr);                                          \
        if (e_ != (want))                                                      \
            KILN_FAIL("%s: got %s, want %s", #expr, kiln_err_str(e_),           \
                      kiln_err_str(want));                                     \
    } while (0)

#endif /* KILN_CHECK_H */

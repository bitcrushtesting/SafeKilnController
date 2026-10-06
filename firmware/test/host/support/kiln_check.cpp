/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The runner.  Each test executable links this and gets a main() that runs
 * every registered test, reporting one line per test and a non-zero exit on any
 * failure, which is all ctest needs.
 */

#include "kiln_check.h"

kiln_test_entry_t kiln_tests[KILN_MAX_TESTS];
int               kiln_test_count       = 0;
int               kiln_current_failures = 0;

int main(int argc, char **argv)
{
    const char *filter = (argc > 1) ? argv[1] : NULL;
    int ran = 0, failed = 0;

    for (int i = 0; i < kiln_test_count; i++) {
        if ((filter != nullptr) && (strstr(kiln_tests[i].name, filter) == nullptr)) {
            continue;
        }

        kiln_current_failures = 0;
        (void)printf("  %-58s", kiln_tests[i].name);
        (void)fflush(stdout);
        kiln_tests[i].fn();
        ran++;

        if (kiln_current_failures == 0) {
            (void)printf(" ok\n");
        } else {
            (void)printf(" FAILED (%d)\n", kiln_current_failures);
            failed++;
        }
    }

    (void)printf("%d run, %d failed\n", ran, failed);
    if (ran == 0) {
        (void)fprintf(stderr, "no tests matched\n");
        return 2;
    }
    return failed == 0 ? 0 : 1;
}

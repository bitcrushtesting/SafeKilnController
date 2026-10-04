/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 * kiln_core/profile -- FR-PRG-01, FR-PRG-02, FR-PRG-05, FR-PRG-09, NFR-19.
 */

#include <string.h>
#include "kiln_check.h"
#include "kiln_core/profile.h"

static kiln_program_t valid_program(void)
{
    kiln_program_t p;
    kiln_profile_init_empty(&p, "ok");
    p.segment_count = 1;
    p.segments[0].target_c     = 600;
    p.segments[0].rate_c_per_h = 120;
    p.segments[0].dwell_min    = 10;
    return p;
}

KILN_TEST(frprg05_accepts_a_valid_program)
{
    const kiln_program_t p = valid_program();
    const kiln_prog_validation_t v = kiln_profile_validate(&p, 1280.0f);
    CHECK_EQ_INT(v.code, KILN_PROG_OK);
    CHECK_EQ_UINT(v.segment, KILN_SEG_NONE);
}

KILN_TEST(frprg05_rejects_every_out_of_range_field_and_names_the_segment)
{
    kiln_program_t p = valid_program();
    p.segment_count = 3;
    p.segments[1] = p.segments[0];
    p.segments[2] = p.segments[0];

    p.segments[1].target_c = 1300;      /* above the configured maximum */
    CHECK_EQ_INT(kiln_profile_validate(&p, 1280.0f).code, KILN_PROG_ERR_TARGET_ABOVE_MAX);
    CHECK_EQ_UINT(kiln_profile_validate(&p, 1280.0f).segment, 1u);

    p.segments[1].target_c = 1400;      /* above the compile-time ceiling */
    CHECK_EQ_INT(kiln_profile_validate(&p, 1400.0f).code,
                 KILN_PROG_ERR_TARGET_ABOVE_CEILING);

    p = valid_program();
    p.segments[0].rate_c_per_h = KILN_RATE_MAX_C_PER_H + 1u;
    CHECK_EQ_INT(kiln_profile_validate(&p, 1280.0f).code, KILN_PROG_ERR_RATE_RANGE);

    p = valid_program();
    p.segments[0].dwell_min = KILN_DWELL_MIN_MAX + 1u;
    CHECK_EQ_INT(kiln_profile_validate(&p, 1280.0f).code, KILN_PROG_ERR_DWELL_RANGE);

    p = valid_program();
    p.segment_count = 0;
    CHECK_EQ_INT(kiln_profile_validate(&p, 1280.0f).code, KILN_PROG_ERR_NO_SEGMENTS);

    p.segment_count = KILN_MAX_SEGMENTS + 1u;
    CHECK_EQ_INT(kiln_profile_validate(&p, 1280.0f).code,
                 KILN_PROG_ERR_TOO_MANY_SEGMENTS);

    p = valid_program();
    p.name[0] = '\0';
    CHECK_EQ_INT(kiln_profile_validate(&p, 1280.0f).code, KILN_PROG_ERR_NAME_EMPTY);

    p = valid_program();
    p.schema_version = 99;
    CHECK_EQ_INT(kiln_profile_validate(&p, 1280.0f).code, KILN_PROG_ERR_SCHEMA);
}

KILN_TEST(a_null_program_is_not_an_empty_one)
{
    /* Reporting "no segments" for a NULL pointer sends whoever reads the message
     * looking at the program instead of at the caller. */
    CHECK_EQ_INT(kiln_profile_validate(NULL, 1280.0f).code, KILN_PROG_ERR_NULL);
    CHECK_STR_EQ(kiln_profile_validation_str(KILN_PROG_ERR_NULL), "no program supplied");
}

KILN_TEST(nfr19_an_unterminated_string_is_rejected)
{
    /* name and description are fixed arrays that arrive from the network and
     * from flash, and the struct is memcpy'd wholesale into kiln_setpoint_t -- so
     * an unterminated one becomes an out-of-bounds read in every later strlen,
     * printf and JSON encode of it. */
    kiln_program_t p = valid_program();
    memset(p.name, 'A', sizeof(p.name));
    CHECK_EQ_INT(kiln_profile_validate(&p, 1280.0f).code,
                 KILN_PROG_ERR_NAME_UNTERMINATED);

    p = valid_program();
    memset(p.description, 'B', sizeof(p.description));
    CHECK_EQ_INT(kiln_profile_validate(&p, 1280.0f).code,
                 KILN_PROG_ERR_NAME_UNTERMINATED);

    /* And a decoder that would rather repair than reject has a way to. */
    kiln_profile_terminate_strings(&p);
    CHECK_EQ_INT(kiln_profile_validate(&p, 1280.0f).code, KILN_PROG_OK);
    CHECK_EQ_UINT(strlen(p.description), KILN_PROGRAM_DESC_LEN - 1u);
}

KILN_TEST(frprg05_rejects_a_program_longer_than_168_hours)
{
    kiln_program_t p = valid_program();
    p.segment_count = 2;
    p.segments[0].dwell_min = KILN_DWELL_MIN_MAX;    /* ~100 h */
    p.segments[1] = p.segments[0];
    p.segments[1].target_c = 700;
    CHECK_EQ_INT(kiln_profile_validate(&p, 1280.0f).code, KILN_PROG_ERR_TOO_LONG);
}

KILN_TEST(frprg06_duration_counts_ramps_and_dwells)
{
    kiln_program_t p;
    kiln_profile_init_empty(&p, "d");
    p.segment_count = 2;
    p.segments[0].target_c = 120;  p.segments[0].rate_c_per_h = 100;
    p.segments[0].dwell_min = 30;
    p.segments[1].target_c = 120;  p.segments[1].rate_c_per_h = 100;
    p.segments[1].dwell_min = 0;

    /* 20 -> 120 degC at 100 degC/h is one hour, plus a half-hour dwell. */
    CHECK_NEAR((double)kiln_profile_duration_s(&p, 20.0f), 5400.0, 2.0);
    CHECK_NEAR((double)kiln_profile_peak_c(&p), 120.0, 0.01);

    CHECK_EQ_UINT(kiln_profile_duration_s(NULL, 20.0f), 0u);
    CHECK_NEAR((double)kiln_profile_peak_c(NULL), 0.0, 0.01);
}

/* --- FR-PRG-09: the built-in examples ---------------------------------- */

KILN_TEST(frprg09_every_built_in_example_passes_validation)
{
    /* The examples were never run through the validator, so an edit could ship a
     * built-in program the controller would refuse to start. */
    const uint8_t n = kiln_profile_example_count();
    CHECK(n > 0);

    for (uint8_t i = 0; i < n; i++) {
        kiln_program_t p;
        CHECK_OK(kiln_profile_example(i, &p));

        const kiln_prog_validation_t v = kiln_profile_validate(&p, 1280.0f);
        CHECK_MSG(v.code == KILN_PROG_OK,
                  "example %u \"%s\" is invalid: %s (segment %u)",
                  i, p.name, kiln_profile_validation_str(v.code), v.segment);

        /* FR-PRG-09: the examples are read-only. */
        CHECK(p.flags & KILN_PROG_FLAG_READONLY);
        CHECK(p.name[0] != '\0');
        CHECK(p.segment_count > 0);
        CHECK(kiln_profile_peak_c(&p) <= 1280.0f);
    }

    kiln_program_t p;
    CHECK_ERR(kiln_profile_example(n, &p), KILN_ERR_NOT_FOUND);
    CHECK_ERR(kiln_profile_example(0, NULL), KILN_ERR_INVALID_ARG);
}

KILN_TEST(frprg09_examples_stay_valid_against_the_lowest_sensible_maximum)
{
    /* A kiln configured for a lower maximum must still be offered examples it
     * can actually run, or the seeded programs are a trap. */
    const uint8_t n = kiln_profile_example_count();
    uint8_t runnable = 0;

    for (uint8_t i = 0; i < n; i++) {
        kiln_program_t p;
        CHECK_OK(kiln_profile_example(i, &p));
        if (kiln_profile_validate(&p, 1000.0f).code == KILN_PROG_OK) runnable++;
    }
    /* Bisque cone 06 peaks at 999 degC and the glass fuse at 804. */
    CHECK(runnable >= 2);
}

KILN_TEST(init_empty_truncates_an_over_long_name)
{
    char longname[128];
    memset(longname, 'x', sizeof(longname));
    longname[sizeof(longname) - 1] = '\0';

    kiln_program_t p;
    kiln_profile_init_empty(&p, longname);
    CHECK_EQ_UINT(strlen(p.name), KILN_PROGRAM_NAME_LEN - 1u);
    CHECK_EQ_UINT(p.schema_version, KILN_PROGRAM_SCHEMA_VERSION);

    kiln_profile_init_empty(&p, NULL);
    CHECK_EQ_UINT(p.name[0], 0u);
}

KILN_TEST(every_validation_code_has_a_message)
{
    for (int c = 0; c <= KILN_PROG_ERR_SCHEMA; c++) {
        const char *s = kiln_profile_validation_str((kiln_prog_valid_t)c);
        CHECK(s != NULL);
        CHECK(s[0] != '\0');
    }
}

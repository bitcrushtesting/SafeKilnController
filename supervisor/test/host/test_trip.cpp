/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The supervisor's trip logic, on the host.  Same harness as the ESP32 side:
 * sharing a test harness does not share product code, and reinventing one
 * would be the only thing worse.
 */
#include <math.h>

#include "kiln_check.h"
#include "sup/trip.h"

namespace {

sup_input_t ok_at(float c)
{
    sup_input_t in = {};
    in.chamber_c     = c;
    in.chamber_valid = true;
    in.fault_bits    = 0u;
    in.lid_open      = false;
    return in;
}

/* Run for a while at one input, 10 Hz, the supervisor's own cycle. */
void run_for(sup_t *s, const sup_input_t *in, float seconds)
{
    for (float t = 0.0f; t < seconds; t += 0.1f) {
        sup_step(s, in, 0.1f);
    }
}

}  // namespace

KILN_TEST(ad22_comes_up_refusing_heat_before_any_conversion)
{
    /* Absence of evidence must not be permission.  This is the defect K1
     * records about the discrete chain, which is closed when the front end is
     * absent; here the supervisor starts closed and has to be convinced. */
    sup_t s;
    sup_init(&s, true);
    CHECK(!s.permit);
    CHECK(!s.tripped);

    sup_input_t in = {};
    in.chamber_valid = false;
    sup_step(&s, &in, 0.1f);
    CHECK(!s.permit);
}

KILN_TEST(ad22_permits_heat_once_a_good_reading_arrives)
{
    sup_t s;
    sup_init(&s, true);
    const sup_input_t in = ok_at(600.0f);
    sup_step(&s, &in, 0.1f);
    CHECK(s.permit);
    CHECK(!s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_NONE);
}

KILN_TEST(sr23_latches_above_the_backstop_without_waiting)
{
    /* A backstop does not have a grace period. */
    sup_t s;
    sup_init(&s, true);
    const sup_input_t hot = ok_at(SUP_OVERTEMP_C + 0.5f);
    sup_step(&s, &hot, 0.1f);
    CHECK(!s.permit);
    CHECK(s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_OVERTEMP);
}

KILN_TEST(sr23_does_not_trip_at_the_ceiling_or_just_below_the_backstop)
{
    /* 1300 is the configurable ceiling and must remain reachable: a backstop
     * that fires at the top of the usable range is a broken product. */
    sup_t s;
    sup_init(&s, true);
    const sup_input_t at_ceiling = ok_at(1300.0f);
    run_for(&s, &at_ceiling, 10.0f);
    CHECK(s.permit);
    CHECK(!s.tripped);

    const sup_input_t just_under = ok_at(SUP_OVERTEMP_C);
    sup_step(&s, &just_under, 0.1f);
    CHECK(s.permit);        /* the comparison is strictly greater-than */
    CHECK(!s.tripped);
}

KILN_TEST(sr23_overtemp_does_not_unlatch_when_it_cools)
{
    sup_t s;
    sup_init(&s, true);
    const sup_input_t hot = ok_at(SUP_OVERTEMP_C + 10.0f);
    sup_step(&s, &hot, 0.1f);
    CHECK(s.tripped);

    const sup_input_t cool = ok_at(200.0f);
    run_for(&s, &cool, 60.0f);
    CHECK(s.tripped);
    CHECK(!s.permit);
    CHECK_EQ_INT(s.reason, SUP_TRIP_OVERTEMP);
}

KILN_TEST(sr04_withholds_heat_at_once_on_a_fault_and_latches_after_the_grace)
{
    sup_t s;
    sup_init(&s, true);
    const sup_input_t good = ok_at(500.0f);
    sup_step(&s, &good, 0.1f);
    CHECK(s.permit);

    sup_input_t faulted = good;
    faulted.fault_bits = 1u;            /* open circuit */
    sup_step(&s, &faulted, 0.1f);
    CHECK(!s.permit);                   /* immediately */
    CHECK(!s.tripped);                  /* but not yet latched */

    run_for(&s, &faulted, SUP_FAULT_GRACE_S + 0.2f);
    CHECK(s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_TC_FAULT);
}

KILN_TEST(sr04_a_transient_fault_shorter_than_the_grace_does_not_latch)
{
    sup_t s;
    sup_init(&s, true);
    const sup_input_t good = ok_at(500.0f);
    sup_step(&s, &good, 0.1f);

    sup_input_t faulted = good;
    faulted.fault_bits = 2u;
    sup_step(&s, &faulted, 0.3f);       /* well under the grace */
    CHECK(!s.permit);
    CHECK(!s.tripped);

    sup_step(&s, &good, 0.1f);          /* recovers */
    CHECK(s.permit);
    CHECK(!s.tripped);
}

KILN_TEST(ad22_an_unusable_reading_latches_as_stale_not_as_a_fault)
{
    /* Different cause, same consequence, reported separately so the other side
     * can say which it was. */
    sup_t s;
    sup_init(&s, true);
    sup_input_t stale = ok_at(500.0f);
    stale.chamber_valid = false;
    run_for(&s, &stale, SUP_FAULT_GRACE_S + 0.2f);
    CHECK(s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_SENSOR_STALE);
}

KILN_TEST(sr31_lid_open_drops_heat_on_the_same_cycle_and_latches_after_confirm)
{
    sup_t s;
    sup_init(&s, true);
    sup_input_t in = ok_at(700.0f);
    sup_step(&s, &in, 0.1f);
    CHECK(s.permit);

    in.lid_open = true;
    sup_step(&s, &in, 0.1f);
    CHECK(!s.permit);                   /* NFR-04: immediately */
    CHECK(!s.tripped);

    run_for(&s, &in, SUP_LID_CONFIRM_S + 0.2f);
    CHECK(s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_LID_OPEN);
}

KILN_TEST(ad22_a_nan_reading_is_stale_and_never_permits)
{
    /* The comparison against the backstop is false for a NaN whichever way it
     * is written, so a non-finite reading must be caught by validity and not
     * by the threshold.  This is the failure the ESP32 side hit in enc_temp. */
    sup_t s;
    sup_init(&s, true);
    sup_input_t nan_in = ok_at(NAN);
    sup_step(&s, &nan_in, 0.1f);
    CHECK(!s.permit);

    /* Declared valid *and* NaN is a caller bug, and must still not permit. */
    CHECK(!(nan_in.chamber_c <= SUP_OVERTEMP_C));
}

KILN_TEST(ad22_a_failed_selftest_never_permits_and_cannot_be_cleared)
{
    sup_t s;
    sup_init(&s, false);
    CHECK(s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_SELF_TEST);

    const sup_input_t good = ok_at(500.0f);
    run_for(&s, &good, 10.0f);
    CHECK(!s.permit);

    sup_clear(&s);                      /* an operator must not be able to */
    CHECK(s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_SELF_TEST);
}

KILN_TEST(ad22_clearing_a_trip_does_not_permit_until_conditions_are_re_established)
{
    sup_t s;
    sup_init(&s, true);
    sup_input_t in = ok_at(SUP_OVERTEMP_C + 5.0f);
    sup_step(&s, &in, 0.1f);
    CHECK(s.tripped);

    sup_clear(&s);
    CHECK(!s.tripped);
    CHECK(!s.permit);                   /* not until a step says so */

    /* Still hot: it latches straight back. */
    sup_step(&s, &in, 0.1f);
    CHECK(s.tripped);
    CHECK(!s.permit);

    sup_clear(&s);
    in = ok_at(400.0f);
    sup_step(&s, &in, 0.1f);
    CHECK(s.permit);
}

KILN_TEST(ad22_flags_describe_what_the_supervisor_is_doing)
{
    sup_t s;
    sup_init(&s, true);
    sup_input_t in = ok_at(500.0f);
    sup_step(&s, &in, 0.1f);
    const uint8_t f = sup_flags(&s, &in);
    CHECK((f & SUP_FLAG_PERMIT) != 0u);
    CHECK((f & SUP_FLAG_TC_VALID) != 0u);
    CHECK((f & SUP_FLAG_SELFTEST_OK) != 0u);
    CHECK((f & SUP_FLAG_TRIPPED) == 0u);
    CHECK((f & SUP_FLAG_LID_OPEN) == 0u);
}

KILN_TEST(nfr17_null_and_negative_time_are_refused_not_faulted)
{
    sup_t s;
    sup_init(&s, true);
    const sup_input_t in = ok_at(500.0f);
    sup_step(nullptr, &in, 0.1f);       /* must not fault */
    sup_step(&s, nullptr, 0.1f);
    sup_clear(nullptr);
    CHECK_EQ_UINT(sup_flags(nullptr, &in), 0u);

    /* A negative or NaN dt must not wind a timer backwards. */
    sup_input_t faulted = in;
    faulted.fault_bits = 1u;
    sup_step(&s, &faulted, -5.0f);
    CHECK_NEAR(s.fault_s, 0.0f, 1e-6);
    sup_step(&s, &faulted, NAN);
    CHECK_NEAR(s.fault_s, 0.0f, 1e-6);
}

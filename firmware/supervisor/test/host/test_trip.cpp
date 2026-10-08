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

KILN_TEST(swa22_comes_up_refusing_heat_before_any_conversion)
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

KILN_TEST(swa22_permits_heat_once_a_good_reading_arrives)
{
    sup_t s;
    sup_init(&s, true);
    const sup_input_t in = ok_at(600.0f);
    sup_step(&s, &in, 0.1f);
    CHECK(s.permit);
    CHECK(!s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_NONE);
}

KILN_TEST(swrsaf23_latches_above_the_backstop_without_waiting)
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

KILN_TEST(swrsaf23_does_not_trip_at_the_ceiling_or_just_below_the_backstop)
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

KILN_TEST(swrsaf23_overtemp_does_not_unlatch_when_it_cools)
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

KILN_TEST(swrsaf04_withholds_heat_at_once_on_a_fault_and_latches_after_the_grace)
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

KILN_TEST(swrsaf04_a_transient_fault_shorter_than_the_grace_does_not_latch)
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

KILN_TEST(swa22_an_unusable_reading_latches_as_stale_not_as_a_fault)
{
    /* Different cause, same consequence, reported separately so the other side
     * can say which it was.  A good reading first, so this is staleness and
     * not a front end that never started. */
    sup_t s;
    sup_init(&s, true);
    const sup_input_t good = ok_at(500.0f);
    sup_step(&s, &good, 0.1f);

    sup_input_t stale = good;
    stale.chamber_valid = false;
    run_for(&s, &stale, SUP_FAULT_GRACE_S + 0.2f);
    CHECK(s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_SENSOR_STALE);
}

KILN_TEST(swa22_a_nan_reading_is_stale_and_never_permits)
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

KILN_TEST(swa22_a_failed_selftest_never_permits_and_cannot_be_cleared)
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

KILN_TEST(swa22_clearing_a_trip_does_not_permit_until_conditions_are_re_established)
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

KILN_TEST(swa22_flags_describe_what_the_supervisor_is_doing)
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
}

KILN_TEST(swrnfr17_null_and_negative_time_are_refused_not_faulted)
{
    sup_t s;
    sup_init(&s, true);
    const sup_input_t in = ok_at(500.0f);
    sup_step(nullptr, &in, 0.1f);       /* must not fault */
    sup_step(&s, nullptr, 0.1f);
    sup_clear(nullptr);
    /* Both halves of each guard, independently.  A guard where only the first
     * condition has ever been exercised is a guard half tested, which is what
     * MC/DC measures and line coverage cannot see. */
    CHECK_EQ_UINT(sup_flags(nullptr, &in), 0u);
    CHECK_EQ_UINT(sup_flags(&s, nullptr), 0u);

    /* A negative or NaN dt must not wind a timer backwards. */
    sup_input_t faulted = in;
    faulted.fault_bits = 1u;
    sup_step(&s, &faulted, -5.0f);
    CHECK_NEAR(s.fault_s, 0.0f, 1e-6);
    sup_step(&s, &faulted, NAN);
    CHECK_NEAR(s.fault_s, 0.0f, 1e-6);
}

/* --- the clear button (R4) ---------------------------------------------- */

KILN_TEST(r4_a_held_button_clears_the_latch_once_the_hold_elapses)
{
    sup_t s;
    sup_init(&s, true);
    sup_input_t in = ok_at(SUP_OVERTEMP_C + 5.0f);
    sup_step(&s, &in, 0.1f);
    CHECK(s.tripped);

    /* Released first, so the button arms, then cooled, then held. */
    in = ok_at(300.0f);
    sup_step(&s, &in, 0.1f);
    CHECK(s.tripped);

    in.clear_pressed = true;
    sup_step(&s, &in, 0.1f);
    CHECK(s.tripped);                   /* not yet: it must be held */

    run_for(&s, &in, SUP_CLEAR_HOLD_S + 0.2f);
    CHECK(!s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_NONE);
}

KILN_TEST(r4_a_tap_shorter_than_the_hold_does_not_clear)
{
    sup_t s;
    sup_init(&s, true);
    sup_input_t in = ok_at(SUP_OVERTEMP_C + 5.0f);
    sup_step(&s, &in, 0.1f);
    in = ok_at(300.0f);
    sup_step(&s, &in, 0.1f);            /* arm */

    in.clear_pressed = true;
    sup_step(&s, &in, 0.2f);            /* well short of the hold */
    in.clear_pressed = false;
    sup_step(&s, &in, 0.1f);
    CHECK(s.tripped);
}

KILN_TEST(r4_a_line_stuck_low_never_clears_the_latch)
{
    /* The case the edge triggering exists for.  A button shorted to ground, or
     * one wedged down, must not turn the latch into a no-op: at power-on it is
     * never seen released, so it never arms. */
    sup_t s;
    sup_init(&s, true);
    sup_input_t in = ok_at(SUP_OVERTEMP_C + 5.0f);
    in.clear_pressed = true;            /* shorted from the very first cycle */
    sup_step(&s, &in, 0.1f);
    CHECK(s.tripped);

    /* Hold it low for a minute at a safe temperature. */
    sup_input_t cool = ok_at(300.0f);
    cool.clear_pressed = true;
    run_for(&s, &cool, 60.0f);
    CHECK(s.tripped);                   /* still latched */
    CHECK(!s.permit);
}

KILN_TEST(r4_clearing_disarms_until_the_button_is_released_again)
{
    /* One press, one clear.  Otherwise a held button would clear each time the
     * supervisor re-latched, which is the stuck-line failure in slow motion. */
    sup_t s;
    sup_init(&s, true);
    sup_input_t cool = ok_at(300.0f);
    sup_step(&s, &cool, 0.1f);          /* arm: released and safe */

    sup_input_t hot = ok_at(SUP_OVERTEMP_C + 5.0f);
    sup_step(&s, &hot, 0.1f);
    CHECK(s.tripped);

    /* Hold the button down and stay hot: it clears once, re-latches, and must
     * not clear again while still held. */
    hot.clear_pressed = true;
    run_for(&s, &hot, SUP_CLEAR_HOLD_S + 0.2f);
    CHECK(s.tripped);                   /* re-latched on the same input */

    run_for(&s, &hot, 10.0f);           /* still held, still hot */
    CHECK(s.tripped);

    /* Release, cool, press again: now it clears. */
    sup_input_t cool2 = ok_at(300.0f);
    sup_step(&s, &cool2, 0.1f);
    cool2.clear_pressed = true;
    run_for(&s, &cool2, SUP_CLEAR_HOLD_S + 0.2f);
    CHECK(!s.tripped);
}

KILN_TEST(r4_the_button_cannot_clear_a_failed_selftest)
{
    sup_t s;
    sup_init(&s, false);
    sup_input_t in = ok_at(300.0f);
    sup_step(&s, &in, 0.1f);            /* arm */
    in.clear_pressed = true;
    run_for(&s, &in, 5.0f);
    CHECK(s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_SELF_TEST);
    CHECK(!s.permit);
}

KILN_TEST(r4_the_button_cannot_clear_a_condition_that_still_holds)
{
    /* Clearing while the kiln is still too hot re-latches on the same cycle,
     * so the button cannot be held down to keep firing. */
    sup_t s;
    sup_init(&s, true);
    sup_input_t cool = ok_at(700.0f);
    sup_step(&s, &cool, 0.1f);          /* arm, permitted */

    sup_input_t hot = ok_at(SUP_OVERTEMP_C + 5.0f);
    sup_step(&s, &hot, 0.1f);
    CHECK(s.tripped);

    hot.clear_pressed = true;
    run_for(&s, &hot, SUP_CLEAR_HOLD_S + 0.5f);
    CHECK(!s.permit);
    CHECK(s.tripped);
}

/* --- the lid is not the supervisor's concern ---------------------------- */

KILN_TEST(syshw21_the_supervisor_has_no_lid_input_so_loading_cold_cannot_trip_it)
{
    /* The lid breaks the coil in hardware and SWR-SAF-31's latch is the ESP32's,
     * which knows whether a firing is running.  A supervisor that latched on
     * lid open regardless would trip on every cold load, which is the nuisance
     * trip HZ-10 warns about.  There is nothing here to latch on, and this
     * test exists so that stays true. */
    sup_t s;
    sup_init(&s, true);
    const sup_input_t in = ok_at(300.0f);
    run_for(&s, &in, 300.0f);           /* five minutes of loading the kiln */
    CHECK(s.permit);
    CHECK(!s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_NONE);
}

/* --- bringing the front end up ----------------------------------------- */

KILN_TEST(swa22_a_front_end_still_starting_up_withholds_heat_without_latching)
{
    /* Boot: no conversion has arrived yet.  The supervisor must not permit,
     * and must not latch either, or a front end whose first conversion takes
     * longer than the grace would demand a button press at every power-on
     * with nothing actually wrong. */
    sup_t s;
    sup_init(&s, true);
    sup_input_t starting = ok_at(0.0f);
    starting.chamber_valid = false;

    run_for(&s, &starting, 30.0f);      /* far beyond the grace */
    CHECK(!s.permit);
    CHECK(!s.tripped);
    CHECK(!s.seen_valid);

    /* Then it comes up, and heat is permitted with no acknowledgement. */
    const sup_input_t good = ok_at(400.0f);
    sup_step(&s, &good, 0.1f);
    CHECK(s.permit);
    CHECK(!s.tripped);
    CHECK(s.seen_valid);
}

KILN_TEST(swa22_a_reading_that_was_working_and_stopped_does_latch)
{
    /* The other half: once a reading has been seen, losing it is a fault to
     * acknowledge rather than a slow start. */
    sup_t s;
    sup_init(&s, true);
    const sup_input_t good = ok_at(400.0f);
    sup_step(&s, &good, 0.1f);
    CHECK(s.seen_valid);

    sup_input_t lost = good;
    lost.chamber_valid = false;
    sup_step(&s, &lost, 0.1f);
    CHECK(!s.permit);                   /* immediately */
    CHECK(!s.tripped);

    run_for(&s, &lost, SUP_FAULT_GRACE_S + 0.2f);
    CHECK(s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_SENSOR_STALE);
}

KILN_TEST(swa22_a_reported_fault_latches_even_before_a_first_reading)
{
    /* A front end actively reporting a fault is not "still starting up": it is
     * telling us something is wrong, and that latches whether or not a good
     * conversion ever arrived. */
    sup_t s;
    sup_init(&s, true);
    sup_input_t faulted = ok_at(0.0f);
    faulted.chamber_valid = false;
    faulted.fault_bits    = 1u;         /* open circuit */

    run_for(&s, &faulted, SUP_FAULT_GRACE_S + 0.2f);
    CHECK(s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_TC_FAULT);
}

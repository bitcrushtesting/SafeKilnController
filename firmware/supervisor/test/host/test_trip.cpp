/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The supervisor's trip logic, on the host.  Same harness as the ESP32 side:
 * sharing a test harness does not share product code, and reinventing one
 * would be the only thing worse.
 */
#include "kiln_check.h"
#include "sup/trip.h"

namespace {

/* One cycle of the supervisor's own 10 Hz loop, in the unit sup_step takes.
 * The suites step in whole cycles so that a stated duration is an exact number
 * of them: the float version advanced by 0.1f and accumulated the error, which
 * put a "run for exactly the grace period" case either side of the boundary
 * depending on how many cycles it took to get there. */
constexpr uint32_t STEP_MS = 100u;

/* Readings are q7, 1/128 degC.  Tests say what they mean in degrees through
 * sup_c_to_q7 and, where a boundary is the point, in LSBs either side of a
 * threshold, which is a sharper test than a fraction of a degree: it is the
 * smallest difference the front end can actually report. */
sup_input_t ok_at(int32_t q7)
{
    sup_input_t in = {};
    in.chamber_q7    = q7;
    in.chamber_valid = true;
    in.fault_bits    = 0u;
    in.diag_ok       = true;
    return in;
}

/* Run for a while at one input, 10 Hz, the supervisor's own cycle. */
void run_for(sup_t *s, const sup_input_t *in, uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += STEP_MS) {
        sup_step(s, in, STEP_MS);
    }
}

}  // namespace

/*
 * @relation(SWA-22, scope=function)
 */
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
    in.diag_ok       = true;      /* isolate what this test is about */
    sup_step(&s, &in, STEP_MS);
    CHECK(!s.permit);
}

/*
 * @relation(SWA-22, scope=function)
 */
KILN_TEST(swa22_permits_heat_once_a_good_reading_arrives)
{
    sup_t s;
    sup_init(&s, true);
    const sup_input_t in = ok_at(sup_c_to_q7(600));
    sup_step(&s, &in, STEP_MS);
    CHECK(s.permit);
    CHECK(!s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_NONE);
}

/*
 * @relation(SWR-SAF-23, scope=function)
 */
KILN_TEST(swrsaf23_latches_above_the_backstop_without_waiting)
{
    /* A backstop does not have a grace period. */
    sup_t s;
    sup_init(&s, true);
    const sup_input_t hot = ok_at(SUP_OVERTEMP + 1);
    sup_step(&s, &hot, STEP_MS);
    CHECK(!s.permit);
    CHECK(s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_OVERTEMP);
}

/*
 * @relation(SWR-SAF-23, scope=function)
 */
KILN_TEST(swrsaf23_does_not_trip_at_the_ceiling_or_just_below_the_backstop)
{
    /* 1300 is the configurable ceiling and must remain reachable: a backstop
     * that fires at the top of the usable range is a broken product. */
    sup_t s;
    sup_init(&s, true);
    const sup_input_t at_ceiling = ok_at(sup_c_to_q7(1300));
    run_for(&s, &at_ceiling, 10000u);
    CHECK(s.permit);
    CHECK(!s.tripped);

    const sup_input_t just_under = ok_at(SUP_OVERTEMP);
    sup_step(&s, &just_under, STEP_MS);
    CHECK(s.permit);        /* the comparison is strictly greater-than */
    CHECK(!s.tripped);
}

/*
 * @relation(SWR-SAF-23, scope=function)
 */
KILN_TEST(swrsaf23_overtemp_does_not_unlatch_when_it_cools)
{
    sup_t s;
    sup_init(&s, true);
    const sup_input_t hot = ok_at(SUP_OVERTEMP + sup_c_to_q7(10));
    sup_step(&s, &hot, STEP_MS);
    CHECK(s.tripped);

    const sup_input_t cool = ok_at(sup_c_to_q7(200));
    run_for(&s, &cool, 60000u);
    CHECK(s.tripped);
    CHECK(!s.permit);
    CHECK_EQ_INT(s.reason, SUP_TRIP_OVERTEMP);
}

/*
 * @relation(SWR-SAF-04, scope=function)
 */
KILN_TEST(swrsaf04_withholds_heat_at_once_on_a_fault_and_latches_after_the_grace)
{
    sup_t s;
    sup_init(&s, true);
    const sup_input_t good = ok_at(sup_c_to_q7(500));
    sup_step(&s, &good, STEP_MS);
    CHECK(s.permit);

    sup_input_t faulted = good;
    faulted.fault_bits = 1u;            /* open circuit */
    sup_step(&s, &faulted, STEP_MS);
    CHECK(!s.permit);                   /* immediately */
    CHECK(!s.tripped);                  /* but not yet latched */

    run_for(&s, &faulted, SUP_FAULT_GRACE_MS + 200u);
    CHECK(s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_TC_FAULT);
}

/*
 * @relation(SWR-SAF-04, scope=function)
 */
KILN_TEST(swrsaf04_a_transient_fault_shorter_than_the_grace_does_not_latch)
{
    sup_t s;
    sup_init(&s, true);
    const sup_input_t good = ok_at(sup_c_to_q7(500));
    sup_step(&s, &good, STEP_MS);

    sup_input_t faulted = good;
    faulted.fault_bits = 2u;
    sup_step(&s, &faulted, 300u);       /* well under the grace */
    CHECK(!s.permit);
    CHECK(!s.tripped);

    sup_step(&s, &good, STEP_MS);          /* recovers */
    CHECK(s.permit);
    CHECK(!s.tripped);
}

/*
 * @relation(SWA-22, scope=function)
 */
KILN_TEST(swa22_an_unusable_reading_latches_as_stale_not_as_a_fault)
{
    /* Different cause, same consequence, reported separately so the other side
     * can say which it was.  A good reading first, so this is staleness and
     * not a front end that never started. */
    sup_t s;
    sup_init(&s, true);
    const sup_input_t good = ok_at(sup_c_to_q7(500));
    sup_step(&s, &good, STEP_MS);

    sup_input_t stale = good;
    stale.chamber_valid = false;
    run_for(&s, &stale, SUP_FAULT_GRACE_MS + 200u);
    CHECK(s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_SENSOR_STALE);
}

/*
 * @relation(SWA-22, scope=function)
 */
KILN_TEST(swa22_every_representable_reading_yields_a_decision)
{
    /* This replaces a test for a NaN reading, and the replacement is the
     * point: with the readings int32_t there is no NaN to defend against, so
     * what is checked now is that the decision is TOTAL.  Every value the type
     * can hold has to produce a defined permit and no undefined arithmetic,
     * including values no thermocouple path can generate, because the struct is
     * public and the next thing to fill it may not be the decode.
     *
     * INT32_MIN and INT32_MAX together are the pair that catches the two ways
     * this could have been written wrongly: a signed subtraction in the
     * disagreement check overflows for them, and negating INT32_MIN to take a
     * magnitude is undefined on its own.  Neither is reachable through
     * sup_tc_decode's 19 bits, which is exactly why it is worth pinning here. */
    sup_t s;
    sup_init(&s, true);

    sup_input_t extreme = ok_at(INT32_MAX);
    extreme.chamber2_q7    = INT32_MIN;
    extreme.chamber2_valid = true;
    sup_step(&s, &extreme, STEP_MS);
    /* Above the backstop on the higher of the two, so it trips at once, and
     * the pair disagrees by more than any band, so heat is withheld twice
     * over. */
    CHECK(!s.permit);
    CHECK(s.tripped);
    CHECK_EQ_UINT((unsigned)s.reason, (unsigned)SUP_TRIP_OVERTEMP);

    /* And the other way round, which exercises the same magnitude with the
     * operands swapped. */
    sup_t s2;
    sup_init(&s2, true);
    sup_input_t extreme2 = ok_at(INT32_MIN);
    extreme2.chamber2_q7    = INT32_MAX;
    extreme2.chamber2_valid = true;
    sup_step(&s2, &extreme2, STEP_MS);
    CHECK(!s2.permit);
}

/*
 * @relation(SWA-22, scope=function)
 */
KILN_TEST(swa22_a_failed_selftest_never_permits_and_cannot_be_cleared)
{
    sup_t s;
    sup_init(&s, false);
    CHECK(s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_SELF_TEST);

    const sup_input_t good = ok_at(sup_c_to_q7(500));
    run_for(&s, &good, 10000u);
    CHECK(!s.permit);

    sup_clear(&s);                      /* an operator must not be able to */
    CHECK(s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_SELF_TEST);
}

/*
 * @relation(SWA-22, scope=function)
 */
KILN_TEST(swa22_clearing_a_trip_does_not_permit_until_conditions_are_re_established)
{
    sup_t s;
    sup_init(&s, true);
    sup_input_t in = ok_at(SUP_OVERTEMP + sup_c_to_q7(5));
    sup_step(&s, &in, STEP_MS);
    CHECK(s.tripped);

    sup_clear(&s);
    CHECK(!s.tripped);
    CHECK(!s.permit);                   /* not until a step says so */

    /* Still hot: it latches straight back. */
    sup_step(&s, &in, STEP_MS);
    CHECK(s.tripped);
    CHECK(!s.permit);

    sup_clear(&s);
    in = ok_at(sup_c_to_q7(400));
    sup_step(&s, &in, STEP_MS);
    CHECK(s.permit);
}

/*
 * @relation(SWA-22, scope=function)
 */
KILN_TEST(swa22_flags_describe_what_the_supervisor_is_doing)
{
    sup_t s;
    sup_init(&s, true);
    const sup_input_t in = ok_at(sup_c_to_q7(500));
    sup_step(&s, &in, STEP_MS);
    const uint8_t f = sup_flags(&s, &in);
    CHECK((f & SUP_FLAG_PERMIT) != 0u);
    CHECK((f & SUP_FLAG_TC_VALID) != 0u);
    CHECK((f & SUP_FLAG_SELFTEST_OK) != 0u);
    CHECK((f & SUP_FLAG_TRIPPED) == 0u);
}

/*
 * @relation(SWR-NFR-17, scope=function)
 */
KILN_TEST(swrnfr17_null_arguments_and_an_extreme_interval_are_refused_not_faulted)
{
    sup_t s;
    sup_init(&s, true);
    const sup_input_t in = ok_at(sup_c_to_q7(500));
    sup_step(nullptr, &in, STEP_MS);       /* must not fault */
    sup_step(&s, nullptr, STEP_MS);
    sup_clear(nullptr);
    /* Both halves of each guard, independently.  A guard where only the first
     * condition has ever been exercised is a guard half tested, which is what
     * MC/DC measures and line coverage cannot see. */
    CHECK_EQ_UINT(sup_flags(nullptr, &in), 0u);
    CHECK_EQ_UINT(sup_flags(&s, nullptr), 0u);

    /* The float version checked here that a negative or a NaN interval could
     * not wind a timer backwards.  A uint32_t can be neither, so what is left
     * to check is the one pathological interval that still exists: an enormous
     * one, which must saturate the timer rather than wrap it back below the
     * threshold and un-arm a condition that is still present. */
    sup_input_t faulted = in;
    faulted.fault_bits = 1u;
    sup_step(&s, &faulted, UINT32_MAX);
    CHECK_EQ_UINT(s.fault_ms, UINT32_MAX);
    sup_step(&s, &faulted, UINT32_MAX);
    CHECK_EQ_UINT(s.fault_ms, UINT32_MAX);      /* saturated, not wrapped */
    CHECK(s.tripped);
}

/* --- the clear button (R4) ---------------------------------------------- */

/*
 * @relation(SWR-SAF-32, scope=function)
 */
KILN_TEST(swrsaf32_a_held_button_clears_the_latch_once_the_hold_elapses)
{
    sup_t s;
    sup_init(&s, true);
    sup_input_t in = ok_at(SUP_OVERTEMP + sup_c_to_q7(5));
    sup_step(&s, &in, STEP_MS);
    CHECK(s.tripped);

    /* Released first, so the button arms, then cooled, then held. */
    in = ok_at(sup_c_to_q7(300));
    sup_step(&s, &in, STEP_MS);
    CHECK(s.tripped);

    in.clear_pressed = true;
    sup_step(&s, &in, STEP_MS);
    CHECK(s.tripped);                   /* not yet: it must be held */

    run_for(&s, &in, SUP_CLEAR_HOLD_MS + 200u);
    CHECK(!s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_NONE);
}

/*
 * @relation(SWR-SAF-32, scope=function)
 */
KILN_TEST(swrsaf32_a_tap_shorter_than_the_hold_does_not_clear)
{
    sup_t s;
    sup_init(&s, true);
    sup_input_t in = ok_at(SUP_OVERTEMP + sup_c_to_q7(5));
    sup_step(&s, &in, STEP_MS);
    in = ok_at(sup_c_to_q7(300));
    sup_step(&s, &in, STEP_MS);            /* arm */

    in.clear_pressed = true;
    sup_step(&s, &in, 200u);            /* well short of the hold */
    in.clear_pressed = false;
    sup_step(&s, &in, STEP_MS);
    CHECK(s.tripped);
}

/*
 * @relation(SWR-SAF-32, scope=function)
 */
KILN_TEST(swrsaf32_a_line_stuck_low_never_clears_the_latch)
{
    /* The case the edge triggering exists for.  A button shorted to ground, or
     * one wedged down, must not turn the latch into a no-op: at power-on it is
     * never seen released, so it never arms. */
    sup_t s;
    sup_init(&s, true);
    sup_input_t in = ok_at(SUP_OVERTEMP + sup_c_to_q7(5));
    in.clear_pressed = true;            /* shorted from the very first cycle */
    sup_step(&s, &in, STEP_MS);
    CHECK(s.tripped);

    /* Hold it low for a minute at a safe temperature. */
    sup_input_t cool = ok_at(sup_c_to_q7(300));
    cool.clear_pressed = true;
    run_for(&s, &cool, 60000u);
    CHECK(s.tripped);                   /* still latched */
    CHECK(!s.permit);
}

/*
 * @relation(SWR-SAF-32, scope=function)
 */
KILN_TEST(swrsaf32_clearing_disarms_until_the_button_is_released_again)
{
    /* One press, one clear.  Otherwise a held button would clear each time the
     * supervisor re-latched, which is the stuck-line failure in slow motion. */
    sup_t s;
    sup_init(&s, true);
    const sup_input_t cool = ok_at(sup_c_to_q7(300));
    sup_step(&s, &cool, STEP_MS);          /* arm: released and safe */

    sup_input_t hot = ok_at(SUP_OVERTEMP + sup_c_to_q7(5));
    sup_step(&s, &hot, STEP_MS);
    CHECK(s.tripped);

    /* Hold the button down and stay hot: it clears once, re-latches, and must
     * not clear again while still held. */
    hot.clear_pressed = true;
    run_for(&s, &hot, SUP_CLEAR_HOLD_MS + 200u);
    CHECK(s.tripped);                   /* re-latched on the same input */

    run_for(&s, &hot, 10000u);           /* still held, still hot */
    CHECK(s.tripped);

    /* Release, cool, press again: now it clears. */
    sup_input_t cool2 = ok_at(sup_c_to_q7(300));
    sup_step(&s, &cool2, STEP_MS);
    cool2.clear_pressed = true;
    run_for(&s, &cool2, SUP_CLEAR_HOLD_MS + 200u);
    CHECK(!s.tripped);
}

/*
 * @relation(SWR-SAF-32, scope=function)
 */
KILN_TEST(swrsaf32_the_button_cannot_clear_a_failed_selftest)
{
    sup_t s;
    sup_init(&s, false);
    sup_input_t in = ok_at(sup_c_to_q7(300));
    sup_step(&s, &in, STEP_MS);            /* arm */
    in.clear_pressed = true;
    run_for(&s, &in, 5000u);
    CHECK(s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_SELF_TEST);
    CHECK(!s.permit);
}

/*
 * @relation(SWR-SAF-32, scope=function)
 */
KILN_TEST(swrsaf32_the_button_cannot_clear_a_condition_that_still_holds)
{
    /* Clearing while the kiln is still too hot re-latches on the same cycle,
     * so the button cannot be held down to keep firing. */
    sup_t s;
    sup_init(&s, true);
    const sup_input_t cool = ok_at(sup_c_to_q7(700));
    sup_step(&s, &cool, STEP_MS);          /* arm, permitted */

    sup_input_t hot = ok_at(SUP_OVERTEMP + sup_c_to_q7(5));
    sup_step(&s, &hot, STEP_MS);
    CHECK(s.tripped);

    hot.clear_pressed = true;
    run_for(&s, &hot, SUP_CLEAR_HOLD_MS + 500u);
    CHECK(!s.permit);
    CHECK(s.tripped);
}

/* --- the lid is not the supervisor's concern ---------------------------- */

/*
 * @relation(SYS-HW-21, scope=function)
 */
KILN_TEST(syshw21_the_supervisor_has_no_lid_input_so_loading_cold_cannot_trip_it)
{
    /* The lid breaks the coil in hardware and SWR-SAF-31's latch is the ESP32's,
     * which knows whether a firing is running.  A supervisor that latched on
     * lid open regardless would trip on every cold load, which is the nuisance
     * trip HZ-10 warns about.  There is nothing here to latch on, and this
     * test exists so that stays true. */
    sup_t s;
    sup_init(&s, true);
    const sup_input_t in = ok_at(sup_c_to_q7(300));
    run_for(&s, &in, 300000u);           /* five minutes of loading the kiln */
    CHECK(s.permit);
    CHECK(!s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_NONE);
}

/* --- bringing the front end up ----------------------------------------- */

/*
 * @relation(SWA-22, scope=function)
 */
KILN_TEST(swa22_a_front_end_still_starting_up_withholds_heat_without_latching)
{
    /* Boot: no conversion has arrived yet.  The supervisor must not permit,
     * and must not latch either, or a front end whose first conversion takes
     * longer than the grace would demand a button press at every power-on
     * with nothing actually wrong. */
    sup_t s;
    sup_init(&s, true);
    sup_input_t starting = ok_at(sup_c_to_q7(0));
    starting.chamber_valid = false;

    run_for(&s, &starting, 30000u);      /* far beyond the grace */
    CHECK(!s.permit);
    CHECK(!s.tripped);
    CHECK(!s.seen_valid);

    /* Then it comes up, and heat is permitted with no acknowledgement. */
    const sup_input_t good = ok_at(sup_c_to_q7(400));
    sup_step(&s, &good, STEP_MS);
    CHECK(s.permit);
    CHECK(!s.tripped);
    CHECK(s.seen_valid);
}

/*
 * @relation(SWA-22, scope=function)
 */
KILN_TEST(swa22_a_reading_that_was_working_and_stopped_does_latch)
{
    /* The other half: once a reading has been seen, losing it is a fault to
     * acknowledge rather than a slow start. */
    sup_t s;
    sup_init(&s, true);
    const sup_input_t good = ok_at(sup_c_to_q7(400));
    sup_step(&s, &good, STEP_MS);
    CHECK(s.seen_valid);

    sup_input_t lost = good;
    lost.chamber_valid = false;
    sup_step(&s, &lost, STEP_MS);
    CHECK(!s.permit);                   /* immediately */
    CHECK(!s.tripped);

    run_for(&s, &lost, SUP_FAULT_GRACE_MS + 200u);
    CHECK(s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_SENSOR_STALE);
}

/*
 * @relation(SWA-22, scope=function)
 */
KILN_TEST(swa22_a_reported_fault_latches_even_before_a_first_reading)
{
    /* A front end actively reporting a fault is not "still starting up": it is
     * telling us something is wrong, and that latches whether or not a good
     * conversion ever arrived. */
    sup_t s;
    sup_init(&s, true);
    sup_input_t faulted = ok_at(sup_c_to_q7(0));
    faulted.chamber_valid = false;
    faulted.fault_bits    = 1u;         /* open circuit */

    run_for(&s, &faulted, SUP_FAULT_GRACE_MS + 200u);
    CHECK(s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_TC_FAULT);
}

/*
 * @relation(SWR-SAF-36, scope=function)
 */
KILN_TEST(swrsaf36_a_failed_diagnostic_withholds_heat_immediately)
{
    sup_t s;
    sup_init(&s, true);

    /* Establish a healthy cycle first, so the test shows the diagnostic taking
     * permission away rather than never having granted it. */
    const sup_input_t good = ok_at(sup_c_to_q7(500));
    sup_step(&s, &good, STEP_MS);
    CHECK(s.permit);

    sup_input_t bad = good;
    bad.diag_ok = false;
    sup_step(&s, &bad, STEP_MS);
    CHECK(!s.permit);
    CHECK(s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_SELF_TEST);
}

/*
 * @relation(SWR-SAF-36, scope=function)
 */
KILN_TEST(swrsaf36_a_failed_diagnostic_is_not_clearable_by_the_button)
{
    /* The distinction from every other trip. An operator can acknowledge an
     * open thermocouple because they can see it and deal with it. A supervisor
     * whose RAM or program sequence has failed cannot be trusted to evaluate
     * the condition they would be acknowledging. */
    sup_t s;
    sup_init(&s, true);

    sup_input_t bad = ok_at(sup_c_to_q7(500));
    bad.diag_ok = false;
    sup_step(&s, &bad, STEP_MS);
    CHECK(s.tripped);

    /* Release, then hold the button well past the hold time, on a now-healthy
     * reading. It must not clear. */
    sup_input_t good = ok_at(sup_c_to_q7(500));
    sup_step(&s, &good, STEP_MS);
    good.clear_pressed = true;
    run_for(&s, &good, SUP_CLEAR_HOLD_MS * 4u);

    CHECK(s.tripped);
    CHECK(!s.permit);
    CHECK_EQ_INT(s.reason, SUP_TRIP_SELF_TEST);

    /* And directly, which is the path a test may take that an operator cannot. */
    sup_clear(&s);
    CHECK(s.tripped);
}

/*
 * @relation(SWR-SAF-36, scope=function)
 */
KILN_TEST(swrsaf36_a_diagnostic_failure_does_not_unlatch_when_it_passes_again)
{
    /* An intermittent RAM fault that reads correctly on the next pass is still
     * a RAM fault. Nothing about a later healthy cycle makes the earlier one
     * trustworthy. */
    sup_t s;
    sup_init(&s, true);

    sup_input_t bad = ok_at(sup_c_to_q7(400));
    bad.diag_ok = false;
    sup_step(&s, &bad, STEP_MS);
    CHECK(s.tripped);

    const sup_input_t good = ok_at(sup_c_to_q7(400));
    run_for(&s, &good, 10000u);
    CHECK(s.tripped);
    CHECK(!s.permit);
    CHECK_EQ_INT(s.reason, SUP_TRIP_SELF_TEST);
}

/*
 * @relation(SWR-SAF-36, scope=function)
 */
KILN_TEST(swrsaf36_a_zero_initialised_input_withholds_heat)
{
    /* The reason diag_ok is positive logic. A caller that forgets the field
     * gets the safe answer, which is the same convention chamber_valid uses. */
    sup_t s;
    sup_init(&s, true);

    sup_input_t forgotten = {};
    forgotten.chamber_q7    = sup_c_to_q7(500);
    forgotten.chamber_valid = true;
    /* diag_ok deliberately not set */
    sup_step(&s, &forgotten, STEP_MS);
    CHECK(!s.permit);
    CHECK_EQ_INT(s.reason, SUP_TRIP_SELF_TEST);
}

/*
 * @relation(SWR-SAF-36, scope=function)
 */
KILN_TEST(swrsaf36_a_diagnostic_failure_outranks_a_thermocouple_fault)
{
    /* Both wrong at once. The diagnostic is reported, because it is the more
     * serious of the two and because it may well be what produced the other. */
    sup_t s;
    sup_init(&s, true);

    sup_input_t both = ok_at(sup_c_to_q7(500));
    both.diag_ok    = false;
    both.fault_bits = 1u;
    run_for(&s, &both, SUP_FAULT_GRACE_MS * 3u);

    CHECK(s.tripped);
    CHECK(!s.permit);
    CHECK_EQ_INT(s.reason, SUP_TRIP_SELF_TEST);
}

/* --- SWR-SAF-37: the second chamber couple ------------------------------ */

namespace {

/* Both couples valid and agreeing, at `q7`. */
sup_input_t pair_at(int32_t q7)
{
    sup_input_t in = ok_at(q7);
    in.chamber2_q7    = q7;
    in.chamber2_valid = true;
    return in;
}

}  // namespace

/*
 * @relation(SWR-SAF-37, scope=function)
 */
KILN_TEST(swrsaf37_a_low_reading_couple_cannot_mask_a_hot_kiln)
{
    /* The whole reason the second sensor is worth a second SPI bus. The backstop
     * acts on the HIGHER of the two, so a couple reading cold cannot hide a
     * chamber that is not. An average would let it: a couple reading 200 degC low
     * would pull the pair 100 degC low, which is the failure wearing redundancy
     * as a disguise. */
    sup_t s;
    sup_init(&s, true);

    sup_input_t in = pair_at(sup_c_to_q7(200));
    in.chamber_q7  = SUP_OVERTEMP + sup_c_to_q7(20); /* couple 1 sees the truth */
    in.chamber2_q7 = sup_c_to_q7(200);              /* couple 2 reads far low  */
    sup_step(&s, &in, STEP_MS);

    CHECK(!s.permit);
    CHECK(s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_OVERTEMP);

    /* and the other way round, because neither couple is privileged */
    sup_t s2;
    sup_init(&s2, true);
    sup_input_t flipped = pair_at(sup_c_to_q7(200));
    flipped.chamber_q7  = sup_c_to_q7(200);
    flipped.chamber2_q7 = SUP_OVERTEMP + sup_c_to_q7(20);
    sup_step(&s2, &flipped, STEP_MS);
    CHECK(!s2.permit);
    CHECK_EQ_INT(s2.reason, SUP_TRIP_OVERTEMP);
}

/*
 * @relation(SWR-SAF-37, scope=function)
 */
KILN_TEST(swrsaf37_agreeing_couples_permit_heat_and_set_both_flags)
{
    sup_t s;
    sup_init(&s, true);
    const sup_input_t in = pair_at(sup_c_to_q7(600));
    sup_step(&s, &in, STEP_MS);
    CHECK(s.permit);
    CHECK(!s.tripped);

    const uint8_t f = sup_flags(&s, &in);
    CHECK((f & SUP_FLAG_TC_VALID) != 0u);
    CHECK((f & SUP_FLAG_TC2_VALID) != 0u);
}

/*
 * @relation(SWR-SAF-37, scope=function)
 */
KILN_TEST(swrsaf37_disagreement_withholds_heat_at_once_and_latches_after_the_window)
{
    sup_t s;
    sup_init(&s, true);

    sup_input_t apart = pair_at(sup_c_to_q7(500));
    apart.chamber2_q7 = sup_c_to_q7(500) + SUP_DISAGREE + sup_c_to_q7(10);

    /* Heat goes away on the first cycle; the window only delays the latch,
     * which is the same shape every confirmed rule in this firmware uses. */
    sup_step(&s, &apart, STEP_MS);
    CHECK(!s.permit);
    CHECK(!s.tripped);

    run_for(&s, &apart, SUP_DISAGREE_MS);
    CHECK(s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_TC_DISAGREE);
}

/*
 * @relation(SWR-SAF-37, scope=function)
 */
KILN_TEST(swrsaf37_a_gradient_inside_the_band_is_not_a_disagreement)
{
    /* A kiln chamber is not isothermal and two probes genuinely differ during a
     * ramp. A band that tripped on that would stop healthy firings, which HZ-10
     * names as how protections come to be switched off. */
    sup_t s;
    sup_init(&s, true);

    sup_input_t near = pair_at(sup_c_to_q7(900));
    near.chamber2_q7 = sup_c_to_q7(900) + SUP_DISAGREE - 1;   /* one LSB inside the band */
    run_for(&s, &near, SUP_DISAGREE_MS * 3u);

    CHECK(s.permit);
    CHECK(!s.tripped);
}

/*
 * @relation(SWR-SAF-37, scope=function)
 */
KILN_TEST(swrsaf37_a_transient_disagreement_does_not_latch)
{
    sup_t s;
    sup_init(&s, true);

    sup_input_t apart = pair_at(sup_c_to_q7(500));
    apart.chamber2_q7 = sup_c_to_q7(500) + SUP_DISAGREE + sup_c_to_q7(10);
    run_for(&s, &apart, (SUP_DISAGREE_MS * 5u) / 10u);
    CHECK(!s.tripped);

    const sup_input_t together = pair_at(sup_c_to_q7(500));
    run_for(&s, &together, 1000u);
    CHECK(!s.tripped);
    CHECK(s.permit);

    /* and the timer restarted rather than resuming where it left off */
    run_for(&s, &apart, (SUP_DISAGREE_MS * 6u) / 10u);
    CHECK(!s.tripped);
}

/*
 * @relation(SWR-SAF-37, scope=function)
 */
KILN_TEST(swrsaf37_one_usable_couple_keeps_protecting_without_a_cross_check)
{
    /* Degraded operation, deliberately allowed. Losing one couple mid-firing
     * should not stop the firing: the remaining one still provides the backstop.
     * What is lost is the comparison, and the flag says so, so the ESP32 can
     * annunciate a degraded supervisor rather than showing a healthy one. */
    sup_t s;
    sup_init(&s, true);

    sup_input_t one = pair_at(sup_c_to_q7(600));
    one.chamber2_valid = false;
    one.chamber2_q7    = 0;            /* a zero that must not be compared */
    sup_step(&s, &one, STEP_MS);

    CHECK(s.permit);
    CHECK(!s.tripped);
    CHECK((sup_flags(&s, &one) & SUP_FLAG_TC2_VALID) == 0u);

    /* and it still trips on temperature using the couple it has */
    sup_input_t hot = one;
    hot.chamber_q7 = SUP_OVERTEMP + sup_c_to_q7(5);
    sup_step(&s, &hot, STEP_MS);
    CHECK(s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_OVERTEMP);
}

/*
 * @relation(SWR-SAF-37, scope=function)
 */
KILN_TEST(swrsaf37_only_the_second_couple_usable_still_protects)
{
    sup_t s;
    sup_init(&s, true);

    sup_input_t two_only = pair_at(sup_c_to_q7(600));
    two_only.chamber_valid = false;
    two_only.chamber_q7    = 0;
    sup_step(&s, &two_only, STEP_MS);
    CHECK(s.permit);

    two_only.chamber2_q7 = SUP_OVERTEMP + sup_c_to_q7(5);
    sup_step(&s, &two_only, STEP_MS);
    CHECK(!s.permit);
    CHECK_EQ_INT(s.reason, SUP_TRIP_OVERTEMP);
}

/*
 * @relation(SWR-SAF-37, scope=function)
 */
KILN_TEST(swrsaf37_a_negative_couple_cannot_win_the_comparison)
{
    /* This was a NaN on couple 2 and is now the integer equivalent: a reading
     * far BELOW zero, which is the value that would win the comparison if the
     * maximum were ever written as a minimum, and the one the sign-extending
     * shift in the decode exists to produce correctly.  Couple 1 decides, the
     * kiln is below the backstop, and the pair is far enough apart to be caught
     * by the disagreement rule rather than by the backstop. */
    sup_t s;
    sup_init(&s, true);

    sup_input_t cold2 = pair_at(sup_c_to_q7(600));
    cold2.chamber2_q7 = sup_c_to_q7(-150);
    sup_step(&s, &cold2, STEP_MS);
    CHECK(!s.permit);                 /* they disagree by 750 degC */
    CHECK(!s.tripped);                /* but not yet long enough to latch */

    /* The effective reading is couple 1's 600 degC and not couple 2's -150, so
     * once they agree again heat is permitted rather than the pair being
     * dragged down. */
    const sup_input_t together = pair_at(sup_c_to_q7(600));
    sup_step(&s, &together, STEP_MS);
    CHECK(s.permit);
}

/* --- SWR-SAF-38: the permit readback ------------------------------------ */

/*
 * @relation(SWR-SAF-38, scope=function)
 */
KILN_TEST(swrsaf38_a_coil_that_stays_on_after_the_permit_is_withdrawn_latches)
{
    /* The output stage was the one part of the chain with no diagnostic at all:
     * the supervisor could command the coil open for an hour against a shorted
     * drive and report everything healthy. */
    sup_t s;
    sup_init(&s, true);

    /* Trip on temperature so the permit is withdrawn, then hold the sense high. */
    const sup_input_t hot = pair_at(SUP_OVERTEMP + sup_c_to_q7(5));
    sup_step(&s, &hot, STEP_MS);
    CHECK(!s.permit);
    CHECK_EQ_INT(s.reason, SUP_TRIP_OVERTEMP);

    sup_input_t stuck = pair_at(sup_c_to_q7(300));
    stuck.permit_sense = true;
    run_for(&s, &stuck, SUP_PERMIT_SETTLE_MS + 1000u);

    CHECK(s.tripped);
    /* It REPLACES the over-temperature reason, which is the one place in this
     * firmware where a later cause overwrites an earlier one. An operator shown
     * "over-temperature" would press the clear button; one shown "permit stuck"
     * is told to isolate at the supply, which is the only thing that helps when
     * the supervisor cannot open the coil. */
    CHECK_EQ_INT(s.reason, SUP_TRIP_PERMIT_STUCK);
}

/*
 * @relation(SWR-SAF-38, scope=function)
 */
KILN_TEST(swrsaf38_a_stuck_coil_does_not_override_a_failed_diagnostic)
{
    /* The limit of the escalation. A supervisor that has failed its own
     * diagnostics cannot be trusted to have concluded correctly that the coil is
     * stuck, so the diagnostic stays the reported reason: it is what makes every
     * other conclusion doubtful. */
    sup_t s;
    sup_init(&s, true);

    sup_input_t diag_bad = pair_at(sup_c_to_q7(300));
    diag_bad.diag_ok = false;
    sup_step(&s, &diag_bad, STEP_MS);
    CHECK_EQ_INT(s.reason, SUP_TRIP_SELF_TEST);

    sup_input_t stuck = pair_at(sup_c_to_q7(300));
    stuck.diag_ok      = false;
    stuck.permit_sense = true;
    run_for(&s, &stuck, SUP_PERMIT_SETTLE_MS + 1000u);

    CHECK(s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_SELF_TEST);
}

/*
 * @relation(SWR-SAF-38, scope=function)
 */
KILN_TEST(swrsaf38_a_slow_contactor_inside_the_settle_window_is_not_a_fault)
{
    sup_t s;
    sup_init(&s, true);

    const sup_input_t hot = pair_at(SUP_OVERTEMP + sup_c_to_q7(5));
    sup_step(&s, &hot, STEP_MS);

    sup_input_t dropping = pair_at(sup_c_to_q7(300));
    dropping.permit_sense = true;
    run_for(&s, &dropping, (SUP_PERMIT_SETTLE_MS * 5u) / 10u);
    CHECK_EQ_INT(s.reason, SUP_TRIP_OVERTEMP);   /* not yet the readback */

    dropping.permit_sense = false;
    run_for(&s, &dropping, 1000u);
    CHECK_EQ_INT(s.reason, SUP_TRIP_OVERTEMP);   /* and never becomes it */
}

/*
 * @relation(SWR-SAF-38, scope=function)
 */
KILN_TEST(swrsaf38_a_commanded_permit_that_is_not_sensed_is_not_a_fault)
{
    /* The other direction is heat NOT being delivered, which is the lid switch
     * or a front end's fault transistor doing its job. That is not heat the
     * supervisor cannot stop, and the ESP32's own rules notice a kiln that will
     * not heat. Tripping here would be a nuisance trip on a working interlock. */
    sup_t s;
    sup_init(&s, true);

    sup_input_t open_path = pair_at(sup_c_to_q7(500));
    open_path.permit_sense = false;          /* permitted, but nothing flows */
    run_for(&s, &open_path, SUP_PERMIT_SETTLE_MS * 5u);

    CHECK(s.permit);
    CHECK(!s.tripped);
}

/*
 * @relation(SWR-SAF-38, scope=function)
 */
KILN_TEST(swrsaf38_a_board_with_no_readback_fitted_reports_no_fault)
{
    /* permit_sense zero-initialises to "not energised", which is the benign
     * value. A board without the divider therefore reports the diagnostic as
     * absent rather than as permanently failing, and SWR-SAF-38 says so. */
    sup_t s;
    sup_init(&s, true);

    const sup_input_t no_sense = pair_at(SUP_OVERTEMP + sup_c_to_q7(5));
    sup_step(&s, &no_sense, STEP_MS);            /* trips on temperature */
    const sup_input_t cool = pair_at(sup_c_to_q7(300));       /* permit_sense stays false */
    run_for(&s, &cool, SUP_PERMIT_SETTLE_MS * 5u);

    CHECK_EQ_INT(s.reason, SUP_TRIP_OVERTEMP);
}

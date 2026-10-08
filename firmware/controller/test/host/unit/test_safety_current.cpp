/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One test per current-based safety rule -- SWR-SAF-25..SWR-SAF-30 and SWR-CUR-11/12.
 * SWR-TST-23 requires every SR to have an automated test, and these are the rules
 * requirements section 5.2 makes the *primary* detection of relay and element
 * failure, so they are the ones that must not ship untested.
 */

#include "kiln_check.h"
#include "kiln_core/safety.h"

static kiln_safety_cfg_t cfg(void)
{
    kiln_safety_cfg_t c;
    kiln_safety_cfg_defaults(&c);
    return c;
}

/* A plant that is behaving: mid-firing, sensors happy, monitoring on. */
static kiln_safety_input_t base(void)
{
    kiln_safety_input_t in = {};
    in.kiln_c               = 500.0f;
    in.case_c               = 30.0f;
    in.setpoint_c           = 500.0f;
    in.rate_c_per_h         = 100.0f;
    in.kiln_valid           = true;
    in.case_valid           = true;
    in.case_present         = true;
    in.heating_active       = true;
    in.current_monitoring   = true;
    in.heat_enable_asserted = true;
    return in;
}

static void leakage(kiln_safety_input_t *in, float amps)
{
    in->current_fresh = true;
    in->current_flags = KILN_CURF_LEAKAGE;
    in->current_a     = amps;
    in->duty_permille = 0;
}

static void conduction(kiln_safety_input_t *in, float amps)
{
    in->current_fresh = true;
    in->current_flags = KILN_CURF_CONDUCTION;
    in->current_a     = amps;
}

/* --- SWR-SAF-25 / SWR-SAF-27 ------------------------------------------------------ */

KILN_TEST(swrsaf25_latches_nothing_before_the_configured_window_count)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);
    kiln_safety_begin_run(&s, NULL);

    kiln_safety_input_t in = base();
    leakage(&in, 1.0f);                       /* well over the 0.5 A threshold */

    /* One window is not two: a single measurement must not stop a firing. */
    const kiln_safety_verdict_t v = kiln_safety_eval(&s, &in, 0.1f);
    CHECK(v.heat_permitted);
    CHECK_EQ_INT(v.fault, KILN_FAULT_NONE);
    CHECK(!v.drop_contactor);
    CHECK_EQ_UINT(s.fail_on_count, 1u);
}

KILN_TEST(swrsaf25_withholds_heat_and_drops_the_contactor_on_the_second_window)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);
    kiln_safety_begin_run(&s, NULL);

    kiln_safety_input_t in = base();
    leakage(&in, 1.0f);

    (void)kiln_safety_eval(&s, &in, 0.1f);
    const kiln_safety_verdict_t v = kiln_safety_eval(&s, &in, 0.1f);

    /* SWR-NFR-27: de-energised in the same cycle as the offending measurement, and
     * no fault latched yet -- naming the SSR before SWR-SAF-27's test has run would
     * pre-empt the distinction from a welded contactor. */
    CHECK(!v.heat_permitted);
    CHECK(v.drop_contactor);
    CHECK_EQ_INT(v.fault, KILN_FAULT_NONE);
    CHECK_EQ_INT(s.weld_phase, KILN_WELD_WAIT_DROPOUT);
}

KILN_TEST(swrsaf27_latches_ssr_shorted_when_current_stops_with_the_contactor_open)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);
    kiln_safety_begin_run(&s, NULL);

    kiln_safety_input_t in = base();
    leakage(&in, 1.0f);
    (void)kiln_safety_eval(&s, &in, 0.1f);
    (void)kiln_safety_eval(&s, &in, 0.1f);     /* sequence starts */

    /* The contactor's drop-out time, with nothing measured yet. */
    in.current_fresh = false;
    float elapsed = 0.0f;
    for (int i = 0; i < 25; i++) {
        const kiln_safety_verdict_t v = kiln_safety_eval(&s, &in, 0.1f);
        elapsed += 0.1f;
        CHECK(!v.heat_permitted);
        CHECK(v.drop_contactor);
        if (s.weld_phase == KILN_WELD_REMEASURE) {
            break;
        }
    }
    CHECK_EQ_INT(s.weld_phase, KILN_WELD_REMEASURE);
    CHECK_NEAR(elapsed, c.weld_wait_s, 0.15f);

    /* Current has ceased: the contactor opened, so it is the SSR that is shorted. */
    leakage(&in, 0.0f);
    const kiln_safety_verdict_t v = kiln_safety_eval(&s, &in, 0.1f);
    CHECK_EQ_INT(v.fault, KILN_FAULT_UNCOMMANDED_CURRENT);
    CHECK(!v.heat_permitted);

    /* SWR-NFR-27: the verdict is inside 3 s of the de-assertion. */
    CHECK(s.weld_timer_s <= c.weld_verdict_s);
}

KILN_TEST(swrsaf27_latches_contactor_welded_when_current_persists)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);
    kiln_safety_begin_run(&s, NULL);

    kiln_safety_input_t in = base();
    leakage(&in, 1.0f);
    (void)kiln_safety_eval(&s, &in, 0.1f);
    (void)kiln_safety_eval(&s, &in, 0.1f);

    in.current_fresh = false;
    for (int i = 0; i < 25 && s.weld_phase != KILN_WELD_REMEASURE; i++) {
        (void)kiln_safety_eval(&s, &in, 0.1f);
    }
    CHECK_EQ_INT(s.weld_phase, KILN_WELD_REMEASURE);

    /* Still flowing with the contactor commanded open: nothing in the controller
     * can interrupt the heaters any more. */
    leakage(&in, 1.0f);
    const kiln_safety_verdict_t v = kiln_safety_eval(&s, &in, 0.1f);
    CHECK_EQ_INT(v.fault, KILN_FAULT_CONTACTOR_WELDED);
    CHECK(!v.heat_permitted);
}

KILN_TEST(swrsaf27_takes_the_severe_verdict_when_no_measurement_arrives_in_time)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);
    kiln_safety_begin_run(&s, NULL);

    kiln_safety_input_t in = base();
    leakage(&in, 1.0f);
    (void)kiln_safety_eval(&s, &in, 0.1f);
    (void)kiln_safety_eval(&s, &in, 0.1f);

    /* The sampler never answers.  A fail-safe decision must not fail open, so
     * the verdict is the more severe of the two. */
    in.current_fresh = false;
    kiln_fault_t got = KILN_FAULT_NONE;
    for (int i = 0; i < 100 && got == KILN_FAULT_NONE; i++) {
        got = kiln_safety_eval(&s, &in, 0.1f).fault;
    }
    CHECK_EQ_INT(got, KILN_FAULT_CONTACTOR_WELDED);
}

KILN_TEST(swrsaf25_stands_down_while_heating_is_commanded_on)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);
    kiln_safety_begin_run(&s, NULL);

    /* Amps in a *conduction* window are the kiln working, not a stuck relay. */
    kiln_safety_input_t in = base();
    in.duty_permille = 600;
    conduction(&in, 28.0f);

    for (int i = 0; i < 10; i++) {
        const kiln_safety_verdict_t v = kiln_safety_eval(&s, &in, 0.1f);
        CHECK_EQ_INT(v.fault, KILN_FAULT_NONE);
    }
    CHECK_EQ_UINT(s.fail_on_count, 0u);
}

/* --- SWR-SAF-26 ------------------------------------------------------------- */

KILN_TEST(swrsaf26_latches_no_heater_current_after_the_configured_period)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);
    kiln_safety_begin_run(&s, NULL);

    kiln_safety_input_t in = base();
    in.duty_permille = 800;
    in.current_ref_a = 30.0f;
    conduction(&in, 1.0f);                 /* far below 20 % of 30 A */

    kiln_fault_t got = KILN_FAULT_NONE;
    float elapsed = 0.0f;
    for (int i = 0; i < 400 && got == KILN_FAULT_NONE; i++) {
        got = kiln_safety_eval(&s, &in, 0.1f).fault;
        elapsed += 0.1f;
    }
    CHECK_EQ_INT(got, KILN_FAULT_NO_HEATER_CURRENT);
    /* And it acts on the configured 30 s, far inside SWR-SAF-07's 15 min backstop. */
    CHECK_NEAR(elapsed, c.fail_off_window_s, 0.3f);
}

KILN_TEST(swrsaf26_uses_the_nominal_floor_before_a_reference_exists)
{
    /* SWR-CUR-08's reference is learned during the cold full-power stretch at the
     * start of a firing -- which is exactly when a dead element group is most
     * detectable, so the rule must not be blind until then. */
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);
    kiln_safety_begin_run(&s, NULL);

    CHECK(c.fail_off_min_a > 0.0f);

    kiln_safety_input_t in = base();
    in.duty_permille = 1000;
    in.current_ref_a = 0.0f;               /* not yet established */
    conduction(&in, 0.2f);

    kiln_fault_t got = KILN_FAULT_NONE;
    for (int i = 0; i < 400 && got == KILN_FAULT_NONE; i++) {
        got = kiln_safety_eval(&s, &in, 0.1f).fault;
    }
    CHECK_EQ_INT(got, KILN_FAULT_NO_HEATER_CURRENT);
}

KILN_TEST(swrsaf26_needs_a_run_of_low_windows_and_not_just_an_elapsed_timer)
{
    /* Elapsed time alone can be tipped over by one unrepresentative measurement
     * that happens to be the last before the window expires.  Requiring a run of
     * them is strictly more evidence for the same conclusion. */
    kiln_safety_t s;
    kiln_safety_cfg_t c = cfg();
    c.fail_off_min_windows = 3;
    kiln_safety_init(&s, &c);
    kiln_safety_begin_run(&s, NULL);

    kiln_safety_input_t in = base();
    in.duty_permille = 800;
    in.current_ref_a = 30.0f;

    /* One low window, then the timer runs well past its threshold with no
     * further measurements at all. */
    conduction(&in, 1.0f);
    (void)kiln_safety_eval(&s, &in, 0.1f);
    CHECK_EQ_UINT(s.fail_off_windows, 1u);

    in.current_fresh = false;
    kiln_fault_t got = KILN_FAULT_NONE;
    for (int i = 0; i < 1000 && got == KILN_FAULT_NONE; i++) {
        got = kiln_safety_eval(&s, &in, 0.1f).fault;
    }
    CHECK_EQ_INT(got, KILN_FAULT_NONE);
    CHECK(s.fail_off_timer_s > c.fail_off_window_s);

    /* The remaining windows arrive and it latches. */
    conduction(&in, 1.0f);
    (void)kiln_safety_eval(&s, &in, 0.1f);
    conduction(&in, 1.0f);
    CHECK_EQ_INT(kiln_safety_eval(&s, &in, 0.1f).fault, KILN_FAULT_NO_HEATER_CURRENT);
}

KILN_TEST(swrsaf26_a_single_good_window_clears_the_accumulated_evidence)
{
    /* The first on-window of a run can be caught while the contactor is still
     * closing and read near zero through no fault of the kiln.  One healthy
     * measurement has to undo that. */
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);
    kiln_safety_begin_run(&s, NULL);

    kiln_safety_input_t in = base();
    in.duty_permille = 800;
    in.current_ref_a = 30.0f;

    conduction(&in, 0.1f);
    (void)kiln_safety_eval(&s, &in, 0.1f);
    (void)kiln_safety_eval(&s, &in, 0.1f);
    CHECK(s.fail_off_windows > 0u);

    conduction(&in, 29.0f);
    (void)kiln_safety_eval(&s, &in, 0.1f);
    CHECK_EQ_UINT(s.fail_off_windows, 0u);
    CHECK_NEAR(s.fail_off_timer_s, 0.0f, 0.001f);
}

KILN_TEST(swrsaf26_does_not_trip_on_healthy_current)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);
    kiln_safety_begin_run(&s, NULL);

    kiln_safety_input_t in = base();
    in.duty_permille = 800;
    in.current_ref_a = 30.0f;
    conduction(&in, 29.0f);

    for (int i = 0; i < 1000; i++) {
        CHECK_EQ_INT(kiln_safety_eval(&s, &in, 0.1f).fault, KILN_FAULT_NONE);
    }
}

/* --- SWR-SAF-28 ------------------------------------------------------------- */

KILN_TEST(swrsaf28_warns_at_the_warning_band_without_withholding_heat)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);
    kiln_safety_begin_run(&s, NULL);

    kiln_safety_input_t in = base();
    in.duty_permille           = 800;
    in.current_ref_a           = 30.0f;
    conduction(&in, 26.0f);
    in.current_deviation       = -0.13f;        /* over 10 %, under 25 % */
    in.current_deviation_valid = true;

    const kiln_safety_verdict_t v = kiln_safety_eval(&s, &in, 0.1f);
    CHECK(v.heat_permitted);
    CHECK_EQ_INT(v.fault, KILN_FAULT_NONE);
    CHECK(v.warnings & KILN_WARN_BIT(KILN_WARN_CURRENT_DEV));
}

KILN_TEST(swrsaf28_latches_current_deviation_at_the_fault_band)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);
    kiln_safety_begin_run(&s, NULL);

    kiln_safety_input_t in = base();
    in.duty_permille           = 800;
    in.current_ref_a           = 30.0f;
    conduction(&in, 20.0f);
    /* One group of three gone is a step of a known fraction -- which is how the
     * requirement describes the thing it wants detected. */
    in.current_deviation       = -1.0f / 3.0f;
    in.current_deviation_valid = true;

    kiln_fault_t got = KILN_FAULT_NONE;
    for (int i = 0; i < 400 && got == KILN_FAULT_NONE; i++) {
        got = kiln_safety_eval(&s, &in, 0.1f).fault;
    }
    CHECK_EQ_INT(got, KILN_FAULT_CURRENT_DEVIATION);
}

KILN_TEST(swrsaf28_holds_its_timer_when_no_comparison_is_available)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);
    kiln_safety_begin_run(&s, NULL);

    kiln_safety_input_t in = base();
    in.duty_permille           = 800;
    in.current_ref_a           = 30.0f;
    conduction(&in, 20.0f);
    in.current_deviation       = -0.33f;
    in.current_deviation_valid = true;

    for (int i = 0; i < 100; i++) {
        (void)kiln_safety_eval(&s, &in, 0.1f);
    }
    const float before = s.deviation_timer_s;
    CHECK(before > 0.0f);

    /* A run of skipped windows (SWR-CUR-05) is not evidence of anything, so it
     * must neither advance the timer nor reset the progress already made. */
    in.current_deviation_valid = false;
    for (int i = 0; i < 100; i++) {
        (void)kiln_safety_eval(&s, &in, 0.1f);
    }
    CHECK_NEAR(s.deviation_timer_s, before, 0.001f);
}

/* --- SWR-SAF-29 ------------------------------------------------------------- */

KILN_TEST(swrsaf29_latches_overcurrent_after_the_configured_windows)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);
    kiln_safety_begin_run(&s, NULL);

    /* 120 % of the 30 A nominal is 36 A. */
    CHECK_NEAR(c.overcurrent_a, 36.0f, 0.01f);

    kiln_safety_input_t in = base();
    in.duty_permille = 1000;
    conduction(&in, 45.0f);

    const kiln_safety_verdict_t v1 = kiln_safety_eval(&s, &in, 0.1f);
    CHECK_EQ_INT(v1.fault, KILN_FAULT_NONE);          /* one burst is not proof */
    const kiln_safety_verdict_t v2 = kiln_safety_eval(&s, &in, 0.1f);
    CHECK_EQ_INT(v2.fault, KILN_FAULT_OVERCURRENT);
    CHECK(!v2.heat_permitted);
}

KILN_TEST(swrsaf29_resets_its_count_on_a_single_clean_window)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);
    kiln_safety_begin_run(&s, NULL);

    kiln_safety_input_t in = base();
    in.duty_permille = 1000;

    conduction(&in, 45.0f);
    (void)kiln_safety_eval(&s, &in, 0.1f);
    conduction(&in, 29.0f);
    (void)kiln_safety_eval(&s, &in, 0.1f);
    CHECK_EQ_UINT(s.overcurrent_count, 0u);
    conduction(&in, 45.0f);
    CHECK_EQ_INT(kiln_safety_eval(&s, &in, 0.1f).fault, KILN_FAULT_NONE);
}

/* --- SWR-SAF-30 ------------------------------------------------------------- */

KILN_TEST(swrsaf30_warns_when_a_relay_reaches_its_life_limit)
{
    kiln_safety_t s;
    kiln_safety_cfg_t c = cfg();
    c.contactor_life_ops = 1000;
    kiln_safety_init(&s, &c);
    kiln_safety_begin_run(&s, NULL);

    kiln_safety_input_t in = base();
    in.contactor_ops = 999;
    CHECK(!(kiln_safety_eval(&s, &in, 0.1f).warnings & KILN_WARN_BIT(KILN_WARN_RELAY_WEAR)));

    in.contactor_ops = 1000;
    const kiln_safety_verdict_t v = kiln_safety_eval(&s, &in, 0.1f);
    CHECK(v.warnings & KILN_WARN_BIT(KILN_WARN_RELAY_WEAR));
    CHECK(v.heat_permitted);     /* a relay at its life limit still works */
}

KILN_TEST(swrsaf30_warns_on_intermittent_mismatches_that_self_clear)
{
    kiln_safety_t s;
    kiln_safety_cfg_t c = cfg();
    c.mismatch_episodes_warn = 3;
    kiln_safety_init(&s, &c);
    kiln_safety_begin_run(&s, NULL);

    kiln_safety_input_t in = base();

    /* Current appears in an off-window and goes away again, three times: never
     * enough to trip SWR-SAF-25, which is exactly the early sign SWR-SAF-30 is after. */
    for (int ep = 0; ep < 3; ep++) {
        leakage(&in, 1.0f);
        (void)kiln_safety_eval(&s, &in, 0.1f);
        leakage(&in, 0.0f);
        (void)kiln_safety_eval(&s, &in, 0.1f);
    }
    CHECK_EQ_UINT(s.mismatch_episodes, 3u);

    const kiln_safety_verdict_t v = kiln_safety_eval(&s, &in, 0.1f);
    CHECK(v.warnings & KILN_WARN_BIT(KILN_WARN_RELAY_SUSPECT));
    CHECK_EQ_INT(v.fault, KILN_FAULT_NONE);
}

/* --- SWR-CUR-11 and SWR-CUR-12 ------------------------------------------- */

KILN_TEST(swrcur11_latches_a_ct_fault_only_after_its_grace_period)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);
    kiln_safety_begin_run(&s, NULL);

    kiln_safety_input_t in = base();
    in.current_fresh = true;
    in.current_flags = (uint8_t)(KILN_CURF_CT_FAULT | KILN_CURF_LEAKAGE);
    in.current_a     = 0.0f;

    /* A burst spoiled by the switching transient of a multi-kilowatt load is not
     * a missing transformer, so it is graced like SWR-SAF-04. */
    for (int i = 0; i < 40; i++) {
        CHECK_EQ_INT(kiln_safety_eval(&s, &in, 0.1f).fault, KILN_FAULT_NONE);
    }
    kiln_fault_t got = KILN_FAULT_NONE;
    for (int i = 0; i < 40 && got == KILN_FAULT_NONE; i++) {
        got = kiln_safety_eval(&s, &in, 0.1f).fault;
    }
    CHECK_EQ_INT(got, KILN_FAULT_CT_FAULT);
}

KILN_TEST(swrcur11_a_void_measurement_does_not_read_as_a_dead_element)
{
    /* The dangerous confusion: 0 A reported by a CT that is not there must not
     * satisfy SWR-SAF-26, or an unplugged transformer would be diagnosed as a failed
     * element group on every firing. */
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);
    kiln_safety_begin_run(&s, NULL);

    kiln_safety_input_t in = base();
    in.duty_permille = 1000;
    in.current_ref_a = 30.0f;
    in.current_fresh = true;
    in.current_flags = (uint8_t)(KILN_CURF_CT_FAULT | KILN_CURF_CONDUCTION);
    in.current_a     = 0.0f;

    /* The CT fault latches first, and SWR-SAF-26 never sees a usable measurement. */
    kiln_fault_t got = KILN_FAULT_NONE;
    for (int i = 0; i < 400 && got == KILN_FAULT_NONE; i++) {
        got = kiln_safety_eval(&s, &in, 0.1f).fault;
    }
    CHECK_EQ_INT(got, KILN_FAULT_CT_FAULT);
    CHECK_NEAR(s.fail_off_timer_s, 0.0f, 0.001f);
}

KILN_TEST(swrcur12_warns_persistently_while_monitoring_is_disabled)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);
    kiln_safety_begin_run(&s, NULL);

    kiln_safety_input_t in = base();
    in.current_monitoring = false;

    for (int i = 0; i < 10; i++) {
        const kiln_safety_verdict_t v = kiln_safety_eval(&s, &in, 0.1f);
        CHECK(v.warnings & KILN_WARN_BIT(KILN_WARN_CURRENT_OFF));
        CHECK(v.heat_permitted);
    }

    /* And it clears the moment monitoring comes back, because a warning is not
     * latching (Appendix A). */
    in.current_monitoring = true;
    const kiln_safety_verdict_t v = kiln_safety_eval(&s, &in, 0.1f);
    CHECK(!(v.warnings & KILN_WARN_BIT(KILN_WARN_CURRENT_OFF)));
}

KILN_TEST(swrcur12_current_rules_stand_down_when_monitoring_is_off)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);
    kiln_safety_begin_run(&s, NULL);

    kiln_safety_input_t in = base();
    in.current_monitoring = false;
    leakage(&in, 50.0f);           /* nonsense, and correctly ignored */

    for (int i = 0; i < 100; i++) {
        CHECK_EQ_INT(kiln_safety_eval(&s, &in, 0.1f).fault, KILN_FAULT_NONE);
    }
    CHECK_EQ_UINT(s.fail_on_count, 0u);
}

KILN_TEST(stale_and_skipped_measurements_are_not_evidence)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);
    kiln_safety_begin_run(&s, NULL);

    kiln_safety_input_t in = base();

    in.current_fresh = true;
    in.current_flags = (uint8_t)(KILN_CURF_LEAKAGE | KILN_CURF_STALE);
    in.current_a     = 5.0f;
    for (int i = 0; i < 10; i++) {
        (void)kiln_safety_eval(&s, &in, 0.1f);
    }
    CHECK_EQ_UINT(s.fail_on_count, 0u);

    in.current_flags = (uint8_t)(KILN_CURF_LEAKAGE | KILN_CURF_SKIPPED);
    for (int i = 0; i < 10; i++) {
        (void)kiln_safety_eval(&s, &in, 0.1f);
    }
    CHECK_EQ_UINT(s.fail_on_count, 0u);

    /* And a measurement that is not fresh is the same window counted twice. */
    in.current_flags = KILN_CURF_LEAKAGE;
    in.current_fresh = false;
    for (int i = 0; i < 10; i++) {
        (void)kiln_safety_eval(&s, &in, 0.1f);
    }
    CHECK_EQ_UINT(s.fail_on_count, 0u);
}

/* --- SWR-SAF-17 / SWR-SAF-18: clearability --------------------------------------- */

KILN_TEST(swrsaf27_welded_contactor_is_never_clearable_by_acknowledgement)
{
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_input_t in = base();
    leakage(&in, 0.0f);

    /* The one fault whose instruction is "isolate the kiln, the controller can no
     * longer interrupt the current".  There is nothing an acknowledgement could
     * make safe, so it must not be one acknowledgement away from cleared. */
    CHECK(!kiln_safety_can_clear(&c, KILN_FAULT_CONTACTOR_WELDED, &in));
}

KILN_TEST(swrsaf18_can_clear_is_an_allow_list_not_a_deny_list)
{
    const kiln_safety_cfg_t c = cfg();
    const kiln_safety_input_t in = base();

    /* A code outside the enum stands in for the next fault someone adds: it must
     * default to needing a decision, not to being clearable. */
    CHECK(!kiln_safety_can_clear(&c, (kiln_fault_t)200, &in));
    CHECK(!kiln_safety_can_clear(&c, KILN_FAULT_MAX, &in));
    CHECK(!kiln_safety_can_clear(NULL, KILN_FAULT_TC_OPEN, &in));
    CHECK(!kiln_safety_can_clear(&c, KILN_FAULT_TC_OPEN, NULL));
}

KILN_TEST(swrsaf18_current_faults_clear_only_once_their_condition_has_gone)
{
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_input_t in = base();

    /* CT fault: clearable once a transformer answers again. */
    in.current_flags = KILN_CURF_CT_FAULT;
    CHECK(!kiln_safety_can_clear(&c, KILN_FAULT_CT_FAULT, &in));
    in.current_flags = KILN_CURF_LEAKAGE;
    CHECK(kiln_safety_can_clear(&c, KILN_FAULT_CT_FAULT, &in));

    /* SWR-SAF-25: only once an off-window actually measures no current. */
    in.current_a = 2.0f;
    CHECK(!kiln_safety_can_clear(&c, KILN_FAULT_UNCOMMANDED_CURRENT, &in));
    in.current_a = 0.0f;
    CHECK(kiln_safety_can_clear(&c, KILN_FAULT_UNCOMMANDED_CURRENT, &in));

    /* SWR-SAF-29: only once current is back inside the limit. */
    in.current_a = 50.0f;
    CHECK(!kiln_safety_can_clear(&c, KILN_FAULT_OVERCURRENT, &in));
    in.current_a = 20.0f;
    CHECK(kiln_safety_can_clear(&c, KILN_FAULT_OVERCURRENT, &in));
}

KILN_TEST(nominal_current_sets_every_limit_derived_from_it)
{
    kiln_safety_cfg_t c = cfg();
    kiln_safety_cfg_set_nominal_current(&c, 50.0f);
    CHECK_NEAR(c.overcurrent_a, 60.0f, 0.01f);
    CHECK_NEAR(c.fail_off_min_a, 10.0f, 0.01f);
}

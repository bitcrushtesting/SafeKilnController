/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One test per thermal safety rule -- SWR-SAF-04..SWR-SAF-13, plus the reading-validity
 * behaviour of SWR-ACQ-12 and the non-latching warning semantics of Appendix A.
 */

#include "kiln_check.h"
#include "kiln_core/safety.h"

static kiln_safety_cfg_t cfg(void)
{
    kiln_safety_cfg_t c;
    kiln_safety_cfg_defaults(&c);
    return c;
}

static kiln_safety_input_t base(void)
{
    kiln_safety_input_t in = {};
    in.kiln_c             = 500.0f;
    in.case_c             = 30.0f;
    in.setpoint_c         = 500.0f;
    in.rate_c_per_h       = 100.0f;
    in.kiln_valid         = true;
    in.case_valid         = true;
    in.case_present       = true;
    in.heating_active     = true;
    in.current_monitoring = true;
    return in;
}

/* Run the supervisor for `seconds` at 100 ms and return the first fault. */
static kiln_fault_t run_for(kiln_safety_t *s, kiln_safety_input_t *in, float seconds)
{
    const int steps = (int)(seconds / 0.1f);
    for (int i = 0; i < steps; i++) {
        const kiln_fault_t f = kiln_safety_eval(s, in, 0.1f).fault;
        if (f != KILN_FAULT_NONE) {
            return f;
        }
    }
    return KILN_FAULT_NONE;
}

/* --- SWR-SAF-04 ------------------------------------------------------------- */

/*
 * @relation(SWR-SAF-04, scope=function)
 */
KILN_TEST(swrsaf04_tolerates_a_glitch_and_latches_a_persistent_tc_fault)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);

    kiln_safety_input_t in = base();

    /* SWR-ACQ-12: inside the grace period a reported fault is tolerated. */
    in.tc_fault_bits = KILN_TC_FAULT_OPEN;
    in.kiln_valid    = false;
    CHECK_EQ_INT(run_for(&s, &in, c.tc_grace_s - 1.0f), KILN_FAULT_NONE);

    /* It cleared: the timer resets and nothing was latched. */
    in.tc_fault_bits = 0;
    in.kiln_valid    = true;
    CHECK_EQ_INT(run_for(&s, &in, 1.0f), KILN_FAULT_NONE);

    /* Now it stays. */
    in.tc_fault_bits = KILN_TC_FAULT_OPEN;
    in.kiln_valid    = false;
    CHECK_EQ_INT(run_for(&s, &in, c.tc_grace_s + 1.0f), KILN_FAULT_TC_OPEN);
}

/*
 * @relation(SWR-SAF-04, scope=function)
 */
KILN_TEST(swrsaf04_maps_each_fault_bit_to_its_own_code)
{
    const kiln_safety_cfg_t c = cfg();
    const struct { uint16_t bits; kiln_fault_t want; } cases[] = {
        { KILN_TC_FAULT_COMMS,     KILN_FAULT_TC_COMMS },
        { KILN_TC_FAULT_OPEN,      KILN_FAULT_TC_OPEN  },
        { KILN_TC_FAULT_SHORT_GND, KILN_FAULT_TC_SHORT },
        { KILN_TC_FAULT_SHORT_VCC, KILN_FAULT_TC_SHORT },
        { KILN_TC_FAULT_CJ_RANGE,  KILN_FAULT_CJ       },
        { KILN_TC_FAULT_TC_RANGE,  KILN_FAULT_TC_RANGE },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        kiln_safety_t s;
        kiln_safety_init(&s, &c);
        kiln_safety_input_t in = base();
        in.tc_fault_bits = cases[i].bits;
        in.kiln_valid    = false;
        CHECK_EQ_INT(run_for(&s, &in, c.tc_grace_s + 0.5f), cases[i].want);
    }
}

/*
 * @relation(SWR-SAF-04, scope=function)
 */
KILN_TEST(swrsaf04_case_channel_reports_its_own_code)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);

    kiln_safety_input_t in = base();
    in.case_fault_bits = KILN_TC_FAULT_OPEN;
    in.case_valid      = false;
    CHECK_EQ_INT(run_for(&s, &in, c.tc_grace_s + 0.5f), KILN_FAULT_CASE_TC);
}

KILN_TEST(an_invalid_chamber_reading_withholds_heat_without_latching)
{
    /* SWR-ACQ-12's grace period says tolerate the fault, not control on a number
     * the front end has disowned. */
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);

    kiln_safety_input_t in = base();
    in.kiln_valid    = false;
    in.tc_fault_bits = KILN_TC_FAULT_OPEN;

    const kiln_safety_verdict_t v = kiln_safety_eval(&s, &in, 0.1f);
    CHECK(!v.heat_permitted);
    CHECK_EQ_INT(v.fault, KILN_FAULT_NONE);
}

KILN_TEST(a_non_finite_reading_is_treated_as_an_invalid_one)
{
    /* SWR-NFR-17 / SYS-SAF-01: a NaN must not be able to produce a non-zero duty, and a
     * NaN compared against a limit satisfies no comparison at all. */
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);

    kiln_safety_input_t in = base();
    in.kiln_c = 0.0f / 0.0f;

    const kiln_safety_verdict_t v = kiln_safety_eval(&s, &in, 0.1f);
    CHECK(!v.heat_permitted);
}

/* --- SWR-SAF-05 / SWR-SAF-06 / SWR-SAF-07 / SWR-SAF-08 / SWR-SAF-10 ----------------------------- */

/*
 * @relation(SWR-SAF-05, scope=function)
 */
KILN_TEST(swrsaf05_latches_a_reversed_thermocouple)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);

    kiln_safety_input_t in = base();
    in.duty_permille = 900;              /* above the 50 % arming duty */
    (void)kiln_safety_eval(&s, &in, 0.1f);   /* arms, reference 500 */

    /* The drop has to persist: an instantaneous one is a kiln with transport lag
     * overshooting, not a reversed probe. */
    in.kiln_c = 500.0f - c.reversed_drop_c - 0.1f;
    CHECK_EQ_INT(run_for(&s, &in, c.reversed_confirm_s - 1.0f), KILN_FAULT_NONE);
    CHECK_EQ_INT(run_for(&s, &in, 2.0f), KILN_FAULT_TC_REVERSED);
}

/*
 * @relation(SWR-SAF-05, scope=function)
 */
KILN_TEST(swrsaf05_does_not_trip_on_an_overshoot_that_recovers)
{
    /* The false trip this confirmation window exists to prevent: the controller
     * pushes duty back up while the kiln, ten seconds of transport lag behind it,
     * is still coasting down off an overshoot. */
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);

    kiln_safety_input_t in = base();
    in.duty_permille = 900;
    (void)kiln_safety_eval(&s, &in, 0.1f);

    for (int cycle = 0; cycle < 20; cycle++) {
        /* Fall 8 degC over ten seconds, then recover the same 8.  The setpoint
         * follows, because SWR-SAF-10 is not the subject here. */
        for (int i = 0; i < 100; i++) {
            in.kiln_c    -= 0.08f;
            in.setpoint_c = in.kiln_c;
            CHECK_EQ_INT(kiln_safety_eval(&s, &in, 0.1f).fault, KILN_FAULT_NONE);
        }
        for (int i = 0; i < 100; i++) {
            in.kiln_c    += 0.08f;
            in.setpoint_c = in.kiln_c;
            CHECK_EQ_INT(kiln_safety_eval(&s, &in, 0.1f).fault, KILN_FAULT_NONE);
        }
    }
}

/*
 * @relation(SWR-SAF-06, scope=function)
 */
KILN_TEST(swrsaf06_latches_a_stuck_sensor_after_its_window)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);

    kiln_safety_input_t in = base();
    in.duty_permille = 900;
    /* Not moving at all while heating hard. */
    CHECK_EQ_INT(run_for(&s, &in, c.stuck_window_s + 1.0f), KILN_FAULT_TC_STUCK);
}

/*
 * @relation(SWR-SAF-06, scope=function)
 */
KILN_TEST(swrsaf06_holds_its_window_across_an_invalid_reading)
{
    /* The reason the rules hold rather than reset: a sensor that glitches once
     * per grace period would otherwise keep a 10 min window permanently at zero,
     * which disables SWR-SAF-06 altogether. */
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);

    kiln_safety_input_t in = base();
    in.duty_permille = 900;
    (void)run_for(&s, &in, 100.0f);
    const float progressed = s.stuck.timer_s;
    CHECK(progressed > 50.0f);

    in.kiln_valid = false;
    (void)run_for(&s, &in, 3.0f);
    CHECK_NEAR(s.stuck.timer_s, progressed, 0.001f);   /* held, not reset */

    in.kiln_valid = true;
    CHECK_EQ_INT(run_for(&s, &in, c.stuck_window_s), KILN_FAULT_TC_STUCK);
}

/*
 * @relation(SWR-SAF-06, scope=function)
 */
KILN_TEST(swrsaf06_does_not_record_an_invalid_reading_as_a_window_extreme)
{
    /* A frozen value admitted into min/max is indistinguishable from a stuck
     * probe -- and worse, a wild one would make a stuck probe look healthy. */
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);

    kiln_safety_input_t in = base();
    in.duty_permille = 900;
    (void)kiln_safety_eval(&s, &in, 0.1f);
    (void)kiln_safety_eval(&s, &in, 0.1f);

    in.kiln_valid = false;
    in.kiln_c     = 900.0f;                /* a wild excursion, not believed */
    (void)run_for(&s, &in, 2.0f);
    CHECK_NEAR(s.stuck.max_c, 500.0f, 0.001f);
}

/*
 * @relation(SWR-SAF-07, scope=function)
 */
KILN_TEST(swrsaf07_latches_a_heating_failure)
{
    kiln_safety_t s;
    /* SWR-SAF-06's window is shorter than SWR-SAF-07's and would fire first on a reading
     * that never moves at all, so it is pushed out of the way here: this test is
     * about the runaway rule, and the two are exercised separately. */
    kiln_safety_cfg_t c = cfg();
    c.stuck_window_s = 100000.0f;
    kiln_safety_init(&s, &c);

    kiln_safety_input_t in = base();
    in.duty_permille = 900;              /* above the 80 % threshold */
    in.rate_c_per_h  = 2.0f;             /* below the 10 degC/h minimum */
    CHECK_EQ_INT(run_for(&s, &in, c.runaway_window_s + 1.0f), KILN_FAULT_RUNAWAY);
}

/*
 * @relation(SWR-SAF-07, scope=function)
 */
KILN_TEST(swrsaf07_does_not_trip_while_the_kiln_is_rising)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);

    kiln_safety_input_t in = base();
    in.duty_permille = 1000;
    in.rate_c_per_h  = 150.0f;
    /* And the reading moves as the rate says it does, so SWR-SAF-06 is satisfied too. */
    const int steps = (int)((c.runaway_window_s + 100.0f) / 0.1f);
    for (int i = 0; i < steps; i++) {
        in.kiln_c += 150.0f / 3600.0f * 0.1f;
        CHECK_EQ_INT(kiln_safety_eval(&s, &in, 0.1f).fault, KILN_FAULT_NONE);
    }
}

/*
 * @relation(SWR-SAF-08, scope=function)
 */
KILN_TEST(swrsaf08_latches_uncommanded_heating_after_the_settle_period)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);

    kiln_safety_input_t in = base();
    in.duty_permille  = 0;
    in.heating_active = false;

    /* Heat soaking inward after a high-duty spell must not read as a shorted
     * SSR, so the rule waits before arming. */
    CHECK_EQ_INT(run_for(&s, &in, c.uncommanded_settle_s - 1.0f), KILN_FAULT_NONE);
    CHECK(!s.uncommanded.armed);
    (void)run_for(&s, &in, 2.0f);
    CHECK(s.uncommanded.armed);

    in.kiln_c = 500.0f + c.uncommanded_rise_c + 0.1f;
    CHECK_EQ_INT(run_for(&s, &in, 1.0f), KILN_FAULT_UNCOMMANDED_HEAT);
}

/*
 * @relation(SWR-SAF-08, scope=function)
 */
KILN_TEST(swrsaf08_is_not_fooled_by_a_cooling_kiln)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);

    kiln_safety_input_t in = base();
    in.duty_permille  = 0;
    in.heating_active = false;
    (void)run_for(&s, &in, c.uncommanded_settle_s + 1.0f);

    /* Falling steadily: the reference follows it down, so the test stays a test
     * of rise rather than of absolute temperature. */
    for (int i = 0; i < 2000; i++) {
        in.kiln_c -= 0.05f;
        CHECK_EQ_INT(kiln_safety_eval(&s, &in, 0.1f).fault, KILN_FAULT_NONE);
    }
}

/*
 * @relation(SWR-SAF-09, scope=function)
 */
KILN_TEST(swrsaf09_withholds_heat_at_the_limit_and_latches_beyond_the_margin)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);

    kiln_safety_input_t in = base();
    in.kiln_c = c.max_temp_c + 1.0f;

    const kiln_safety_verdict_t v1 = kiln_safety_eval(&s, &in, 0.1f);
    CHECK(!v1.heat_permitted);
    CHECK_EQ_INT(v1.fault, KILN_FAULT_NONE);

    in.kiln_c = c.max_temp_c + c.overtemp_margin_c + 1.0f;
    const kiln_safety_verdict_t v2 = kiln_safety_eval(&s, &in, 0.1f);
    CHECK(!v2.heat_permitted);
    CHECK_EQ_INT(v2.fault, KILN_FAULT_OVERTEMP);
}

/*
 * @relation(SWR-SAF-09, scope=function)
 */
KILN_TEST(swrsaf09_binds_the_compile_time_ceiling_whatever_is_configured)
{
    /* SWR-SAF-23: no configuration may exceed the compile-time ceiling. */
    kiln_safety_t s;
    kiln_safety_cfg_t c = cfg();
    c.max_temp_c = 5000.0f;
    kiln_safety_init(&s, &c);
    CHECK_NEAR(s.cfg.max_temp_c, KILN_TEMP_CEILING_C, 0.01f);
}

/*
 * @relation(SWR-SAF-10, scope=function)
 */
KILN_TEST(swrsaf10_latches_a_setpoint_excursion)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);

    kiln_safety_input_t in = base();
    in.kiln_c     = 500.0f + c.excursion_band_c + 1.0f;
    in.setpoint_c = 500.0f;
    CHECK_EQ_INT(run_for(&s, &in, c.excursion_window_s + 1.0f), KILN_FAULT_SP_EXCURSION);
}

/*
 * @relation(SWR-SAF-11, scope=function)
 */
KILN_TEST(swrsaf11_latches_an_over_hot_enclosure)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);

    kiln_safety_input_t in = base();
    in.case_c = c.max_case_temp_c + 1.0f;

    const kiln_safety_verdict_t v = kiln_safety_eval(&s, &in, 0.1f);
    CHECK(!v.heat_permitted);
    CHECK_EQ_INT(v.fault, KILN_FAULT_CASE_OVERTEMP);
}

/*
 * @relation(SWR-SAF-12, scope=function)
 */
KILN_TEST(swrsaf12_warns_when_a_band_costs_more_than_the_baseline)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);

    kiln_insulation_baseline_t bl = {};
    bl.duty_s[5] = 100;     /* 600 degC cost 100 duty-seconds before */
    bl.valid[5]  = true;
    kiln_safety_begin_run(&s, &bl);

    kiln_safety_input_t in = base();
    in.kiln_c        = 500.0f;
    in.duty_permille = 1000;

    /* Spend 200 duty-seconds getting there: well over 1.3x the baseline. */
    for (int i = 0; i < 2000; i++) {
        (void)kiln_safety_eval(&s, &in, 0.1f);
    }
    in.kiln_c = 650.0f;
    const kiln_safety_verdict_t v = kiln_safety_eval(&s, &in, 0.1f);

    CHECK(v.warnings & KILN_WARN_BIT(KILN_WARN_INSULATION));
    CHECK(v.heat_permitted);     /* a warning never withholds heat */
}

/*
 * @relation(SWR-SAF-13, scope=function)
 */
KILN_TEST(swrsaf13_reports_a_missed_deadline_and_withholds_heat)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);

    kiln_safety_input_t in = base();
    in.safety_deadline_missed = true;
    kiln_safety_verdict_t v = kiln_safety_eval(&s, &in, 0.1f);
    CHECK(!v.heat_permitted);
    CHECK_EQ_INT(v.fault, KILN_FAULT_SAFETY_DEADLINE);

    in.safety_deadline_missed  = false;
    in.control_deadline_missed = true;
    v = kiln_safety_eval(&s, &in, 0.1f);
    CHECK(!v.heat_permitted);
    CHECK_EQ_INT(v.fault, KILN_FAULT_CONTROL_DEADLINE);
}

/* --- warning semantics (Appendix A) ------------------------------------- */

KILN_TEST(warning_107_clears_when_duty_comes_off_the_ceiling)
{
    /* Appendix A defines warnings as non-latching, and SWR-CTL-15's is a live
     * condition: the kiln is at full power *now*. */
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);
    kiln_safety_begin_run(&s, NULL);

    kiln_safety_input_t in = base();
    in.duty_permille = KILN_DUTY_MAX;

    /* Rising as it goes, so SWR-SAF-06 and SWR-SAF-07 stay satisfied: the warning is the
     * subject here, not a race between rules. */
    for (int i = 0; i < (int)(c.saturated_warn_s / 0.1f) + 10; i++) {
        in.kiln_c += 0.01f;
        (void)kiln_safety_eval(&s, &in, 0.1f);
    }
    CHECK(kiln_safety_eval(&s, &in, 0.1f).warnings &
          KILN_WARN_BIT(KILN_WARN_DUTY_SATURATED));

    in.duty_permille = 700;
    const kiln_safety_verdict_t v = kiln_safety_eval(&s, &in, 0.1f);
    CHECK(!(v.warnings & KILN_WARN_BIT(KILN_WARN_DUTY_SATURATED)));
}

KILN_TEST(warning_101_stays_for_the_run_because_its_evidence_has_passed)
{
    /* The genuinely episodic exception: a band crossing that cost too much is a
     * conclusion drawn once, from evidence that is now in the past. */
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);

    kiln_insulation_baseline_t bl = {};
    bl.duty_s[0] = 1;
    bl.valid[0]  = true;
    kiln_safety_begin_run(&s, &bl);

    kiln_safety_input_t in = base();
    in.kiln_c        = 50.0f;
    in.duty_permille = 1000;
    for (int i = 0; i < 100; i++) {
        (void)kiln_safety_eval(&s, &in, 0.1f);
    }
    in.kiln_c = 150.0f;
    CHECK(kiln_safety_eval(&s, &in, 0.1f).warnings &
          KILN_WARN_BIT(KILN_WARN_INSULATION));

    in.duty_permille  = 0;
    in.heating_active = false;
    CHECK(kiln_safety_eval(&s, &in, 0.1f).warnings &
          KILN_WARN_BIT(KILN_WARN_INSULATION));

    /* And a new run starts from nothing. */
    kiln_safety_begin_run(&s, NULL);
    CHECK(!(kiln_safety_eval(&s, &in, 0.1f).warnings &
            KILN_WARN_BIT(KILN_WARN_INSULATION)));
}

KILN_TEST(eval_checked_separates_an_invalid_argument_from_a_missed_deadline)
{
    /* Reporting fault 14 for a NULL pointer conflates a firmware defect with
     * SWR-SAF-13's missed deadline, and sends the reader looking at the scheduler. */
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);
    const kiln_safety_input_t in = base();

    kiln_safety_verdict_t v;
    CHECK_ERR(kiln_safety_eval_checked(&s, &in, -1.0f, &v), KILN_ERR_INVALID_ARG);
    CHECK(!v.heat_permitted);
    CHECK_EQ_INT(v.fault, KILN_FAULT_NONE);

    /* And the unchecked form too: fault 14 is SWR-SAF-13's missed deadline and
     * nothing else, so a bad argument must not borrow it. */
    v = kiln_safety_eval(&s, &in, -1.0f);
    CHECK(!v.heat_permitted);
    CHECK_EQ_INT(v.fault, KILN_FAULT_NONE);
    v = kiln_safety_eval(&s, NULL, 0.1f);
    CHECK(!v.heat_permitted);
    CHECK_EQ_INT(v.fault, KILN_FAULT_NONE);
    v = kiln_safety_eval(NULL, &in, 0.1f);
    CHECK(!v.heat_permitted);
    CHECK_EQ_INT(v.fault, KILN_FAULT_NONE);

    CHECK_ERR(kiln_safety_eval_checked(&s, NULL, 0.1f, &v), KILN_ERR_INVALID_ARG);
    CHECK_OK(kiln_safety_eval_checked(&s, &in, 0.1f, &v));
    CHECK(v.heat_permitted);
}

KILN_TEST(band_duty_validates_its_argument)
{
    uint8_t n = 0;
    CHECK(kiln_safety_band_duty(NULL, &n) == NULL);
    CHECK_EQ_UINT(n, KILN_INSUL_BANDS);
}

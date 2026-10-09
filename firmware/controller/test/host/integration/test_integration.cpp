/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The real core and application against the plant simulator -- architecture
 * section 14.4's integration level, and the fault-injection suite of SWR-TST-27.
 *
 * SWA-02's injected clock is what makes these possible: a four-hour firing, a
 * 15 min runaway timer and a 30 s fail-off window are all simulated in
 * milliseconds of wall time, with no clock being read anywhere in the core.
 */

#include <math.h>
#include <string.h>
#include "kiln_check.h"
#include "kiln_app/app.h"
#include "kiln_core/faults.h"
#include "kiln_core/profile.h"
#include "kiln_core/suplink.h"
#include "kiln_sim/sim.h"

/* --- the harness -------------------------------------------------------- */

typedef struct {
    kiln_sim_t       sim;
    kiln_sim_ports_t ports;
    kiln_app_t       app;
    double           t_s;
    /* The periods of architecture section 6.1, driven from one loop. */
    double           next_window_s, next_safety_s, next_acquire_s, next_control_s;
} rig_t;

#define WINDOW_DT  0.010
#define SAFETY_DT  0.100
#define ACQUIRE_DT 0.250
#define CONTROL_DT 1.000

static void rig_init(rig_t *r, float ambient_c)
{
    memset(r, 0, sizeof(*r));

    kiln_sim_cfg_t sc;
    kiln_sim_cfg_defaults(&sc);
    sc.ambient_c      = ambient_c;
    sc.case_ambient_c = ambient_c + 2.0f;
    /* A faster plant than a real kiln, so a schedule completes in a test rather
     * than in a day.  The rules under test are all in seconds or minutes. */
    sc.tau_s          = 300.0f;
    sc.dead_time_s    = 10.0f;
    sc.noise_c        = 0.1f;
    kiln_sim_init(&r->sim, &sc);
    kiln_sim_bind(&r->sim, &r->ports);

    kiln_app_ports_t ports = {};
    ports.tc       = &r->ports.tc;
    ports.case_tc  = &r->ports.case_tc;
    ports.heat     = &r->ports.heat;
    ports.current  = &r->ports.current;
    ports.counters = &r->ports.counters;

    kiln_config_t cfg;
    kiln_config_defaults(&cfg);
    cfg.kp             = 6.0f;
    cfg.ki             = 0.02f;
    cfg.kd             = 30.0f;
    cfg.filter_tau_s   = 1.0f;
    cfg.holdback_band_c = 0.0f;     /* most tests want the curve to run */

    CHECK_OK(kiln_app_init(&r->app, &ports, &cfg));
}

/* Advance everything by one window tick, firing the slower cycles on their own
 * periods.  Deliberately in period order: acquire outranks control so a control
 * cycle always sees a fresh sample (architecture section 6.1). */
static void rig_step(rig_t *r)
{
    kiln_sim_step(&r->sim, (float)WINDOW_DT);
    r->t_s += WINDOW_DT;

    kiln_app_window_tick(&r->app, (uint32_t)(WINDOW_DT * 1000.0));

    if (r->t_s >= r->next_acquire_s) {
        kiln_app_acquire_cycle(&r->app, (float)ACQUIRE_DT);
        r->next_acquire_s += ACQUIRE_DT;
    }
    if (r->t_s >= r->next_safety_s) {
        kiln_app_safety_cycle(&r->app, (float)SAFETY_DT);
        r->next_safety_s += SAFETY_DT;
    }
    if (r->t_s >= r->next_control_s) {
        kiln_app_control_cycle(&r->app, (float)CONTROL_DT);
        r->next_control_s += CONTROL_DT;
    }
}

static void rig_run(rig_t *r, double seconds)
{
    const int steps = (int)(seconds / WINDOW_DT);
    for (int i = 0; i < steps; i++) {
        rig_step(r);
    }
}

/* Run until a fault latches, or the budget runs out. */
static kiln_fault_t rig_run_until_fault(rig_t *r, double max_seconds)
{
    const int steps = (int)(max_seconds / WINDOW_DT);
    for (int i = 0; i < steps; i++) {
        rig_step(r);
        if (r->app.fault != KILN_FAULT_NONE) {
            return r->app.fault;
        }
    }
    return KILN_FAULT_NONE;
}

/* Run until the run controller leaves the running state, and report where the
 * kiln was at that moment rather than wherever it has drifted to since. */
static bool rig_run_until_not_running(rig_t *r, double max_seconds)
{
    const int steps = (int)(max_seconds / WINDOW_DT);
    for (int i = 0; i < steps; i++) {
        rig_step(r);
        if (r->app.state != KILN_STATE_RUNNING) {
            return true;
        }
    }
    return false;
}

static kiln_program_t simple_program(uint16_t target_c, uint16_t rate, uint16_t dwell_min)
{
    kiln_program_t p;
    kiln_profile_init_empty(&p, "integration");
    p.segment_count            = 1;
    p.segments[0].target_c     = target_c;
    p.segments[0].rate_c_per_h = rate;
    p.segments[0].dwell_min    = dwell_min;
    return p;
}

/* --- the happy path ----------------------------------------------------- */

KILN_TEST(a_program_runs_closed_loop_against_the_simulator)
{
    rig_t r;
    rig_init(&r, 20.0f);
    rig_run(&r, 2.0);                 /* let acquisition answer */

    const kiln_program_t p = simple_program(300, 3600, 1);
    CHECK_OK(kiln_app_start(&r.app, &p));
    CHECK_EQ_INT(r.app.state, KILN_STATE_RUNNING);

    /* 20 -> 300 degC at 3600 degC/h is 280 s, plus a minute of dwell -- which
     * SWR-CTL-12 only accrues while the kiln is actually within tolerance, so
     * reaching Complete at all is the substance of the test. */
    CHECK(rig_run_until_not_running(&r, 1200.0));

    CHECK_EQ_INT(r.app.fault, KILN_FAULT_NONE);
    CHECK_EQ_INT(r.app.state, KILN_STATE_COMPLETE);
    /* Measured at completion: afterwards heat is off and it cools. */
    CHECK_NEAR(kiln_sim_temperature(&r.sim), 300.0f, 15.0f);
}

KILN_TEST(the_setpoint_is_tracked_through_the_ramp_not_just_at_the_end)
{
    rig_t r;
    rig_init(&r, 20.0f);
    rig_run(&r, 2.0);

    const kiln_program_t p = simple_program(400, 1800, 0);
    CHECK_OK(kiln_app_start(&r.app, &p));

    float worst = 0.0f;
    for (int i = 0; i < 60000; i++) {
        rig_step(&r);
        if (r.app.state != KILN_STATE_RUNNING) {
            break;
        }
        /* Skip the first minute, where the dead time dominates. */
        if (r.t_s < 70.0) {
            continue;
        }
        const float err = fabsf(r.app.kiln_c - kiln_setpoint_value(&r.app.sp));
        if (err > worst) {
            worst = err;
        }
    }
    CHECK_MSG(worst < 25.0f, "worst tracking error was %.1f degC", (double)worst);
}

/*
 * @relation(SWR-CUR-08, scope=function)
 */
KILN_TEST(swrcur08_a_reference_current_is_learned_during_the_cold_climb)
{
    rig_t r;
    rig_init(&r, 20.0f);
    rig_run(&r, 2.0);

    /* Rate 0 means full duty while it climbs, which is the cold full-on
     * condition SWR-CUR-08 learns from. */
    const kiln_program_t p = simple_program(600, 0, 1);
    CHECK_OK(kiln_app_start(&r.app, &p));
    rig_run(&r, 120.0);

    CHECK(kiln_current_ref_valid(&r.app.cur));
    /* The simulator's nominal is 30 A at ambient. */
    CHECK_NEAR(kiln_current_ref(&r.app.cur), 30.0f, 2.5f);
    CHECK_NEAR(r.app.record.current_ref_a, kiln_current_ref(&r.app.cur), 0.001f);

    /* SWR-CUR-07: energy accumulated while it was heating. */
    CHECK(kiln_current_energy_wh(&r.app.cur) > 0.0);
}

/*
 * @relation(SWR-CUR-04, scope=function)
 */
KILN_TEST(swrcur04_measurements_are_gated_to_the_commanded_window)
{
    rig_t r;
    rig_init(&r, 20.0f);
    rig_run(&r, 2.0);

    const kiln_program_t p = simple_program(500, 600, 10);
    CHECK_OK(kiln_app_start(&r.app, &p));

    /* Climb to where the PID is modulating rather than saturated. */
    rig_run(&r, 400.0);

    int conduction = 0, leakage = 0;
    for (int i = 0; i < 100000; i++) {
        rig_step(&r);
        if (r.app.cur_fresh) {
            const uint8_t f = kiln_current_flags(&r.app.cur);
            if ((f & KILN_CURF_CONDUCTION) != 0u) {
                conduction++;
            }
            if ((f & KILN_CURF_LEAKAGE) != 0u) {
                leakage++;
            }
        }
        if (conduction > 20 && leakage > 20) {
            break;
        }
    }
    /* Both kinds of window get measured, which is the whole point of SWA-17: a
     * blind average would read a few percent of full current and say nothing. */
    CHECK(conduction > 20);
    CHECK(leakage > 20);
}

/* --- electrical fault injection, SWR-TST-27 ---------------------------------- */

/*
 * @relation(SWR-SAF-25, scope=function)
 */
KILN_TEST(swrsaf25_a_relay_stuck_on_cannot_pass_current_while_the_contactor_is_open)
{
    /* Worth asserting rather than assuming: at idle the contactor is open, so a
     * stuck SSR passes nothing and there is nothing for SWR-SAF-25 to find -- and
     * nothing unsafe either, because the same open contactor is what stops the
     * kiln heating.  This is the defence in depth of architecture section 8.1:
     * two interrupting devices in series, and one of them is enough. */
    rig_t r;
    rig_init(&r, 20.0f);
    rig_run(&r, 2.0);

    kiln_sim_inject(&r.sim, KILN_INJ_RELAY_FAIL_ON);
    CHECK(!kiln_sim_contactor(&r.sim));

    CHECK_EQ_INT(rig_run_until_fault(&r, 30.0), KILN_FAULT_NONE);
    CHECK_NEAR(kiln_sim_current(&r.sim), 0.0f, 0.05f);
    CHECK_NEAR(kiln_sim_temperature(&r.sim), 20.0f, 1.0f);
}

/*
 * @relation(SWR-SAF-25, scope=function)
 */
KILN_TEST(swrsaf25_a_relay_stuck_on_is_caught_during_a_firing)
{
    /* The case that matters: the contactor is closed because the kiln is meant
     * to be firing, and the SSR has stopped modulating.  SWR-SAF-25 sees amps in a
     * commanded-off window; SWR-SAF-08 would be waiting on five degrees of rise over
     * three minutes, and only once duty had been zero for a full minute. */
    rig_t r;
    rig_init(&r, 20.0f);
    rig_run(&r, 2.0);

    const kiln_program_t p = simple_program(600, 0, 10);
    CHECK_OK(kiln_app_start(&r.app, &p));
    rig_run(&r, 10.0);
    CHECK(kiln_sim_contactor(&r.sim));

    kiln_sim_inject(&r.sim, KILN_INJ_RELAY_FAIL_ON);

    const double t0 = r.t_s;
    const kiln_fault_t f = rig_run_until_fault(&r, 60.0);
    const double took = r.t_s - t0;

    /* Either verdict is right for this injection: the simulator's fail-on is an
     * SSR that conducts, and whether dropping the contactor clears it is exactly
     * what SWR-SAF-27 goes on to establish. */
    CHECK_MSG(f == KILN_FAULT_UNCOMMANDED_CURRENT || f == KILN_FAULT_CONTACTOR_WELDED,
              "got fault %u", (unsigned)f);
    CHECK_MSG(took < 15.0, "SWR-SAF-25 took %.1f s", took);
    CHECK_EQ_INT(r.app.state, KILN_STATE_FAULT);
    CHECK(!r.app.heat_authorised);
}

/*
 * @relation(SWR-SAF-27, scope=function)
 */
KILN_TEST(swrsaf27_discriminates_a_welded_contactor_from_a_shorted_ssr)
{
    /* The same symptom, two very different instructions to the operator. */
    rig_t ssr, weld;

    rig_init(&ssr, 20.0f);
    rig_run(&ssr, 2.0);
    {
        /* The contactor has to be closed for a shorted SSR to pass anything, so
         * the kiln has to be firing.  SWR-CTL-08's quantiser preserves a
         * measurable off interval even at saturation precisely so that the
         * leakage measurement still exists here. */
        const kiln_program_t p = simple_program(600, 0, 10);
        CHECK_OK(kiln_app_start(&ssr.app, &p));
        rig_run(&ssr, 10.0);
    }
    kiln_sim_inject(&ssr.sim, KILN_INJ_SSR_SHORTED);
    CHECK_EQ_INT(rig_run_until_fault(&ssr, 60.0), KILN_FAULT_UNCOMMANDED_CURRENT);
    /* The contactor did its job: it opened. */
    CHECK(!kiln_sim_contactor(&ssr.sim));

    rig_init(&weld, 20.0f);
    rig_run(&weld, 2.0);
    kiln_sim_inject(&weld.sim, KILN_INJ_SSR_SHORTED | KILN_INJ_CONTACTOR_WELD);
    CHECK_EQ_INT(rig_run_until_fault(&weld, 60.0), KILN_FAULT_CONTACTOR_WELDED);
    /* And here it did not, which is what the sequence found out. */
    CHECK(kiln_sim_contactor(&weld.sim));

    /* SWR-SAF-27's fault is not clearable, so the operator cannot acknowledge their
     * way back to a running kiln. */
    CHECK_ERR(kiln_app_clear_fault(&weld.app), KILN_ERR_STATE);
    CHECK_EQ_INT(weld.app.fault, KILN_FAULT_CONTACTOR_WELDED);
}

/*
 * @relation(SWR-SAF-26, scope=function)
 */
KILN_TEST(swrsaf26_relay_fail_off_is_caught_long_before_the_thermal_backstop)
{
    rig_t r;
    rig_init(&r, 20.0f);
    rig_run(&r, 2.0);

    const kiln_program_t p = simple_program(600, 0, 10);
    CHECK_OK(kiln_app_start(&r.app, &p));

    /* No current reaches the elements, from the first on-window. */
    kiln_sim_inject(&r.sim, KILN_INJ_RELAY_FAIL_OFF);

    const double t0 = r.t_s;
    const kiln_fault_t f = rig_run_until_fault(&r, 300.0);
    const double took = r.t_s - t0;

    CHECK_EQ_INT(f, KILN_FAULT_NO_HEATER_CURRENT);
    /* SWR-SAF-26's 30 s window against SWR-SAF-07's 15 min: the electrical rule is the
     * reason requirements section 5.2 calls it the primary detection. */
    CHECK_MSG(took < 60.0, "SWR-SAF-26 took %.1f s", took);
}

/*
 * @relation(SWR-SAF-26, scope=function)
 */
KILN_TEST(swrsaf26_also_catches_fully_open_elements)
{
    rig_t r;
    rig_init(&r, 20.0f);
    rig_run(&r, 2.0);

    const kiln_program_t p = simple_program(600, 0, 10);
    CHECK_OK(kiln_app_start(&r.app, &p));
    kiln_sim_inject(&r.sim, KILN_INJ_ELEMENT_OPEN);

    CHECK_EQ_INT(rig_run_until_fault(&r, 300.0), KILN_FAULT_NO_HEATER_CURRENT);
}

/*
 * @relation(SWR-SAF-29, scope=function)
 */
KILN_TEST(swrsaf29_overcurrent_is_caught)
{
    rig_t r;
    rig_init(&r, 20.0f);
    rig_run(&r, 2.0);

    const kiln_program_t p = simple_program(600, 0, 10);
    CHECK_OK(kiln_app_start(&r.app, &p));
    rig_run(&r, 5.0);

    kiln_sim_inject(&r.sim, KILN_INJ_OVERCURRENT);
    CHECK_EQ_INT(rig_run_until_fault(&r, 60.0), KILN_FAULT_OVERCURRENT);
}

/*
 * @relation(SWR-CUR-11, scope=function)
 */
KILN_TEST(swrcur11_a_disconnected_transformer_is_detected_and_refuses_a_start)
{
    rig_t r;
    rig_init(&r, 20.0f);
    rig_run(&r, 2.0);

    kiln_sim_inject(&r.sim, KILN_INJ_CT_DISCONNECTED);

    /* SWR-CUR-12: no firing starts without the monitoring it is configured for. */
    const kiln_program_t p = simple_program(600, 600, 0);
    CHECK_ERR(kiln_app_start(&r.app, &p), KILN_ERR_STATE);

    /* And the fault latches on its own, from idle -- fault 26, not warning 111:
     * an absent transformer is a hardware failure, not an operator's decision to
     * fire without electrical cover. */
    CHECK_EQ_INT(rig_run_until_fault(&r, 60.0), KILN_FAULT_CT_FAULT);
    CHECK(!(r.app.warnings & KILN_WARN_BIT(KILN_WARN_CURRENT_OFF)));
}

/*
 * @relation(SWR-CUR-12, scope=function)
 */
KILN_TEST(swrcur12_a_run_may_start_without_monitoring_when_it_is_disabled)
{
    rig_t r;
    rig_init(&r, 20.0f);

    kiln_config_t cfg = r.app.cfg;
    cfg.current_enabled = false;
    const kiln_cfg_item_t *bad = NULL;
    CHECK_OK(kiln_app_apply_config(&r.app, &cfg, &bad));

    rig_run(&r, 2.0);
    kiln_sim_inject(&r.sim, KILN_INJ_CT_DISCONNECTED);

    /* Explicitly disabled: the operator has accepted the thermal backstop, and
     * warning 111 says so for as long as it lasts. */
    const kiln_program_t p = simple_program(300, 3600, 0);
    CHECK_OK(kiln_app_start(&r.app, &p));
    rig_run(&r, 10.0);
    CHECK(r.app.warnings & KILN_WARN_BIT(KILN_WARN_CURRENT_OFF));
    CHECK_EQ_INT(r.app.fault, KILN_FAULT_NONE);
}

/*
 * @relation(SWR-SAF-28, scope=function)
 */
KILN_TEST(swrsaf28_partial_element_failure_shows_up_in_the_current)
{
    rig_t r;
    rig_init(&r, 20.0f);
    rig_run(&r, 2.0);

    const kiln_program_t p = simple_program(600, 0, 30);
    CHECK_OK(kiln_app_start(&r.app, &p));

    /* Learn the reference first, cold and fully on. */
    rig_run(&r, 120.0);
    CHECK(kiln_current_ref_valid(&r.app.cur));

    /* Then lose one of three element groups: a step of a known fraction. */
    kiln_sim_inject(&r.sim, KILN_INJ_ELEMENT_PARTIAL);

    bool warned = false;
    for (int i = 0; i < 200000; i++) {
        rig_step(&r);
        if ((r.app.warnings & KILN_WARN_BIT(KILN_WARN_CURRENT_DEV)) != 0u) {
            warned = true;
        }
        if (r.app.fault != KILN_FAULT_NONE) {
            break;
        }
    }
    /* A third is over the 25 % fault band, so this ends as fault 24 -- having
     * warned on the way there. */
    CHECK(warned);
    CHECK(r.app.fault == KILN_FAULT_CURRENT_DEVIATION ||
          r.app.fault == KILN_FAULT_NO_HEATER_CURRENT);
}

/* --- sensing and thermal injections ------------------------------------ */

/*
 * @relation(SWR-SAF-04, scope=function)
 */
KILN_TEST(swrsaf04_an_open_thermocouple_latches_after_the_grace_period)
{
    rig_t r;
    rig_init(&r, 20.0f);
    rig_run(&r, 2.0);

    const kiln_program_t p = simple_program(600, 600, 0);
    CHECK_OK(kiln_app_start(&r.app, &p));
    rig_run(&r, 5.0);

    kiln_sim_inject(&r.sim, KILN_INJ_TC_OPEN);
    CHECK_EQ_INT(rig_run_until_fault(&r, 30.0), KILN_FAULT_TC_OPEN);
    CHECK(!r.app.heat_authorised);

    /* SWR-SAF-18: not clearable while the front end is still reporting it. */
    CHECK_ERR(kiln_app_clear_fault(&r.app), KILN_ERR_STATE);
    kiln_sim_clear(&r.sim, KILN_INJ_TC_OPEN);
    rig_run(&r, 1.0);
    CHECK_OK(kiln_app_clear_fault(&r.app));
    CHECK_EQ_INT(r.app.state, KILN_STATE_IDLE);
}

/*
 * @relation(SWR-SAF-05, scope=function)
 */
KILN_TEST(swrsaf05_a_reversed_thermocouple_is_caught)
{
    rig_t r;
    rig_init(&r, 20.0f);
    rig_run(&r, 2.0);

    /* A dwell long enough that the run is still going when the probe reverses:
     * rate 0 means the setpoint steps to the target, so a zero dwell would see
     * the program complete within a couple of cycles. */
    const kiln_program_t p = simple_program(600, 0, 20);
    CHECK_OK(kiln_app_start(&r.app, &p));
    rig_run(&r, 30.0);

    kiln_sim_inject(&r.sim, KILN_INJ_TC_REVERSED);
    /* The drop has to persist for reversed_confirm_s, which a reversed couple's
     * does: the reading goes the wrong way and stays there. */
    CHECK_EQ_INT(rig_run_until_fault(&r, 600.0), KILN_FAULT_TC_REVERSED);
}

/*
 * @relation(SWR-SAF-11, scope=function)
 */
KILN_TEST(swrsaf11_an_over_hot_enclosure_stops_the_firing)
{
    rig_t r;
    rig_init(&r, 20.0f);
    rig_run(&r, 2.0);

    const kiln_program_t p = simple_program(600, 0, 10);
    CHECK_OK(kiln_app_start(&r.app, &p));
    rig_run(&r, 5.0);

    kiln_sim_inject(&r.sim, KILN_INJ_CASE_HEATING);
    /* The enclosure's time constant is four times the chamber's, so this takes a
     * while in simulated time -- and nothing in wall time (SWA-02). */
    CHECK_EQ_INT(rig_run_until_fault(&r, 6000.0), KILN_FAULT_CASE_OVERTEMP);
}

/*
 * @relation(SWR-SAF-09, scope=function)
 */
KILN_TEST(swrsaf09_the_configured_maximum_is_not_exceeded_by_the_margin)
{
    rig_t r;
    rig_init(&r, 20.0f);

    kiln_config_t cfg = r.app.cfg;
    cfg.max_temp_c = 200.0f;
    const kiln_cfg_item_t *bad = NULL;
    CHECK_OK(kiln_app_apply_config(&r.app, &cfg, &bad));
    rig_run(&r, 2.0);

    /* Drive hard in manual, which is the state that can ask for more than the
     * program validator would ever allow. */
    CHECK_OK(kiln_app_manual(&r.app, KILN_DUTY_MAX));

    float peak = 0.0f;
    for (int i = 0; i < 400000; i++) {
        rig_step(&r);
        if (r.app.kiln_c > peak) {
            peak = r.app.kiln_c;
        }
        if (r.app.fault != KILN_FAULT_NONE) {
            break;
        }
    }
    /* Heat is withheld at the limit; the fault only comes at the margin, and the
     * plant's own lag is what decides which happens.  Either way it stops. */
    CHECK_MSG(peak < 200.0f + cfg.overtemp_margin_c + 25.0f,
              "peak was %.1f degC against a 200 degC limit", (double)peak);
    CHECK(!r.app.heat_authorised);
}

/* --- run control -------------------------------------------------------- */

/*
 * @relation(SWR-RUN-03, scope=function)
 */
KILN_TEST(swrrun03_pause_freezes_the_program_and_stops_the_heat)
{
    rig_t r;
    rig_init(&r, 20.0f);
    rig_run(&r, 2.0);

    const kiln_program_t p = simple_program(600, 600, 0);
    CHECK_OK(kiln_app_start(&r.app, &p));
    rig_run(&r, 60.0);

    const float sp = kiln_setpoint_value(&r.app.sp);
    CHECK_OK(kiln_app_pause(&r.app));
    CHECK_EQ_INT(r.app.state, KILN_STATE_PAUSED);

    rig_run(&r, 60.0);
    CHECK_NEAR(kiln_setpoint_value(&r.app.sp), sp, 0.001f);
    CHECK(!r.app.heat_authorised);
    CHECK_EQ_UINT(r.app.duty_request, 0u);

    CHECK_OK(kiln_app_resume(&r.app));
    rig_run(&r, 30.0);
    CHECK(kiln_setpoint_value(&r.app.sp) > sp);
}

/*
 * @relation(SWR-RUN-04, scope=function)
 */
KILN_TEST(swrrun04_abort_de_energises_promptly_from_any_state)
{
    rig_t r;
    rig_init(&r, 20.0f);
    rig_run(&r, 2.0);

    const kiln_program_t p = simple_program(600, 0, 10);
    CHECK_OK(kiln_app_start(&r.app, &p));
    rig_run(&r, 30.0);
    CHECK(r.app.heat_authorised);

    CHECK_OK(kiln_app_abort(&r.app));
    /* SWR-RUN-04 allows 1 s; the authority is withdrawn in the call itself. */
    CHECK(!r.app.heat_authorised);
    CHECK_EQ_UINT(r.app.duty_request, 0u);
    CHECK_EQ_INT(r.app.state, KILN_STATE_IDLE);
    CHECK_EQ_INT(r.app.record.end_reason, KILN_END_OPERATOR_ABORT);

    rig_run(&r, 2.0);
    CHECK(!kiln_sim_contactor(&r.sim));
}

/*
 * @relation(SWR-RUN-10, scope=function)
 */
KILN_TEST(swrrun10_a_run_is_refused_while_a_fault_is_latched)
{
    rig_t r;
    rig_init(&r, 20.0f);
    rig_run(&r, 2.0);

    kiln_sim_inject(&r.sim, KILN_INJ_TC_OPEN);
    CHECK(rig_run_until_fault(&r, 30.0) != KILN_FAULT_NONE);

    const kiln_program_t p = simple_program(600, 600, 0);
    CHECK_ERR(kiln_app_start(&r.app, &p), KILN_ERR_STATE);
    CHECK_ERR(kiln_app_manual(&r.app, 500), KILN_ERR_STATE);
    CHECK_ERR(kiln_app_autotune(&r.app, 600.0f), KILN_ERR_STATE);
}

/*
 * @relation(SWR-RUN-02, scope=function)
 */
KILN_TEST(swrrun02_a_run_is_refused_while_the_chamber_channel_is_silent)
{
    rig_t r;
    rig_init(&r, 20.0f);
    kiln_sim_inject(&r.sim, KILN_INJ_TC_COMMS);
    rig_run(&r, 2.0);

    const kiln_program_t p = simple_program(600, 600, 0);
    CHECK_ERR(kiln_app_start(&r.app, &p), KILN_ERR_STATE);
}

/*
 * @relation(SWR-PRG-05, scope=function)
 */
KILN_TEST(swrprg05_an_invalid_program_is_refused_at_the_start_gate)
{
    rig_t r;
    rig_init(&r, 20.0f);
    rig_run(&r, 2.0);

    kiln_program_t p = simple_program(600, 600, 0);
    p.segments[0].target_c = 1340;      /* above the configured 1280 */
    CHECK_ERR(kiln_app_start(&r.app, &p), KILN_ERR_RANGE);

    p = simple_program(600, 600, 0);
    p.name[0] = '\0';
    CHECK_ERR(kiln_app_start(&r.app, &p), KILN_ERR_RANGE);
}

/*
 * @relation(SWR-CTL-13, scope=function)
 */
KILN_TEST(swrctl13_a_cooling_segment_is_executed_passively)
{
    rig_t r;
    rig_init(&r, 20.0f);
    rig_run(&r, 2.0);

    kiln_program_t p;
    kiln_profile_init_empty(&p, "cool");
    p.segment_count = 2;
    p.segments[0].target_c = 300;  p.segments[0].rate_c_per_h = 3600;
    p.segments[1].target_c = 100;  p.segments[1].rate_c_per_h = 3600;

    CHECK_OK(kiln_app_start(&r.app, &p));

    bool saw_cooling = false;
    for (int i = 0; i < 300000; i++) {
        rig_step(&r);
        if (r.app.state != KILN_STATE_RUNNING) {
            break;
        }
        if (kiln_setpoint_segment(&r.app.sp) == 1 &&
            r.app.sp.phase == KILN_SP_PHASE_RAMP) {
            saw_cooling = true;
            /* No duty is requested during a cooling ramp: the kiln cools as fast
             * as it cools, and the heaters play no part. */
            CHECK_EQ_UINT(r.app.duty_request, 0u);
            CHECK(!r.app.heat_allowed);
        }
    }
    CHECK(saw_cooling);
}

/*
 * @relation(SWR-CUR-13, scope=function)
 */
KILN_TEST(swrcur13_switching_operations_are_counted_across_a_firing)
{
    rig_t r;
    rig_init(&r, 20.0f);
    rig_run(&r, 2.0);

    const kiln_program_t p = simple_program(300, 600, 5);
    CHECK_OK(kiln_app_start(&r.app, &p));
    rig_run(&r, 400.0);

    kiln_switch_counters_t c = {};
    CHECK_OK(r.ports.counters.load(r.ports.counters.ctx, &c));
    CHECK(c.ssr_ops[0] > 10u);

    /* And the figure reaches the run record (SWR-RUN-07). */
    CHECK_OK(kiln_app_abort(&r.app));
    CHECK(r.app.record.ssr_ops[0] > 0u);
}

/*
 * @relation(SWR-RUN-06, scope=function)
 */
KILN_TEST(swrrun06_completion_sounds_the_alarm_for_its_configured_duration)
{
    rig_t r;
    rig_init(&r, 20.0f);
    rig_run(&r, 2.0);

    const kiln_program_t p = simple_program(200, 3600, 0);
    CHECK_OK(kiln_app_start(&r.app, &p));
    CHECK(rig_run_until_not_running(&r, 1200.0));

    CHECK_EQ_INT(r.app.state, KILN_STATE_COMPLETE);
    CHECK(r.app.complete_pending);
    CHECK_EQ_INT(r.app.record.end_reason, KILN_END_COMPLETE);
    CHECK(!r.app.heat_authorised);

    /* The default is 30 s. */
    rig_run(&r, 40.0);
    CHECK(!r.app.complete_pending);
    CHECK_OK(kiln_app_idle(&r.app));
}

/*
 * @relation(SWA-05, scope=function)
 */
KILN_TEST(swa05_a_stopped_safety_cycle_releases_the_contactor)
{
    /* The central safety property of the whole design: the coil is held up by a
     * *repeated* call into the charge pump, so a crashed, hung or deadlocked
     * safety task releases it with no code involved.  The simulator models the
     * decay as a timeout -- whether the real circuit reaches the contactor's
     * drop-out voltage inside SWR-NFR-04's one second is a question about resistors,
     * and belongs on the HIL jig (SWR-TST-17, tasklist A7). */
    rig_t r;
    rig_init(&r, 20.0f);
    rig_run(&r, 2.0);

    const kiln_program_t p = simple_program(600, 0, 10);
    CHECK_OK(kiln_app_start(&r.app, &p));
    rig_run(&r, 5.0);
    CHECK(kiln_sim_contactor(&r.sim));

    /* Now stop calling the safety cycle, and nothing else: the control task
     * keeps asking for duty, the window keeps ticking, the plant keeps running. */
    for (int i = 0; i < 300; i++) {
        kiln_sim_step(&r.sim, (float)WINDOW_DT);
        r.t_s += WINDOW_DT;
        kiln_app_window_tick(&r.app, (uint32_t)(WINDOW_DT * 1000.0));
        kiln_app_acquire_cycle(&r.app, (float)WINDOW_DT);
    }

    CHECK(!kiln_sim_contactor(&r.sim));
    CHECK_NEAR(kiln_sim_current(&r.sim), 0.0f, 0.05f);
}

/*
 * @relation(SWR-SAF-16, scope=function)
 */
KILN_TEST(swrsaf16_withdrawing_authority_de_energises_without_waiting_for_a_window_edge)
{
    rig_t r;
    rig_init(&r, 20.0f);
    rig_run(&r, 2.0);

    const kiln_program_t p = simple_program(600, 0, 10);
    CHECK_OK(kiln_app_start(&r.app, &p));
    rig_run(&r, 5.0);
    CHECK(r.app.heat_authorised);

    CHECK_OK(kiln_app_abort(&r.app));
    rig_run(&r, 0.5);
    CHECK(!kiln_sim_contactor(&r.sim));
    CHECK_NEAR(kiln_sim_current(&r.sim), 0.0f, 0.05f);
}

/*
 * @relation(SWR-CFG-08, scope=function)
 */
KILN_TEST(swrcfg08_safety_configuration_cannot_change_during_a_firing)
{
    rig_t r;
    rig_init(&r, 20.0f);
    rig_run(&r, 2.0);

    const kiln_program_t p = simple_program(600, 600, 0);
    CHECK_OK(kiln_app_start(&r.app, &p));

    kiln_config_t cfg = r.app.cfg;
    cfg.max_temp_c = 600.0f;
    const kiln_cfg_item_t *bad = NULL;
    CHECK_ERR(kiln_app_apply_config(&r.app, &cfg, &bad), KILN_ERR_STATE);
    CHECK(bad != NULL);
    CHECK_NEAR(r.app.cfg.max_temp_c, 1280.0f, 0.01f);
}

/* --- an accelerated soak ------------------------------------------------ */

KILN_TEST(a_multi_segment_firing_completes_without_a_spurious_fault)
{
    /* The thing that matters most about the rule set as a whole: a kiln that is
     * working must be able to complete a firing.  A safety rule that trips on a
     * healthy plant is worse than none, because it gets switched off. */
    rig_t r;
    rig_init(&r, 20.0f);
    rig_run(&r, 2.0);

    kiln_program_t p;
    kiln_profile_init_empty(&p, "soak");
    p.segment_count = 4;
    p.segments[0].target_c = 120; p.segments[0].rate_c_per_h = 1800;
    p.segments[0].dwell_min = 1;
    p.segments[1].target_c = 500; p.segments[1].rate_c_per_h = 3600;
    p.segments[2].target_c = 800; p.segments[2].rate_c_per_h = 2400;
    p.segments[2].dwell_min = 2;
    p.segments[3].target_c = 200; p.segments[3].rate_c_per_h = 3600;

    CHECK_OK(kiln_app_start(&r.app, &p));

    for (int i = 0; i < 1500000; i++) {
        rig_step(&r);
        if (r.app.state != KILN_STATE_RUNNING) {
            break;
        }
    }

    CHECK_MSG(r.app.fault == KILN_FAULT_NONE, "spurious fault %u %s",
              (unsigned)r.app.fault, kiln_fault_label(r.app.fault));
    CHECK_EQ_INT(r.app.state, KILN_STATE_COMPLETE);
    CHECK(r.app.record.peak_c > 750.0f);
    CHECK(kiln_current_energy_wh(&r.app.cur) > 0.0);
}

/* ===========================================================================
 * THE SUPERVISOR LINK, END TO END  (SWA-22, tasklist R8)
 * ===========================================================================
 * The pieces were each tested and the chain was not. test_suplink proves that
 * a quiet link and a stale sequence number both become KILN_TC_FAULT_COMMS;
 * test_safety_thermal proves that bit becomes KILN_FAULT_TC_COMMS; other tests
 * prove a latched fault withholds heat. What R8 actually asks for is the
 * property those three add up to, and an assembly of proven parts is not a
 * proven assembly: the chamber temperature enters this firmware at exactly one
 * place, over a wire, and if silence on that wire does not stop the heater then
 * nothing else in the safety argument matters.
 *
 * So these two run the real app with the real link bound as the chamber port,
 * the way main.cpp binds it on the board, and watch the heater.
 */

namespace {

/* The chamber port, as the product has it: a decoder fed bytes from a UART. */
typedef struct {
    kiln_suplink_t   link;
    kiln_port_tc_t   tc;
} suplink_rig_t;

sup_report_t sup_rep(uint8_t seq, float c)
{
    sup_report_t r = {};
    r.version     = SUP_VERSION;
    r.seq         = seq;
    r.chamber_dc  = (int16_t)lroundf(c * (float)SUP_DC_PER_C);
    r.cj_dc       = (int16_t)(25 * SUP_DC_PER_C);
    r.fault_bits  = 0u;
    r.flags = (uint8_t)(SUP_FLAG_PERMIT | SUP_FLAG_TC_VALID | SUP_FLAG_SELFTEST_OK);
    r.trip_reason = SUP_TRIP_NONE;
    return r;
}

void sup_send(kiln_suplink_t *s, const sup_report_t &r)
{
    uint8_t f[SUP_FRAME_BYTES];
    (void)sup_encode(&r, f, sizeof(f));
    kiln_suplink_feed(s, f, sizeof(f));
}

/* One acquisition period's worth of link service, which on the board is the
 * UART draining into the decoder and kiln_suplink_tick ageing it. */
void sup_service(suplink_rig_t *sl, rig_t *r, uint8_t seq, bool speak)
{
    if (speak) {
        sup_send(&sl->link, sup_rep(seq, (float)kiln_sim_temperature(&r->sim)));
    }
    kiln_suplink_tick(&sl->link, (float)ACQUIRE_DT);
}

} // namespace

/*
 * @relation(SWA-22, scope=function)
 */
KILN_TEST(swa22_silence_on_the_link_takes_the_heat_away)
{
    static rig_t r;
    static suplink_rig_t sl;
    rig_init(&r, 20.0f);

    memset(&sl, 0, sizeof(sl));
    kiln_suplink_init(&sl.link);
    kiln_suplink_bind(&sl.link, &sl.tc);

    /* Rebind the chamber port to the link, which is what the board does: the
     * simulated plant still provides the physics, and the temperature reaches
     * the core only by way of a frame. */
    kiln_app_ports_t ports = {};
    ports.tc       = &sl.tc;
    ports.case_tc  = &r.ports.case_tc;
    ports.heat     = &r.ports.heat;
    ports.current  = &r.ports.current;
    ports.counters = &r.ports.counters;
    kiln_config_t cfg;
    kiln_config_defaults(&cfg);
    cfg.kp = 6.0f; cfg.ki = 0.02f; cfg.kd = 30.0f;
    cfg.filter_tau_s = 1.0f;
    cfg.holdback_band_c = 0.0f;
    CHECK_OK(kiln_app_init(&r.app, &ports, &cfg));

    kiln_program_t prog;
    CHECK_OK(kiln_profile_example(0, &prog));

    /* Let acquisition answer first, which over this port means letting a few
     * frames arrive: a start is refused while the chamber has no reading, and
     * on this device a reading is a frame. */
    uint8_t seq = 1;
    for (int i = 0; i < 200; i++) {
        rig_step(&r);
        if ((i % 25) == 0) { sup_service(&sl, &r, seq++, true); }
    }
    CHECK_OK(kiln_app_start(&r.app, &prog));

    /* A talking link: the kiln heats. Asserted, because a test where the heat
     * never came on would pass the interesting half for the wrong reason. */
    for (int i = 0; i < 400; i++) {
        rig_step(&r);
        if ((i % 25) == 0) { sup_service(&sl, &r, seq++, true); }
    }
    CHECK_MSG(r.app.heat_authorised, "the kiln never started heating");
    CHECK_EQ_INT(r.app.fault, KILN_FAULT_NONE);

    /* Now the supervisor stops talking: the board is pulled, the wire breaks,
     * the other processor hangs. Nothing announces it. */
    bool heat_seen = false;
    kiln_fault_t fault = KILN_FAULT_NONE;
    for (int i = 0; i < 4000; i++) {
        rig_step(&r);
        if ((i % 25) == 0) { sup_service(&sl, &r, seq++, false); }
        if (r.app.fault != KILN_FAULT_NONE) { fault = r.app.fault; break; }
        if (r.app.heat_authorised) { heat_seen = true; }
    }

    /* SWR-SAF-04's grace, then a comms fault: not a temperature of zero, not
     * the last reading held for ever. */
    CHECK_EQ_INT(fault, KILN_FAULT_TC_COMMS);
    CHECK(heat_seen);                       /* it was heating before */
    CHECK(!r.app.heat_authorised);          /* and is not now */

    /* And the contactor is open, which is the part the kiln cares about. */
    rig_run(&r, 1.0);
    CHECK(!kiln_sim_contactor(&r.sim));
}

/*
 * @relation(SWA-22, scope=function)
 */
KILN_TEST(swa22_a_supervisor_repeating_itself_takes_the_heat_away_too)
{
    /* The failure that looks like health: frames keep arriving, the CRC is
     * good, the temperature is plausible, and the sequence number never
     * advances -- a supervisor stuck in a loop, or a transmit buffer being
     * replayed. A link check that only counted bytes would see a healthy
     * channel and let the kiln run on a reading from minutes ago.
     */
    static rig_t r;
    static suplink_rig_t sl;
    rig_init(&r, 20.0f);

    memset(&sl, 0, sizeof(sl));
    kiln_suplink_init(&sl.link);
    kiln_suplink_bind(&sl.link, &sl.tc);

    kiln_app_ports_t ports = {};
    ports.tc       = &sl.tc;
    ports.case_tc  = &r.ports.case_tc;
    ports.heat     = &r.ports.heat;
    ports.current  = &r.ports.current;
    ports.counters = &r.ports.counters;
    kiln_config_t cfg;
    kiln_config_defaults(&cfg);
    cfg.kp = 6.0f; cfg.ki = 0.02f; cfg.kd = 30.0f;
    cfg.filter_tau_s = 1.0f;
    cfg.holdback_band_c = 0.0f;
    CHECK_OK(kiln_app_init(&r.app, &ports, &cfg));

    kiln_program_t prog;
    CHECK_OK(kiln_profile_example(0, &prog));
    uint8_t seq = 1;
    for (int i = 0; i < 200; i++) {
        rig_step(&r);
        if ((i % 25) == 0) { sup_service(&sl, &r, seq++, true); }
    }
    CHECK_OK(kiln_app_start(&r.app, &prog));
    for (int i = 0; i < 400; i++) {
        rig_step(&r);
        if ((i % 25) == 0) { sup_service(&sl, &r, seq++, true); }
    }
    CHECK(r.app.heat_authorised);

    /* Frames keep coming, all of them seq 9, all of them well formed. */
    const uint8_t stuck = 9;
    kiln_fault_t fault = KILN_FAULT_NONE;
    const uint32_t frames_before = sl.link.frames;
    for (int i = 0; i < 4000; i++) {
        rig_step(&r);
        if ((i % 25) == 0) {
            sup_send(&sl.link, sup_rep(stuck, 600.0f));
            kiln_suplink_tick(&sl.link, (float)ACQUIRE_DT);
        }
        if (r.app.fault != KILN_FAULT_NONE) { fault = r.app.fault; break; }
    }

    CHECK_EQ_INT(fault, KILN_FAULT_TC_COMMS);
    CHECK(!r.app.heat_authorised);
    /* Frames really were arriving throughout: this is not the silence test
     * wearing a different hat. */
    CHECK(sl.link.frames > frames_before + 10u);

    rig_run(&r, 1.0);
    CHECK(!kiln_sim_contactor(&r.sim));
}

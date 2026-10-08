/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One static function per rule, each with its own accumulator, so that each row
 * of architecture section 8.2 maps to exactly one host test (SWR-TST-23).
 */

#include <math.h>
#include <string.h>
#include "kiln_core/safety.h"

void kiln_safety_cfg_defaults(kiln_safety_cfg_t *cfg)
{
    const kiln_safety_cfg_t d = {
        .max_temp_c               = 1280.0f,
        .overtemp_margin_c        = 10.0f,   /* SWR-SAF-09 */
        .max_case_temp_c          = 70.0f,   /* SWR-SAF-11 */
        .tc_grace_s               = 5.0f,    /* SWR-ACQ-12 */

        .reversed_duty_permille   = 500,     /* SWR-SAF-05 */
        .reversed_drop_c          = 5.0f,
        .reversed_window_s        = 120.0f,
        .reversed_confirm_s       = 30.0f,

        .stuck_delta_c            = 2.0f,    /* SWR-SAF-06 */
        .stuck_duty_permille      = 500,
        .stuck_window_s           = 600.0f,

        .runaway_duty_permille    = 800,     /* SWR-SAF-07 */
        .runaway_window_s         = 900.0f,
        .runaway_min_rate_c_per_h = 10.0f,

        .uncommanded_rise_c       = 5.0f,    /* SWR-SAF-08 */
        .uncommanded_window_s     = 180.0f,
        .uncommanded_settle_s     = 60.0f,

        .excursion_band_c         = 50.0f,   /* SWR-SAF-10 */
        .excursion_window_s       = 120.0f,

        .door_confirm_s           = 0.2f,    /* SWR-SAF-31 */

        .insulation_factor        = 1.3f,    /* SWR-SAF-12 */
        .saturated_warn_s         = 600.0f,  /* warning 107 */

        .ct_fault_window_s        = 5.0f,    /* SWR-CUR-11 */

        .fail_on_threshold_a      = 0.5f,    /* SWR-SAF-25 */
        .fail_on_windows          = 2,

        .fail_off_fraction        = 0.20f,   /* SWR-SAF-26 */
        /* Derived from the nominal current by the call below; named here
         * because C++ -Wmissing-field-initializers will not let a designated
         * initialiser leave a member to be zero-filled silently, and a safety
         * threshold that appears from nowhere is worth the two lines. */
        .fail_off_min_a           = 0.0f,
        .fail_off_window_s        = 30.0f,
        .fail_off_min_windows     = 3,

        /* SWR-SAF-27 / SWR-NFR-27: 2 s for the contactor to drop, 3 s total to a verdict. */
        .weld_wait_s              = 2.0f,
        .weld_verdict_s           = 3.0f,

        .deviation_warn_frac      = 0.10f,   /* SWR-SAF-28 */
        .deviation_fault_frac     = 0.25f,
        .deviation_window_s       = 30.0f,
        .deviation_min_windows    = 3,

        .overcurrent_a            = 0.0f,    /* derived, as fail_off_min_a */
        .overcurrent_windows      = 2,       /* SWR-SAF-29 */

        .contactor_life_ops       = 100000u, /* SWR-SAF-30 */
        .ssr_life_ops             = 10000000u,
        .mismatch_episodes_warn   = 3,
    };
    *cfg = d;

    /* The three current limits that are all a function of the nominal. */
    kiln_safety_cfg_set_nominal_current(cfg, 30.0f);
}

void kiln_safety_cfg_set_nominal_current(kiln_safety_cfg_t *cfg, float nominal_a)
{
    if (cfg == nullptr) {
        return;
    }
    const float nom = kiln_clampf(nominal_a, 0.0f, 200.0f);

    /* SWR-SAF-29: 120 % of nominal. */
    cfg->overcurrent_a = nom * 1.20f;
    /* SWR-SAF-26's floor before SWR-CUR-08's reference exists: the same 20 % fraction
     * applied to the nominal, so the rule has teeth from the first on-window. */
    cfg->fail_off_min_a = nom * cfg->fail_off_fraction;
}

namespace {

/* Bound the configuration and report whether anything had to be moved (SWR-NFR-17:
 * a silently corrected safety threshold is a defect, not a convenience). */
bool clamp_cfg(kiln_safety_cfg_t *c)
{
    bool moved = false;

    /* SWR-SAF-23 binds whatever was configured. */
    moved |= kiln_clampf_moved(&c->max_temp_c, 0.0f, KILN_TEMP_CEILING_C);

    if (c->fail_on_windows == 0) {
        c->fail_on_windows = 1;
        moved              = true;
    }
    if (c->overcurrent_windows == 0) {
        c->overcurrent_windows = 1;
        moved                  = true;
    }
    /* SWR-SAF-22: bounded, and the lower bound is not zero-with-an-escape -- 0 s is
     * a legitimate setting here (latch on the first open sample) and the upper
     * bound keeps the latch inside SWR-NFR-04's 500 ms whatever is configured. */
    moved |= kiln_clampf_moved(&c->door_confirm_s, 0.0f, 0.5f);
    moved |= kiln_clampf_moved(&c->fail_off_fraction, 0.0f, 1.0f);
    moved |= kiln_clampf_moved(&c->weld_wait_s, 0.1f, 10.0f);
    /* The verdict budget must leave room for at least one measurement after the
     * contactor has had its drop-out time. */
    if (c->weld_verdict_s < c->weld_wait_s + 0.25f) {
        c->weld_verdict_s = c->weld_wait_s + 0.25f;
        moved             = true;
    }
    if (c->mismatch_episodes_warn == 0) {
        c->mismatch_episodes_warn = 3;
        moved                     = true;
    }
    if (c->fail_off_min_windows == 0) {
        c->fail_off_min_windows = 1;
        moved                   = true;
    }
    if (c->deviation_min_windows == 0) {
        c->deviation_min_windows = 1;
        moved                    = true;
    }

    return moved;
}

} // namespace

void kiln_safety_init(kiln_safety_t *s, const kiln_safety_cfg_t *cfg)
{
    const kiln_safety_t zero = {};
    *s = zero;
    s->cfg = *cfg;
    (void)clamp_cfg(&s->cfg);
}

void kiln_safety_reconfigure(kiln_safety_t *s, const kiln_safety_cfg_t *cfg)
{
    s->cfg = *cfg;
    (void)clamp_cfg(&s->cfg);
}

void kiln_safety_begin_run(kiln_safety_t *s, const kiln_insulation_baseline_t *baseline)
{
    const kiln_rule_state_t clear = {};
    s->tc_grace = s->case_grace = clear;
    s->reversed = s->stuck = s->runaway = s->uncommanded = clear;
    s->excursion = s->saturated = clear;
    s->ct_fault = clear;

    s->run_duty_s = 0.0;
    memset(s->band_duty_s,  0, sizeof(s->band_duty_s));
    memset(s->band_crossed, 0, sizeof(s->band_crossed));

    s->fail_on_count         = 0;
    s->overcurrent_count     = 0;
    s->fail_off_timer_s      = 0.0f;
    s->fail_off_windows      = 0;
    s->fail_off_below        = false;
    s->deviation_timer_s     = 0.0f;
    s->deviation_windows     = 0;
    s->weld_phase            = KILN_WELD_IDLE;
    s->weld_timer_s          = 0.0f;
    s->weld_saw_current      = false;
    s->weld_fault            = KILN_FAULT_NONE;
    s->mismatch_episodes     = 0;
    s->fail_on_episode_open  = false;
    s->fail_off_episode_open = false;

    s->latched_warnings = 0;

    if (baseline != nullptr) {
        s->baseline = *baseline;
    }
    else {
        const kiln_insulation_baseline_t none = {};
        s->baseline = none;
    }
}

/* --- SWR-SAF-04: thermocouple and front end faults --------------------------- */

namespace {

kiln_fault_t map_tc_fault(uint16_t bits, bool is_case)
{
    /* Most specific and most actionable first. */
    if ((bits & KILN_TC_FAULT_COMMS) != 0u) {
        return is_case ? KILN_FAULT_CASE_TC : KILN_FAULT_TC_COMMS;
    }
    if ((bits & KILN_TC_FAULT_OPEN) != 0u) {
        return is_case ? KILN_FAULT_CASE_TC : KILN_FAULT_TC_OPEN;
    }
    if ((bits & (KILN_TC_FAULT_SHORT_VCC | KILN_TC_FAULT_SHORT_GND)) != 0u) {
        return is_case ? KILN_FAULT_CASE_TC : KILN_FAULT_TC_SHORT;
    }
    if ((bits & KILN_TC_FAULT_CJ_RANGE) != 0u) {
        return is_case ? KILN_FAULT_CASE_TC : KILN_FAULT_CJ;
    }
    if ((bits & KILN_TC_FAULT_TC_RANGE) != 0u) {
        return is_case ? KILN_FAULT_CASE_TC : KILN_FAULT_TC_RANGE;
    }
    if ((bits & KILN_TC_FAULT_OVUV) != 0u) {
        return is_case ? KILN_FAULT_CASE_TC : KILN_FAULT_TC_RANGE;
    }
    return KILN_FAULT_NONE;
}

/* SWR-ACQ-12: a transient glitch is tolerated for the grace period; a fault that
 * outlives it is real. */
kiln_fault_t rule_tc(kiln_rule_state_t *st, const kiln_safety_cfg_t *cfg,
                            uint16_t bits, bool is_case, float dt_s)
{
    if (bits == 0) {
        st->timer_s = 0.0f;
        return KILN_FAULT_NONE;
    }
    st->timer_s += dt_s;
    if (st->timer_s >= cfg->tc_grace_s) {
        return map_tc_fault(bits, is_case);
    }
    return KILN_FAULT_NONE;
}

/* --- SWR-SAF-05: reversed thermocouple --------------------------------------- */

/* The temperature rules take `valid`, and every one of them *holds* on an
 * invalid reading rather than resetting: the reading inside an SWR-ACQ-12 grace
 * window is not a measurement, so it must neither be compared against nor
 * recorded as the extreme of a window.  Resetting instead would let a sensor
 * that glitches once per grace period keep a 10 min window permanently at zero,
 * which is how SWR-SAF-06 gets quietly disabled. */

kiln_fault_t rule_reversed(kiln_rule_state_t *st, const kiln_safety_cfg_t *cfg,
                                  const kiln_safety_input_t *in, bool valid, float dt_s)
{
    if (!in->heating_active || in->duty_permille < cfg->reversed_duty_permille) {
        st->armed     = false;
        st->confirm_s = 0.0f;
        return KILN_FAULT_NONE;
    }
    if (!valid) {
        return KILN_FAULT_NONE; /* hold */
    }

    if (!st->armed) {
        st->armed     = true;
        st->ref_c     = in->kiln_c;
        st->timer_s   = 0.0f;
        st->confirm_s = 0.0f;
        return KILN_FAULT_NONE;
    }

    st->timer_s += dt_s;

    /* Heating hard yet the reading is going the wrong way -- and staying there.
     * See reversed_confirm_s. */
    if (in->kiln_c <= st->ref_c - cfg->reversed_drop_c) {
        st->confirm_s += dt_s;
        if (st->confirm_s >= cfg->reversed_confirm_s) {
            return KILN_FAULT_TC_REVERSED;
        }
        return KILN_FAULT_NONE;
    }
    st->confirm_s = 0.0f;

    /* Track upward movement, and restart the window periodically so a slow
     * genuine rise never accumulates toward a false trip. */
    if (in->kiln_c > st->ref_c) {
        st->ref_c = in->kiln_c;
    }
    if (st->timer_s >= cfg->reversed_window_s) {
        st->ref_c   = in->kiln_c;
        st->timer_s = 0.0f;
    }
    return KILN_FAULT_NONE;
}

/* --- SWR-SAF-06: stuck sensor ------------------------------------------------ */

kiln_fault_t rule_stuck(kiln_rule_state_t *st, const kiln_safety_cfg_t *cfg,
                               const kiln_safety_input_t *in, bool valid, float dt_s)
{
    if (!in->heating_active || in->duty_permille < cfg->stuck_duty_permille) {
        st->armed = false;
        return KILN_FAULT_NONE;
    }
    if (!valid) {
        return KILN_FAULT_NONE; /* hold */
    }

    if (!st->armed) {
        st->armed   = true;
        st->timer_s = 0.0f;
        st->min_c   = in->kiln_c;
        st->max_c   = in->kiln_c;
        return KILN_FAULT_NONE;
    }

    st->timer_s += dt_s;
    if (in->kiln_c < st->min_c) {
        st->min_c = in->kiln_c;
    }
    if (in->kiln_c > st->max_c) {
        st->max_c = in->kiln_c;
    }

    if (st->timer_s >= cfg->stuck_window_s) {
        if ((st->max_c - st->min_c) < cfg->stuck_delta_c) {
            return KILN_FAULT_TC_STUCK;
        }
        /* Moved as expected: start a fresh window. */
        st->timer_s = 0.0f;
        st->min_c = st->max_c = in->kiln_c;
    }
    return KILN_FAULT_NONE;
}

/* --- SWR-SAF-07: thermal runaway / heating failure --------------------------- */

kiln_fault_t rule_runaway(kiln_rule_state_t *st, const kiln_safety_cfg_t *cfg,
                                 const kiln_safety_input_t *in, bool valid, float dt_s)
{
    const bool driving_hard = in->heating_active &&
                              in->duty_permille >= cfg->runaway_duty_permille;
    const bool not_rising   = in->rate_c_per_h < cfg->runaway_min_rate_c_per_h;

    if (!driving_hard) {
        st->timer_s = 0.0f;
        return KILN_FAULT_NONE;
    }
    if (!valid) {
        return KILN_FAULT_NONE; /* hold: the rate is not a rate */
    }

    if (!not_rising) {
        st->timer_s = 0.0f;
        return KILN_FAULT_NONE;
    }

    st->timer_s += dt_s;
    if (st->timer_s >= cfg->runaway_window_s) {
        return KILN_FAULT_RUNAWAY;
    }
    return KILN_FAULT_NONE;
}

/* --- SWR-SAF-08: uncommanded heating (shorted SSR) --------------------------- */

kiln_fault_t rule_uncommanded(kiln_rule_state_t *st, const kiln_safety_cfg_t *cfg,
                                     const kiln_safety_input_t *in, bool valid, float dt_s)
{
    if (in->duty_permille > 0) {
        st->armed   = false;
        st->timer_s = 0.0f;
        return KILN_FAULT_NONE;
    }

    /* Wait out the settle period before arming, so residual heat soaking in
     * from the elements is not mistaken for a shorted SSR. */
    if (!st->armed) {
        st->timer_s += dt_s;
        if (st->timer_s < cfg->uncommanded_settle_s) {
            return KILN_FAULT_NONE;
        }
        if (!valid) {
            return KILN_FAULT_NONE;
        }
        st->armed   = true;
        st->ref_c   = in->kiln_c;
        st->timer_s = 0.0f;
        return KILN_FAULT_NONE;
    }

    if (!valid) {
        return KILN_FAULT_NONE; /* hold */
    }

    st->timer_s += dt_s;

    if (in->kiln_c >= st->ref_c + cfg->uncommanded_rise_c) {
        return KILN_FAULT_UNCOMMANDED_HEAT;
    }

    /* A cooling kiln lowers the reference, so the test stays a test of rise. */
    if (in->kiln_c < st->ref_c) {
        st->ref_c = in->kiln_c;
    }
    if (st->timer_s >= cfg->uncommanded_window_s) {
        st->ref_c   = in->kiln_c;
        st->timer_s = 0.0f;
    }
    return KILN_FAULT_NONE;
}

/* --- SWR-SAF-10: setpoint excursion ----------------------------------------- */

kiln_fault_t rule_excursion(kiln_rule_state_t *st, const kiln_safety_cfg_t *cfg,
                                   const kiln_safety_input_t *in, bool valid, float dt_s)
{
    if (!in->heating_active) {
        st->timer_s = 0.0f;
        return KILN_FAULT_NONE;
    }
    if (!valid) {
        return KILN_FAULT_NONE; /* hold */
    }

    if ((in->kiln_c - in->setpoint_c) <= cfg->excursion_band_c) {
        st->timer_s = 0.0f;
        return KILN_FAULT_NONE;
    }

    st->timer_s += dt_s;
    if (st->timer_s >= cfg->excursion_window_s) {
        return KILN_FAULT_SP_EXCURSION;
    }
    return KILN_FAULT_NONE;
}

/* --- SWR-SAF-12: insulation degradation (warning only) ---------------------- */

void rule_insulation(kiln_safety_t *s, const kiln_safety_input_t *in,
                            bool valid, float dt_s)
{
    s->run_duty_s += (double)in->duty_permille * (double)dt_s / (double)KILN_DUTY_MAX;
    if (!valid) {
        return;
    }

    for (uint8_t b = 0; b < KILN_INSUL_BANDS; b++) {
        const float boundary = 100.0f * (float)(b + 1);
        if (s->band_crossed[b] || in->kiln_c < boundary) {
            continue;
        }

        s->band_crossed[b] = true;
        s->band_duty_s[b]  = (uint32_t)s->run_duty_s;

        if (s->baseline.valid[b] && s->baseline.duty_s[b] > 0) {
            const double limit = (double)s->baseline.duty_s[b] *
                                 (double)s->cfg.insulation_factor;
            if ((double)s->band_duty_s[b] > limit) {
                /* Episodic: the evidence is a boundary crossing that has now
                 * passed, so this one is carried for the rest of the run. */
                s->latched_warnings |= KILN_WARN_BIT(KILN_WARN_INSULATION);
            }
        }
    }
}

/* --- heater current ---------------------------------------------------- */

/* A measurement is usable when a transformer answered, the window was long
 * enough, and it is this cycle's rather than a carried-over one. */
bool cur_usable(const kiln_safety_input_t *in)
{
    return in->current_monitoring && in->current_fresh && kiln_is_finite(in->current_a) &&
           ((in->current_flags & (KILN_CURF_CT_FAULT | KILN_CURF_SKIPPED | KILN_CURF_STALE)) == 0u);
}

bool cur_is_leakage(const kiln_safety_input_t *in)
{
    return cur_usable(in) && (in->current_flags & KILN_CURF_LEAKAGE) != 0;
}

bool cur_is_conduction(const kiln_safety_input_t *in)
{
    return cur_usable(in) && (in->current_flags & KILN_CURF_CONDUCTION) != 0;
}

/* SWR-CUR-11.  Graced like SWR-SAF-04: one burst spoiled by the switching transient of
 * a multi-kilowatt load is not a missing transformer. */
kiln_fault_t rule_ct_fault(kiln_safety_t *s, const kiln_safety_input_t *in, float dt_s)
{
    if (!in->current_monitoring || ((in->current_flags & KILN_CURF_CT_FAULT) == 0u)) {
        s->ct_fault.timer_s = 0.0f;
        return KILN_FAULT_NONE;
    }
    s->ct_fault.timer_s += dt_s;
    return s->ct_fault.timer_s >= s->cfg.ct_fault_window_s ? KILN_FAULT_CT_FAULT
                                                           : KILN_FAULT_NONE;
}

/* SWR-SAF-29.  Counted in measurement windows rather than latched on one sample: the
 * CT sits on a conductor carrying a switched multi-kilowatt load, and a single
 * burst caught across a turn-on transient should not end a firing. */
kiln_fault_t rule_overcurrent(kiln_safety_t *s, const kiln_safety_input_t *in)
{
    if (!cur_usable(in)) {
        return KILN_FAULT_NONE;
    }
    if (!(s->cfg.overcurrent_a > 0.0f)) {
        return KILN_FAULT_NONE;
    }

    if (in->current_a > s->cfg.overcurrent_a) {
        if (s->overcurrent_count < 255u) {
            s->overcurrent_count++;
        }
        if (s->overcurrent_count >= s->cfg.overcurrent_windows) {
            return KILN_FAULT_OVERCURRENT;
        }
    } else {
        s->overcurrent_count = 0;
    }
    return KILN_FAULT_NONE;
}

/* SWR-SAF-25 and SWR-SAF-27, which are one mechanism: detect current in an off-window,
 * then find out *which* device failed by opening the contactor and looking again.
 *
 * Until the verdict, heat is withheld and no fault is latched -- latching 21
 * early would name the SSR before the test that distinguishes it from the far
 * more serious welded contactor of SWR-SAF-27.  SWR-NFR-27 bounds the whole sequence:
 * de-energise within 1 s of the offending window, verdict within a further 3 s.
 */
kiln_fault_t rule_fail_on_and_weld(kiln_safety_t *s, const kiln_safety_input_t *in,
                                          kiln_safety_verdict_t *v, float dt_s)
{
    /* The sequence, once started, outranks the detector that started it. */
    if (s->weld_phase != KILN_WELD_IDLE) {
        v->heat_permitted = false;
        v->drop_contactor = true;

        if (s->weld_phase == KILN_WELD_DONE) {
            return s->weld_fault;
        }

        s->weld_timer_s += dt_s;

        if ((s->weld_phase == KILN_WELD_WAIT_DROPOUT) && (s->weld_timer_s >= s->cfg.weld_wait_s)) {
            s->weld_phase = KILN_WELD_REMEASURE;
        }

        if (s->weld_phase == KILN_WELD_REMEASURE) {
            if (cur_is_leakage(in)) {
                s->weld_saw_current = in->current_a > s->cfg.fail_on_threshold_a;
                /* Current stopped when the contactor opened: the contactor is
                 * sound and the SSR is shorted.  Current persisted: the
                 * contactor is welded and nothing left in the controller can
                 * interrupt the heaters. */
                s->weld_fault = s->weld_saw_current ? KILN_FAULT_CONTACTOR_WELDED
                                                    : KILN_FAULT_UNCOMMANDED_CURRENT;
                s->weld_phase = KILN_WELD_DONE;
                return s->weld_fault;
            }
            if (s->weld_timer_s >= s->cfg.weld_verdict_s) {
                /* No measurement inside SWR-NFR-27's budget.  Take the more severe
                 * verdict: a fail-safe decision must not fail open, and the cost
                 * of being wrong here is an operator who isolates a kiln that
                 * only had a shorted SSR. */
                s->weld_fault = KILN_FAULT_CONTACTOR_WELDED;
                s->weld_phase = KILN_WELD_DONE;
                return s->weld_fault;
            }
        }
        return KILN_FAULT_NONE;
    }

    if (!cur_is_leakage(in)) {
        return KILN_FAULT_NONE;
    }

    if (in->current_a > s->cfg.fail_on_threshold_a) {
        if (s->fail_on_count < 255u) {
            s->fail_on_count++;
        }
        s->fail_on_episode_open = true;

        if (s->fail_on_count >= s->cfg.fail_on_windows) {
            /* SWR-NFR-27: heat comes off now, in the same cycle as the offending
             * measurement.  The verdict follows. */
            v->heat_permitted       = false;
            v->drop_contactor       = true;
            s->weld_phase           = KILN_WELD_WAIT_DROPOUT;
            s->weld_timer_s         = 0.0f;
            s->weld_saw_current     = false;
            s->fail_on_episode_open = false;   /* a fault, not an intermittency */
        }
        return KILN_FAULT_NONE;
    }

    /* Below threshold again having been above it: SWR-SAF-30's intermittent mismatch,
     * the early sign that a relay is becoming defective before it fails. */
    if (s->fail_on_episode_open) {
        if (s->mismatch_episodes < 0xFFFFu) {
            s->mismatch_episodes++;
        }
        s->fail_on_episode_open = false;
    }
    s->fail_on_count = 0;
    return KILN_FAULT_NONE;
}

/* SWR-SAF-26.  Acts long before SWR-SAF-07's 15 min thermal window: a failed SSR, an open
 * contactor, an open safety chain, a blown heater fuse or fully open elements all
 * show up as no amps in an on-window. */
kiln_fault_t rule_fail_off(kiln_safety_t *s, const kiln_safety_input_t *in, float dt_s)
{
    if (!in->current_monitoring || !in->heating_active || in->duty_permille == 0) {
        if (s->fail_off_episode_open) {
            if (s->mismatch_episodes < 0xFFFFu) {
                s->mismatch_episodes++;
            }
            s->fail_off_episode_open = false;
        }
        s->fail_off_timer_s = 0.0f;
        s->fail_off_windows = 0;
        s->fail_off_below   = false;
        return KILN_FAULT_NONE;
    }

    /* SWR-CUR-08's reference once it exists, the nominal-derived floor before. */
    const float threshold = (in->current_ref_a > 0.0f)
                          ? s->cfg.fail_off_fraction * in->current_ref_a
                          : s->cfg.fail_off_min_a;
    if (!(threshold > 0.0f)) {
        return KILN_FAULT_NONE;
    }

    if (cur_is_conduction(in)) {
        s->fail_off_below = in->current_a < threshold;
        if (s->fail_off_below) {
            if (s->fail_off_windows < 255u) {
                s->fail_off_windows++;
            }
            s->fail_off_episode_open = true;
        } else {
            if (s->fail_off_episode_open) {
                if (s->mismatch_episodes < 0xFFFFu) {
                    s->mismatch_episodes++;
                }
                s->fail_off_episode_open = false;
            }
            s->fail_off_windows = 0;
            s->fail_off_timer_s = 0.0f;
        }
    }

    if (!s->fail_off_below) {
        s->fail_off_timer_s = 0.0f;
        s->fail_off_windows = 0;
        return KILN_FAULT_NONE;
    }

    /* The elapsed time is tracked on wall time -- SWR-CUR-05 skips short windows,
     * and a low duty would otherwise stretch "30 s" into an hour -- but the
     * verdict needs both that *and* a run of low measurements.  See
     * fail_off_min_windows for why one of the two is not enough. */
    s->fail_off_timer_s += dt_s;
    if (s->fail_off_timer_s >= s->cfg.fail_off_window_s &&
        s->fail_off_windows >= s->cfg.fail_off_min_windows) {
        s->fail_off_episode_open = false;
        return KILN_FAULT_NO_HEATER_CURRENT;
    }
    return KILN_FAULT_NONE;
}

/* SWR-SAF-28.  Losing one of several element groups is a step change of a known
 * fraction -- a third of the current for one of three groups -- so the
 * interesting signal is the size of the step, not its direction. */
kiln_fault_t rule_deviation(kiln_safety_t *s, const kiln_safety_input_t *in,
                                   uint32_t *warnings, float dt_s)
{
    if (!in->current_monitoring || !in->current_deviation_valid ||
        !kiln_is_finite(in->current_deviation)) {
        return KILN_FAULT_NONE;          /* hold the timer: no evidence either way */
    }

    const float d = fabsf(in->current_deviation);

    if (d >= s->cfg.deviation_warn_frac) {
        *warnings |= KILN_WARN_BIT(KILN_WARN_CURRENT_DEV);
    }

    if (d >= s->cfg.deviation_fault_frac) {
        if (in->current_fresh && s->deviation_windows < 255u) {
            s->deviation_windows++;
        }
        s->deviation_timer_s += dt_s;
        /* Both, as for SWR-SAF-26. */
        if (s->deviation_timer_s >= s->cfg.deviation_window_s &&
            s->deviation_windows >= s->cfg.deviation_min_windows) {
            return KILN_FAULT_CURRENT_DEVIATION;
        }
    } else {
        s->deviation_timer_s = 0.0f;
        s->deviation_windows = 0;
    }
    return KILN_FAULT_NONE;
}

/* SWR-SAF-30.  Warnings only: a relay at its life limit still works, and an
 * intermittent mismatch is a prediction rather than a failure. */
void rule_relay_wear(kiln_safety_t *s, const kiln_safety_input_t *in,
                            uint32_t *warnings)
{
    if (s->cfg.contactor_life_ops > 0 && in->contactor_ops >= s->cfg.contactor_life_ops) {
        *warnings |= KILN_WARN_BIT(KILN_WARN_RELAY_WEAR);
    }
    if (s->cfg.ssr_life_ops > 0) {
        for (uint8_t ch = 0; ch < KILN_HEAT_CHANNELS; ch++) {
            if (in->ssr_ops[ch] >= s->cfg.ssr_life_ops) {
                *warnings |= KILN_WARN_BIT(KILN_WARN_RELAY_WEAR);
            }
        }
    }

    if (s->mismatch_episodes >= s->cfg.mismatch_episodes_warn) {
        /* Episodic by definition: the mismatches self-cleared, so the evidence
         * is a count of things that are no longer happening. */
        s->latched_warnings |= KILN_WARN_BIT(KILN_WARN_RELAY_SUSPECT);
    }
}

} // namespace

/* --- evaluation -------------------------------------------------------- */

kiln_err_t kiln_safety_eval_checked(kiln_safety_t *s,
                                    const kiln_safety_input_t *in,
                                    float dt_s,
                                    kiln_safety_verdict_t *out)
{
    if ((s == nullptr) || (in == nullptr) || (out == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    if (!kiln_is_finite(dt_s) || dt_s < 0.0f) {
        /* Fail safe, and say what was actually wrong rather than blaming SWR-SAF-13. */
        out->heat_permitted = false;
        out->drop_contactor = false;
        out->fault          = KILN_FAULT_NONE;
        out->warnings       = s->latched_warnings;
        return KILN_ERR_INVALID_ARG;
    }
    *out = kiln_safety_eval(s, in, dt_s);
    return KILN_OK;
}

namespace {

/* SWR-SAF-31: door / lid interlock.
 *
 * The one rule in the table that needs no history, no timer and no trust in
 * any other reading -- which is why kiln_safety_eval runs it first, ahead even
 * of SWR-SAF-13.  A door that is open is a fact; everything else here is an
 * inference from a measurement.
 *
 * Two tiers, as SWR-SAF-09 has:
 *   - heat is withheld and the contactor dropped on the FIRST open sample, with
 *     no delay, because that is the whole point of a door switch;
 *   - the fault latches only once the door has read open for door_confirm_s,
 *     so a single noise-corrupted sample costs a fraction of a second of duty
 *     rather than stopping a healthy firing (HZ-10).
 *
 * It latches only while heating_active.  Opening the door of an idle kiln is
 * what loading one looks like, and a controller that demanded an
 * acknowledgement for it would teach its operator to clear faults reflexively.
 */
kiln_fault_t rule_door(kiln_safety_t *s, const kiln_safety_input_t *in,
                              kiln_safety_verdict_t *v, float dt_s)
{
    if (!in->door_monitoring) {
        s->door.timer_s = 0.0f;
        return KILN_FAULT_NONE;
    }
    if (!in->door_open) {
        s->door.timer_s = 0.0f;
        return KILN_FAULT_NONE;
    }

    /* Tier one, unconditional and immediate. */
    v->heat_permitted = false;
    v->drop_contactor = true;

    /* Tier two. */
    s->door.timer_s += dt_s;
    if (in->heating_active && s->door.timer_s >= s->cfg.door_confirm_s) {
        return KILN_FAULT_DOOR_OPEN;
    }
    return KILN_FAULT_NONE;
}

} // namespace

kiln_safety_verdict_t kiln_safety_eval(kiln_safety_t *s,
                                       const kiln_safety_input_t *in,
                                       float dt_s)
{
    kiln_safety_verdict_t v = { .heat_permitted = true,
                                .drop_contactor = false,
                                .fault          = KILN_FAULT_NONE,
                                .warnings       = 0 };
    uint32_t warnings = 0;

    if ((s == nullptr) || (in == nullptr) || !kiln_is_finite(dt_s) || dt_s < 0.0f) {
        /* Fail safe, but do not call it SWR-SAF-13: fault 14 means the safety
         * supervisor missed its deadline, and reporting it for a NULL pointer or
         * a negative dt sends whoever reads the fault log looking at the
         * scheduler instead of at the caller.  The verdict withholds heat; a
         * caller that wants to know *why* uses kiln_safety_eval_checked. */
        v.heat_permitted = false;
        v.fault          = KILN_FAULT_NONE;
        return v;
    }

    /* SWR-ACQ-12 plus SWR-NFR-17: a reading is believable when the acquisition layer
     * says so *and* it is a number.  A non-finite value reaching here is a defect
     * upstream, and is treated as an invalid reading rather than propagated. */
    const bool kiln_ok = in->kiln_valid && kiln_is_finite(in->kiln_c) &&
                         kiln_is_finite(in->rate_c_per_h);
    const bool case_ok = in->case_valid && kiln_is_finite(in->case_c);

    /* SWR-SAF-31 before everything, including SWR-SAF-13.  A door switch is a direct
     * physical signal: it does not depend on the loop having met its deadline,
     * on a reading being believable, or on any timer having run.  If the door
     * is open the kiln must not be heating, whatever else is also wrong. */
    {
        const kiln_fault_t f = rule_door(s, in, &v, dt_s);
        if (f != KILN_FAULT_NONE) { v.heat_permitted = false; v.fault = f; goto done; }
    }
    if (!in->door_monitoring) {
        warnings |= KILN_WARN_BIT(KILN_WARN_DOOR_OFF);
    }
    /* Heat already withheld by tier one; keep evaluating so a door left open
     * does not mask a fault that also wants latching. */

    /* SWR-SAF-13: if the loop timing itself is broken, nothing downstream can
     * be trusted. */
    if (in->safety_deadline_missed)  { v.heat_permitted = false; v.fault = KILN_FAULT_SAFETY_DEADLINE;  goto done; }
    if (in->control_deadline_missed) { v.heat_permitted = false; v.fault = KILN_FAULT_CONTROL_DEADLINE; goto done; }

    /* SWR-SAF-25/SWR-SAF-27 next, and before the sensor rules: the sequence is already
     * holding heat off and is inside a 3 s verdict budget (SWR-NFR-27), so nothing
     * else may pre-empt it. */
    {
        const kiln_fault_t f = rule_fail_on_and_weld(s, in, &v, dt_s);
        if (f != KILN_FAULT_NONE) { v.heat_permitted = false; v.fault = f; goto done; }
        if (s->weld_phase != KILN_WELD_IDLE) {
            goto done; /* verdict pending */
        }
    }

    /* SWR-SAF-04, both channels. */
    {
        const kiln_fault_t f = rule_tc(&s->tc_grace, &s->cfg, in->tc_fault_bits, false, dt_s);
        if (f != KILN_FAULT_NONE) { v.heat_permitted = false; v.fault = f; goto done; }
    }
    if (in->case_present) {
        const kiln_fault_t f = rule_tc(&s->case_grace, &s->cfg, in->case_fault_bits, true, dt_s);
        if (f != KILN_FAULT_NONE) { v.heat_permitted = false; v.fault = f; goto done; }
    }

    /* SWR-CUR-11 */
    {
        const kiln_fault_t f = rule_ct_fault(s, in, dt_s);
        if (f != KILN_FAULT_NONE) { v.heat_permitted = false; v.fault = f; goto done; }
    }

    /* SWR-SAF-29 */
    {
        const kiln_fault_t f = rule_overcurrent(s, in);
        if (f != KILN_FAULT_NONE) { v.heat_permitted = false; v.fault = f; goto done; }
    }

    /* No trustworthy chamber reading: withhold heat for the duration of the
     * grace period rather than control on a number the front end has disowned.
     * No fault -- that is SWR-SAF-04's decision, and it has its own timer. */
    if (!kiln_ok) {
        v.heat_permitted = false;
    }

    /* SWR-SAF-09.  Exceeding the limit withholds heat at once; exceeding it by the
     * margin is a latching fault. */
    if (kiln_ok && in->kiln_c > s->cfg.max_temp_c) {
        v.heat_permitted = false;
        if (in->kiln_c > s->cfg.max_temp_c + s->cfg.overtemp_margin_c) {
            v.fault = KILN_FAULT_OVERTEMP;
            goto done;
        }
    }

    /* SWR-SAF-11 */
    if (in->case_present && case_ok && in->case_c > s->cfg.max_case_temp_c) {
        v.heat_permitted = false;
        v.fault = KILN_FAULT_CASE_OVERTEMP;
        goto done;
    }

    /* SWR-SAF-08 runs in every state: a shorted SSR is most likely to be noticed
     * while the controller believes it is idle.  SWR-SAF-25 sees the same failure in
     * amps rather than degrees, and far sooner; this is the backstop for a kiln
     * whose current monitoring is off or whose CT has failed (SWR-CUR-12). */
    {
        const kiln_fault_t f = rule_uncommanded(&s->uncommanded, &s->cfg, in, kiln_ok, dt_s);
        if (f != KILN_FAULT_NONE) { v.heat_permitted = false; v.fault = f; goto done; }
    }

    /* SWR-SAF-26 */
    {
        const kiln_fault_t f = rule_fail_off(s, in, dt_s);
        if (f != KILN_FAULT_NONE) { v.heat_permitted = false; v.fault = f; goto done; }
    }

    /* SWR-SAF-28 */
    {
        const kiln_fault_t f = rule_deviation(s, in, &warnings, dt_s);
        if (f != KILN_FAULT_NONE) { v.heat_permitted = false; v.fault = f; goto done; }
    }

    {
        const kiln_fault_t f = rule_reversed(&s->reversed, &s->cfg, in, kiln_ok, dt_s);
        if (f != KILN_FAULT_NONE) { v.heat_permitted = false; v.fault = f; goto done; }
    }
    {
        const kiln_fault_t f = rule_stuck(&s->stuck, &s->cfg, in, kiln_ok, dt_s);
        if (f != KILN_FAULT_NONE) { v.heat_permitted = false; v.fault = f; goto done; }
    }
    {
        const kiln_fault_t f = rule_runaway(&s->runaway, &s->cfg, in, kiln_ok, dt_s);
        if (f != KILN_FAULT_NONE) { v.heat_permitted = false; v.fault = f; goto done; }
    }
    {
        const kiln_fault_t f = rule_excursion(&s->excursion, &s->cfg, in, kiln_ok, dt_s);
        if (f != KILN_FAULT_NONE) { v.heat_permitted = false; v.fault = f; goto done; }
    }

    /* Warnings: never withhold heat. */
    if (in->heating_active) {
        rule_insulation(s, in, kiln_ok, dt_s);

        /* SWR-CTL-15 / warning 107.  Live, not sticky: once duty comes off the
         * ceiling the kiln is keeping up again, and Appendix A defines a warning
         * as non-latching. */
        if (in->duty_permille >= KILN_DUTY_MAX) {
            s->saturated.timer_s += dt_s;
        } else {
            s->saturated.timer_s = 0.0f;
        }
    }
    if (s->saturated.timer_s >= s->cfg.saturated_warn_s) {
        warnings |= KILN_WARN_BIT(KILN_WARN_DUTY_SATURATED);
    }

    rule_relay_wear(s, in, &warnings);

done:
    /* SWR-CUR-12: a persistent warning for as long as monitoring is off, because
     * relay failure can then only be inferred from temperature. */
    if (!in->current_monitoring) {
        warnings |= KILN_WARN_BIT(KILN_WARN_CURRENT_OFF);
    }

    v.warnings = warnings | s->latched_warnings;
    return v;
}

bool kiln_safety_can_clear(const kiln_safety_cfg_t *cfg,
                           kiln_fault_t code,
                           const kiln_safety_input_t *in)
{
    if ((cfg == nullptr) || (in == nullptr)) {
        return false;
    }

    /* An allow-list, not a deny-list.  SWR-SAF-17/SWR-SAF-18 are a fail-safe decision, and
     * a fail-safe decision must not fail open: the previous `default: return
     * true` made KILN_FAULT_CONTACTOR_WELDED -- the one fault whose instruction
     * is "isolate the kiln, the controller can no longer interrupt the current"
     * -- clearable by omission.  A new code added to the enum now defaults to
     * requiring a deliberate decision here. */
    switch (code) {
    case KILN_FAULT_NONE:
        return true;

    /* Sensor faults: clearable once the front end stops reporting them. */
    case KILN_FAULT_TC_OPEN:
    case KILN_FAULT_TC_SHORT:
    case KILN_FAULT_TC_RANGE:
    case KILN_FAULT_CJ:
    case KILN_FAULT_TC_COMMS:
        return in->tc_fault_bits == 0;
    case KILN_FAULT_CASE_TC:
        return in->case_fault_bits == 0;

    /* SWR-SAF-31: clearable once the door is shut again, and not before.  Reuses the
     * input the detector reads, so the two cannot disagree. */
    case KILN_FAULT_DOOR_OPEN:
        return !in->door_monitoring || !in->door_open;

    /* Temperature faults: clearable once the kiln is back inside its limit. */
    case KILN_FAULT_OVERTEMP:
        return in->kiln_c <= cfg->max_temp_c;
    case KILN_FAULT_CASE_OVERTEMP:
        return in->case_c <= cfg->max_case_temp_c;

    /* SWR-SAF-08: refuse while the temperature is still climbing with heating
     * commanded off. */
    case KILN_FAULT_UNCOMMANDED_HEAT:
        return in->duty_permille != 0 || in->rate_c_per_h <= 0.0f;

    /* SWR-CUR-11: clearable once a transformer is answering again. */
    case KILN_FAULT_CT_FAULT:
        return (in->current_flags & KILN_CURF_CT_FAULT) == 0u;

    /* SWR-SAF-25: the SSR is shorted.  Clearable only once an off-window measures no
     * current -- which, since the fault means the SSR passes current whenever the
     * contactor is closed, in practice means after the SSR has been replaced. */
    case KILN_FAULT_UNCOMMANDED_CURRENT:
        return ((in->current_flags & KILN_CURF_LEAKAGE) != 0u) &&
               ((in->current_flags & (KILN_CURF_CT_FAULT | KILN_CURF_STALE)) == 0u) &&
               in->current_a <= cfg->fail_on_threshold_a;

    /* SWR-SAF-29: clearable once current is back inside the limit. */
    case KILN_FAULT_OVERCURRENT:
        return in->current_a <= cfg->overcurrent_a;

    /* Episodes that have ended and whose cause the operator is expected to have
     * investigated.  Each is listed deliberately. */
    case KILN_FAULT_TC_REVERSED:        /* rewire the probe            */
    case KILN_FAULT_TC_STUCK:           /* replace the probe           */
    case KILN_FAULT_RUNAWAY:            /* SWR-SAF-07, element/contactor    */
    case KILN_FAULT_SP_EXCURSION:       /* SWR-SAF-10, retune               */
    case KILN_FAULT_NO_HEATER_CURRENT:  /* SWR-SAF-26, element/SSR/fuse     */
    case KILN_FAULT_CURRENT_DEVIATION:  /* SWR-SAF-28, element group        */
    case KILN_FAULT_CONTROL_DEADLINE:
    case KILN_FAULT_SAFETY_DEADLINE:
    case KILN_FAULT_WATCHDOG:
    case KILN_FAULT_OPERATOR_ABORT:
    case KILN_FAULT_TUNE_NO_CONVERGE:
    case KILN_FAULT_RECOVERY_REFUSED:
    case KILN_FAULT_CONFIG_STORAGE:
        return true;

    /* SWR-SAF-27.  Never clearable from the interface: the controller has no
     * remaining means of interrupting the current, so there is nothing an
     * acknowledgement could make safe.  It is cleared by a power cycle, after
     * the contactor has been replaced. */
    case KILN_FAULT_CONTACTOR_WELDED:
        return false;

    case KILN_FAULT_MAX:
    default:
        return false;
    }
}

const uint32_t *kiln_safety_band_duty(const kiln_safety_t *s, uint8_t *count)
{
    if (count != nullptr) {
        *count = KILN_INSUL_BANDS;
    }
    if (s == nullptr) {
        return NULL; /* its neighbours validate; so does this one now */
    }
    return s->band_duty_s;
}

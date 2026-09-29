/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One static function per rule, each with its own accumulator, so that each row
 * of architecture section 8.2 maps to exactly one host test (TR-23).
 */

#include <string.h>
#include "kiln_core/safety.h"

void kiln_safety_cfg_defaults(kiln_safety_cfg_t *cfg)
{
    const kiln_safety_cfg_t d = {
        .max_temp_c               = 1280.0f,
        .overtemp_margin_c        = 10.0f,   /* SR-09 */
        .max_case_temp_c          = 70.0f,   /* SR-11 */
        .tc_grace_s               = 5.0f,    /* FR-ACQ-12 */

        .reversed_duty_permille   = 500,     /* SR-05 */
        .reversed_drop_c          = 5.0f,
        .reversed_window_s        = 120.0f,

        .stuck_delta_c            = 2.0f,    /* SR-06 */
        .stuck_duty_permille      = 500,
        .stuck_window_s           = 600.0f,

        .runaway_duty_permille    = 800,     /* SR-07 */
        .runaway_window_s         = 900.0f,
        .runaway_min_rate_c_per_h = 10.0f,

        .uncommanded_rise_c       = 5.0f,    /* SR-08 */
        .uncommanded_window_s     = 180.0f,
        .uncommanded_settle_s     = 60.0f,

        .excursion_band_c         = 50.0f,   /* SR-10 */
        .excursion_window_s       = 120.0f,

        .insulation_factor        = 1.3f,    /* SR-12 */
        .saturated_warn_s         = 600.0f,  /* warning 107 */
    };
    *cfg = d;
}

void kiln_safety_init(kiln_safety_t *s, const kiln_safety_cfg_t *cfg)
{
    const kiln_safety_t zero = {0};
    *s = zero;
    s->cfg = *cfg;
    /* SR-23 binds whatever was configured. */
    s->cfg.max_temp_c = kiln_clampf(s->cfg.max_temp_c, 0.0f, KILN_TEMP_CEILING_C);
}

void kiln_safety_reconfigure(kiln_safety_t *s, const kiln_safety_cfg_t *cfg)
{
    s->cfg = *cfg;
    s->cfg.max_temp_c = kiln_clampf(s->cfg.max_temp_c, 0.0f, KILN_TEMP_CEILING_C);
}

void kiln_safety_begin_run(kiln_safety_t *s, const kiln_insulation_baseline_t *baseline)
{
    const kiln_rule_state_t clear = {0};
    s->tc_grace = s->case_grace = clear;
    s->reversed = s->stuck = s->runaway = s->uncommanded = clear;
    s->excursion = s->saturated = clear;

    s->run_duty_s = 0.0;
    memset(s->band_duty_s,  0, sizeof(s->band_duty_s));
    memset(s->band_crossed, 0, sizeof(s->band_crossed));
    s->warnings = 0;

    if (baseline) {
        s->baseline = *baseline;
    } else {
        const kiln_insulation_baseline_t none = {0};
        s->baseline = none;
    }
}

/* --- SR-04: thermocouple and front end faults --------------------------- */

static kiln_fault_t map_tc_fault(uint16_t bits, bool is_case)
{
    /* Most specific and most actionable first. */
    if (bits & KILN_TC_FAULT_COMMS)      return is_case ? KILN_FAULT_CASE_TC : KILN_FAULT_TC_COMMS;
    if (bits & KILN_TC_FAULT_OPEN)       return is_case ? KILN_FAULT_CASE_TC : KILN_FAULT_TC_OPEN;
    if (bits & (KILN_TC_FAULT_SHORT_VCC | KILN_TC_FAULT_SHORT_GND))
                                         return is_case ? KILN_FAULT_CASE_TC : KILN_FAULT_TC_SHORT;
    if (bits & KILN_TC_FAULT_CJ_RANGE)   return is_case ? KILN_FAULT_CASE_TC : KILN_FAULT_CJ;
    if (bits & KILN_TC_FAULT_TC_RANGE)   return is_case ? KILN_FAULT_CASE_TC : KILN_FAULT_TC_RANGE;
    if (bits & KILN_TC_FAULT_OVUV)       return is_case ? KILN_FAULT_CASE_TC : KILN_FAULT_TC_RANGE;
    return KILN_FAULT_NONE;
}

/* FR-ACQ-12: a transient glitch is tolerated for the grace period; a fault that
 * outlives it is real. */
static kiln_fault_t rule_tc(kiln_rule_state_t *st, const kiln_safety_cfg_t *cfg,
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

/* --- SR-05: reversed thermocouple --------------------------------------- */

static kiln_fault_t rule_reversed(kiln_rule_state_t *st, const kiln_safety_cfg_t *cfg,
                                  const kiln_safety_input_t *in, float dt_s)
{
    if (!in->heating_active || in->duty_permille < cfg->reversed_duty_permille) {
        st->armed = false;
        return KILN_FAULT_NONE;
    }

    if (!st->armed) {
        st->armed   = true;
        st->ref_c   = in->kiln_c;
        st->timer_s = 0.0f;
        return KILN_FAULT_NONE;
    }

    st->timer_s += dt_s;

    /* Heating hard yet the reading is going the wrong way. */
    if (in->kiln_c <= st->ref_c - cfg->reversed_drop_c) {
        return KILN_FAULT_TC_REVERSED;
    }

    /* Track upward movement, and restart the window periodically so a slow
     * genuine rise never accumulates toward a false trip. */
    if (in->kiln_c > st->ref_c) st->ref_c = in->kiln_c;
    if (st->timer_s >= cfg->reversed_window_s) {
        st->ref_c   = in->kiln_c;
        st->timer_s = 0.0f;
    }
    return KILN_FAULT_NONE;
}

/* --- SR-06: stuck sensor ------------------------------------------------ */

static kiln_fault_t rule_stuck(kiln_rule_state_t *st, const kiln_safety_cfg_t *cfg,
                               const kiln_safety_input_t *in, float dt_s)
{
    if (!in->heating_active || in->duty_permille < cfg->stuck_duty_permille) {
        st->armed = false;
        return KILN_FAULT_NONE;
    }

    if (!st->armed) {
        st->armed   = true;
        st->timer_s = 0.0f;
        st->min_c   = in->kiln_c;
        st->max_c   = in->kiln_c;
        return KILN_FAULT_NONE;
    }

    st->timer_s += dt_s;
    if (in->kiln_c < st->min_c) st->min_c = in->kiln_c;
    if (in->kiln_c > st->max_c) st->max_c = in->kiln_c;

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

/* --- SR-07: thermal runaway / heating failure --------------------------- */

static kiln_fault_t rule_runaway(kiln_rule_state_t *st, const kiln_safety_cfg_t *cfg,
                                 const kiln_safety_input_t *in, float dt_s)
{
    const bool driving_hard = in->heating_active &&
                              in->duty_permille >= cfg->runaway_duty_permille;
    const bool not_rising   = in->rate_c_per_h < cfg->runaway_min_rate_c_per_h;

    if (!driving_hard || !not_rising) {
        st->timer_s = 0.0f;
        return KILN_FAULT_NONE;
    }

    st->timer_s += dt_s;
    if (st->timer_s >= cfg->runaway_window_s) {
        return KILN_FAULT_RUNAWAY;
    }
    return KILN_FAULT_NONE;
}

/* --- SR-08: uncommanded heating (shorted SSR) --------------------------- */

static kiln_fault_t rule_uncommanded(kiln_rule_state_t *st, const kiln_safety_cfg_t *cfg,
                                     const kiln_safety_input_t *in, float dt_s)
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
        if (st->timer_s < cfg->uncommanded_settle_s) return KILN_FAULT_NONE;
        st->armed   = true;
        st->ref_c   = in->kiln_c;
        st->timer_s = 0.0f;
        return KILN_FAULT_NONE;
    }

    st->timer_s += dt_s;

    if (in->kiln_c >= st->ref_c + cfg->uncommanded_rise_c) {
        return KILN_FAULT_UNCOMMANDED_HEAT;
    }

    /* A cooling kiln lowers the reference, so the test stays a test of rise. */
    if (in->kiln_c < st->ref_c) st->ref_c = in->kiln_c;
    if (st->timer_s >= cfg->uncommanded_window_s) {
        st->ref_c   = in->kiln_c;
        st->timer_s = 0.0f;
    }
    return KILN_FAULT_NONE;
}

/* --- SR-10: setpoint excursion ----------------------------------------- */

static kiln_fault_t rule_excursion(kiln_rule_state_t *st, const kiln_safety_cfg_t *cfg,
                                   const kiln_safety_input_t *in, float dt_s)
{
    if (!in->heating_active ||
        (in->kiln_c - in->setpoint_c) <= cfg->excursion_band_c) {
        st->timer_s = 0.0f;
        return KILN_FAULT_NONE;
    }

    st->timer_s += dt_s;
    if (st->timer_s >= cfg->excursion_window_s) {
        return KILN_FAULT_SP_EXCURSION;
    }
    return KILN_FAULT_NONE;
}

/* --- SR-12: insulation degradation (warning only) ---------------------- */

static void rule_insulation(kiln_safety_t *s, const kiln_safety_input_t *in, float dt_s)
{
    s->run_duty_s += (double)in->duty_permille * (double)dt_s / (double)KILN_DUTY_MAX;

    for (uint8_t b = 0; b < KILN_INSUL_BANDS; b++) {
        const float boundary = 100.0f * (float)(b + 1);
        if (s->band_crossed[b] || in->kiln_c < boundary) continue;

        s->band_crossed[b] = true;
        s->band_duty_s[b]  = (uint32_t)s->run_duty_s;

        if (s->baseline.valid[b] && s->baseline.duty_s[b] > 0) {
            const double limit = (double)s->baseline.duty_s[b] *
                                 (double)s->cfg.insulation_factor;
            if ((double)s->band_duty_s[b] > limit) {
                s->warnings |= KILN_WARN_BIT(KILN_WARN_INSULATION);
            }
        }
    }
}

/* --- evaluation -------------------------------------------------------- */

kiln_safety_verdict_t kiln_safety_eval(kiln_safety_t *s,
                                       const kiln_safety_input_t *in,
                                       float dt_s)
{
    kiln_safety_verdict_t v = { .heat_permitted = true,
                                .fault          = KILN_FAULT_NONE,
                                .warnings       = 0 };

    if (!s || !in || dt_s < 0.0f) {
        v.heat_permitted = false;
        v.fault = KILN_FAULT_SAFETY_DEADLINE;
        return v;
    }

    /* SR-13 first: if the loop timing itself is broken, nothing downstream can
     * be trusted. */
    if (in->safety_deadline_missed)  { v.heat_permitted = false; v.fault = KILN_FAULT_SAFETY_DEADLINE;  goto done; }
    if (in->control_deadline_missed) { v.heat_permitted = false; v.fault = KILN_FAULT_CONTROL_DEADLINE; goto done; }

    /* SR-04, both channels. */
    {
        const kiln_fault_t f = rule_tc(&s->tc_grace, &s->cfg, in->tc_fault_bits, false, dt_s);
        if (f != KILN_FAULT_NONE) { v.heat_permitted = false; v.fault = f; goto done; }
    }
    if (in->case_present) {
        const kiln_fault_t f = rule_tc(&s->case_grace, &s->cfg, in->case_fault_bits, true, dt_s);
        if (f != KILN_FAULT_NONE) { v.heat_permitted = false; v.fault = f; goto done; }
    }

    /* SR-09.  Exceeding the limit withholds heat at once; exceeding it by the
     * margin is a latching fault. */
    if (in->kiln_c > s->cfg.max_temp_c) {
        v.heat_permitted = false;
        if (in->kiln_c > s->cfg.max_temp_c + s->cfg.overtemp_margin_c) {
            v.fault = KILN_FAULT_OVERTEMP;
            goto done;
        }
    }

    /* SR-11 */
    if (in->case_present && in->case_c > s->cfg.max_case_temp_c) {
        v.heat_permitted = false;
        v.fault = KILN_FAULT_CASE_OVERTEMP;
        goto done;
    }

    /* SR-08 runs in every state: a shorted SSR is most likely to be noticed
     * while the controller believes it is idle. */
    {
        const kiln_fault_t f = rule_uncommanded(&s->uncommanded, &s->cfg, in, dt_s);
        if (f != KILN_FAULT_NONE) { v.heat_permitted = false; v.fault = f; goto done; }
    }

    {
        const kiln_fault_t f = rule_reversed(&s->reversed, &s->cfg, in, dt_s);
        if (f != KILN_FAULT_NONE) { v.heat_permitted = false; v.fault = f; goto done; }
    }
    {
        const kiln_fault_t f = rule_stuck(&s->stuck, &s->cfg, in, dt_s);
        if (f != KILN_FAULT_NONE) { v.heat_permitted = false; v.fault = f; goto done; }
    }
    {
        const kiln_fault_t f = rule_runaway(&s->runaway, &s->cfg, in, dt_s);
        if (f != KILN_FAULT_NONE) { v.heat_permitted = false; v.fault = f; goto done; }
    }
    {
        const kiln_fault_t f = rule_excursion(&s->excursion, &s->cfg, in, dt_s);
        if (f != KILN_FAULT_NONE) { v.heat_permitted = false; v.fault = f; goto done; }
    }

    /* Warnings: never withhold heat. */
    if (in->heating_active) {
        rule_insulation(s, in, dt_s);

        if (in->duty_permille >= KILN_DUTY_MAX) {
            s->saturated.timer_s += dt_s;
            if (s->saturated.timer_s >= s->cfg.saturated_warn_s) {
                s->warnings |= KILN_WARN_BIT(KILN_WARN_DUTY_SATURATED);
            }
        } else {
            s->saturated.timer_s = 0.0f;
        }
    }

done:
    v.warnings = s->warnings;
    return v;
}

bool kiln_safety_can_clear(const kiln_safety_cfg_t *cfg,
                           kiln_fault_t code,
                           const kiln_safety_input_t *in)
{
    if (!cfg || !in) return false;

    switch (code) {
    /* Sensor faults: clearable once the front end stops reporting them. */
    case KILN_FAULT_TC_OPEN:
    case KILN_FAULT_TC_SHORT:
    case KILN_FAULT_TC_RANGE:
    case KILN_FAULT_CJ:
    case KILN_FAULT_TC_COMMS:
        return in->tc_fault_bits == 0;
    case KILN_FAULT_CASE_TC:
        return in->case_fault_bits == 0;

    /* Temperature faults: clearable once the kiln is back inside its limit. */
    case KILN_FAULT_OVERTEMP:
        return in->kiln_c <= cfg->max_temp_c;
    case KILN_FAULT_CASE_OVERTEMP:
        return in->case_c <= cfg->max_case_temp_c;

    /* SR-08 is the dangerous one: refuse to clear while the temperature is
     * still climbing with heating commanded off. */
    case KILN_FAULT_UNCOMMANDED_HEAT:
        return !(in->duty_permille == 0 && in->rate_c_per_h > 0.0f);

    /* The rest describe an episode that has ended, not a standing condition:
     * they are clearable, and the operator is expected to have investigated. */
    default:
        return true;
    }
}

const uint32_t *kiln_safety_band_duty(const kiln_safety_t *s, uint8_t *count)
{
    if (count) *count = KILN_INSUL_BANDS;
    return s->band_duty_s;
}

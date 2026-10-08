/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <math.h>
#include "kiln_core/autotune.h"

#define KILN_PI 3.14159265358979323846f

void kiln_autotune_cfg_defaults(kiln_tune_cfg_t *cfg)
{
    const kiln_tune_cfg_t d = {
        .setpoint_c          = 600.0f,
        .max_temp_c          = 1280.0f,   /* SR-23; the configured kiln maximum */
        .amplitude_permille  = 500,
        .hysteresis_c        = 1.0f,
        .peak_threshold_c    = 1.0f,
        .timeout_s           = 7200.0f,
        .required_cycles     = 3,
        .period_tol          = 0.15f,
        .amplitude_tol       = 0.20f,
        .settle_rate_c_per_h = 30.0f,
        .settle_max_s        = 1800.0f,
    };
    *cfg = d;
}

kiln_err_t kiln_autotune_start(kiln_autotune_t *at, const kiln_tune_cfg_t *cfg)
{
    if ((at == nullptr) || (cfg == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    const kiln_autotune_t zero = {};
    *at = zero;
    at->cfg = *cfg;

    /* SR-23: the configured maximum and the compile-time ceiling both bind, and
     * they bind here rather than in a comment addressed to the caller. */
    const float requested = at->cfg.setpoint_c;
    const float limit     = kiln_clampf(at->cfg.max_temp_c, 0.0f, KILN_TEMP_CEILING_C);
    at->cfg.max_temp_c         = limit;
    at->cfg.setpoint_c         = kiln_clampf(at->cfg.setpoint_c, 0.0f, limit);
    at->cfg.amplitude_permille = kiln_clampu16(at->cfg.amplitude_permille, 100, KILN_DUTY_MAX);
    at->cfg.hysteresis_c       = kiln_clampf(at->cfg.hysteresis_c, 0.1f, 20.0f);
    if (!(at->cfg.peak_threshold_c > 0.0f)) {
        at->cfg.peak_threshold_c = at->cfg.hysteresis_c;
    }
    at->cfg.peak_threshold_c   = kiln_clampf(at->cfg.peak_threshold_c, 0.05f, 20.0f);
    at->cfg.timeout_s          = kiln_clampf(at->cfg.timeout_s, 600.0f, 28800.0f);
    if (at->cfg.required_cycles < 1) {
        at->cfg.required_cycles = 3;
    }
    if (at->cfg.required_cycles > KILN_TUNE_MAX_CYCLES - 1) {
        at->cfg.required_cycles = KILN_TUNE_MAX_CYCLES - 1;
    }
    if (at->cfg.period_tol <= 0.0f) {
        at->cfg.period_tol = 0.15f;
    }
    if (at->cfg.amplitude_tol <= 0.0f) {
        at->cfg.amplitude_tol = 0.20f;
    }

    at->phase    = KILN_TUNE_APPROACH;
    at->relay_on = true;

    /* The procedure will run, but not where it was asked to: say so. */
    return (at->cfg.setpoint_c < requested) ? KILN_ERR_RANGE : KILN_OK;
}

void kiln_autotune_cancel(kiln_autotune_t *at)
{
    if (at == nullptr) {
        return;
    }
    at->phase       = KILN_TUNE_FAILED;
    at->fail_reason = KILN_FAULT_OPERATOR_ABORT;
}

kiln_gains_t kiln_autotune_gains_from(float ku, float tu, kiln_tune_rule_t rule)
{
    kiln_gains_t g = {};
    if (!(ku > 0.0f) || !(tu > 0.0f)) {
        return g;
    }

    float kp, ti, td;
    switch (rule) {
    case KILN_TUNE_RULE_ZN:
        kp = 0.60f * ku;
        ti = 0.50f * tu;
        td = 0.125f * tu;
        break;
    case KILN_TUNE_RULE_TL:
    default:
        kp = ku / 2.2f;
        ti = 2.2f * tu;
        td = tu / 6.3f;
        break;
    }

    g.kp = kp;
    g.ki = ti > 0.0f ? kp / ti : 0.0f;
    g.kd = kp * td;
    return g;
}

namespace {

/* Record a completed cycle, measured peak to peak. */
void push_cycle(kiln_autotune_t *at, float period_s, float amplitude_c)
{
    if (at->cycle_count < KILN_TUNE_MAX_CYCLES) {
        at->cycles[at->cycle_count].period_s    = period_s;
        at->cycles[at->cycle_count].amplitude_c = amplitude_c;
        at->cycle_count++;
    }
}

/* FR-TUN-05: discard the first cycle, then require the rest to agree. */
bool qualify(const kiln_autotune_t *at, float *ku_out, float *tu_out)
{
    if (at->cycle_count < (uint8_t)(at->cfg.required_cycles + 1u)) {
        return false;
    }

    const kiln_tune_cycle_t *c = &at->cycles[1];          /* skip the first */
    const uint8_t n = (uint8_t)(at->cycle_count - 1u);

    float p_min = c[0].period_s,    p_max = c[0].period_s;
    float a_min = c[0].amplitude_c, a_max = c[0].amplitude_c;
    double p_sum = 0.0, a_sum = 0.0;

    for (uint8_t i = 0; i < n; i++) {
        if (c[i].period_s < p_min) {
            p_min = c[i].period_s;
        }
        if (c[i].period_s > p_max) {
            p_max = c[i].period_s;
        }
        if (c[i].amplitude_c < a_min) {
            a_min = c[i].amplitude_c;
        }
        if (c[i].amplitude_c > a_max) {
            a_max = c[i].amplitude_c;
        }
        p_sum += c[i].period_s;
        a_sum += c[i].amplitude_c;
    }

    if (!(p_min > 0.0f) || !(a_min > 0.0f)) {
        return false;
    }
    if ((p_max - p_min) / p_min > at->cfg.period_tol) {
        return false;
    }
    if ((a_max - a_min) / a_min > at->cfg.amplitude_tol) {
        return false;
    }

    const float tu = (float)(p_sum / (double)n);
    const float a  = (float)(a_sum / (double)n);
    const float d  = (float)at->cfg.amplitude_permille / 10.0f;   /* percent */

    if (!(a > 0.0f)) {
        return false;
    }

    *tu_out = tu;
    *ku_out = 4.0f * d / (KILN_PI * a);
    return true;
}

/* Relay with hysteresis about the tuning setpoint. */
void relay_update(kiln_autotune_t *at, float pv_c)
{
    const float sp = at->cfg.setpoint_c;
    const float h  = at->cfg.hysteresis_c;

    if (pv_c < sp - h) {
        at->relay_on = true;
    }
    else if (pv_c > sp + h) {
        at->relay_on = false;
    }
    /* inside the band: hold, which is what makes it a relay and not a comparator */
}

/* Peak and trough detection, hysteresis-gated so noise cannot manufacture
 * extrema. */
void track_extremes(kiln_autotune_t *at, float pv_c)
{
    /* Its own threshold, not the relay band -- see peak_threshold_c. */
    const float h = at->cfg.peak_threshold_c;

    if (!at->have_extreme) {
        at->have_extreme = true;
        at->extreme_c    = pv_c;
        at->rising       = true;
        return;
    }

    if (at->rising) {
        if (pv_c > at->extreme_c) {
            at->extreme_c = pv_c;
        } else if (pv_c < at->extreme_c - h) {
            /* Confirmed peak at extreme_c. */
            if (at->have_peak) {
                const float period = at->elapsed_s - at->last_peak_t_s;
                if (at->have_trough) {
                    const float amp = (at->extreme_c - at->last_trough_c) * 0.5f;
                    push_cycle(at, period, amp);
                }
            }
            at->last_peak_c   = at->extreme_c;
            at->last_peak_t_s = at->elapsed_s;
            at->have_peak     = true;

            at->rising    = false;
            at->extreme_c = pv_c;
        }
    } else {
        if (pv_c < at->extreme_c) {
            at->extreme_c = pv_c;
        } else if (pv_c > at->extreme_c + h) {
            at->last_trough_c = at->extreme_c;
            at->have_trough   = true;

            at->rising    = true;
            at->extreme_c = pv_c;
        }
    }
}

} // namespace

uint16_t kiln_autotune_tick(kiln_autotune_t *at, float pv_c,
                            float rate_c_per_h, float dt_s)
{
    if (at == nullptr) {
        return 0;
    }

    /* NFR-17 / SR-01: extreme_c and last_pv_c are persistent state, and a NaN
     * admitted into them never leaves.  No heat, and the caller is told by the
     * counter rather than by a quietly wrong tuning result. */
    if (!kiln_is_finite(dt_s) || dt_s <= 0.0f ||
        !kiln_is_finite(pv_c) || !kiln_is_finite(rate_c_per_h)) {
        at->bad_calls++;
        return 0;
    }

    if (at->phase == KILN_TUNE_IDLE || at->phase == KILN_TUNE_FAILED ||
        at->phase == KILN_TUNE_PRESENT) {
        return 0;
    }

    at->elapsed_s       += dt_s;
    at->phase_elapsed_s += dt_s;

    /* FR-TUN-07: one timeout covers the whole procedure. */
    if (at->elapsed_s > at->cfg.timeout_s) {
        at->phase       = KILN_TUNE_FAILED;
        at->fail_reason = KILN_FAULT_TUNE_NO_CONVERGE;
        return 0;
    }

    at->last_pv_c = pv_c;

    switch (at->phase) {
    case KILN_TUNE_APPROACH:
        if (pv_c >= at->cfg.setpoint_c - at->cfg.hysteresis_c) {
            at->phase           = KILN_TUNE_SETTLE;
            at->phase_elapsed_s = 0.0f;
            at->relay_on        = false;
        }
        return at->cfg.amplitude_permille;

    case KILN_TUNE_SETTLE:
        /* Wait for the approach transient to die away, but do not wait forever.
         * The rate is the regressed one from tempfilt: see the header. */
        if (fabsf(rate_c_per_h) < at->cfg.settle_rate_c_per_h ||
            at->phase_elapsed_s > at->cfg.settle_max_s) {
            at->phase           = KILN_TUNE_RELAY;
            at->phase_elapsed_s = 0.0f;
            at->have_extreme    = false;
            at->have_peak       = false;
            at->have_trough     = false;
            at->cycle_count     = 0;
        }
        relay_update(at, pv_c);
        return at->relay_on ? at->cfg.amplitude_permille : 0u;

    case KILN_TUNE_RELAY:
        relay_update(at, pv_c);
        track_extremes(at, pv_c);

        /* Enough cycles to try: hand over to IDENTIFY, which is a real phase
         * rather than a declared-and-never-entered one.  The relay keeps running
         * through it, because qualification may well say "not yet". */
        if (at->cycle_count >= (uint8_t)(at->cfg.required_cycles + 1u)) {
            at->phase           = KILN_TUNE_IDENTIFY;
            at->phase_elapsed_s = 0.0f;
        }
        return at->relay_on ? at->cfg.amplitude_permille : 0u;

    case KILN_TUNE_IDENTIFY: {
        /* FR-TUN-05 qualification and the Ku/Tu identification.  Keep driving the
         * relay: dropping the output for a cycle here would perturb the very
         * oscillation being measured. */
        relay_update(at, pv_c);
        track_extremes(at, pv_c);

        float ku = 0.0f, tu = 0.0f;
        if (qualify(at, &ku, &tu)) {
            at->ku = ku;
            at->tu = tu;
            for (int r = 0; r < KILN_TUNE_RULE_COUNT; r++) {
                at->gains[r] = kiln_autotune_gains_from(ku, tu, (kiln_tune_rule_t)r);
            }
            at->phase = KILN_TUNE_PRESENT;   /* FR-TUN-09: nothing stored yet */
            return 0;
        }

        /* Not consistent enough.  Drop the oldest cycle, go back to collecting,
         * and let FR-TUN-07's timeout decide when to give up. */
        if (at->cycle_count >= KILN_TUNE_MAX_CYCLES) {
            for (uint8_t i = 1; i < at->cycle_count; i++) {
                at->cycles[i - 1] = at->cycles[i];
            }
            at->cycle_count--;
        }
        at->phase           = KILN_TUNE_RELAY;
        at->phase_elapsed_s = 0.0f;
        return at->relay_on ? at->cfg.amplitude_permille : 0u;
    }

    case KILN_TUNE_PRESENT:
    case KILN_TUNE_FAILED:
    case KILN_TUNE_IDLE:
        break;
    }
    return 0;
}

const char *kiln_autotune_phase_str(kiln_tune_phase_t phase)
{
    switch (phase) {
    case KILN_TUNE_IDLE:     return "idle";
    case KILN_TUNE_APPROACH: return "approach";
    case KILN_TUNE_SETTLE:   return "settle";
    case KILN_TUNE_RELAY:    return "relay";
    case KILN_TUNE_IDENTIFY: return "identify";
    case KILN_TUNE_PRESENT:  return "present";
    case KILN_TUNE_FAILED:   return "failed";
    }
    return "?";
}

const char *kiln_autotune_rule_str(kiln_tune_rule_t rule)
{
    switch (rule) {
    case KILN_TUNE_RULE_ZN: return "ziegler-nichols";
    case KILN_TUNE_RULE_TL: return "tyreus-luyben";
    default: break;
    }
    return "?";
}

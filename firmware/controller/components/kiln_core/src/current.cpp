/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <math.h>
#include <string.h>
#include "kiln_core/current.h"

void kiln_current_cfg_defaults(kiln_current_cfg_t *cfg)
{
    const kiln_current_cfg_t d = {
        .enabled            = true,       /* FR-CUR-12: mandatory by default   */
        .sample_rate_hz     = 4000u,      /* FR-CUR-03; see tasklist A4 on the
                                           * anti-alias corner that must match */
        .mains_hz           = 50u,
        .cycles_per_burst   = 2u,
        .settle_ms          = 20u,        /* FR-CUR-04 default                 */
        .remeasure_ms       = 250u,

        /* A 60 A / 0.5 V RMS voltage-output CT (HR-16), which is the rescaling
         * tasklist A5 asks for: 120 A per volt. */
        .ct_a_per_v         = 120.0f,
        .adc_v_per_count    = 3.3f / 4095.0f,

        .cal_gain           = 1.0f,       /* FR-CUR-06                         */
        .zero_offset_a      = 0.0f,

        .noise_floor_counts = 1.0f,       /* FR-CUR-11                         */
        .bias_min_counts    = 1600u,      /* mid rail 2048 +/- 450 counts      */
        .bias_max_counts    = 2500u,

        .mains_v            = 230.0f,     /* FR-CUR-07                         */

        .nominal_a          = 30.0f,
        .ref_cold_max_c     = 200.0f,     /* FR-CUR-08 "while cold"            */
        .ref_min_frac       = 0.25f,
        .ref_max_frac       = 2.00f,

        .element_tc_per_c   = 0.0f,       /* SR-28, OQ-07: off until entered   */
    };
    *cfg = d;
}

namespace {

/* Clamp the configuration into the ranges the requirements state, and report
 * whether anything had to be moved -- NFR-17 calls a silently corrected
 * configuration a defect, so the caller is told. */
bool clamp_cfg(kiln_current_cfg_t *c)
{
    bool moved = false;

    if (c->sample_rate_hz < KILN_CUR_RATE_MIN_HZ) {
        c->sample_rate_hz = KILN_CUR_RATE_MIN_HZ;
        moved             = true;
    }
    if (c->sample_rate_hz > 100000u) {
        c->sample_rate_hz = 100000u;
        moved             = true;
    }
    if (c->mains_hz != 50u && c->mains_hz != 60u) {
        c->mains_hz = 50u;
        moved       = true;
    }
    if (c->cycles_per_burst == 0u) {
        c->cycles_per_burst = 1u;
        moved               = true;
    }
    if (c->settle_ms > 200u) {
        c->settle_ms = 200u; /* FR-CUR-04 */
        moved        = true;
    }
    if (c->remeasure_ms == 0u) {
        c->remeasure_ms = 250u;
        moved           = true;
    }

    moved |= kiln_clampf_moved(&c->cal_gain, 0.50f, 2.00f); /* FR-CUR-06 */
    if (!kiln_is_finite(c->zero_offset_a)) {
        c->zero_offset_a = 0.0f;
        moved            = true;
    }
    if (!kiln_is_finite(c->ct_a_per_v) || c->ct_a_per_v <= 0.0f) {
        c->ct_a_per_v = 120.0f;
        moved         = true;
    }
    if (!kiln_is_finite(c->adc_v_per_count) || c->adc_v_per_count <= 0.0f) {
        c->adc_v_per_count = 3.3f / 4095.0f;
        moved              = true;
    }
    moved |= kiln_clampf_moved(&c->noise_floor_counts, 0.0f, 100.0f);
    if (c->bias_max_counts <= c->bias_min_counts) {
        c->bias_min_counts = 1600u;
        c->bias_max_counts = 2500u;
        moved              = true;
    }
    moved |= kiln_clampf_moved(&c->mains_v, 0.0f, 500.0f);
    moved |= kiln_clampf_moved(&c->nominal_a, 0.0f, 200.0f);
    moved |= kiln_clampf_moved(&c->ref_cold_max_c, 20.0f, 600.0f);
    moved |= kiln_clampf_moved(&c->ref_min_frac, 0.05f, 0.95f);
    moved |= kiln_clampf_moved(&c->ref_max_frac, 1.05f, 5.0f);
    /* A plausible band for metallic and SiC elements; 0 disables. */
    moved |= kiln_clampf_moved(&c->element_tc_per_c, 0.0f, 0.01f);

    return moved;
}

void derive(kiln_current_t *c)
{
    /* FR-CUR-03: a whole number of mains cycles.  Rounded up, so the burst is
     * never short of a cycle; the reduction trims to whole cycles using the rate
     * the front end actually achieved. */
    const uint32_t per_cycle = (c->cfg.sample_rate_hz + c->cfg.mains_hz - 1u) / c->cfg.mains_hz;
    uint32_t n = per_cycle * c->cfg.cycles_per_burst;
    if (n > KILN_CUR_BURST_MAX) {
        n = KILN_CUR_BURST_MAX;
    }
    if (n < 2u) {
        n = 2u;
    }
    c->samples_per_burst = (uint16_t)n;

    c->amps_per_count = c->cfg.adc_v_per_count * c->cfg.ct_a_per_v;
}

} // namespace

kiln_err_t kiln_current_init(kiln_current_t *c, const kiln_current_cfg_t *cfg)
{
    if ((c == nullptr) || (cfg == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    const kiln_current_t zero = {};
    *c = zero;
    c->cfg = *cfg;
    const bool corrected = clamp_cfg(&c->cfg);
    derive(c);
    c->gate = KILN_CUR_GATE_SKIPPED;   /* nothing measured yet */

    return corrected ? KILN_ERR_RANGE : KILN_OK;
}

kiln_err_t kiln_current_reconfigure(kiln_current_t *c, const kiln_current_cfg_t *cfg)
{
    if ((c == nullptr) || (cfg == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    c->cfg = *cfg;
    const bool corrected = clamp_cfg(&c->cfg);
    derive(c);

    /* A changed scale invalidates a reference learned under the old one. */
    c->ref_valid      = false;
    c->ref_pool_count = 0;

    return corrected ? KILN_ERR_RANGE : KILN_OK;
}

void kiln_current_begin_run(kiln_current_t *c)
{
    if (c == nullptr) {
        return;
    }

    c->ref_valid        = false;
    c->ref_rejected     = false;
    c->ref_pool_count   = 0;
    c->ref_a            = 0.0f;
    c->ref_temp_c       = 0.0f;
    c->energy_wh        = 0.0;
    c->apparent_va      = 0.0f;
    c->measurements     = 0;
    c->skipped          = 0;
    c->current_a        = 0.0f;
    c->conduction_a     = 0.0f;
    c->flags            = 0;
    c->have_window      = false;
    c->gate             = KILN_CUR_GATE_SKIPPED;
    c->window_age_ms    = 0.0f;
    c->since_measure_ms = 0.0f;
}

/* --- gating, FR-CUR-04 and FR-CUR-05 ------------------------------------ */

namespace {

/* Milliseconds one burst occupies at the configured rate. */
uint32_t burst_ms(const kiln_current_t *c)
{
    const uint32_t ms = ((uint32_t)c->samples_per_burst * 1000u + c->cfg.sample_rate_hz - 1u)
                      / c->cfg.sample_rate_hz;
    return (ms != 0u) ? ms : 1u;
}

void mark_skipped(kiln_current_t *c)
{
    c->gate   = KILN_CUR_GATE_SKIPPED;
    c->flags  = (uint8_t)(KILN_CURF_SKIPPED | KILN_CURF_STALE);
    c->skipped++;
}

} // namespace

kiln_cur_action_t kiln_current_tick(kiln_current_t *c,
                                    kiln_cur_window_t window,
                                    uint32_t window_remaining_ms,
                                    uint32_t dt_ms,
                                    uint16_t *n_samples_out)
{
    if (n_samples_out != nullptr) {
        *n_samples_out = 0;
    }
    if (c == nullptr) {
        return KILN_CUR_ACT_NONE;
    }

    if (!c->cfg.enabled) {           /* FR-CUR-12 */
        c->flags     = 0;
        c->current_a = 0.0f;
        return KILN_CUR_ACT_NONE;
    }

    const uint32_t b_ms = burst_ms(c);

    /* A new commanded interval.  FR-CUR-05: decide here whether it can be
     * measured at all, so a window too short to hold a settle plus a whole
     * mains cycle is *skipped* and never reported as zero current. */
    if (!c->have_window || window != c->window) {
        const bool was_armed = (c->gate == KILN_CUR_GATE_ARMED);

        c->have_window      = true;
        c->window           = window;
        c->window_age_ms    = 0.0f;
        c->since_measure_ms = 0.0f;

        if (window_remaining_ms < (uint32_t)c->cfg.settle_ms + b_ms) {
            mark_skipped(c);
        } else {
            c->gate = KILN_CUR_GATE_WAIT_SETTLE;
        }
        /* A burst in flight belonged to the interval that just ended: discard
         * it rather than reduce samples straddling the switching edge. */
        return was_armed ? KILN_CUR_ACT_ABORT : KILN_CUR_ACT_NONE;
    }

    c->window_age_ms    += (float)dt_ms;
    c->since_measure_ms += (float)dt_ms;

    switch (c->gate) {
    case KILN_CUR_GATE_WAIT_SETTLE:
        if (c->window_age_ms < (float)c->cfg.settle_ms) {
            return KILN_CUR_ACT_NONE;
        }
        if (window_remaining_ms < b_ms) {   /* shorter than it promised */
            mark_skipped(c);
            return KILN_CUR_ACT_NONE;
        }
        c->gate = KILN_CUR_GATE_ARMED;
        if (n_samples_out != nullptr) {
            *n_samples_out = c->samples_per_burst;
        }
        return KILN_CUR_ACT_START_BURST;

    case KILN_CUR_GATE_MEASURED:
        /* Re-measure inside a long or unbounded interval; see remeasure_ms. */
        if (c->since_measure_ms < (float)c->cfg.remeasure_ms) {
            return KILN_CUR_ACT_NONE;
        }
        if (window_remaining_ms < b_ms) {
            return KILN_CUR_ACT_NONE;
        }
        c->gate = KILN_CUR_GATE_ARMED;
        if (n_samples_out != nullptr) {
            *n_samples_out = c->samples_per_burst;
        }
        return KILN_CUR_ACT_START_BURST;

    case KILN_CUR_GATE_ARMED:
    case KILN_CUR_GATE_SKIPPED:
    default:
        return KILN_CUR_ACT_NONE;
    }
}

/* --- reduction, FR-CUR-02, FR-CUR-03, FR-CUR-11 ------------------------- */

namespace {

/* True RMS of the AC component.  The DC bias is measured rather than assumed,
 * because HR-17's mid-rail divider is a real resistor pair whose centre moves
 * with temperature and supply -- and because where it sits is itself the
 * FR-CUR-11 evidence that a transformer is connected at all. */
void reduce(const kiln_cur_burst_t *b, uint16_t n,
                   float *bias_out, float *rms_out)
{
    double sum = 0.0;
    for (uint16_t i = 0; i < n; i++) {
        sum += (double)b->samples[i];
    }
    const double mean = sum / (double)n;

    double ss = 0.0;
    for (uint16_t i = 0; i < n; i++) {
        const double d = (double)b->samples[i] - mean;
        ss += d * d;
    }

    *bias_out = (float)mean;
    *rms_out  = (float)sqrt(ss / (double)n);
}

/* Keep the pool sorted on insert, so the median is just the middle element and
 * there is no scratch buffer or qsort in the core. */
void ref_pool_insert(kiln_current_t *c, float a)
{
    if (c->ref_pool_count >= KILN_CUR_REF_SAMPLES) {
        return;
    }

    uint8_t i = c->ref_pool_count;
    while (i > 0 && c->ref_pool[i - 1] > a) {
        c->ref_pool[i] = c->ref_pool[i - 1];
        i--;
    }
    c->ref_pool[i] = a;
    c->ref_pool_count++;

    if (c->ref_pool_count == KILN_CUR_REF_SAMPLES) {
        const float median = c->ref_pool[KILN_CUR_REF_SAMPLES / 2];
        const float lo = c->cfg.nominal_a * c->cfg.ref_min_frac;
        const float hi = c->cfg.nominal_a * c->cfg.ref_max_frac;

        /* See ref_min_frac: a reference learned from a kiln that is not drawing
         * current would make SR-26 agree with the fault. */
        if (median >= lo && median <= hi) {
            c->ref_a      = median;
            c->ref_temp_c = c->plant_c;
            c->ref_valid  = true;
        } else {
            c->ref_rejected = true;
        }
    }
}

} // namespace

kiln_err_t kiln_current_push_burst(kiln_current_t *c, const kiln_cur_burst_t *b)
{
    if ((c == nullptr) || (b == nullptr) || (b->samples == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    if (!c->cfg.enabled) {
        return KILN_ERR_STATE;
    }

    /* Only a burst this component asked for, for the interval it is still in. */
    if (c->gate != KILN_CUR_GATE_ARMED) {
        return KILN_ERR_STATE;
    }
    if (b->window != c->window) {
        mark_skipped(c);
        return KILN_ERR_STATE;
    }

    const uint8_t window_flag = (c->window == KILN_CUR_WINDOW_ON)
                              ? KILN_CURF_CONDUCTION : KILN_CURF_LEAKAGE;

    if (b->truncated || b->count < 2u) {
        /* FR-CUR-05 again, after the fact: the interval closed under the burst. */
        mark_skipped(c);
        c->flags = (uint8_t)(c->flags | window_flag);
        return KILN_OK;
    }

    uint16_t n = b->count > KILN_CUR_BURST_MAX ? KILN_CUR_BURST_MAX : b->count;

    /* FR-CUR-03: trim to a whole number of mains cycles at the rate the front
     * end actually achieved, not the one that was requested -- that is the whole
     * reason the burst reports its own rate. */
    const uint32_t rate = (b->sample_rate_hz != 0u) ? b->sample_rate_hz : c->cfg.sample_rate_hz;
    const uint32_t per_cycle = rate / (uint32_t)c->cfg.mains_hz;
    if (per_cycle >= 2u && n >= per_cycle) {
        n = (uint16_t)((n / per_cycle) * per_cycle);
    }

    float bias = 0.0f, rms = 0.0f;
    reduce(b, n, &bias, &rms);

    c->bias_counts      = bias;
    c->rms_counts       = rms;
    c->gate             = KILN_CUR_GATE_MEASURED;
    c->since_measure_ms = 0.0f;
    c->measurements++;

    /* FR-CUR-11.  Two independent symptoms of "no transformer", neither of which
     * a genuine zero current produces: the input has left the bias window the
     * conditioning holds it in (an open input has no DC path -- tasklist A6), or
     * there is no AC at all, not even the noise floor a live winding always
     * contributes. */
    if (bias < (float)c->cfg.bias_min_counts || bias > (float)c->cfg.bias_max_counts ||
        rms < c->cfg.noise_floor_counts) {
        /* Void, not zero: the window flag is kept so the log says which interval
         * it was void in, and safety gives the CT fault precedence over the
         * rules that would otherwise read 0 A as a dead element. */
        c->current_a = 0.0f;
        c->flags     = (uint8_t)(KILN_CURF_CT_FAULT | window_flag);
        return KILN_OK;
    }

    /* FR-CUR-02 / FR-CUR-06 */
    float amps = rms * c->amps_per_count * c->cfg.cal_gain - c->cfg.zero_offset_a;
    if (!kiln_is_finite(amps) || amps < 0.0f) {
        amps = 0.0f;
    }

    c->current_a = amps;
    c->flags     = window_flag;

    if (window_flag == KILN_CURF_CONDUCTION) {
        c->conduction_a = amps;
        c->apparent_va  = amps * c->cfg.mains_v;      /* FR-CUR-07 */

        /* FR-CUR-08: the reference is the median of conduction measurements
         * taken while the elements are cold and fully on.  Both conditions
         * matter -- cold because of the temperature coefficient, fully on
         * because a partial window measures a different thing. */
        if (!c->ref_valid && !c->ref_rejected &&
            c->plant_duty_permille >= KILN_DUTY_MAX &&
            c->plant_c <= c->cfg.ref_cold_max_c) {
            ref_pool_insert(c, amps);
        }
    }

    return KILN_OK;
}

void kiln_current_note_plant(kiln_current_t *c, float kiln_c,
                             uint16_t duty_permille, float dt_s)
{
    if (c == nullptr) {
        return;
    }

    c->plant_c             = kiln_sanitisef(kiln_c, c->plant_c);
    c->plant_duty_permille = duty_permille > KILN_DUTY_MAX ? KILN_DUTY_MAX : duty_permille;

    if (!c->cfg.enabled || !kiln_is_finite(dt_s) || dt_s <= 0.0f) {
        return;
    }

    /* FR-CUR-07.  Current only flows during the commanded-on fraction of the
     * window, so the integral is weighted by duty rather than by wall time.  A
     * resistive load is assumed, which is stated wherever the figure appears. */
    const float duty_frac = (float)c->plant_duty_permille / (float)KILN_DUTY_MAX;
    const float va        = c->conduction_a * c->cfg.mains_v;
    c->energy_wh += (double)va * (double)duty_frac * (double)dt_s / 3600.0;
}

void kiln_current_mark_stale(kiln_current_t *c)
{
    if (c == nullptr) {
        return;
    }
    c->flags = (uint8_t)(c->flags | KILN_CURF_STALE);
}

kiln_err_t kiln_current_calibrate(kiln_current_t *c, float known_a)
{
    if (c == nullptr) {
        return KILN_ERR_INVALID_ARG;
    }
    if (!kiln_is_finite(known_a) || known_a <= 0.0f) {
        return KILN_ERR_INVALID_ARG;
    }

    /* FR-CUR-06 is a one-point calibration against a *load*: it needs a
     * conduction measurement that is actually valid.  Calibrating against a
     * leakage window, a skipped window or a CT fault would produce a gain with
     * no relation to anything, and would do it silently. */
    if (((c->flags & KILN_CURF_CONDUCTION) == 0u) ||
        ((c->flags & (KILN_CURF_CT_FAULT | KILN_CURF_SKIPPED | KILN_CURF_STALE)) != 0u)) {
        return KILN_ERR_STATE;
    }

    const float uncal = c->rms_counts * c->amps_per_count;
    if (!(uncal > 0.0f)) {
        return KILN_ERR_STATE;
    }

    const float gain = (known_a + c->cfg.zero_offset_a) / uncal;
    if (gain < 0.50f || gain > 2.00f) {
        return KILN_ERR_RANGE; /* FR-CUR-06 */
    }

    c->cfg.cal_gain = gain;
    c->current_a    = known_a;
    c->conduction_a = known_a;

    /* A reference learned under the old gain is no longer the same quantity. */
    c->ref_valid      = false;
    c->ref_pool_count = 0;
    return KILN_OK;
}

bool kiln_current_deviation(const kiln_current_t *c, float *deviation_out)
{
    if ((c == nullptr) || (deviation_out == nullptr)) {
        return false;
    }
    if (!c->ref_valid || !(c->ref_a > 0.0f)) {
        return false;
    }
    if ((c->flags & KILN_CURF_CONDUCTION) == 0u) {
        return false;
    }
    if ((c->flags & (KILN_CURF_CT_FAULT | KILN_CURF_SKIPPED | KILN_CURF_STALE)) != 0u) {
        return false;
    }

    /* SR-28: elements gain resistance as they heat, so the expected current at
     * temperature is below the cold reference.  Comparing against the raw cold
     * figure would accuse a perfectly good kiln of having lost a group. */
    float expected = c->ref_a;
    if (c->cfg.element_tc_per_c > 0.0f) {
        const float dT = c->plant_c - c->ref_temp_c;
        const float r  = 1.0f + c->cfg.element_tc_per_c * dT;
        if (r > 0.1f) {
            expected = c->ref_a / r;
        }
    }
    if (!(expected > 0.0f)) {
        return false;
    }

    *deviation_out = (c->conduction_a - expected) / expected;
    return kiln_is_finite(*deviation_out);
}

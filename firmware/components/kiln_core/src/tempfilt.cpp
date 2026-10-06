/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "kiln_core/tempfilt.h"

namespace {

void clamp_cfg(kiln_tempfilt_cfg_t *c)
{
    c->offset_c     = kiln_clampf(c->offset_c, -50.0f, 50.0f);
    c->gain         = kiln_clampf(c->gain, 0.90f, 1.10f);
    c->filter_tau_s = kiln_clampf(c->filter_tau_s, 0.0f, 30.0f);
    if (c->rate_window_s < KILN_RATE_WINDOW_MIN_S) {
        c->rate_window_s = KILN_RATE_WINDOW_MIN_S;
    }
    if (c->rate_window_s > KILN_RATE_WINDOW_MAX_S) {
        c->rate_window_s = KILN_RATE_WINDOW_MAX_S;
    }
}

} // namespace

void kiln_tempfilt_init(kiln_tempfilt_t *f, const kiln_tempfilt_cfg_t *cfg)
{
    const kiln_tempfilt_t zero = {};
    *f = zero;
    f->cfg = *cfg;
    if (f->cfg.gain == 0.0f) {
        f->cfg.gain = 1.0f;
    }
    clamp_cfg(&f->cfg);
}

void kiln_tempfilt_reconfigure(kiln_tempfilt_t *f, const kiln_tempfilt_cfg_t *cfg)
{
    f->cfg = *cfg;
    if (f->cfg.gain == 0.0f) {
        f->cfg.gain = 1.0f;
    }
    clamp_cfg(&f->cfg);
}

void kiln_tempfilt_reset(kiln_tempfilt_t *f)
{
    f->primed        = false;
    f->hist_count    = 0;
    f->hist_head     = 0;
    f->decim_accum_s = 0.0f;
    f->prev_filt_c   = 0.0f;
    f->rate_c_per_h  = 0.0f;
}

namespace {

/* Least-squares slope over the decimated history, in degC per hour.
 * Samples are 1/KILN_DECIM_HZ apart by construction, so x is the sample index.
 *
 * Single precision throughout: the ESP32-S3 has a single-precision FPU, so a
 * double here is software-emulated, and this walks up to 300 points.  The
 * accumulators are the only concern, and at 300 samples of at most 1350 degC the
 * largest term is ~1.2e8 against float's ~1.7e38 -- no overflow, and the slope
 * is wanted to a tenth of a degC per hour, not to machine precision. */
float regress_rate_per_h(const kiln_tempfilt_t *f)
{
    const uint16_t n_want = (uint16_t)(f->cfg.rate_window_s * KILN_DECIM_HZ);
    const uint16_t n = f->hist_count < n_want ? f->hist_count : n_want;

    if (n < 3) {
        return 0.0f; /* not enough evidence to claim a rate */
    }

    /* Walk the n most recent samples, oldest first. */
    float sum_x = 0.0f, sum_y = 0.0f, sum_xx = 0.0f, sum_xy = 0.0f;
    for (uint16_t i = 0; i < n; i++) {
        const uint16_t idx = (uint16_t)((f->hist_head + KILN_RATE_MAX_POINTS - n + i)
                                        % KILN_RATE_MAX_POINTS);
        const float x = (float)i;
        const float y = f->hist_c[idx];
        sum_x  += x;
        sum_y  += y;
        sum_xx += x * x;
        sum_xy += x * y;
    }

    const float nn    = (float)n;
    const float denom = nn * sum_xx - sum_x * sum_x;
    if (denom == 0.0f) {
        return 0.0f;
    }

    /* x is in decimation steps, so convert to seconds before scaling to hours. */
    const float slope_per_step = (nn * sum_xy - sum_x * sum_y) / denom;
    return slope_per_step * (float)KILN_DECIM_HZ * 3600.0f;
}

void hist_push(kiln_tempfilt_t *f, float v)
{
    f->hist_c[f->hist_head] = v;
    f->hist_head = (uint16_t)((f->hist_head + 1u) % KILN_RATE_MAX_POINTS);
    if (f->hist_count < KILN_RATE_MAX_POINTS) {
        f->hist_count++;
    }
}

} // namespace

bool kiln_tempfilt_push(kiln_tempfilt_t *f, float raw_sensor_c, float dt_s)
{
    if (f == nullptr) {
        return false;
    }

    /* NFR-17 / SR-01: refuse rather than poison persistent state. */
    if (!kiln_is_finite(raw_sensor_c) || !kiln_is_finite(dt_s)) {
        f->rejected++;
        return false;
    }

    /* FR-ACQ-08: gain then offset, applied before anything else uses the value. */
    f->raw_c = raw_sensor_c * f->cfg.gain + f->cfg.offset_c;

    /* FR-ACQ-07: first order low pass, exact-enough discrete form. */
    const float prev = f->primed ? f->filt_c : f->raw_c;
    if (!f->primed) {
        f->filt_c      = f->raw_c;
        f->prev_filt_c = f->raw_c;
        f->primed      = true;
    } else if (f->cfg.filter_tau_s > 0.0f && dt_s > 0.0f) {
        const float alpha = dt_s / (f->cfg.filter_tau_s + dt_s);
        f->filt_c += alpha * (f->raw_c - f->filt_c);
    } else {
        f->filt_c = f->raw_c;
    }

    /* Decimate to KILN_DECIM_HZ for the rate history. */
    if (dt_s > 0.0f) {
        const float step_s    = 1.0f / (float)KILN_DECIM_HZ;
        const float accum_was = f->decim_accum_s;
        f->decim_accum_s += dt_s;

        if (f->decim_accum_s >= step_s) {
            uint16_t pushes = (uint16_t)(f->decim_accum_s / step_s);
            if (pushes > KILN_RATE_MAX_POINTS) {
                pushes = KILN_RATE_MAX_POINTS;
            }

            /* A cycle that overran covers several decimation boundaries.  Pushing
             * the same filt_c at each of them would plant duplicate points at
             * distinct x positions, which flattens the regressed slope -- and a
             * flattened slope is exactly what SR-07 reads as "not rising".
             * Interpolate across the interval instead, which is the best
             * available statement about where the temperature was. */
            for (uint16_t k = 1; k <= pushes; k++) {
                const float t_k = (float)k * step_s - accum_was;   /* into dt_s */
                const float frac = kiln_clampf(t_k / dt_s, 0.0f, 1.0f);
                hist_push(f, prev + (f->filt_c - prev) * frac);
            }
            f->decim_accum_s -= (float)pushes * step_s;
            if (f->decim_accum_s < 0.0f) {
                f->decim_accum_s = 0.0f;
            }

            /* The history only changes here, so the regression only runs here.
             * At 4 Hz acquisition (FR-ACQ-03) three of every four pushes used to
             * repeat an identical 300-point walk for nothing. */
            f->rate_c_per_h = regress_rate_per_h(f);
        }
    }

    f->prev_filt_c = f->filt_c;
    return true;
}

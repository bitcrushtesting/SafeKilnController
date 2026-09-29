/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "kiln_core/tempfilt.h"

static void clamp_cfg(kiln_tempfilt_cfg_t *c)
{
    c->offset_c     = kiln_clampf(c->offset_c, -50.0f, 50.0f);
    c->gain         = kiln_clampf(c->gain, 0.90f, 1.10f);
    c->filter_tau_s = kiln_clampf(c->filter_tau_s, 0.0f, 30.0f);
    if (c->rate_window_s < 10)  c->rate_window_s = 10;
    if (c->rate_window_s > KILN_RATE_MAX_POINTS) c->rate_window_s = KILN_RATE_MAX_POINTS;
}

void kiln_tempfilt_init(kiln_tempfilt_t *f, const kiln_tempfilt_cfg_t *cfg)
{
    const kiln_tempfilt_t zero = {0};
    *f = zero;
    f->cfg = *cfg;
    if (f->cfg.gain == 0.0f) f->cfg.gain = 1.0f;
    clamp_cfg(&f->cfg);
}

void kiln_tempfilt_reconfigure(kiln_tempfilt_t *f, const kiln_tempfilt_cfg_t *cfg)
{
    f->cfg = *cfg;
    if (f->cfg.gain == 0.0f) f->cfg.gain = 1.0f;
    clamp_cfg(&f->cfg);
}

void kiln_tempfilt_reset(kiln_tempfilt_t *f)
{
    f->primed        = false;
    f->hist_count    = 0;
    f->hist_head     = 0;
    f->decim_accum_s = 0.0f;
    f->rate_c_per_h  = 0.0f;
}

/* Least-squares slope over the decimated history, in degC per hour.
 * Samples are 1 s apart by construction, so x is simply the sample index. */
static float regress_rate_per_h(const kiln_tempfilt_t *f)
{
    const uint16_t n_want = f->cfg.rate_window_s;
    uint16_t n = f->hist_count < n_want ? f->hist_count : n_want;

    if (n < 3) return 0.0f;     /* not enough evidence to claim a rate */

    /* Walk the n most recent samples, oldest first. */
    double sum_x = 0, sum_y = 0, sum_xx = 0, sum_xy = 0;
    for (uint16_t i = 0; i < n; i++) {
        const uint16_t idx = (uint16_t)((f->hist_head + KILN_RATE_MAX_POINTS - n + i)
                                        % KILN_RATE_MAX_POINTS);
        const double x = (double)i;         /* seconds */
        const double y = (double)f->hist_c[idx];
        sum_x  += x;
        sum_y  += y;
        sum_xx += x * x;
        sum_xy += x * y;
    }

    const double nn    = (double)n;
    const double denom = nn * sum_xx - sum_x * sum_x;
    if (denom == 0.0) return 0.0f;

    const double slope_per_s = (nn * sum_xy - sum_x * sum_y) / denom;
    return (float)(slope_per_s * 3600.0);
}

void kiln_tempfilt_push(kiln_tempfilt_t *f, float raw_sensor_c, float dt_s)
{
    /* FR-ACQ-08: gain then offset, applied before anything else uses the value. */
    f->raw_c = raw_sensor_c * f->cfg.gain + f->cfg.offset_c;

    /* FR-ACQ-07: first order low pass, exact-enough discrete form. */
    if (!f->primed) {
        f->filt_c = f->raw_c;
        f->primed = true;
    } else if (f->cfg.filter_tau_s > 0.0f && dt_s > 0.0f) {
        const float alpha = dt_s / (f->cfg.filter_tau_s + dt_s);
        f->filt_c += alpha * (f->raw_c - f->filt_c);
    } else {
        f->filt_c = f->raw_c;
    }

    /* Decimate to 1 Hz for the rate history. */
    if (dt_s > 0.0f) {
        f->decim_accum_s += dt_s;
        while (f->decim_accum_s >= 1.0f) {
            f->decim_accum_s -= 1.0f;
            f->hist_c[f->hist_head] = f->filt_c;
            f->hist_head = (uint16_t)((f->hist_head + 1u) % KILN_RATE_MAX_POINTS);
            if (f->hist_count < KILN_RATE_MAX_POINTS) f->hist_count++;
        }
    }

    f->rate_c_per_h = regress_rate_per_h(f);
}

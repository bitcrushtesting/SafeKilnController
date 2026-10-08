/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Calibration, filtering and rate of change -- FR-ACQ-07, FR-ACQ-08, FR-ACQ-11.
 *
 * The rate window can be 300 s and acquisition runs at 4 Hz, which would be
 * 1200 samples.  Rate is therefore regressed over samples decimated to 1 Hz:
 * 300 points, 1.2 kB, comfortably inside the core's static budget, and ample
 * resolution for a kiln whose interesting rates are tens of degC per hour.
 */
#ifndef KILN_CORE_TEMPFILT_H
#define KILN_CORE_TEMPFILT_H

#include "kiln/types.h"

/* The decimated history is a window in *seconds* sampled at a rate in hertz.
 * Deriving the point count from both keeps the two units apart: they happen to
 * be numerically equal at 1 Hz, which is exactly the kind of coincidence that
 * turns into a bug the first time the decimation rate changes. */
constexpr uint16_t KILN_RATE_WINDOW_MAX_S = 300;  /* FR-ACQ-11 maximum */
constexpr uint16_t KILN_RATE_WINDOW_MIN_S = 10;   /* FR-ACQ-11 minimum */
constexpr uint16_t KILN_DECIM_HZ          = 1;
constexpr uint16_t KILN_RATE_MAX_POINTS   = KILN_RATE_WINDOW_MAX_S * KILN_DECIM_HZ;

typedef struct {
    float    offset_c;        /* FR-ACQ-08: -50 .. +50   */
    float    gain;            /* FR-ACQ-08: 0.90 .. 1.10 */
    float    filter_tau_s;    /* FR-ACQ-07: 0 disables   */
    uint16_t rate_window_s;   /* FR-ACQ-11: 10 .. 300    */
} kiln_tempfilt_cfg_t;

typedef struct {
    kiln_tempfilt_cfg_t cfg;

    float raw_c;              /* calibrated, unfiltered */
    float filt_c;             /* calibrated, filtered   */
    bool  primed;

    /* 1 Hz decimated ring for the rate regression. */
    float    hist_c[KILN_RATE_MAX_POINTS];
    uint16_t hist_count;
    uint16_t hist_head;       /* next write position */
    float    decim_accum_s;
    float    prev_filt_c;     /* value at the previous push, for interpolation */

    float rate_c_per_h;
    uint32_t rejected;        /* non-finite samples refused (NFR-17) */
} kiln_tempfilt_t;

void  kiln_tempfilt_init(kiln_tempfilt_t *f, const kiln_tempfilt_cfg_t *cfg);
void  kiln_tempfilt_reconfigure(kiln_tempfilt_t *f, const kiln_tempfilt_cfg_t *cfg);
void  kiln_tempfilt_reset(kiln_tempfilt_t *f);

/* Feed one acquisition.  raw_sensor_c is what the front end reported.
 *
 * Returns false, leaving every piece of state untouched, for a non-finite
 * sample.  This is the acquisition boundary of SR-01 and NFR-17: filt_c is
 * persistent state, so one NaN admitted here would poison the filter, the rate
 * regression and every rule downstream of them for the rest of the run.  A false
 * return is a front-end fault and the caller must treat it as one. */
bool  kiln_tempfilt_push(kiln_tempfilt_t *f, float raw_sensor_c, float dt_s);

static inline float kiln_tempfilt_raw(const kiln_tempfilt_t *f)  { return f->raw_c; }
static inline float kiln_tempfilt_filt(const kiln_tempfilt_t *f) { return f->filt_c; }
static inline float kiln_tempfilt_rate(const kiln_tempfilt_t *f) { return f->rate_c_per_h; }

#endif

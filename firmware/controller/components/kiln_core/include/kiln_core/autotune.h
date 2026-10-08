/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Relay autotune -- architecture section 7.4, SWR-TUN-01..SWR-TUN-11.
 *
 * Astrom-Haegglund: drive a bounded on/off limit cycle about a tuning setpoint,
 * measure its amplitude and period, and read the ultimate gain off the
 * describing-function estimate
 *
 *     Ku = 4d / (pi * a)
 *
 * with d the relay half amplitude in percent duty and a the half amplitude of
 * the resulting temperature oscillation in degC.  Gains then follow from a
 * published rule (SWR-TUN-06).
 *
 * This component only ever *requests* a duty; safety supervision applies
 * throughout, exactly as during a normal firing.
 */
#ifndef KILN_CORE_AUTOTUNE_H
#define KILN_CORE_AUTOTUNE_H

#include "kiln/err.h"
#include "kiln/types.h"

constexpr size_t KILN_TUNE_MAX_CYCLES = 8;

typedef enum {
    KILN_TUNE_IDLE = 0,
    KILN_TUNE_APPROACH,    /* heat up to the tuning setpoint     */
    KILN_TUNE_SETTLE,      /* let the transient die away         */
    KILN_TUNE_RELAY,       /* bang-bang, collecting cycles       */
    KILN_TUNE_IDENTIFY,    /* qualify the cycles, compute Ku/Tu  */
    KILN_TUNE_PRESENT,     /* results awaiting operator decision */
    KILN_TUNE_FAILED,
} kiln_tune_phase_t;

typedef enum {
    KILN_TUNE_RULE_ZN = 0,     /* Ziegler-Nichols, fast, overshoots     */
    KILN_TUNE_RULE_TL,         /* Tyreus-Luyben, conservative (default) */
    KILN_TUNE_RULE_COUNT,
} kiln_tune_rule_t;

typedef struct { float kp, ki, kd; } kiln_gains_t;

typedef struct {
    float    setpoint_c;
    /* SWR-SAF-23 requires *every* temperature setpoint, program target and tuning
     * setpoint to be clamped to the configured maximum -- not only to the
     * compile-time ceiling.  It arrives here rather than being left to the
     * caller by comment, so that it is enforced where it cannot be forgotten. */
    float    max_temp_c;
    uint16_t amplitude_permille;   /* d: SWR-TUN-04, 100..1000, default 500 */
    float    hysteresis_c;         /* SWR-TUN-04, 0.1..20, default 1        */

    /* Peak detection's confirmation threshold, independent of the relay band.
     *
     * Measured against a synthetic oscillation (see the host suite), because the
     * expectation here is easy to get backwards:
     *
     *  - On a *clean* oscillation there is no amplitude bias at all.  An extreme
     *    is confirmed once the measurement has reversed by this much, but the
     *    value recorded is the extreme itself, not the value that confirmed it --
     *    so Ku comes out exact whatever the threshold.
     *  - What the threshold actually buys is noise immunity, and it is not
     *    optional: with 0.5 degC of sensor noise a threshold of 0.25 degC lets
     *    noise manufacture extrema, cycles never agree within SWR-TUN-05's
     *    tolerances, and the procedure fails on its timeout having learned
     *    nothing.
     *  - Noise that does get through inflates the measured half-amplitude (peak
     *    plus noise, trough minus noise), and since Ku = 4d/(pi*a) that biases Ku
     *    *low* -- about 9 % low at 0.5 degC of noise on a 5 degC oscillation.
     *    Low Ku means gentler gains, which is the conservative direction.
     *
     * So: comfortably above the sensor noise, and the amplitude bias it costs is
     * in the safe direction.  0 means "use hysteresis_c". */
    float    peak_threshold_c;

    float    timeout_s;            /* SWR-TUN-07, 600..28800, default 7200  */
    uint8_t  required_cycles;      /* SWR-TUN-05, default 3 (after the first
                                    * is discarded)                        */
    float    period_tol;           /* SWR-TUN-05, default 0.15              */
    float    amplitude_tol;        /* SWR-TUN-05, default 0.20              */
    float    settle_rate_c_per_h;  /* considered settled below this        */
    float    settle_max_s;         /* give up settling after this          */
} kiln_tune_cfg_t;

void kiln_autotune_cfg_defaults(kiln_tune_cfg_t *cfg);

typedef struct {
    float period_s;
    float amplitude_c;   /* half amplitude (peak to trough) / 2 */
} kiln_tune_cycle_t;

typedef struct {
    kiln_tune_cfg_t   cfg;
    kiln_tune_phase_t phase;

    float elapsed_s;
    float phase_elapsed_s;

    /* relay state */
    bool  relay_on;
    float last_pv_c;

    /* peak tracking */
    bool  rising;
    bool  have_extreme;
    float extreme_c;        /* running max while rising, min while falling */
    float last_peak_c;
    float last_trough_c;
    bool  have_peak, have_trough;
    float last_peak_t_s;

    kiln_tune_cycle_t cycles[KILN_TUNE_MAX_CYCLES];
    uint8_t           cycle_count;   /* recorded, including the discarded first */

    uint32_t bad_calls;              /* SWR-NFR-17: contract violations by the caller */

    /* results */
    float ku, tu;
    kiln_gains_t gains[KILN_TUNE_RULE_COUNT];
    kiln_fault_t fail_reason;
} kiln_autotune_t;

/* SWR-SAF-23 is enforced here: setpoint_c is clamped to cfg->max_temp_c and to the
 * compile-time ceiling.  SWR-TUN-03's "ambient + 50 degC" lower bound still
 * belongs to the caller, which is the only party that knows ambient.
 *
 * Returns KILN_ERR_RANGE when the tuning setpoint had to be clamped: the
 * procedure will run, but not at the temperature that was asked for. */
kiln_err_t kiln_autotune_start(kiln_autotune_t *at, const kiln_tune_cfg_t *cfg);

/* One control cycle.  Returns the duty being requested, per mille.
 *
 * rate_c_per_h must be the *filtered* rate, i.e. kiln_tempfilt_rate().  A
 * single-sample difference cannot serve: at a 0.25 s cycle, 0.5 degC of sensor
 * noise is 7200 degC/h against a settle threshold of 30, so the settle test could
 * essentially never pass and SETTLE always fell through on its 30 min timeout
 * instead of on the transient having died away. */
uint16_t kiln_autotune_tick(kiln_autotune_t *at, float pv_c,
                            float rate_c_per_h, float dt_s);

void kiln_autotune_cancel(kiln_autotune_t *at);

/* Derive gains from Ku and Tu by the given rule.  Exposed separately so the
 * arithmetic is unit-testable against published values. */
kiln_gains_t kiln_autotune_gains_from(float ku, float tu, kiln_tune_rule_t rule);

static inline bool kiln_autotune_done(const kiln_autotune_t *at)
{
    return at->phase == KILN_TUNE_PRESENT || at->phase == KILN_TUNE_FAILED;
}
static inline bool kiln_autotune_succeeded(const kiln_autotune_t *at)
{
    return at->phase == KILN_TUNE_PRESENT;
}

const char *kiln_autotune_phase_str(kiln_tune_phase_t phase);
const char *kiln_autotune_rule_str(kiln_tune_rule_t rule);

#endif

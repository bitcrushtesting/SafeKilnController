/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Relay autotune -- architecture section 7.4, FR-TUN-01..FR-TUN-11.
 *
 * Astrom-Haegglund: drive a bounded on/off limit cycle about a tuning setpoint,
 * measure its amplitude and period, and read the ultimate gain off the
 * describing-function estimate
 *
 *     Ku = 4d / (pi * a)
 *
 * with d the relay half amplitude in percent duty and a the half amplitude of
 * the resulting temperature oscillation in degC.  Gains then follow from a
 * published rule (FR-TUN-06).
 *
 * This component only ever *requests* a duty; safety supervision applies
 * throughout, exactly as during a normal firing.
 */
#ifndef KILN_CORE_AUTOTUNE_H
#define KILN_CORE_AUTOTUNE_H

#include "kiln/err.h"
#include "kiln/types.h"

#define KILN_TUNE_MAX_CYCLES 8

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
    uint16_t amplitude_permille;   /* d: FR-TUN-04, 100..1000, default 500 */
    float    hysteresis_c;         /* FR-TUN-04, 0.1..20, default 1        */
    float    timeout_s;            /* FR-TUN-07, 600..28800, default 7200  */
    uint8_t  required_cycles;      /* FR-TUN-05, default 3 (after the first
                                    * is discarded)                        */
    float    period_tol;           /* FR-TUN-05, default 0.15              */
    float    amplitude_tol;        /* FR-TUN-05, default 0.20              */
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

    /* results */
    float ku, tu;
    kiln_gains_t gains[KILN_TUNE_RULE_COUNT];
    kiln_fault_t fail_reason;
} kiln_autotune_t;

/* setpoint_c must already have been range-checked by the caller against the
 * configured maximum and ambient + 50 degC (FR-TUN-03). */
kiln_err_t kiln_autotune_start(kiln_autotune_t *at, const kiln_tune_cfg_t *cfg);

/* One control cycle.  Returns the duty being requested, per mille. */
uint16_t kiln_autotune_tick(kiln_autotune_t *at, float pv_c, float dt_s);

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

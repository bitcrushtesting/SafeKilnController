/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * PID controller -- architecture section 7.2.
 *
 * Gain units are fixed and documented so a stored gain set is unambiguous:
 *   Kp  percent duty per degC
 *   Ki  percent duty per (degC * second)
 *   Kd  percent duty * second per degC
 *
 * Pure: no clock, no allocation, no globals (SWR-TST-01, SWR-TST-03, SWR-TST-04, SWR-TST-05).
 */
#ifndef KILN_CORE_PID_H
#define KILN_CORE_PID_H

#include "kiln/types.h"

constexpr float KILN_PID_KP_MAX =   100.0f;
constexpr float KILN_PID_KI_MAX =    10.0f;
constexpr float KILN_PID_KD_MAX = 10000.0f;

constexpr uint16_t KILN_PID_DUTY_MAX_MIN = 100u;  /* SWR-CTL-16 lower bound on the ceiling */

typedef struct {
    float    kp, ki, kd;
    uint16_t duty_max_permille;   /* SWR-CTL-16, 100..1000; 0 means "default" */
} kiln_pid_cfg_t;

typedef struct {
    kiln_pid_cfg_t cfg;

    float integral_pct;           /* integral term, percent duty  */
    float pv_prev_c;
    bool  primed;                 /* pv_prev_c is meaningful      */

    /* Diagnostics, published every cycle for SWR-CTL-15. */
    float error_c;
    float p_pct, i_pct, d_pct;
    float u_raw_pct;
    bool  saturated;

    /* SWR-NFR-17: calls that violated the contract below.  Non-zero is a defect
     * somewhere upstream, and is reported rather than absorbed. */
    uint32_t bad_calls;
} kiln_pid_t;

void     kiln_pid_init(kiln_pid_t *pid, const kiln_pid_cfg_t *cfg);
void     kiln_pid_set_gains(kiln_pid_t *pid, float kp, float ki, float kd);
void     kiln_pid_set_duty_max(kiln_pid_t *pid, uint16_t duty_max_permille);

/* Forget history: next update produces no derivative kick. */
void     kiln_pid_reset(kiln_pid_t *pid);

/* SWR-CTL-06: back-calculate the integral so the output continues from
 * current_duty_permille instead of stepping.
 *
 * Returns false when the transfer is *not* bumpless, which happens whenever the
 * integral the present output implies -- u minus the proportional term -- falls
 * outside [0, duty_max].  Clamping it there is the only option (an integral
 * above the ceiling is a wind-up the anti-windup logic exists to prevent), so the
 * first cycle after the transfer will step by the difference.  That is a real
 * jump of up to duty_max, most likely when switching to manual at a large
 * tracking error, and the caller is told rather than left to discover it. */
bool     kiln_pid_bumpless(kiln_pid_t *pid, uint16_t current_duty_permille,
                           float sp_c, float pv_c);

/* One cycle.  Returns duty in per mille, 0..duty_max.
 *
 * Contract: dt_s > 0, and sp_c and pv_c finite.  A violation returns duty 0,
 * leaves the integral and the pv history untouched, and increments bad_calls --
 * fail-safe rather than fail-quiet, because the alternative (holding the previous
 * output) means a NaN sensor reading or a stalled clock keeps the elements on at
 * whatever the last duty happened to be. */
uint16_t kiln_pid_update(kiln_pid_t *pid, float sp_c, float pv_c, float dt_s);

#endif

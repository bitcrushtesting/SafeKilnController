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
 * Pure: no clock, no allocation, no globals (TR-01, TR-03, TR-04, TR-05).
 */
#ifndef KILN_CORE_PID_H
#define KILN_CORE_PID_H

#include "kiln/types.h"

#define KILN_PID_KP_MAX   100.0f
#define KILN_PID_KI_MAX    10.0f
#define KILN_PID_KD_MAX 10000.0f

typedef struct {
    float    kp, ki, kd;
    uint16_t duty_max_permille;   /* FR-CTL-16, 100..1000 */
} kiln_pid_cfg_t;

typedef struct {
    kiln_pid_cfg_t cfg;

    float integral_pct;           /* integral term, percent duty  */
    float pv_prev_c;
    bool  primed;                 /* pv_prev_c is meaningful      */

    /* Diagnostics, published every cycle for FR-CTL-15. */
    float error_c;
    float p_pct, i_pct, d_pct;
    float u_raw_pct;
    bool  saturated;
} kiln_pid_t;

void     kiln_pid_init(kiln_pid_t *pid, const kiln_pid_cfg_t *cfg);
void     kiln_pid_set_gains(kiln_pid_t *pid, float kp, float ki, float kd);
void     kiln_pid_set_duty_max(kiln_pid_t *pid, uint16_t duty_max_permille);

/* Forget history: next update produces no derivative kick. */
void     kiln_pid_reset(kiln_pid_t *pid);

/* FR-CTL-06: back-calculate the integral so the output continues from
 * current_duty_permille instead of stepping. */
void     kiln_pid_bumpless(kiln_pid_t *pid, uint16_t current_duty_permille,
                           float sp_c, float pv_c);

/* One cycle.  dt_s must be > 0.  Returns duty in per mille, 0..duty_max. */
uint16_t kiln_pid_update(kiln_pid_t *pid, float sp_c, float pv_c, float dt_s);

#endif

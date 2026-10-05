/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "kiln_core/pid.h"

static float duty_max_pct(const kiln_pid_t *pid)
{
    return (float)pid->cfg.duty_max_permille / 10.0f;
}

void kiln_pid_init(kiln_pid_t *pid, const kiln_pid_cfg_t *cfg)
{
    const kiln_pid_t zero = {};
    *pid = zero;
    pid->cfg = *cfg;
    /* One range for the ceiling, the one pid.h documents: 0 means "default", and
     * everything else is bound to 100..1000 exactly as kiln_pid_set_duty_max
     * does.  Two different accepted ranges for the same field is how a gain set
     * loaded from NVS ends up with a ceiling the setter would have refused. */
    if (pid->cfg.duty_max_permille == 0) {
        pid->cfg.duty_max_permille = KILN_DUTY_MAX;
    } else {
        pid->cfg.duty_max_permille =
            kiln_clampu16(pid->cfg.duty_max_permille, KILN_PID_DUTY_MAX_MIN, KILN_DUTY_MAX);
    }
}

void kiln_pid_set_gains(kiln_pid_t *pid, float kp, float ki, float kd)
{
    pid->cfg.kp = kiln_clampf(kp, 0.0f, KILN_PID_KP_MAX);
    pid->cfg.ki = kiln_clampf(ki, 0.0f, KILN_PID_KI_MAX);
    pid->cfg.kd = kiln_clampf(kd, 0.0f, KILN_PID_KD_MAX);
}

void kiln_pid_set_duty_max(kiln_pid_t *pid, uint16_t duty_max_permille)
{
    pid->cfg.duty_max_permille =
        kiln_clampu16(duty_max_permille, KILN_PID_DUTY_MAX_MIN, KILN_DUTY_MAX);
    /* Keep a stale integral from exceeding the new ceiling. */
    pid->integral_pct = kiln_clampf(pid->integral_pct, 0.0f, duty_max_pct(pid));
}

void kiln_pid_reset(kiln_pid_t *pid)
{
    pid->integral_pct = 0.0f;
    pid->pv_prev_c    = 0.0f;
    pid->primed       = false;
    pid->error_c      = 0.0f;
    pid->p_pct = pid->i_pct = pid->d_pct = 0.0f;
    pid->u_raw_pct    = 0.0f;
    pid->saturated    = false;
}

bool kiln_pid_bumpless(kiln_pid_t *pid, uint16_t current_duty_permille,
                       float sp_c, float pv_c)
{
    if (pid == nullptr) {
        return false;
    }
    if (!kiln_is_finite(sp_c) || !kiln_is_finite(pv_c)) {
        pid->bad_calls++;
        return false;
    }

    /* I := u - P - D, with D taken as zero because pv history is discarded.
     * The next update therefore starts from exactly the present output. */
    const float u_max   = duty_max_pct(pid);
    const float u_pct   = (float)current_duty_permille / 10.0f;
    const float p_pct   = pid->cfg.kp * (sp_c - pv_c);
    const float i_exact = u_pct - p_pct;

    pid->integral_pct = kiln_clampf(i_exact, 0.0f, u_max);
    pid->pv_prev_c    = pv_c;
    pid->primed       = true;   /* no derivative kick on the first cycle back */

    /* See pid.h: outside the clamp the transfer is not bumpless, and the caller
     * is the only one who can decide what to do about a step. */
    return i_exact >= 0.0f && i_exact <= u_max;
}

uint16_t kiln_pid_update(kiln_pid_t *pid, float sp_c, float pv_c, float dt_s)
{
    if (pid == nullptr) {
        return 0;
    }

    const float u_max = duty_max_pct(pid);

    /* NFR-17 / SR-01.  A non-finite setpoint or measurement, or a non-positive
     * dt, is a defect upstream; the fail-safe answer is no heat, not the previous
     * duty.  A NaN would otherwise enter integral_pct and stay there, and
     * kiln_clampf cannot save a value that is already NaN on both sides of the
     * comparison. */
    if (!kiln_is_finite(dt_s) || dt_s <= 0.0f ||
        !kiln_is_finite(sp_c) || !kiln_is_finite(pv_c)) {
        pid->bad_calls++;
        pid->saturated = false;
        return 0;
    }

    const float e = sp_c - pv_c;

    const float p = pid->cfg.kp * e;

    /* FR-CTL-04: derivative on the measurement, so a setpoint step cannot
     * produce a derivative kick.  Negated because d(pv) opposes d(error). */
    const float d = pid->primed
                  ? -pid->cfg.kd * (pv_c - pid->pv_prev_c) / dt_s
                  : 0.0f;

    /* FR-CTL-05: integrate tentatively, then withdraw it if the output is
     * saturated and this error would only drive it further out. */
    const float i_candidate = pid->integral_pct + pid->cfg.ki * e * dt_s;
    const float u_tentative = p + i_candidate + d;

    const bool winding_up = (u_tentative > u_max && e > 0.0f) ||
                            (u_tentative < 0.0f && e < 0.0f);
    if (!winding_up) {
        pid->integral_pct = kiln_clampf(i_candidate, 0.0f, u_max);
    }

    const float u_raw = p + pid->integral_pct + d;
    const float u     = kiln_clampf(u_raw, 0.0f, u_max);

    pid->error_c   = e;
    pid->p_pct     = p;
    pid->i_pct     = pid->integral_pct;
    pid->d_pct     = d;
    pid->u_raw_pct = u_raw;
    pid->saturated = (u_raw > u_max) || (u_raw < 0.0f);

    pid->pv_prev_c = pv_c;
    pid->primed    = true;

    return (uint16_t)(u * 10.0f + 0.5f);
}

/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <string.h>
#include "kiln_core/setpoint.h"
#include "kiln_core/profile.h"

/* Upper bound on the forward simulation used for time prediction: a program is
 * capped at 168 h by FR-PRG-05, so 200 h of 1 s steps cannot be exceeded by a
 * valid program and bounds the loop for an invalid one. */
#define PREDICT_MAX_STEPS   (200u * 3600u)
#define PREDICT_DT_S        1.0f

static float move_toward(float from, float to, float step)
{
    if (from < to) {
        from += step;
        return from > to ? to : from;
    }
    if (from > to) {
        from -= step;
        return from < to ? to : from;
    }
    return to;
}

static void enter_segment(kiln_setpoint_t *st, uint8_t seg, float pv_c)
{
    st->seg           = seg;
    st->phase         = KILN_SP_PHASE_RAMP;
    st->seg_start_c   = pv_c;
    st->seg_elapsed_s = 0.0f;
}

static void advance_segment(kiln_setpoint_t *st)
{
    if (st->seg + 1u < st->prog.segment_count) {
        /* The next segment ramps from where this one ended, which is the
         * setpoint, not the measurement: a lagging kiln must not shorten the
         * next ramp. */
        enter_segment(st, (uint8_t)(st->seg + 1u), st->sp_c);
    } else {
        st->phase    = KILN_SP_PHASE_DONE;
        st->finished = true;
    }
}

kiln_err_t kiln_setpoint_start(kiln_setpoint_t *st,
                               const kiln_setpoint_cfg_t *cfg,
                               const kiln_program_t *prog,
                               float pv_c)
{
    if (!st || !cfg || !prog) return KILN_ERR_INVALID_ARG;
    if (prog->segment_count == 0 || prog->segment_count > KILN_MAX_SEGMENTS) {
        return KILN_ERR_INVALID_ARG;
    }

    const kiln_setpoint_t zero = {0};
    *st = zero;

    st->cfg  = *cfg;
    st->prog = *prog;

    /* SR-23 binds regardless of what the caller configured. */
    st->cfg.max_temp_c = kiln_clampf(st->cfg.max_temp_c, 0.0f, KILN_TEMP_CEILING_C);
    if (st->cfg.dwell_tol_c <= 0.0f) st->cfg.dwell_tol_c = 5.0f;
    if (st->cfg.holdback_band_c < 0.0f) st->cfg.holdback_band_c = 0.0f;

    /* The setpoint starts where the kiln is, so the first ramp does not begin
     * with a step the PID would have to chase. */
    st->sp_c    = kiln_clampf(pv_c, KILN_TEMP_FLOOR_C, st->cfg.max_temp_c);
    st->started = true;
    enter_segment(st, 0, st->sp_c);

    return KILN_OK;
}

void kiln_setpoint_tick(kiln_setpoint_t *st, float pv_c, float dt_s)
{
    if (!st || !st->started || st->finished || dt_s <= 0.0f) return;
    if (st->phase == KILN_SP_PHASE_AWAIT_ACK) return;   /* frozen until acked */

    /* FR-CTL-11: while the kiln is outside the hold-back band, neither the
     * setpoint nor the segment timer advances, so a slow kiln cannot silently
     * fall behind its curve. */
    if (st->cfg.holdback_band_c > 0.0f) {
        const float lag = st->sp_c > pv_c ? st->sp_c - pv_c : pv_c - st->sp_c;
        if (lag > st->cfg.holdback_band_c) {
            st->holdback_active = true;
            return;
        }
    }
    st->holdback_active = false;

    st->total_elapsed_s += dt_s;

    const kiln_segment_t *s = &st->prog.segments[st->seg];
    const float target = kiln_clampf((float)s->target_c, 0.0f, st->cfg.max_temp_c);

    switch (st->phase) {
    case KILN_SP_PHASE_RAMP:
        if (s->rate_c_per_h == 0) {
            st->sp_c = target;              /* FR-CTL-10 */
        } else {
            const float step = (float)s->rate_c_per_h * dt_s / 3600.0f;
            st->sp_c = move_toward(st->sp_c, target, step);
        }
        if (st->sp_c == target) {
            st->phase         = KILN_SP_PHASE_DWELL;
            st->seg_elapsed_s = 0.0f;
        }
        break;

    case KILN_SP_PHASE_DWELL: {
        /* FR-CTL-12: dwell time only accrues while the kiln is actually within
         * tolerance of the target. */
        const float err = pv_c > target ? pv_c - target : target - pv_c;
        if (err <= st->cfg.dwell_tol_c) {
            st->seg_elapsed_s += dt_s;
        }
        if (st->seg_elapsed_s >= (float)s->dwell_min * 60.0f) {
            if (s->flags & KILN_SEG_FLAG_REQUIRE_ACK) {
                st->phase = KILN_SP_PHASE_AWAIT_ACK;    /* FR-PRG-03 */
            } else {
                advance_segment(st);
            }
        }
        break;
    }

    case KILN_SP_PHASE_AWAIT_ACK:
    case KILN_SP_PHASE_DONE:
        break;
    }

    st->sp_c = kiln_clampf(st->sp_c, KILN_TEMP_FLOOR_C, st->cfg.max_temp_c);
}

kiln_err_t kiln_setpoint_ack(kiln_setpoint_t *st)
{
    if (!st) return KILN_ERR_INVALID_ARG;
    if (st->phase != KILN_SP_PHASE_AWAIT_ACK) return KILN_ERR_STATE;
    advance_segment(st);
    return KILN_OK;
}

kiln_err_t kiln_setpoint_replace_remaining(kiln_setpoint_t *st,
                                           const kiln_program_t *updated)
{
    if (!st || !updated) return KILN_ERR_INVALID_ARG;
    if (st->finished) return KILN_ERR_STATE;

    /* The running segment and everything before it are history: FR-PRG-10
     * permits editing only what has not started. */
    if (updated->segment_count <= st->seg) return KILN_ERR_STATE;
    for (uint8_t i = 0; i <= st->seg; i++) {
        if (memcmp(&updated->segments[i], &st->prog.segments[i],
                   sizeof(kiln_segment_t)) != 0) {
            return KILN_ERR_STATE;
        }
    }

    const kiln_prog_validation_t v = kiln_profile_validate(updated, st->cfg.max_temp_c);
    if (v.code != KILN_PROG_OK) return KILN_ERR_RANGE;

    /* Keep name/description and the segments; execution state is untouched. */
    st->prog = *updated;
    return KILN_OK;
}

bool kiln_setpoint_heat_allowed(const kiln_setpoint_t *st)
{
    if (!st || !st->started || st->finished) return false;
    if (st->phase != KILN_SP_PHASE_RAMP) return true;   /* a soak needs heat */

    /* FR-CTL-13: a ramp whose target is below where the segment started is a
     * cooling ramp, and is executed passively. */
    const float target = (float)st->prog.segments[st->seg].target_c;
    return !(target < st->seg_start_c);
}

/* --- time prediction ---------------------------------------------------- */

/* Run a copy forward with the kiln tracking perfectly, which is the assumption
 * FR-PRG-06 and FR-RUN-05 state.  Hold-back and dwell tolerance are therefore
 * never triggered, and the simulation terminates. */
static uint32_t predict_s(const kiln_setpoint_t *st, bool stop_at_segment_end)
{
    if (!st || !st->started || st->finished) return 0;

    kiln_setpoint_t sim = *st;
    sim.holdback_active = false;
    sim.cfg.holdback_band_c = 0.0f;   /* perfect tracking cannot lag */

    const uint8_t start_seg = sim.seg;
    uint32_t steps = 0;

    while (!sim.finished && steps < PREDICT_MAX_STEPS) {
        if (sim.phase == KILN_SP_PHASE_AWAIT_ACK) break;  /* waits on a human */
        kiln_setpoint_tick(&sim, sim.sp_c, PREDICT_DT_S);
        steps++;
        if (stop_at_segment_end && sim.seg != start_seg) break;
    }

    return steps;
}

uint32_t kiln_setpoint_remaining_s(const kiln_setpoint_t *st)
{
    return predict_s(st, false);
}

uint32_t kiln_setpoint_segment_remaining_s(const kiln_setpoint_t *st)
{
    return predict_s(st, true);
}

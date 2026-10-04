/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <math.h>
#include <string.h>
#include "kiln_core/setpoint.h"
#include "kiln_core/profile.h"

/* Upper bound on a predicted duration: a program is capped at 168 h by
 * FR-PRG-05, so 200 h cannot be exceeded by a valid program and bounds the
 * answer for an invalid one. */
#define PREDICT_MAX_S   (200u * 3600u)

/* The clamped target of a segment.  One helper, used by the executor, by
 * heat_allowed and by the duration prediction, because the three disagreeing is
 * a real defect: comparing an *unclamped* target against the segment start made
 * heat_allowed report true while the executor was ramping the clamped setpoint
 * downward -- a cooling ramp that FR-CTL-13 should have made passive, driven at
 * full duty instead. */
static float seg_target_c(const kiln_setpoint_t *st, uint8_t seg)
{
    return kiln_clampf((float)st->prog.segments[seg].target_c, 0.0f, st->cfg.max_temp_c);
}

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

kiln_err_t kiln_setpoint_tick(kiln_setpoint_t *st, float pv_c, float dt_s)
{
    if (!st) return KILN_ERR_INVALID_ARG;
    if (!kiln_is_finite(dt_s) || dt_s <= 0.0f) return KILN_ERR_INVALID_ARG;
    if (!kiln_is_finite(pv_c)) return KILN_ERR_INVALID_ARG;
    if (!st->started || st->finished) return KILN_ERR_STATE;
    if (st->phase == KILN_SP_PHASE_AWAIT_ACK) return KILN_ERR_STATE;  /* frozen */

    /* FR-CTL-11: while the kiln is outside the hold-back band, neither the
     * setpoint nor the segment timer advances, so a slow kiln cannot silently
     * fall behind its curve. */
    if (st->cfg.holdback_band_c > 0.0f) {
        const float lag = st->sp_c > pv_c ? st->sp_c - pv_c : pv_c - st->sp_c;
        if (lag > st->cfg.holdback_band_c) {
            st->holdback_active = true;
            return KILN_OK;
        }
    }
    st->holdback_active = false;

    st->total_elapsed_s += dt_s;

    const kiln_segment_t *s = &st->prog.segments[st->seg];
    const float target = seg_target_c(st, st->seg);

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
    return KILN_OK;
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

    /* The whole program is taken, name and description included -- FR-PRG-10
     * permits editing anything that has not started, and the identity of a
     * program is as editable as its tail.  Execution state is untouched. */
    st->prog = *updated;
    return KILN_OK;
}

bool kiln_setpoint_heat_allowed(const kiln_setpoint_t *st)
{
    if (!st || !st->started || st->finished) return false;
    if (st->phase != KILN_SP_PHASE_RAMP) return true;   /* a soak needs heat */

    /* FR-CTL-13: a ramp whose target is below where the segment started is a
     * cooling ramp, and is executed passively.  The *clamped* target, which is
     * what the executor is actually ramping toward. */
    return !(seg_target_c(st, st->seg) < st->seg_start_c);
}

/* --- time prediction ---------------------------------------------------- */

/* Seconds a ramp occupies, matching the executor tick for tick.  The executor
 * steps the setpoint by rate/3600 each second and transitions on the step that
 * reaches the target, so the count is a ceiling -- and is at least one second,
 * because even a zero-span ramp costs the cycle that notices it has arrived. */
static uint32_t ramp_s(float from_c, float to_c, uint16_t rate_c_per_h)
{
    if (rate_c_per_h == 0) return 1u;        /* FR-CTL-10: steps to target */

    const float step_per_s = (float)rate_c_per_h / 3600.0f;
    if (!(step_per_s > 0.0f)) return 1u;

    const double k = ceil((double)fabsf(to_c - from_c) / (double)step_per_s);
    if (!(k >= 1.0)) return 1u;
    if (k > (double)PREDICT_MAX_S) return PREDICT_MAX_S;
    return (uint32_t)k;
}

/* Seconds of dwell still to accrue.  Also at least one second: the executor adds
 * dt before testing, so a zero dwell still consumes the cycle that ends it. */
static uint32_t dwell_s(uint16_t dwell_min, float already_s)
{
    const double rem = (double)dwell_min * 60.0 - (double)already_s;
    if (!(rem > 0.0)) return 1u;

    const double k = ceil(rem);
    if (k > (double)PREDICT_MAX_S) return PREDICT_MAX_S;
    return k < 1.0 ? 1u : (uint32_t)k;
}

/* Closed-form equivalent of running the generator forward with the kiln tracking
 * perfectly, which is the assumption FR-PRG-06 and FR-RUN-05 state.  Hold-back
 * and dwell tolerance therefore never trigger, which is what makes the
 * arithmetic closed. */
static uint32_t predict_s(const kiln_setpoint_t *st, bool stop_at_segment_end)
{
    if (!st || !st->started || st->finished) return 0;
    /* A segment waiting on an operator has no predictable duration at all. */
    if (st->phase == KILN_SP_PHASE_AWAIT_ACK || st->phase == KILN_SP_PHASE_DONE) return 0;

    uint64_t        total   = 0;
    float           sp      = st->sp_c;
    float           elapsed = st->seg_elapsed_s;
    kiln_sp_phase_t phase   = st->phase;

    for (uint8_t seg = st->seg; seg < st->prog.segment_count; seg++) {
        const kiln_segment_t *s      = &st->prog.segments[seg];
        const float           target = seg_target_c(st, seg);

        if (phase == KILN_SP_PHASE_RAMP) {
            total  += ramp_s(sp, target, s->rate_c_per_h);
            sp      = target;
            elapsed = 0.0f;
        }
        total += dwell_s(s->dwell_min, elapsed);

        /* The next segment ramps from this one's target, not from the
         * measurement: a lagging kiln must not shorten the next ramp. */
        phase   = KILN_SP_PHASE_RAMP;
        elapsed = 0.0f;

        /* FR-PRG-03: the schedule stops at a segment that waits on a human. */
        if (s->flags & KILN_SEG_FLAG_REQUIRE_ACK) break;
        if (stop_at_segment_end) break;
        if (total >= (uint64_t)PREDICT_MAX_S) break;
    }

    return total > (uint64_t)PREDICT_MAX_S ? PREDICT_MAX_S : (uint32_t)total;
}

uint32_t kiln_setpoint_remaining_s(const kiln_setpoint_t *st)
{
    return predict_s(st, false);
}

uint32_t kiln_setpoint_segment_remaining_s(const kiln_setpoint_t *st)
{
    return predict_s(st, true);
}

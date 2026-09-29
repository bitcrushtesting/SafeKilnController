/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Setpoint generator -- architecture section 7.1.
 *
 * AD-06: this is deliberately separate from the PID.  The program produces a
 * moving setpoint; the PID only ever tracks the setpoint it is handed.  That
 * split is what makes curve execution and loop tuning independently testable.
 *
 * Implements FR-CTL-09 (interpolated ramp, not a step), FR-CTL-10 (rate 0 =
 * as fast as the kiln allows), FR-CTL-11 (hold-back), FR-CTL-12 (dwell
 * tolerance), FR-CTL-13 (cooling segments are passive) and FR-PRG-03 (per
 * segment operator acknowledgement).
 */
#ifndef KILN_CORE_SETPOINT_H
#define KILN_CORE_SETPOINT_H

#include "kiln/err.h"
#include "kiln/types.h"

typedef enum {
    KILN_SP_PHASE_RAMP = 0,
    KILN_SP_PHASE_DWELL,
    KILN_SP_PHASE_AWAIT_ACK,
    KILN_SP_PHASE_DONE,
} kiln_sp_phase_t;

typedef struct {
    float holdback_band_c;   /* FR-CTL-11: 0 .. 200, 0 disables */
    float dwell_tol_c;       /* FR-CTL-12: 0.5 .. 50            */
    float max_temp_c;        /* configured kiln maximum         */
} kiln_setpoint_cfg_t;

typedef struct {
    kiln_setpoint_cfg_t cfg;

    /* A private copy of the program.  Two reasons: FR-PRG-11 wants the program
     * as executed preserved against later edits, and FR-PRG-10 edits must not
     * be able to mutate the segment currently running. */
    kiln_program_t prog;

    uint8_t         seg;
    kiln_sp_phase_t phase;

    float sp_c;
    float seg_start_c;        /* temperature at which this segment began */
    float seg_elapsed_s;      /* in DWELL: time accumulated within tolerance */
    float total_elapsed_s;

    bool holdback_active;
    bool finished;
    bool started;
} kiln_setpoint_t;

/* Begin executing prog from the present temperature pv_c. */
kiln_err_t kiln_setpoint_start(kiln_setpoint_t *st,
                               const kiln_setpoint_cfg_t *cfg,
                               const kiln_program_t *prog,
                               float pv_c);

/* One control cycle. */
void kiln_setpoint_tick(kiln_setpoint_t *st, float pv_c, float dt_s);

/* FR-PRG-03: release a segment that is waiting for the operator. */
kiln_err_t kiln_setpoint_ack(kiln_setpoint_t *st);

/* FR-PRG-10: replace the not-yet-started segments.  Rejects any change to the
 * current or completed segments, and any program that fails validation. */
kiln_err_t kiln_setpoint_replace_remaining(kiln_setpoint_t *st,
                                           const kiln_program_t *updated);

/* FR-PRG-06 / FR-RUN-05: seconds still to run, assuming the kiln keeps up.
 * Computed by running this same generator forward over a copy, so there is no
 * second estimator that can drift out of agreement with the executor. */
uint32_t kiln_setpoint_remaining_s(const kiln_setpoint_t *st);

/* Seconds remaining in the current segment, same assumption. */
uint32_t kiln_setpoint_segment_remaining_s(const kiln_setpoint_t *st);

/* FR-CTL-13: false while a cooling ramp is in progress, so the caller holds
 * duty at zero rather than relying on the sign of the PID error. */
bool kiln_setpoint_heat_allowed(const kiln_setpoint_t *st);

static inline float kiln_setpoint_value(const kiln_setpoint_t *st) { return st->sp_c; }
static inline bool  kiln_setpoint_finished(const kiln_setpoint_t *st) { return st->finished; }
static inline bool  kiln_setpoint_holdback(const kiln_setpoint_t *st) { return st->holdback_active; }
static inline uint8_t kiln_setpoint_segment(const kiln_setpoint_t *st) { return st->seg; }
static inline bool kiln_setpoint_awaiting_ack(const kiln_setpoint_t *st)
{
    return st->phase == KILN_SP_PHASE_AWAIT_ACK;
}

#endif

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

/* One control cycle.
 *
 * NFR-17: returns KILN_ERR_INVALID_ARG for a NULL generator, a non-positive or
 * non-finite dt_s or a non-finite pv_c, and KILN_ERR_STATE when there is nothing
 * to advance (not started, finished, or frozen awaiting an acknowledgement).
 * Previously all of these returned silently, so a stalled clock looked exactly
 * like a program running correctly. */
kiln_err_t kiln_setpoint_tick(kiln_setpoint_t *st, float pv_c, float dt_s);

/* FR-PRG-03: release a segment that is waiting for the operator. */
kiln_err_t kiln_setpoint_ack(kiln_setpoint_t *st);

/* FR-PRG-10: replace the not-yet-started segments.  Rejects any change to the
 * current or completed segments, and any program that fails validation. */
kiln_err_t kiln_setpoint_replace_remaining(kiln_setpoint_t *st,
                                           const kiln_program_t *updated);

/* FR-PRG-06 / FR-RUN-05: seconds still to run, assuming the kiln keeps up.
 *
 * Closed form.  These are display and API calls, served from the HMI and web
 * tasks, and the arithmetic is a sum of ramp spans over rates plus dwell times --
 * so simulating it a second at a time, up to 720 000 iterations over a ~440 byte
 * stack copy of the whole generator, bought agreement with the executor at the
 * price of a multi-millisecond blocking loop against NFR-02's 50 ms ceiling.
 *
 * Agreement is instead maintained by test: the host suite runs the generator
 * forward as an oracle and asserts the closed form matches it, which is the same
 * guarantee without the cost.  Expect agreement to within one second per
 * segment; the ramp count is a ceiling over a float accumulation.
 *
 * The estimate is optimistic through a cooling segment, because FR-CTL-13 makes
 * cooling passive -- a real kiln cools as fast as it cools, not at the rate the
 * program names.  Stated here and in the API documentation (tasklist D4). */
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

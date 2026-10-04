/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Application orchestration -- architecture section 5.3 and section 6.
 *
 * This is the layer the core deliberately does not have: it reads ports, holds
 * the mode state machine of requirements section 2.2, and applies the safety
 * verdict.  Everything it *decides* it decides by calling kiln_core; what is here
 * is sequencing and authority.
 *
 * Four entry points, one per period of the task table in architecture section
 * 6.1.  They are separate functions rather than one tick because their periods
 * and their priorities differ, and because AD-04 requires that heat authority be
 * written from exactly one of them:
 *
 *   kiln_app_window_tick     10 ms   SSR pin and the gated current sampler (AD-07, AD-17)
 *   kiln_app_safety_cycle   100 ms   the only writer of heat authority (AD-04)
 *   kiln_app_acquire_cycle  250 ms   >= 4 Hz per FR-ACQ-03
 *   kiln_app_control_cycle     1 s   setpoint, PID, duty request
 *
 * On the target each runs in its own task; on the host the integration suite
 * calls them from one loop against a virtual clock, which is what makes a 168 h
 * firing testable in milliseconds (AD-02).
 *
 * Deliberately one component rather than the six of architecture section 5.3:
 * run_controller, control_task, safety_task, telemetry and the event bus are all
 * here, because splitting them before kiln_web and kiln_hmi exist would be
 * inventing an interface with no second caller.  The seam that matters -- core
 * decides, app sequences, ports touch hardware -- is the one that is held.
 */
#ifndef KILN_APP_H
#define KILN_APP_H

#include "kiln/err.h"
#include "kiln/types.h"
#include "kiln_core/autotune.h"
#include "kiln_core/configmodel.h"
#include "kiln_core/current.h"
#include "kiln_core/pid.h"
#include "kiln_core/profile.h"
#include "kiln_core/runstate.h"
#include "kiln_core/safety.h"
#include "kiln_core/setpoint.h"
#include "kiln_core/tempfilt.h"
#include "kiln_core/window.h"
#include "kiln_ports/port_alarm.h"
#include "kiln_ports/port_counters.h"
#include "kiln_ports/port_current.h"
#include "kiln_ports/port_heat.h"
#include "kiln_ports/port_tc.h"

typedef struct {
    const kiln_port_tc_t       *tc;         /* required */
    const kiln_port_tc_t       *case_tc;    /* optional */
    const kiln_port_heat_t     *heat;       /* required */
    const kiln_port_current_t  *current;    /* optional: FR-CUR-12 */
    const kiln_port_counters_t *counters;   /* optional */
    const kiln_port_alarm_t    *alarm;      /* optional */
} kiln_app_ports_t;

typedef struct {
    kiln_config_t    cfg;
    kiln_app_ports_t ports;

    /* core components */
    kiln_tempfilt_t filt;
    kiln_tempfilt_t case_filt;
    kiln_pid_t      pid;
    kiln_setpoint_t sp;
    kiln_window_t   win;
    kiln_safety_t   safety;
    kiln_current_t  cur;
    kiln_autotune_t tune;

    /* mode state machine, requirements section 2.2 */
    kiln_state_t state;
    kiln_fault_t fault;              /* latched; SR-17                     */
    uint32_t     warnings;

    /* readings */
    float    kiln_c, kiln_raw_c, case_c, cj_c, rate_c_per_h;
    uint16_t tc_fault_bits, case_fault_bits;
    bool     kiln_valid, case_valid;

    /* the duty the control path requests, and the authority safety grants */
    uint16_t duty_request;
    uint16_t duty_manual;
    bool     heat_authorised;
    bool     heat_allowed;           /* FR-CTL-13 */

    /* current sampling (AD-17) */
    bool     cur_fresh;              /* a measurement landed since the last
                                      * safety evaluation                   */
    bool     cur_burst_pending;
    float    cur_deviation;
    bool     cur_deviation_valid;

    kiln_switch_counters_t counters;
    uint32_t ssr_switch_seen[KILN_HEAT_CHANNELS];

    /* run bookkeeping */
    kiln_run_record_t record;
    uint32_t next_run_id;
    double   run_elapsed_s;
    float    alarm_timer_s;

    /* FR-RUN-06 */
    bool     complete_pending;

    /* NFR-17: safety cycles called with an argument that violated the contract.
     * Non-zero is a defect in the task that drives this one. */
    uint32_t safety_bad_calls;
} kiln_app_t;

/* cfg may be NULL, in which case the defaults are used. */
kiln_err_t kiln_app_init(kiln_app_t *app, const kiln_app_ports_t *ports,
                         const kiln_config_t *cfg);

/* FR-CFG-03/04/08: validate, refuse what may not change now, apply the rest. */
kiln_err_t kiln_app_apply_config(kiln_app_t *app, const kiln_config_t *cfg,
                                 const kiln_cfg_item_t **bad);

void kiln_app_window_tick(kiln_app_t *app, uint32_t dt_ms);
void kiln_app_acquire_cycle(kiln_app_t *app, float dt_s);
void kiln_app_control_cycle(kiln_app_t *app, float dt_s);
void kiln_app_safety_cycle(kiln_app_t *app, float dt_s);

/* --- commands (requirements section 2.2, FR-RUN) ------------------------ */

kiln_err_t kiln_app_start(kiln_app_t *app, const kiln_program_t *prog);
kiln_err_t kiln_app_pause(kiln_app_t *app);
kiln_err_t kiln_app_resume(kiln_app_t *app);
kiln_err_t kiln_app_abort(kiln_app_t *app);
kiln_err_t kiln_app_ack_segment(kiln_app_t *app);
kiln_err_t kiln_app_clear_fault(kiln_app_t *app);
kiln_err_t kiln_app_manual(kiln_app_t *app, uint16_t duty_permille);
kiln_err_t kiln_app_autotune(kiln_app_t *app, float setpoint_c);
kiln_err_t kiln_app_idle(kiln_app_t *app);

void kiln_app_snapshot(const kiln_app_t *app, kiln_snapshot_t *out);

static inline const kiln_run_record_t *kiln_app_record(const kiln_app_t *app)
{
    return &app->record;
}

#endif

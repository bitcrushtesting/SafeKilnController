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
 * and their priorities differ, and because SWA-04 requires that heat authority be
 * written from exactly one of them:
 *
 *   kiln_app_window_tick     10 ms   SSR pin and the gated current sampler (SWA-07, SWA-17)
 *   kiln_app_safety_cycle   100 ms   the only writer of heat authority (SWA-04)
 *   kiln_app_acquire_cycle  250 ms   >= 4 Hz per SWR-ACQ-03
 *   kiln_app_control_cycle     1 s   setpoint, PID, duty request
 *
 * On the target each runs in its own task; on the host the integration suite
 * calls them from one loop against a virtual clock, which is what makes a 168 h
 * firing testable in milliseconds (SWA-02).
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
#include "kiln_ports/port_door.h"
#include "kiln_ports/port_supervisor.h"
#include "kiln_ports/port_counters.h"
#include "kiln_ports/port_current.h"
#include "kiln_ports/port_clock.h"
#include "kiln_ports/port_filestore.h"
#include "kiln_ports/port_heat.h"
#include "kiln_ports/port_kvstore.h"
#include "kiln_ports/port_logstore.h"
#include "kiln_ports/port_system.h"
#include "kiln_ports/port_tc.h"
#include "kiln_app/settings.h"

typedef struct {
    const kiln_port_tc_t       *tc;         /* required */
    const kiln_port_tc_t       *case_tc;    /* optional */
    const kiln_port_heat_t     *heat;       /* required */
    const kiln_port_current_t  *current;    /* optional: SWR-CUR-12 */
    const kiln_port_counters_t *counters;   /* optional */
    const kiln_port_alarm_t    *alarm;      /* optional */
    const kiln_port_door_t     *door;       /* optional: SWR-SAF-31, warning 113 */
    /* SWA-22.  Optional, and reporting only: the supervisor's authority is a
     * series element in the coil, not anything this firmware consults.  What
     * this port is for is being able to tell the operator *which* condition
     * tripped, and that the button on the panel is what clears it. */
    const kiln_port_supervisor_t *supervisor;

    /* Persistence (M5).  All optional: SWR-LOG-14 requires the firing to
     * continue with a warning when the log store is unavailable, and the same
     * reasoning covers a board whose storage has not been wired up yet. */
    const kiln_port_logstore_t  *logstore;
    const kiln_port_kvstore_t   *kvstore;
    const kiln_port_filestore_t *filestore;
    const kiln_port_clock_t     *clock;
} kiln_app_ports_t;

/* Architecture 13.4 budgets 64 records for the log queue.  The control task
 * enqueues and only the logger touches flash (SWR-LOG-14), so a full queue drops
 * the sample and counts it rather than stalling control. */
constexpr size_t KILN_APP_LOG_QUEUE = 64;

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
    kiln_fault_t fault;              /* latched; SWR-SAF-17                     */
    uint32_t     warnings;

    /* readings */
    float    kiln_c, kiln_raw_c, case_c, cj_c, rate_c_per_h;
    uint16_t tc_fault_bits, case_fault_bits;
    bool     kiln_valid, case_valid;

    /* the duty the control path requests, and the authority safety grants */
    uint16_t duty_request;
    uint16_t duty_manual;
    bool     heat_authorised;
    bool     heat_allowed;           /* SWR-CTL-13 */

    /* current sampling (SWA-17) */
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

    /* SWR-SAF-31, as the safety cycle last read it.  Published in the snapshot
     * rather than re-read by the display, so the panel and the API cannot
     * disagree with the rule that acted. */
    bool     door_open;
    bool     door_monitoring;

    /* SWR-RUN-06 */
    bool     complete_pending;

    /* --- logging (FR-LOG) --------------------------------------------- */
    uint8_t  log_q[KILN_APP_LOG_QUEUE][KILN_LOG_RECORD_BYTES];
    uint16_t log_head, log_tail;
    uint32_t log_dropped;          /* SWR-LOG-14 / warning 103 */
    uint32_t log_written;
    uint32_t log_errors;
    double   log_accum_s;
    uint8_t  last_logged_state;
    uint32_t last_logged_warnings;
    bool     log_run_open;

    /* --- boot (SWR-RUN-08, SWR-SAF-17) -------------------------------------- */
    kiln_recovery_decision_t recovery;
    kiln_latched_fault_t     latched;
    bool                     latched_valid;
    bool                     config_storage_failed;   /* fault 20 */

    /* SWR-SAF-12: the baseline is the run history, loaded at boot. */
    kiln_insulation_baseline_t baseline;
    bool                       baseline_valid;

    /* SWR-NFR-17: safety cycles called with an argument that violated the contract.
     * Non-zero is a defect in the task that drives this one. */
    uint32_t safety_bad_calls;
} kiln_app_t;

/* cfg may be NULL, in which case the defaults are used. */
kiln_err_t kiln_app_init(kiln_app_t *app, const kiln_app_ports_t *ports,
                         const kiln_config_t *cfg);

/* SWR-CFG-03/04/08: validate, refuse what may not change now, apply the rest. */
kiln_err_t kiln_app_apply_config(kiln_app_t *app, const kiln_config_t *cfg,
                                 const kiln_cfg_item_t **bad);

/* Everything that has to happen once, before the cycles start running:
 * configuration, seeded programs, the run-id sequence, the latched fault of
 * SWR-SAF-17, and the power-loss decision of SWR-RUN-08.
 *
 * `outage_s` is how long power was off if the wall clock can say; pass a
 * negative value when it cannot, because an unknown outage is not a short one.
 *
 * Performs one acquisition cycle of its own before deciding, because SWR-RUN-08's
 * band test needs a temperature that has actually been measured.
 *
 * Returns KILN_OK, or KILN_ERR_IO when configuration storage failed -- in which
 * case defaults are in use and SWR-CFG-05 wants fault 20 reported. */
kiln_err_t kiln_app_boot(kiln_app_t *app, kiln_reset_cause_t cause, float outage_s);

static inline const kiln_recovery_decision_t *kiln_app_recovery(const kiln_app_t *app)
{
    return &app->recovery;
}

/* SWR-LOG-04: an out-of-band record, written at once rather than at the next
 * sample boundary. */
void kiln_app_log_event(kiln_app_t *app, kiln_log_event_t event);

/* Drain up to `max_records` to the log store.  The only call here that touches
 * flash, and the only one that may block (SWR-LOG-14). */
uint32_t kiln_app_log_drain(kiln_app_t *app, uint32_t max_records);

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

/* --- SWR-CFG-09: the owner erases everything ---------------------------
 *
 * What the owner's data is, and what it is not. Everything this removes was
 * put there by whoever owns the kiln: the configuration, the WiFi
 * credentials, the run history, the sample log, the wear counters, the
 * latched fault. The production data block of SWR-PROD-01 and the firmware
 * stay: those identify the unit for its whole life and belong to the unit
 * rather than to its owner.
 *
 * **Erasure means gone, not unreferenced.** A key holding a credential is
 * overwritten with zeros and committed BEFORE it is erased, because on NVS an
 * erase marks an entry dead and leaves its bytes in the page until the next
 * garbage collection -- and "the API says it is not configured" is not the
 * question somebody selling a kiln is asking. The test for this reads the raw
 * store back and looks for the passphrase.
 *
 * Refused while a run or an autotune is going, for the obvious reason, and it
 * reports what it erased rather than only that it finished: an owner who is
 * about to sell the thing deserves to see counts. */
typedef struct {
    bool     config;        /* the configuration blob                      */
    bool     credentials;   /* the WiFi passphrase, overwritten then erased */
    bool     latched_fault;
    bool     log;           /* the sample log partition                    */
    bool     counters;      /* switching-operation wear counters           */
    uint8_t  runs_erased;   /* run records removed from the file store     */
    uint8_t  failures;      /* how many of the above could not be done     */
} kiln_factory_reset_t;

/* KILN_ERR_STATE while running or autotuning. Otherwise it does as much as it
 * can and reports the rest: a reset that stopped at the first failure would
 * leave an owner with some of their data erased and no idea which. */
kiln_err_t kiln_app_factory_reset(kiln_app_t *app, kiln_factory_reset_t *out);

void kiln_app_snapshot(const kiln_app_t *app, kiln_snapshot_t *out);

/* SWR-CUR-07: apparent power and cumulative energy from the measured current
 * and the configured mains voltage.  A resistive load is assumed (SYS-ASM-09),
 * and every presentation of these figures states it. */
double  kiln_app_apparent_va(const kiln_app_t *app);
double  kiln_app_energy_wh(const kiln_app_t *app);

static inline const kiln_run_record_t *kiln_app_record(const kiln_app_t *app)
{
    return &app->record;
}

#endif

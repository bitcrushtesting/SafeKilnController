/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Run records and power-loss recovery -- SWR-RUN-07, SWR-RUN-08, SWR-RUN-09,
 * SWR-SAF-12's baseline, architecture section 5.1 and SWA-09.
 *
 * SWA-09: the sample log is also the power-loss journal.  Recovery state is
 * reconstructed from the log tail rather than from a separate periodic write,
 * which removes a 60 000-write-per-run NVS hot spot and costs nothing, because
 * the data was already being written.  Recovery granularity therefore equals the
 * log sample interval (default 10 s), well inside SWR-RUN-08's tolerance.
 *
 * The decision itself is here, as a pure function of the tail plus the policy, so
 * that "would this kiln resume?" is a host test and not a bench experiment with a
 * mains plug (SWR-TST-23's power-loss-at-a-random-instant suite).
 */
#ifndef KILN_CORE_RUNSTATE_H
#define KILN_CORE_RUNSTATE_H

#include "kiln/err.h"
#include "kiln/types.h"
#include "kiln_core/autotune.h"      /* kiln_gains_t          */
#include "kiln_core/configmodel.h"   /* kiln_recovery_policy_t */
#include "kiln_core/logrec.h"        /* kiln_log_sample_t     */
#include "kiln_core/safety.h"        /* KILN_INSUL_BANDS, baseline */
#include "kiln_ports/port_system.h"  /* kiln_reset_cause_t    */

/* Why a run ended.  Recorded in the run record, and shown to the operator. */
typedef enum {
    KILN_END_UNKNOWN = 0,
    KILN_END_COMPLETE,          /* SWR-RUN-06            */
    KILN_END_OPERATOR_ABORT,    /* SWR-RUN-04            */
    KILN_END_FAULT,             /* the fault is carried */
    KILN_END_POWER_LOSS,        /* never closed out     */
    KILN_END_COUNT,
} kiln_run_end_t;

/* SWR-LOG-09: the run records of the 20 most recent runs are kept even once
 * their samples have been overwritten by the ring, and such a run is *marked* --
 * otherwise a chart with no data in it is indistinguishable from a run that
 * never logged, and the operator is left wondering which. */
constexpr uint32_t KILN_RUN_FLAG_TRUNCATED = 1u << 0u;
/* The run was still open when the controller lost power or reset. */
constexpr uint32_t KILN_RUN_FLAG_INTERRUPTED = 1u << 1u;

/* SWR-RUN-07 */
typedef struct {
    uint32_t       run_id;
    uint64_t       start_wall_utc_s;    /* 0 when the clock was unsynced   */
    uint64_t       end_wall_utc_s;
    uint32_t       duration_s;          /* monotonic, always meaningful    */
    kiln_program_t program;             /* as executed (SWR-PRG-11)         */
    kiln_gains_t   gains;
    char           gain_set_name[KILN_CFG_GAINSET_LEN];
    uint8_t        end_reason;          /* kiln_run_end_t                  */
    uint8_t        fault;               /* kiln_fault_t, when end_reason is FAULT */
    uint8_t        flags;               /* KILN_RUN_FLAG_*                 */
    uint32_t       log_first_ms, log_last_ms;   /* log extent              */
    float          peak_c;
    float          current_ref_a;       /* SWR-CUR-08, required by SWR-RUN-07 */
    float          energy_wh;           /* SWR-CUR-07                       */
    uint32_t       band_duty_s[KILN_INSUL_BANDS];   /* SWR-SAF-12              */
    uint32_t       contactor_ops;       /* SWR-CUR-13, at run end           */
    uint32_t       ssr_ops[KILN_HEAT_CHANNELS];
} kiln_run_record_t;

void kiln_runstate_record_init(kiln_run_record_t *r, uint32_t run_id);

/* --- power-loss recovery (SWR-RUN-08) ------------------------------------ */

typedef struct {
    uint8_t  policy;                /* kiln_recovery_policy_t              */
    uint16_t max_outage_min;        /* 1 .. 120, default 15                */
    float    band_c;                /* 5 .. 200, default 50                */
} kiln_recovery_cfg_t;

void kiln_runstate_recovery_defaults(kiln_recovery_cfg_t *cfg);

typedef enum {
    KILN_RECOVER_NO_RUN = 0,        /* nothing was running: ordinary boot  */
    KILN_RECOVER_RESUME,            /* conditions met                      */
    KILN_RECOVER_ABORT,             /* policy says abort; not a fault      */
    KILN_RECOVER_REFUSED,           /* wanted to resume but may not: fault 19 */
} kiln_recover_action_t;

typedef struct {
    kiln_recover_action_t action;
    kiln_fault_t          fault;        /* RECOVERY_REFUSED, or WATCHDOG    */
    uint8_t               segment;      /* where to pick up                 */
    float                 setpoint_c;   /* the setpoint at interruption     */
    float                 kiln_c;       /* the temperature at interruption  */
    uint32_t              t_rel_ms;     /* log position resumed from        */
    const char           *reason;       /* operator-facing, one line        */
} kiln_recovery_decision_t;

/* Decide what to do about a run the log says was in progress.
 *
 *   tail        the newest decodable sample of that run, NULL if there is none
 *   now_kiln_c  the present temperature, measured this boot
 *   outage_s    how long power was off, if the wall clock can say; pass a
 *               negative value when it cannot, which is treated as "longer than
 *               the limit" -- an unknown outage is not a short one
 *   cause       SWR-NFR-15 / SWR-SAF-14: an abnormal reset is never resumed, whatever the
 *               policy says, because the firmware's own state is in question
 */
kiln_recovery_decision_t kiln_runstate_decide(const kiln_recovery_cfg_t *cfg,
                                              const kiln_log_sample_t *tail,
                                              float now_kiln_c,
                                              float outage_s,
                                              kiln_reset_cause_t cause);

/* --- SWR-SAF-12 baseline ----------------------------------------------------- */

/* The median duty-seconds needed to pass each 100 degC boundary across previous
 * runs.  Median rather than mean: one aborted run that never reached 900 degC, or
 * one firing with a full load against one with a half load, should not move the
 * baseline that a degradation warning is measured against.
 *
 * A band is marked valid only when at least min_runs of the supplied records
 * actually crossed it. */
kiln_err_t kiln_runstate_baseline(const kiln_run_record_t *records, uint8_t count,
                                  uint8_t min_runs,
                                  kiln_insulation_baseline_t *out);

const char *kiln_run_end_str(kiln_run_end_t reason);

#endif

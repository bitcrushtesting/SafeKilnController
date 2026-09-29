/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Safety supervisor -- architecture section 8, requirements section 5.
 *
 * Every rule of SR-04..SR-13 lives here as a pure function of an input
 * snapshot plus its own timer state, which is what lets each one be driven
 * directly by a host test (TR-09, TR-23).  This component decides; it does not
 * act.  The caller (the safety task, which is the only holder of heat
 * authority under AD-04) applies the verdict.
 */
#ifndef KILN_CORE_SAFETY_H
#define KILN_CORE_SAFETY_H

#include "kiln/types.h"

/* SR-12: duty-seconds needed to reach each 100 degC boundary, established from
 * previous comparable runs.  A rise above factor x baseline suggests failing
 * insulation or ageing elements. */
#define KILN_INSUL_BANDS 13          /* 100, 200, ... 1300 degC */

typedef struct {
    uint32_t duty_s[KILN_INSUL_BANDS];
    bool     valid[KILN_INSUL_BANDS];
} kiln_insulation_baseline_t;

typedef struct {
    /* SR-09 */
    float max_temp_c;
    float overtemp_margin_c;
    /* SR-11 */
    float max_case_temp_c;
    /* SR-04 / FR-ACQ-12 */
    float tc_grace_s;
    /* SR-05 reversed thermocouple */
    uint16_t reversed_duty_permille;
    float    reversed_drop_c;
    float    reversed_window_s;
    /* SR-06 stuck sensor */
    float    stuck_delta_c;
    uint16_t stuck_duty_permille;
    float    stuck_window_s;
    /* SR-07 thermal runaway */
    uint16_t runaway_duty_permille;
    float    runaway_window_s;
    float    runaway_min_rate_c_per_h;
    /* SR-08 uncommanded heating */
    float uncommanded_rise_c;
    float uncommanded_window_s;
    /* Settle time before SR-08 arms.  After a spell at high duty the measured
     * temperature keeps climbing for a while as heat soaks inward from the
     * elements; arming immediately would read that as a shorted SSR. */
    float uncommanded_settle_s;
    /* SR-10 setpoint excursion */
    float excursion_band_c;
    float excursion_window_s;
    /* SR-12 */
    float insulation_factor;
    /* FR-CTL-15 / warning 107 */
    float saturated_warn_s;
} kiln_safety_cfg_t;

void kiln_safety_cfg_defaults(kiln_safety_cfg_t *cfg);

typedef struct {
    float    kiln_c;
    float    case_c;
    float    rate_c_per_h;
    float    setpoint_c;
    uint16_t duty_permille;            /* what the control path is requesting */
    uint16_t tc_fault_bits;            /* KILN_TC_FAULT_*                     */
    uint16_t case_fault_bits;
    bool     case_present;             /* enclosure channel fitted            */
    bool     heating_active;           /* Running, Manual or Autotune         */
    bool     control_deadline_missed;  /* SR-13                               */
    bool     safety_deadline_missed;   /* SR-13                               */
} kiln_safety_input_t;

typedef struct {
    bool         heat_permitted;
    kiln_fault_t fault;                /* KILN_FAULT_NONE when none tripped   */
    uint32_t     warnings;             /* KILN_WARN_BIT(...) mask             */
} kiln_safety_verdict_t;

/* One rule's accumulator.  Grouped per rule so a test can inspect exactly the
 * rule it is exercising. */
typedef struct {
    float timer_s;
    float ref_c;
    float min_c, max_c;
    bool  armed;
} kiln_rule_state_t;

typedef struct {
    kiln_safety_cfg_t cfg;

    kiln_rule_state_t tc_grace;        /* SR-04 kiln channel  */
    kiln_rule_state_t case_grace;      /* SR-04 case channel  */
    kiln_rule_state_t reversed;        /* SR-05 */
    kiln_rule_state_t stuck;           /* SR-06 */
    kiln_rule_state_t runaway;         /* SR-07 */
    kiln_rule_state_t uncommanded;     /* SR-08 */
    kiln_rule_state_t excursion;       /* SR-10 */
    kiln_rule_state_t saturated;       /* warning 107 */

    /* SR-12 */
    kiln_insulation_baseline_t baseline;
    double   run_duty_s;
    uint32_t band_duty_s[KILN_INSUL_BANDS];
    bool     band_crossed[KILN_INSUL_BANDS];

    uint32_t warnings;                 /* sticky within a run */
} kiln_safety_t;

void kiln_safety_init(kiln_safety_t *s, const kiln_safety_cfg_t *cfg);
void kiln_safety_reconfigure(kiln_safety_t *s, const kiln_safety_cfg_t *cfg);

/* Reset all per-run accumulators.  Called on entering a heating state. */
void kiln_safety_begin_run(kiln_safety_t *s, const kiln_insulation_baseline_t *baseline);

/* Evaluate every rule.  dt_s is the interval since the previous call. */
kiln_safety_verdict_t kiln_safety_eval(kiln_safety_t *s,
                                       const kiln_safety_input_t *in,
                                       float dt_s);

/* SR-18: may a latched fault be cleared?  False while its triggering condition
 * is still observably true.  The same thresholds are reused, so there is no
 * second implementation to disagree with the detector. */
bool kiln_safety_can_clear(const kiln_safety_cfg_t *cfg,
                           kiln_fault_t code,
                           const kiln_safety_input_t *in);

/* SR-12: duty-seconds this run needed to reach each band, for the run record. */
const uint32_t *kiln_safety_band_duty(const kiln_safety_t *s, uint8_t *count);

#endif

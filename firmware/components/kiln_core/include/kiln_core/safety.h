/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Safety supervisor -- architecture section 8, requirements section 5.
 *
 * Every rule of SR-04..SR-13 and of SR-25..SR-30 lives here as a pure function
 * of an input snapshot plus its own timer state, which is what lets each one be
 * driven directly by a host test (TR-09, TR-23).  This component decides; it does
 * not act.  The caller (the safety task, which is the only holder of heat
 * authority under AD-04) applies the verdict.
 *
 * Requirements section 5.2 makes the current rules the *primary* detection of
 * relay and element failure: SR-25 acts on amps in an off-window within one
 * second (NFR-27), where SR-08 waits for five degrees of temperature rise over
 * three minutes.  The thermal rules are retained as an independent backstop, for
 * the kiln whose monitoring has been disabled or whose transformer has failed
 * (FR-CUR-12), and for a fault on an unmonitored phase (ASM-10).
 */
#ifndef KILN_CORE_SAFETY_H
#define KILN_CORE_SAFETY_H

#include "kiln/err.h"
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
    /* SR-05 reversed thermocouple.  The drop must *persist* for
     * reversed_confirm_s, not merely occur: a kiln with transport lag whose gains
     * are imperfect overshoots and then falls several degrees while the
     * controller is already pushing duty back up, and an instantaneous test reads
     * that as a reversed probe.  A genuinely reversed couple falls monotonically
     * and does not come back, so the confirmation costs it nothing -- whereas a
     * rule that stops a healthy firing is worse than no rule, because it gets
     * switched off. */
    uint16_t reversed_duty_permille;
    float    reversed_drop_c;
    float    reversed_window_s;
    float    reversed_confirm_s;
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
     * elements; arming immediately would read that as a shorted SSR.  Note the
     * consequence, which architecture section 8.2 should state (tasklist D2):
     * during a normal firing duty is rarely zero for a full minute, so SR-08 is
     * effectively inactive while running.  That is precisely why SR-25 is the
     * primary detection and this one the backstop. */
    float uncommanded_settle_s;
    /* SR-10 setpoint excursion */
    float excursion_band_c;
    float excursion_window_s;
    /* SR-12 */
    float insulation_factor;
    /* FR-CTL-15 / warning 107 */
    float saturated_warn_s;

    /* --- heater current, SR-25..SR-30 ---------------------------------- */

    /* FR-CUR-11: a transformer fault is given a grace period for the same
     * reason FR-ACQ-12 gives one to the thermocouple -- a single burst spoiled
     * by switching noise is not a missing CT. */
    float    ct_fault_window_s;

    /* SR-25 relay fail-on.  Consecutive *measurement windows*, not seconds:
     * the measurement cadence is the sampler's business, and counting windows is
     * what the requirement says. */
    float    fail_on_threshold_a;
    uint8_t  fail_on_windows;

    /* SR-26 relay or element fail-off.  The threshold is a fraction of the run
     * reference; fail_off_min_a is the absolute floor used before a reference has
     * been established, so the rule is not blind during the cold full-power
     * stretch at the start of a firing -- which is exactly when a dead element
     * group is most detectable. */
    float    fail_off_fraction;
    float    fail_off_min_a;
    float    fail_off_window_s;
    /* And a minimum number of consecutive low measurements, not elapsed time
     * alone.  SR-25 counts windows and is the more robust rule for it: a rule
     * decided purely on a timer can be tipped over by one unrepresentative
     * measurement that happens to be the last before the window expires -- the
     * first on-window of a run, say, caught while the contactor is still
     * closing and reading near zero through no fault of the kiln.  Requiring
     * both is strictly more evidence for the same conclusion and costs a
     * fraction of a second. */
    uint8_t  fail_off_min_windows;

    /* SR-27 weld discrimination.  NFR-27 allows 1 s from the offending window to
     * de-energising and a further 3 s to the verdict, so the wait for the
     * contactor to drop plus the re-measurement must fit inside 3 s. */
    float    weld_wait_s;
    float    weld_verdict_s;

    /* SR-28 current deviation */
    float    deviation_warn_frac;
    float    deviation_fault_frac;
    float    deviation_window_s;
    uint8_t  deviation_min_windows;   /* as fail_off_min_windows */

    /* SR-29 over-current */
    float    overcurrent_a;
    uint8_t  overcurrent_windows;

    /* SR-30 relay wear */
    uint32_t contactor_life_ops;
    uint32_t ssr_life_ops;
    uint8_t  mismatch_episodes_warn;
} kiln_safety_cfg_t;

void kiln_safety_cfg_defaults(kiln_safety_cfg_t *cfg);

/* FR-CUR-02's nominal current drives three defaults at once (fail-off floor,
 * over-current limit, deviation baseline before a reference exists).  Derive
 * them in one place so a configured nominal cannot leave one of them behind. */
void kiln_safety_cfg_set_nominal_current(kiln_safety_cfg_t *cfg, float nominal_a);

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

    /* FR-ACQ-12: false while the reading is inside the grace period and must not
     * be believed.  Without these the temperature rules consume a reading the
     * acquisition layer has already declared meaningless -- which for SR-06 and
     * SR-07 means a timer accumulating against a number that is not a
     * measurement. */
    bool     kiln_valid;
    bool     case_valid;

    /* --- heater current ------------------------------------------------ */

    /* FR-CUR-12: monitoring is configured on *and* the channel is delivering.
     * With it false the current rules stand down and warning 111 is raised; the
     * thermal rules carry the load alone. */
    bool     current_monitoring;
    /* A measurement landed since the previous evaluation.  The window-counting
     * rules advance only on a fresh measurement, so a safety cycle faster than
     * the measurement cadence cannot count the same window twice. */
    bool     current_fresh;
    float    current_a;
    float    current_ref_a;            /* FR-CUR-08, 0 when not yet learned   */
    float    current_deviation;        /* SR-28, signed fraction vs reference */
    bool     current_deviation_valid;
    uint8_t  current_flags;            /* KILN_CURF_*                         */

    /* SR-27: has the heat-enable line been de-asserted, so the contactor should
     * have opened?  The discrimination sequence is a statement about what the
     * current did after this went false. */
    bool     heat_enable_asserted;

    /* FR-CUR-13, feeding SR-30's wear warning. */
    uint32_t contactor_ops;
    uint32_t ssr_ops[KILN_HEAT_CHANNELS];
} kiln_safety_input_t;

typedef struct {
    bool         heat_permitted;
    /* SR-27: de-assert heat enable now, so the contactor opens and the
     * discrimination sequence has something to measure.  Distinct from
     * !heat_permitted, which only withdraws the SSR duty -- the contactor is the
     * second interrupting device and dropping it is the point of the test. */
    bool         drop_contactor;
    kiln_fault_t fault;                /* KILN_FAULT_NONE when none tripped   */
    uint32_t     warnings;             /* KILN_WARN_BIT(...) mask             */
} kiln_safety_verdict_t;

/* One rule's accumulator.  Grouped per rule so a test can inspect exactly the
 * rule it is exercising. */
typedef struct {
    float timer_s;
    float confirm_s;      /* how long the trip condition has held */
    float ref_c;
    float min_c, max_c;
    bool  armed;
} kiln_rule_state_t;

/* SR-27's sequence, which is the one rule that is not a timer: de-assert, wait
 * for the contactor to drop, re-measure, and read the verdict off whether the
 * current stopped. */
typedef enum {
    KILN_WELD_IDLE = 0,
    KILN_WELD_WAIT_DROPOUT,   /* heat enable de-asserted, contactor opening   */
    KILN_WELD_REMEASURE,      /* waiting for a fresh leakage measurement      */
    KILN_WELD_DONE,
} kiln_weld_phase_t;

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

    /* --- current rule state ------------------------------------------- */
    kiln_rule_state_t ct_fault;        /* FR-CUR-11 grace                    */
    uint8_t  fail_on_count;            /* SR-25 consecutive off-windows       */
    uint8_t  overcurrent_count;        /* SR-29                               */
    float    fail_off_timer_s;         /* SR-26                               */
    uint8_t  fail_off_windows;         /* consecutive low conduction windows  */
    bool     fail_off_below;           /* last conduction window was low      */
    float    deviation_timer_s;        /* SR-28                               */
    uint8_t  deviation_windows;

    kiln_weld_phase_t weld_phase;      /* SR-27 */
    float             weld_timer_s;
    bool              weld_saw_current;
    kiln_fault_t      weld_fault;      /* the verdict, once reached           */

    /* SR-30: a mismatch that accumulated and then cleared on its own is the
     * early sign the requirement asks for -- a relay that is becoming defective
     * before it fails outright. */
    uint16_t mismatch_episodes;
    bool     fail_on_episode_open;
    bool     fail_off_episode_open;

    /* Appendix A defines a warning as non-latching, so the mask is rebuilt every
     * evaluation.  Only the genuinely episodic ones -- a conclusion drawn once
     * from evidence that has since passed -- are carried here. */
    uint32_t latched_warnings;
} kiln_safety_t;

void kiln_safety_init(kiln_safety_t *s, const kiln_safety_cfg_t *cfg);
void kiln_safety_reconfigure(kiln_safety_t *s, const kiln_safety_cfg_t *cfg);

/* Reset all per-run accumulators.  Called on entering a heating state. */
void kiln_safety_begin_run(kiln_safety_t *s, const kiln_insulation_baseline_t *baseline);

/* Evaluate every rule.  dt_s is the interval since the previous call.
 *
 * A NULL argument or a non-finite or negative dt_s is a caller bug: the verdict
 * withholds heat and reports no fault.  It deliberately does NOT report
 * KILN_FAULT_SAFETY_DEADLINE, which means SR-13's missed deadline and nothing
 * else.  Use kiln_safety_eval_checked when the caller wants to know. */
kiln_safety_verdict_t kiln_safety_eval(kiln_safety_t *s,
                                       const kiln_safety_input_t *in,
                                       float dt_s);

/* NFR-17: the same evaluation, with an invalid argument reported as an invalid
 * argument instead of being conflated with SR-13's missed deadline (fault 14).
 * The verdict is still fail-safe in that case. */
kiln_err_t kiln_safety_eval_checked(kiln_safety_t *s,
                                    const kiln_safety_input_t *in,
                                    float dt_s,
                                    kiln_safety_verdict_t *out);

/* SR-18: may a latched fault be cleared?  False while its triggering condition
 * is still observably true.  The same thresholds are reused, so there is no
 * second implementation to disagree with the detector.
 *
 * SR-17/SR-18 are a fail-safe decision, so this is an allow-list: a code that is
 * not listed is NOT clearable.  A new fault code therefore defaults to needing a
 * deliberate decision about its clearability, rather than inheriting
 * "acknowledge and carry on" by omission. */
bool kiln_safety_can_clear(const kiln_safety_cfg_t *cfg,
                           kiln_fault_t code,
                           const kiln_safety_input_t *in);

/* SR-12: duty-seconds this run needed to reach each band, for the run record. */
const uint32_t *kiln_safety_band_duty(const kiln_safety_t *s, uint8_t *count);

#endif

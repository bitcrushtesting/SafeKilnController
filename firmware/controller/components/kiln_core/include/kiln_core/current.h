/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Heater current measurement -- SWR-CUR-02..SWR-CUR-08, SWR-CUR-11, architecture
 * sections 5.1 and SWA-17.
 *
 * What this component is for: a time-proportional output at 2 % duty is off 98 %
 * of the time, so a blind average of the CT reads ~2 % of full current and says
 * nothing about the relay.  Gating the measurement to the commanded window is
 * what turns it into a statement about the *relay* instead -- "current while I
 * asked for off" and "no current while I asked for on" -- which is what SWR-SAF-25 and
 * SWR-SAF-26 act on.
 *
 * Division of labour: this component measures and scales.  It does not decide.
 * The thresholds, timers and latching of SWR-SAF-25..SWR-SAF-30 are in `safety`, fed the
 * reading and its flags, so there is one place where heat is withheld and one
 * place where a current is turned into amps.
 *
 * Pure, like the rest of the core: no clock, no allocation, no globals.  Time
 * arrives as a dt and samples arrive as a burst (SWA-02, SWA-03, SWR-TST-03, SWR-TST-04).
 *
 * OQ-06 (one CT on a representative phase, or one per phase) is still open.  A
 * kiln_current_t is one channel, so a second and third are additive -- an array
 * of these and a rule that reduces across them -- rather than a rewrite.
 */
#ifndef KILN_CORE_CURRENT_H
#define KILN_CORE_CURRENT_H

#include "kiln/err.h"
#include "kiln/types.h"
#include "kiln_ports/port_current.h"

/* SWR-CUR-08: the reference is a median, so an outlier -- the one window that
 * caught a contactor still closing -- cannot move it.  Nine cold full-on windows
 * is a couple of minutes at the start of a firing. */
constexpr size_t KILN_CUR_REF_SAMPLES = 9;

typedef struct {
    /* SWR-CUR-12: with monitoring disabled every measurement is suppressed and
     * the rules fall back to the thermal detections, under warning 111. */
    bool     enabled;

    /* SWR-CUR-03: >= 1 kHz, over a whole number of mains cycles so the result is
     * independent of sampling phase. */
    uint32_t sample_rate_hz;
    uint16_t mains_hz;              /* 50 or 60                              */
    uint16_t cycles_per_burst;      /* >= 1, default 2                       */

    /* SWR-CUR-04: 0..200 ms, default 20.  Covers zero-cross turn-on and CT
     * settling, so the burst does not straddle the switching edge. */
    uint16_t settle_ms;

    /* How often to re-measure inside a window that does not end.  At duty 0 the
     * commanded-off interval is unbounded, and that is precisely where SWR-SAF-25
     * matters most -- a relay stuck on while the controller believes it is idle.
     * Measuring once per window would give SWR-SAF-25 a single sample and then
     * silence.  SWR-NFR-27 allows 1 s from the offending window to de-energising,
     * and SWR-SAF-25 wants two consecutive windows, so the cadence has to be
     * comfortably under 500 ms. */
    uint16_t remeasure_ms;

    /* Scaling, counts -> amps.  Split in two because they come from different
     * places: the CT and its burden are a hardware fact (SYS-HW-16), the ADC scale
     * is a calibration of the board. */
    float    ct_a_per_v;            /* voltage-output CT: amps per volt RMS   */
    float    adc_v_per_count;

    /* SWR-CUR-06 */
    float    cal_gain;              /* 0.50 .. 2.00                          */
    float    zero_offset_a;         /* subtracted after scaling              */

    /* SWR-CUR-11.  A healthy channel always shows *some* noise; a transformer
     * that is absent, unplugged or shorted shows none at all, and an input with
     * no DC path drifts off the mid-rail bias the conditioning establishes.  The
     * two tests together separate "no signal" from "zero current", which is the
     * distinction the requirement actually asks for. */
    float    noise_floor_counts;    /* RMS deviation below this => no signal  */
    uint16_t bias_min_counts;
    uint16_t bias_max_counts;

    /* SWR-CUR-07.  A resistive load is assumed, which is true of kiln elements
     * and is stated wherever the figure is presented. */
    float    mains_v;

    /* SWR-CUR-02 nominal range, and the conditions under which the SWR-CUR-08
     * reference may be learned ("while the elements are cold and fully on"). */
    float    nominal_a;
    float    ref_cold_max_c;        /* default 200 degC                      */

    /* How far the learned reference may sit from the configured nominal before
     * it is rejected, as a fraction.
     *
     * This is not belt-and-braces -- without it the reference is a hole straight
     * through SWR-SAF-26.  "Cold and fully on" is exactly the condition a kiln with a
     * failed SSR, an open contactor or dead elements is also in, so the median of
     * those windows is the *fault* current, and SWR-SAF-26's threshold of 20 % of the
     * reference then sits below the noise floor.  The rule would measure no
     * current, compare it against a reference of no current, and conclude that
     * all was well.
     *
     * Rejected means ref_valid stays false, which is the state SWR-SAF-26's absolute
     * nominal-derived floor exists to cover.  The window is generous, because a
     * mis-entered nominal should cost accuracy rather than the whole rule. */
    float    ref_min_frac;          /* default 0.25 of nominal               */
    float    ref_max_frac;          /* default 2.00 of nominal               */

    /* SWR-SAF-28: elements have a positive temperature coefficient, so conduction
     * current falls as the kiln heats.  Comparing a hot reading against a cold
     * reference without correcting for it would read as a lost element group.
     * OQ-07 decides whether this is measured on the first firing or entered from
     * the element datasheet; either way it arrives here as a number.  Zero
     * disables the correction. */
    float    element_tc_per_c;      /* fractional resistance rise per degC   */
} kiln_current_cfg_t;

void kiln_current_cfg_defaults(kiln_current_cfg_t *cfg);

/* What the gating state machine wants done with the front end next. */
typedef enum {
    KILN_CUR_ACT_NONE = 0,
    KILN_CUR_ACT_START_BURST,   /* settle has elapsed and the window is long enough */
    KILN_CUR_ACT_ABORT,         /* the window closed under a burst in flight        */
} kiln_cur_action_t;

typedef enum {
    KILN_CUR_GATE_WAIT_SETTLE = 0,
    KILN_CUR_GATE_ARMED,        /* burst requested, waiting for samples */
    KILN_CUR_GATE_MEASURED,     /* this window is done                  */
    KILN_CUR_GATE_SKIPPED,      /* SWR-CUR-05: too short to measure      */
} kiln_cur_gate_t;

typedef struct {
    kiln_current_cfg_t cfg;

    /* Derived once, so the tick does no arithmetic it need not. */
    uint16_t samples_per_burst;
    float    amps_per_count;

    /* Gating (SWR-CUR-04, SWR-CUR-05). */
    kiln_cur_window_t window;
    kiln_cur_gate_t   gate;
    float             window_age_ms;
    float             since_measure_ms;
    bool              have_window;

    /* Latest measurement. */
    float    current_a;
    uint8_t  flags;              /* KILN_CURF_*                             */
    float    bias_counts;        /* measured DC bias, for diagnostics       */
    float    rms_counts;
    float    conduction_a;       /* last conduction reading, for SWR-CUR-07/SWR-SAF-28 */
    uint32_t measurements;       /* bursts actually reduced to a reading    */
    uint32_t skipped;            /* SWR-CUR-05 windows                       */

    /* SWR-CUR-08 */
    float    ref_pool[KILN_CUR_REF_SAMPLES];
    uint8_t  ref_pool_count;
    float    ref_a;
    float    ref_temp_c;         /* the temperature the reference was taken at */
    bool     ref_valid;
    bool     ref_rejected;       /* a median was reached and found implausible */

    /* SWR-CUR-07 */
    float    apparent_va;
    double   energy_wh;

    /* Plant context, refreshed by the control cycle: needed to decide whether a
     * conduction measurement qualifies for the reference pool, and to weight the
     * energy integral by the duty actually commanded. */
    float    plant_c;
    uint16_t plant_duty_permille;
} kiln_current_t;

kiln_err_t kiln_current_init(kiln_current_t *c, const kiln_current_cfg_t *cfg);
kiln_err_t kiln_current_reconfigure(kiln_current_t *c, const kiln_current_cfg_t *cfg);

/* Clear the per-run accumulators: reference pool, energy, counters.  The
 * calibration and configuration survive. */
void kiln_current_begin_run(kiln_current_t *c);

/* Drive the gating from the same 10 ms tick that owns the SSR pin (SWA-17).
 *
 *   window                 the level the output is commanded to *now*
 *   window_remaining_ms    how much longer that level will hold, which the
 *                          window generator knows and the sampler cannot
 *   dt_ms                  tick period
 *
 * Returns the action to apply to port_current, and for KILN_CUR_ACT_START_BURST
 * writes the sample count to request.  Never blocks and never allocates
 * (SWR-CUR-14). */
kiln_cur_action_t kiln_current_tick(kiln_current_t *c,
                                    kiln_cur_window_t window,
                                    uint32_t window_remaining_ms,
                                    uint32_t dt_ms,
                                    uint16_t *n_samples_out);

/* Reduce one collected burst to a reading.  Rejects a burst whose window no
 * longer matches the commanded state, rather than reporting a conduction current
 * measured partly in an off interval. */
kiln_err_t kiln_current_push_burst(kiln_current_t *c, const kiln_cur_burst_t *b);

/* The control cycle's view of the plant, for reference learning (SWR-CUR-08) and
 * the energy integral (SWR-CUR-07).  dt_s is the interval since the previous
 * call. */
void kiln_current_note_plant(kiln_current_t *c, float kiln_c,
                             uint16_t duty_permille, float dt_s);

/* No measurement arrived for a whole control cycle: the published reading is
 * carried over, and says so (KILN_CURF_STALE). */
void kiln_current_mark_stale(kiln_current_t *c);

/* SWR-CUR-06: one-point calibration against a known load or reference meter.
 * Adjusts cal_gain so the present reading reads known_a.  Refuses a reading that
 * is not a usable conduction measurement, because calibrating against a leakage
 * or skipped window would silently produce a nonsense gain. */
kiln_err_t kiln_current_calibrate(kiln_current_t *c, float known_a);

/* SWR-SAF-28: measured conduction current against the run reference, corrected for
 * the element temperature coefficient.  Returns the signed fractional deviation
 * (-0.25 is 25 % low) and false when there is nothing valid to compare. */
bool kiln_current_deviation(const kiln_current_t *c, float *deviation_out);

/* The reference the run record carries (SWR-RUN-07). */
static inline bool  kiln_current_ref_valid(const kiln_current_t *c) { return c->ref_valid; }
static inline float kiln_current_ref(const kiln_current_t *c)       { return c->ref_a; }
static inline float kiln_current_amps(const kiln_current_t *c)      { return c->current_a; }
static inline uint8_t kiln_current_flags(const kiln_current_t *c)   { return c->flags; }
static inline float kiln_current_apparent_va(const kiln_current_t *c) { return c->apparent_va; }
static inline double kiln_current_energy_wh(const kiln_current_t *c) { return c->energy_wh; }

/* True when the channel is delivering usable measurements: enabled, and not
 * reporting a transformer fault.  SWR-CUR-12's start gate. */
static inline bool kiln_current_available(const kiln_current_t *c)
{
    return c->cfg.enabled && ((c->flags & KILN_CURF_CT_FAULT) == 0u);
}

#endif /* KILN_CORE_CURRENT_H */

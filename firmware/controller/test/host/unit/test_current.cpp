/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * kiln_core/current -- FR-CUR-02..FR-CUR-08 and FR-CUR-11.
 */

#include <math.h>
#include "kiln_check.h"
#include "kiln_core/current.h"

#define PI 3.14159265358979323846

static kiln_current_cfg_t cfg(void)
{
    kiln_current_cfg_t c;
    kiln_current_cfg_defaults(&c);
    return c;
}

/* Synthesise what HR-17's conditioning delivers for a given RMS current: a sine
 * about the mid-rail bias, plus a noise floor. */
static uint16_t g_samples[KILN_CUR_BURST_MAX];

static kiln_cur_burst_t make_burst(const kiln_current_cfg_t *c, float rms_a,
                                   kiln_cur_window_t window, uint16_t n)
{
    const float amps_per_count = c->adc_v_per_count * c->ct_a_per_v;
    const float peak           = rms_a * 1.41421356f / amps_per_count;

    for (uint16_t i = 0; i < n; i++) {
        const double phase = 2.0 * PI * (double)c->mains_hz *
                             ((double)i / (double)c->sample_rate_hz);
        /* A deterministic dither, so the noise floor is present without making
         * the test depend on a particular random sequence. */
        const double dither = ((i % 7) - 3) * 0.5;
        double v = (double)c->bias_min_counts +
                   ((double)c->bias_max_counts - c->bias_min_counts) / 2.0 +
                   peak * sin(phase) + dither;
        if (v < 0.0) {
            v = 0.0;
        }
        if (v > 4095.0) {
            v = 4095.0;
        }
        g_samples[i] = (uint16_t)lround(v);
    }

    kiln_cur_burst_t b = {};
    b.samples        = g_samples;
    b.count          = n;
    b.sample_rate_hz = c->sample_rate_hz;
    b.window         = window;
    return b;
}

/* Walk a current component to the point where it has asked for a burst. */
static uint16_t arm(kiln_current_t *c, kiln_cur_window_t window)
{
    uint16_t n = 0;
    for (int i = 0; i < 200; i++) {
        const kiln_cur_action_t a = kiln_current_tick(c, window, 2000u, 10u, &n);
        if (a == KILN_CUR_ACT_START_BURST) {
            return n;
        }
    }
    return 0;
}

/* --- FR-CUR-02, FR-CUR-03 ---------------------------------------------- */

KILN_TEST(frcur02_measures_rms_to_the_required_accuracy)
{
    const kiln_current_cfg_t cc = cfg();
    kiln_current_t c;
    CHECK_OK(kiln_current_init(&c, &cc));

    const float wanted[] = { 1.0f, 5.0f, 15.0f, 30.0f, 55.0f };

    for (size_t i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
        kiln_current_t ch = c;
        const uint16_t n = arm(&ch, KILN_CUR_WINDOW_ON);
        CHECK(n > 0);

        const kiln_cur_burst_t b = make_burst(&cc, wanted[i], KILN_CUR_WINDOW_ON, n);
        CHECK_OK(kiln_current_push_burst(&ch, &b));

        /* FR-CUR-02: +/-3 % of reading or +/-0.3 A, whichever is greater. */
        const float tol = wanted[i] * 0.03f > 0.3f ? wanted[i] * 0.03f : 0.3f;
        CHECK_NEAR(kiln_current_amps(&ch), wanted[i], tol);
        CHECK(kiln_current_flags(&ch) & KILN_CURF_CONDUCTION);
    }
}

KILN_TEST(frcur03_result_is_independent_of_sampling_phase)
{
    /* The point of whole mains cycles: start the burst anywhere in the waveform
     * and the answer must not move. */
    const kiln_current_cfg_t cc = cfg();
    kiln_current_t c;
    CHECK_OK(kiln_current_init(&c, &cc));

    const uint16_t n = arm(&c, KILN_CUR_WINDOW_ON);
    const float amps_per_count = cc.adc_v_per_count * cc.ct_a_per_v;
    const float peak = 20.0f * 1.41421356f / amps_per_count;

    float first = 0.0f;
    for (int shift = 0; shift < 4; shift++) {
        for (uint16_t i = 0; i < n; i++) {
            const double phase = 2.0 * PI * cc.mains_hz *
                                 ((double)(i + shift * 7) / (double)cc.sample_rate_hz);
            g_samples[i] = (uint16_t)lround(2048.0 + peak * sin(phase));
        }
        kiln_cur_burst_t b = {};
        b.samples = g_samples; b.count = n;
        b.sample_rate_hz = cc.sample_rate_hz; b.window = KILN_CUR_WINDOW_ON;

        kiln_current_t ch = c;
        (void)arm(&ch, KILN_CUR_WINDOW_ON);
        CHECK_OK(kiln_current_push_burst(&ch, &b));

        if (shift == 0) {
            first = kiln_current_amps(&ch);
        }
        else {
            CHECK_NEAR(kiln_current_amps(&ch), first, 0.15f);
        }
    }
}

/* --- FR-CUR-04, FR-CUR-05: gating -------------------------------------- */

KILN_TEST(frcur04_waits_out_the_settle_delay_before_measuring)
{
    kiln_current_cfg_t cc = cfg();
    cc.settle_ms = 50;
    kiln_current_t c;
    CHECK_OK(kiln_current_init(&c, &cc));

    uint16_t n = 0;
    /* The first tick opens the window; the settle delay runs from there. */
    CHECK_EQ_INT(kiln_current_tick(&c, KILN_CUR_WINDOW_ON, 2000u, 10u, &n),
                 KILN_CUR_ACT_NONE);
    for (int ms = 10; ms < 50; ms += 10) {
        CHECK_EQ_INT(kiln_current_tick(&c, KILN_CUR_WINDOW_ON, 2000u, 10u, &n),
                     KILN_CUR_ACT_NONE);
    }
    CHECK_EQ_INT(kiln_current_tick(&c, KILN_CUR_WINDOW_ON, 2000u, 10u, &n),
                 KILN_CUR_ACT_START_BURST);
    CHECK(n > 0);
}

KILN_TEST(frcur05_skips_a_window_too_short_to_measure)
{
    const kiln_current_cfg_t cc = cfg();
    kiln_current_t c;
    CHECK_OK(kiln_current_init(&c, &cc));

    /* 30 ms cannot hold a 20 ms settle plus a whole mains cycle.  Skipped, and
     * flagged as such -- never reported as zero current, which SR-25 would read
     * as a healthy relay and SR-26 as a dead element. */
    uint16_t n = 0;
    CHECK_EQ_INT(kiln_current_tick(&c, KILN_CUR_WINDOW_ON, 30u, 10u, &n),
                 KILN_CUR_ACT_NONE);
    CHECK_EQ_INT(c.gate, KILN_CUR_GATE_SKIPPED);
    CHECK(kiln_current_flags(&c) & KILN_CURF_SKIPPED);
    CHECK_EQ_UINT(c.skipped, 1u);

    for (int i = 0; i < 10; i++) {
        CHECK_EQ_INT(kiln_current_tick(&c, KILN_CUR_WINDOW_ON, 30u, 10u, &n),
                     KILN_CUR_ACT_NONE);
    }
}

KILN_TEST(frcur04_aborts_a_burst_when_the_window_closes_under_it)
{
    const kiln_current_cfg_t cc = cfg();
    kiln_current_t c;
    CHECK_OK(kiln_current_init(&c, &cc));

    CHECK(arm(&c, KILN_CUR_WINDOW_ON) > 0);
    CHECK_EQ_INT(c.gate, KILN_CUR_GATE_ARMED);

    /* The level changed: those samples would straddle the switching edge. */
    uint16_t n = 0;
    CHECK_EQ_INT(kiln_current_tick(&c, KILN_CUR_WINDOW_OFF, 2000u, 10u, &n),
                 KILN_CUR_ACT_ABORT);
}

KILN_TEST(a_burst_from_the_wrong_window_is_rejected)
{
    const kiln_current_cfg_t cc = cfg();
    kiln_current_t c;
    CHECK_OK(kiln_current_init(&c, &cc));

    const uint16_t n = arm(&c, KILN_CUR_WINDOW_ON);
    const kiln_cur_burst_t b = make_burst(&cc, 25.0f, KILN_CUR_WINDOW_OFF, n);
    CHECK_ERR(kiln_current_push_burst(&c, &b), KILN_ERR_STATE);
}

KILN_TEST(an_unrequested_burst_is_rejected)
{
    const kiln_current_cfg_t cc = cfg();
    kiln_current_t c;
    CHECK_OK(kiln_current_init(&c, &cc));

    const kiln_cur_burst_t b = make_burst(&cc, 25.0f, KILN_CUR_WINDOW_ON, 160);
    CHECK_ERR(kiln_current_push_burst(&c, &b), KILN_ERR_STATE);
}

KILN_TEST(an_unbounded_off_window_is_remeasured_so_sr25_keeps_its_evidence)
{
    /* At duty 0 the commanded-off interval never ends, and that is exactly where
     * SR-25 matters most.  Measuring once and falling silent would leave the rule
     * with a single sample. */
    const kiln_current_cfg_t cc = cfg();
    kiln_current_t c;
    CHECK_OK(kiln_current_init(&c, &cc));

    int measurements = 0;
    uint16_t n = 0;
    for (int ms = 0; ms < 2000; ms += 10) {
        const kiln_cur_action_t a =
            kiln_current_tick(&c, KILN_CUR_WINDOW_OFF, 60000u, 10u, &n);
        if (a == KILN_CUR_ACT_START_BURST) {
            const kiln_cur_burst_t b = make_burst(&cc, 0.0f, KILN_CUR_WINDOW_OFF, n);
            CHECK_OK(kiln_current_push_burst(&c, &b));
            measurements++;
        }
    }
    /* Two consecutive windows inside NFR-27's one second. */
    CHECK(measurements >= 4);
}

/* --- FR-CUR-06 --------------------------------------------------------- */

KILN_TEST(frcur06_one_point_calibration_makes_the_reading_match_the_meter)
{
    const kiln_current_cfg_t cc = cfg();
    kiln_current_t c;
    CHECK_OK(kiln_current_init(&c, &cc));

    const uint16_t n = arm(&c, KILN_CUR_WINDOW_ON);
    const kiln_cur_burst_t b = make_burst(&cc, 20.0f, KILN_CUR_WINDOW_ON, n);
    CHECK_OK(kiln_current_push_burst(&c, &b));

    /* The reference meter says 22.0 A. */
    CHECK_OK(kiln_current_calibrate(&c, 22.0f));
    CHECK_NEAR(kiln_current_amps(&c), 22.0f, 0.01f);

    /* And the next measurement of the same primary current reads the new figure. */
    (void)arm(&c, KILN_CUR_WINDOW_ON);
    const kiln_cur_burst_t b2 = make_burst(&cc, 20.0f, KILN_CUR_WINDOW_ON, n);
    CHECK_OK(kiln_current_push_burst(&c, &b2));
    CHECK_NEAR(kiln_current_amps(&c), 22.0f, 0.7f);
}

KILN_TEST(frcur06_refuses_to_calibrate_against_a_measurement_that_is_not_one)
{
    const kiln_current_cfg_t cc = cfg();
    kiln_current_t c;
    CHECK_OK(kiln_current_init(&c, &cc));

    /* Nothing measured yet. */
    CHECK_ERR(kiln_current_calibrate(&c, 20.0f), KILN_ERR_STATE);

    /* A leakage window is not a load. */
    const uint16_t n = arm(&c, KILN_CUR_WINDOW_OFF);
    const kiln_cur_burst_t b = make_burst(&cc, 0.3f, KILN_CUR_WINDOW_OFF, n);
    CHECK_OK(kiln_current_push_burst(&c, &b));
    CHECK_ERR(kiln_current_calibrate(&c, 20.0f), KILN_ERR_STATE);

    CHECK_ERR(kiln_current_calibrate(&c, 0.0f), KILN_ERR_INVALID_ARG);
    CHECK_ERR(kiln_current_calibrate(&c, -5.0f), KILN_ERR_INVALID_ARG);
}

KILN_TEST(frcur06_refuses_a_calibration_gain_outside_its_range)
{
    const kiln_current_cfg_t cc = cfg();
    kiln_current_t c;
    CHECK_OK(kiln_current_init(&c, &cc));

    const uint16_t n = arm(&c, KILN_CUR_WINDOW_ON);
    const kiln_cur_burst_t b = make_burst(&cc, 20.0f, KILN_CUR_WINDOW_ON, n);
    CHECK_OK(kiln_current_push_burst(&c, &b));

    /* A meter reading 5x the measurement means the front end is wrong, not the
     * gain: 0.50..2.00 is the range the requirement allows. */
    CHECK_ERR(kiln_current_calibrate(&c, 100.0f), KILN_ERR_RANGE);
}

/* --- FR-CUR-08 --------------------------------------------------------- */

KILN_TEST(frcur08_learns_the_reference_from_cold_full_power_windows)
{
    const kiln_current_cfg_t cc = cfg();
    kiln_current_t c;
    CHECK_OK(kiln_current_init(&c, &cc));
    kiln_current_begin_run(&c);

    kiln_current_note_plant(&c, 60.0f, KILN_DUTY_MAX, 1.0f);

    CHECK(!kiln_current_ref_valid(&c));
    for (size_t i = 0; i < KILN_CUR_REF_SAMPLES; i++) {
        const uint16_t n = arm(&c, KILN_CUR_WINDOW_ON);
        /* A spread around 30 A, including one outlier that a mean would feel. */
        const float a = (i == 3) ? 12.0f : 30.0f + (float)(i % 3) * 0.2f;
        const kiln_cur_burst_t b = make_burst(&cc, a, KILN_CUR_WINDOW_ON, n);
        CHECK_OK(kiln_current_push_burst(&c, &b));
    }
    CHECK(kiln_current_ref_valid(&c));
    /* The median ignores the outlier. */
    CHECK_NEAR(kiln_current_ref(&c), 30.2f, 0.6f);
}

KILN_TEST(frcur08_does_not_learn_a_reference_from_a_hot_or_partial_window)
{
    const kiln_current_cfg_t cc = cfg();
    kiln_current_t c;
    CHECK_OK(kiln_current_init(&c, &cc));
    kiln_current_begin_run(&c);

    /* Hot: the temperature coefficient would bake itself into the baseline. */
    kiln_current_note_plant(&c, 900.0f, KILN_DUTY_MAX, 1.0f);
    for (size_t i = 0; i < KILN_CUR_REF_SAMPLES + 2; i++) {
        const uint16_t n = arm(&c, KILN_CUR_WINDOW_ON);
        const kiln_cur_burst_t b = make_burst(&cc, 25.0f, KILN_CUR_WINDOW_ON, n);
        CHECK_OK(kiln_current_push_burst(&c, &b));
    }
    CHECK(!kiln_current_ref_valid(&c));

    /* Not fully on: a partial window measures a different thing. */
    kiln_current_begin_run(&c);
    kiln_current_note_plant(&c, 50.0f, 500, 1.0f);
    for (size_t i = 0; i < KILN_CUR_REF_SAMPLES + 2; i++) {
        const uint16_t n = arm(&c, KILN_CUR_WINDOW_ON);
        const kiln_cur_burst_t b = make_burst(&cc, 25.0f, KILN_CUR_WINDOW_ON, n);
        CHECK_OK(kiln_current_push_burst(&c, &b));
    }
    CHECK(!kiln_current_ref_valid(&c));
}

/* --- SR-28 deviation, with the temperature correction ------------------ */

KILN_TEST(sr28_deviation_corrects_for_the_element_temperature_coefficient)
{
    kiln_current_cfg_t cc = cfg();
    cc.element_tc_per_c = 0.0005f;
    kiln_current_t c;
    CHECK_OK(kiln_current_init(&c, &cc));
    kiln_current_begin_run(&c);

    /* Learn 30 A cold. */
    kiln_current_note_plant(&c, 50.0f, KILN_DUTY_MAX, 1.0f);
    for (size_t i = 0; i < KILN_CUR_REF_SAMPLES; i++) {
        const uint16_t n = arm(&c, KILN_CUR_WINDOW_ON);
        const kiln_cur_burst_t b = make_burst(&cc, 30.0f, KILN_CUR_WINDOW_ON, n);
        CHECK_OK(kiln_current_push_burst(&c, &b));
    }
    CHECK(kiln_current_ref_valid(&c));

    /* At 1050 degC the expected current is 30 / (1 + 0.0005*1000) = 20 A.  A
     * perfectly healthy kiln therefore reads 33 % low against the cold
     * reference, and without the correction SR-28 would latch fault 24 on every
     * firing that got hot. */
    kiln_current_note_plant(&c, 1050.0f, KILN_DUTY_MAX, 1.0f);
    const uint16_t n = arm(&c, KILN_CUR_WINDOW_ON);
    const kiln_cur_burst_t b = make_burst(&cc, 20.0f, KILN_CUR_WINDOW_ON, n);
    CHECK_OK(kiln_current_push_burst(&c, &b));

    float dev = 99.0f;
    CHECK(kiln_current_deviation(&c, &dev));
    CHECK_NEAR(dev, 0.0f, 0.05f);

    /* Lose a third of the elements at that temperature, and it shows. */
    (void)arm(&c, KILN_CUR_WINDOW_ON);
    const kiln_cur_burst_t b2 = make_burst(&cc, 13.3f, KILN_CUR_WINDOW_ON, n);
    CHECK_OK(kiln_current_push_burst(&c, &b2));
    CHECK(kiln_current_deviation(&c, &dev));
    CHECK_NEAR(dev, -1.0f / 3.0f, 0.05f);
}

KILN_TEST(deviation_is_unavailable_without_a_usable_comparison)
{
    const kiln_current_cfg_t cc = cfg();
    kiln_current_t c;
    CHECK_OK(kiln_current_init(&c, &cc));
    kiln_current_begin_run(&c);

    float dev = 0.0f;
    CHECK(!kiln_current_deviation(&c, &dev));        /* no reference yet */
    CHECK(!kiln_current_deviation(&c, NULL));
    CHECK(!kiln_current_deviation(NULL, &dev));
}

/* --- FR-CUR-07 --------------------------------------------------------- */

KILN_TEST(frcur07_derives_apparent_power_and_accumulates_energy)
{
    const kiln_current_cfg_t cc = cfg();
    kiln_current_t c;
    CHECK_OK(kiln_current_init(&c, &cc));
    kiln_current_begin_run(&c);

    const uint16_t n = arm(&c, KILN_CUR_WINDOW_ON);
    const kiln_cur_burst_t b = make_burst(&cc, 30.0f, KILN_CUR_WINDOW_ON, n);
    CHECK_OK(kiln_current_push_burst(&c, &b));

    /* 30 A at 230 V, resistive load assumed. */
    CHECK_NEAR(kiln_current_apparent_va(&c), 6900.0f, 250.0f);

    /* One hour at half duty: half of 6.9 kW. */
    kiln_current_note_plant(&c, 500.0f, 500, 3600.0f);
    CHECK_NEAR(kiln_current_energy_wh(&c), 3450.0, 150.0);

    /* And nothing accumulates at zero duty. */
    const double was = kiln_current_energy_wh(&c);
    kiln_current_note_plant(&c, 500.0f, 0, 3600.0f);
    CHECK_NEAR(kiln_current_energy_wh(&c), was, 0.001);
}

/* --- FR-CUR-11 --------------------------------------------------------- */

KILN_TEST(frcur11_absent_transformer_is_distinguished_from_zero_current)
{
    const kiln_current_cfg_t cc = cfg();
    kiln_current_t c;
    CHECK_OK(kiln_current_init(&c, &cc));

    /* A genuinely zero current still carries the channel's noise floor, and sits
     * on the bias the conditioning holds it at: a real, valid, zero reading. */
    uint16_t n = arm(&c, KILN_CUR_WINDOW_OFF);
    const kiln_cur_burst_t zero = make_burst(&cc, 0.0f, KILN_CUR_WINDOW_OFF, n);
    CHECK_OK(kiln_current_push_burst(&c, &zero));
    CHECK(!(kiln_current_flags(&c) & KILN_CURF_CT_FAULT));
    CHECK(kiln_current_flags(&c) & KILN_CURF_LEAKAGE);
    CHECK_NEAR(kiln_current_amps(&c), 0.0f, 0.3f);

    /* No signal at all, not even noise: a shorted or absent winding. */
    n = arm(&c, KILN_CUR_WINDOW_OFF);
    for (uint16_t i = 0; i < n; i++) {
        g_samples[i] = 2048;
    }
    kiln_cur_burst_t dead = {};
    dead.samples = g_samples; dead.count = n;
    dead.sample_rate_hz = cc.sample_rate_hz; dead.window = KILN_CUR_WINDOW_OFF;
    CHECK_OK(kiln_current_push_burst(&c, &dead));
    CHECK(kiln_current_flags(&c) & KILN_CURF_CT_FAULT);
    CHECK_NEAR(kiln_current_amps(&c), 0.0f, 0.001f);

    /* Off the bias window entirely: an input with no DC path (tasklist A6). */
    n = arm(&c, KILN_CUR_WINDOW_OFF);
    for (uint16_t i = 0; i < n; i++) {
        g_samples[i] = (uint16_t)(300 + (i % 5));
    }
    kiln_cur_burst_t floating = dead;
    floating.count = n;
    CHECK_OK(kiln_current_push_burst(&c, &floating));
    CHECK(kiln_current_flags(&c) & KILN_CURF_CT_FAULT);
}

/* --- FR-CUR-12 and configuration --------------------------------------- */

KILN_TEST(frcur12_disabled_monitoring_suppresses_every_measurement)
{
    kiln_current_cfg_t cc = cfg();
    cc.enabled = false;
    kiln_current_t c;
    CHECK_OK(kiln_current_init(&c, &cc));

    uint16_t n = 0;
    for (int i = 0; i < 100; i++) {
        CHECK_EQ_INT(kiln_current_tick(&c, KILN_CUR_WINDOW_ON, 2000u, 10u, &n),
                     KILN_CUR_ACT_NONE);
    }
    CHECK_EQ_UINT(kiln_current_flags(&c), 0u);
    CHECK(!kiln_current_available(&c));

    const kiln_cur_burst_t b = make_burst(&cc, 30.0f, KILN_CUR_WINDOW_ON, 160);
    CHECK_ERR(kiln_current_push_burst(&c, &b), KILN_ERR_STATE);
}

KILN_TEST(configuration_outside_its_range_is_corrected_and_reported)
{
    /* NFR-17: a silently corrected configuration is a defect. */
    kiln_current_cfg_t cc = cfg();
    cc.sample_rate_hz = 100;        /* below FR-CUR-03's 1 kHz */
    cc.settle_ms      = 5000;       /* beyond FR-CUR-04's 200 ms */
    cc.cal_gain       = 9.0f;       /* beyond FR-CUR-06's 2.00 */
    cc.mains_hz       = 42;

    kiln_current_t c;
    CHECK_ERR(kiln_current_init(&c, &cc), KILN_ERR_RANGE);
    CHECK_EQ_UINT(c.cfg.sample_rate_hz, KILN_CUR_RATE_MIN_HZ);
    CHECK_EQ_UINT(c.cfg.settle_ms, 200u);
    CHECK_NEAR(c.cfg.cal_gain, 2.0f, 0.001f);
    CHECK_EQ_UINT(c.cfg.mains_hz, 50u);

    const kiln_current_cfg_t good = cfg();
    CHECK_OK(kiln_current_init(&c, &good));
}

KILN_TEST(reconfiguring_the_scale_invalidates_the_learned_reference)
{
    const kiln_current_cfg_t cc = cfg();
    kiln_current_t c;
    CHECK_OK(kiln_current_init(&c, &cc));
    kiln_current_begin_run(&c);

    kiln_current_note_plant(&c, 50.0f, KILN_DUTY_MAX, 1.0f);
    for (size_t i = 0; i < KILN_CUR_REF_SAMPLES; i++) {
        const uint16_t n = arm(&c, KILN_CUR_WINDOW_ON);
        const kiln_cur_burst_t b = make_burst(&cc, 30.0f, KILN_CUR_WINDOW_ON, n);
        CHECK_OK(kiln_current_push_burst(&c, &b));
    }
    CHECK(kiln_current_ref_valid(&c));

    /* A reference learned under the old scale is not the same quantity. */
    kiln_current_cfg_t other = cc;
    other.ct_a_per_v = 60.0f;
    CHECK_OK(kiln_current_reconfigure(&c, &other));
    CHECK(!kiln_current_ref_valid(&c));
}

KILN_TEST(null_and_nonfinite_arguments_are_refused)
{
    const kiln_current_cfg_t cc = cfg();
    kiln_current_t c;

    CHECK_ERR(kiln_current_init(NULL, &cc), KILN_ERR_INVALID_ARG);
    CHECK_ERR(kiln_current_init(&c, NULL), KILN_ERR_INVALID_ARG);
    CHECK_OK(kiln_current_init(&c, &cc));
    CHECK_ERR(kiln_current_push_burst(&c, NULL), KILN_ERR_INVALID_ARG);
    CHECK_ERR(kiln_current_calibrate(&c, 0.0f / 0.0f), KILN_ERR_INVALID_ARG);

    /* A non-finite temperature or dt must not enter the energy integral. */
    kiln_current_note_plant(&c, 0.0f / 0.0f, 500, 1.0f);
    kiln_current_note_plant(&c, 500.0f, 500, 0.0f / 0.0f);
    CHECK(kiln_is_finite((float)kiln_current_energy_wh(&c)));

    uint16_t n = 0;
    CHECK_EQ_INT(kiln_current_tick(NULL, KILN_CUR_WINDOW_ON, 100u, 10u, &n),
                 KILN_CUR_ACT_NONE);
}

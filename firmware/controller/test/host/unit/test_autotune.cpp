/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 * kiln_core/autotune -- FR-TUN-03..FR-TUN-09, SR-23, NFR-17.
 */

#include <math.h>
#include "kiln_check.h"
#include "kiln_core/autotune.h"

#define PI 3.14159265358979323846

static kiln_tune_cfg_t cfg(float sp)
{
    kiln_tune_cfg_t c;
    kiln_autotune_cfg_defaults(&c);
    c.setpoint_c = sp;
    return c;
}

/* --- FR-TUN-06: the published gain rules ------------------------------- */

KILN_TEST(frtun06_ziegler_nichols_matches_the_published_rule)
{
    /* Ku = 2, Tu = 100 s.  ZN PID: Kp = 0.6 Ku, Ti = 0.5 Tu, Td = 0.125 Tu. */
    const kiln_gains_t g = kiln_autotune_gains_from(2.0f, 100.0f, KILN_TUNE_RULE_ZN);
    CHECK_NEAR(g.kp, 1.2f, 0.001f);
    CHECK_NEAR(g.ki, 1.2f / 50.0f, 0.0001f);
    CHECK_NEAR(g.kd, 1.2f * 12.5f, 0.001f);
}

KILN_TEST(frtun06_tyreus_luyben_matches_the_published_rule)
{
    /* TL: Kp = Ku/2.2, Ti = 2.2 Tu, Td = Tu/6.3. */
    const kiln_gains_t g = kiln_autotune_gains_from(2.0f, 100.0f, KILN_TUNE_RULE_TL);
    CHECK_NEAR(g.kp, 2.0f / 2.2f, 0.001f);
    CHECK_NEAR(g.ki, (2.0f / 2.2f) / 220.0f, 0.00001f);
    CHECK_NEAR(g.kd, (2.0f / 2.2f) * (100.0f / 6.3f), 0.01f);
}

KILN_TEST(frtun06_nonsense_inputs_give_zero_gains)
{
    const kiln_gains_t a = kiln_autotune_gains_from(0.0f, 100.0f, KILN_TUNE_RULE_TL);
    CHECK_NEAR(a.kp, 0.0f, 0.001f);
    const kiln_gains_t b = kiln_autotune_gains_from(2.0f, 0.0f, KILN_TUNE_RULE_TL);
    CHECK_NEAR(b.kp, 0.0f, 0.001f);
    const kiln_gains_t c = kiln_autotune_gains_from(0.0f / 0.0f, 100.0f, KILN_TUNE_RULE_TL);
    CHECK_NEAR(c.kp, 0.0f, 0.001f);
}

/* --- SR-23: the clamp that B4 moved into the component ----------------- */

KILN_TEST(sr23_the_tuning_setpoint_is_clamped_to_the_configured_maximum)
{
    /* SR-23 names the tuning setpoint explicitly, so the clamp cannot be left to
     * the caller by comment -- a caller that forgets would tune a 900 degC kiln
     * at 1300 degC. */
    kiln_tune_cfg_t c = cfg(1300.0f);
    c.max_temp_c = 900.0f;

    kiln_autotune_t at;
    CHECK_ERR(kiln_autotune_start(&at, &c), KILN_ERR_RANGE);
    CHECK_NEAR(at.cfg.setpoint_c, 900.0f, 0.01f);

    /* And the compile-time ceiling still binds above it. */
    c.max_temp_c = 1e9f;
    c.setpoint_c = 5000.0f;
    CHECK_ERR(kiln_autotune_start(&at, &c), KILN_ERR_RANGE);
    CHECK_NEAR(at.cfg.setpoint_c, KILN_TEMP_CEILING_C, 0.01f);

    /* A setpoint inside the limit is accepted without complaint. */
    c.max_temp_c = 1280.0f;
    c.setpoint_c = 600.0f;
    CHECK_OK(kiln_autotune_start(&at, &c));
    CHECK_NEAR(at.cfg.setpoint_c, 600.0f, 0.01f);
}

/* --- FR-TUN-05: the settle test that B12 made usable ------------------- */

KILN_TEST(frtun_settle_uses_the_filtered_rate_not_a_single_sample_difference)
{
    /* At a 0.25 s cycle, 0.5 degC of sensor noise is 7200 degC/h against a
     * 30 degC/h threshold: computed from one sample difference the settle test
     * could essentially never pass, so SETTLE always fell through on its 30 min
     * timeout instead of on the transient having died away. */
    kiln_tune_cfg_t c = cfg(600.0f);
    c.settle_max_s = 1800.0f;

    kiln_autotune_t at;
    CHECK_OK(kiln_autotune_start(&at, &c));

    /* Approach. */
    for (int i = 0; i < 100 && at.phase == KILN_TUNE_APPROACH; i++) {
        (void)kiln_autotune_tick(&at, 600.0f, 0.0f, 0.25f);
    }
    CHECK_EQ_INT(at.phase, KILN_TUNE_SETTLE);

    /* Noisy readings, but a filtered rate that says "settled".  The phase moves
     * on in a couple of cycles rather than after half an hour. */
    float elapsed = 0.0f;
    for (int i = 0; i < 20 && at.phase == KILN_TUNE_SETTLE; i++) {
        const float noisy = 600.0f + (((i % 2) != 0) ? 0.5f : -0.5f);
        (void)kiln_autotune_tick(&at, noisy, 5.0f, 0.25f);
        elapsed += 0.25f;
    }
    CHECK_EQ_INT(at.phase, KILN_TUNE_RELAY);
    CHECK(elapsed < 10.0f);
}

KILN_TEST(frtun_settle_still_gives_up_on_its_timeout)
{
    kiln_tune_cfg_t c = cfg(600.0f);
    c.settle_max_s = 60.0f;

    kiln_autotune_t at;
    CHECK_OK(kiln_autotune_start(&at, &c));
    for (int i = 0; i < 100 && at.phase == KILN_TUNE_APPROACH; i++) {
        (void)kiln_autotune_tick(&at, 600.0f, 500.0f, 0.25f);
    }
    CHECK_EQ_INT(at.phase, KILN_TUNE_SETTLE);

    /* A rate that never settles. */
    for (int i = 0; i < 400 && at.phase == KILN_TUNE_SETTLE; i++) {
        (void)kiln_autotune_tick(&at, 600.0f, 500.0f, 0.25f);
    }
    CHECK_EQ_INT(at.phase, KILN_TUNE_RELAY);
}

/* --- the full procedure against a synthetic oscillation ---------------- */

/* Drive the state machine with a clean sinusoid of known amplitude and period,
 * which is what the describing-function estimate assumes. */
static bool run_identification(kiln_autotune_t *at, float amp_c, float period_s,
                               float peak_threshold_c, float *ku_out)
{
    kiln_tune_cfg_t c = cfg(600.0f);
    c.peak_threshold_c = peak_threshold_c;
    c.required_cycles  = 3;
    c.timeout_s        = 28800.0f;
    c.settle_max_s     = 1.0f;
    if (kiln_autotune_start(at, &c) != KILN_OK) {
        return false;
    }

    const float dt = 0.25f;
    float t = 0.0f;
    for (int i = 0; i < 400000 && !kiln_autotune_done(at); i++) {
        const float pv = 600.0f + amp_c * sinf(2.0f * (float)PI * t / period_s);
        (void)kiln_autotune_tick(at, pv, 0.0f, dt);
        t += dt;
    }
    if (!kiln_autotune_succeeded(at)) {
        return false;
    }
    *ku_out = at->ku;
    return true;
}

KILN_TEST(frtun05_identifies_ku_and_tu_from_a_qualified_oscillation)
{
    kiln_autotune_t at;
    float ku = 0.0f;
    CHECK(run_identification(&at, 5.0f, 600.0f, 0.1f, &ku));

    CHECK_NEAR(at.tu, 600.0f, 10.0f);

    /* Ku = 4d/(pi a) with d = 50 % duty and a = 5 degC. */
    const float expected = 4.0f * 50.0f / ((float)PI * 5.0f);
    CHECK_NEAR(ku, expected, expected * 0.10f);
}

/* Drive the state machine with a sinusoid plus deterministic noise. */
static bool run_identification_noisy(kiln_autotune_t *at, float amp_c, float period_s,
                                     float peak_threshold_c, float noise_c,
                                     float *ku_out)
{
    kiln_tune_cfg_t c = cfg(600.0f);
    c.peak_threshold_c = peak_threshold_c;
    c.required_cycles  = 3;
    c.timeout_s        = 28800.0f;
    c.settle_max_s     = 1.0f;
    if (kiln_autotune_start(at, &c) != KILN_OK) {
        return false;
    }

    const float dt = 0.25f;
    float t = 0.0f;
    uint32_t seed = 7u;
    for (int i = 0; i < 400000 && !kiln_autotune_done(at); i++) {
        seed = seed * 1103515245u + 12345u;
        const float n = noise_c *
            (((float)((seed >> 16u) & 0xFFFFu) / 32767.5f) - 1.0f);
        const float pv = 600.0f + amp_c * sinf(2.0f * (float)PI * t / period_s) + n;
        (void)kiln_autotune_tick(at, pv, 0.0f, dt);
        t += dt;
    }
    if (!kiln_autotune_succeeded(at)) {
        return false;
    }
    *ku_out = at->ku;
    return true;
}

KILN_TEST(frtun05_a_clean_oscillation_is_identified_exactly_at_any_threshold)
{
    /* The expectation here is easy to get backwards.  An extreme is confirmed
     * only once the measurement has reversed by the threshold -- but the value
     * recorded is the extreme itself, not the value that confirmed it, so there
     * is no amplitude bias from the threshold at all on a clean signal. */
    const float amp   = 5.0f;
    const float truth = 4.0f * 50.0f / ((float)PI * amp);

    const float thresholds[] = { 0.05f, 0.25f, 1.0f, 2.0f };
    for (size_t i = 0; i < sizeof(thresholds) / sizeof(thresholds[0]); i++) {
        kiln_autotune_t at;
        float ku = 0.0f;
        CHECK(run_identification_noisy(&at, amp, 600.0f, thresholds[i], 0.0f, &ku));
        CHECK_NEAR(ku, truth, truth * 0.01f);
        CHECK_NEAR(at.tu, 600.0f, 5.0f);
    }
}

KILN_TEST(frtun05_the_peak_threshold_is_what_buys_noise_immunity)
{
    /* What the threshold is actually for, measured: with half a degree of sensor
     * noise, a threshold below it lets noise manufacture extrema, the cycles
     * never agree within FR-TUN-05's tolerances, and the procedure fails on its
     * timeout having learned nothing.  That is why it is not a free parameter and
     * why the default sits comfortably above the noise floor. */
    const float amp   = 5.0f;
    const float truth = 4.0f * 50.0f / ((float)PI * amp);
    const float noise = 0.5f;

    kiln_autotune_t fine;
    float ku_fine = 0.0f;
    CHECK(!run_identification_noisy(&fine, amp, 600.0f, 0.25f, noise, &ku_fine));
    CHECK(kiln_autotune_done(&fine));
    CHECK(!kiln_autotune_succeeded(&fine));

    /* The default does identify it. */
    kiln_tune_cfg_t def;
    kiln_autotune_cfg_defaults(&def);
    CHECK(def.peak_threshold_c >= 1.0f);

    kiln_autotune_t at;
    float ku = 0.0f;
    CHECK(run_identification_noisy(&at, amp, 600.0f, def.peak_threshold_c, noise, &ku));

    /* And the residual bias is *low*-side: noise inflates the measured
     * half-amplitude (peak plus noise, trough minus noise), and Ku = 4d/(pi*a),
     * so a larger a gives a smaller Ku.  Low Ku means gentler gains, which is the
     * conservative direction -- about 9 % here. */
    CHECK(ku < truth);
    CHECK_MSG((truth - ku) / truth < 0.15f,
              "residual bias was %.1f %%", (double)(100.0f * (truth - ku) / truth));
}

KILN_TEST(frtun05_an_inconsistent_oscillation_does_not_qualify)
{
    kiln_tune_cfg_t c = cfg(600.0f);
    c.timeout_s    = 600.0f;
    c.settle_max_s = 1.0f;

    kiln_autotune_t at;
    CHECK_OK(kiln_autotune_start(&at, &c));

    /* An amplitude that grows every cycle: periods and amplitudes never agree
     * inside the tolerances, so FR-TUN-07's timeout decides. */
    const float dt = 0.25f;
    float t = 0.0f, amp = 2.0f;
    for (int i = 0; i < 100000 && !kiln_autotune_done(&at); i++) {
        const float pv = 600.0f + amp * sinf(2.0f * (float)PI * t / 120.0f);
        (void)kiln_autotune_tick(&at, pv, 0.0f, dt);
        t += dt;
        amp *= 1.0005f;
    }
    CHECK(kiln_autotune_done(&at));
    CHECK(!kiln_autotune_succeeded(&at));
    CHECK_EQ_INT(at.fail_reason, KILN_FAULT_TUNE_NO_CONVERGE);
}

KILN_TEST(frtun07_the_timeout_covers_the_whole_procedure)
{
    kiln_tune_cfg_t c = cfg(1200.0f);
    c.timeout_s = 600.0f;

    kiln_autotune_t at;
    CHECK_OK(kiln_autotune_start(&at, &c));

    /* A kiln that never reaches the tuning setpoint. */
    for (int i = 0; i < 10000 && !kiln_autotune_done(&at); i++) {
        (void)kiln_autotune_tick(&at, 300.0f, 10.0f, 0.25f);
    }
    CHECK_EQ_INT(at.phase, KILN_TUNE_FAILED);
    CHECK_EQ_INT(at.fail_reason, KILN_FAULT_TUNE_NO_CONVERGE);
    CHECK_EQ_UINT(kiln_autotune_tick(&at, 300.0f, 0.0f, 0.25f), 0u);
}

KILN_TEST(frtun09_success_presents_results_and_stores_nothing)
{
    kiln_autotune_t at;
    float ku = 0.0f;
    CHECK(run_identification(&at, 5.0f, 600.0f, 0.1f, &ku));

    CHECK_EQ_INT(at.phase, KILN_TUNE_PRESENT);
    /* Both rules are offered; the operator chooses (FR-TUN-06, FR-TUN-09). */
    CHECK(at.gains[KILN_TUNE_RULE_ZN].kp > 0.0f);
    CHECK(at.gains[KILN_TUNE_RULE_TL].kp > 0.0f);
    CHECK(at.gains[KILN_TUNE_RULE_ZN].kp > at.gains[KILN_TUNE_RULE_TL].kp);

    /* And no duty is requested once it is done. */
    CHECK_EQ_UINT(kiln_autotune_tick(&at, 600.0f, 0.0f, 0.25f), 0u);
}

KILN_TEST(the_identify_phase_is_actually_entered)
{
    /* It was declared, had a label, and was never reached: qualification
     * happened inline in RELAY. */
    kiln_autotune_t at;
    kiln_tune_cfg_t c = cfg(600.0f);
    c.peak_threshold_c = 0.1f;
    c.required_cycles  = 3;
    c.timeout_s        = 28800.0f;
    c.settle_max_s     = 1.0f;
    CHECK_OK(kiln_autotune_start(&at, &c));

    bool saw_identify = false;
    const float dt = 0.25f;
    float t = 0.0f;
    for (int i = 0; i < 400000 && !kiln_autotune_done(&at); i++) {
        const float pv = 600.0f + 5.0f * sinf(2.0f * (float)PI * t / 600.0f);
        (void)kiln_autotune_tick(&at, pv, 0.0f, dt);
        if (at.phase == KILN_TUNE_IDENTIFY) {
            saw_identify = true;
        }
        t += dt;
    }
    CHECK(saw_identify);
    CHECK_STR_EQ(kiln_autotune_phase_str(KILN_TUNE_IDENTIFY), "identify");
}

KILN_TEST(cancel_fails_the_procedure_with_an_operator_abort)
{
    kiln_autotune_t at;
    const kiln_tune_cfg_t c = cfg(600.0f);
    CHECK_OK(kiln_autotune_start(&at, &c));

    kiln_autotune_cancel(&at);
    CHECK_EQ_INT(at.phase, KILN_TUNE_FAILED);
    CHECK_EQ_INT(at.fail_reason, KILN_FAULT_OPERATOR_ABORT);
    CHECK(kiln_autotune_done(&at));
    CHECK(!kiln_autotune_succeeded(&at));
}

KILN_TEST(nfr17_a_non_finite_input_produces_no_duty_and_no_persistent_damage)
{
    kiln_autotune_t at;
    const kiln_tune_cfg_t c = cfg(600.0f);
    CHECK_OK(kiln_autotune_start(&at, &c));

    for (int i = 0; i < 10; i++) {
        (void)kiln_autotune_tick(&at, 500.0f, 10.0f, 0.25f);
    }
    const float nan = 0.0f / 0.0f;

    CHECK_EQ_UINT(kiln_autotune_tick(&at, nan, 10.0f, 0.25f), 0u);
    CHECK_EQ_UINT(kiln_autotune_tick(&at, 500.0f, nan, 0.25f), 0u);
    CHECK_EQ_UINT(kiln_autotune_tick(&at, 500.0f, 10.0f, nan), 0u);
    CHECK_EQ_UINT(kiln_autotune_tick(&at, 500.0f, 10.0f, 0.0f), 0u);
    CHECK_EQ_UINT(at.bad_calls, 4u);

    CHECK(kiln_is_finite(at.extreme_c));
    CHECK(kiln_is_finite(at.last_pv_c));
    CHECK(kiln_autotune_tick(&at, 500.0f, 10.0f, 0.25f) > 0u);
}

KILN_TEST(configuration_is_bounded_to_the_documented_ranges)
{
    kiln_tune_cfg_t c = cfg(600.0f);
    c.amplitude_permille = 5;
    c.hysteresis_c       = 100.0f;
    c.timeout_s          = 1.0f;
    c.required_cycles    = 0;

    kiln_autotune_t at;
    (void)kiln_autotune_start(&at, &c);
    CHECK_EQ_UINT(at.cfg.amplitude_permille, 100u);
    CHECK_NEAR(at.cfg.hysteresis_c, 20.0f, 0.001f);
    CHECK_NEAR(at.cfg.timeout_s, 600.0f, 0.001f);
    CHECK_EQ_UINT(at.cfg.required_cycles, 3u);

    CHECK_ERR(kiln_autotune_start(NULL, &c), KILN_ERR_INVALID_ARG);
    CHECK_ERR(kiln_autotune_start(&at, NULL), KILN_ERR_INVALID_ARG);
}

KILN_TEST(every_phase_and_rule_has_a_label)
{
    for (int p = 0; p <= KILN_TUNE_FAILED; p++) {
        const char *s = kiln_autotune_phase_str((kiln_tune_phase_t)p);
        CHECK(s && s[0] != '\0');
        CHECK(s[0] != '?');
    }
    for (int r = 0; r < KILN_TUNE_RULE_COUNT; r++) {
        const char *s = kiln_autotune_rule_str((kiln_tune_rule_t)r);
        CHECK(s && s[0] != '?');
    }
}

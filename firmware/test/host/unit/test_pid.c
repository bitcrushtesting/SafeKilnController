/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 * kiln_core/pid -- FR-CTL-03..FR-CTL-06, FR-CTL-16, NFR-17.
 */

#include "kiln_check.h"
#include "kiln_core/pid.h"

static kiln_pid_t make(float kp, float ki, float kd)
{
    const kiln_pid_cfg_t c = { .kp = kp, .ki = ki, .kd = kd,
                               .duty_max_permille = KILN_DUTY_MAX };
    kiln_pid_t p;
    kiln_pid_init(&p, &c);
    return p;
}

KILN_TEST(frctl03_proportional_term_is_percent_duty_per_degc)
{
    kiln_pid_t p = make(2.0f, 0.0f, 0.0f);
    /* 10 degC of error at 2 %/degC is 20 % duty, which is 200 per mille. */
    CHECK_EQ_UINT(kiln_pid_update(&p, 110.0f, 100.0f, 1.0f), 200u);
    CHECK_NEAR(p.p_pct, 20.0f, 0.001f);
}

KILN_TEST(frctl03_integral_term_accumulates_in_percent_per_degc_second)
{
    kiln_pid_t p = make(0.0f, 0.5f, 0.0f);
    /* 1 degC for 2 s at 0.5 %/(degC.s) is 1 % duty. */
    (void)kiln_pid_update(&p, 101.0f, 100.0f, 1.0f);
    CHECK_EQ_UINT(kiln_pid_update(&p, 101.0f, 100.0f, 1.0f), 10u);
}

KILN_TEST(frctl04_derivative_is_on_the_measurement_so_a_setpoint_step_does_not_kick)
{
    kiln_pid_t p = make(1.0f, 0.0f, 10.0f);
    (void)kiln_pid_update(&p, 100.0f, 100.0f, 1.0f);   /* primes pv history */

    /* The setpoint jumps 50 degC; the measurement has not moved. */
    (void)kiln_pid_update(&p, 150.0f, 100.0f, 1.0f);
    CHECK_NEAR(p.d_pct, 0.0f, 0.001f);

    /* The measurement rises: the derivative opposes it. */
    (void)kiln_pid_update(&p, 150.0f, 102.0f, 1.0f);
    CHECK_NEAR(p.d_pct, -20.0f, 0.001f);
}

KILN_TEST(frctl05_integral_does_not_wind_up_while_the_output_is_saturated)
{
    kiln_pid_t p = make(10.0f, 1.0f, 0.0f);

    for (int i = 0; i < 1000; i++) (void)kiln_pid_update(&p, 1000.0f, 0.0f, 1.0f);
    CHECK(p.saturated);
    CHECK(p.integral_pct <= 100.0f);

    /* And the output comes off the ceiling promptly once the error reverses,
     * rather than after unwinding an hour of accumulated integral. */
    uint16_t duty = KILN_DUTY_MAX;
    int cycles = 0;
    while (duty > 0 && cycles < 200) {
        duty = kiln_pid_update(&p, 0.0f, 100.0f, 1.0f);
        cycles++;
    }
    CHECK(cycles < 50);
}

KILN_TEST(frctl16_duty_ceiling_is_honoured_and_has_one_range)
{
    /* pid.h documents 100..1000, and both the initialiser and the setter now
     * accept exactly that -- a gain set loaded from NVS cannot carry a ceiling
     * the setter would have refused. */
    const kiln_pid_cfg_t c = { .kp = 100.0f, .duty_max_permille = 50 };
    kiln_pid_t p;
    kiln_pid_init(&p, &c);
    CHECK_EQ_UINT(p.cfg.duty_max_permille, KILN_PID_DUTY_MAX_MIN);

    kiln_pid_set_duty_max(&p, 50);
    CHECK_EQ_UINT(p.cfg.duty_max_permille, KILN_PID_DUTY_MAX_MIN);

    kiln_pid_set_duty_max(&p, 600);
    CHECK_EQ_UINT(kiln_pid_update(&p, 1000.0f, 0.0f, 1.0f), 600u);

    /* Zero still means "default". */
    const kiln_pid_cfg_t z = { .kp = 1.0f, .duty_max_permille = 0 };
    kiln_pid_init(&p, &z);
    CHECK_EQ_UINT(p.cfg.duty_max_permille, KILN_DUTY_MAX);
}

KILN_TEST(frctl06_bumpless_transfer_continues_from_the_present_output)
{
    kiln_pid_t p = make(1.0f, 0.1f, 0.0f);

    /* 40 % duty with 10 degC of error: P is 10 %, so I must be 30 %. */
    CHECK(kiln_pid_bumpless(&p, 400, 110.0f, 100.0f));
    CHECK_NEAR(p.integral_pct, 30.0f, 0.001f);
    CHECK_NEAR((float)kiln_pid_update(&p, 110.0f, 100.0f, 0.0001f), 400.0f, 1.0f);
}

KILN_TEST(frctl06_bumpless_reports_a_transfer_it_cannot_make_bumplessly)
{
    /* The integral the present output implies is negative, and an integral below
     * zero is a wind-up the anti-windup logic exists to prevent -- so it is
     * clamped and the next cycle steps.  Saying so is the whole fix. */
    kiln_pid_t p = make(10.0f, 0.1f, 0.0f);
    CHECK(!kiln_pid_bumpless(&p, 0, 200.0f, 100.0f));
    CHECK_NEAR(p.integral_pct, 0.0f, 0.001f);

    /* And above the ceiling, likewise. */
    kiln_pid_set_duty_max(&p, 500);
    CHECK(!kiln_pid_bumpless(&p, 500, 100.0f, 100.0f + 100.0f));
}

KILN_TEST(nfr17_a_non_finite_input_produces_no_heat_and_no_persistent_damage)
{
    kiln_pid_t p = make(2.0f, 0.1f, 5.0f);

    for (int i = 0; i < 10; i++) (void)kiln_pid_update(&p, 110.0f, 100.0f, 1.0f);
    const float integral = p.integral_pct;
    CHECK(integral > 0.0f);

    const float nan = 0.0f / 0.0f;
    CHECK_EQ_UINT(kiln_pid_update(&p, nan, 100.0f, 1.0f), 0u);
    CHECK_EQ_UINT(kiln_pid_update(&p, 110.0f, nan, 1.0f), 0u);
    CHECK_EQ_UINT(kiln_pid_update(&p, 110.0f, 100.0f, nan), 0u);
    CHECK_EQ_UINT(kiln_pid_update(&p, 110.0f, 100.0f, 0.0f), 0u);
    CHECK_EQ_UINT(kiln_pid_update(&p, 110.0f, 100.0f, -1.0f), 0u);

    /* The integral survived untouched, and the counter says five calls were
     * wrong -- fail-safe, and visible. */
    CHECK_NEAR(p.integral_pct, integral, 0.001f);
    CHECK_EQ_UINT(p.bad_calls, 5u);
    CHECK(kiln_is_finite(p.integral_pct));

    /* A legitimate call still works afterwards. */
    CHECK(kiln_pid_update(&p, 110.0f, 100.0f, 1.0f) > 0u);
}

KILN_TEST(gains_are_clamped_to_their_documented_ranges)
{
    kiln_pid_t p = make(0.0f, 0.0f, 0.0f);
    kiln_pid_set_gains(&p, 1e6f, 1e6f, 1e6f);
    CHECK_NEAR(p.cfg.kp, KILN_PID_KP_MAX, 0.001f);
    CHECK_NEAR(p.cfg.ki, KILN_PID_KI_MAX, 0.001f);
    CHECK_NEAR(p.cfg.kd, KILN_PID_KD_MAX, 0.001f);

    kiln_pid_set_gains(&p, -1.0f, -1.0f, -1.0f);
    CHECK_NEAR(p.cfg.kp, 0.0f, 0.001f);
}

KILN_TEST(reset_forgets_history_so_the_next_cycle_has_no_derivative_kick)
{
    kiln_pid_t p = make(1.0f, 0.1f, 100.0f);
    for (int i = 0; i < 10; i++) (void)kiln_pid_update(&p, 110.0f, 100.0f, 1.0f);

    kiln_pid_reset(&p);
    CHECK_NEAR(p.integral_pct, 0.0f, 0.001f);
    CHECK(!p.primed);
    (void)kiln_pid_update(&p, 110.0f, 50.0f, 1.0f);
    CHECK_NEAR(p.d_pct, 0.0f, 0.001f);
}

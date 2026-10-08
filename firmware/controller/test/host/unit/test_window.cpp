/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 * kiln_core/window -- SWR-CTL-07, SWR-CTL-08, SWR-SAF-16, and the SWR-CUR-04 interaction.
 */

#include "kiln_check.h"
#include "kiln_core/window.h"

static kiln_window_cfg_t base(void)
{
    kiln_window_cfg_t c;
    kiln_window_cfg_defaults(&c);
    c.preserve_off_window = false;     /* the plain SWR-CTL-08 behaviour */
    return c;
}

/* Fraction of a window the pin spends on, measured over the *second* window: the
 * minimum off-time is honoured from initialisation, so the first turn-on of a
 * freshly initialised window is held back by min_off_ms and the first window is
 * not representative. */
static float on_fraction(kiln_window_t *w, uint16_t duty)
{
    for (uint32_t t = 0; t < w->ticks_per_window; t++) {
        (void)kiln_window_tick(w, duty, true);
    }
    uint32_t on = 0;
    for (uint32_t t = 0; t < w->ticks_per_window; t++) {
        if (kiln_window_tick(w, duty, true)) {
            on++;
        }
    }
    return (float)on / (float)w->ticks_per_window;
}

KILN_TEST(swrctl07_realises_the_requested_duty_over_a_window)
{
    kiln_window_cfg_t c = base();
    c.min_on_ms = c.min_off_ms = 0;
    kiln_window_t w;
    CHECK_OK(kiln_window_init(&w, &c));
    CHECK_EQ_UINT(w.ticks_per_window, 200u);     /* 2000 ms / 10 ms */

    const uint16_t duties[] = { 0, 100, 250, 500, 750, 1000 };
    for (size_t i = 0; i < sizeof(duties) / sizeof(duties[0]); i++) {
        kiln_window_t fresh;
        CHECK_OK(kiln_window_init(&fresh, &c));
        CHECK_NEAR(on_fraction(&fresh, duties[i]),
                   (float)duties[i] / (float)KILN_DUTY_MAX, 0.01f);
    }
}

KILN_TEST(swrctl07_rejects_a_window_that_is_not_a_whole_number_of_ticks)
{
    /* ticks_per_window is an integer division, so 2505 ms at a 10 ms tick used to
     * become 2500 ms silently and every duty computed against it was 0.2 % out. */
    kiln_window_cfg_t c = base();
    c.window_ms = 2505;

    kiln_window_t w;
    CHECK_ERR(kiln_window_init(&w, &c), KILN_ERR_RANGE);
    CHECK_EQ_UINT(w.cfg.window_ms, 2500u);
    CHECK_EQ_UINT(w.ticks_per_window, 250u);

    c.window_ms = 2500;
    CHECK_OK(kiln_window_init(&w, &c));
}

KILN_TEST(swrctl07_bounds_the_window_period)
{
    kiln_window_cfg_t c = base();
    kiln_window_t w;

    c.window_ms = 100;
    CHECK_ERR(kiln_window_init(&w, &c), KILN_ERR_RANGE);
    CHECK_EQ_UINT(w.cfg.window_ms, KILN_WINDOW_MS_MIN);

    c.window_ms = 60000;
    CHECK_ERR(kiln_window_init(&w, &c), KILN_ERR_RANGE);
    CHECK_EQ_UINT(w.cfg.window_ms, KILN_WINDOW_MS_MAX);

    CHECK_ERR(kiln_window_init(NULL, &c), KILN_ERR_INVALID_ARG);
    CHECK_ERR(kiln_window_init(&w, NULL), KILN_ERR_INVALID_ARG);
}

KILN_TEST(swrctl08_quantises_away_an_unrealisable_on_time)
{
    kiln_window_cfg_t c = base();
    c.min_on_ms  = 100;
    c.min_off_ms = 100;
    kiln_window_t w;
    CHECK_OK(kiln_window_init(&w, &c));

    /* 2 % of 2000 ms is 40 ms, under the 100 ms minimum: fully off. */
    CHECK_EQ_UINT(kiln_window_quantise(&w, 20), 0u);
    /* 98 % leaves 40 ms off: fully on. */
    CHECK_EQ_UINT(kiln_window_quantise(&w, 980), KILN_DUTY_MAX);
    /* And the middle is left alone. */
    CHECK_EQ_UINT(kiln_window_quantise(&w, 500), 500u);
}

KILN_TEST(swrcur04_a_measurable_off_interval_survives_quantisation)
{
    /* The interaction worth writing down: promoting a near-full duty to 100 %
     * deletes the commanded-off interval that SWR-SAF-25 needs to see a stuck relay,
     * and SWR-SAF-25 is the *primary* relay detection per requirements section 5.2. */
    kiln_window_cfg_t c = base();
    c.preserve_off_window = true;
    c.min_measure_off_ms  = 100;
    kiln_window_t w;
    CHECK_OK(kiln_window_init(&w, &c));

    const uint16_t q = kiln_window_quantise(&w, KILN_DUTY_MAX);
    CHECK(q < KILN_DUTY_MAX);
    CHECK_EQ_UINT(q, 950u);                      /* 1900 ms on, 100 ms off */

    kiln_window_t fresh;
    CHECK_OK(kiln_window_init(&fresh, &c));
    const float on = on_fraction(&fresh, KILN_DUTY_MAX);
    CHECK(on < 1.0f);
    /* The cost is a twentieth of the available power, which is the trade. */
    CHECK_NEAR(on, 0.95f, 0.01f);
}

KILN_TEST(the_reserved_off_interval_must_be_realisable)
{
    kiln_window_cfg_t c = base();
    c.preserve_off_window = true;
    c.min_off_ms          = 200;
    c.min_measure_off_ms  = 50;      /* shorter than min_off_ms: contradictory */

    kiln_window_t w;
    CHECK_ERR(kiln_window_init(&w, &c), KILN_ERR_RANGE);
    CHECK_EQ_UINT(w.cfg.min_measure_off_ms, 200u);
}

KILN_TEST(swrsaf16_withdrawing_authority_takes_effect_without_waiting_for_an_edge)
{
    kiln_window_cfg_t c = base();
    c.min_on_ms  = 1000;            /* a long minimum on-time, deliberately  */
    c.min_off_ms = 0;               /* ...so the first turn-on is immediate  */
    kiln_window_t w;
    CHECK_OK(kiln_window_init(&w, &c));

    CHECK(kiln_window_tick(&w, 1000, true));
    /* Authority outranks every minimum time. */
    CHECK(!kiln_window_tick(&w, 1000, false));
    CHECK(!w.on);
}

KILN_TEST(swrctl08_minimum_dwell_stops_the_ssr_chattering)
{
    kiln_window_cfg_t c = base();
    c.min_on_ms = c.min_off_ms = 150;
    kiln_window_t w;
    CHECK_OK(kiln_window_init(&w, &c));

    /* Count level changes over several windows at a duty that would otherwise
     * switch every few ticks. */
    bool prev = false;
    uint32_t changes = 0;
    for (uint32_t t = 0; t < w.ticks_per_window * 5u; t++) {
        const bool on = kiln_window_tick(&w, 500, true);
        if (on != prev) {
            changes++;
        }
        prev = on;
    }
    CHECK(changes <= 11u);
}

KILN_TEST(swrcur05_level_remaining_tells_the_sampler_what_it_needs)
{
    kiln_window_cfg_t c = base();
    c.min_on_ms = c.min_off_ms = 0;
    kiln_window_t w;
    CHECK_OK(kiln_window_init(&w, &c));

    /* At 50 % duty the first half of the window is on. */
    CHECK_EQ_UINT(kiln_window_level_remaining_ms(&w, 500), 1000u);
    for (int i = 0; i < 50; i++) {
        (void)kiln_window_tick(&w, 500, true);
    }
    CHECK_EQ_UINT(kiln_window_level_remaining_ms(&w, 500), 500u);
    for (int i = 0; i < 50; i++) {
        (void)kiln_window_tick(&w, 500, true);
    }
    /* Now off, with the rest of the window to run. */
    CHECK_EQ_UINT(kiln_window_level_remaining_ms(&w, 500), 1000u);
}

KILN_TEST(swrcur13_on_transitions_are_counted)
{
    kiln_window_cfg_t c = base();
    c.min_on_ms = c.min_off_ms = 0;
    kiln_window_t w;
    CHECK_OK(kiln_window_init(&w, &c));

    for (uint32_t t = 0; t < w.ticks_per_window * 10u; t++) {
        (void)kiln_window_tick(&w, 500, true);
    }
    /* One switch-on per window. */
    CHECK_EQ_UINT(w.switch_count, 10u);
}

/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 * kiln_core/tempfilt -- FR-ACQ-07, FR-ACQ-08, FR-ACQ-11, NFR-01, NFR-17.
 */

#include "kiln_check.h"
#include "kiln_core/tempfilt.h"

static kiln_tempfilt_t make(float tau_s, uint16_t window_s)
{
    const kiln_tempfilt_cfg_t c = { .offset_c = 0.0f, .gain = 1.0f,
                                    .filter_tau_s = tau_s,
                                    .rate_window_s = window_s };
    kiln_tempfilt_t f;
    kiln_tempfilt_init(&f, &c);
    return f;
}

KILN_TEST(fracq08_applies_gain_then_offset)
{
    const kiln_tempfilt_cfg_t c = { .offset_c = 5.0f, .gain = 1.02f,
                                    .filter_tau_s = 0.0f, .rate_window_s = 60 };
    kiln_tempfilt_t f;
    kiln_tempfilt_init(&f, &c);

    CHECK(kiln_tempfilt_push(&f, 100.0f, 0.25f));
    CHECK_NEAR(kiln_tempfilt_raw(&f), 107.0f, 0.001f);
}

KILN_TEST(fracq08_bounds_the_calibration_ranges)
{
    const kiln_tempfilt_cfg_t c = { .offset_c = 500.0f, .gain = 5.0f,
                                    .filter_tau_s = 1000.0f, .rate_window_s = 9999 };
    kiln_tempfilt_t f;
    kiln_tempfilt_init(&f, &c);

    CHECK_NEAR(f.cfg.offset_c, 50.0f, 0.001f);
    CHECK_NEAR(f.cfg.gain, 1.10f, 0.001f);
    CHECK_NEAR(f.cfg.filter_tau_s, 30.0f, 0.001f);
    CHECK_EQ_UINT(f.cfg.rate_window_s, KILN_RATE_WINDOW_MAX_S);
}

KILN_TEST(fracq07_first_order_filter_reaches_63_percent_in_one_tau)
{
    kiln_tempfilt_t f = make(10.0f, 60);

    CHECK(kiln_tempfilt_push(&f, 0.0f, 0.25f));     /* primes at 0 */
    for (int i = 0; i < 40; i++) {                  /* 10 s at 0.25 s */
        CHECK(kiln_tempfilt_push(&f, 100.0f, 0.25f));
    }
    CHECK_NEAR(kiln_tempfilt_filt(&f), 63.2f, 2.5f);
    CHECK_NEAR(kiln_tempfilt_raw(&f), 100.0f, 0.001f);
}

KILN_TEST(fracq11_regresses_a_steady_rate)
{
    kiln_tempfilt_t f = make(0.0f, 60);

    /* 360 degC/h is 0.1 degC/s. */
    float t = 20.0f;
    for (int i = 0; i < 400; i++) {                 /* 100 s at 0.25 s */
        CHECK(kiln_tempfilt_push(&f, t, 0.25f));
        t += 0.025f;
    }
    CHECK_NEAR(kiln_tempfilt_rate(&f), 360.0f, 5.0f);
}

KILN_TEST(fracq11_reports_no_rate_until_there_is_evidence)
{
    kiln_tempfilt_t f = make(0.0f, 60);
    CHECK(kiln_tempfilt_push(&f, 20.0f, 0.25f));
    CHECK_NEAR(kiln_tempfilt_rate(&f), 0.0f, 0.001f);
    CHECK(kiln_tempfilt_push(&f, 21.0f, 0.25f));
    CHECK_NEAR(kiln_tempfilt_rate(&f), 0.0f, 0.001f);
}

KILN_TEST(nfr01_the_regression_runs_only_when_the_history_changes)
{
    /* At 4 Hz acquisition against 1 Hz decimation, three of every four pushes
     * used to repeat an identical 300-point walk for nothing.  The observable
     * consequence: the published rate changes only on a decimation boundary. */
    kiln_tempfilt_t f = make(0.0f, 60);

    float t = 20.0f;
    for (int i = 0; i < 40; i++) { (void)kiln_tempfilt_push(&f, t, 0.25f); t += 0.1f; }

    const uint16_t count_before = f.hist_count;
    const float    rate_before  = kiln_tempfilt_rate(&f);

    /* Three sub-second pushes: no new history, so no new rate. */
    for (int i = 0; i < 3; i++) { (void)kiln_tempfilt_push(&f, t, 0.25f); t += 0.1f; }
    CHECK_EQ_UINT(f.hist_count, count_before);
    CHECK_NEAR(kiln_tempfilt_rate(&f), rate_before, 0.0f);

    /* The fourth crosses the boundary. */
    (void)kiln_tempfilt_push(&f, t, 0.25f);
    CHECK_EQ_UINT(f.hist_count, count_before + 1u);
}

KILN_TEST(sr07_an_overrunning_cycle_does_not_flatten_the_regressed_rate)
{
    /* A cycle that overruns covers several decimation boundaries.  Pushing the
     * same value at each of them plants duplicate points at distinct x
     * positions, which flattens the slope -- and SR-07 reads a flattened slope
     * as "not rising", which is a heating-failure fault on a kiln that is
     * heating perfectly well. */
    kiln_tempfilt_t steady = make(0.0f, 60);
    kiln_tempfilt_t bumpy  = make(0.0f, 60);

    float t = 20.0f;
    for (int i = 0; i < 100; i++) {          /* 100 s of 1 degC/s = 3600 degC/h */
        (void)kiln_tempfilt_push(&steady, t, 1.0f);
        t += 1.0f;
    }
    CHECK_NEAR(kiln_tempfilt_rate(&steady), 3600.0f, 20.0f);

    /* The same ramp, delivered in 5 s chunks because the task kept overrunning. */
    t = 20.0f;
    for (int i = 0; i < 20; i++) {
        t += 5.0f;
        (void)kiln_tempfilt_push(&bumpy, t, 5.0f);
    }
    CHECK_NEAR(kiln_tempfilt_rate(&bumpy), 3600.0f, 150.0f);
    CHECK(kiln_tempfilt_rate(&bumpy) > 3000.0f);
}

KILN_TEST(nfr17_a_non_finite_sample_is_refused_and_changes_nothing)
{
    kiln_tempfilt_t f = make(5.0f, 60);

    for (int i = 0; i < 20; i++) CHECK(kiln_tempfilt_push(&f, 500.0f, 0.25f));
    const float filt = kiln_tempfilt_filt(&f);
    const uint16_t hist = f.hist_count;

    /* filt_c is persistent state: one NaN admitted here would poison the filter,
     * the rate regression and every rule downstream for the rest of the run. */
    CHECK(!kiln_tempfilt_push(&f, 0.0f / 0.0f, 0.25f));
    CHECK(!kiln_tempfilt_push(&f, 1.0f / 0.0f, 0.25f));
    CHECK(!kiln_tempfilt_push(&f, 500.0f, 0.0f / 0.0f));

    CHECK_NEAR(kiln_tempfilt_filt(&f), filt, 0.0f);
    CHECK_EQ_UINT(f.hist_count, hist);
    CHECK_EQ_UINT(f.rejected, 3u);
    CHECK(kiln_is_finite(kiln_tempfilt_filt(&f)));

    CHECK(kiln_tempfilt_push(&f, 501.0f, 0.25f));
}

KILN_TEST(the_history_is_a_ring_that_keeps_the_most_recent_window)
{
    kiln_tempfilt_t f = make(0.0f, KILN_RATE_WINDOW_MAX_S);

    /* Flat for a long time, then a sharp rise: the rate must reflect the rise,
     * not the average of the whole run. */
    for (int i = 0; i < 400; i++) (void)kiln_tempfilt_push(&f, 100.0f, 1.0f);
    CHECK_NEAR(kiln_tempfilt_rate(&f), 0.0f, 1.0f);

    float t = 100.0f;
    for (int i = 0; i < 400; i++) { t += 0.5f; (void)kiln_tempfilt_push(&f, t, 1.0f); }
    CHECK_NEAR(kiln_tempfilt_rate(&f), 1800.0f, 20.0f);
    CHECK_EQ_UINT(f.hist_count, KILN_RATE_MAX_POINTS);
}

KILN_TEST(reset_clears_the_history_without_losing_the_calibration)
{
    kiln_tempfilt_t f = make(5.0f, 60);
    for (int i = 0; i < 100; i++) (void)kiln_tempfilt_push(&f, 500.0f, 1.0f);

    kiln_tempfilt_reset(&f);
    CHECK_EQ_UINT(f.hist_count, 0u);
    CHECK_NEAR(kiln_tempfilt_rate(&f), 0.0f, 0.001f);
    CHECK(!f.primed);
    CHECK_NEAR(f.cfg.filter_tau_s, 5.0f, 0.001f);
}

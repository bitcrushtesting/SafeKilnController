/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 * kiln_core/setpoint -- FR-CTL-09..FR-CTL-13, FR-PRG-03, FR-PRG-06, FR-PRG-10.
 */

#include "kiln_check.h"
#include "kiln_core/setpoint.h"
#include "kiln_core/profile.h"

static kiln_setpoint_cfg_t cfg(float holdback, float max_temp)
{
    const kiln_setpoint_cfg_t c = { .holdback_band_c = holdback,
                                    .dwell_tol_c = 5.0f,
                                    .max_temp_c = max_temp };
    return c;
}

static kiln_program_t prog_of(uint8_t n, const kiln_segment_t *segs)
{
    kiln_program_t p;
    kiln_profile_init_empty(&p, "test");
    p.segment_count = n;
    for (uint8_t i = 0; i < n; i++) {
        p.segments[i] = segs[i];
    }
    return p;
}

/* --- FR-CTL-09, FR-CTL-10 ---------------------------------------------- */

KILN_TEST(frctl09_the_setpoint_ramps_rather_than_stepping)
{
    const kiln_segment_t segs[] = { { 600, 360, 0, 0, 0 } };   /* 360 degC/h */
    const kiln_program_t p = prog_of(1, segs);

    kiln_setpoint_t st;
    const kiln_setpoint_cfg_t c = cfg(0.0f, 1280.0f);
    CHECK_OK(kiln_setpoint_start(&st, &c, &p, 20.0f));
    CHECK_NEAR(kiln_setpoint_value(&st), 20.0f, 0.01f);

    /* 0.1 degC/s. */
    for (int i = 0; i < 100; i++) {
        CHECK_OK(kiln_setpoint_tick(&st, 20.0f, 1.0f));
    }
    CHECK_NEAR(kiln_setpoint_value(&st), 30.0f, 0.2f);
}

KILN_TEST(frctl10_rate_zero_means_as_fast_as_the_kiln_allows)
{
    const kiln_segment_t segs[] = { { 600, 0, 0, 0, 0 } };
    const kiln_program_t p = prog_of(1, segs);

    kiln_setpoint_t st;
    const kiln_setpoint_cfg_t c = cfg(0.0f, 1280.0f);
    CHECK_OK(kiln_setpoint_start(&st, &c, &p, 20.0f));
    CHECK_OK(kiln_setpoint_tick(&st, 20.0f, 1.0f));
    CHECK_NEAR(kiln_setpoint_value(&st), 600.0f, 0.01f);
}

/* --- FR-CTL-11, FR-CTL-12 ---------------------------------------------- */

KILN_TEST(frctl11_holdback_freezes_the_curve_while_the_kiln_lags)
{
    const kiln_segment_t segs[] = { { 600, 360, 0, 0, 0 } };
    const kiln_program_t p = prog_of(1, segs);

    kiln_setpoint_t st;
    const kiln_setpoint_cfg_t c = cfg(20.0f, 1280.0f);
    CHECK_OK(kiln_setpoint_start(&st, &c, &p, 20.0f));

    /* 360 degC/h is 0.1 degC/s, so the setpoint needs a little over 200 s to get
     * a 20 degC band ahead of a kiln that is not moving. */
    for (int i = 0; i < 300; i++) {
        CHECK_OK(kiln_setpoint_tick(&st, 20.0f, 1.0f));
    }
    CHECK(kiln_setpoint_holdback(&st));
    CHECK_NEAR(kiln_setpoint_value(&st), 40.0f, 1.5f);

    /* The kiln catches up and the curve resumes. */
    for (int i = 0; i < 10; i++) {
        CHECK_OK(kiln_setpoint_tick(&st, 40.0f, 1.0f));
    }
    CHECK(!kiln_setpoint_holdback(&st));
}

KILN_TEST(frctl12_dwell_accrues_only_while_the_kiln_is_within_tolerance)
{
    const kiln_segment_t segs[] = { { 100, 0, 1, 0, 0 } };   /* 1 minute dwell */
    const kiln_program_t p = prog_of(1, segs);

    kiln_setpoint_t st;
    const kiln_setpoint_cfg_t c = cfg(0.0f, 1280.0f);
    CHECK_OK(kiln_setpoint_start(&st, &c, &p, 100.0f));
    CHECK_OK(kiln_setpoint_tick(&st, 100.0f, 1.0f));
    CHECK_EQ_INT(st.phase, KILN_SP_PHASE_DWELL);

    /* 50 degC adrift: the dwell timer does not move. */
    for (int i = 0; i < 120; i++) {
        CHECK_OK(kiln_setpoint_tick(&st, 50.0f, 1.0f));
    }
    CHECK(!kiln_setpoint_finished(&st));
    CHECK_NEAR(st.seg_elapsed_s, 0.0f, 0.001f);

    for (int i = 0; i < 61 && !kiln_setpoint_finished(&st); i++) {
        CHECK_OK(kiln_setpoint_tick(&st, 100.0f, 1.0f));
    }
    CHECK(kiln_setpoint_finished(&st));
}

/* --- FR-CTL-13: the clamping consistency that B9 fixed ----------------- */

KILN_TEST(frctl13_a_cooling_ramp_is_passive)
{
    const kiln_segment_t segs[] = { { 400, 100, 0, 0, 0 } };
    const kiln_program_t p = prog_of(1, segs);

    kiln_setpoint_t st;
    const kiln_setpoint_cfg_t c = cfg(0.0f, 1280.0f);
    CHECK_OK(kiln_setpoint_start(&st, &c, &p, 900.0f));
    CHECK(!kiln_setpoint_heat_allowed(&st));
}

KILN_TEST(frctl13_heat_allowed_uses_the_same_clamped_target_as_the_executor)
{
    /* The executor ramps toward clamp(target, 0, max); heat_allowed used to
     * compare the *unclamped* target against the segment start, so the two could
     * disagree about which direction the ramp was going -- and a cooling ramp
     * that heat_allowed called a heating one is FR-CTL-13's passive cooling not
     * happening, with the PID driving full duty into a descending setpoint.
     *
     * With the start value also clamped to the maximum, the divergence is not
     * currently reachable from kiln_setpoint_start, so this is a regression guard
     * on the invariant rather than a reproduction of a live bug: whatever the
     * program says, the two must agree. */
    const kiln_segment_t segs[] = {
        { 1300, 100, 0, 0, 0 },     /* above the configured maximum */
        {  300, 100, 0, 0, 0 },     /* a genuine cooling ramp       */
        { 1300,   0, 0, 0, 0 },
    };
    const kiln_program_t p = prog_of(3, segs);

    kiln_setpoint_t st;
    const kiln_setpoint_cfg_t c = cfg(0.0f, 700.0f);
    CHECK_OK(kiln_setpoint_start(&st, &c, &p, 900.0f));

    /* The start is clamped too, so the generator never begins above the limit. */
    CHECK_NEAR(kiln_setpoint_value(&st), 700.0f, 0.01f);

    bool saw_cooling = false;
    for (int i = 0; i < 100000 && !kiln_setpoint_finished(&st); i++) {
        const uint8_t seg = kiln_setpoint_segment(&st);
        const float   target = kiln_clampf((float)p.segments[seg].target_c,
                                           0.0f, st.cfg.max_temp_c);
        if (st.phase == KILN_SP_PHASE_RAMP) {
            const bool cooling = target < st.seg_start_c;
            if (cooling) {
                saw_cooling = true;
            }
            CHECK_MSG(kiln_setpoint_heat_allowed(&st) == !cooling,
                      "segment %u: heat_allowed disagreed with the ramp direction", seg);
        }
        (void)kiln_setpoint_tick(&st, kiln_setpoint_value(&st), 1.0f);
    }
    CHECK(saw_cooling);
}

KILN_TEST(sr23_the_configured_maximum_and_the_ceiling_both_bind)
{
    const kiln_segment_t segs[] = { { 1340, 0, 0, 0, 0 } };
    const kiln_program_t p = prog_of(1, segs);

    kiln_setpoint_t st;
    const kiln_setpoint_cfg_t c = cfg(0.0f, 1e9f);
    CHECK_OK(kiln_setpoint_start(&st, &c, &p, 20.0f));
    CHECK_NEAR(st.cfg.max_temp_c, KILN_TEMP_CEILING_C, 0.01f);

    CHECK_OK(kiln_setpoint_tick(&st, 20.0f, 1.0f));
    CHECK(kiln_setpoint_value(&st) <= KILN_TEMP_CEILING_C);
}

/* --- FR-PRG-03 --------------------------------------------------------- */

KILN_TEST(frprg03_a_segment_can_wait_for_an_operator)
{
    const kiln_segment_t segs[] = {
        { 100, 0, 0, KILN_SEG_FLAG_REQUIRE_ACK, 0 },
        { 200, 0, 0, 0, 0 },
    };
    const kiln_program_t p = prog_of(2, segs);

    kiln_setpoint_t st;
    const kiln_setpoint_cfg_t c = cfg(0.0f, 1280.0f);
    CHECK_OK(kiln_setpoint_start(&st, &c, &p, 100.0f));

    for (int i = 0; i < 10; i++) {
        (void)kiln_setpoint_tick(&st, 100.0f, 1.0f);
    }
    CHECK(kiln_setpoint_awaiting_ack(&st));
    CHECK_EQ_UINT(kiln_setpoint_segment(&st), 0u);

    /* Frozen: ticking does nothing at all. */
    CHECK_ERR(kiln_setpoint_tick(&st, 100.0f, 1.0f), KILN_ERR_STATE);
    CHECK(kiln_setpoint_awaiting_ack(&st));

    CHECK_OK(kiln_setpoint_ack(&st));
    CHECK_EQ_UINT(kiln_setpoint_segment(&st), 1u);
    CHECK_ERR(kiln_setpoint_ack(&st), KILN_ERR_STATE);
}

/* --- FR-PRG-06 / FR-RUN-05: the closed-form prediction of B6 ------------ */

/* The oracle: the forward simulation the closed form replaced.  Kept as a test
 * fixture rather than in the firmware, so the two cannot drift apart while
 * NFR-02's 50 ms ceiling is not being spent on it in production. */
static uint32_t predict_by_simulation(const kiln_setpoint_t *st, bool stop_at_segment_end)
{
    if (!st->started || st->finished) {
        return 0;
    }

    kiln_setpoint_t sim = *st;
    sim.holdback_active     = false;
    sim.cfg.holdback_band_c = 0.0f;      /* perfect tracking cannot lag */

    const uint8_t start_seg = sim.seg;
    uint32_t steps = 0;

    while (!sim.finished && steps < 200u * 3600u) {
        if (sim.phase == KILN_SP_PHASE_AWAIT_ACK) {
            break;
        }
        (void)kiln_setpoint_tick(&sim, sim.sp_c, 1.0f);
        steps++;
        if (stop_at_segment_end && sim.seg != start_seg) {
            break;
        }
    }
    return steps;
}

static void check_prediction_matches_oracle(const kiln_program_t *p, float start_c,
                                            float max_temp_c)
{
    kiln_setpoint_t st;
    const kiln_setpoint_cfg_t c = cfg(0.0f, max_temp_c);
    CHECK_OK(kiln_setpoint_start(&st, &c, p, start_c));

    /* The closed form is the *more* accurate of the two: the oracle adds
     * rate/3600 to a float some tens of thousands of times, and the accumulated
     * rounding makes it reach the target a few steps late.  Agreement is
     * therefore to a small relative figure plus a tick per segment, not to the
     * second -- and where they differ it is the simulation that has drifted. */
    const double oracle_total = (double)predict_by_simulation(&st, false);
    const double tol = 0.002 * oracle_total + (double)p->segment_count + 1.0;

    CHECK_NEAR((double)kiln_setpoint_remaining_s(&st), oracle_total, tol);

    const double oracle_seg = (double)predict_by_simulation(&st, true);
    CHECK_NEAR((double)kiln_setpoint_segment_remaining_s(&st), oracle_seg,
               0.002 * oracle_seg + 2.0);

    /* And again partway through, so a mid-ramp and a mid-dwell state are both
     * covered rather than just the start. */
    for (int i = 0; i < 500 && !kiln_setpoint_finished(&st); i++) {
        (void)kiln_setpoint_tick(&st, kiln_setpoint_value(&st), 1.0f);
        if (i % 97 == 0) {
            const double oracle = (double)predict_by_simulation(&st, false);
            CHECK_NEAR((double)kiln_setpoint_remaining_s(&st), oracle,
                       0.002 * oracle + (double)p->segment_count + 1.0);
        }
    }
}

KILN_TEST(frprg06_closed_form_prediction_agrees_with_the_simulation_oracle)
{
    const kiln_segment_t a[] = { { 600, 100, 30, 0, 0 } };
    const kiln_program_t pa = prog_of(1, a);
    check_prediction_matches_oracle(&pa, 20.0f, 1280.0f);

    const kiln_segment_t b[] = {
        { 100,  60, 30, 0, 0 },
        { 600, 100,  0, 0, 0 },
        { 900, 150,  0, 0, 0 },
        { 999,  80, 20, 0, 0 },
        {  40,  90,  0, 0, 0 },
    };
    const kiln_program_t pb = prog_of(5, b);
    check_prediction_matches_oracle(&pb, 20.0f, 1280.0f);

    /* Rate 0 segments, a zero dwell, and a target above the configured maximum. */
    const kiln_segment_t c2[] = {
        { 540, 220, 20, 0, 0 },
        { 804,   0, 10, 0, 0 },
        { 516, 999, 60, 0, 0 },
        { 900,   0,  0, 0, 0 },
    };
    const kiln_program_t pc = prog_of(4, c2);
    check_prediction_matches_oracle(&pc, 20.0f, 700.0f);
}

KILN_TEST(frprg06_prediction_stops_at_a_segment_that_waits_on_a_human)
{
    const kiln_segment_t segs[] = {
        { 100, 0, 1, 0, 0 },
        { 200, 0, 1, KILN_SEG_FLAG_REQUIRE_ACK, 0 },
        { 300, 0, 60, 0, 0 },      /* an hour, which must not be counted */
    };
    const kiln_program_t p = prog_of(3, segs);

    kiln_setpoint_t st;
    const kiln_setpoint_cfg_t c = cfg(0.0f, 1280.0f);
    CHECK_OK(kiln_setpoint_start(&st, &c, &p, 100.0f));

    const uint32_t remaining = kiln_setpoint_remaining_s(&st);
    CHECK(remaining < 200u);
    CHECK_EQ_UINT(remaining, predict_by_simulation(&st, false));
}

KILN_TEST(frprg06_prediction_is_zero_once_there_is_nothing_to_predict)
{
    kiln_setpoint_t st = {};
    CHECK_EQ_UINT(kiln_setpoint_remaining_s(&st), 0u);
    CHECK_EQ_UINT(kiln_setpoint_segment_remaining_s(&st), 0u);

    const kiln_segment_t segs[] = { { 100, 0, 0, 0, 0 } };
    const kiln_program_t p = prog_of(1, segs);
    const kiln_setpoint_cfg_t c = cfg(0.0f, 1280.0f);
    CHECK_OK(kiln_setpoint_start(&st, &c, &p, 100.0f));
    while (!kiln_setpoint_finished(&st)) {
        (void)kiln_setpoint_tick(&st, 100.0f, 1.0f);
    }
    CHECK_EQ_UINT(kiln_setpoint_remaining_s(&st), 0u);
}

/* --- FR-PRG-10 --------------------------------------------------------- */

KILN_TEST(frprg10_only_segments_that_have_not_started_may_be_replaced)
{
    const kiln_segment_t segs[] = {
        { 100, 0, 0, 0, 0 },
        { 200, 0, 0, 0, 0 },
        { 300, 0, 0, 0, 0 },
    };
    kiln_program_t p = prog_of(3, segs);
    p.schema_version = KILN_PROGRAM_SCHEMA_VERSION;

    kiln_setpoint_t st;
    const kiln_setpoint_cfg_t c = cfg(0.0f, 1280.0f);
    CHECK_OK(kiln_setpoint_start(&st, &c, &p, 100.0f));
    (void)kiln_setpoint_tick(&st, 100.0f, 1.0f);
    (void)kiln_setpoint_tick(&st, 100.0f, 1.0f);
    CHECK_EQ_UINT(kiln_setpoint_segment(&st), 1u);

    /* Editing the tail is allowed. */
    kiln_program_t edit = p;
    edit.segments[2].target_c = 350;
    CHECK_OK(kiln_setpoint_replace_remaining(&st, &edit));
    CHECK_EQ_UINT(st.prog.segments[2].target_c, 350u);

    /* Editing the running segment is not. */
    kiln_program_t bad = p;
    bad.segments[1].target_c = 250;
    CHECK_ERR(kiln_setpoint_replace_remaining(&st, &bad), KILN_ERR_STATE);

    /* Nor is truncating the program to before where it already is. */
    kiln_program_t shorter = p;
    shorter.segment_count = 1;
    CHECK_ERR(kiln_setpoint_replace_remaining(&st, &shorter), KILN_ERR_STATE);
}

/* --- NFR-17 ------------------------------------------------------------ */

KILN_TEST(nfr17_tick_reports_what_it_refused_to_do)
{
    const kiln_segment_t segs[] = { { 100, 0, 0, 0, 0 } };
    const kiln_program_t p = prog_of(1, segs);

    kiln_setpoint_t st;
    const kiln_setpoint_cfg_t c = cfg(0.0f, 1280.0f);

    /* Not started: a stalled clock used to look exactly like a running program. */
    kiln_setpoint_t fresh = {};
    CHECK_ERR(kiln_setpoint_tick(&fresh, 100.0f, 1.0f), KILN_ERR_STATE);

    CHECK_OK(kiln_setpoint_start(&st, &c, &p, 100.0f));
    CHECK_ERR(kiln_setpoint_tick(&st, 100.0f, 0.0f), KILN_ERR_INVALID_ARG);
    CHECK_ERR(kiln_setpoint_tick(&st, 100.0f, -1.0f), KILN_ERR_INVALID_ARG);
    CHECK_ERR(kiln_setpoint_tick(&st, 0.0f / 0.0f, 1.0f), KILN_ERR_INVALID_ARG);
    CHECK_ERR(kiln_setpoint_tick(NULL, 100.0f, 1.0f), KILN_ERR_INVALID_ARG);
    CHECK_OK(kiln_setpoint_tick(&st, 100.0f, 1.0f));

    CHECK_ERR(kiln_setpoint_start(&st, &c, NULL, 20.0f), KILN_ERR_INVALID_ARG);
    kiln_program_t empty = p;
    empty.segment_count = 0;
    CHECK_ERR(kiln_setpoint_start(&st, &c, &empty, 20.0f), KILN_ERR_INVALID_ARG);
}

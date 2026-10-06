/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * HR-22 and FR-CUR-15: the phase strap, three current transformers, and the
 * power and energy that come out of them.
 *
 * The interesting case is not "three phases work".  It is that a fault on a
 * phase other than the monitored one is now *seen* -- which is exactly what
 * ASM-10 used to concede was invisible, and what OQ-06 was open about.
 */

#include <math.h>
#include "kiln_check.h"
#include "kiln_app/app.h"
#include "kiln_core/faults.h"
#include "kiln_core/profile.h"
#include "kiln_sim/sim.h"

typedef struct {
    kiln_sim_t       sim;
    kiln_sim_ports_t sp;
    kiln_app_t       app;
    double           t_s, na, ns, nc;
} rig_t;

static void rig_init(rig_t *r, uint8_t phases)
{
    memset(r, 0, sizeof(*r));

    kiln_sim_cfg_t sc;
    kiln_sim_cfg_defaults(&sc);
    sc.tau_s       = 300.0f;
    sc.dead_time_s = 10.0f;
    sc.noise_c     = 0.1f;
    kiln_sim_init(&r->sim, &sc);
    kiln_sim_set_phases(&r->sim, phases);
    kiln_sim_bind(&r->sim, &r->sp);

    kiln_app_ports_t ports = {};
    ports.tc       = &r->sp.tc;
    ports.case_tc  = &r->sp.case_tc;
    ports.heat     = &r->sp.heat;
    ports.current  = &r->sp.current;
    ports.counters = &r->sp.counters;
    ports.phase    = &r->sp.phase;

    kiln_config_t cfg;
    kiln_config_defaults(&cfg);
    cfg.kp = 6.0f; cfg.ki = 0.02f; cfg.kd = 30.0f;
    cfg.filter_tau_s = 1.0f;
    cfg.holdback_band_c = 0.0f;
    CHECK_OK(kiln_app_init(&r->app, &ports, &cfg));
}

static void rig_run(rig_t *r, double seconds)
{
    const int n = (int)(seconds / 0.01);
    for (int i = 0; i < n; i++) {
        kiln_sim_step(&r->sim, 0.01f);
        r->t_s += 0.01;
        kiln_app_window_tick(&r->app, 10);
        if (r->t_s >= r->na) { kiln_app_acquire_cycle(&r->app, 0.25f); r->na += 0.25; }
        if (r->t_s >= r->ns) { kiln_app_safety_cycle(&r->app, 0.1f);   r->ns += 0.1;  }
        if (r->t_s >= r->nc) { kiln_app_control_cycle(&r->app, 1.0f);  r->nc += 1.0;  }
    }
}

static void start(rig_t *r)
{
    /* FR-CUR-12 refuses a start until current monitoring is actually
     * delivering, so let a measurement land first. */
    rig_run(r, 2.0);

    kiln_program_t p;
    kiln_profile_init_empty(&p, "phases");
    p.segment_count            = 1;
    p.segments[0].target_c     = 600;
    p.segments[0].rate_c_per_h = 3600;
    p.segments[0].dwell_min    = 10;
    CHECK_OK(kiln_app_start(&r->app, &p));
}

/* --- the strap --------------------------------------------------------- */

KILN_TEST(hr22_the_strap_selects_one_or_three_phases)
{
    static rig_t one, three;
    rig_init(&one, 1);
    rig_init(&three, 3);
    CHECK_EQ_UINT(kiln_app_phases(&one.app), 1u);
    CHECK_EQ_UINT(kiln_app_phases(&three.app), 3u);
    CHECK_EQ_UINT(one.app.cur_channels, 1u);
    CHECK_EQ_UINT(three.app.cur_channels, 3u);
}

KILN_TEST(hr22_an_unreadable_strap_reads_as_single_phase)
{
    /* port_phase.h: under-reporting power on a three-phase kiln is visibly
     * wrong and gets fixed; over-reporting on a single-phase kiln looks
     * plausible and does not. */
    static rig_t r;
    rig_init(&r, 1);
    r.app.ports.phase = NULL;
    CHECK_EQ_UINT(kiln_app_phases(&r.app), 1u);
}

/* --- power and energy -------------------------------------------------- */

KILN_TEST(frcur07_three_phases_report_about_three_times_the_power)
{
    static rig_t one, three;
    rig_init(&one, 1);
    rig_init(&three, 3);
    start(&one);
    start(&three);
    rig_run(&one, 60.0);
    rig_run(&three, 60.0);

    const double va1 = kiln_app_apparent_va(&one.app);
    const double va3 = kiln_app_apparent_va(&three.app);
    CHECK_MSG(va1 > 0.0, "single-phase power should be measured, got %.0f", va1);
    CHECK_MSG(va3 > 2.5 * va1,
              "three phases should sum to about 3x one: %.0f vs %.0f", va3, va1);
    CHECK_MSG(va3 < 3.5 * va1, "...but not more than three: %.0f vs %.0f", va3, va1);
}

KILN_TEST(frcur07_energy_accumulates_across_every_measured_phase)
{
    static rig_t r;
    rig_init(&r, 3);
    start(&r);
    rig_run(&r, 120.0);

    const double total = kiln_app_energy_wh(&r.app);
    CHECK_MSG(total > 0.0, "no energy accumulated");

    double sum = 0.0;
    for (uint8_t i = 0; i < r.app.cur_channels; i++) {
        sum += kiln_current_energy_wh(&r.app.cur[i]);
    }
    CHECK_NEAR(total, sum, 0.001);
}

/* --- what ASM-10 used to concede --------------------------------------- */

KILN_TEST(frcur15_an_element_lost_on_one_phase_is_seen_electrically)
{
    /* The whole reason for the extra transformers.  The simulator applies a
     * partial element loss to phase 0 only, as losing one group really does. */
    static rig_t r;
    rig_init(&r, 3);
    start(&r);
    rig_run(&r, 60.0);

    /* conduction_a, not current_a: the latter is whichever window landed last,
     * and a time-proportional output spends most of its life in the off one. */
    const float healthy0 = r.app.cur[0].conduction_a;
    const float healthy1 = r.app.cur[1].conduction_a;
    CHECK_MSG(healthy0 > 1.0f && healthy1 > 1.0f,
              "both phases should be conducting first: %.1f / %.1f", healthy0, healthy1);

    kiln_sim_inject(&r.sim, KILN_INJ_ELEMENT_PARTIAL);
    rig_run(&r, 60.0);

    const float faulted0 = r.app.cur[0].conduction_a;
    const float faulted1 = r.app.cur[1].conduction_a;
    CHECK_MSG(faulted0 < healthy0 * 0.95f,
              "phase 0 should have dropped: %.1f -> %.1f", healthy0, faulted0);
    CHECK_MSG(faulted1 > faulted0,
              "the healthy phase should still be higher: %.1f vs %.1f",
              faulted1, faulted0);
}

KILN_TEST(frcur15_a_three_phase_strap_with_one_ct_warns)
{
    static rig_t r;
    rig_init(&r, 3);
    /* The strap says three; pretend only one transformer is fitted. */
    r.app.cur_channels = 1u;
    rig_run(&r, 1.0);
    CHECK_MSG((r.app.warnings & KILN_WARN_BIT(KILN_WARN_PHASE_MISMATCH)) != 0u,
              "warning 114 should flag the mismatch");
    CHECK_STR_EQ(kiln_warn_label(KILN_WARN_PHASE_MISMATCH), "PHASE?");
}

KILN_TEST(frcur15_a_matched_single_phase_install_does_not_warn)
{
    static rig_t r;
    rig_init(&r, 1);
    rig_run(&r, 1.0);
    CHECK((r.app.warnings & KILN_WARN_BIT(KILN_WARN_PHASE_MISMATCH)) == 0u);
}

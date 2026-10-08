/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The real application against real storage logic and a simulated kiln --
 * SWR-LOG-01..SWR-LOG-04, SWR-LOG-14, SWR-CFG-05, SWR-SAF-17, and SWR-RUN-08's recovery
 * at an arbitrary instant.
 *
 * "Reboot" here means discarding every piece of RAM state and mounting the same
 * storage again, which is the only honest way to test a boot path: a test that
 * keeps the struct around is testing a reset, not a power loss.
 */

#include <string.h>
#include "kiln_check.h"
#include "kiln_app/app.h"
#include "kiln_app/program_store.h"
#include "kiln_app/run_index.h"
#include "kiln_app/settings.h"
#include "kiln_core/faults.h"
#include "kiln_core/logring.h"
#include "kiln_core/profile.h"
#include "kiln_hal_host/hal_host.h"
#include "kiln_sim/sim.h"

constexpr size_t LOG_SECTORS  = 32u;
constexpr size_t SECTOR_BYTES = 4096u;

/* The storage survives a reboot; everything else does not. */
typedef struct {
    uint8_t               flash_storage[LOG_SECTORS * SECTOR_BYTES];
    kiln_host_flash_t     flash;
    kiln_host_kv_t        kv;
    kiln_host_fs_t        fs;
    kiln_host_clock_t     clk;
} medium_t;

typedef struct {
    medium_t             *m;
    kiln_port_flash_t     flash_port;
    kiln_port_kvstore_t   kv_port;
    kiln_port_filestore_t fs_port;
    kiln_port_clock_t     clk_port;
    kiln_logring_t        ring;
    kiln_port_logstore_t  log_port;

    kiln_sim_t            sim;
    kiln_sim_ports_t      sim_ports;
    kiln_app_t            app;
    double                t_s, na, ns, nc;
} boot_t;

#define WINDOW_DT  0.010
#define SAFETY_DT  0.100
#define ACQUIRE_DT 0.250
#define CONTROL_DT 1.000

static void medium_init(medium_t *m)
{
    memset(m, 0, sizeof(*m));
    kiln_host_flash_init(&m->flash, m->flash_storage, sizeof(m->flash_storage),
                         SECTOR_BYTES);
    kiln_host_kv_init(&m->kv);
    kiln_host_fs_init(&m->fs);
    kiln_host_clock_init(&m->clk, 1767225600ull, true);
}

/* Bring a controller up on the given medium.  Returns what kiln_app_boot said. */
static kiln_err_t boot(boot_t *b, medium_t *m, kiln_reset_cause_t cause,
                       float outage_s, float ambient_c)
{
    memset(b, 0, sizeof(*b));
    b->m = m;

    kiln_host_flash_bind(&m->flash, &b->flash_port);
    kiln_host_kv_bind(&m->kv, &b->kv_port);
    kiln_host_fs_bind(&m->fs, &b->fs_port);
    kiln_host_clock_bind(&m->clk, &b->clk_port);

    (void)kiln_logring_mount(&b->ring, &b->flash_port);
    kiln_logring_bind(&b->ring, &b->log_port);

    kiln_sim_cfg_t sc;
    kiln_sim_cfg_defaults(&sc);
    sc.ambient_c      = ambient_c;
    sc.case_ambient_c = 22.0f;
    sc.tau_s          = 300.0f;
    sc.dead_time_s    = 10.0f;
    sc.noise_c        = 0.1f;
    kiln_sim_init(&b->sim, &sc);
    kiln_sim_set_temperature(&b->sim, ambient_c);
    kiln_sim_bind(&b->sim, &b->sim_ports);

    kiln_app_ports_t ports = {};
    ports.tc        = &b->sim_ports.tc;
    ports.case_tc   = &b->sim_ports.case_tc;
    ports.heat      = &b->sim_ports.heat;
    ports.current   = &b->sim_ports.current;
    ports.counters  = &b->sim_ports.counters;
    ports.logstore  = &b->log_port;
    ports.kvstore   = &b->kv_port;
    ports.filestore = &b->fs_port;
    ports.clock     = &b->clk_port;

    kiln_config_t cfg;
    kiln_config_defaults(&cfg);
    cfg.kp = 6.0f; cfg.ki = 0.02f; cfg.kd = 30.0f;
    cfg.filter_tau_s = 1.0f; cfg.holdback_band_c = 0.0f;
    cfg.log_interval_s = 1;      /* so a short test produces a visible log */
    CHECK_OK(kiln_app_init(&b->app, &ports, &cfg));

    return kiln_app_boot(&b->app, cause, outage_s);
}

static void step(boot_t *b)
{
    kiln_sim_step(&b->sim, (float)WINDOW_DT);
    b->t_s += WINDOW_DT;
    kiln_host_clock_advance(&b->m->clk, (uint64_t)(WINDOW_DT * 1e6));

    kiln_app_window_tick(&b->app, (uint32_t)(WINDOW_DT * 1000.0));
    if (b->t_s >= b->na) { kiln_app_acquire_cycle(&b->app, (float)ACQUIRE_DT); b->na += ACQUIRE_DT; }
    if (b->t_s >= b->ns) { kiln_app_safety_cycle(&b->app, (float)SAFETY_DT);   b->ns += SAFETY_DT;  }
    if (b->t_s >= b->nc) { kiln_app_control_cycle(&b->app, (float)CONTROL_DT); b->nc += CONTROL_DT; }

    /* The logger task: the only thing that touches flash (SWR-LOG-14). */
    (void)kiln_app_log_drain(&b->app, 4);
}

static void run_for(boot_t *b, double seconds)
{
    const int steps = (int)(seconds / WINDOW_DT);
    for (int i = 0; i < steps; i++) {
        step(b);
    }
}

static kiln_program_t program(uint16_t target, uint16_t rate, uint16_t dwell_min)
{
    kiln_program_t p;
    kiln_profile_init_empty(&p, "persist");
    p.segment_count            = 1;
    p.segments[0].target_c     = target;
    p.segments[0].rate_c_per_h = rate;
    p.segments[0].dwell_min    = dwell_min;
    return p;
}

typedef struct {
    uint32_t samples;
    uint32_t events[KILN_LOGE_COUNT];
} tally_t;

static bool tally_fn(void *user, uint32_t run, const uint8_t rec[KILN_LOG_RECORD_BYTES])
{
    (void)run;
    tally_t *t = static_cast<tally_t *>(user);
    kiln_log_sample_t s;
    if (kiln_logrec_decode(rec, &s) != KILN_OK) {
        return true;
    }
    t->samples++;
    if (s.event < KILN_LOGE_COUNT) {
        t->events[s.event]++;
    }
    return true;
}

static tally_t tally(boot_t *b, uint32_t run_id)
{
    tally_t t = {};
    CHECK_OK(kiln_logring_iterate(&b->ring, run_id, tally_fn, &t));
    return t;
}

/* --- SWR-LOG-01..04 ------------------------------------------------------ */

/*
 * @relation(SWR-LOG-01, scope=function)
 */
KILN_TEST(swrlog01_a_firing_is_logged_from_start_to_finish)
{
    static medium_t m;
    static boot_t   b;
    medium_init(&m);
    CHECK_OK(boot(&b, &m, KILN_RESET_POWER_ON, -1.0f, 20.0f));
    run_for(&b, 2.0);

    const kiln_program_t p = program(200, 3600, 0);
    CHECK_OK(kiln_app_start(&b.app, &p));
    const uint32_t run_id = b.app.record.run_id;

    for (int i = 0; i < 120000 && b.app.state == KILN_STATE_RUNNING; i++) {
        step(&b);
    }
    CHECK_EQ_INT(b.app.state, KILN_STATE_COMPLETE);
    run_for(&b, 1.0);                 /* let the logger drain */

    const tally_t t = tally(&b, run_id);
    CHECK(t.samples > 10u);

    /* SWR-LOG-04: the narrative, not just the series. */
    CHECK_EQ_UINT(t.events[KILN_LOGE_RUN_START], 1u);
    CHECK_EQ_UINT(t.events[KILN_LOGE_RUN_END], 1u);
    CHECK(t.events[KILN_LOGE_STATE_CHANGE] >= 1u);
    CHECK(t.events[KILN_LOGE_SAMPLE] > 10u);

    CHECK_EQ_UINT(b.app.log_dropped, 0u);
    CHECK_EQ_UINT(b.app.log_errors, 0u);
}

/*
 * @relation(SWR-RUN-07, scope=function)
 */
KILN_TEST(swrrun07_the_run_record_is_persisted_when_the_firing_ends)
{
    static medium_t m;
    static boot_t   b;
    medium_init(&m);
    CHECK_OK(boot(&b, &m, KILN_RESET_POWER_ON, -1.0f, 20.0f));
    run_for(&b, 2.0);

    const kiln_program_t p = program(200, 3600, 0);
    CHECK_OK(kiln_app_start(&b.app, &p));
    const uint32_t run_id = b.app.record.run_id;
    for (int i = 0; i < 120000 && b.app.state == KILN_STATE_RUNNING; i++) {
        step(&b);
    }

    kiln_run_record_t r;
    CHECK_OK(kiln_run_index_find(&b.fs_port, run_id, &r));
    CHECK_EQ_INT(r.end_reason, KILN_END_COMPLETE);
    CHECK_STR_EQ(r.program.name, "persist");
    CHECK(r.peak_c > 150.0f);
    CHECK(r.duration_s > 0u);
    CHECK(r.start_wall_utc_s > 0u);      /* SWR-LOG-12: the clock was synced */
}

/*
 * @relation(SWR-LOG-14, scope=function)
 */
KILN_TEST(swrlog14_a_log_store_that_fails_does_not_stop_the_firing)
{
    static medium_t m;
    static boot_t   b;
    medium_init(&m);
    CHECK_OK(boot(&b, &m, KILN_RESET_POWER_ON, -1.0f, 20.0f));
    run_for(&b, 2.0);

    const kiln_program_t p = program(200, 3600, 0);
    CHECK_OK(kiln_app_start(&b.app, &p));
    run_for(&b, 5.0);

    /* The flash dies mid-firing. */
    m.flash.fail_write_after = m.flash.writes;

    for (int i = 0; i < 120000 && b.app.state == KILN_STATE_RUNNING; i++) {
        step(&b);
    }

    /* SWR-LOG-14: the firing continued and finished, and the operator was told. */
    CHECK_EQ_INT(b.app.fault, KILN_FAULT_NONE);
    CHECK_EQ_INT(b.app.state, KILN_STATE_COMPLETE);
    CHECK(b.app.log_errors > 0u);
    CHECK(b.app.warnings & KILN_WARN_BIT(KILN_WARN_LOG_UNAVAIL));
}

/*
 * @relation(SWR-LOG-14, scope=function)
 */
KILN_TEST(swrlog14_a_queue_that_overflows_drops_and_counts)
{
    static medium_t m;
    static boot_t   b;
    medium_init(&m);
    CHECK_OK(boot(&b, &m, KILN_RESET_POWER_ON, -1.0f, 20.0f));
    run_for(&b, 2.0);

    const kiln_program_t p = program(600, 600, 10);
    CHECK_OK(kiln_app_start(&b.app, &p));

    /* A logger that never runs: the queue fills, and then the control path keeps
     * going regardless, which is the whole requirement. */
    for (int i = 0; i < 40000; i++) {
        kiln_sim_step(&b.sim, (float)WINDOW_DT);
        b.t_s += WINDOW_DT;
        kiln_app_window_tick(&b.app, 10);
        if (b.t_s >= b.na) { kiln_app_acquire_cycle(&b.app, (float)ACQUIRE_DT); b.na += ACQUIRE_DT; }
        if (b.t_s >= b.ns) { kiln_app_safety_cycle(&b.app, (float)SAFETY_DT);   b.ns += SAFETY_DT;  }
        if (b.t_s >= b.nc) { kiln_app_control_cycle(&b.app, (float)CONTROL_DT); b.nc += CONTROL_DT; }
    }

    CHECK(b.app.log_dropped > 0u);
    CHECK_EQ_INT(b.app.fault, KILN_FAULT_NONE);
    CHECK_EQ_INT(b.app.state, KILN_STATE_RUNNING);
    CHECK(b.app.warnings & KILN_WARN_BIT(KILN_WARN_LOG_UNAVAIL));
}

/* --- SWR-CFG-05 across a reboot ------------------------------------------ */

/*
 * @relation(SWR-CFG-05, scope=function)
 */
KILN_TEST(swrcfg05_configuration_survives_a_reboot)
{
    static medium_t m;
    static boot_t   b;
    medium_init(&m);
    CHECK_OK(boot(&b, &m, KILN_RESET_POWER_ON, -1.0f, 20.0f));

    kiln_config_t cfg = b.app.cfg;
    cfg.max_temp_c    = 1100.0f;
    cfg.log_interval_s = 25;
    const kiln_cfg_item_t *bad = NULL;
    CHECK_OK(kiln_app_apply_config(&b.app, &cfg, &bad));

    /* Reboot. */
    static boot_t b2;
    CHECK_OK(boot(&b2, &m, KILN_RESET_POWER_ON, -1.0f, 20.0f));
    CHECK_NEAR(b2.app.cfg.max_temp_c, 1100.0f, 0.01f);
    CHECK_EQ_UINT(b2.app.cfg.log_interval_s, 25u);

    /* And it reached the safety supervisor, not just the struct. */
    CHECK_NEAR(b2.app.safety.cfg.max_temp_c, 1100.0f, 0.01f);
}

/*
 * @relation(SWR-PRG-09, scope=function)
 */
KILN_TEST(swrprg09_the_examples_are_present_after_a_first_boot)
{
    static medium_t m;
    static boot_t   b;
    medium_init(&m);
    CHECK_OK(boot(&b, &m, KILN_RESET_POWER_ON, -1.0f, 20.0f));

    CHECK_EQ_UINT(kiln_program_store_count(&b.fs_port), kiln_profile_example_count());

    kiln_program_t p;
    CHECK_OK(kiln_program_store_load(&b.fs_port, "Glaze cone 6", &p));
    CHECK(p.flags & KILN_PROG_FLAG_READONLY);
}

/* --- SWR-SAF-17 across a reboot ---------------------------------------------- */

/*
 * @relation(SWR-SAF-17, scope=function)
 */
KILN_TEST(swrsaf17_a_latched_fault_is_still_latched_after_a_power_cycle)
{
    static medium_t m;
    static boot_t   b;
    medium_init(&m);
    CHECK_OK(boot(&b, &m, KILN_RESET_POWER_ON, -1.0f, 20.0f));
    run_for(&b, 2.0);

    const kiln_program_t p = program(600, 0, 10);
    CHECK_OK(kiln_app_start(&b.app, &p));
    run_for(&b, 10.0);

    /* Weld the contactor and short the SSR: SWR-SAF-27's worst verdict. */
    kiln_sim_inject(&b.sim, KILN_INJ_SSR_SHORTED | KILN_INJ_CONTACTOR_WELD);
    for (int i = 0; i < 20000 && b.app.fault == KILN_FAULT_NONE; i++) {
        step(&b);
    }
    CHECK_EQ_INT(b.app.fault, KILN_FAULT_CONTACTOR_WELDED);

    /* Power cycle.  A fault that evaporates on reboot is a fault the operator
     * never sees, and this is the one whose instruction is to isolate the kiln. */
    static boot_t b2;
    CHECK_OK(boot(&b2, &m, KILN_RESET_POWER_ON, -1.0f, 500.0f));
    CHECK_EQ_INT(b2.app.fault, KILN_FAULT_CONTACTOR_WELDED);
    CHECK_EQ_INT(b2.app.state, KILN_STATE_FAULT);
    CHECK(b2.app.latched_valid);

    /* The snapshot came back with it. */
    CHECK_EQ_INT(b2.app.latched.fault, KILN_FAULT_CONTACTOR_WELDED);
    CHECK(b2.app.latched.kiln_c > 20.0f);

    /* And no run can be started while it stands. */
    CHECK_ERR(kiln_app_start(&b2.app, &p), KILN_ERR_STATE);
}

/*
 * @relation(SWR-SAF-18, scope=function)
 */
KILN_TEST(swrsaf18_clearing_a_fault_clears_the_stored_copy_too)
{
    static medium_t m;
    static boot_t   b;
    medium_init(&m);
    CHECK_OK(boot(&b, &m, KILN_RESET_POWER_ON, -1.0f, 20.0f));
    run_for(&b, 2.0);

    kiln_sim_inject(&b.sim, KILN_INJ_TC_OPEN);
    for (int i = 0; i < 20000 && b.app.fault == KILN_FAULT_NONE; i++) {
        step(&b);
    }
    CHECK_EQ_INT(b.app.fault, KILN_FAULT_TC_OPEN);

    kiln_sim_clear(&b.sim, KILN_INJ_TC_OPEN);
    run_for(&b, 1.0);
    CHECK_OK(kiln_app_clear_fault(&b.app));

    /* Reboot: it must not come back, or acknowledging it achieved nothing. */
    static boot_t b2;
    CHECK_OK(boot(&b2, &m, KILN_RESET_POWER_ON, -1.0f, 20.0f));
    CHECK_EQ_INT(b2.app.fault, KILN_FAULT_NONE);
    CHECK_EQ_INT(b2.app.state, KILN_STATE_IDLE);
}

/* --- SWR-RUN-08 ---------------------------------------------------------- */

/* Cut power part-way into a firing and bring the controller back up. */
static void interrupt_mid_run(medium_t *m, boot_t *b, uint16_t target,
                              double seconds, float *temp_out)
{
    medium_init(m);
    CHECK_OK(boot(b, m, KILN_RESET_POWER_ON, -1.0f, 20.0f));
    run_for(b, 2.0);

    const kiln_program_t p = program(target, 1800, 30);
    CHECK_OK(kiln_app_start(&b->app, &p));
    run_for(b, seconds);
    CHECK_EQ_INT(b->app.state, KILN_STATE_RUNNING);
    if (temp_out != nullptr) {
        *temp_out = b->app.kiln_c;
    }
    /* Power vanishes.  Nothing is flushed, nothing is closed. */
}

/*
 * @relation(SWR-RUN-08, scope=function)
 */
KILN_TEST(swrrun08_the_default_policy_aborts_an_interrupted_firing)
{
    static medium_t m;
    static boot_t   b;
    float temp = 0.0f;
    interrupt_mid_run(&m, &b, 600, 150.0, &temp);
    /* 1800 degC/h for 150 s is ~75 degC of ramp from 20 degC ambient. */
    CHECK_MSG(temp > 50.0f, "the kiln only reached %.1f degC", (double)temp);

    static boot_t b2;
    CHECK_OK(boot(&b2, &m, KILN_RESET_POWER_ON, 60.0f, temp));

    /* Resuming is something an operator opts into, so the default is abort --
     * and an abort is not a fault. */
    CHECK_EQ_INT(b2.app.recovery.action, KILN_RECOVER_ABORT);
    CHECK_EQ_INT(b2.app.fault, KILN_FAULT_NONE);
    CHECK_EQ_INT(b2.app.state, KILN_STATE_IDLE);
    CHECK(b2.app.recovery.reason && b2.app.recovery.reason[0]);
}

/*
 * @relation(SWR-RUN-08, scope=function)
 */
KILN_TEST(swrrun08_resume_is_offered_when_the_policy_allows_and_the_kiln_is_in_band)
{
    static medium_t m;
    static boot_t   b;
    float temp = 0.0f;

    medium_init(&m);
    CHECK_OK(boot(&b, &m, KILN_RESET_POWER_ON, -1.0f, 20.0f));

    kiln_config_t cfg = b.app.cfg;
    cfg.recovery_policy = KILN_RECOVERY_RESUME;
    const kiln_cfg_item_t *bad = NULL;
    CHECK_OK(kiln_app_apply_config(&b.app, &cfg, &bad));

    run_for(&b, 2.0);
    const kiln_program_t p = program(600, 1800, 30);
    CHECK_OK(kiln_app_start(&b.app, &p));
    run_for(&b, 120.0);
    temp = b.app.kiln_c;

    /* Back five minutes later, still hot: the schedule can be picked up. */
    static boot_t b2;
    CHECK_OK(boot(&b2, &m, KILN_RESET_POWER_ON, 300.0f, temp));
    CHECK_EQ_INT(b2.app.recovery.action, KILN_RECOVER_RESUME);
    CHECK_EQ_INT(b2.app.fault, KILN_FAULT_NONE);
    /* SWA-09: the log tail supplied the setpoint and the segment. */
    CHECK(b2.app.recovery.setpoint_c > 20.0f);
    CHECK(b2.app.recovery.t_rel_ms > 0u);
}

/*
 * @relation(SWR-RUN-08, scope=function)
 */
KILN_TEST(swrrun08_a_kiln_that_has_cooled_out_of_band_is_refused_and_faults)
{
    static medium_t m;
    static boot_t   b;

    medium_init(&m);
    CHECK_OK(boot(&b, &m, KILN_RESET_POWER_ON, -1.0f, 20.0f));
    kiln_config_t cfg = b.app.cfg;
    cfg.recovery_policy = KILN_RECOVERY_RESUME;
    const kiln_cfg_item_t *bad = NULL;
    CHECK_OK(kiln_app_apply_config(&b.app, &cfg, &bad));

    run_for(&b, 2.0);
    const kiln_program_t p = program(600, 1800, 30);
    CHECK_OK(kiln_app_start(&b.app, &p));
    run_for(&b, 150.0);

    /* Back a few minutes later, but the kiln is cold: resuming would thermally
     * shock whatever is inside it. */
    static boot_t b2;
    CHECK_OK(boot(&b2, &m, KILN_RESET_POWER_ON, 120.0f, 25.0f));
    CHECK_EQ_INT(b2.app.recovery.action, KILN_RECOVER_REFUSED);
    CHECK_EQ_INT(b2.app.fault, KILN_FAULT_RECOVERY_REFUSED);
    CHECK_EQ_INT(b2.app.state, KILN_STATE_FAULT);

    /* And that refusal is itself latched, so it survives another power cut. */
    static boot_t b3;
    CHECK_OK(boot(&b3, &m, KILN_RESET_POWER_ON, -1.0f, 25.0f));
    CHECK_EQ_INT(b3.app.fault, KILN_FAULT_RECOVERY_REFUSED);
}

/*
 * @relation(SWR-RUN-08, scope=function)
 */
KILN_TEST(swrrun08_an_outage_longer_than_the_limit_is_refused)
{
    static medium_t m;
    static boot_t   b;

    medium_init(&m);
    CHECK_OK(boot(&b, &m, KILN_RESET_POWER_ON, -1.0f, 20.0f));
    kiln_config_t cfg = b.app.cfg;
    cfg.recovery_policy = KILN_RECOVERY_RESUME;
    const kiln_cfg_item_t *bad = NULL;
    CHECK_OK(kiln_app_apply_config(&b.app, &cfg, &bad));

    run_for(&b, 2.0);
    const kiln_program_t p = program(600, 1800, 30);
    CHECK_OK(kiln_app_start(&b.app, &p));
    run_for(&b, 120.0);
    const float temp = b.app.kiln_c;

    /* Sixteen minutes, against a fifteen-minute limit. */
    static boot_t b2;
    CHECK_OK(boot(&b2, &m, KILN_RESET_POWER_ON, 16.0f * 60.0f, temp));
    CHECK_EQ_INT(b2.app.recovery.action, KILN_RECOVER_REFUSED);
    CHECK_EQ_INT(b2.app.fault, KILN_FAULT_RECOVERY_REFUSED);
}

/*
 * @relation(SWR-SAF-14, scope=function)
 */
KILN_TEST(swrsaf14_a_watchdog_reset_during_a_firing_is_never_resumed)
{
    static medium_t m;
    static boot_t   b;

    medium_init(&m);
    CHECK_OK(boot(&b, &m, KILN_RESET_POWER_ON, -1.0f, 20.0f));
    kiln_config_t cfg = b.app.cfg;
    cfg.recovery_policy = KILN_RECOVERY_RESUME;
    const kiln_cfg_item_t *bad = NULL;
    CHECK_OK(kiln_app_apply_config(&b.app, &cfg, &bad));

    run_for(&b, 2.0);
    const kiln_program_t p = program(600, 1800, 30);
    CHECK_OK(kiln_app_start(&b.app, &p));
    run_for(&b, 120.0);
    const float temp = b.app.kiln_c;

    /* SWR-NFR-15: the firmware's own state was in question at the moment it died, so
     * the policy does not get a vote. */
    static boot_t b2;
    CHECK_OK(boot(&b2, &m, KILN_RESET_TASK_WDT, 5.0f, temp));
    CHECK_EQ_INT(b2.app.recovery.action, KILN_RECOVER_REFUSED);
    CHECK_EQ_INT(b2.app.fault, KILN_FAULT_WATCHDOG);
}

/*
 * @relation(SWR-RUN-08, scope=function)
 */
KILN_TEST(swrrun08_a_clean_boot_after_a_completed_firing_is_an_ordinary_boot)
{
    static medium_t m;
    static boot_t   b;
    medium_init(&m);
    CHECK_OK(boot(&b, &m, KILN_RESET_POWER_ON, -1.0f, 20.0f));
    run_for(&b, 2.0);

    const kiln_program_t p = program(200, 3600, 0);
    CHECK_OK(kiln_app_start(&b.app, &p));
    for (int i = 0; i < 120000 && b.app.state == KILN_STATE_RUNNING; i++) {
        step(&b);
    }
    CHECK_EQ_INT(b.app.state, KILN_STATE_COMPLETE);
    run_for(&b, 1.0);

    static boot_t b2;
    CHECK_OK(boot(&b2, &m, KILN_RESET_POWER_ON, -1.0f, 150.0f));
    CHECK_EQ_INT(b2.app.recovery.action, KILN_RECOVER_NO_RUN);
    CHECK_EQ_INT(b2.app.fault, KILN_FAULT_NONE);
    CHECK_EQ_INT(b2.app.state, KILN_STATE_IDLE);
}

/*
 * @relation(SWR-RUN-08, scope=function)
 */
KILN_TEST(swrrun08_recovery_is_decided_at_many_different_instants)
{
    /* Architecture 14.4 asks for power loss at random instants.  Cutting at a
     * spread of points through a firing exercises the ring head landing mid
     * sector, on a boundary, and immediately after a wrap. */
    for (int i = 1; i <= 12; i++) {
        static medium_t m;
        static boot_t   b;
        float temp = 0.0f;

        medium_init(&m);
        CHECK_OK(boot(&b, &m, KILN_RESET_POWER_ON, -1.0f, 20.0f));
        kiln_config_t cfg = b.app.cfg;
        cfg.recovery_policy = KILN_RECOVERY_RESUME;
        const kiln_cfg_item_t *bad = NULL;
        CHECK_OK(kiln_app_apply_config(&b.app, &cfg, &bad));

        run_for(&b, 2.0);
        const kiln_program_t p = program(600, 1800, 30);
        CHECK_OK(kiln_app_start(&b.app, &p));
        run_for(&b, 7.3 * (double)i);
        temp = b.app.kiln_c;

        static boot_t b2;
        const kiln_err_t e = boot(&b2, &m, KILN_RESET_POWER_ON, 60.0f, temp);
        CHECK_MSG(e == KILN_OK, "boot after a cut at %.1f s failed: %s",
                  7.3 * (double)i, kiln_err_str(e));

        /* Whatever it decides, it must decide *something* and never come up
         * believing it is still firing. */
        CHECK(b2.app.recovery.action == KILN_RECOVER_RESUME ||
              b2.app.recovery.action == KILN_RECOVER_ABORT  ||
              b2.app.recovery.action == KILN_RECOVER_REFUSED);
        CHECK(b2.app.state != KILN_STATE_RUNNING);
        CHECK(!b2.app.heat_authorised);
    }
}

/*
 * @relation(SWA-09, scope=function)
 */
KILN_TEST(swa09_recovery_needs_no_write_of_its_own)
{
    /* The whole point of SWA-09: a 10 s run-state write to NVS would have added
     * ~60 000 writes per week-long run.  Recovery state comes out of the log,
     * which was already being written. */
    static medium_t m;
    static boot_t   b;
    medium_init(&m);
    CHECK_OK(boot(&b, &m, KILN_RESET_POWER_ON, -1.0f, 20.0f));
    run_for(&b, 2.0);

    const uint32_t sets_before = m.kv.sets;

    const kiln_program_t p = program(600, 1800, 30);
    CHECK_OK(kiln_app_start(&b.app, &p));
    run_for(&b, 120.0);

    /* Two minutes of firing, and not one key/value write. */
    CHECK_EQ_UINT(m.kv.sets, sets_before);

    /* But the log tail still knows where the firing had got to. */
    uint8_t rec[KILN_LOG_RECORD_BYTES];
    CHECK_OK(kiln_logring_last_record(&b.ring, 0, rec));
    kiln_log_sample_t s;
    CHECK_OK(kiln_logrec_decode(rec, &s));
    CHECK_EQ_UINT(s.state, KILN_STATE_RUNNING);
    CHECK(s.setpoint_c > 20.0f);
}

/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Configuration, program and run-record persistence -- SWR-CFG-05, SWR-SAF-17,
 * SWR-PRG-04, SWR-PRG-09, SWR-RUN-07, SWR-LOG-09.
 */

#include <string.h>
#include "kiln_check.h"
#include "kiln_app/run_index.h"
#include "kiln_app/settings.h"
#include "kiln_core/profile.h"
#include "kiln_hal_host/hal_host.h"

/* --- configuration (SWR-CFG-05) ------------------------------------------ */

/*
 * @relation(SWR-CFG-05, scope=function)
 */
KILN_TEST(swrcfg05_a_saved_configuration_comes_back)
{
    kiln_host_kv_t kv;
    kiln_port_kvstore_t port;
    kiln_host_kv_init(&kv);
    kiln_host_kv_bind(&kv, &port);

    kiln_config_t out;
    /* Nothing stored yet is not an error, it is a first boot. */
    CHECK_ERR(kiln_settings_load(&port, &out), KILN_ERR_NOT_FOUND);
    CHECK_NEAR(out.max_temp_c, 1280.0f, 0.01f);    /* and defaults are in place */

    kiln_config_t in;
    kiln_config_defaults(&in);
    in.max_temp_c    = 1150.0f;
    in.log_interval_s = 30;
    CHECK_OK(kiln_config_set_str(&in, kiln_config_find("net.hostname"), "kiln-a"));
    CHECK_OK(kiln_settings_save(&port, &in));

    CHECK_OK(kiln_settings_load(&port, &out));
    CHECK_NEAR(out.max_temp_c, 1150.0f, 0.01f);
    CHECK_EQ_UINT(out.log_interval_s, 30u);
    CHECK_STR_EQ(out.hostname, "kiln-a");

    /* Committed, not merely set: an uncommitted configuration is one the next
     * boot has never heard of. */
    CHECK(kv.commits > 0u);
}

/*
 * @relation(SWR-CFG-05, scope=function)
 */
KILN_TEST(swrcfg05_storage_that_will_not_answer_leaves_defaults_in_place)
{
    kiln_host_kv_t kv;
    kiln_port_kvstore_t port;
    kiln_host_kv_init(&kv);
    kiln_host_kv_bind(&kv, &port);

    /* Something stored, then a store that fails reads.  Simulated by storing a
     * blob that is not a configuration at all. */
    CHECK_OK(port.set(port.ctx, KILN_NVS_NAMESPACE, KILN_NVS_KEY_CONFIG, "junk", 4));

    kiln_config_t out;
    CHECK_ERR(kiln_settings_load(&port, &out), KILN_ERR_CORRUPT);
    /* There is no state in which the controller may run without a
     * configuration, so a failure still leaves a valid one. */
    CHECK_OK(kiln_config_validate(&out, NULL));
    CHECK_NEAR(out.max_temp_c, 1280.0f, 0.01f);
}

/*
 * @relation(SWR-CFG-05, scope=function)
 */
KILN_TEST(swrcfg05_a_write_failure_is_reported_rather_than_silently_lost)
{
    kiln_host_kv_t kv;
    kiln_port_kvstore_t port;
    kiln_host_kv_init(&kv);
    kiln_host_kv_bind(&kv, &port);
    kv.fail_writes = true;

    kiln_config_t cfg;
    kiln_config_defaults(&cfg);
    CHECK_ERR(kiln_settings_save(&port, &cfg), KILN_ERR_IO);
}

/* --- the latched fault (SWR-SAF-17) ------------------------------------------ */

/*
 * @relation(SWR-SAF-17, scope=function)
 */
KILN_TEST(swrsaf17_a_latched_fault_survives_a_power_loss_with_its_snapshot)
{
    kiln_host_kv_t kv;
    kiln_port_kvstore_t port;
    kiln_host_kv_init(&kv);
    kiln_host_kv_bind(&kv, &port);

    kiln_latched_fault_t out;
    CHECK_ERR(kiln_settings_load_fault(&port, &out), KILN_ERR_NOT_FOUND);

    const kiln_latched_fault_t in = {
        .fault         = KILN_FAULT_CONTACTOR_WELDED,
        .state         = KILN_STATE_FAULT,
        .flags         = 0,
        .run_id        = 42,
        .t_rel_ms      = 3600000u,
        .wall_utc_s    = 1767225600ull,
        .kiln_c        = 987.5f,
        .setpoint_c    = 990.0f,
        .case_c        = 45.5f,
        .current_a     = 24.75f,
        .duty_permille = 650,
        .warnings      = KILN_WARN_BIT(KILN_WARN_RELAY_SUSPECT),
    };
    CHECK_OK(kiln_settings_save_fault(&port, &in));

    /* A bare code would tell the operator that something stopped the firing but
     * not what the kiln was doing, which is the half that makes it
     * diagnosable. */
    CHECK_OK(kiln_settings_load_fault(&port, &out));
    CHECK_EQ_INT(out.fault, KILN_FAULT_CONTACTOR_WELDED);
    CHECK_EQ_UINT(out.run_id, 42u);
    CHECK_EQ_UINT(out.t_rel_ms, 3600000u);
    CHECK_EQ_UINT(out.wall_utc_s, 1767225600ull);
    CHECK_NEAR(out.kiln_c, 987.5f, 0.001f);
    CHECK_NEAR(out.current_a, 24.75f, 0.001f);
    CHECK_EQ_UINT(out.duty_permille, 650u);
    CHECK_EQ_UINT(out.warnings, KILN_WARN_BIT(KILN_WARN_RELAY_SUSPECT));

    /* SWR-SAF-18's acknowledgement path clears it, and then it stays cleared. */
    CHECK_OK(kiln_settings_clear_fault(&port));
    CHECK_ERR(kiln_settings_load_fault(&port, &out), KILN_ERR_NOT_FOUND);
}

/*
 * @relation(SWR-SAF-17, scope=function)
 */
KILN_TEST(swrsaf17_a_corrupt_stored_fault_is_not_believed)
{
    kiln_host_kv_t kv;
    kiln_port_kvstore_t port;
    kiln_host_kv_init(&kv);
    kiln_host_kv_bind(&kv, &port);

    kiln_latched_fault_t in = {};
    in.fault = KILN_FAULT_OVERTEMP;
    CHECK_OK(kiln_settings_save_fault(&port, &in));

    /* Flip a byte in the stored blob. */
    kiln_host_kv_entry_t *e = NULL;
    for (size_t i = 0; i < KILN_HOST_KV_ENTRIES; i++) {
        if (kv.entries[i].used && strcmp(kv.entries[i].key, KILN_NVS_KEY_FAULT) == 0) {
            e = &kv.entries[i];
        }
    }
    CHECK(e != NULL);
    e->value[10] ^= 0xFFu;

    kiln_latched_fault_t out;
    CHECK_ERR(kiln_settings_load_fault(&port, &out), KILN_ERR_CORRUPT);
}

/* --- programs (FR-PRG) -------------------------------------------------- */

static kiln_program_t named(const char *name, uint16_t target)
{
    kiln_program_t p;
    kiln_profile_init_empty(&p, name);
    p.segment_count            = 1;
    p.segments[0].target_c     = target;
    p.segments[0].rate_c_per_h = 120;
    p.segments[0].dwell_min    = 10;
    return p;
}

/* --- the programs, which are now firmware (SWR-PRG-09) ------------------
 *
 * Six tests used to live here, over a store that saved programs to flash,
 * replaced them by name, refused to overwrite the read-only examples, filled
 * twenty slots and then said no, and re-validated what it read back because a
 * stored program is untrusted input.
 *
 * All of it is gone, with the store. SWR-PRG-07, SWR-PRG-08 and SWR-PRG-10 are
 * withdrawn -- nothing authors a program on this device -- and the three
 * profiles that exist arrive in the image, so persisting them was keeping a
 * copy of something the firmware already contained, in a medium that can wear
 * out, behind a seeding path that could fail.
 *
 * What survives is the property that actually matters, and it is now checkable
 * without a medium at all: the programs the firmware carries are valid
 * programs. A profile compiled in wrong is a firing that fails at the moment
 * somebody starts it.
 */

/*
 * @relation(SWR-PRG-09, scope=function)
 */
KILN_TEST(swrprg09_every_compiled_in_program_is_a_valid_program)
{
    const uint8_t n = kiln_profile_example_count();
    CHECK(n > 0u);
    CHECK(n <= KILN_MAX_PROGRAMS);

    for (uint8_t i = 0; i < n; i++) {
        kiln_program_t p;
        CHECK_OK(kiln_profile_example(i, &p));

        /* Named, terminated, and not empty: the name is what the menu and the
         * API show, and the run record keeps a copy of it. */
        CHECK(p.name[0] != '\0');
        CHECK(strnlen(p.name, KILN_PROGRAM_NAME_LEN) < KILN_PROGRAM_NAME_LEN);

        /* Validates against the default ceiling of SWR-SAF-01. An example that
         * does not is one an operator cannot start, which they would discover
         * by selecting it. */
        CHECK_MSG(kiln_profile_validate(&p, 1280.0f).code == KILN_PROG_OK,
                  "example %u (%s) does not validate", i, p.name);
        CHECK(p.segment_count > 0u);
        CHECK(kiln_profile_peak_c(&p) > 0.0f);
    }

    /* Names are distinct, because they are what the operator picks by. */
    for (uint8_t i = 0; i < n; i++) {
        for (uint8_t k = (uint8_t)(i + 1u); k < n; k++) {
            kiln_program_t a, b;
            CHECK_OK(kiln_profile_example(i, &a));
            CHECK_OK(kiln_profile_example(k, &b));
            CHECK_MSG(strcmp(a.name, b.name) != 0,
                      "examples %u and %u are both called %s", i, k, a.name);
        }
    }

    /* And an index past the end is refused rather than returning whatever is
     * next in memory: the id comes off a URL (/api/programs/{id}). */
    kiln_program_t out;
    CHECK_ERR(kiln_profile_example(n, &out), KILN_ERR_NOT_FOUND);
    CHECK_ERR(kiln_profile_example(255, &out), KILN_ERR_NOT_FOUND);
    CHECK_ERR(kiln_profile_example(0, nullptr), KILN_ERR_INVALID_ARG);
}

/* --- run records (SWR-RUN-07, SWR-LOG-09) -------------------------------- */

static kiln_run_record_t run(uint32_t id, kiln_run_end_t reason)
{
    kiln_run_record_t r;
    kiln_runstate_record_init(&r, id);
    r.end_reason    = (uint8_t)reason;
    r.duration_s    = 3600u * id;
    r.peak_c        = 900.0f + (float)id;
    r.current_ref_a = 29.5f;
    r.energy_wh     = 12000.0f;
    r.program       = named("fired", 900);
    return r;
}

/*
 * @relation(SWR-RUN-07, scope=function)
 */
KILN_TEST(swrrun07_a_run_record_round_trips_with_everything_the_requirement_lists)
{
    kiln_host_fs_t fs;
    kiln_port_filestore_t port;
    kiln_host_fs_init(&fs);
    kiln_host_fs_bind(&fs, &port);

    kiln_run_record_t in = run(1, KILN_END_COMPLETE);
    in.gains.kp        = 2.5f;
    in.gains.ki        = 0.01f;
    in.gains.kd        = 90.0f;
    in.band_duty_s[5]  = 1234u;
    in.contactor_ops   = 777u;
    in.ssr_ops[0]      = 8888u;
    in.start_wall_utc_s = 1767225600ull;
    CHECK_OK(kiln_run_index_append(&port, &in));

    kiln_run_record_t out;
    CHECK_OK(kiln_run_index_find(&port, 1, &out));
    CHECK_EQ_UINT(out.run_id, 1u);
    CHECK_EQ_INT(out.end_reason, KILN_END_COMPLETE);
    CHECK_STR_EQ(out.program.name, "fired");           /* program as executed */
    CHECK_NEAR(out.gains.kp, 2.5f, 0.001f);            /* gains used */
    CHECK_NEAR(out.current_ref_a, 29.5f, 0.001f);      /* SWR-CUR-08 reference */
    CHECK_EQ_UINT(out.band_duty_s[5], 1234u);          /* SWR-SAF-12 bands */
    CHECK_EQ_UINT(out.contactor_ops, 777u);            /* SWR-CUR-13 */
    CHECK_EQ_UINT(out.start_wall_utc_s, 1767225600ull);
    CHECK_NEAR(out.peak_c, 901.0f, 0.001f);
}

/*
 * @relation(SWR-LOG-09, scope=function)
 */
KILN_TEST(swrlog09_twenty_runs_are_retained_and_the_oldest_is_evicted)
{
    kiln_host_fs_t fs;
    kiln_port_filestore_t port;
    kiln_host_fs_init(&fs);
    kiln_host_fs_bind(&fs, &port);

    for (uint32_t id = 1; id <= KILN_RUN_SLOTS; id++) {
        const kiln_run_record_t r = run(id, KILN_END_COMPLETE);
        CHECK_OK(kiln_run_index_append(&port, &r));
    }
    CHECK_EQ_UINT(kiln_run_index_count(&port), KILN_RUN_SLOTS);
    CHECK(KILN_RUN_SLOTS >= 20u);              /* SWR-LOG-09's floor */

    /* The 21st evicts run 1 and nothing else. */
    const kiln_run_record_t extra = run(KILN_RUN_SLOTS + 1u, KILN_END_COMPLETE);
    CHECK_OK(kiln_run_index_append(&port, &extra));
    CHECK_EQ_UINT(kiln_run_index_count(&port), KILN_RUN_SLOTS);

    kiln_run_record_t out;
    CHECK_ERR(kiln_run_index_find(&port, 1, &out), KILN_ERR_NOT_FOUND);
    CHECK_OK(kiln_run_index_find(&port, 2, &out));
    CHECK_OK(kiln_run_index_find(&port, KILN_RUN_SLOTS + 1u, &out));
}

/*
 * @relation(SWR-LOG-09, scope=function)
 */
KILN_TEST(swrlog09_a_run_whose_samples_are_gone_is_marked_truncated)
{
    /* Otherwise a chart with no data in it is indistinguishable from a run that
     * never logged, and the operator is left guessing which. */
    kiln_host_fs_t fs;
    kiln_port_filestore_t port;
    kiln_host_fs_init(&fs);
    kiln_host_fs_bind(&fs, &port);

    for (uint32_t id = 1; id <= 5; id++) {
        const kiln_run_record_t r = run(id, KILN_END_COMPLETE);
        CHECK_OK(kiln_run_index_append(&port, &r));
    }

    /* The ring now only goes back as far as run 3. */
    CHECK_OK(kiln_run_index_mark_truncated(&port, 3));

    kiln_run_record_t out;
    CHECK_OK(kiln_run_index_find(&port, 1, &out));
    CHECK(out.flags & KILN_RUN_FLAG_TRUNCATED);
    CHECK_OK(kiln_run_index_find(&port, 2, &out));
    CHECK(out.flags & KILN_RUN_FLAG_TRUNCATED);
    CHECK_OK(kiln_run_index_find(&port, 3, &out));
    CHECK(!(out.flags & KILN_RUN_FLAG_TRUNCATED));

    /* Idempotent: re-marking rewrites nothing, which matters because this runs
     * on every boot and each rewrite is a flash write. */
    const uint32_t writes = fs.writes;
    CHECK_OK(kiln_run_index_mark_truncated(&port, 3));
    CHECK_EQ_UINT(fs.writes, writes);
}

KILN_TEST(run_numbering_continues_across_a_reboot)
{
    kiln_host_fs_t fs;
    kiln_port_filestore_t port;
    kiln_host_fs_init(&fs);
    kiln_host_fs_bind(&fs, &port);

    /* No history: the first run is 1. */
    CHECK_EQ_UINT(kiln_run_index_next_run_id(&port), 1u);

    for (uint32_t id = 1; id <= 5; id++) {
        const kiln_run_record_t r = run(id, KILN_END_COMPLETE);
        CHECK_OK(kiln_run_index_append(&port, &r));
    }
    /* Restarting the numbering would collide with records already on disk, and
     * then a log query by run id would return two runs' samples. */
    CHECK_EQ_UINT(kiln_run_index_next_run_id(&port), 6u);
}

/*
 * @relation(SWR-SAF-12, scope=function)
 */
KILN_TEST(swrsaf12_the_baseline_comes_from_the_stored_run_history)
{
    kiln_host_fs_t fs;
    kiln_port_filestore_t port;
    kiln_host_fs_init(&fs);
    kiln_host_fs_bind(&fs, &port);

    for (uint32_t id = 1; id <= 5; id++) {
        kiln_run_record_t r = run(id, KILN_END_COMPLETE);
        r.band_duty_s[5] = 900u + id * 50u;        /* 950 .. 1150 */
        CHECK_OK(kiln_run_index_append(&port, &r));
    }

    kiln_insulation_baseline_t bl;
    CHECK_OK(kiln_run_index_baseline(&port, 3, &bl));
    CHECK(bl.valid[5]);
    CHECK_EQ_UINT(bl.duty_s[5], 1050u);            /* the median */
    CHECK(!bl.valid[0]);
}

KILN_TEST(an_atomic_replace_leaves_the_previous_content_on_a_power_cut)
{
    /* The property atomic-by-two-copies buys, and the one a plain write loses:
     * half a record on disk is worse than the old record.  Driven through the
     * run index, which is what still writes to this store now that the
     * programs are compiled into the image. */
    kiln_host_fs_t fs;
    kiln_port_filestore_t port;
    kiln_host_fs_init(&fs);
    kiln_host_fs_bind(&fs, &port);

    kiln_run_record_t a = run(1, KILN_END_COMPLETE);
    a.duration_s = 3600u;
    CHECK_OK(kiln_run_index_append(&port, &a));

    fs.cut_power_at_write = fs.writes + 1u;
    kiln_run_record_t b = a;
    b.duration_s = 7200u;
    (void)kiln_run_index_append(&port, &b);

    fs.powered = true;
    kiln_run_record_t out = {};
    CHECK_OK(kiln_run_index_find(&port, 1u, &out));
    CHECK_EQ_UINT(out.duration_s, 3600u);    /* the old one, intact */
}

KILN_TEST(the_stores_validate_their_arguments)
{
    kiln_host_fs_t fs;
    kiln_port_filestore_t port;
    kiln_host_fs_init(&fs);
    kiln_host_fs_bind(&fs, &port);

    kiln_run_record_t r;

    CHECK_ERR(kiln_run_index_append(&port, NULL), KILN_ERR_INVALID_ARG);
    CHECK_ERR(kiln_run_index_get_slot(&port, 200, &r), KILN_ERR_INVALID_ARG);
    CHECK_ERR(kiln_run_index_find(NULL, 1, &r), KILN_ERR_INVALID_ARG);

    CHECK_ERR(kiln_settings_load(NULL, NULL), KILN_ERR_INVALID_ARG);
    CHECK_ERR(kiln_settings_save(NULL, NULL), KILN_ERR_INVALID_ARG);
}

/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Configuration, program and run-record persistence -- FR-CFG-05, SR-17,
 * FR-PRG-04, FR-PRG-09, FR-RUN-07, FR-LOG-09.
 */

#include <string.h>
#include "kiln_check.h"
#include "kiln_app/program_store.h"
#include "kiln_app/run_index.h"
#include "kiln_app/settings.h"
#include "kiln_core/profile.h"
#include "kiln_hal_host/hal_host.h"

/* --- configuration (FR-CFG-05) ------------------------------------------ */

KILN_TEST(frcfg05_a_saved_configuration_comes_back)
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

KILN_TEST(frcfg05_storage_that_will_not_answer_leaves_defaults_in_place)
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

KILN_TEST(frcfg05_a_write_failure_is_reported_rather_than_silently_lost)
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

/* --- the latched fault (SR-17) ------------------------------------------ */

KILN_TEST(sr17_a_latched_fault_survives_a_power_loss_with_its_snapshot)
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

    /* SR-18's acknowledgement path clears it, and then it stays cleared. */
    CHECK_OK(kiln_settings_clear_fault(&port));
    CHECK_ERR(kiln_settings_load_fault(&port, &out), KILN_ERR_NOT_FOUND);
}

KILN_TEST(sr17_a_corrupt_stored_fault_is_not_believed)
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

KILN_TEST(frprg09_the_examples_are_seeded_once_and_are_read_only)
{
    kiln_host_fs_t fs;
    kiln_port_filestore_t port;
    kiln_host_fs_init(&fs);
    kiln_host_fs_bind(&fs, &port);

    CHECK_OK(kiln_program_store_seed(&port));
    const uint8_t n = kiln_profile_example_count();
    CHECK_EQ_UINT(kiln_program_store_count(&port), n);

    const uint32_t writes = fs.writes;
    /* Idempotent, so it can run on every boot without rewriting flash. */
    CHECK_OK(kiln_program_store_seed(&port));
    CHECK_EQ_UINT(fs.writes, writes);
    CHECK_EQ_UINT(kiln_program_store_count(&port), n);

    kiln_program_t example;
    CHECK_OK(kiln_profile_example(0, &example));

    /* FR-PRG-09: neither overwritable nor deletable, so the operator always has
     * a known-good program to fall back to. */
    kiln_program_t edited = example;
    edited.segments[0].target_c = 500;
    CHECK_ERR(kiln_program_store_save(&port, &edited, 1280.0f), KILN_ERR_STATE);
    CHECK_ERR(kiln_program_store_delete(&port, example.name), KILN_ERR_STATE);
}

KILN_TEST(frprg_programs_round_trip_and_replace_by_name)
{
    kiln_host_fs_t fs;
    kiln_port_filestore_t port;
    kiln_host_fs_init(&fs);
    kiln_host_fs_bind(&fs, &port);

    const kiln_program_t p = named("my firing", 900);
    CHECK_OK(kiln_program_store_save(&port, &p, 1280.0f));

    kiln_program_t out;
    CHECK_OK(kiln_program_store_load(&port, "my firing", &out));
    CHECK_STR_EQ(out.name, "my firing");
    CHECK_EQ_UINT(out.segments[0].target_c, 900u);

    /* Saving the same name replaces rather than accumulating. */
    const kiln_program_t edited = named("my firing", 1000);
    CHECK_OK(kiln_program_store_save(&port, &edited, 1280.0f));
    CHECK_EQ_UINT(kiln_program_store_count(&port), 1u);
    CHECK_OK(kiln_program_store_load(&port, "my firing", &out));
    CHECK_EQ_UINT(out.segments[0].target_c, 1000u);

    CHECK_OK(kiln_program_store_delete(&port, "my firing"));
    CHECK_ERR(kiln_program_store_load(&port, "my firing", &out), KILN_ERR_NOT_FOUND);
    CHECK_ERR(kiln_program_store_delete(&port, "my firing"), KILN_ERR_NOT_FOUND);
}

KILN_TEST(frprg05_an_invalid_program_never_reaches_storage)
{
    kiln_host_fs_t fs;
    kiln_port_filestore_t port;
    kiln_host_fs_init(&fs);
    kiln_host_fs_bind(&fs, &port);

    kiln_program_t bad = named("too hot", 1340);
    CHECK_ERR(kiln_program_store_save(&port, &bad, 1280.0f), KILN_ERR_RANGE);
    CHECK_EQ_UINT(kiln_program_store_count(&port), 0u);

    bad = named("", 900);
    CHECK_ERR(kiln_program_store_save(&port, &bad, 1280.0f), KILN_ERR_RANGE);
    CHECK_EQ_UINT(fs.writes, 0u);
}

KILN_TEST(frprg04_the_store_holds_twenty_programs_and_then_says_no)
{
    kiln_host_fs_t fs;
    kiln_port_filestore_t port;
    kiln_host_fs_init(&fs);
    kiln_host_fs_bind(&fs, &port);

    char name[KILN_PROGRAM_NAME_LEN];
    for (unsigned i = 0; i < KILN_PROGRAM_SLOTS; i++) {
        (void)snprintf(name, sizeof(name), "program %u", i);
        const kiln_program_t p = named(name, 900);
        CHECK_OK(kiln_program_store_save(&port, &p, 1280.0f));
    }
    CHECK_EQ_UINT(kiln_program_store_count(&port), KILN_PROGRAM_SLOTS);
    CHECK_EQ_UINT(KILN_PROGRAM_SLOTS, KILN_MAX_PROGRAMS);

    const kiln_program_t one_too_many = named("overflow", 900);
    CHECK_ERR(kiln_program_store_save(&port, &one_too_many, 1280.0f), KILN_ERR_NO_SPACE);

    /* But replacing an existing one still works when full, which is the case a
     * naive "is there a free slot" check gets wrong. */
    const kiln_program_t replace = named("program 3", 950);
    CHECK_OK(kiln_program_store_save(&port, &replace, 1280.0f));
}

KILN_TEST(nfr19_a_stored_program_is_treated_as_untrusted_on_the_way_back_in)
{
    kiln_host_fs_t fs;
    kiln_port_filestore_t port;
    kiln_host_fs_init(&fs);
    kiln_host_fs_bind(&fs, &port);

    const kiln_program_t p = named("ok", 900);
    CHECK_OK(kiln_program_store_save(&port, &p, 1280.0f));

    /* Scribble over the stored name so it has no terminator, then fix the CRC so
     * it passes -- which is what a firmware with a different struct layout, or a
     * sufficiently unlucky corruption, looks like. */
    CHECK(fs.files[0].used);
    uint8_t *blob = fs.files[0].data;
    memset(&blob[8], 'A', KILN_PROGRAM_NAME_LEN);
    const size_t n = fs.files[0].len;
    const uint16_t crc = kiln_crc16(blob, n - 2);
    blob[n - 2] = (uint8_t)crc;
    blob[n - 1] = (uint8_t)(crc >> 8u);

    kiln_program_t out;
    CHECK_OK(kiln_program_store_get_slot(&port, 0, &out));
    /* Terminated on the way in, so nothing downstream reads off the end. */
    CHECK_EQ_UINT(strlen(out.name), KILN_PROGRAM_NAME_LEN - 1u);
}

KILN_TEST(a_corrupt_program_file_is_skipped_not_returned)
{
    kiln_host_fs_t fs;
    kiln_port_filestore_t port;
    kiln_host_fs_init(&fs);
    kiln_host_fs_bind(&fs, &port);

    const kiln_program_t a = named("good", 900);
    CHECK_OK(kiln_program_store_save(&port, &a, 1280.0f));
    const kiln_program_t b = named("also good", 800);
    CHECK_OK(kiln_program_store_save(&port, &b, 1280.0f));

    fs.files[0].data[20] ^= 0xFFu;        /* corrupt the first */

    CHECK_EQ_UINT(kiln_program_store_count(&port), 1u);
    kiln_program_t out;
    CHECK_ERR(kiln_program_store_load(&port, "good", &out), KILN_ERR_NOT_FOUND);
    CHECK_OK(kiln_program_store_load(&port, "also good", &out));
}

/* --- run records (FR-RUN-07, FR-LOG-09) -------------------------------- */

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

KILN_TEST(frrun07_a_run_record_round_trips_with_everything_the_requirement_lists)
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
    CHECK_NEAR(out.current_ref_a, 29.5f, 0.001f);      /* FR-CUR-08 reference */
    CHECK_EQ_UINT(out.band_duty_s[5], 1234u);          /* SR-12 bands */
    CHECK_EQ_UINT(out.contactor_ops, 777u);            /* FR-CUR-13 */
    CHECK_EQ_UINT(out.start_wall_utc_s, 1767225600ull);
    CHECK_NEAR(out.peak_c, 901.0f, 0.001f);
}

KILN_TEST(frlog09_twenty_runs_are_retained_and_the_oldest_is_evicted)
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
    CHECK(KILN_RUN_SLOTS >= 20u);              /* FR-LOG-09's floor */

    /* The 21st evicts run 1 and nothing else. */
    const kiln_run_record_t extra = run(KILN_RUN_SLOTS + 1u, KILN_END_COMPLETE);
    CHECK_OK(kiln_run_index_append(&port, &extra));
    CHECK_EQ_UINT(kiln_run_index_count(&port), KILN_RUN_SLOTS);

    kiln_run_record_t out;
    CHECK_ERR(kiln_run_index_find(&port, 1, &out), KILN_ERR_NOT_FOUND);
    CHECK_OK(kiln_run_index_find(&port, 2, &out));
    CHECK_OK(kiln_run_index_find(&port, KILN_RUN_SLOTS + 1u, &out));
}

KILN_TEST(frlog09_a_run_whose_samples_are_gone_is_marked_truncated)
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

KILN_TEST(sr12_the_baseline_comes_from_the_stored_run_history)
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
    /* The property atomic-by-rename buys, and the one a plain write loses: half
     * a program on disk is worse than the old program. */
    kiln_host_fs_t fs;
    kiln_port_filestore_t port;
    kiln_host_fs_init(&fs);
    kiln_host_fs_bind(&fs, &port);

    const kiln_program_t a = named("keeper", 900);
    CHECK_OK(kiln_program_store_save(&port, &a, 1280.0f));

    fs.cut_power_at_write = fs.writes + 1u;
    const kiln_program_t b = named("keeper", 1000);
    CHECK_ERR(kiln_program_store_save(&port, &b, 1280.0f), KILN_ERR_IO);

    fs.powered = true;
    kiln_program_t out;
    CHECK_OK(kiln_program_store_load(&port, "keeper", &out));
    CHECK_EQ_UINT(out.segments[0].target_c, 900u);    /* the old one, intact */
}

KILN_TEST(the_stores_validate_their_arguments)
{
    kiln_host_fs_t fs;
    kiln_port_filestore_t port;
    kiln_host_fs_init(&fs);
    kiln_host_fs_bind(&fs, &port);

    kiln_program_t p;
    kiln_run_record_t r;
    CHECK_ERR(kiln_program_store_seed(NULL), KILN_ERR_INVALID_ARG);
    CHECK_ERR(kiln_program_store_save(&port, NULL, 1280.0f), KILN_ERR_INVALID_ARG);
    CHECK_ERR(kiln_program_store_load(&port, NULL, &p), KILN_ERR_INVALID_ARG);
    CHECK_ERR(kiln_program_store_get_slot(&port, 200, &p), KILN_ERR_INVALID_ARG);
    CHECK_EQ_UINT(kiln_program_store_count(NULL), 0u);

    CHECK_ERR(kiln_run_index_append(&port, NULL), KILN_ERR_INVALID_ARG);
    CHECK_ERR(kiln_run_index_get_slot(&port, 200, &r), KILN_ERR_INVALID_ARG);
    CHECK_ERR(kiln_run_index_find(NULL, 1, &r), KILN_ERR_INVALID_ARG);

    CHECK_ERR(kiln_settings_load(NULL, NULL), KILN_ERR_INVALID_ARG);
    CHECK_ERR(kiln_settings_save(NULL, NULL), KILN_ERR_INVALID_ARG);
}

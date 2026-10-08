/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * program_store and run_index driven over the real flash-backed file store
 * rather than a RAM fake -- SWA-10, SWR-PRG-04, SWR-PRG-09, SWR-LOG-09, SWR-RUN-08.
 *
 * test_stores covers what the two stores decide.  This covers the stack they
 * now sit on: the same calls, over kiln_fileslots, over a flash fake that
 * enforces NOR semantics and can lose power in the middle of a write.  It is
 * the combination that runs on the board, and the reason M1 was blocked.
 */

#include <string.h>
#include "kiln_check.h"
#include "kiln_app/program_store.h"
#include "kiln_app/run_index.h"
#include "kiln_core/fileslots.h"
#include "kiln_core/profile.h"
#include "kiln_hal_host/hal_host.h"

constexpr size_t SECTOR_BYTES = 4096u;
constexpr size_t TEST_REGIONS = 64u;  /* the real kilnfs geometry: 512K / 8K */

typedef struct {
    uint8_t               storage[TEST_REGIONS * 2u * SECTOR_BYTES];
    kiln_host_flash_t     flash;
    kiln_port_flash_t     port;
    kiln_fileslots_t      fs;
    kiln_port_filestore_t store;
} rig_t;

static rig_t g_rig;

static void boot(rig_t *r)
{
    CHECK_OK(kiln_fileslots_mount(&r->fs, &r->port));
    kiln_fileslots_bind(&r->fs, &r->store);
}

static void power_up_fresh(rig_t *r)
{
    memset(r, 0, sizeof(*r));
    kiln_host_flash_init(&r->flash, r->storage, sizeof(r->storage), SECTOR_BYTES);
    kiln_host_flash_bind(&r->flash, &r->port);
    boot(r);
}

static kiln_program_t named(const char *name, uint16_t top_c)
{
    kiln_program_t p;
    kiln_profile_init_empty(&p, name);
    p.segment_count            = 1;
    p.segments[0].target_c     = top_c;
    p.segments[0].rate_c_per_h = 120;
    p.segments[0].dwell_min    = 10;
    return p;
}

/*
 * @relation(SWA-10, scope=function)
 */
KILN_TEST(swa10_the_seeded_examples_survive_a_reboot_on_flash)
{
    rig_t *r = &g_rig;
    power_up_fresh(r);

    /* This is the call that produced nothing on a real board before the store
     * existed, which is why the display offered an empty list (M1). */
    CHECK_OK(kiln_program_store_seed(&r->store));
    const uint8_t seeded = kiln_program_store_count(&r->store);
    CHECK(seeded > 0u);

    boot(r);    /* reboot */
    CHECK_EQ_UINT(kiln_program_store_count(&r->store), seeded);

    /* Seeding is idempotent (SWR-PRG-09), including across the reboot. */
    CHECK_OK(kiln_program_store_seed(&r->store));
    CHECK_EQ_UINT(kiln_program_store_count(&r->store), seeded);
}

/*
 * @relation(SWR-PRG-04, scope=function)
 */
KILN_TEST(swr_prg_04_a_saved_program_reloads_byte_for_byte_after_a_reboot)
{
    rig_t *r = &g_rig;
    power_up_fresh(r);

    const kiln_program_t p = named("bisque", 1000);
    CHECK_OK(kiln_program_store_save(&r->store, &p, 1300.0f));

    boot(r);

    kiln_program_t back = {};
    CHECK_OK(kiln_program_store_load(&r->store, "bisque", &back));
    CHECK_STR_EQ(back.name, "bisque");
    CHECK(memcmp(&back, &p, sizeof(p)) == 0);
}

/*
 * @relation(SWR-PRG-04, scope=function)
 */
KILN_TEST(swr_prg_04_the_store_fills_to_its_slot_count_and_then_refuses)
{
    rig_t *r = &g_rig;
    power_up_fresh(r);

    char name[KILN_PROGRAM_NAME_LEN];
    unsigned saved = 0;
    for (unsigned i = 0; i < KILN_PROGRAM_SLOTS + 4u; i++) {
        (void)snprintf(name, sizeof(name), "p%02u", i);
        const kiln_program_t p = named(name, 900);
        if (kiln_program_store_save(&r->store, &p, 1300.0f) == KILN_OK) {
            saved++;
        }
    }
    /* The slot count is the limit, not the region count: 64 regions are there
     * to be shared with the run records. */
    CHECK_EQ_UINT(saved, KILN_PROGRAM_SLOTS);
    CHECK_EQ_UINT(kiln_program_store_count(&r->store), KILN_PROGRAM_SLOTS);

    boot(r);
    CHECK_EQ_UINT(kiln_program_store_count(&r->store), KILN_PROGRAM_SLOTS);
}

/*
 * @relation(SWR-LOG-09, scope=function)
 */
KILN_TEST(swr_log_09_run_records_ring_and_survive_a_reboot_on_flash)
{
    rig_t *r = &g_rig;
    power_up_fresh(r);

    for (unsigned i = 1; i <= KILN_RUN_SLOTS + 3u; i++) {
        kiln_run_record_t rec = {};
        rec.run_id     = i;
        rec.duration_s = i * 60u;
        rec.peak_c     = 900.0f + (float)i;
        rec.program    = named("ring", 900);
        CHECK_OK(kiln_run_index_append(&r->store, &rec));
    }

    /* Full, with the oldest evicted rather than the newest refused. */
    CHECK_EQ_UINT(kiln_run_index_count(&r->store), KILN_RUN_SLOTS);

    boot(r);
    CHECK_EQ_UINT(kiln_run_index_count(&r->store), KILN_RUN_SLOTS);
    CHECK_EQ_UINT(kiln_run_index_next_run_id(&r->store), KILN_RUN_SLOTS + 4u);

    kiln_run_record_t got = {};
    CHECK_OK(kiln_run_index_find(&r->store, KILN_RUN_SLOTS + 3u, &got));
    CHECK_EQ_UINT(got.duration_s, (KILN_RUN_SLOTS + 3u) * 60u);
}

/*
 * @relation(SWR-PRG-04, scope=function)
 */
KILN_TEST(swr_prg_04_twenty_programs_and_twenty_runs_share_the_partition)
{
    rig_t *r = &g_rig;
    power_up_fresh(r);

    char name[KILN_PROGRAM_NAME_LEN];
    for (unsigned i = 0; i < KILN_PROGRAM_SLOTS; i++) {
        (void)snprintf(name, sizeof(name), "p%02u", i);
        const kiln_program_t p = named(name, 900);
        CHECK_OK(kiln_program_store_save(&r->store, &p, 1300.0f));
    }
    for (unsigned i = 1; i <= KILN_RUN_SLOTS; i++) {
        kiln_run_record_t rec = {};
        rec.run_id  = i;
        rec.program = named("r", 900);
        CHECK_OK(kiln_run_index_append(&r->store, &rec));
    }

    CHECK_EQ_UINT(kiln_fileslots_used_regions(&r->fs),
                  KILN_PROGRAM_SLOTS + KILN_RUN_SLOTS);

    /* 40 of 64, so the partition is sized with headroom and not to the inch. */
    CHECK(kiln_fileslots_used_regions(&r->fs) < TEST_REGIONS);

    boot(r);
    CHECK_EQ_UINT(kiln_program_store_count(&r->store), KILN_PROGRAM_SLOTS);
    CHECK_EQ_UINT(kiln_run_index_count(&r->store), KILN_RUN_SLOTS);
}

/*
 * @relation(SWR-RUN-08, scope=function)
 */
KILN_TEST(swr_run_08_a_power_cut_while_saving_a_program_keeps_the_old_one)
{
    rig_t *r = &g_rig;
    power_up_fresh(r);

    const kiln_program_t first = named("glaze", 1200);
    CHECK_OK(kiln_program_store_save(&r->store, &first, 1300.0f));

    /* Overwrite the same name, and lose power inside the payload. */
    const kiln_program_t second = named("glaze", 1250);
    r->flash.cut_power_at_write = r->flash.writes + 2u;
    r->flash.cut_bytes          = 16u;
    (void)kiln_program_store_save(&r->store, &second, 1300.0f);

    kiln_host_flash_power_on(&r->flash);
    boot(r);

    /* Neither the old program nor the slot is gone, and the half-written one is
     * nowhere: a kiln that loses power mid-save still has its firing program. */
    kiln_program_t back = {};
    CHECK_OK(kiln_program_store_load(&r->store, "glaze", &back));
    CHECK_EQ_UINT(back.segments[0].target_c, 1200u);
    CHECK_EQ_UINT(kiln_program_store_count(&r->store), 1u);
}

/*
 * @relation(SWR-RUN-08, scope=function)
 */
KILN_TEST(swr_run_08_a_power_cut_while_appending_a_run_keeps_the_earlier_runs)
{
    rig_t *r = &g_rig;
    power_up_fresh(r);

    for (unsigned i = 1; i <= 3u; i++) {
        kiln_run_record_t rec = {};
        rec.run_id     = i;
        rec.duration_s = i;
        rec.program    = named("keep", 900);
        CHECK_OK(kiln_run_index_append(&r->store, &rec));
    }

    kiln_run_record_t doomed = {};
    doomed.run_id  = 4;
    doomed.program = named("lost", 900);
    r->flash.cut_power_at_write = r->flash.writes + 3u;
    r->flash.cut_bytes          = 6u;
    (void)kiln_run_index_append(&r->store, &doomed);

    kiln_host_flash_power_on(&r->flash);
    boot(r);

    CHECK_EQ_UINT(kiln_run_index_count(&r->store), 3u);
    CHECK_ERR(kiln_run_index_find(&r->store, 4u, &doomed), KILN_ERR_NOT_FOUND);

    /* And the next run gets the identifier the lost one never committed. */
    CHECK_EQ_UINT(kiln_run_index_next_run_id(&r->store), 4u);
}

KILN_TEST(fr_cfg_12_a_format_clears_programs_and_runs_together)
{
    rig_t *r = &g_rig;
    power_up_fresh(r);

    CHECK_OK(kiln_program_store_seed(&r->store));
    kiln_run_record_t rec = {};
    rec.run_id  = 1;
    rec.program = named("x", 900);
    CHECK_OK(kiln_run_index_append(&r->store, &rec));

    CHECK_OK(kiln_fileslots_format(&r->fs));
    CHECK_EQ_UINT(kiln_program_store_count(&r->store), 0u);
    CHECK_EQ_UINT(kiln_run_index_count(&r->store), 0u);

    boot(r);
    CHECK_EQ_UINT(kiln_program_store_count(&r->store), 0u);

    /* And seeding brings the examples back, so a reset is recoverable. */
    CHECK_OK(kiln_program_store_seed(&r->store));
    CHECK(kiln_program_store_count(&r->store) > 0u);
}

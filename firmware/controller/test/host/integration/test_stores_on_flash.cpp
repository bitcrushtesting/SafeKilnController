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

/* The three tests that stood here saved programs to this flash, reloaded them
 * byte for byte after a reboot, and filled twenty slots before refusing the
 * twenty-first.  They went with the program store: the profiles are compiled
 * into the image now, so a program cannot be lost by a medium it was never on.
 *
 * What the partition still holds is run records, and every property they
 * relied on -- atomicity across a power cut, the ring, the identifier
 * sequence, surviving a reboot -- is exercised below over the same flash fake.
 */

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
 * @relation(SWR-LOG-09, scope=function)
 */
KILN_TEST(swr_log_09_the_run_records_are_now_the_only_thing_on_the_partition)
{
    /* This test used to fill twenty program slots and twenty run slots and
     * check that 40 of 64 regions were in use with headroom to spare.  With
     * the programs in the image, the partition holds twenty regions of the
     * sixty-four: it is now oversized by a factor of three rather than by half,
     * which is recorded rather than acted on, because a 512 kB partition that
     * is a third used costs nothing and shrinking it is a flash layout change
     * that breaks every device already flashed. */
    rig_t *r = &g_rig;
    power_up_fresh(r);

    for (unsigned i = 1; i <= KILN_RUN_SLOTS; i++) {
        kiln_run_record_t rec = {};
        rec.run_id  = i;
        rec.program = named("r", 900);
        CHECK_OK(kiln_run_index_append(&r->store, &rec));
    }

    CHECK_EQ_UINT(kiln_fileslots_used_regions(&r->fs), KILN_RUN_SLOTS);
    CHECK(kiln_fileslots_used_regions(&r->fs) * 3u <= TEST_REGIONS);

    boot(r);
    CHECK_EQ_UINT(kiln_run_index_count(&r->store), KILN_RUN_SLOTS);
}

/*
 * @relation(SWR-RUN-08, scope=function)
 */
KILN_TEST(swr_run_08_a_power_cut_while_rewriting_a_run_keeps_the_old_one)
{
    /* The same property the program save used to prove, over the file that is
     * still written at run time: a rewrite of an existing name that loses
     * power inside the payload leaves the previous content, not half of each.
     *
     * run_index rewrites a slot when it marks a run truncated, so this is not
     * a hypothetical path. */
    rig_t *r = &g_rig;
    power_up_fresh(r);

    kiln_run_record_t first = {};
    first.run_id     = 7;
    first.duration_s = 3600;
    first.program    = named("glaze", 1200);
    CHECK_OK(kiln_run_index_append(&r->store, &first));

    kiln_run_record_t edited = first;
    edited.duration_s = 9999;
    r->flash.cut_power_at_write = r->flash.writes + 2u;
    r->flash.cut_bytes          = 16u;
    (void)kiln_run_index_append(&r->store, &edited);

    kiln_host_flash_power_on(&r->flash);
    boot(r);

    kiln_run_record_t back = {};
    CHECK_OK(kiln_run_index_find(&r->store, 7u, &back));
    CHECK_EQ_UINT(back.duration_s, 3600u);
    CHECK_EQ_UINT(kiln_run_index_count(&r->store), 1u);
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

KILN_TEST(fr_cfg_12_a_format_clears_the_run_history)
{
    rig_t *r = &g_rig;
    power_up_fresh(r);

    kiln_run_record_t rec = {};
    rec.run_id  = 1;
    rec.program = named("x", 900);
    CHECK_OK(kiln_run_index_append(&r->store, &rec));

    CHECK_OK(kiln_fileslots_format(&r->fs));
    CHECK_EQ_UINT(kiln_run_index_count(&r->store), 0u);

    boot(r);
    CHECK_EQ_UINT(kiln_run_index_count(&r->store), 0u);

    /* A factory reset no longer costs the operator their programs, which is
     * the other half of compiling them in: the only thing on this partition is
     * history, and losing history is what a factory reset is for.  The next
     * run also starts from 1 again, since the sequence is read from here. */
    CHECK_EQ_UINT(kiln_run_index_next_run_id(&r->store), 1u);
}

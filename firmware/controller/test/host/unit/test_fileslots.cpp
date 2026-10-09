/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The named blob store behind kiln_port_filestore -- SWA-10, SWR-PRG-04,
 * SWR-LOG-09, SWR-RUN-08.
 *
 * Driven through the flash fake, which enforces NOR semantics and can cut power
 * part-way through a write.  That is the whole reason this store exists in the
 * core rather than behind a filesystem: the claim is that no power cut can lose
 * a file that was already there, and here the test can actually cut the power.
 */

#include <stdio.h>
#include <string.h>
#include "kiln_check.h"
#include "kiln_core/fileslots.h"
#include "kiln_hal_host/hal_host.h"
/* For the blob size the one real caller stores, in the mount-cost tests at the
 * end of this file: a cost measured against made-up payloads measures nothing. */
#include "kiln_app/run_index.h"

constexpr size_t SECTOR_BYTES = 4096u;
/* 20 programs plus 20 run records, plus two spare to prove the store refuses a
 * file when it is genuinely full rather than when it is merely busy. */
constexpr size_t TEST_REGIONS = 42u;

typedef struct {
    uint8_t             storage[TEST_REGIONS * 2u * SECTOR_BYTES];
    kiln_host_flash_t   flash;
    kiln_port_flash_t   port;
    kiln_fileslots_t    fs;
    kiln_port_filestore_t store;
} rig_t;

static rig_t g_rig;

static void rig_init(rig_t *r)
{
    memset(r, 0, sizeof(*r));
    kiln_host_flash_init(&r->flash, r->storage, sizeof(r->storage), SECTOR_BYTES);
    kiln_host_flash_bind(&r->flash, &r->port);
    CHECK_OK(kiln_fileslots_mount(&r->fs, &r->port));
    kiln_fileslots_bind(&r->fs, &r->store);
}

/* Remount the same medium, exactly as a reboot would. */
static void rig_remount(rig_t *r)
{
    CHECK_OK(kiln_fileslots_mount(&r->fs, &r->port));
    kiln_fileslots_bind(&r->fs, &r->store);
}

/* The store hands back bytes; these fixtures wrote NUL-terminated text into
 * them, so the comparisons read them as a string.  One named conversion rather
 * than a cast at each call site. */
static const char *as_str(const void *b)
{
    return static_cast<const char *>(b);
}

static kiln_err_t put(rig_t *r, const char *path, const void *d, size_t n)
{
    return r->store.write_atomic(r->store.ctx, path, d, n);
}

static kiln_err_t get(rig_t *r, const char *path, void *out, size_t cap, size_t *n)
{
    return r->store.read(r->store.ctx, path, out, cap, n);
}

/*
 * @relation(SWA-10, scope=function)
 */
KILN_TEST(swa10_an_empty_medium_mounts_with_nothing_in_it)
{
    rig_t *r = &g_rig;
    rig_init(r);

    CHECK_EQ_UINT(kiln_fileslots_used_regions(&r->fs), 0u);
    CHECK_EQ_UINT(r->fs.region_count, TEST_REGIONS);

    uint8_t buf[16];
    CHECK_ERR(get(r, "/p/00", buf, sizeof(buf), nullptr), KILN_ERR_NOT_FOUND);
}

/*
 * @relation(SWA-10, scope=function)
 */
KILN_TEST(swa10_a_file_reads_back_and_survives_a_remount)
{
    rig_t *r = &g_rig;
    rig_init(r);

    const char payload[] = "cone 6 glaze";
    CHECK_OK(put(r, "/p/03", payload, sizeof(payload)));

    uint8_t  buf[64] = {};
    size_t   n       = 0;
    CHECK_OK(get(r, "/p/03", buf, sizeof(buf), &n));
    CHECK_EQ_UINT(n, sizeof(payload));
    CHECK_STR_EQ(as_str(buf), payload);

    rig_remount(r);

    memset(buf, 0, sizeof(buf));
    CHECK_OK(get(r, "/p/03", buf, sizeof(buf), &n));
    CHECK_STR_EQ(as_str(buf), payload);
    CHECK_EQ_UINT(kiln_fileslots_used_regions(&r->fs), 1u);
}

/*
 * @relation(SWA-10, scope=function)
 */
KILN_TEST(swa10_rewriting_alternates_the_two_copies_of_the_region)
{
    rig_t *r = &g_rig;
    rig_init(r);

    /* The copy being replaced is never the copy being erased, which is the only
     * reason a cut cannot take both. */
    CHECK_OK(put(r, "/p/00", "a", 2));
    CHECK_EQ_UINT(r->fs.entry[0].copy, 0u);
    CHECK_EQ_UINT(r->fs.entry[0].seq, 1u);

    CHECK_OK(put(r, "/p/00", "b", 2));
    CHECK_EQ_UINT(r->fs.entry[0].copy, 1u);
    CHECK_EQ_UINT(r->fs.entry[0].seq, 2u);

    CHECK_OK(put(r, "/p/00", "c", 2));
    CHECK_EQ_UINT(r->fs.entry[0].copy, 0u);
    CHECK_EQ_UINT(r->fs.entry[0].seq, 3u);

    /* Both copies are valid now; the mount has to pick the higher seq. */
    rig_remount(r);
    uint8_t buf[8] = {};
    CHECK_OK(get(r, "/p/00", buf, sizeof(buf), nullptr));
    CHECK_STR_EQ(as_str(buf), "c");
}

/*
 * @relation(SWR-RUN-08, scope=function)
 */
KILN_TEST(swr_run_08_a_cut_before_the_commit_leaves_the_previous_copy)
{
    rig_t *r = &g_rig;
    rig_init(r);

    CHECK_OK(put(r, "/r/00", "first", 6));

    /* Writes per update: the name, then the payload, then the commit header.
     * Cut inside the payload, so the new copy never gets a magic. */
    r->flash.cut_power_at_write = r->flash.writes + 2u;
    r->flash.cut_bytes          = 2u;
    (void)put(r, "/r/00", "second", 7);

    kiln_host_flash_power_on(&r->flash);
    rig_remount(r);

    uint8_t buf[16] = {};
    CHECK_OK(get(r, "/r/00", buf, sizeof(buf), nullptr));
    CHECK_STR_EQ(as_str(buf), "first");
}

/*
 * @relation(SWR-RUN-08, scope=function)
 */
KILN_TEST(swr_run_08_a_cut_inside_the_commit_header_leaves_the_previous_copy)
{
    rig_t *r = &g_rig;
    rig_init(r);

    CHECK_OK(put(r, "/r/01", "keep", 5));

    /* Four bytes in: the magic has landed and the sequence number has not, so
     * the copy looks committed to anything that only checks the magic. */
    r->flash.cut_power_at_write = r->flash.writes + 3u;
    r->flash.cut_bytes          = 4u;
    (void)put(r, "/r/01", "lose", 5);

    kiln_host_flash_power_on(&r->flash);
    rig_remount(r);

    uint8_t buf[16] = {};
    CHECK_OK(get(r, "/r/01", buf, sizeof(buf), nullptr));
    CHECK_STR_EQ(as_str(buf), "keep");
}

/*
 * @relation(SWR-RUN-08, scope=function)
 */
KILN_TEST(swr_run_08_a_cut_with_only_the_crc_missing_leaves_the_previous_copy)
{
    rig_t *r = &g_rig;
    rig_init(r);

    CHECK_OK(put(r, "/r/02", "keep", 5));

    /* Ten bytes in: magic, sequence and length are all down and only the CRC is
     * missing.  Nothing but the CRC can reject this one. */
    r->flash.cut_power_at_write = r->flash.writes + 3u;
    r->flash.cut_bytes          = 10u;
    (void)put(r, "/r/02", "lose", 5);

    kiln_host_flash_power_on(&r->flash);
    rig_remount(r);

    uint8_t buf[16] = {};
    CHECK_OK(get(r, "/r/02", buf, sizeof(buf), nullptr));
    CHECK_STR_EQ(as_str(buf), "keep");
}

/*
 * @relation(SWR-RUN-08, scope=function)
 */
KILN_TEST(swr_run_08_a_cut_on_a_first_write_leaves_no_file_rather_than_half_of_one)
{
    rig_t *r = &g_rig;
    rig_init(r);

    r->flash.cut_power_at_write = r->flash.writes + 2u;
    r->flash.cut_bytes          = 2u;
    (void)put(r, "/p/07", "half", 5);

    kiln_host_flash_power_on(&r->flash);
    rig_remount(r);

    uint8_t buf[16];
    CHECK_ERR(get(r, "/p/07", buf, sizeof(buf), nullptr), KILN_ERR_NOT_FOUND);
    CHECK_EQ_UINT(kiln_fileslots_used_regions(&r->fs), 0u);
}

/*
 * @relation(SWR-PRG-04, scope=function)
 */
KILN_TEST(swr_prg_04_twenty_programs_and_twenty_run_records_coexist)
{
    rig_t *r = &g_rig;
    rig_init(r);

    char path[KILN_PATH_MAX];
    for (unsigned i = 0; i < 20u; i++) {
        (void)snprintf(path, sizeof(path), "/p/%02u", i);
        CHECK_OK(put(r, path, &i, sizeof(i)));
        (void)snprintf(path, sizeof(path), "/r/%02u", i);
        CHECK_OK(put(r, path, &i, sizeof(i)));
    }
    CHECK_EQ_UINT(kiln_fileslots_used_regions(&r->fs), 40u);

    rig_remount(r);
    CHECK_EQ_UINT(kiln_fileslots_used_regions(&r->fs), 40u);

    for (unsigned i = 0; i < 20u; i++) {
        unsigned got = 0;
        (void)snprintf(path, sizeof(path), "/p/%02u", i);
        CHECK_OK(get(r, path, &got, sizeof(got), nullptr));
        CHECK_EQ_UINT(got, i);
    }
}

/*
 * @relation(SWA-10, scope=function)
 */
KILN_TEST(swa10_a_full_medium_refuses_a_new_file_and_still_takes_a_rewrite)
{
    rig_t *r = &g_rig;
    rig_init(r);

    char path[KILN_PATH_MAX];
    for (unsigned i = 0; i < TEST_REGIONS; i++) {
        (void)snprintf(path, sizeof(path), "/p/%02u", i);
        CHECK_OK(put(r, path, "x", 2));
    }
    CHECK_ERR(put(r, "/p/99", "x", 2), KILN_ERR_NO_SPACE);

    /* A rewrite needs no new region, so a full store must still accept one. */
    CHECK_OK(put(r, "/p/00", "y", 2));
}

/*
 * @relation(SWA-10, scope=function)
 */
KILN_TEST(swa10_remove_frees_the_region_for_another_name)
{
    rig_t *r = &g_rig;
    rig_init(r);

    uint8_t buf[16];
    CHECK_OK(put(r, "/p/00", "gone", 5));
    CHECK_OK(get(r, "/p/00", buf, sizeof(buf), nullptr));
    CHECK_OK(r->store.remove(r->store.ctx, "/p/00"));

    CHECK_ERR(get(r, "/p/00", buf, sizeof(buf), nullptr), KILN_ERR_NOT_FOUND);
    CHECK_EQ_UINT(kiln_fileslots_used_regions(&r->fs), 0u);
    CHECK_ERR(r->store.remove(r->store.ctx, "/p/00"), KILN_ERR_NOT_FOUND);

    rig_remount(r);
    CHECK_EQ_UINT(kiln_fileslots_used_regions(&r->fs), 0u);
}

/*
 * @relation(SWA-10, scope=function)
 */
KILN_TEST(swa10_a_payload_too_large_for_a_sector_is_refused)
{
    rig_t *r = &g_rig;
    rig_init(r);

    static uint8_t big[SECTOR_BYTES];
    memset(big, 0x5A, sizeof(big));
    CHECK_ERR(put(r, "/p/00", big, sizeof(big)), KILN_ERR_RANGE);

    /* Exactly the maximum is accepted, so the boundary is where it is claimed. */
    CHECK_OK(put(r, "/p/00", big, r->fs.payload_max));
    size_t n = 0;
    static uint8_t back[SECTOR_BYTES];
    CHECK_OK(get(r, "/p/00", back, sizeof(back), &n));
    CHECK_EQ_UINT(n, r->fs.payload_max);
    CHECK(memcmp(back, big, r->fs.payload_max) == 0);
}

/*
 * @relation(SWA-10, scope=function)
 */
KILN_TEST(swa10_a_payload_corrupted_under_the_store_is_not_served)
{
    rig_t *r = &g_rig;
    rig_init(r);

    CHECK_OK(put(r, "/p/00", "intact", 7));

    /* Clear a set bit in the payload, which is all a NOR medium can do by
     * itself: 'i' is 0x69, so bit 0 is the one with something to clear. */
    r->storage[KILN_FILESLOT_HDR] &= 0xFEu;

    uint8_t buf[16];
    CHECK_ERR(get(r, "/p/00", buf, sizeof(buf), nullptr), KILN_ERR_CORRUPT);

    /* And a mount must not adopt it either. */
    rig_remount(r);
    CHECK_ERR(get(r, "/p/00", buf, sizeof(buf), nullptr), KILN_ERR_NOT_FOUND);
}

/*
 * @relation(SWA-10, scope=function)
 */
KILN_TEST(swa10_a_short_buffer_is_refused_rather_than_truncated)
{
    rig_t *r = &g_rig;
    rig_init(r);

    CHECK_OK(put(r, "/p/00", "0123456789", 11));
    uint8_t small[4];
    CHECK_ERR(get(r, "/p/00", small, sizeof(small), nullptr), KILN_ERR_RANGE);
}

/*
 * @relation(SWA-10, scope=function)
 */
KILN_TEST(swa10_usage_reports_what_is_stored)
{
    rig_t *r = &g_rig;
    rig_init(r);

    CHECK_OK(put(r, "/p/00", "aaaa", 4));
    CHECK_OK(put(r, "/r/00", "bbbbbb", 6));

    size_t total = 0;
    size_t used  = 0;
    CHECK_OK(r->store.usage(r->store.ctx, &total, &used));
    CHECK_EQ_UINT(used, 10u);
    CHECK_EQ_UINT(total, (size_t)TEST_REGIONS * r->fs.payload_max);

    /* usage is reported from the in-RAM index, so it has to survive a reboot
     * on the strength of the mount alone. */
    rig_remount(r);
    used = 0;
    CHECK_OK(r->store.usage(r->store.ctx, nullptr, &used));
    CHECK_EQ_UINT(used, 10u);
}

/*
 * @relation(SWA-10, scope=function)
 */
KILN_TEST(swa10_format_empties_the_store)
{
    rig_t *r = &g_rig;
    rig_init(r);

    CHECK_OK(put(r, "/p/00", "x", 2));
    CHECK_OK(put(r, "/r/00", "y", 2));
    CHECK_OK(kiln_fileslots_format(&r->fs));
    CHECK_EQ_UINT(kiln_fileslots_used_regions(&r->fs), 0u);

    rig_remount(r);
    CHECK_EQ_UINT(kiln_fileslots_used_regions(&r->fs), 0u);
}

/*
 * @relation(SWA-10, scope=function)
 */
KILN_TEST(swa10_the_store_validates_its_arguments)
{
    rig_t *r = &g_rig;
    rig_init(r);

    kiln_fileslots_t fs;
    CHECK_ERR(kiln_fileslots_mount(nullptr, &r->port), KILN_ERR_INVALID_ARG);
    CHECK_ERR(kiln_fileslots_mount(&fs, nullptr), KILN_ERR_INVALID_ARG);

    uint8_t buf[8];
    CHECK_ERR(get(r, nullptr, buf, sizeof(buf), nullptr), KILN_ERR_INVALID_ARG);
    CHECK_ERR(get(r, "", buf, sizeof(buf), nullptr), KILN_ERR_INVALID_ARG);
    CHECK_ERR(get(r, "/p/00", nullptr, sizeof(buf), nullptr), KILN_ERR_INVALID_ARG);
    CHECK_ERR(put(r, "/p/00", nullptr, 4), KILN_ERR_INVALID_ARG);

    /* A name that fills its field has nowhere to put the terminator. */
    char longname[KILN_PATH_MAX + 8];
    memset(longname, 'n', sizeof(longname));
    longname[sizeof(longname) - 1] = '\0';
    CHECK_ERR(put(r, longname, "x", 2), KILN_ERR_INVALID_ARG);

    /* An unmounted store answers rather than following a null flash pointer. */
    kiln_fileslots_t cold = {};
    kiln_port_filestore_t cold_port;
    kiln_fileslots_bind(&cold, &cold_port);
    CHECK_ERR(cold_port.read(cold_port.ctx, "/p/00", buf, sizeof(buf), nullptr),
              KILN_ERR_INVALID_ARG);
    CHECK_ERR(kiln_fileslots_format(&cold), KILN_ERR_STATE);
}

/* ===========================================================================
 * WEAR  (tasklist Q1)
 * ===========================================================================
 * A NOR sector at the end of its life takes the write, reports success, and
 * keeps something other than what it was given. Every test above this point
 * drives a medium that does what it is told, so the one failure mode the part
 * will actually exhibit was the one nothing exercised.
 *
 * The flash fake grows two knobs for it: garble_write_at spoils one write,
 * garble_every_write spoils all of them, and both return KILN_OK, because an
 * error code is exactly what this failure does not come with.
 */

/* The write counter after priming, so a test can spoil the next write rather
 * than guess which one it is. */
static void spoil_next_write(rig_t *r)
{
    r->flash.garble_write_at = r->flash.writes + 1u;
}

/*
 * @relation(SWA-21, scope=function)
 */
KILN_TEST(swa21_a_sector_that_lies_about_a_write_retires_its_region)
{
    rig_t *r = &g_rig;
    rig_init(r);

    CHECK_OK(put(r, "/r/00", "first", 6));
    CHECK_EQ_UINT(kiln_fileslots_used_regions(&r->fs), 1u);
    CHECK_EQ_UINT(kiln_fileslots_retired_regions(&r->fs), 0u);

    /* The region the file is in is about to stop holding what it is given. */
    spoil_next_write(r);
    CHECK_OK(put(r, "/r/00", "second", 7));

    /* The caller gets KILN_OK because the file IS stored -- somewhere else. */
    uint8_t buf[16];
    size_t  n = 0;
    CHECK_OK(get(r, "/r/00", buf, sizeof(buf), &n));
    CHECK_STR_EQ(as_str(buf), "second");
    CHECK_EQ_UINT(n, 7u);

    CHECK_EQ_UINT(kiln_fileslots_retired_regions(&r->fs), 1u);
    CHECK_EQ_UINT(kiln_fileslots_used_regions(&r->fs), 1u);

    /* And the retired region is not handed out again. */
    r->flash.garble_write_at = 0;
    CHECK_OK(put(r, "/r/01", "other", 6));
    CHECK_EQ_UINT(kiln_fileslots_used_regions(&r->fs), 2u);
    CHECK_EQ_UINT(kiln_fileslots_retired_regions(&r->fs), 1u);
    CHECK_OK(get(r, "/r/00", buf, sizeof(buf), nullptr));
    CHECK_STR_EQ(as_str(buf), "second");
}

/*
 * @relation(SWA-21, scope=function)
 */
KILN_TEST(swa21_a_migration_leaves_no_stale_copy_to_come_back_later)
{
    rig_t *r = &g_rig;
    rig_init(r);

    CHECK_OK(put(r, "/r/00", "first", 6));
    spoil_next_write(r);
    CHECK_OK(put(r, "/r/00", "second", 7));
    r->flash.garble_write_at = 0;

    /* The erase of the retired region succeeded, so nothing on the medium
     * claims the name twice and the region is back in the pool at the next
     * boot: a sector that can still be erased has not earned a life sentence
     * from one bad write. */
    rig_remount(r);
    CHECK_EQ_UINT(kiln_fileslots_used_regions(&r->fs), 1u);
    CHECK_EQ_UINT(kiln_fileslots_retired_regions(&r->fs), 0u);

    uint8_t buf[16];
    CHECK_OK(get(r, "/r/00", buf, sizeof(buf), nullptr));
    CHECK_STR_EQ(as_str(buf), "second");

    /* The case this erase is for: a removed file must not be resurrected by a
     * copy the migration left behind. */
    CHECK_OK(r->store.remove(r->store.ctx, "/r/00"));
    rig_remount(r);
    CHECK_ERR(get(r, "/r/00", buf, sizeof(buf), nullptr), KILN_ERR_NOT_FOUND);
    CHECK_EQ_UINT(kiln_fileslots_used_regions(&r->fs), 0u);
}

/*
 * @relation(SWA-21, scope=function)
 */
KILN_TEST(swa21_a_region_that_cannot_even_be_erased_stays_retired_across_a_reboot)
{
    rig_t *r = &g_rig;
    rig_init(r);

    CHECK_OK(put(r, "/r/00", "first", 6));

    /* Now region 0, where the file lives, is properly gone: it keeps zeros
     * instead of data, and it will not erase either, so the copy it holds
     * cannot be cleaned up after the migration. */
    spoil_next_write(r);
    r->flash.fail_erase_off = 0u;    /* region 0, copy 0 */
    CHECK_OK(put(r, "/r/00", "second", 7));
    r->flash.garble_write_at = 0;
    r->flash.fail_erase_off = KILN_HOST_NO_OFFSET;

    uint8_t buf[16];
    CHECK_OK(get(r, "/r/00", buf, sizeof(buf), nullptr));
    CHECK_STR_EQ(as_str(buf), "second");

    /* Two regions now hold /r/00. Mount resolves it by seq and retires the
     * loser, which is the region whose write failed -- so retirement outlives
     * the power cycle without a bad-block table anywhere. */
    rig_remount(r);
    CHECK_EQ_UINT(kiln_fileslots_used_regions(&r->fs), 1u);
    CHECK_EQ_UINT(kiln_fileslots_retired_regions(&r->fs), 1u);
    CHECK_OK(get(r, "/r/00", buf, sizeof(buf), nullptr));
    CHECK_STR_EQ(as_str(buf), "second");

    /* Twice, because a resolution that is not stable is not a resolution. */
    rig_remount(r);
    CHECK_EQ_UINT(kiln_fileslots_retired_regions(&r->fs), 1u);
    CHECK_OK(get(r, "/r/00", buf, sizeof(buf), nullptr));
    CHECK_STR_EQ(as_str(buf), "second");

    /* An explicit factory reset takes it back: FR-CFG-12 erases everything, and
     * refusing to try the region again would leave the operator no way to find
     * out whether the part is really finished. */
    CHECK_OK(kiln_fileslots_format(&r->fs));
    CHECK_EQ_UINT(kiln_fileslots_retired_regions(&r->fs), 0u);
}

/*
 * @relation(SWA-21, scope=function)
 */
KILN_TEST(swa21_a_part_with_nowhere_left_to_write_says_so)
{
    rig_t *r = &g_rig;
    rig_init(r);

    CHECK_OK(put(r, "/r/00", "first", 6));

    /* Every write from here keeps zeros. Three regions are tried, which is
     * KILN_FILESLOT_WRITE_TRIES, and then the store stops spending erase
     * cycles on a part that is not coming back. */
    r->flash.garble_every_write = true;
    CHECK_ERR(put(r, "/r/00", "second", 7), KILN_ERR_CORRUPT);
    r->flash.garble_every_write = false;

    CHECK_EQ_UINT(kiln_fileslots_retired_regions(&r->fs), 3u);

    /* The previous content is still there: the write that failed was the one
     * that did not happen, not the file. */
    uint8_t buf[16];
    CHECK_OK(get(r, "/r/00", buf, sizeof(buf), nullptr));
    CHECK_STR_EQ(as_str(buf), "first");
    CHECK_EQ_UINT(kiln_fileslots_used_regions(&r->fs), 1u);

    rig_remount(r);
    CHECK_OK(get(r, "/r/00", buf, sizeof(buf), nullptr));
    CHECK_STR_EQ(as_str(buf), "first");
}

/*
 * @relation(SWA-21, scope=function)
 */
KILN_TEST(swa21_a_fresh_file_migrates_too_and_the_store_can_fill_up_with_retirements)
{
    rig_t *r = &g_rig;
    rig_init(r);

    /* A file that has no home yet meets the same sector. Nothing has to be
     * preserved here, which is the easy half, but the region still has to be
     * retired or the next attempt goes straight back into it. */
    spoil_next_write(r);
    CHECK_OK(put(r, "/p/00", "program", 8));
    r->flash.garble_write_at = 0;

    uint8_t buf[16];
    CHECK_OK(get(r, "/p/00", buf, sizeof(buf), nullptr));
    CHECK_STR_EQ(as_str(buf), "program");
    CHECK_EQ_UINT(kiln_fileslots_used_regions(&r->fs), 1u);
    CHECK_EQ_UINT(kiln_fileslots_retired_regions(&r->fs), 1u);

    /* Retired regions are not capacity. Fill what is left and the store
     * reports no space rather than retrying a region it has written off. */
    for (size_t i = 1; i < TEST_REGIONS - 1u; i++) {
        char path[16];
        (void)snprintf(path, sizeof(path), "/p/%02zu", i);
        CHECK_OK(put(r, path, "x", 2));
    }
    CHECK_EQ_UINT(kiln_fileslots_used_regions(&r->fs), TEST_REGIONS - 1u);
    CHECK_ERR(put(r, "/p/99", "y", 2), KILN_ERR_NO_SPACE);
}

/* ===========================================================================
 * WHAT A MOUNT COSTS  (tasklist Q2)
 * ===========================================================================
 * Mount reads every copy of every region and streams its payload through the
 * CRC, so start-up is paid in flash reads and CRC bytes before the controller
 * does anything. That cost had been described rather than measured, and the
 * description was wrong by 60 %.
 *
 * These two tests measure it on the medium the partition actually gives us: 64
 * regions, both copies valid, which is the state normal operation leaves behind
 * because a region keeps the copy it is replacing until the next write.
 *
 * The expected numbers are DERIVED here rather than written down, so the tests
 * pin the model of the cost instead of a constant somebody would update to
 * whatever the code now does. Make mount lazy and they fail, which is the
 * point: that is a change to when start-up is paid for, and it should not pass
 * unnoticed.
 */
constexpr size_t COST_REGIONS = KILN_FILESLOT_REGIONS_MAX;   /* 64 */
constexpr size_t COST_CRC_CHUNK = 64u;    /* read_copy's streaming buffer */

typedef struct {
    uint8_t               storage[COST_REGIONS * 2u * SECTOR_BYTES];
    kiln_host_flash_t     flash;
    kiln_port_flash_t     port;
    kiln_fileslots_t      fs;
    kiln_port_filestore_t store;
} cost_rig_t;

static cost_rig_t g_cost;

/* Reads and bytes that validating one committed copy of `len` payload bytes
 * costs: the 76 byte header, then the payload in 64 byte chunks. */
static void copy_cost(size_t len, uint32_t *reads, uint32_t *bytes)
{
    *reads = 1u + (uint32_t)((len + COST_CRC_CHUNK - 1u) / COST_CRC_CHUNK);
    *bytes = KILN_FILESLOT_HDR + (uint32_t)len;
}

/* Fill every region, writing each file twice so that both copies are valid --
 * the state a running kiln leaves the medium in. */
static void cost_fill(cost_rig_t *r, size_t payload)
{
    memset(r, 0, sizeof(*r));
    kiln_host_flash_init(&r->flash, r->storage, sizeof(r->storage), SECTOR_BYTES);
    kiln_host_flash_bind(&r->flash, &r->port);
    CHECK_OK(kiln_fileslots_mount(&r->fs, &r->port));
    kiln_fileslots_bind(&r->fs, &r->store);
    CHECK_EQ_UINT(r->fs.region_count, COST_REGIONS);

    static uint8_t blob[4096];
    memset(blob, 0xA5, sizeof(blob));
    for (size_t i = 0; i < COST_REGIONS; i++) {
        char path[16];
        (void)snprintf(path, sizeof(path), "/x/%02zu", i);
        CHECK_OK(r->store.write_atomic(r->store.ctx, path, blob, payload));
        CHECK_OK(r->store.write_atomic(r->store.ctx, path, blob, payload));
    }
    CHECK_EQ_UINT(kiln_fileslots_used_regions(&r->fs), COST_REGIONS);
}

/*
 * @relation(SWA-21, scope=function)
 */
KILN_TEST(swa21_a_full_mount_costs_what_the_model_says)
{
    /* The largest payload a 4 096 byte sector can hold once the 76 byte header
     * is taken out, rounded down to a word: (4096 - 76) & ~3. */
    const size_t payload = 4020u;

    cost_rig_t *r = &g_cost;
    cost_fill(r, payload);
    CHECK_EQ_UINT(r->fs.payload_max, payload);

    r->flash.reads = 0;
    r->flash.bytes_read = 0;
    CHECK_OK(kiln_fileslots_mount(&r->fs, &r->port));

    uint32_t per_reads = 0;
    uint32_t per_bytes = 0;
    copy_cost(payload, &per_reads, &per_bytes);

    /* Two valid copies per region, both validated in full. */
    CHECK_EQ_UINT(r->flash.reads, 2u * COST_REGIONS * per_reads);
    CHECK_EQ_UINT(r->flash.bytes_read, 2u * COST_REGIONS * per_bytes);

    /* The absolute figures, so a reader of this suite does not have to do the
     * arithmetic: 8 192 reads and 524 288 bytes, which is the whole partition.
     * That is the number the architecture's start-up budget has to carry, and
     * it is 1.6 times the 320 kB the task list had guessed at. */
    CHECK_EQ_UINT(r->flash.reads, 8192u);
    CHECK_EQ_UINT(r->flash.bytes_read, 524288u);
}

/*
 * @relation(SWA-21, scope=function)
 */
KILN_TEST(swa21_the_mount_this_product_actually_pays_for)
{
    /* What the one real caller stores: 20 run records at the size run_index's
     * own serialiser produces. The worst case above is what the partition
     * permits; this is what the firmware puts in it.
     *
     * It used to be 20 programs as well, which is why the medium here is half
     * what it was: the programs are compiled into the image and the partition
     * holds history and nothing else. */
    constexpr size_t RUN_BLOB  = 8u + sizeof(kiln_run_record_t) + 2u;

    cost_rig_t *r = &g_cost;
    memset(r, 0, sizeof(*r));
    kiln_host_flash_init(&r->flash, r->storage, sizeof(r->storage), SECTOR_BYTES);
    kiln_host_flash_bind(&r->flash, &r->port);
    CHECK_OK(kiln_fileslots_mount(&r->fs, &r->port));
    kiln_fileslots_bind(&r->fs, &r->store);

    static uint8_t blob[4096];
    memset(blob, 0x5A, sizeof(blob));
    for (size_t i = 0; i < 20u; i++) {
        char path[16];
        (void)snprintf(path, sizeof(path), "/r/%02zu", i);
        CHECK_OK(r->store.write_atomic(r->store.ctx, path, blob, RUN_BLOB));
        CHECK_OK(r->store.write_atomic(r->store.ctx, path, blob, RUN_BLOB));
    }

    r->flash.reads = 0;
    r->flash.bytes_read = 0;
    CHECK_OK(kiln_fileslots_mount(&r->fs, &r->port));

    uint32_t rr = 0, rb = 0;
    copy_cost(RUN_BLOB, &rr, &rb);

    /* 20 regions hold a file; the other 44 cost one failed header read each. */
    const uint32_t want_reads = 2u * 20u * rr + (uint32_t)(COST_REGIONS - 20u) * 2u;
    const uint32_t want_bytes = 2u * 20u * rb
                              + (uint32_t)(COST_REGIONS - 20u) * 2u * KILN_FILESLOT_HDR;
    CHECK_EQ_UINT(r->flash.reads, want_reads);
    CHECK_EQ_UINT(r->flash.bytes_read, want_bytes);
    printf("    mount: %u reads, %u bytes (run blob %zu)\n",
           r->flash.reads, r->flash.bytes_read, RUN_BLOB);

    /* Measured, against 8 192 reads and 524 288 bytes for a medium full of
     * maximum-size files: a small fraction of the worst case, which is the
     * reason the worst case is a bound rather than a budget. The exact numbers
     * are asserted so that making mount lazy, or putting something else on
     * this partition, shows up here rather than on a board. */
    CHECK_EQ_UINT(r->flash.reads, 488u);
    CHECK_EQ_UINT(r->flash.bytes_read, 31888u);
}

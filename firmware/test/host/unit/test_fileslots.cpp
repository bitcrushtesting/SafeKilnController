/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The named blob store behind kiln_port_filestore -- AD-10, FR-PRG-04,
 * FR-LOG-09, FR-RUN-08.
 *
 * Driven through the flash fake, which enforces NOR semantics and can cut power
 * part-way through a write.  That is the whole reason this store exists in the
 * core rather than behind a filesystem: the claim is that no power cut can lose
 * a file that was already there, and here the test can actually cut the power.
 */

#include <string.h>
#include "kiln_check.h"
#include "kiln_core/fileslots.h"
#include "kiln_hal_host/hal_host.h"

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

KILN_TEST(ad10_an_empty_medium_mounts_with_nothing_in_it)
{
    rig_t *r = &g_rig;
    rig_init(r);

    CHECK_EQ_UINT(kiln_fileslots_used_regions(&r->fs), 0u);
    CHECK_EQ_UINT(r->fs.region_count, TEST_REGIONS);

    uint8_t buf[16];
    CHECK_ERR(get(r, "/p/00", buf, sizeof(buf), nullptr), KILN_ERR_NOT_FOUND);
}

KILN_TEST(ad10_a_file_reads_back_and_survives_a_remount)
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

KILN_TEST(ad10_rewriting_alternates_the_two_copies_of_the_region)
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

KILN_TEST(fr_run_08_a_cut_before_the_commit_leaves_the_previous_copy)
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

KILN_TEST(fr_run_08_a_cut_inside_the_commit_header_leaves_the_previous_copy)
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

KILN_TEST(fr_run_08_a_cut_with_only_the_crc_missing_leaves_the_previous_copy)
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

KILN_TEST(fr_run_08_a_cut_on_a_first_write_leaves_no_file_rather_than_half_of_one)
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

KILN_TEST(fr_prg_04_twenty_programs_and_twenty_run_records_coexist)
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

KILN_TEST(ad10_a_full_medium_refuses_a_new_file_and_still_takes_a_rewrite)
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

KILN_TEST(ad10_remove_frees_the_region_for_another_name)
{
    rig_t *r = &g_rig;
    rig_init(r);

    CHECK_OK(put(r, "/p/00", "gone", 5));
    CHECK_OK(r->store.exists(r->store.ctx, "/p/00"));
    CHECK_OK(r->store.remove(r->store.ctx, "/p/00"));

    CHECK_ERR(r->store.exists(r->store.ctx, "/p/00"), KILN_ERR_NOT_FOUND);
    CHECK_EQ_UINT(kiln_fileslots_used_regions(&r->fs), 0u);
    CHECK_ERR(r->store.remove(r->store.ctx, "/p/00"), KILN_ERR_NOT_FOUND);

    rig_remount(r);
    CHECK_EQ_UINT(kiln_fileslots_used_regions(&r->fs), 0u);
}

KILN_TEST(ad10_a_payload_too_large_for_a_sector_is_refused)
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

KILN_TEST(ad10_a_payload_corrupted_under_the_store_is_not_served)
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

KILN_TEST(ad10_a_short_buffer_is_refused_rather_than_truncated)
{
    rig_t *r = &g_rig;
    rig_init(r);

    CHECK_OK(put(r, "/p/00", "0123456789", 11));
    uint8_t small[4];
    CHECK_ERR(get(r, "/p/00", small, sizeof(small), nullptr), KILN_ERR_RANGE);
}

KILN_TEST(ad10_usage_and_list_report_what_is_stored)
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

    struct counter { unsigned n; size_t bytes; } c = {0, 0};
    auto const tally = [](void *user, const char *name, size_t size) -> bool {
        (void)name;
        struct counter *k = static_cast<struct counter *>(user);
        k->n++;
        k->bytes += size;
        return true;
    };
    CHECK_OK(r->store.list(r->store.ctx, "/p/", tally, &c));
    CHECK_EQ_UINT(c.n, 1u);
    CHECK_EQ_UINT(c.bytes, 4u);
}

KILN_TEST(ad10_format_empties_the_store)
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

KILN_TEST(ad10_the_store_validates_its_arguments)
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

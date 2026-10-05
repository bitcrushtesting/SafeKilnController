/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The circular sample log -- AD-08, architecture 10.3 and 10.4,
 * FR-LOG-05..FR-LOG-09, FR-LOG-14, FR-LOG-15.
 *
 * Driven through a flash fake that enforces NOR semantics and can cut power
 * part-way through a write, which is the one thing a real device does that a
 * test otherwise cannot ask for -- and the entire subject of FR-LOG-08.
 */

#include <string.h>
#include "kiln_check.h"
#include "kiln_core/logring.h"
#include "kiln_hal_host/hal_host.h"

/* 16 sectors is enough to exercise wrap quickly; one test uses the real 2 MB. */
#define TEST_SECTORS 16u
#define SECTOR_BYTES 4096u

typedef struct {
    uint8_t             storage[TEST_SECTORS * SECTOR_BYTES];
    kiln_host_flash_t   flash;
    kiln_port_flash_t   port;
    kiln_logring_t      ring;
} rig_t;

static void rig_init(rig_t *r)
{
    memset(r, 0, sizeof(*r));
    kiln_host_flash_init(&r->flash, r->storage, sizeof(r->storage), SECTOR_BYTES);
    kiln_host_flash_bind(&r->flash, &r->port);
}

static kiln_log_sample_t sample(uint32_t t_ms, float kiln_c)
{
    kiln_log_sample_t s = {};
    s.t_rel_ms      = t_ms;
    s.kiln_filt_c   = kiln_c;
    s.kiln_raw_c    = kiln_c;
    s.setpoint_c    = kiln_c + 1.0f;
    s.case_c        = 30.0f;
    s.current_a     = 25.0f;
    s.duty_permille = 500;
    s.segment       = 1;
    s.state         = KILN_STATE_RUNNING;
    s.event         = KILN_LOGE_SAMPLE;
    return s;
}

static kiln_err_t append(rig_t *r, uint32_t t_ms, float kiln_c)
{
    const kiln_log_sample_t s = sample(t_ms, kiln_c);
    uint8_t rec[KILN_LOG_RECORD_BYTES];
    kiln_logrec_encode(&s, rec);
    return kiln_logring_append(&r->ring, rec);
}

typedef struct {
    uint32_t count;
    uint32_t first_t, last_t;
    uint32_t runs_seen;
    uint32_t last_run;
} count_ctx_t;

static bool counter(void *user, uint32_t run, const uint8_t rec[KILN_LOG_RECORD_BYTES])
{
    count_ctx_t *c = (count_ctx_t *)user;
    kiln_log_sample_t s;
    if (kiln_logrec_decode(rec, &s) != KILN_OK) {
        return true;
    }

    if (c->count == 0) {
        c->first_t = s.t_rel_ms;
    }
    c->last_t = s.t_rel_ms;
    if (run != c->last_run) { c->runs_seen++; c->last_run = run; }
    c->count++;
    return true;
}

static count_ctx_t count_all(rig_t *r, uint32_t run_id)
{
    count_ctx_t c = {};
    CHECK_OK(kiln_logring_iterate(&r->ring, run_id, counter, &c));
    return c;
}

/* --- mount and format --------------------------------------------------- */

KILN_TEST(frlog05_an_erased_partition_mounts_as_empty_and_is_usable)
{
    rig_t r;
    rig_init(&r);

    /* Nothing is wrong with a blank partition; it simply has no sectors yet. */
    CHECK_ERR(kiln_logring_mount(&r.ring, &r.port), KILN_ERR_CORRUPT);
    CHECK(r.ring.available);
    CHECK_EQ_UINT(r.ring.records_stored, 0u);
    CHECK_EQ_UINT(kiln_logring_capacity(&r.ring), TEST_SECTORS * 204u);

    CHECK_OK(kiln_logring_begin_run(&r.ring, 1));
    CHECK_OK(append(&r, 0, 100.0f));
    CHECK_EQ_UINT(count_all(&r, 0).count, 1u);
}

KILN_TEST(frlog02_records_round_trip_through_the_ring)
{
    rig_t r;
    rig_init(&r);
    (void)kiln_logring_mount(&r.ring, &r.port);
    CHECK_OK(kiln_logring_begin_run(&r.ring, 7));

    for (uint32_t i = 0; i < 50; i++) {
        CHECK_OK(append(&r, i * 10000u, 100.0f + (float)i));
    }

    const count_ctx_t c = count_all(&r, 0);
    CHECK_EQ_UINT(c.count, 50u);
    CHECK_EQ_UINT(c.first_t, 0u);
    CHECK_EQ_UINT(c.last_t, 49u * 10000u);
}

KILN_TEST(head_discovery_resumes_where_the_previous_boot_stopped)
{
    /* Architecture 10.3: the head comes from the highest valid seq and then a
     * scan for the first erased slot.  No separate metadata, so there is nothing
     * that can disagree with the data. */
    rig_t r;
    rig_init(&r);
    (void)kiln_logring_mount(&r.ring, &r.port);
    CHECK_OK(kiln_logring_begin_run(&r.ring, 1));
    for (uint32_t i = 0; i < 30; i++) {
        CHECK_OK(append(&r, i, 100.0f));
    }

    const uint32_t head_slot = r.ring.head_slot;
    const uint32_t head_seq  = r.ring.head_seq;

    /* Reboot: the medium is untouched, the struct is not. */
    kiln_logring_t fresh;
    CHECK_OK(kiln_logring_mount(&fresh, &r.port));
    CHECK_EQ_UINT(fresh.head_slot, head_slot);
    CHECK_EQ_UINT(fresh.head_seq, head_seq);
    CHECK_EQ_UINT(fresh.records_stored, 30u);

    /* And appending continues rather than overwriting. */
    r.ring = fresh;
    CHECK_OK(append(&r, 999, 200.0f));
    CHECK_EQ_UINT(count_all(&r, 0).count, 31u);
}

/* --- FR-LOG-06: wrap ---------------------------------------------------- */

KILN_TEST(frlog06_the_oldest_data_is_overwritten_and_the_run_never_fails)
{
    rig_t r;
    rig_init(&r);
    (void)kiln_logring_mount(&r.ring, &r.port);
    CHECK_OK(kiln_logring_begin_run(&r.ring, 1));

    const uint32_t capacity = kiln_logring_capacity(&r.ring);

    /* One and a half times round.  Every append must succeed: FR-LOG-06 is
     * explicit that a full log never fails a run. */
    for (uint32_t i = 0; i < capacity + capacity / 2u; i++) {
        CHECK_OK(append(&r, i, 100.0f));
    }

    const count_ctx_t c = count_all(&r, 0);
    /* Bounded by capacity, and holding the *newest* records. */
    CHECK(c.count <= capacity);
    CHECK(c.count > capacity - 204u * 2u);
    CHECK_EQ_UINT(c.last_t, capacity + capacity / 2u - 1u);
    CHECK(c.first_t > 0u);          /* the beginning is gone, as designed */
}

KILN_TEST(wrap_erases_immediately_before_writing_and_never_in_advance)
{
    /* The ordering matters: erasing the next sector ahead of time would leave a
     * window in which a power loss destroys records the index still claims. */
    rig_t r;
    rig_init(&r);
    (void)kiln_logring_mount(&r.ring, &r.port);
    CHECK_OK(kiln_logring_begin_run(&r.ring, 1));

    const uint32_t after_begin = r.flash.erases;
    CHECK_EQ_UINT(after_begin, 1u);          /* the run's own sector */

    /* Fill that sector exactly.  No further erase may have happened. */
    for (uint32_t i = 0; i < r.ring.recs_per_sector; i++) {
        CHECK_OK(append(&r, i, 100.0f));
    }
    CHECK_EQ_UINT(r.flash.erases, after_begin);

    /* The next append is the one that needs new space, and only then. */
    CHECK_OK(append(&r, 9999, 100.0f));
    CHECK_EQ_UINT(r.flash.erases, after_begin + 1u);
}

/* --- FR-LOG-08: torn records -------------------------------------------- */

KILN_TEST(frlog08_a_torn_record_costs_only_itself)
{
    rig_t r;
    rig_init(&r);
    (void)kiln_logring_mount(&r.ring, &r.port);
    CHECK_OK(kiln_logring_begin_run(&r.ring, 1));

    for (uint32_t i = 0; i < 20; i++) {
        CHECK_OK(append(&r, i, 100.0f + (float)i));
    }

    /* Cut power half way through the next record: some bytes land, the rest do
     * not, and the medium stays that way. */
    r.flash.cut_power_at_write = r.flash.writes + 1u;
    r.flash.cut_bytes          = KILN_LOG_RECORD_BYTES / 2u;
    CHECK_ERR(append(&r, 999, 500.0f), KILN_ERR_IO);
    CHECK(!r.flash.powered);

    /* Reboot. */
    kiln_host_flash_power_on(&r.flash);
    kiln_logring_t fresh;
    CHECK_OK(kiln_logring_mount(&fresh, &r.port));
    r.ring = fresh;

    /* The twenty good records are all still there, and the torn one is not. */
    const count_ctx_t c = count_all(&r, 0);
    CHECK_EQ_UINT(c.count, 20u);
    CHECK_EQ_UINT(c.last_t, 19u);

    /* And the head is at the torn slot, so appending overwrites nothing good.
     * (The torn slot itself is sacrificed -- a partially written slot cannot be
     * written again without an erase, which would cost the whole sector.) */
    CHECK_EQ_UINT(fresh.head_slot, 20u);
}

KILN_TEST(frlog08_a_single_bit_rot_is_skipped_by_readers)
{
    rig_t r;
    rig_init(&r);
    (void)kiln_logring_mount(&r.ring, &r.port);
    CHECK_OK(kiln_logring_begin_run(&r.ring, 1));
    for (uint32_t i = 0; i < 10; i++) {
        CHECK_OK(append(&r, i, 100.0f));
    }

    /* Corrupt the 6th record in place, behind the ring's back. */
    const uint32_t off = KILN_LOG_HEADER_BYTES + 5u * KILN_LOG_RECORD_BYTES;
    r.storage[off + 3] ^= 0x10u;

    /* The scan of that sector stops at the bad record: five good ones survive,
     * and nothing after it is reported as data. */
    const count_ctx_t c = count_all(&r, 0);
    CHECK_EQ_UINT(c.count, 5u);
}

/* --- runs --------------------------------------------------------------- */

KILN_TEST(iteration_can_select_a_single_run)
{
    rig_t r;
    rig_init(&r);
    (void)kiln_logring_mount(&r.ring, &r.port);

    for (uint32_t run = 1; run <= 3; run++) {
        CHECK_OK(kiln_logring_begin_run(&r.ring, run));
        for (uint32_t i = 0; i < 10; i++) {
            CHECK_OK(append(&r, run * 100u + i, 100.0f));
        }
    }

    CHECK_EQ_UINT(count_all(&r, 1).count, 10u);
    CHECK_EQ_UINT(count_all(&r, 2).count, 10u);
    CHECK_EQ_UINT(count_all(&r, 3).count, 10u);
    CHECK_EQ_UINT(count_all(&r, 0).count, 30u);
    CHECK_EQ_UINT(count_all(&r, 99).count, 0u);
}

KILN_TEST(ad09_the_log_tail_is_the_power_loss_journal)
{
    /* The whole of AD-09: recovery state is read back out of the log rather than
     * written separately, because the data was already going there. */
    rig_t r;
    rig_init(&r);
    (void)kiln_logring_mount(&r.ring, &r.port);
    CHECK_OK(kiln_logring_begin_run(&r.ring, 4));

    for (uint32_t i = 0; i < 25; i++) {
        CHECK_OK(append(&r, i * 10000u, 500.0f + (float)i));
    }

    uint8_t rec[KILN_LOG_RECORD_BYTES];
    CHECK_OK(kiln_logring_last_record(&r.ring, 4, rec));

    kiln_log_sample_t s;
    CHECK_OK(kiln_logrec_decode(rec, &s));
    CHECK_EQ_UINT(s.t_rel_ms, 24u * 10000u);
    CHECK_NEAR(s.kiln_filt_c, 524.0f, 0.1f);
    CHECK_EQ_UINT(s.state, KILN_STATE_RUNNING);

    CHECK_ERR(kiln_logring_last_record(&r.ring, 99, rec), KILN_ERR_NOT_FOUND);
}

KILN_TEST(ad09_the_tail_survives_a_power_cut_mid_run)
{
    rig_t r;
    rig_init(&r);
    (void)kiln_logring_mount(&r.ring, &r.port);
    CHECK_OK(kiln_logring_begin_run(&r.ring, 5));
    for (uint32_t i = 0; i < 40; i++) {
        CHECK_OK(append(&r, i * 10000u, 600.0f));
    }

    /* Power goes out with no warning at all. */
    r.flash.powered = false;
    kiln_host_flash_power_on(&r.flash);

    kiln_logring_t fresh;
    CHECK_OK(kiln_logring_mount(&fresh, &r.port));

    uint8_t rec[KILN_LOG_RECORD_BYTES];
    CHECK_OK(kiln_logring_last_record(&fresh, 0, rec));
    kiln_log_sample_t s;
    CHECK_OK(kiln_logrec_decode(rec, &s));
    CHECK_EQ_UINT(s.t_rel_ms, 39u * 10000u);
}

/* --- FR-LOG-13, FR-LOG-14, FR-LOG-15 ----------------------------------- */

KILN_TEST(frlog13_erase_all_leaves_an_empty_store)
{
    rig_t r;
    rig_init(&r);
    (void)kiln_logring_mount(&r.ring, &r.port);
    CHECK_OK(kiln_logring_begin_run(&r.ring, 1));
    for (uint32_t i = 0; i < 100; i++) {
        CHECK_OK(append(&r, i, 100.0f));
    }
    CHECK(count_all(&r, 0).count > 0u);

    CHECK_OK(kiln_logring_erase_all(&r.ring));
    CHECK_EQ_UINT(count_all(&r, 0).count, 0u);
    CHECK_EQ_UINT(r.ring.records_stored, 0u);

    /* And it is immediately usable again. */
    CHECK_OK(kiln_logring_begin_run(&r.ring, 2));
    CHECK_OK(append(&r, 0, 100.0f));
    CHECK_EQ_UINT(count_all(&r, 0).count, 1u);
}

KILN_TEST(frlog14_a_write_failure_is_counted_and_not_fatal)
{
    rig_t r;
    rig_init(&r);
    (void)kiln_logring_mount(&r.ring, &r.port);
    CHECK_OK(kiln_logring_begin_run(&r.ring, 1));
    for (uint32_t i = 0; i < 5; i++) {
        CHECK_OK(append(&r, i, 100.0f));
    }

    r.flash.fail_write_after = r.flash.writes;
    CHECK_ERR(append(&r, 99, 100.0f), KILN_ERR_IO);
    CHECK_EQ_UINT(r.ring.write_errors, 1u);

    /* The store recovers when the hardware does; nothing latched. */
    r.flash.fail_write_after = 0;
    CHECK_OK(append(&r, 100, 100.0f));
}

KILN_TEST(frlog15_stats_report_store_health)
{
    rig_t r;
    rig_init(&r);
    (void)kiln_logring_mount(&r.ring, &r.port);
    CHECK_OK(kiln_logring_begin_run(&r.ring, 1));
    for (uint32_t i = 0; i < 300; i++) {
        CHECK_OK(append(&r, i, 100.0f));
    }

    kiln_logstore_stats_t st;
    CHECK_OK(kiln_logring_stats(&r.ring, &st));
    CHECK(st.available);
    CHECK_EQ_UINT(st.sectors_total, TEST_SECTORS);
    CHECK_EQ_UINT(st.records_total, TEST_SECTORS * 204u);
    CHECK_EQ_UINT(st.records_stored, 300u);
    CHECK_EQ_UINT(st.write_errors, 0u);
    CHECK(st.erase_count >= 2u);          /* 300 records spans two sectors */
    CHECK(st.newest_seq >= st.oldest_seq);
}

KILN_TEST(a_dead_flash_mounts_unavailable_rather_than_pretending)
{
    rig_t r;
    rig_init(&r);
    r.flash.powered = false;

    CHECK_ERR(kiln_logring_mount(&r.ring, &r.port), KILN_ERR_IO);
    CHECK(!r.ring.available);

    /* FR-LOG-14: the caller carries on and warns.  Every entry point says no
     * rather than writing somewhere undefined. */
    CHECK_ERR(kiln_logring_begin_run(&r.ring, 1), KILN_ERR_IO);
    CHECK_ERR(append(&r, 0, 100.0f), KILN_ERR_IO);
}

KILN_TEST(the_ring_validates_its_arguments)
{
    rig_t r;
    rig_init(&r);
    CHECK_ERR(kiln_logring_mount(NULL, &r.port), KILN_ERR_INVALID_ARG);
    CHECK_ERR(kiln_logring_mount(&r.ring, NULL), KILN_ERR_INVALID_ARG);

    kiln_port_flash_t partial = r.port;
    partial.erase = NULL;
    CHECK_ERR(kiln_logring_mount(&r.ring, &partial), KILN_ERR_INVALID_ARG);

    (void)kiln_logring_mount(&r.ring, &r.port);
    CHECK_ERR(kiln_logring_append(&r.ring, NULL), KILN_ERR_INVALID_ARG);
    CHECK_ERR(kiln_logring_iterate(&r.ring, 0, NULL, NULL), KILN_ERR_INVALID_ARG);
    CHECK_ERR(kiln_logring_stats(&r.ring, NULL), KILN_ERR_INVALID_ARG);
}

/* --- FR-LOG-07 and the endurance analysis of architecture 10.4 ---------- */

/* The real partition: 2 MB of 4 kB sectors. */
static uint8_t g_full_partition[2u * 1024u * 1024u];

KILN_TEST(frlog07_the_real_partition_holds_more_than_150_hours)
{
    kiln_host_flash_t flash;
    kiln_port_flash_t port;
    kiln_logring_t    ring;

    kiln_host_flash_init(&flash, g_full_partition, sizeof(g_full_partition), 4096u);
    kiln_host_flash_bind(&flash, &port);
    (void)kiln_logring_mount(&ring, &port);

    /* Architecture 10.3's figures, confirmed against the implementation rather
     * than recomputed by hand. */
    CHECK_EQ_UINT(ring.sector_count, 512u);
    CHECK_EQ_UINT(ring.recs_per_sector, 204u);
    CHECK_EQ_UINT(kiln_logring_capacity(&ring), 104448u);

    /* FR-LOG-07: at the default 10 s interval. */
    const double hours = (double)kiln_logring_capacity(&ring) * 10.0 / 3600.0;
    CHECK_MSG(hours >= 150.0, "capacity is only %.0f h", hours);
    CHECK_NEAR(hours, 290.0, 1.0);        /* AD-18's recomputed figure */
}

KILN_TEST(the_erase_count_for_150_hours_matches_the_endurance_analysis)
{
    /* Architecture 10.4 claims one erase per 34 min of running, and bases the
     * whole NFR-14 endurance argument on it.  This measures it. */
    kiln_host_flash_t flash;
    kiln_port_flash_t port;
    kiln_logring_t    ring;

    kiln_host_flash_init(&flash, g_full_partition, sizeof(g_full_partition), 4096u);
    kiln_host_flash_bind(&flash, &port);
    (void)kiln_logring_mount(&ring, &port);
    CHECK_OK(kiln_logring_begin_run(&ring, 1));

    const uint32_t samples = 150u * 3600u / 10u;      /* 150 h at 10 s = 54 000 */
    for (uint32_t i = 0; i < samples; i++) {
        kiln_log_sample_t s = {};
        s.t_rel_ms    = i * 10000u;
        s.kiln_filt_c = 500.0f;
        s.state       = KILN_STATE_RUNNING;
        uint8_t rec[KILN_LOG_RECORD_BYTES];
        kiln_logrec_encode(&s, rec);
        CHECK_OK(kiln_logring_append(&ring, rec));
    }

    /* 54 000 / 204 = 265 sectors, plus the one begin_run claimed. */
    const uint32_t expected = samples / 204u + 1u;
    CHECK_MSG(flash.erases <= expected + 1u && flash.erases >= expected - 1u,
              "150 h of logging cost %u erases, analysis says about %u",
              flash.erases, expected);

    /* One erase per 34 min of running, which is the figure the 10-year argument
     * rests on. */
    const double minutes_per_erase = 150.0 * 60.0 / (double)flash.erases;
    CHECK_NEAR(minutes_per_erase, 34.0, 1.0);

    /* And it has not wrapped at 150 h, so FR-LOG-07 is met with the data still
     * present rather than merely survivable. */
    CHECK(!ring.wrapped);
}

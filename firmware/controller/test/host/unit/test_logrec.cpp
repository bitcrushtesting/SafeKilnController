/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 * kiln_core/logrec -- SWR-LOG-02, SWR-LOG-08, SWR-LOG-10, SWR-LOG-11, SWA-18.
 */

#include <stdio.h>
#include <string.h>
#include "kiln_check.h"
#include "kiln_core/logrec.h"

static kiln_log_sample_t sample(uint32_t t_ms, float kiln_c, float current_a)
{
    kiln_log_sample_t s = {};
    s.t_rel_ms      = t_ms;
    s.kiln_raw_c    = kiln_c + 0.3f;
    s.kiln_filt_c   = kiln_c;
    s.setpoint_c    = kiln_c + 2.0f;
    s.case_c        = 35.0f;
    s.current_a     = current_a;
    s.duty_permille = 500;
    s.segment       = 2;
    s.state         = KILN_STATE_RUNNING;
    s.flags         = KILN_LOGF_WALL_VALID;
    s.current_flags = KILN_CURF_CONDUCTION;
    return s;
}

/*
 * @relation(SWA-18, scope=function)
 */
KILN_TEST(swa18_the_record_is_twenty_bytes)
{
    /* SWA-18: 20 B since SWR-CUR-09 added heater current, which gives 204 records
     * per 4 kB sector. */
    CHECK_EQ_UINT(KILN_LOG_RECORD_BYTES, 20u);
    CHECK_EQ_UINT(KILN_LOG_RECS_PER_SECTOR, 204u);
}

/*
 * @relation(SWR-LOG-02, scope=function)
 */
KILN_TEST(swrlog02_a_record_round_trips_within_its_stated_resolution)
{
    const kiln_log_sample_t in = sample(123456u, 987.6f, 28.45f);
    uint8_t rec[KILN_LOG_RECORD_BYTES];
    kiln_logrec_encode(&in, rec);

    kiln_log_sample_t out;
    CHECK_OK(kiln_logrec_decode(rec, &out));

    CHECK_EQ_UINT(out.t_rel_ms, in.t_rel_ms);
    CHECK_NEAR(out.kiln_filt_c, in.kiln_filt_c, 0.05f);     /* 0.1 degC steps */
    CHECK_NEAR(out.kiln_raw_c, in.kiln_raw_c, 0.05f);
    CHECK_NEAR(out.setpoint_c, in.setpoint_c, 0.05f);
    CHECK_NEAR(out.case_c, in.case_c, 0.05f);
    /* SWR-CUR-02 asks for 0.1 A; the record carries 10 mA steps. */
    CHECK_NEAR(out.current_a, in.current_a, 0.01f);
    CHECK_NEAR((double)out.duty_permille, (double)in.duty_permille, 3.0);
    CHECK_EQ_UINT(out.segment, in.segment);
    CHECK_EQ_UINT(out.state, in.state);
    CHECK_EQ_UINT(out.flags, in.flags);
    CHECK_EQ_UINT(out.current_flags, in.current_flags);
}

/*
 * @relation(SWR-LOG-02, scope=function)
 */
KILN_TEST(swrlog02_saturates_rather_than_wrapping)
{
    kiln_log_sample_t in = sample(0, 5000.0f, 900.0f);
    in.duty_permille = 60000;
    uint8_t rec[KILN_LOG_RECORD_BYTES];
    kiln_logrec_encode(&in, rec);

    kiln_log_sample_t out;
    CHECK_OK(kiln_logrec_decode(rec, &out));
    CHECK(out.kiln_filt_c > 3000.0f);          /* saturated high, not wrapped */
    CHECK(out.current_a > 600.0f);
    CHECK(out.duty_permille <= KILN_DUTY_MAX);

    in = sample(0, -1000.0f, -5.0f);
    kiln_logrec_encode(&in, rec);
    CHECK_OK(kiln_logrec_decode(rec, &out));
    CHECK(out.kiln_filt_c < -1000.0f || out.kiln_filt_c > -4000.0f);
    CHECK_NEAR(out.current_a, 0.0f, 0.001f);   /* a negative current is zero */
}

/*
 * @relation(SWR-LOG-08, scope=function)
 */
KILN_TEST(swrlog08_a_torn_record_is_corrupt_and_an_erased_one_is_simply_absent)
{
    /* The distinction matters to the reader: "not found" is the end of what has
     * been written and iteration stops; "corrupt" is a torn write or bit rot and
     * iteration should skip this record and carry on. */
    const kiln_log_sample_t in = sample(1000u, 500.0f, 10.0f);
    uint8_t rec[KILN_LOG_RECORD_BYTES];
    kiln_logrec_encode(&in, rec);

    rec[5] ^= 0x40u;                            /* one flipped bit */
    kiln_log_sample_t out;
    CHECK_ERR(kiln_logrec_decode(rec, &out), KILN_ERR_CORRUPT);

    memset(rec, 0xFF, sizeof(rec));
    CHECK(kiln_logrec_is_erased(rec));
    CHECK_ERR(kiln_logrec_decode(rec, &out), KILN_ERR_NOT_FOUND);

    CHECK_ERR(kiln_logrec_decode(NULL, &out), KILN_ERR_INVALID_ARG);
    CHECK_ERR(kiln_logrec_decode(rec, NULL), KILN_ERR_INVALID_ARG);
}

KILN_TEST(every_single_bit_flip_is_caught_by_the_record_crc)
{
    const kiln_log_sample_t in = sample(7777u, 612.3f, 22.0f);
    uint8_t good[KILN_LOG_RECORD_BYTES];
    kiln_logrec_encode(&in, good);

    for (size_t byte = 0; byte < KILN_LOG_RECORD_BYTES; byte++) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t rec[KILN_LOG_RECORD_BYTES];
            memcpy(rec, good, sizeof(rec));
            rec[byte] = (uint8_t)(rec[byte] ^ (1u << (unsigned)bit));
            if (kiln_logrec_is_erased(rec)) {
                continue;
            }

            kiln_log_sample_t out;
            CHECK_MSG(kiln_logrec_decode(rec, &out) == KILN_ERR_CORRUPT,
                      "byte %zu bit %d was not detected", byte, bit);
        }
    }
}

KILN_TEST(the_sector_header_round_trips_and_rejects_a_foreign_one)
{
    const kiln_log_sector_hdr_t h = { .magic = KILN_LOG_MAGIC, .seq = 42u,
                                      .run_id = 7u,
                                      .format_version = KILN_LOG_FORMAT_VERSION };
    uint8_t buf[KILN_LOG_HEADER_BYTES];
    kiln_logrec_encode_hdr(&h, buf);

    kiln_log_sector_hdr_t out;
    CHECK_OK(kiln_logrec_decode_hdr(buf, &out));
    CHECK_EQ_UINT(out.seq, 42u);
    CHECK_EQ_UINT(out.run_id, 7u);

    buf[1] ^= 0x01u;
    CHECK_ERR(kiln_logrec_decode_hdr(buf, &out), KILN_ERR_CORRUPT);

    /* A header from a future format version, with a valid CRC. */
    kiln_log_sector_hdr_t future = h;
    future.format_version = KILN_LOG_FORMAT_VERSION + 1u;
    kiln_logrec_encode_hdr(&future, buf);
    CHECK_ERR(kiln_logrec_decode_hdr(buf, &out), KILN_ERR_UNSUPPORTED);
}

/* --- decimation, SWR-LOG-10 and SWR-LOG-11 ------------------------------- */

/*
 * @relation(SWR-LOG-11, scope=function)
 */
KILN_TEST(swrlog11_a_brief_excursion_survives_downsampling)
{
    kiln_log_bucket_t buckets[10];
    kiln_decimator_t d;
    CHECK_OK(kiln_decimator_init(&d, buckets, 10, 0, 10000));

    for (uint32_t t = 0; t < 10000u; t += 100u) {
        /* One sample, in the middle of a bucket, 300 degC above the rest. */
        const float c = (t == 2500u) ? 800.0f : 500.0f;
        const kiln_log_sample_t s = sample(t, c, 20.0f);
        kiln_decimator_push(&d, &s);
    }

    /* Averaging it away is precisely what SWR-LOG-11 forbids. */
    bool found = false;
    for (uint16_t i = 0; i < d.used; i++) {
        if (buckets[i].kiln_max_c > 790.0f) {
            found = true;
        }
    }
    CHECK(found);
    CHECK_EQ_UINT(d.used, 10u);
}

/*
 * @relation(SWR-LOG-10, scope=function)
 */
KILN_TEST(swrlog10_an_open_ended_query_returns_a_view_of_the_whole_run)
{
    /* The old behaviour filled buckets sequentially and then dropped everything,
     * so an open-ended query returned the *first* N samples rather than a
     * downsampled view -- which for a chart of a ten-hour firing meant the first
     * few minutes of it. */
    kiln_log_bucket_t buckets[64];
    kiln_decimator_t d;
    CHECK_OK(kiln_decimator_init(&d, buckets, 64, 0, 0));
    CHECK(d.unbounded);

    const uint32_t span_ms = 10u * 3600u * 1000u;      /* ten hours */
    const uint32_t step_ms = 10000u;                   /* the default interval */
    uint32_t pushed = 0;

    for (uint32_t t = 0; t <= span_ms; t += step_ms) {
        /* A ramp, so position in the output is checkable. */
        const float c = 20.0f + 100.0f * (float)t / (float)span_ms;
        const kiln_log_sample_t s = sample(t, c, 20.0f);
        kiln_decimator_push(&d, &s);
        pushed++;
    }

    CHECK_EQ_UINT(d.accepted, pushed);
    CHECK(d.used > 1u);
    CHECK(d.used <= 64u);
    CHECK(d.folds > 0u);

    /* The last bucket holds the end of the run, not the start. */
    const kiln_log_bucket_t *last = &buckets[d.used - 1u];
    CHECK_NEAR(last->kiln_max_c, 120.0f, 2.0f);
    CHECK_NEAR((double)buckets[0].kiln_min_c, 20.0, 2.0);
    CHECK(last->t_rel_ms > span_ms / 2u);

    /* Every sample landed in some bucket. */
    uint32_t counted = 0;
    for (uint16_t i = 0; i < d.used; i++) {
        counted += buckets[i].count;
    }
    CHECK_EQ_UINT(counted, pushed);
}

/*
 * @relation(SWR-LOG-11, scope=function)
 */
KILN_TEST(swrlog11_extrema_survive_a_fold)
{
    kiln_log_bucket_t buckets[8];
    kiln_decimator_t d;
    CHECK_OK(kiln_decimator_init(&d, buckets, 8, 0, 0));

    /* Force several foldings, with one spike that must still be visible. */
    for (uint32_t t = 0; t < 100000u; t += 100u) {
        const float c = (t == 50000u) ? 900.0f : 400.0f;
        const kiln_log_sample_t s = sample(t, c, 20.0f);
        kiln_decimator_push(&d, &s);
    }
    CHECK(d.folds > 0u);

    bool found = false;
    for (uint16_t i = 0; i < d.used; i++) {
        if (buckets[i].kiln_max_c > 890.0f) {
            found = true;
        }
    }
    CHECK(found);
}

KILN_TEST(decimation_ignores_samples_outside_a_bounded_range)
{
    kiln_log_bucket_t buckets[4];
    kiln_decimator_t d;
    CHECK_OK(kiln_decimator_init(&d, buckets, 4, 1000, 2000));

    const kiln_log_sample_t before = sample(500u, 100.0f, 0.0f);
    const kiln_log_sample_t inside = sample(1500u, 200.0f, 0.0f);
    const kiln_log_sample_t after  = sample(5000u, 300.0f, 0.0f);

    kiln_decimator_push(&d, &before);
    kiln_decimator_push(&d, &inside);
    kiln_decimator_push(&d, &after);
    CHECK_EQ_UINT(d.accepted, 1u);
}

KILN_TEST(the_decimator_validates_its_arguments)
{
    /* It used to memset its target before any validation, while push checked all
     * three of its pointers. */
    kiln_log_bucket_t buckets[4];
    kiln_decimator_t d;

    CHECK_ERR(kiln_decimator_init(NULL, buckets, 4, 0, 100), KILN_ERR_INVALID_ARG);
    CHECK_ERR(kiln_decimator_init(&d, NULL, 4, 0, 100), KILN_ERR_INVALID_ARG);
    CHECK_OK(kiln_decimator_init(&d, buckets, 0, 0, 100));
    CHECK_EQ_UINT(d.capacity, 1u);

    const kiln_log_sample_t s = sample(0, 100.0f, 0.0f);
    kiln_decimator_push(NULL, &s);
    kiln_decimator_push(&d, NULL);
}

/* ===========================================================================
 * THE WIRE FORMAT, AGAINST A COMMITTED FIXTURE  (SWA-18, tasklist C7)
 * ===========================================================================
 * `firmware/controller/test/host/fixtures/logring.bin` is two log sectors
 * produced by this encoder and checked into git. Two things read it and
 * neither may drift:
 *
 *   this test, which re-encodes the same samples and compares bytes, so the
 *   firmware cannot change the format without the fixture changing too;
 *
 *   tools/logdump.py, which decodes it with an implementation written from
 *   the format's documentation rather than from this code.
 *
 * That second reader is the point. A log is the only evidence of what a kiln
 * did before it failed, and "did the firmware record this correctly" cannot
 * be answered by the firmware's own decoder: a codec that encodes and decodes
 * with the same wrong idea round-trips perfectly and proves nothing. Two
 * implementations agreeing is evidence; one agreeing with itself is a
 * tautology.
 *
 * If this test fails, the question is which side moved. If the format changed
 * deliberately, the sector header's format_version changes with it and the
 * fixture is regenerated; if it did not, the encoder has a bug that would
 * have made every log on every device unreadable by the tooling.
 */

/*
 * @relation(SWA-18, scope=function)
 */
KILN_TEST(swa18_the_encoder_still_produces_the_committed_wire_format)
{
    const char *path = KILN_FIXTURE_DIR "/logring.bin";
    FILE *fh = fopen(path, "rb");
    CHECK_MSG(fh != nullptr, "cannot open the fixture at %s", path);
    if (fh == nullptr) { return; }

    static uint8_t want[2 * KILN_LOG_SECTOR_BYTES];
    const size_t n = fread(want, 1, sizeof(want), fh);
    (void)fclose(fh);
    CHECK_EQ_UINT(n, sizeof(want));

    /* The same two headers and three samples the fixture was built from. The
     * values are chosen for what a decoder gets wrong: a negative
     * temperature, the segment sentinel, every flag at once, a non-sample
     * event, and each field at the edge of its scale. */
    static uint8_t got[2 * KILN_LOG_SECTOR_BYTES];
    memset(got, 0xFF, sizeof(got));

    const struct { uint32_t seq, run; } secs[2] = { {9, 2}, {8, 2} };
    for (int s = 0; s < 2; s++) {
        kiln_log_sector_hdr_t h = {};
        h.magic          = KILN_LOG_MAGIC;
        h.seq            = secs[s].seq;
        h.run_id         = secs[s].run;
        h.format_version = KILN_LOG_FORMAT_VERSION;
        uint8_t hb[KILN_LOG_HEADER_BYTES];
        kiln_logrec_encode_hdr(&h, hb);
        memcpy(&got[(size_t)s * KILN_LOG_SECTOR_BYTES], hb, sizeof(hb));
    }

    kiln_log_sample_t a = {};
    a.t_rel_ms = 1000; a.kiln_raw_c = 20.5f; a.kiln_filt_c = 20.4f;
    a.setpoint_c = 100.0f; a.case_c = 30.0f; a.current_a = 12.34f;
    a.duty_permille = 500; a.segment = 3; a.state = 1; a.flags = 0;
    a.current_flags = 1; a.event = 0;

    kiln_log_sample_t b = a;
    b.t_rel_ms = 2000; b.kiln_raw_c = -12.3f; b.setpoint_c = 0.0f;
    b.segment = KILN_SEG_NONE; b.state = 0; b.flags = 0xF0; b.event = 3;
    b.duty_permille = 0; b.current_a = 0.0f;

    kiln_log_sample_t c = a;
    c.t_rel_ms = 3000; c.kiln_raw_c = 1285.0f; c.kiln_filt_c = 1284.9f;
    c.setpoint_c = 1300.0f; c.case_c = 55.5f; c.current_a = 31.75f;
    c.duty_permille = 1000; c.segment = 4; c.state = 5; c.flags = 0x10;
    c.event = 2; c.current_flags = 0;

    /* Sector 1 holds the older two, sector 0 the newest: deliberately out of
     * address order, so anything that reads the ring by address rather than by
     * sequence number interleaves two firings. */
    const struct { int sector; const kiln_log_sample_t *s; } rows[] = {
        { 1, &a }, { 1, &b }, { 0, &c },
    };
    size_t used[2] = {0, 0};
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        uint8_t rec[KILN_LOG_RECORD_BYTES];
        kiln_logrec_encode(rows[i].s, rec);
        const size_t off = (size_t)rows[i].sector * KILN_LOG_SECTOR_BYTES
                         + KILN_LOG_HEADER_BYTES
                         + used[rows[i].sector] * KILN_LOG_RECORD_BYTES;
        memcpy(&got[off], rec, sizeof(rec));
        used[rows[i].sector]++;
    }

    /* Byte for byte, and the first difference is reported: "the images differ"
     * sends the reader to a hex dump, while an offset sends them to a field. */
    size_t first_diff = sizeof(got);
    for (size_t i = 0; i < sizeof(got); i++) {
        if (got[i] != want[i]) { first_diff = i; break; }
    }
    CHECK_MSG(first_diff == sizeof(got),
              "the encoder no longer produces the committed format: first "
              "difference at byte %zu (sector %zu, offset %zu in it), got "
              "0x%02X want 0x%02X",
              first_diff, first_diff / KILN_LOG_SECTOR_BYTES,
              first_diff % KILN_LOG_SECTOR_BYTES,
              (unsigned)got[first_diff < sizeof(got) ? first_diff : 0],
              (unsigned)want[first_diff < sizeof(want) ? first_diff : 0]);

    /* And the round trip, so a change that is symmetrical in the codec but
     * wrong against the fixture is still caught above rather than here. */
    kiln_log_sample_t back = {};
    CHECK_OK(kiln_logrec_decode(&want[KILN_LOG_SECTOR_BYTES + KILN_LOG_HEADER_BYTES],
                                &back));
    CHECK_EQ_UINT(back.t_rel_ms, 1000u);
    CHECK_NEAR(back.kiln_raw_c, 20.5f, 0.051f);
    CHECK_EQ_UINT(back.segment, 3u);
}

/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Decode over arbitrary bytes -- architecture section 14.4's fuzz level.
 *
 * A deterministic sweep rather than a coverage-guided fuzzer, so it runs in CI
 * on every push without a corpus to maintain.  Under ASan and UBSan (TR-20) this
 * is what catches an out-of-bounds read or a signed-overflow in the codec; the
 * property being asserted is simply that *no* input of the right length can make
 * the decoder do anything but succeed or return an error.
 */

#include <string.h>
#include "kiln_check.h"
#include "kiln_core/logrec.h"

static uint32_t rng_state = 0x12345678u;

static uint32_t rng(void)
{
    uint32_t x = rng_state;
    x ^= x << 13u; x ^= x >> 17u; x ^= x << 5u;
    rng_state = x;
    return x;
}

KILN_TEST(record_decode_survives_arbitrary_bytes)
{
    for (int iter = 0; iter < 200000; iter++) {
        uint8_t rec[KILN_LOG_RECORD_BYTES];
        for (size_t i = 0; i < sizeof(rec); i++) {
            rec[i] = (uint8_t)(rng() >> 13u);
        }

        kiln_log_sample_t out;
        memset(&out, 0xA5, sizeof(out));
        const kiln_err_t e = kiln_logrec_decode(rec, &out);

        CHECK(e == KILN_OK || e == KILN_ERR_CORRUPT || e == KILN_ERR_NOT_FOUND);
        if (e == KILN_OK) {
            /* A record that passed its CRC must decode to usable values: the
             * fields are fixed-point, so there is no input that can produce a
             * NaN or an out-of-enum state. */
            CHECK(kiln_is_finite(out.kiln_filt_c));
            CHECK(kiln_is_finite(out.setpoint_c));
            CHECK(kiln_is_finite(out.current_a));
            CHECK(out.current_a >= 0.0f);
            CHECK(out.duty_permille <= KILN_DUTY_MAX + 5u);
            CHECK(out.state < 16u);
        }
    }
}

KILN_TEST(record_decode_survives_bytes_near_a_valid_record)
{
    /* Random bytes almost never pass an 8-bit CRC, so most of the above only
     * exercises the rejection path.  Mutating a *valid* record reaches the
     * decode path far more often. */
    kiln_log_sample_t in = {};
    in.t_rel_ms      = 123456u;
    in.kiln_filt_c   = 900.0f;
    in.setpoint_c    = 905.0f;
    in.current_a     = 25.0f;
    in.duty_permille = 650;
    in.state         = KILN_STATE_RUNNING;

    uint8_t good[KILN_LOG_RECORD_BYTES];
    kiln_logrec_encode(&in, good);

    int accepted = 0;
    for (int iter = 0; iter < 200000; iter++) {
        uint8_t rec[KILN_LOG_RECORD_BYTES];
        memcpy(rec, good, sizeof(rec));

        /* Mutate a few bytes, and recompute the CRC half the time so the
         * post-CRC path gets properly exercised. */
        const int muts = 1 + (int)(rng() % 3u);
        for (int m = 0; m < muts; m++) {
            rec[rng() % KILN_LOG_RECORD_BYTES] = (uint8_t)(rng() >> 11u);
        }
        if ((rng() & 1u) != 0u) {
            rec[KILN_LOG_RECORD_BYTES - 1] =
                kiln_crc8(rec, KILN_LOG_RECORD_BYTES - 1);
        }

        kiln_log_sample_t out;
        const kiln_err_t e = kiln_logrec_decode(rec, &out);
        CHECK(e == KILN_OK || e == KILN_ERR_CORRUPT || e == KILN_ERR_NOT_FOUND);
        if (e == KILN_OK) {
            accepted++;
            CHECK(kiln_is_finite(out.kiln_filt_c));
            CHECK(out.current_a >= 0.0f);
        }
    }
    CHECK(accepted > 1000);
}

KILN_TEST(header_decode_survives_arbitrary_bytes)
{
    for (int iter = 0; iter < 200000; iter++) {
        uint8_t buf[KILN_LOG_HEADER_BYTES];
        for (size_t i = 0; i < sizeof(buf); i++) {
            buf[i] = (uint8_t)(rng() >> 13u);
        }
        if ((rng() & 1u) != 0u) {
            const uint16_t crc = kiln_crc16(buf, KILN_LOG_HEADER_BYTES - 2);
            buf[14] = (uint8_t)crc;
            buf[15] = (uint8_t)(crc >> 8u);
        }

        kiln_log_sector_hdr_t out;
        const kiln_err_t e = kiln_logrec_decode_hdr(buf, &out);
        CHECK(e == KILN_OK || e == KILN_ERR_CORRUPT || e == KILN_ERR_UNSUPPORTED);
        if (e == KILN_OK) {
            CHECK_EQ_UINT(out.magic, KILN_LOG_MAGIC);
            CHECK_EQ_UINT(out.format_version, KILN_LOG_FORMAT_VERSION);
        }
    }
}

KILN_TEST(decimation_survives_arbitrary_samples)
{
    /* FR-LOG-10's decimator now folds buckets as a run grows, which is new index
     * arithmetic over caller-supplied storage -- exactly the thing a sweep under
     * ASan should be pointed at. */
    for (int iter = 0; iter < 2000; iter++) {
        kiln_log_bucket_t buckets[32];
        kiln_decimator_t d;

        const uint16_t cap   = (uint16_t)(1u + (rng() % 32u));
        const uint32_t from  = rng() % 100000u;
        const uint32_t to = ((rng() & 1u) != 0u) ? 0u : from + (rng() % 100000u);
        CHECK_OK(kiln_decimator_init(&d, buckets, cap, from, to));

        for (int i = 0; i < 500; i++) {
            kiln_log_sample_t s = {};
            s.t_rel_ms      = rng();
            s.kiln_filt_c   = (float)(int32_t)rng() / 1e6f;
            s.setpoint_c    = (float)(int32_t)rng() / 1e6f;
            s.current_a     = (float)(rng() % 10000u) / 100.0f;
            s.duty_permille = (uint16_t)(rng() % 1001u);
            s.state         = (uint8_t)(rng() % 16u);
            kiln_decimator_push(&d, &s);

            CHECK(d.used <= d.capacity);
        }

        /* Whatever arrived, the invariants hold: no bucket beyond `used` has a
         * count, and every non-empty bucket has min <= max. */
        for (uint16_t b = 0; b < d.used; b++) {
            if (buckets[b].count == 0) {
                continue;
            }
            CHECK(buckets[b].kiln_min_c <= buckets[b].kiln_max_c);
            CHECK(buckets[b].duty_min <= buckets[b].duty_max);
        }
        for (uint16_t b = d.used; b < cap; b++) {
            CHECK_EQ_UINT(buckets[b].count, 0u);
        }
    }
}

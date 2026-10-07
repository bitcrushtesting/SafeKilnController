/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The link wire format.  Both ends of the link depend on this agreeing with
 * itself, and the ESP32 side has no way to ask for a retransmission, so a
 * frame that decodes wrongly is a wrong temperature acted on.
 */
#include <string.h>

#include "kiln_check.h"
#include "sup_proto.h"

namespace {

sup_report_t sample()
{
    sup_report_t r = {};
    r.version     = SUP_VERSION;
    r.seq         = 42u;
    r.chamber_c   = 1234.5f;
    r.cj_c        = 28.3f;
    r.fault_bits  = 0u;
    r.flags = (uint8_t)(SUP_FLAG_PERMIT | SUP_FLAG_TC_VALID | SUP_FLAG_SELFTEST_OK);
    r.trip_reason = SUP_TRIP_NONE;
    return r;
}

}  // namespace

KILN_TEST(ad22_crc16_matches_the_log_records_vector)
{
    /* CCITT-FALSE.  The supervisor carries its own implementation so it
     * depends on nothing of kiln_core's; this vector is the contract between
     * the two copies, and the same check exists on the other side. */
    const uint8_t v[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    CHECK_EQ_UINT(sup_crc16(v, sizeof(v)), 0x29B1u);
    CHECK_EQ_UINT(sup_crc16(nullptr, 4u), 0xFFFFu);
}

KILN_TEST(ad22_a_frame_round_trips)
{
    const sup_report_t in = sample();
    uint8_t buf[SUP_FRAME_BYTES];
    CHECK_EQ_UINT(sup_encode(&in, buf, sizeof(buf)), SUP_FRAME_BYTES);

    sup_report_t out = {};
    size_t skip = 99u;
    CHECK_EQ_UINT(sup_decode(buf, sizeof(buf), &out, &skip), SUP_FRAME_BYTES);
    CHECK_EQ_UINT(skip, 0u);
    CHECK_EQ_UINT(out.version, SUP_VERSION);
    CHECK_EQ_UINT(out.seq, 42u);
    CHECK_NEAR(out.chamber_c, 1234.5f, 0.05);
    CHECK_NEAR(out.cj_c, 28.3f, 0.05);
    CHECK_EQ_UINT(out.flags, in.flags);
    CHECK_EQ_INT(out.trip_reason, SUP_TRIP_NONE);
}

KILN_TEST(ad22_the_frame_is_the_declared_length_and_starts_with_the_sof)
{
    const sup_report_t in = sample();
    uint8_t buf[32];
    memset(buf, 0, sizeof(buf));
    CHECK_EQ_UINT(sup_encode(&in, buf, sizeof(buf)), 13u);
    CHECK_EQ_UINT(SUP_FRAME_BYTES, 13u);
    CHECK_EQ_UINT(buf[0], SUP_SOF);
    CHECK_EQ_UINT(buf[1], SUP_VERSION);
}

KILN_TEST(ad22_a_single_flipped_bit_anywhere_is_rejected)
{
    const sup_report_t in = sample();
    uint8_t good[SUP_FRAME_BYTES];
    (void)sup_encode(&in, good, sizeof(good));

    for (size_t byte = 0; byte < SUP_FRAME_BYTES; byte++) {
        for (unsigned bit = 0; bit < 8u; bit++) {
            uint8_t bad[SUP_FRAME_BYTES];
            memcpy(bad, good, sizeof(bad));
            bad[byte] ^= (uint8_t)(1u << bit);
            sup_report_t out = {};
            size_t skip = 0;
            const size_t n = sup_decode(bad, sizeof(bad), &out, &skip);
            if (n != 0u) {
                /* The only acceptable survivor is a flip in the SOF that moves
                 * the frame out of consideration entirely, which cannot
                 * produce a *valid* frame from this buffer. */
                KILN_FAIL("byte %u bit %u decoded despite a flipped bit",
                          (unsigned)byte, bit);
            }
        }
    }
}

KILN_TEST(ad22_resyncs_past_line_noise_and_reports_what_to_discard)
{
    /* The receiver must recover from a partial frame or a burst of noise
     * without an escape scheme, because the supervisor never retransmits. */
    const sup_report_t in = sample();
    uint8_t stream[8u + SUP_FRAME_BYTES];
    const uint8_t noise[8] = { 0x00, 0xFF, 0xA5, 0x13, 0xA5, 0xA5, 0x7E, 0x01 };
    memcpy(stream, noise, sizeof(noise));
    (void)sup_encode(&in, &stream[8], SUP_FRAME_BYTES);

    sup_report_t out = {};
    size_t skip = 0;
    CHECK_EQ_UINT(sup_decode(stream, sizeof(stream), &out, &skip),
                  8u + SUP_FRAME_BYTES);
    CHECK_EQ_UINT(skip, 8u);
    CHECK_EQ_UINT(out.seq, 42u);
}

KILN_TEST(ad22_a_partial_frame_consumes_nothing_and_keeps_the_tail)
{
    const sup_report_t in = sample();
    uint8_t buf[SUP_FRAME_BYTES];
    (void)sup_encode(&in, buf, sizeof(buf));

    sup_report_t out = {};
    size_t skip = 123u;
    /* One byte short: nothing yet, and nothing discarded. */
    CHECK_EQ_UINT(sup_decode(buf, SUP_FRAME_BYTES - 1u, &out, &skip), 0u);
    CHECK_EQ_UINT(skip, 0u);
}

KILN_TEST(ad22_a_buffer_of_pure_noise_is_eventually_discardable)
{
    /* Otherwise a receiver wedges on a buffer that can never contain a frame. */
    uint8_t noise[64];
    for (size_t i = 0; i < sizeof(noise); i++) {
        noise[i] = (uint8_t)(i * 7u + 3u);
    }
    sup_report_t out = {};
    size_t skip = 0;
    CHECK_EQ_UINT(sup_decode(noise, sizeof(noise), &out, &skip), 0u);
    CHECK_EQ_UINT(skip, sizeof(noise) - (SUP_FRAME_BYTES - 1u));
}

KILN_TEST(ad22_temperatures_saturate_rather_than_wrap_and_survive_a_nan)
{
    sup_report_t r = sample();
    uint8_t buf[SUP_FRAME_BYTES];
    sup_report_t out = {};

    r.chamber_c = 9999.0f;
    (void)sup_encode(&r, buf, sizeof(buf));
    (void)sup_decode(buf, sizeof(buf), &out, nullptr);
    CHECK(out.chamber_c > 3000.0f);     /* saturated, not wrapped negative */

    r.chamber_c = -9999.0f;
    (void)sup_encode(&r, buf, sizeof(buf));
    (void)sup_decode(buf, sizeof(buf), &out, nullptr);
    CHECK(out.chamber_c < -3000.0f);

    r.chamber_c = NAN;
    CHECK_EQ_UINT(sup_encode(&r, buf, sizeof(buf)), SUP_FRAME_BYTES);
    (void)sup_decode(buf, sizeof(buf), &out, nullptr);
    CHECK_NEAR(out.chamber_c, 0.0f, 0.05);   /* a defined value, not garbage */
}

KILN_TEST(ad22_a_trip_is_still_reported_so_the_other_side_can_say_why)
{
    sup_report_t r = sample();
    r.flags = (uint8_t)(SUP_FLAG_TRIPPED | SUP_FLAG_TC_VALID | SUP_FLAG_SELFTEST_OK);
    r.trip_reason = SUP_TRIP_OVERTEMP;
    r.chamber_c   = 1361.2f;

    uint8_t buf[SUP_FRAME_BYTES];
    (void)sup_encode(&r, buf, sizeof(buf));
    sup_report_t out = {};
    CHECK_EQ_UINT(sup_decode(buf, sizeof(buf), &out, nullptr), SUP_FRAME_BYTES);
    CHECK((out.flags & SUP_FLAG_TRIPPED) != 0u);
    CHECK((out.flags & SUP_FLAG_PERMIT) == 0u);
    CHECK_EQ_INT(out.trip_reason, SUP_TRIP_OVERTEMP);
}

KILN_TEST(nfr17_encode_and_decode_refuse_bad_arguments)
{
    const sup_report_t r = sample();
    uint8_t buf[SUP_FRAME_BYTES];
    CHECK_EQ_UINT(sup_encode(nullptr, buf, sizeof(buf)), 0u);
    CHECK_EQ_UINT(sup_encode(&r, nullptr, sizeof(buf)), 0u);
    CHECK_EQ_UINT(sup_encode(&r, buf, SUP_FRAME_BYTES - 1u), 0u);

    sup_report_t out = {};
    CHECK_EQ_UINT(sup_decode(nullptr, 32u, &out, nullptr), 0u);
    CHECK_EQ_UINT(sup_decode(buf, sizeof(buf), nullptr, nullptr), 0u);
}

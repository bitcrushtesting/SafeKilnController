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
    r.chamber_dc  = 12345;      /* 1234.5 degC in tenths */
    r.cj_dc       = 283;        /*   28.3 degC           */
    r.fault_bits  = 0u;
    r.flags = (uint8_t)(SUP_FLAG_PERMIT | SUP_FLAG_TC_VALID | SUP_FLAG_SELFTEST_OK);
    r.trip_reason = SUP_TRIP_NONE;
    return r;
}

}  // namespace

/*
 * @relation(SWA-22, scope=function)
 */
KILN_TEST(swa22_crc16_matches_the_log_records_vector)
{
    /* CCITT-FALSE.  The supervisor carries its own implementation so it
     * depends on nothing of kiln_core's; this vector is the contract between
     * the two copies, and the same check exists on the other side. */
    const uint8_t v[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    CHECK_EQ_UINT(sup_crc16(v, sizeof(v)), 0x29B1u);
    CHECK_EQ_UINT(sup_crc16(nullptr, 4u), 0xFFFFu);
}

/*
 * @relation(SWA-22, scope=function)
 */
KILN_TEST(swa22_a_frame_round_trips)
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
    /* Exact equality, not a tolerance: the frame carries the same integer the
     * report held, so a round trip that loses anything is a defect rather than
     * a rounding.  The float version could only ever check "close enough". */
    CHECK_EQ_INT(out.chamber_dc, 12345);
    CHECK_EQ_INT(out.cj_dc, 283);
    CHECK_EQ_UINT(out.flags, in.flags);
    CHECK_EQ_INT(out.trip_reason, SUP_TRIP_NONE);
}

/*
 * @relation(SWA-22, scope=function)
 */
KILN_TEST(swa22_the_frame_is_the_declared_length_and_starts_with_the_sof)
{
    const sup_report_t in = sample();
    uint8_t buf[32];
    memset(buf, 0, sizeof(buf));
    CHECK_EQ_UINT(sup_encode(&in, buf, sizeof(buf)), 13u);
    CHECK_EQ_UINT(SUP_FRAME_BYTES, 13u);
    CHECK_EQ_UINT(buf[0], SUP_SOF);
    CHECK_EQ_UINT(buf[1], SUP_VERSION);
}

/*
 * @relation(SWA-22, scope=function)
 */
KILN_TEST(swa22_a_single_flipped_bit_anywhere_is_rejected)
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

/*
 * @relation(SWA-22, scope=function)
 */
KILN_TEST(swa22_resyncs_past_line_noise_and_reports_what_to_discard)
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

/*
 * @relation(SWA-22, scope=function)
 */
KILN_TEST(swa22_a_partial_frame_consumes_nothing_and_keeps_the_tail)
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

/*
 * @relation(SWA-22, scope=function)
 */
KILN_TEST(swa22_a_buffer_of_pure_noise_is_eventually_discardable)
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

/*
 * @relation(SWA-22, scope=function)
 */
KILN_TEST(swa22_q7_to_tenths_is_exact_rounded_and_saturating)
{
    /* The scaling and the saturation used to live inside sup_encode, where they
     * could only be observed through a frame, and the input that needed
     * defending against was a NaN.  Both moved into sup_q7_to_dc, which is a
     * total function of an int32_t and can therefore be pinned directly.
     *
     * Exact cases first: 10/128 is 5/64, so any q7 that is a multiple of 64
     * converts without rounding at all. */
    CHECK_EQ_INT(sup_q7_to_dc(0), 0);
    CHECK_EQ_INT(sup_q7_to_dc(sup_c_to_q7(1)), 10);
    CHECK_EQ_INT(sup_q7_to_dc(sup_c_to_q7(1350)), 13500);
    CHECK_EQ_INT(sup_q7_to_dc(sup_c_to_q7(-200)), -2000);

    /* Rounding, symmetrically about zero.  One q7 LSB is 1/128 degC, so 12 of
     * them is 0.09375 degC and must round to 0.1; six is 0.046875 and must
     * round to 0.0. */
    CHECK_EQ_INT(sup_q7_to_dc(12), 1);
    CHECK_EQ_INT(sup_q7_to_dc(-12), -1);
    CHECK_EQ_INT(sup_q7_to_dc(6), 0);
    CHECK_EQ_INT(sup_q7_to_dc(-6), 0);
    /* Exactly half a tenth, which is where truncation and rounding differ:
     * 6.4 q7 is not representable, so the nearest half is 32 q7 = 0.25 degC,
     * rounding away from zero to 0.3 and -0.3 rather than towards it. */
    CHECK_EQ_INT(sup_q7_to_dc(32), 3);
    CHECK_EQ_INT(sup_q7_to_dc(-32), -3);

    /* Saturation at both ends, for values no front end can produce.  The point
     * is that the function is defined for every int32_t, INT32_MIN included,
     * which is the input that would break a negate-then-divide. */
    CHECK_EQ_INT(sup_q7_to_dc(INT32_MAX), 32767);
    CHECK_EQ_INT(sup_q7_to_dc(INT32_MIN), -32767);
    CHECK_EQ_INT(sup_q7_to_dc(sup_c_to_q7(100000)), 32767);
    CHECK_EQ_INT(sup_q7_to_dc(sup_c_to_q7(-100000)), -32767);

    /* And the frame carries whatever it is handed, unchanged: sup_encode holds
     * no arithmetic now, which is why the cases above are the whole of it. */
    sup_report_t r = sample();
    uint8_t buf[SUP_FRAME_BYTES];
    sup_report_t out = {};
    r.chamber_dc = 32767;
    (void)sup_encode(&r, buf, sizeof(buf));
    (void)sup_decode(buf, sizeof(buf), &out, nullptr);
    CHECK_EQ_INT(out.chamber_dc, 32767);
    r.chamber_dc = -32767;
    (void)sup_encode(&r, buf, sizeof(buf));
    (void)sup_decode(buf, sizeof(buf), &out, nullptr);
    CHECK_EQ_INT(out.chamber_dc, -32767);
}

/*
 * @relation(SWA-22, scope=function)
 */
KILN_TEST(swa22_a_trip_is_still_reported_so_the_other_side_can_say_why)
{
    sup_report_t r = sample();
    r.flags = (uint8_t)(SUP_FLAG_TRIPPED | SUP_FLAG_TC_VALID | SUP_FLAG_SELFTEST_OK);
    r.trip_reason = SUP_TRIP_OVERTEMP;
    r.chamber_dc  = 13612;

    uint8_t buf[SUP_FRAME_BYTES];
    (void)sup_encode(&r, buf, sizeof(buf));
    sup_report_t out = {};
    CHECK_EQ_UINT(sup_decode(buf, sizeof(buf), &out, nullptr), SUP_FRAME_BYTES);
    CHECK((out.flags & SUP_FLAG_TRIPPED) != 0u);
    CHECK((out.flags & SUP_FLAG_PERMIT) == 0u);
    CHECK_EQ_INT(out.trip_reason, SUP_TRIP_OVERTEMP);
}

/*
 * @relation(SWA-22, scope=function)
 */
KILN_TEST(swa22_decode_tolerates_a_caller_that_does_not_want_the_skip_count)
{
    /* The resync path has to work for a caller passing nullptr for skip, not
     * only for the tests that inspect it.  MC/DC found this: the condition had
     * never been false while there was also enough input to consider. */
    uint8_t noise[32];
    for (size_t i = 0; i < sizeof(noise); i++) {
        noise[i] = (uint8_t)(i * 11u + 5u);
    }
    sup_report_t out = {};
    CHECK_EQ_UINT(sup_decode(noise, sizeof(noise), &out, nullptr), 0u);
}

/*
 * @relation(SWR-NFR-17, scope=function)
 */
KILN_TEST(swrnfr17_encode_and_decode_refuse_bad_arguments)
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

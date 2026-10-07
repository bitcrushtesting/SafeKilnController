/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The ESP32's end of the supervisor link (AD-22).
 *
 * This is the one place where the chamber temperature enters the firmware, so
 * every way the link can lie or fall silent has to end up somewhere the safety
 * rules already understand.
 */
#include <string.h>

#include "kiln_check.h"
#include "kiln_core/suplink.h"

namespace {

sup_report_t rep(uint8_t seq, float c)
{
    sup_report_t r = {};
    r.version     = SUP_VERSION;
    r.seq         = seq;
    r.chamber_c   = c;
    r.cj_c        = 25.0f;
    r.fault_bits  = 0u;
    r.flags = (uint8_t)(SUP_FLAG_PERMIT | SUP_FLAG_TC_VALID | SUP_FLAG_SELFTEST_OK);
    r.trip_reason = SUP_TRIP_NONE;
    return r;
}

/* Hand a frame over the way a UART would: as bytes. */
void send(kiln_suplink_t *s, const sup_report_t &r)
{
    uint8_t f[SUP_FRAME_BYTES];
    (void)sup_encode(&r, f, sizeof(f));
    kiln_suplink_feed(s, f, sizeof(f));
}

}  // namespace

KILN_TEST(ad22_a_frame_becomes_a_thermocouple_reading)
{
    kiln_suplink_t s;
    kiln_suplink_init(&s);
    kiln_port_tc_t port = {};
    kiln_suplink_bind(&s, &port);

    send(&s, rep(1u, 812.4f));
    kiln_tc_reading_t out = {};
    CHECK_OK(port.read(port.ctx, &out));
    CHECK_NEAR(out.temp_c, 812.4f, 0.05);
    CHECK_NEAR(out.cj_c, 25.0f, 0.05);
    CHECK_EQ_UINT(out.fault_bits, 0u);
    CHECK_EQ_UINT(s.frames, 1u);
}

KILN_TEST(sr04_before_anything_is_received_the_front_end_has_not_answered)
{
    /* Absence of evidence is not a temperature.  The core sees exactly what it
     * would see from a MAX31856 that is not responding, so SR-04's grace and
     * latch handle a missing supervisor with no new rule. */
    kiln_suplink_t s;
    kiln_suplink_init(&s);
    kiln_port_tc_t port = {};
    kiln_suplink_bind(&s, &port);

    kiln_tc_reading_t out = {};
    CHECK_ERR(port.read(port.ctx, &out), KILN_ERR_IO);
    CHECK_EQ_UINT(out.fault_bits, KILN_TC_FAULT_COMMS);
    CHECK(!kiln_suplink_fresh(&s));
}

KILN_TEST(sr04_a_link_that_goes_quiet_becomes_a_comms_fault)
{
    kiln_suplink_t s;
    kiln_suplink_init(&s);
    kiln_port_tc_t port = {};
    kiln_suplink_bind(&s, &port);

    send(&s, rep(1u, 600.0f));
    CHECK(kiln_suplink_fresh(&s));

    /* Two acquisition cycles with nothing is not an event: the threshold is
     * sized against the pump rate, not the report rate. */
    kiln_suplink_tick(&s, 0.25f);
    CHECK(kiln_suplink_fresh(&s));
    kiln_suplink_tick(&s, 0.25f);
    CHECK(kiln_suplink_fresh(&s));

    kiln_suplink_tick(&s, 0.25f);
    CHECK(!kiln_suplink_fresh(&s));

    kiln_tc_reading_t out = {};
    CHECK_ERR(port.read(port.ctx, &out), KILN_ERR_IO);
    CHECK_EQ_UINT(out.fault_bits, KILN_TC_FAULT_COMMS);
    /* And no stale temperature is handed out dressed as current. */
    CHECK_NEAR(out.temp_c, 0.0f, 0.001);
}

KILN_TEST(ad22_a_repeated_sequence_number_ages_into_a_fault)
{
    /* Bytes arriving is not the same as news arriving.  A supervisor repeating
     * one frame would otherwise look perfectly healthy. */
    kiln_suplink_t s;
    kiln_suplink_init(&s);
    send(&s, rep(7u, 500.0f));
    CHECK(kiln_suplink_fresh(&s));

    for (int i = 0; i < 10; i++) {
        send(&s, rep(7u, 500.0f));      /* same seq, over and over */
        kiln_suplink_tick(&s, 0.25f);
    }
    CHECK(!kiln_suplink_fresh(&s));
    CHECK_EQ_UINT(s.repeats, 10u);

    /* A fresh sequence number revives it. */
    send(&s, rep(8u, 500.0f));
    CHECK(kiln_suplink_fresh(&s));
}

KILN_TEST(ad22_the_sequence_number_may_wrap)
{
    kiln_suplink_t s;
    kiln_suplink_init(&s);
    send(&s, rep(255u, 400.0f));
    kiln_suplink_tick(&s, 0.1f);
    send(&s, rep(0u, 401.0f));          /* wrapped, and still news */
    CHECK(kiln_suplink_fresh(&s));
    CHECK_EQ_UINT(s.repeats, 0u);
}

KILN_TEST(ad22_a_frame_split_across_reads_still_arrives)
{
    /* A UART read can split a frame anywhere, including between every byte. */
    kiln_suplink_t s;
    kiln_suplink_init(&s);
    uint8_t f[SUP_FRAME_BYTES];
    const sup_report_t r = rep(3u, 999.9f);
    (void)sup_encode(&r, f, sizeof(f));

    for (size_t i = 0; i < sizeof(f); i++) {
        kiln_suplink_feed(&s, &f[i], 1u);
    }
    CHECK(kiln_suplink_fresh(&s));
    CHECK_EQ_UINT(s.frames, 1u);
}

KILN_TEST(ad22_several_frames_in_one_read_are_all_consumed)
{
    kiln_suplink_t s;
    kiln_suplink_init(&s);
    uint8_t buf[SUP_FRAME_BYTES * 3u];
    for (uint8_t i = 0; i < 3u; i++) {
        const sup_report_t r = rep((uint8_t)(i + 1u), 100.0f * (float)(i + 1));
        (void)sup_encode(&r, &buf[i * SUP_FRAME_BYTES], SUP_FRAME_BYTES);
    }
    kiln_suplink_feed(&s, buf, sizeof(buf));
    CHECK_EQ_UINT(s.frames, 3u);

    kiln_port_tc_t port = {};
    kiln_suplink_bind(&s, &port);
    kiln_tc_reading_t out = {};
    CHECK_OK(port.read(port.ctx, &out));
    CHECK_NEAR(out.temp_c, 300.0f, 0.05);   /* the last one */
}

KILN_TEST(ad22_line_noise_is_resynchronised_past)
{
    kiln_suplink_t s;
    kiln_suplink_init(&s);
    const uint8_t noise[] = { 0xA5, 0x01, 0x02, 0xFF, 0x00, 0xA5, 0xA5 };
    kiln_suplink_feed(&s, noise, sizeof(noise));
    send(&s, rep(1u, 700.0f));
    CHECK(kiln_suplink_fresh(&s));
    CHECK_EQ_UINT(s.frames, 1u);
}

KILN_TEST(ad22_a_corrupt_frame_is_rejected_not_interpreted)
{
    kiln_suplink_t s;
    kiln_suplink_init(&s);
    uint8_t f[SUP_FRAME_BYTES];
    const sup_report_t r = rep(1u, 700.0f);
    (void)sup_encode(&r, f, sizeof(f));
    f[5] ^= 0x10u;                      /* flip a temperature bit */
    kiln_suplink_feed(&s, f, sizeof(f));
    CHECK(!kiln_suplink_fresh(&s));
    CHECK_EQ_UINT(s.frames, 0u);
}

KILN_TEST(ad22_a_buffer_of_pure_noise_does_not_wedge_the_decoder)
{
    /* Otherwise a disconnected, floating line fills the window once and no
     * frame is ever seen again.  This is why the pull-up is specified, and why
     * this works even without it. */
    kiln_suplink_t s;
    kiln_suplink_init(&s);
    uint8_t noise[64];
    for (size_t i = 0; i < sizeof(noise); i++) {
        noise[i] = (uint8_t)(i * 13u + 7u);
    }
    for (int round = 0; round < 20; round++) {
        kiln_suplink_feed(&s, noise, sizeof(noise));
    }
    send(&s, rep(1u, 650.0f));
    CHECK(kiln_suplink_fresh(&s));
}

KILN_TEST(ad22_a_different_protocol_version_is_refused)
{
    /* Guessing at the fields of a version this firmware does not know would be
     * worse than silence. */
    kiln_suplink_t s;
    kiln_suplink_init(&s);
    const sup_report_t r = rep(1u, 700.0f);
    uint8_t f[SUP_FRAME_BYTES];
    (void)sup_encode(&r, f, sizeof(f));
    f[1] = SUP_VERSION + 1u;            /* bump the version */
    f[11] = 0; f[12] = 0;
    const uint16_t crc = sup_crc16(f, 11u);
    f[11] = (uint8_t)(crc & 0xFFu);
    f[12] = (uint8_t)(crc >> 8u);       /* re-CRC so only the version is wrong */

    kiln_suplink_feed(&s, f, sizeof(f));
    CHECK(!kiln_suplink_fresh(&s));
    CHECK_EQ_UINT(s.version_errors, 1u);
}

KILN_TEST(ad22_the_supervisors_fault_bits_pass_straight_through)
{
    /* It reads the same part and reports its status register, so SR-04's
     * existing decoding applies without translation. */
    kiln_suplink_t s;
    kiln_suplink_init(&s);
    kiln_port_tc_t port = {};
    kiln_suplink_bind(&s, &port);

    sup_report_t r = rep(1u, 700.0f);
    r.fault_bits = KILN_TC_FAULT_OPEN;
    send(&s, r);

    kiln_tc_reading_t out = {};
    CHECK_OK(port.read(port.ctx, &out));
    CHECK_EQ_UINT(out.fault_bits, (uint16_t)KILN_TC_FAULT_OPEN);
}

KILN_TEST(ad22_a_trip_is_visible_so_the_operator_can_be_told_why)
{
    kiln_suplink_t s;
    kiln_suplink_init(&s);
    sup_report_t r = rep(1u, 1361.0f);
    r.flags = (uint8_t)(SUP_FLAG_TRIPPED | SUP_FLAG_TC_VALID | SUP_FLAG_SELFTEST_OK);
    r.trip_reason = SUP_TRIP_OVERTEMP;
    send(&s, r);

    sup_report_t got = {};
    CHECK(kiln_suplink_status(&s, &got));
    CHECK((got.flags & SUP_FLAG_TRIPPED) != 0u);
    CHECK((got.flags & SUP_FLAG_PERMIT) == 0u);
    CHECK_EQ_INT(got.trip_reason, SUP_TRIP_OVERTEMP);
}

KILN_TEST(fracq02_the_esp32_may_not_configure_the_chamber_front_end)
{
    /* The supervisor owns it and the type is fixed to K, so that nothing here
     * can change what its backstop means.  Refused rather than ignored, so a
     * caller that still thinks it owns the front end finds out. */
    kiln_suplink_t s;
    kiln_suplink_init(&s);
    kiln_port_tc_t port = {};
    kiln_suplink_bind(&s, &port);
    CHECK_ERR(port.configure(port.ctx, KILN_TC_TYPE_S, 60u), KILN_ERR_UNSUPPORTED);
}

KILN_TEST(nfr17_suplink_refuses_bad_arguments)
{
    kiln_suplink_t s;
    kiln_suplink_init(&s);
    kiln_suplink_init(nullptr);
    kiln_suplink_feed(nullptr, nullptr, 0u);
    kiln_suplink_feed(&s, nullptr, 4u);
    kiln_suplink_tick(nullptr, 0.1f);
    CHECK(!kiln_suplink_fresh(nullptr));
    CHECK(!kiln_suplink_status(nullptr, nullptr));

    kiln_port_tc_t port = {};
    kiln_suplink_bind(&s, &port);
    CHECK_ERR(port.read(port.ctx, nullptr), KILN_ERR_INVALID_ARG);

    /* A negative or NaN dt must not wind the staleness timer backwards. */
    send(&s, rep(1u, 500.0f));
    kiln_suplink_tick(&s, -5.0f);
    CHECK(kiln_suplink_fresh(&s));
}

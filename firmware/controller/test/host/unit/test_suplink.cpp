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
#include "kiln_core/faults.h"
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

/* --- what the operator is told (R12) ------------------------------------ */

KILN_TEST(ad22_the_reason_reaches_the_operators_vocabulary)
{
    kiln_suplink_t s;
    kiln_suplink_init(&s);
    kiln_port_supervisor_t sup = {};
    kiln_suplink_bind_supervisor(&s, &sup);

    sup_report_t r = rep(1u, 1361.0f);
    r.flags       = (uint8_t)(SUP_FLAG_TRIPPED | SUP_FLAG_TC_VALID | SUP_FLAG_SELFTEST_OK);
    r.trip_reason = SUP_TRIP_OVERTEMP;
    send(&s, r);

    kiln_sup_status_t st = {};
    CHECK(sup.status(sup.ctx, &st));
    CHECK_EQ_INT(st.reason, KILN_SUP_OVERTEMP);
    CHECK(st.tripped);
    CHECK(!st.permitting);
    CHECK(st.link_ok);
}

KILN_TEST(ad22_a_silent_supervisor_reads_as_absent_not_as_content)
{
    /* The reason the port has its own enum: a supervisor that has gone quiet
     * cannot report that it is quiet, and the distinction between "the
     * backstop fired" and "the backstop is missing" is the one an operator
     * most needs. */
    kiln_suplink_t s;
    kiln_suplink_init(&s);
    kiln_port_supervisor_t sup = {};
    kiln_suplink_bind_supervisor(&s, &sup);

    kiln_sup_status_t st = {};
    CHECK(!sup.status(sup.ctx, &st));           /* never heard from */
    CHECK_EQ_INT(st.reason, KILN_SUP_LINK_DEAD);

    /* Heard, happy, then silent: the last frame said OK and permitting, and
     * neither may survive the link going away. */
    send(&s, rep(1u, 600.0f));
    CHECK(sup.status(sup.ctx, &st));
    CHECK_EQ_INT(st.reason, KILN_SUP_OK);
    CHECK(st.permitting);

    for (int i = 0; i < 5; i++) { kiln_suplink_tick(&s, 0.25f); }
    CHECK(sup.status(sup.ctx, &st));            /* it did speak, once */
    CHECK_EQ_INT(st.reason, KILN_SUP_LINK_DEAD);
    CHECK(!st.permitting);
    CHECK(!st.link_ok);
}

KILN_TEST(ad22_every_wire_reason_maps_to_one_the_operator_can_read)
{
    const struct { sup_trip_reason_t wire; kiln_sup_reason_t shown; } cases[] = {
        { SUP_TRIP_NONE,         KILN_SUP_OK },
        { SUP_TRIP_OVERTEMP,     KILN_SUP_OVERTEMP },
        { SUP_TRIP_TC_FAULT,     KILN_SUP_TC_FAULT },
        { SUP_TRIP_SENSOR_STALE, KILN_SUP_SENSOR_STALE },
        { SUP_TRIP_SELF_TEST,    KILN_SUP_SELF_TEST },
    };
    for (size_t ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
        const auto &c = cases[ci];
        kiln_suplink_t s;
        kiln_suplink_init(&s);
        sup_report_t r = rep(1u, 500.0f);
        r.trip_reason = c.wire;
        send(&s, r);
        CHECK_EQ_INT(kiln_suplink_reason(&s), c.shown);
    }
}

KILN_TEST(ad22_an_unrecognised_wire_reason_is_a_fault_not_an_ok)
{
    /* Same protocol version, a reason this build does not know.  Reporting it
     * as OK would turn a trip into silence. */
    kiln_suplink_t s;
    kiln_suplink_init(&s);
    const sup_report_t r = rep(1u, 500.0f);
    uint8_t f[SUP_FRAME_BYTES];
    (void)sup_encode(&r, f, sizeof(f));
    f[10] = 200u;                               /* not a known reason */
    const uint16_t crc = sup_crc16(f, 11u);
    f[11] = (uint8_t)(crc & 0xFFu);
    f[12] = (uint8_t)(crc >> 8u);
    kiln_suplink_feed(&s, f, sizeof(f));
    CHECK_EQ_INT(kiln_suplink_reason(&s), KILN_SUP_TC_FAULT);
}

KILN_TEST(nfr23_every_supervisor_reason_has_text_in_both_languages)
{
    /* The same obligation the fault table carries: a reason with no German is
     * a screen that falls back to English in front of an operator who does not
     * read it. */
    for (int i = 0; i < KILN_SUP_REASON_COUNT; i++) {
        const kiln_sup_reason_t r = (kiln_sup_reason_t)i;
        const kiln_lang_t langs[] = { KILN_LANG_EN, KILN_LANG_DE };
        for (size_t li = 0; li < sizeof(langs) / sizeof(langs[0]); li++) {
            const char *label = kiln_sup_reason_label_in(r, langs[li]);
            const char *cause = kiln_sup_reason_cause_in(r, langs[li]);
            CHECK(label != nullptr && label[0] != '\0');
            CHECK(cause != nullptr && cause[0] != '\0');
        }
        /* And the two languages must actually differ, or the row is a stub. */
        if (r != KILN_SUP_OK) {
            CHECK(strcmp(kiln_sup_reason_label_in(r, KILN_LANG_EN),
                         kiln_sup_reason_label_in(r, KILN_LANG_DE)) != 0);
        }
    }
    /* Out of range is answered, not faulted. */
    CHECK_STR_EQ(kiln_sup_reason_label_in((kiln_sup_reason_t)99, KILN_LANG_EN), "?");
}

KILN_TEST(r12_the_link_counters_tell_wiring_apart_from_a_dead_supervisor)
{
    /* Bytes discarded while frames still climb is noise or wiring; frames
     * stopping altogether is a dead supervisor.  The field needs both.
     *
     * Bytes rather than a frame count because resynchronisation slides a
     * window: in noise there are no frame boundaries to count. */
    kiln_suplink_t s;
    kiln_suplink_init(&s);
    kiln_port_supervisor_t sup = {};
    kiln_suplink_bind_supervisor(&s, &sup);

    for (uint8_t i = 1u; i <= 5u; i++) {
        send(&s, rep(i, 500.0f));
        /* a corrupt frame between each good one */
        uint8_t f[SUP_FRAME_BYTES];
        const sup_report_t r = rep((uint8_t)(i + 100u), 500.0f);
        (void)sup_encode(&r, f, sizeof(f));
        f[4] ^= 0x01u;   /* corrupt it */
        kiln_suplink_feed(&s, f, sizeof(f));
    }
    kiln_sup_status_t st = {};
    CHECK(sup.status(sup.ctx, &st));
    CHECK_EQ_UINT(st.frames, 5u);
    CHECK(st.discarded > 0u);
}

/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The MAX31856 register decode, on the host.
 *
 * Every byte pattern below was produced by encoding a known temperature from
 * the datasheet's description of the register layout, not by running the decoder
 * and recording what came out. That distinction is the whole value of the file:
 * a test written from the implementation agrees with a wrong implementation.
 *
 *   linearised TC   19-bit two's complement, 2^-7 degC/LSB,
 *                   in LTCBH[7:0] LTCBM[7:0] LTCBL[7:5]
 *   cold junction   14-bit two's complement, 2^-6 degC/LSB,
 *                   in CJTH[7:0] CJTL[7:2]
 */
#include "kiln_check.h"
#include "sup/max31856.h"
#include "sup/trip.h"

namespace {

/* CJTH, CJTL, LTCBH, LTCBM, LTCBL, SR. */
struct vector {
    uint8_t regs[SUP_TC_BURST_BYTES];
    float   chamber_c;
    float   cj_c;
};

sup_tc_sample_t decode(const uint8_t (&regs)[SUP_TC_BURST_BYTES])
{
    sup_tc_sample_t s = {};
    sup_tc_decode(regs, &s);
    return s;
}

}  // namespace

KILN_TEST(swracq02_the_config_is_type_k_with_open_circuit_detect_and_50hz)
{
    /* SWR-ACQ-02 fixes the type, and the supervisor has no receive path to be
     * told another one, so this is a compile-time fact rather than a setting. */
    CHECK_EQ_UINT(SUP_TC_CR1_VALUE & 0x03u, SUP_TC_CR1_TYPE_K);

    /* The bit that makes SWR-SAF-04 work at all: without OCFAULT the part never
     * reports an open couple, and a backstop whose sensor can fall off
     * silently is not a backstop. */
    CHECK((SUP_TC_CR0_VALUE & SUP_TC_CR0_OCFAULT_1) != 0u);
    CHECK((SUP_TC_CR0_VALUE & SUP_TC_CR0_CMODE_AUTO) != 0u);
    CHECK((SUP_TC_CR0_VALUE & SUP_TC_CR0_FILTER_50) != 0u);

    /* MASK defaults to 0xFF, which masks every fault off the ~FAULT pin. */
    CHECK_EQ_UINT(SUP_TC_MASK_VALUE, 0u);
}

KILN_TEST(swracq02_cr1_readback_rejects_an_absent_part)
{
    CHECK(sup_tc_cr1_ok(SUP_TC_CR1_VALUE));
    /* The two ways a missing part reads, depending on which way the bus
     * floats. Both must fail, because sup_init's selftest_ok argument is only
     * worth something if it can be false. */
    CHECK(!sup_tc_cr1_ok(0x00u));
    CHECK(!sup_tc_cr1_ok(0xFFu));
    CHECK(!sup_tc_cr1_ok(SUP_TC_CR1_VALUE ^ 0x01u));   /* wrong type */
}

KILN_TEST(swracq04_known_temperatures_decode_to_the_values_they_encode)
{
    static const vector v[] = {
        { { 0x19, 0x00, 0x00, 0x00, 0x00, 0x00 },    0.0f, 25.0f },
        { { 0x2A, 0x80, 0x3E, 0x80, 0x00, 0x00 }, 1000.0f, 42.5f },
        { { 0x1E, 0x00, 0x54, 0x58, 0x00, 0x00 }, 1349.5f, 30.0f },
        { { 0x1E, 0x00, 0x54, 0x68, 0x00, 0x00 }, 1350.5f, 30.0f },
    };
    for (const vector &c : v) {
        const sup_tc_sample_t s = decode(c.regs);
        CHECK(s.valid);
        CHECK_EQ_UINT(s.fault_bits, 0u);
        CHECK_NEAR(s.chamber_c, c.chamber_c, 0.01);
        CHECK_NEAR(s.cj_c, c.cj_c, 0.01);
    }
}

KILN_TEST(swrsaf05_a_negative_reading_stays_negative)
{
    /* The reason the shift in the decoder is signed. A logical shift would read
     * -1 degC as +524287, which is above SUP_OVERTEMP_C, so the supervisor
     * would latch an over-temperature on a cold kiln: a nuisance trip, and
     * HZ-10 is about what nuisance trips lead people to do.
     *
     * It also matters the other way: SWR-SAF-05 detects a reversed couple by the
     * reading falling, which it cannot do if negative is unrepresentable. */
    static const vector v[] = {
        { { 0xFB, 0x00, 0xFF, 0x38, 0x00, 0x00 }, -12.5f, -5.0f },
        { { 0x14, 0x00, 0xFF, 0xF0, 0x00, 0x00 },  -1.0f, 20.0f },
    };
    for (const vector &c : v) {
        const sup_tc_sample_t s = decode(c.regs);
        CHECK(s.valid);
        CHECK(s.chamber_c < 0.0f);
        CHECK_NEAR(s.chamber_c, c.chamber_c, 0.01);
        CHECK_NEAR(s.cj_c, c.cj_c, 0.01);
    }
}

KILN_TEST(swrsaf04_a_silent_front_end_is_a_comms_fault_not_a_reading_of_zero)
{
    /* All zeros decodes arithmetically to 0 degC with no fault bits set, which
     * is a plausible, in-range, and entirely wrong reading for a kiln. The
     * burst-level check is what stops a dead bus from being reported as a cold
     * kiln -- which, with a conjunctive permit, would otherwise be the one
     * failure that reads as "safe to heat". */
    const uint8_t zeros[SUP_TC_BURST_BYTES] = { 0, 0, 0, 0, 0, 0 };
    const sup_tc_sample_t z = decode(zeros);
    CHECK(!z.valid);
    CHECK_EQ_UINT(z.fault_bits, (unsigned)SUP_TC_FAULT_COMMS);

    const uint8_t ones[SUP_TC_BURST_BYTES] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    const sup_tc_sample_t o = decode(ones);
    CHECK(!o.valid);
    CHECK_EQ_UINT(o.fault_bits, (unsigned)SUP_TC_FAULT_COMMS);
}

KILN_TEST(swrsaf04_a_null_burst_is_a_comms_fault_and_never_a_reading)
{
    sup_tc_sample_t s = {};
    s.valid = true;                 /* must be overwritten, not left alone */
    sup_tc_decode(nullptr, &s);
    CHECK(!s.valid);
    CHECK_EQ_UINT(s.fault_bits, (unsigned)SUP_TC_FAULT_COMMS);

    sup_tc_decode(nullptr, nullptr);    /* must not fault */
}

KILN_TEST(swracq10_every_status_bit_maps_to_its_fault)
{
    CHECK_EQ_UINT(sup_tc_faults(0u), 0u);
    CHECK_EQ_UINT(sup_tc_faults(SUP_TC_SR_OPEN), (unsigned)SUP_TC_FAULT_OPEN);
    CHECK_EQ_UINT(sup_tc_faults(SUP_TC_SR_OVUV), (unsigned)SUP_TC_FAULT_OVUV);

    /* The part has three ways of saying each of "cold junction" and
     * "thermocouple"; each group collapses to one bit. */
    CHECK_EQ_UINT(sup_tc_faults(SUP_TC_SR_CJ_RANGE), (unsigned)SUP_TC_FAULT_CJ_RANGE);
    CHECK_EQ_UINT(sup_tc_faults(SUP_TC_SR_CJ_HIGH),  (unsigned)SUP_TC_FAULT_CJ_RANGE);
    CHECK_EQ_UINT(sup_tc_faults(SUP_TC_SR_CJ_LOW),   (unsigned)SUP_TC_FAULT_CJ_RANGE);
    CHECK_EQ_UINT(sup_tc_faults(SUP_TC_SR_TC_RANGE), (unsigned)SUP_TC_FAULT_TC_RANGE);
    CHECK_EQ_UINT(sup_tc_faults(SUP_TC_SR_TC_HIGH),  (unsigned)SUP_TC_FAULT_TC_RANGE);
    CHECK_EQ_UINT(sup_tc_faults(SUP_TC_SR_TC_LOW),   (unsigned)SUP_TC_FAULT_TC_RANGE);

    /* Several at once, which is what an open couple on a cold bench looks
     * like: open, and out of range because it reads the rail. */
    CHECK_EQ_UINT(sup_tc_faults(SUP_TC_SR_OPEN | SUP_TC_SR_TC_RANGE),
                  (unsigned)(SUP_TC_FAULT_OPEN | SUP_TC_FAULT_TC_RANGE));
}

KILN_TEST(swrsaf04_a_fault_bit_makes_the_reading_unusable_however_plausible_it_is)
{
    /* 1000 degC is a perfectly reasonable number. With OPEN asserted it is
     * still not a measurement, and valid has to say so: the permit is
     * conjunctive, so an unusable reading withholds heat instead of being
     * weighed against anything. */
    uint8_t regs[SUP_TC_BURST_BYTES] = { 0x2A, 0x80, 0x3E, 0x80, 0x00, 0x00 };
    regs[5] = (uint8_t)SUP_TC_SR_OPEN;
    const sup_tc_sample_t s = decode(regs);
    CHECK(!s.valid);
    CHECK_EQ_UINT(s.fault_bits, (unsigned)SUP_TC_FAULT_OPEN);
    /* The number is still decoded and still reported: the ESP32 shows it, and
     * suppressing it would lose the one clue about what the sensor was doing. */
    CHECK_NEAR(s.chamber_c, 1000.0f, 0.01);
}

KILN_TEST(swa22_a_decoded_reading_drives_the_trip_logic_end_to_end)
{
    /* The two halves joined: bytes off the bus, through the decode, into the
     * trip logic, and out as a permit decision. Neither half is interesting on
     * its own and the seam between them is where a units error would hide. */
    sup_t s;
    sup_init(&s, true);

    const uint8_t hot[SUP_TC_BURST_BYTES]  = { 0x1E, 0x00, 0x54, 0x58, 0x00, 0x00 };
    const uint8_t over[SUP_TC_BURST_BYTES] = { 0x1E, 0x00, 0x54, 0x68, 0x00, 0x00 };

    sup_tc_sample_t sample = decode(hot);
    sup_input_t in = {};
    in.chamber_c     = sample.chamber_c;
    in.chamber_valid = sample.valid;
    in.fault_bits    = sample.fault_bits;
    sup_step(&s, &in, 0.1f);
    CHECK(s.permit);                        /* 1349.5 is below the backstop */
    CHECK(!s.tripped);

    sample          = decode(over);
    in.chamber_c    = sample.chamber_c;
    in.chamber_valid = sample.valid;
    in.fault_bits   = sample.fault_bits;
    sup_step(&s, &in, 0.1f);
    CHECK(!s.permit);                       /* 1350.5 is above it */
    CHECK(s.tripped);
    CHECK_EQ_INT(s.reason, SUP_TRIP_OVERTEMP);
}

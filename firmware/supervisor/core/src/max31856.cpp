/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "sup/max31856.h"

uint16_t sup_tc_faults(unsigned sr)
{
    uint32_t bits = 0u;
    if ((sr & SUP_TC_SR_OPEN) != 0u) { bits |= SUP_TC_FAULT_OPEN; }
    if ((sr & SUP_TC_SR_OVUV) != 0u) { bits |= SUP_TC_FAULT_OVUV; }

    /* The part reports three separate cold-junction complaints and three
     * thermocouple ones; they collapse to one bit each because the reaction is
     * identical and the supervisor has one thing to say about either. */
    if ((sr & (SUP_TC_SR_CJ_RANGE | SUP_TC_SR_CJ_HIGH | SUP_TC_SR_CJ_LOW)) != 0u) {
        bits |= SUP_TC_FAULT_CJ_RANGE;
    }
    if ((sr & (SUP_TC_SR_TC_RANGE | SUP_TC_SR_TC_HIGH | SUP_TC_SR_TC_LOW)) != 0u) {
        bits |= SUP_TC_FAULT_TC_RANGE;
    }
    return (uint16_t)bits;
}

bool sup_tc_cr1_ok(unsigned readback)
{
    return (readback & 0xFFu) == SUP_TC_CR1_VALUE;
}

void sup_tc_decode(const uint8_t *regs, sup_tc_sample_t *out)
{
    if (out == nullptr) {
        return;
    }
    const sup_tc_sample_t zero = {};
    *out = zero;

    if (regs == nullptr) {
        out->fault_bits = (uint16_t)SUP_TC_FAULT_COMMS;
        return;
    }

    /* All ones or all zeros across the whole burst is a part that is not
     * answering: unpowered, absent, or a bus held at one rail. The status
     * register cannot tell us this, because it is one of the bytes that did not
     * arrive. It is reported as a comms fault rather than a thermocouple fault
     * because they call for different things to be looked at. */
    bool all_ff = true;
    bool all_00 = true;
    for (unsigned i = 0; i < SUP_TC_BURST_BYTES; i++) {
        if (regs[i] != 0xFFu) { all_ff = false; }
        if (regs[i] != 0x00u) { all_00 = false; }
    }
    if (all_ff || all_00) {
        out->fault_bits = (uint16_t)SUP_TC_FAULT_COMMS;
        return;
    }

    /* Cold junction: 14 bits, signed, 2^-6 degC per LSB, left-aligned in 16.
     *
     * Assembled unsigned and then reinterpreted, rather than shifted as a
     * signed value. For the hot junction below this is not a matter of taste:
     * regs[2] << 24 with regs[2] >= 0x80 overflows int32_t, which is undefined
     * behaviour, and 0x80 is exactly the case that means "below zero". In
     * uint32_t the shift is defined for every input, and the conversion back is
     * the two's-complement reinterpretation the datasheet describes. */
    const uint16_t cj_bits = (uint16_t)(((uint32_t)regs[0] << 8u) | (uint32_t)regs[1]);
    const int16_t  cj_raw  = (int16_t)cj_bits;
    /* >> 2 drops the two unused low bits and leaves 2^-6 degC per LSB; * 2
     * carries that into the q7 the rest of the supervisor speaks, exactly,
     * because the two units differ by a single power of two.  Written as a
     * multiply rather than a shift so it reads as a change of unit and not as
     * bit twiddling, and because shifting a signed value left is the operation
     * worth not acquiring a habit of. */
    out->cj_q7 = (int32_t)(cj_raw >> 2) * 2;

    /* Linearised hot junction: 19 bits, signed, 2^-7 degC per LSB, left-aligned
     * in 32 across three registers.
     *
     * The shift back down is deliberately signed, and this is the one place the
     * arithmetic has to be: an arithmetic shift sign-extends, and a negative
     * reading has to stay negative. A logical shift would read -1 degC as
     * +524287, which is above the backstop, and the supervisor would trip on a
     * cold kiln with a slightly negative reading. That is a nuisance trip, and
     * HZ-10 is about what nuisance trips lead people to do. */
    const uint32_t tc_bits = ((uint32_t)regs[2] << 24u) |
                             ((uint32_t)regs[3] << 16u) |
                             ((uint32_t)regs[4] << 8u);
    const int32_t  tc_raw  = (int32_t)tc_bits;
    /* And then nothing: the 19-bit value is already 2^-7 degC per LSB, which is
     * q7, so the shift back down is the entire decode.  This is the saving the
     * unit was chosen for. */
    out->chamber_q7 = tc_raw >> 13;

    out->fault_bits = sup_tc_faults(regs[5]);

    /* Valid means the part completed a conversion and reported nothing wrong
     * with it. Any fault bit at all makes the number untrustworthy, and the
     * supervisor's permit is conjunctive, so an untrustworthy number withholds
     * heat rather than being weighed against anything.
     *
     * There is deliberately no range check of our own on top of the part's.
     * The MAX31856 asserts TC_RANGE outside type K's -200..1372 degC, which is
     * above SUP_OVERTEMP, so a genuine runaway crosses the 1350 degC
     * backstop, and latches OVERTEMP, many cycles before the part would call
     * the reading out of range. Adding a second range gate here would only
     * create a window where a real over-temperature was reported as a sensor
     * fault instead. */
    out->valid = (out->fault_bits == 0u);
}

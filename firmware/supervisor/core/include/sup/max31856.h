/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The MAX31856's register layout and the decode of one burst read.
 *
 * This is in sup_core, next to the trip logic, for the reason main.cpp states
 * about itself: everything that *decides* anything is platform-free and tested
 * on the host, and what is left on the board is the part that needs silicon.
 * Turning six register bytes into a temperature and a fault word is arithmetic,
 * and arithmetic nobody can check by reading is exactly what SWA-01 says to put
 * where a test can reach it. Two's-complement reassembly across three bytes
 * with a sign-extending shift is not something to get right by inspection.
 *
 * The board layer keeps the SPI transaction: chip select, byte exchange, and
 * the ~FAULT pin. Those are in main.cpp and are still unverified on hardware.
 *
 * The numbers here are the MAX31856 datasheet's (Maxim/ADI 19-100116, tables 2
 * to 6), and the decode deliberately matches the ESP32's own adapter in
 * kiln_hal_esp32s3/src/hal_tc.cpp bit for bit. That is NOT shared code: SWA-22
 * wants two independent implementations of the safety function, and it gets
 * them. What it does not want is two different readings of the same part, so
 * the register decode agrees on purpose while the decisions made from it do
 * not.
 */
#ifndef SUP_MAX31856_H
#define SUP_MAX31856_H

#include <stdbool.h>
#include <stdint.h>

#include "sup_proto.h"      /* SUP_TC_FAULT_* */

/* --- registers (datasheet table 2) --------------------------------------- */
#define SUP_TC_REG_CR0      0x00u
#define SUP_TC_REG_CR1      0x01u
#define SUP_TC_REG_MASK     0x02u
#define SUP_TC_REG_CJTH     0x0Au   /* the burst read starts here            */
#define SUP_TC_REG_SR       0x0Fu   /* ... and ends here                     */
#define SUP_TC_REG_WRITE    0x80u   /* address | 0x80 selects a write        */

/* CJTH, CJTL, LTCBH, LTCBM, LTCBL, SR: six registers in one transaction.
 *
 * One transaction rather than six is a correctness requirement, not an
 * optimisation: the temperature and the fault status must describe the same
 * conversion, and two transactions can straddle one and produce a reading the
 * fault bits disown. */
#define SUP_TC_BURST_BYTES  6u

/* --- the values written at init (datasheet tables 3 to 5) ---------------- */

/* CR0: continuous conversion, open-circuit detect on, 50 Hz rejection.
 *
 * OCFAULT is the bit that matters most here. It defaults to 00, which means
 * the part will NOT report an open couple, and an over-temperature backstop
 * whose sensor can fall off without saying so is not a backstop. */
#define SUP_TC_CR0_CMODE_AUTO   0x80u
#define SUP_TC_CR0_OCFAULT_1    0x10u   /* open-circuit detect, < 5 kohm      */
#define SUP_TC_CR0_FILTER_50    0x01u   /* 0 = 60 Hz, 1 = 50 Hz (SWR-ACQ-06)   */
#define SUP_TC_CR0_VALUE \
    (SUP_TC_CR0_CMODE_AUTO | SUP_TC_CR0_OCFAULT_1 | SUP_TC_CR0_FILTER_50)

/* CR1: 4-sample averaging and type K.
 *
 * Type K is fixed, not configured: SWR-ACQ-02 settles the thermocouple type, and
 * the supervisor has no receive path to be told a different one. That is the
 * same reason the link is simplex. */
#define SUP_TC_CR1_AVG_4        0x20u
#define SUP_TC_CR1_TYPE_K       0x03u
#define SUP_TC_CR1_VALUE        (SUP_TC_CR1_AVG_4 | SUP_TC_CR1_TYPE_K)

/* MASK: 0x00, every fault allowed through to the ~FAULT pin.
 *
 * It defaults to 0xFF, which masks every fault OFF the pin. Writing this is
 * what makes the ~FAULT input on PA3 mean anything at all, and it is written
 * FIRST so there is no window in which the part is converting while its fault
 * output is still inert. */
#define SUP_TC_MASK_VALUE       0x00u

/* --- status register bits (datasheet table 6) ---------------------------- */
#define SUP_TC_SR_CJ_RANGE  0x80u
#define SUP_TC_SR_TC_RANGE  0x40u
#define SUP_TC_SR_CJ_HIGH   0x20u
#define SUP_TC_SR_CJ_LOW    0x10u
#define SUP_TC_SR_TC_HIGH   0x08u
#define SUP_TC_SR_TC_LOW    0x04u
#define SUP_TC_SR_OVUV      0x02u
#define SUP_TC_SR_OPEN      0x01u

typedef struct {
    float    chamber_c;     /* linearised hot junction, degC                 */
    float    cj_c;          /* cold junction, degC                           */
    uint16_t fault_bits;    /* SUP_TC_FAULT_*, 0 for none                    */
    /* A conversion completed and the part reported nothing wrong with it.
     * False leaves chamber_c at 0, which is never read by a caller that checks
     * this first, and is a deliberately useless value for one that does not. */
    bool     valid;
} sup_tc_sample_t;

/* Map a status register byte to SUP_TC_FAULT_* bits.
 *
 * Taken as unsigned rather than uint8_t because a uint8_t argument promotes to
 * int at every mask inside. */
uint16_t sup_tc_faults(unsigned sr);

/* Decode one SUP_TC_BURST_BYTES burst starting at CJTH.
 *
 * `regs` must hold SUP_TC_BURST_BYTES bytes. A null pointer, or a burst that is
 * all 0x00 or all 0xFF, yields SUP_TC_FAULT_COMMS and valid == false: those two
 * patterns are a part that is absent, unpowered or on a dead bus, which the
 * status register alone cannot report because it is part of the burst that did
 * not arrive. */
void sup_tc_decode(const uint8_t *regs, sup_tc_sample_t *out);

/* True when `readback` is what SUP_TC_CR1_VALUE wrote.
 *
 * The init self-test reads CR1 back rather than trusting the write. A missing
 * part reads 0x00 or 0xFF depending on which way the bus floats, and both are
 * distinguishable from a configured CR1, which is what makes sup_init's
 * selftest_ok argument mean something. */
bool sup_tc_cr1_ok(unsigned readback);

#endif /* SUP_MAX31856_H */

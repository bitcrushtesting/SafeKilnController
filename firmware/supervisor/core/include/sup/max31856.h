/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file max31856.h
 * @brief The MAX31856's register layout and the decode of one burst read.
 *
 * @derivedfrom SWA-22 and SWA-01: everything that decides is platform-free.
 * @safetyclass Produces the measurement the trip logic acts on. A wrong decode
 *              is a wrong temperature, which is the hazard the backstop exists
 *              for, so this is in the tested core rather than on the board.
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
 *
 * @par Provenance of the numbers
 * Every constant below is from the MAX31856 datasheet (Maxim/ADI 19-100116,
 * tables 2 to 6), named rather than written as a literal at the point of use.
 *
 * @verifiedby `test_max31856.cpp`. Its vectors were produced by encoding known
 *             temperatures into the datasheet's register layout, not by
 *             recording what this decoder returned, which would have tested
 *             the decoder against itself.
 */
#ifndef SUP_MAX31856_H
#define SUP_MAX31856_H

#include <stdbool.h>
#include <stdint.h>

#include "sup_proto.h"      /* SUP_TC_FAULT_* */

/**
 * @name Register addresses (datasheet table 2)
 * @rangeof Fixed by the part. Compile-time constants.
 * @{
 */
constexpr uint8_t SUP_TC_REG_CR0 = 0x00u;   /**< Configuration 0. */
constexpr uint8_t SUP_TC_REG_CR1 = 0x01u;   /**< Configuration 1. */
constexpr uint8_t SUP_TC_REG_MASK = 0x02u;  /**< Fault mask. */
constexpr uint8_t SUP_TC_REG_CJTH = 0x0Au;  /**< Cold junction high; the burst read starts here. */
constexpr uint8_t SUP_TC_REG_SR = 0x0Fu;    /**< Status; the burst read ends here. */
constexpr uint8_t SUP_TC_REG_WRITE = 0x80u; /**< OR with an address to select a write. */
/** @} */

/**
 * @brief Bytes in one burst read: CJTH, CJTL, LTCBH, LTCBM, LTCBL, SR.
 *
 * @rangeof 6. Compile-time constant.
 *
 * @rationale
 * One transaction rather than six is a **correctness requirement**, not an
 * optimisation: the temperature and the fault status must describe the same
 * conversion, and two transactions can straddle one and produce a reading the
 * fault bits disown.
 *
 * @implements SWR-SAF-22
 */
constexpr unsigned SUP_TC_BURST_BYTES = 6u;

/* --- the values written at init (datasheet tables 3 to 5) ---------------- */

/**
 * @name CR0: continuous conversion, open-circuit detect, 50 Hz rejection
 *
 * @errorbehaviour
 * OCFAULT is the bit that matters most here. It **defaults to 00**, which
 * means the part will not report an open couple, and an over-temperature
 * backstop whose sensor can fall off without saying so is not a backstop.
 *
 * @implements SWR-ACQ-06
 * @{
 */
constexpr uint8_t SUP_TC_CR0_CMODE_AUTO = 0x80u; /**< Continuous conversion. */
constexpr uint8_t SUP_TC_CR0_OCFAULT_1 = 0x10u;  /**< Open-circuit detect, < 5 kohm. */
constexpr uint8_t SUP_TC_CR0_FILTER_50 = 0x01u;  /**< 0 is 60 Hz, 1 is 50 Hz. */
/** @brief The value written to CR0 at init. */
constexpr uint8_t SUP_TC_CR0_VALUE =
    (uint8_t)(SUP_TC_CR0_CMODE_AUTO | SUP_TC_CR0_OCFAULT_1 | SUP_TC_CR0_FILTER_50);
/** @} */

/**
 * @name CR1: four-sample averaging and type K
 *
 * @rationale
 * Type K is fixed, not configured: SWR-ACQ-02 settles the thermocouple type,
 * and the supervisor has no receive path to be told a different one. That is
 * the same reason the link is simplex.
 *
 * @implements SWR-ACQ-02
 * @{
 */
constexpr uint8_t SUP_TC_CR1_AVG_4 = 0x20u;  /**< Average four samples. */
constexpr uint8_t SUP_TC_CR1_TYPE_K = 0x03u; /**< Thermocouple type K. */
/** @brief The value written to CR1 at init, and read back by @ref sup_tc_cr1_ok. */
constexpr uint8_t SUP_TC_CR1_VALUE = (uint8_t)(SUP_TC_CR1_AVG_4 | SUP_TC_CR1_TYPE_K);
/** @} */

/**
 * @brief MASK: 0x00, every fault allowed through to the ~FAULT pin.
 *
 * @rangeof 0x00. Compile-time constant.
 *
 * @errorbehaviour
 * It **defaults to 0xFF**, which masks every fault off the pin. Writing this
 * is what makes the ~FAULT input on PA3 mean anything at all, and it is
 * written **first** so there is no window in which the part is converting
 * while its fault output is still inert.
 *
 * @implements SWR-SAF-22
 */
constexpr uint8_t SUP_TC_MASK_VALUE = 0x00u;

/**
 * @name Status register bits (datasheet table 6)
 * @rangeof Fixed by the part. @ref sup_tc_faults maps these to the
 *          `SUP_TC_FAULT_*` set that crosses the link.
 * @{
 */
constexpr uint8_t SUP_TC_SR_CJ_RANGE = 0x80u; /**< Cold junction out of range. */
constexpr uint8_t SUP_TC_SR_TC_RANGE = 0x40u; /**< Thermocouple out of range. */
constexpr uint8_t SUP_TC_SR_CJ_HIGH = 0x20u;  /**< Cold junction above its threshold. */
constexpr uint8_t SUP_TC_SR_CJ_LOW = 0x10u;   /**< Cold junction below its threshold. */
constexpr uint8_t SUP_TC_SR_TC_HIGH = 0x08u;  /**< Thermocouple above its threshold. */
constexpr uint8_t SUP_TC_SR_TC_LOW = 0x04u;   /**< Thermocouple below its threshold. */
constexpr uint8_t SUP_TC_SR_OVUV = 0x02u;     /**< Over or under voltage on an input. */
constexpr uint8_t SUP_TC_SR_OPEN = 0x01u;     /**< Open circuit detected. */
/** @} */

/**
 * @brief One decoded burst: two temperatures, a fault word and a verdict.
 *
 * @rangeof Temperatures are q7 (1/128 degC), the part's own unit. See
 *          @ref SUP_Q7_PER_C for why the whole supervisor measures in it, and
 *          note what it buys here: the hot junction is a shift of the register
 *          bytes and nothing more, so there is no scaling step in which a
 *          reading can be rounded, truncated or promoted to a type this
 *          processor cannot multiply.
 */
typedef struct {
    int32_t  chamber_q7; /**< Linearised hot junction, 1/128 degC. */
    int32_t  cj_q7;      /**< Cold junction, 1/128 degC. */
    uint16_t fault_bits; /**< `SUP_TC_FAULT_*` set, 0 for none. */

    /**
     * @brief A conversion completed and the part reported nothing wrong with it.
     *
     * False leaves @ref chamber_q7 at 0, which is never read by a caller that
     * checks this first, and is a deliberately useless value for one that does
     * not.
     */
    bool     valid;
} sup_tc_sample_t;

/**
 * @brief Map a status register byte to `SUP_TC_FAULT_*` bits.
 *
 * @param[in] sr The status register as read.
 * @return The corresponding fault set, 0 when the part reports nothing wrong.
 *
 * @rangeof `sr` is the eight bits of the status register; bits above those are
 *          ignored rather than rejected, since the register is a byte.
 *
 * @rationale
 * Taken as `unsigned` rather than `uint8_t` because a `uint8_t` argument
 * promotes to `int` at every mask inside, which is signed arithmetic in a
 * bitwise expression and the high-integrity profile objects.
 *
 * @sideeffects None. Pure function.
 *
 * @implements SWR-SAF-22
 * @verifiedby `test_max31856.cpp`, each status bit on its own and in
 *             combination.
 */
uint16_t sup_tc_faults(unsigned sr);

/**
 * @brief Decode one burst read into a sample.
 *
 * @param[in]  regs The @ref SUP_TC_BURST_BYTES bytes read starting at CJTH.
 * @param[out] out  The decoded sample. Always written.
 *
 * @rangeof `regs` must hold exactly @ref SUP_TC_BURST_BYTES bytes. The hot
 *          junction is a 19-bit signed value at 2^-7 degC per LSB; the cold
 *          junction is at 2^-6 and is shifted left by one, which is exact.
 *
 * @errorbehaviour
 * A null pointer, or a burst that is all 0x00 or all 0xFF, yields
 * @ref SUP_TC_FAULT_COMMS and `valid == false`. Those two patterns are a part
 * that is absent, unpowered or on a dead bus, which the status register alone
 * **cannot** report, because the status register is part of the burst that did
 * not arrive.
 *
 * @rationale
 * This is arithmetic nobody can check by reading, which is why it is here
 * rather than on the board: two's-complement reassembly across three bytes
 * with a sign-extending shift. A logical shift instead of an arithmetic one
 * reads -1 degC as +524287, which is above the backstop, so a cold kiln would
 * trip. That is the specific mistake the host tests exist to catch.
 *
 * @sideeffects Writes `*out`. No allocation, no I/O.
 *
 * @implements SWR-SAF-22, SWR-ACQ-02
 * @verifiedby `test_max31856.cpp`, with vectors encoded from the datasheet's
 *             layout rather than recorded from this function, including
 *             negative temperatures and both dead-bus patterns.
 */
void sup_tc_decode(const uint8_t *regs, sup_tc_sample_t *out);

/**
 * @brief Whether a CR1 readback matches what was written.
 *
 * @param[in] readback The byte read back from CR1 after configuration.
 * @return True when it equals @ref SUP_TC_CR1_VALUE.
 *
 * @rangeof `readback` is one register byte, taken as `unsigned` for the same
 *          promotion reason as @ref sup_tc_faults.
 *
 * @errorbehaviour
 * A missing part reads 0x00 or 0xFF depending on which way the bus floats, and
 * both are distinguishable from a configured CR1. That is what makes
 * @ref sup_init 's `selftest_ok` argument mean something rather than being an
 * assumption the caller passes along.
 *
 * @rationale
 * The init self-test reads CR1 back rather than trusting the write, because a
 * write to an absent part succeeds exactly as a write to a present one does.
 *
 * @implements SWR-SAF-36
 * @verifiedby `test_max31856.cpp`: the configured value, both float patterns,
 *             and a neighbouring value that must not pass.
 */
bool sup_tc_cr1_ok(unsigned readback);

#endif /* SUP_MAX31856_H */

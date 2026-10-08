/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The supervisor's start-up and periodic self-checks: clock integrity and
 * program-memory integrity.
 *
 * Both are here rather than in main.cpp for the reason the trip logic is: they
 * are decisions about register values and memory contents, they are pure
 * functions of their inputs, and a diagnostic that cannot be exercised on a
 * development host is a diagnostic nobody has tested. main.cpp reads the
 * registers and hands them over; what counts as healthy is decided here.
 *
 * These exist because an assessment against EN IEC 60730-1 Annex H or
 * EN ISO 13849-1 asks for them. Annex H's software classes B and C expect
 * program-memory integrity checking and clock monitoring among their measures,
 * and a single-channel supervisor claims diagnostic coverage only for the
 * failures it can actually observe. The three measures the supervisor now has
 * are this file's two plus the windowed watchdog in main.cpp.
 */
#ifndef SUP_SELFCHECK_H
#define SUP_SELFCHECK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- clock integrity (RM0444 RCC_CR and RCC_CFGR) -----------------------
 *
 * Bit positions are from ST's CMSIS-SVD for this part, like everything in
 * stm32g031.h. What is deliberately NOT used is the SWS *encoding*: the SVD
 * carries no enumerated values for it, and its RCC_CR reset value disagrees
 * with the reference manual, so every check below is phrased as an invariant
 * that holds whatever the encoding turns out to be.
 */
#define SUP_RCC_CR_HSIRDY   (1u << 10)
#define SUP_RCC_CR_PLLON    (1u << 24)
#define SUP_RCC_CFGR_SW_Msk (7u << 0)
#define SUP_RCC_CFGR_SWS_Pos 3u
#define SUP_RCC_CFGR_SWS_Msk (7u << SUP_RCC_CFGR_SWS_Pos)

/* True when the clock tree is the one the firmware was written against.
 *
 * Three invariants, none of which needs the SWS encoding:
 *
 *   SWS == SW.  The clock switch *status* must agree with the clock switch
 *   *selection*. This catches a switch that did not take effect and a source
 *   that changed underneath us, and it is true for any selection.
 *
 *   HSIRDY set. The firmware runs from HSI16 by keeping the reset default
 *   (board_clocks_init states this rather than writing it), so the oscillator
 *   it depends on has to be running.
 *
 *   PLLON clear. SUP_SYSCLK_HZ assumes no PLL. A PLL that became enabled means
 *   the system clock is not 16 MHz, and every timer derived from it is wrong.
 *
 * Returning false leads to SUP_TRIP_SELF_TEST, which never permits heat and
 * which sup_clear deliberately refuses to clear: a supervisor that cannot
 * trust its own time base must not be talked back into service by a button. */
bool sup_clock_ok(uint32_t rcc_cr, uint32_t rcc_cfgr);

/* --- program memory integrity -------------------------------------------
 *
 * CRC-32, the standard reflected polynomial, computed in software.
 *
 * Software rather than the STM32's CRC peripheral on purpose: it makes the
 * computation a pure function that the host tests exercise against known
 * vectors, which is worth more than the handful of microseconds the peripheral
 * would save on a check that runs once per boot over 4 kB.
 */
uint32_t sup_crc32(const void *data, size_t len);

/* True when `len` bytes at `data` hash to `expected`.
 *
 * The expected value is patched into the image after linking by
 * tools/sup-crc.py, over the region the linker script marks. A corrupted or
 * partially-programmed image therefore fails to match and the supervisor comes
 * up refusing heat, rather than running whatever the flash happens to hold.
 *
 * SUP_CRC_UNPROGRAMMED is what the placeholder reads before the tool has run.
 * It is treated as a FAILURE and not as "skip the check", because an image that
 * reached a board without passing through the build step that stamps it is
 * exactly the image whose integrity is unknown. */
#define SUP_CRC_UNPROGRAMMED 0xFFFFFFFFu

bool sup_flash_ok(const void *data, size_t len, uint32_t expected);

#endif /* SUP_SELFCHECK_H */

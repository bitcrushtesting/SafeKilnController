/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Cortex-M0+ core peripherals the supervisor uses, which at present is
 * SysTick and nothing else.
 *
 * This is a separate header from stm32g031.h on purpose, and the reason is not
 * tidiness. stm32g031.h is GENERATED from ST's CMSIS-SVD by
 * tools/gen-stm32g031-header.py and says at the top to regenerate rather than
 * edit; core peripherals are ARM's, not ST's, and are not in that SVD at all.
 * Putting SysTick in there would mean either hand-editing a generated file or
 * teaching the generator about registers its source does not contain.
 *
 * The addresses below are architectural: they are fixed by the ARMv6-M
 * Architecture Reference Manual (B3.3, "System timer, SysTick") and are the
 * same on every Cortex-M0+ implementation, which is why they can be written
 * down here without a vendor file to derive them from.
 */
/**
 * @file cortex_m0plus.h
 * @brief Cortex-M0+ core registers used by the supervisor.
 *
 * @derivedfrom SWA-22.
 *
 * Core peripherals rather than STM32 ones: SysTick and the registers the
 * start-up path touches. Separate from `stm32g031.h` because that file is
 * generated from ST's CMSIS-SVD and this is the ARM core underneath it.
 */

#ifndef SUP_CORTEX_M0PLUS_H
#define SUP_CORTEX_M0PLUS_H

#include <stdint.h>

#include "stm32g031.h"      /* for SUP_REG32 */

/* --- SysTick (ARMv6-M B3.3) ---------------------------------------------- */
#define SYST_BASE      0xE000E010u

#define SYST_CSR       SUP_REG32(SYST_BASE + 0x00u)   /* control and status  */
#define SYST_RVR       SUP_REG32(SYST_BASE + 0x04u)   /* reload value        */
#define SYST_CVR       SUP_REG32(SYST_BASE + 0x08u)   /* current value       */
#define SYST_CALIB     SUP_REG32(SYST_BASE + 0x0Cu)   /* calibration         */

#define SYST_CSR_ENABLE     (1u << 0)   /* counter running                   */
#define SYST_CSR_TICKINT    (1u << 1)   /* exception on reaching zero        */
#define SYST_CSR_CLKSOURCE  (1u << 2)   /* 1 = processor clock               */
/* Set when the counter has reached zero since this register was last read.
 * READING CSR CLEARS IT, which is the whole mechanism the cycle wait below
 * depends on: the flag is a one-shot event, not a level. */
#define SYST_CSR_COUNTFLAG  (1u << 16)

/* RVR is 24 bits.  A reload value that does not fit is not a slow tick, it is
 * a silently truncated one, so the period calculation has to be checked
 * against this rather than assumed to fit. */
#define SYST_RVR_MAX        0x00FFFFFFu

#endif /* SUP_CORTEX_M0PLUS_H */

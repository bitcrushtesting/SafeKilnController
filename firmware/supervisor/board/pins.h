/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The supervisor's pin map for the STM32G031K8T6 (LQFP32).
 *
 * Taken from ST's own data, not from recollection: the package pinout and the
 * alternate-function numbers come from STM32CubeMX's MCU database
 * (db/mcu/STM32G031K(4-6-8)Tx.xml and the GPIO modes file for
 * STM32G03x_gpio_v1_0), and the register bit positions in stm32g031.h come
 * from the CMSIS-SVD.  Both ship with ST's tools, so neither is a build-time
 * fetch (UR-CON-04).
 *
 * ---------------------------------------------------------------------------
 * Three properties this assignment holds on purpose
 * ---------------------------------------------------------------------------
 * 1. EVERYTHING IS ON PORT A.  startup.cpp drives the permit line low before
 *    .data is copied, and to do that it enables exactly one GPIO clock.  A
 *    function that moved to port B would silently break that, because the
 *    permit pin would be driven before its port had a clock.
 *
 * 2. PA9 TO PA12 ARE AVOIDED ENTIRELY.  On this package PA11/PA12 can be
 *    remapped to behave as PA9/PA10 through SYSCFG_CFGR1, and positions 19 and
 *    21 are listed as NC-or-PA9/PA10 depending on bonding.  A supervisor's pin
 *    map should not depend on a remap bit being right.
 *
 * 3. THE UART's RECEIVE PIN IS NEVER CONFIGURED.  The link is simplex
 *    (SWR-ACQ-02 fixed the thermocouple type, so there is nothing to send the
 *    supervisor).  USART2_RX would be PA3, and PA3 is used as a plain input
 *    instead, so the receiver is not merely unused, it is unwired.
 */
#ifndef SUP_PINS_H
#define SUP_PINS_H

#include "stm32g031.h"

/* --- the assignment ------------------------------------------------------
 *
 *   pin   pos  function                        mode
 *   PA0     7  clear button                    input, pull-up
 *   PA2     9  UART TX to the ESP32            AF1, USART2_TX
 *   PA3    10  MAX31856 ~FAULT                 input, pull-up
 *   PA4    11  MAX31856 ~CS                    output, idle high
 *   PA5    12  SPI1 SCK                        AF0
 *   PA6    13  SPI1 MISO                       AF0
 *   PA7    14  SPI1 MOSI                       AF0
 *   PA8    18  coil permit                     output, low = coil open
 *   PA13   24  SWDIO                           reserved, do not use
 *   PA14   25  SWCLK                           reserved, do not use
 *   PF2     6  NRST
 *
 * PA5/PA6/PA7 are adjacent and all AF0, which puts the whole SPI bus on three
 * neighbouring pins next to the chip select on PA4.  Free for later: PA1,
 * PA15, PB0 to PB9, PC6, PC14, PC15. */

#define SUP_PIN_CLEAR        0u   /* PA0,  pos 7   */
#define SUP_PIN_UART_TX      2u   /* PA2,  pos 9   */
#define SUP_PIN_TC_FAULT     3u   /* PA3,  pos 10  */
#define SUP_PIN_TC_CS        4u   /* PA4,  pos 11  */
#define SUP_PIN_SPI_SCK      5u   /* PA5,  pos 12  */
#define SUP_PIN_SPI_MISO     6u   /* PA6,  pos 13  */
#define SUP_PIN_SPI_MOSI     7u   /* PA7,  pos 14  */
#define SUP_PIN_PERMIT       8u   /* PA8,  pos 18  */

/* From the GPIO modes database for this part, not assumed. */
#define SUP_AF_SPI1          0u   /* SCK, MISO, MOSI on PA5/PA6/PA7 */
#define SUP_AF_USART2        1u   /* TX on PA2                      */

/* Peripheral clock enables, bit positions from the SVD. */
#define SUP_RCC_IOPENR_PORTA     (1u << 0)    /* IOPAEN   */
#define SUP_RCC_APBENR2_SPI1     (1u << 12)   /* SPI1EN   */
#define SUP_RCC_APBENR1_USART2   (1u << 17)   /* USART2EN */

/* Used by startup.cpp before .data exists; see property 1 above. */
#define SUP_RCC_IOPENR_PERMIT_PORT  SUP_RCC_IOPENR_PORTA
#define SUP_PERMIT_GPIO_BSRR        GPIOA_BSRR
#define SUP_PERMIT_GPIO_MODER       GPIOA_MODER

/* Active high, so an unpowered or resetting supervisor opens the coil path.
 *
 * This places a requirement on the hardware: every GPIO is a high-impedance
 * input between reset and the first instruction, so the series element must be
 * held OFF by an external pull-down rather than by this pin.  Without it there
 * is a window at every reset where the element's state is whatever the board
 * leaks to, which is the inverse of the open-drain problem tasklist K1 records
 * about the chain this replaces. */
#define SUP_PERMIT_ACTIVE_HIGH 1

#endif /* SUP_PINS_H */

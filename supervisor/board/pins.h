/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The supervisor's pin map, in one file so confirming it against the datasheet
 * is a one-file review.
 *
 * ---------------------------------------------------------------------------
 * UNCONFIRMED: alternate-function numbers and the physical pinout
 * ---------------------------------------------------------------------------
 * The peripheral addresses in stm32g031.h come from ST's SVD and are
 * authoritative.  The AF *mappings* below do not: an SVD does not carry them,
 * they live in the datasheet's alternate-function table, and they differ
 * between packages.  They are marked and gathered here rather than scattered,
 * and tasklist R6 covers checking them against the STM32G031K8 datasheet
 * before anything is fabricated.  Nothing else in this firmware depends on
 * them.
 */
#ifndef SUP_PINS_H
#define SUP_PINS_H

#include "stm32g031.h"

/* --- port A, everything the supervisor uses ----------------------------- */
#define SUP_PIN_SPI_SCK     1u   /* to MAX31856 SCK    AF0  (confirm)       */
#define SUP_PIN_SPI_MISO    6u   /* from MAX31856 SDO  AF0  (confirm)       */
#define SUP_PIN_SPI_MOSI    7u   /* to MAX31856 SDI    AF0  (confirm)       */
#define SUP_PIN_TC_CS       4u   /* MAX31856 ~CS, plain output              */
#define SUP_PIN_TC_FAULT    5u   /* MAX31856 ~FAULT, input, pulled up       */
#define SUP_PIN_LID         8u   /* lid switch, NC, input pulled up         */
#define SUP_PIN_PERMIT      9u   /* coil series element, high permits       */
#define SUP_PIN_CLEAR      10u   /* local clear button, input pulled up     */
#define SUP_PIN_UART_TX     2u   /* USART2 TX to the ESP32  AF1  (confirm)  */

#define SUP_AF_SPI1         0u   /* confirm against the datasheet */
#define SUP_AF_USART2       1u   /* confirm against the datasheet */

/* Everything is on port A, which is why the startup path only enables one
 * clock before it drives the permit line low. */
#define SUP_RCC_IOPENR_PERMIT_PORT  (1u << 0)   /* GPIOAEN */
#define SUP_PERMIT_GPIO_BSRR        GPIOA_BSRR
#define SUP_PERMIT_GPIO_MODER       GPIOA_MODER

/* The permit line is active high and must be driven low to open the coil
 * path, so an unpowered or resetting supervisor removes heat.  This is the
 * opposite polarity to the open-drain chain it replaces, which tasklist K1
 * records as being closed when its front end is absent. */
#define SUP_PERMIT_ACTIVE_HIGH 1

#endif /* SUP_PINS_H */

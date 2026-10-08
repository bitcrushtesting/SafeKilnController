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
 * 1. THE PERMIT LINE IS ON PORT A, AND MUST STAY THERE.  startup.cpp drives it
 *    low before .data is copied, and to do that it enables exactly one GPIO
 *    clock.  A permit pin that moved to port B would silently break that,
 *    because it would be driven before its port had a clock.
 *
 *    Port B is now in use for the second thermocouple's SPI bus, which is
 *    fine: nothing on port B is touched before main() enables IOPBEN.  The
 *    property that matters is specifically about the permit pin, and it is
 *    stated that way now rather than as "everything on port A", which was the
 *    shape of the rule rather than its reason.
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
 *   PA1     8  coil permit readback            input, no pull
 *   PA2     9  UART TX to the ESP32            AF1, USART2_TX
 *   PA3    10  TC1 ~FAULT                      input, pull-up
 *   PA4    11  TC1 ~CS                         output, idle high
 *   PA5    12  SPI1 SCK                        AF0
 *   PA6    13  SPI1 MISO                       AF0
 *   PA7    14  SPI1 MOSI                       AF0
 *   PA8    18  coil permit                     output, low = coil open
 *   PB4    28  TC2 ~FAULT                      input, pull-up
 *   PB5    29  TC2 ~CS                         output, idle high
 *   PB6    30  SPI2 MISO                       AF4   <- note the AF
 *   PB7    31  SPI2 MOSI                       AF1
 *   PB8    32  SPI2 SCK                        AF1
 *   PA13   24  SWDIO                           reserved, do not use
 *   PA14   25  SWCLK                           reserved, do not use
 *   PF2     6  NRST
 *
 * PA5/PA6/PA7 are adjacent and all AF0, which puts the first SPI bus on three
 * neighbouring pins next to its chip select on PA4.  PB6/PB7/PB8 do the same
 * for the second, next to its chip select on PB5 and its fault input on PB4,
 * so each channel is one contiguous run: PA3 to PA7, and PB4 to PB8.
 *
 * THE SPI2 ALTERNATE FUNCTIONS ARE NOT ALL THE SAME NUMBER.  MISO is AF4 while
 * SCK and MOSI are AF1, which is unlike SPI1 where all three are AF0.  Taken
 * from the GPIO modes database, and worth the shouting: a single SUP_AF_SPI2
 * constant applied to all three would configure MISO as something else
 * entirely and the bus would read nothing, with no error anywhere.
 *
 * Why PB6/PB7/PB8 and not somewhere on port A: on this package the only SPI2
 * pins are PA0/PB8 for SCK, PA3/PB2/PA9/PA11/PB6 for MISO, PA4/PA10/PA12/PB7
 * for MOSI and PB9/PA8 for NSS.  Every port A option is either already taken
 * (PA0 clear, PA3 fault, PA4 chip select, PA8 permit) or inside the PA9..PA12
 * range that property 2 below refuses to depend on.  Port B is what is left,
 * and it is adjacent and contiguous.
 *
 * Free for later: PA15, PB0 to PB3, PB9, PC6, PC14, PC15. */

#define SUP_PIN_CLEAR        0u   /* PA0,  pos 7   */
#define SUP_PIN_PERMIT_SENSE 1u   /* PA1,  pos 8   */
#define SUP_PIN_UART_TX      2u   /* PA2,  pos 9   */
#define SUP_PIN_TC_FAULT     3u   /* PA3,  pos 10  */
#define SUP_PIN_TC_CS        4u   /* PA4,  pos 11  */
#define SUP_PIN_SPI_SCK      5u   /* PA5,  pos 12  */
#define SUP_PIN_SPI_MISO     6u   /* PA6,  pos 13  */
#define SUP_PIN_SPI_MOSI     7u   /* PA7,  pos 14  */
#define SUP_PIN_PERMIT       8u   /* PA8,  pos 18  */

/* --- port B: the second thermocouple's bus (SWR-SAF-37) -----------------
 *
 * PB4 is a plain GPIO on this package with no competing function, which is why
 * the fault input went there rather than onto PC14/PC15: those are the OSC32
 * pins, and nothing is using them only because the supervisor runs from the LSI.
 * Putting a safety input on them would foreclose ever fitting an LSE crystal,
 * which is the obvious upgrade for the IWDG window check's timing accuracy. */
#define SUP_PIN_TC2_FAULT    4u   /* PB4,  pos 28  */
#define SUP_PIN_TC2_CS       5u   /* PB5,  pos 29  */
#define SUP_PIN_SPI2_MISO    6u   /* PB6,  pos 30  */
#define SUP_PIN_SPI2_MOSI    7u   /* PB7,  pos 31  */
#define SUP_PIN_SPI2_SCK     8u   /* PB8,  pos 32  */

/* From the GPIO modes database for this part, not assumed.
 *
 * Three separate constants for SPI2 because the three pins genuinely use three
 * different alternate functions.  See the warning in the header comment. */
#define SUP_AF_SPI1          0u   /* SCK, MISO, MOSI on PA5/PA6/PA7 */
#define SUP_AF_USART2        1u   /* TX on PA2                      */
#define SUP_AF_SPI2_SCK      1u   /* PB8                            */
#define SUP_AF_SPI2_MOSI     1u   /* PB7                            */
#define SUP_AF_SPI2_MISO     4u   /* PB6, and NOT 1                 */

/* Peripheral clock enables, bit positions from the SVD. */
#define SUP_RCC_IOPENR_PORTA     (1u << 0)    /* IOPAEN   */
#define SUP_RCC_IOPENR_PORTB     (1u << 1)    /* IOPBEN   */
#define SUP_RCC_APBENR2_SPI1     (1u << 12)   /* SPI1EN   */
#define SUP_RCC_APBENR1_SPI2     (1u << 14)   /* SPI2EN   */
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

/* --- the permit readback (SWR-SAF-38) -----------------------------------
 *
 * PA1 senses the coil side of the permit path, so the supervisor can tell
 * whether the line it drove actually did anything.  Without it the output stage
 * is the one part of the chain with no diagnostic at all: the supervisor could
 * command the coil open for an hour against a shorted drive transistor and
 * report everything healthy.
 *
 * This places a requirement on the board, which does not yet carry it: a divider
 * from the coil drive node to PA1, scaled to 3V3 logic and referenced to the
 * supervisor's ground.  No pull is configured, because the divider defines the
 * level and a pull-up would fight it.
 *
 * Sense polarity is stated here rather than inferred: HIGH means the drive node
 * is energised, which is to say the permit is being delivered.  A board that
 * inverts it must change this, and the firmware checks sense against command, so
 * getting it backwards fails loudly on the first cycle rather than quietly
 * agreeing half the time. */
#define SUP_PERMIT_SENSE_ACTIVE_HIGH 1

/* --- why the second couple has a ~FAULT pin too -------------------------
 *
 * The status register carries the same fault bits over SPI, so this pin adds no
 * information the cycle does not already read.  What it adds is INDEPENDENCE:
 * the status register arrives over the same bus as the data it qualifies, so a
 * bus failure makes the fault bits unreadable at exactly the moment they matter.
 * The all-0xFF/all-0x00 burst check does catch a dead bus and reports it as a
 * comms fault, so the condition is not missed, but it is caught by one channel
 * inferring that another has gone quiet rather than by an independent
 * indication.
 *
 * Having gone to the trouble of two front ends on two buses for SWR-SAF-37,
 * leaving the fault indication dependent on one of those buses would be
 * inconsistent.  The pin costs one GPIO, adjacent to the bus it belongs to.
 *
 * Both parts need MASK = 0x00 before the pin means anything, and both get it:
 * tc_init writes it first, for whichever front end it is handed. */

/* --- the pin map, checked at compile time --------------------------------
 *
 * Added when port B reached five pins, because a collision there is otherwise
 * silent: two names for the same bit compile, link and run, and the symptom is
 * one peripheral configuring another's pin somewhere in the middle of
 * board_gpio_init. Nothing downstream complains.
 *
 * Sum-equals-OR is the whole trick. Both fold the same (1u << pin) terms, but
 * addition carries where a bitwise OR absorbs, so the two agree if and only if
 * no bit appears twice. No <bit>, no popcount, no constexpr function: it holds
 * in a freestanding build. */
#define SUP_BIT(p) (1u << (p))

/* Written out rather than folded by a variadic macro: the preprocessor does not
 * rescan a list-valued macro into separate arguments, so the clever version
 * does not compile, and the version that does would need an apply-indirection
 * layer to read correctly. Thirteen terms twice is worth not having that. */
#define SUP_PORTA_SUM                                                        \
    (SUP_BIT(SUP_PIN_CLEAR)      + SUP_BIT(SUP_PIN_PERMIT_SENSE) +           \
     SUP_BIT(SUP_PIN_UART_TX)    + SUP_BIT(SUP_PIN_TC_FAULT)     +           \
     SUP_BIT(SUP_PIN_TC_CS)      + SUP_BIT(SUP_PIN_SPI_SCK)      +           \
     SUP_BIT(SUP_PIN_SPI_MISO)   + SUP_BIT(SUP_PIN_SPI_MOSI)     +           \
     SUP_BIT(SUP_PIN_PERMIT))

#define SUP_PORTA_OR                                                         \
    (SUP_BIT(SUP_PIN_CLEAR)      | SUP_BIT(SUP_PIN_PERMIT_SENSE) |           \
     SUP_BIT(SUP_PIN_UART_TX)    | SUP_BIT(SUP_PIN_TC_FAULT)     |           \
     SUP_BIT(SUP_PIN_TC_CS)      | SUP_BIT(SUP_PIN_SPI_SCK)      |           \
     SUP_BIT(SUP_PIN_SPI_MISO)   | SUP_BIT(SUP_PIN_SPI_MOSI)     |           \
     SUP_BIT(SUP_PIN_PERMIT))

#define SUP_PORTB_SUM                                                        \
    (SUP_BIT(SUP_PIN_TC2_FAULT)  + SUP_BIT(SUP_PIN_TC2_CS)       +           \
     SUP_BIT(SUP_PIN_SPI2_MISO)  + SUP_BIT(SUP_PIN_SPI2_MOSI)    +           \
     SUP_BIT(SUP_PIN_SPI2_SCK))

#define SUP_PORTB_OR                                                         \
    (SUP_BIT(SUP_PIN_TC2_FAULT)  | SUP_BIT(SUP_PIN_TC2_CS)       |           \
     SUP_BIT(SUP_PIN_SPI2_MISO)  | SUP_BIT(SUP_PIN_SPI2_MOSI)    |           \
     SUP_BIT(SUP_PIN_SPI2_SCK))

#ifdef __cplusplus
static_assert(SUP_PORTA_SUM == SUP_PORTA_OR, "two port A pins share a bit");
static_assert(SUP_PORTB_SUM == SUP_PORTB_OR, "two port B pins share a bit");

/* Property 2 of the header comment, as a check rather than a paragraph: nothing
 * may land on PA9..PA12, whose bonding and SYSCFG remap this map refuses to
 * depend on. PA13/PA14 are SWD and the same mask covers them. */
static_assert((SUP_PORTA_OR & 0x7E00u) == 0u, "a port A pin landed on PA9..PA14");
#endif

#endif /* SUP_PINS_H */

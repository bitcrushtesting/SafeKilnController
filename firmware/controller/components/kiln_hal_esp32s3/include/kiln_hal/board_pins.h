/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pin map for the safekiln rev A board (SYS-HW-10).
 *
 * SYS-HW-10 requires pin assignments to be defined in one place per board variant
 * and not duplicated across the codebase.  This is that place.  Every adapter
 * in this component takes its pins from here, and nothing else in the firmware
 * mentions a GPIO number.
 *
 * Transcribed from hardware/safekiln.kicad_sch and verified against the
 * netlist rather than the drawing, so the names below are the net names.
 *
 * ---------------------------------------------------------------------------
 * SYS-HW-08: pins that must not be used
 * ---------------------------------------------------------------------------
 * The ESP32-S3 samples IO0, IO3, IO45 and IO46 at reset to choose boot mode,
 * JTAG source and flash voltage.  They are excluded from anything safety
 * relevant: a strapping pin is driven by the bootloader before the firmware
 * runs, so a heat output on one would glitch during reset, and an input on one
 * is read by the ROM before it is read by us.  IO19 and IO20 are the USB data
 * pair.
 *
 * ---------------------------------------------------------------------------
 * Only ADC1 is usable, which is why there is only one CT
 * ---------------------------------------------------------------------------
 * On the ESP32-S3, ADC1 is GPIO1..GPIO10 and ADC2 is GPIO11..GPIO20.  **ADC2
 * cannot be used while WiFi is active**, which for this device means it cannot
 * be used at all (SWR-NET-01).  CURR_SENSE on IO1 is the only analogue input
 * the board uses.
 *
 * That costs nothing today: the project supports single-phase kilns only, and
 * a single-phase kiln needs exactly one transformer (SYS-HW-11).  It is written
 * down because it will bite whoever next wants an analogue input for anything
 * at all, and because it is the reason three-phase support was dropped rather
 * than merely deferred.  IO9 and IO10 are ADC1 and have been unrouted since the
 * thermocouple bus moved to the supervisor (HWR-05); IO3 is ADC1 too, but a
 * strapping pin, and IO5 is held for SYS-HW-12.
 */
#ifndef KILN_HAL_BOARD_PINS_H
#define KILN_HAL_BOARD_PINS_H

#include <stdint.h>

/* --- thermocouple front ends (SYS-HW-02, HWR-05) -------------------------------- */
/* None on this side.  Both MAX31856s are the supervisor's, each on its own SPI
 * bus, and HWR-05 forbids an ESP32-S3 pin on either, so there is no SPI, chip
 * select, DRDY or FAULT pin to give here.  The chamber temperature arrives over
 * the supervisor link below.
 *
 * The enclosure channel SYS-HW-02 used to put on this side has no front end on
 * rev A, and how it is to be measured is open.  This is 0 rather than absent so
 * that hal_tc.cpp can say so: kiln_hal_tc_init then returns KILN_ERR_UNSUPPORTED
 * and SWR-SAF-11 stands down, instead of binding a channel to pins that go
 * nowhere.  The SPI driver does not notice a missing part, so without this the
 * enclosure port would come up and read whatever a floating MISO reads. */
#define KILN_HAL_BOARD_HAS_CASE_TC 0

/* --- heat output (SYS-HW-06, SYS-HW-07, SWA-05) --------------------------------- */
constexpr int KILN_PIN_SSR1 = 4;
/* IO5 is not routed on rev A: the board has one SSR output, J6.  The pin is kept
 * for SYS-HW-12's optional second channel, and driving an unrouted pin low is
 * harmless. */
constexpr int KILN_PIN_SSR2 = 5;
/* SWA-05: a software-generated square wave into a charge pump, never a static
 * level and never a hardware PWM peripheral.  See hal_heat.cpp. */
constexpr int KILN_PIN_HEAT_EN = 6;

/* --- annunciation (SYS-HW-09) ---------------------------------------------- */
constexpr int KILN_PIN_ALARM = 7;

/* --- heater current (SYS-HW-11, SYS-HW-17) ------------------------------------- */
/* ADC1_CH0, and the only analogue input on the board; see above. */
constexpr int KILN_PIN_CURR_SENSE = 1;
constexpr int KILN_HAL_CURR_ADC_UNIT = 1;
constexpr int KILN_HAL_CURR_ADC_CHANNEL = 0;

/* --- interlocks -------------------------------------------------------- */
/* SWR-SAF-31 / SYS-HW-21.  The lid contacts are in the coil circuit; this senses the
 * node through the R30/R31 divider, so it reads the *coil supply*, not a bare
 * switch: high means the lid is shut and the coil may be fed.  The divider is
 * sized for the internal pull-down hal_io.cpp enables (HWR-27), and D10 clamps
 * the pin, so turning that pull-down off moves the threshold. */
constexpr int KILN_PIN_LID_SENSE = 38;

/* --- local interface (SYS-HW-04, SYS-HW-05) ------------------------------------ */
constexpr int KILN_PIN_I2C_SDA = 8;
constexpr int KILN_PIN_I2C_SCL = 18;
constexpr uint8_t KILN_HAL_OLED_ADDR = 0x3C;
constexpr int KILN_PIN_ENC_A = 16;
constexpr int KILN_PIN_ENC_B = 17;
constexpr int KILN_PIN_ENC_BTN = 15;

/* --- the supervisor link (SWA-22) ---------------------------------------
 *
 * One wire, receive only.  The supervisor transmits at 10 Hz and is told
 * nothing, so no TX pin is assigned here or bound in the UART driver: the
 * simplex link is structural at both ends rather than conventional at one.
 *
 * IO47 because it is free on every ESP32-S3-WROOM-1 variant, is not a
 * strapping pin (those are IO0, IO3, IO45, IO46 and the supervisor's TX idles
 * high, which would hold a strap through boot), is not JTAG (IO39 to IO42),
 * and is neither USB nor the console.  IO35 to IO37 were avoided because the
 * octal-PSRAM module variants consume them internally.
 *
 * The line needs a 10k pull-up to 3V3 at this end: the supervisor's TX is
 * high-impedance between its reset and the moment it configures the pin, and
 * permanently so if it is absent.  Idling high is the UART's idle state, so an
 * absent supervisor presents as silence, which kiln_suplink already treats as
 * a comms fault.  Without the pull-up it presents as noise, which looks like a
 * different fault. */
constexpr int KILN_PIN_SUP_RX = 47;
constexpr int KILN_HAL_SUP_UART = 1;   /* UART0 is the console */

/* --- expansion --------------------------------------------------------- */
constexpr int KILN_PIN_EXP_IO2 = 2;
constexpr int KILN_PIN_EXP_IO42 = 42;

#endif /* KILN_HAL_BOARD_PINS_H */

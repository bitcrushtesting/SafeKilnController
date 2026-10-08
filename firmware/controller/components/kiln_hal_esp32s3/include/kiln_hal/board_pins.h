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
 * Only one ADC channel is available, which is why there is only one CT
 * ---------------------------------------------------------------------------
 * On the ESP32-S3, ADC1 is GPIO1..GPIO10 and ADC2 is GPIO11..GPIO20.  **ADC2
 * cannot be used while WiFi is active**, which for this device means it cannot
 * be used at all (SWR-NET-01).  Every GPIO1..GPIO10 pin on this board is taken,
 * so CURR_SENSE is the only analogue input there is and there is no room for
 * a second.
 *
 * That costs nothing today: the project supports single-phase kilns only, and
 * a single-phase kiln needs exactly one transformer (SYS-HW-11).  It is written
 * down because it will bite whoever next wants an analogue input for anything
 * at all, and because it is the reason three-phase support was dropped rather
 * than merely deferred.
 */
#ifndef KILN_HAL_BOARD_PINS_H
#define KILN_HAL_BOARD_PINS_H

/* --- thermocouple front ends (SYS-HW-02, SYS-HW-03) ---------------------------- */
/* One SPI bus, two chip selects.  SPI2 (FSPI) because SPI0/SPI1 are the flash
 * controller's and SYS-HW-03 wants the TC bus free of anything that can stall it. */
constexpr int KILN_PIN_SPI_SCK = 12;
constexpr int KILN_PIN_SPI_MOSI = 11;
constexpr int KILN_PIN_SPI_MISO = 13;
constexpr int KILN_PIN_TC1_CS = 10;                     /* chamber   */
constexpr int KILN_PIN_TC2_CS = 9;                      /* enclosure */
constexpr int KILN_PIN_TC1_DRDY = 14;
constexpr int KILN_PIN_TC2_DRDY = 21;
/* The FAULT outputs are not read by the firmware: SYS-HW-24 wires them into the
 * contactor coil in hardware, and SWR-SAF-04 reads the fault register over SPI.
 * They are deliberately absent from this map. */

/* --- heat output (SYS-HW-06, SYS-HW-07, SWA-05) --------------------------------- */
constexpr int KILN_PIN_SSR1 = 4;
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
 * switch: high means the lid is shut and the coil may be fed. */
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

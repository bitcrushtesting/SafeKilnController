/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pin map for the kilncontrol rev A board (HR-10).
 *
 * HR-10 requires pin assignments to be defined in one place per board variant
 * and not duplicated across the codebase.  This is that place.  Every adapter
 * in this component takes its pins from here, and nothing else in the firmware
 * mentions a GPIO number.
 *
 * Transcribed from hardware/kilncontrol.kicad_sch and verified against the
 * netlist rather than the drawing, so the names below are the net names.
 *
 * ---------------------------------------------------------------------------
 * HR-08: pins that must not be used
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
 * be used at all (FR-NET-01).  Every GPIO1..GPIO10 pin on this board is taken,
 * so CURR_SENSE is the only analogue input there is and there is no room for
 * a second.
 *
 * That costs nothing today: the project supports single-phase kilns only, and
 * a single-phase kiln needs exactly one transformer (HR-11).  It is written
 * down because it will bite whoever next wants an analogue input for anything
 * at all, and because it is the reason three-phase support was dropped rather
 * than merely deferred.
 */
#ifndef KILN_HAL_BOARD_PINS_H
#define KILN_HAL_BOARD_PINS_H

/* --- thermocouple front ends (HR-02, HR-03) ---------------------------- */
/* One SPI bus, two chip selects.  SPI2 (FSPI) because SPI0/SPI1 are the flash
 * controller's and HR-03 wants the TC bus free of anything that can stall it. */
constexpr int KILN_PIN_SPI_SCK = 12;
constexpr int KILN_PIN_SPI_MOSI = 11;
constexpr int KILN_PIN_SPI_MISO = 13;
constexpr int KILN_PIN_TC1_CS = 10;                     /* chamber   */
constexpr int KILN_PIN_TC2_CS = 9;                      /* enclosure */
constexpr int KILN_PIN_TC1_DRDY = 14;
constexpr int KILN_PIN_TC2_DRDY = 21;
/* The FAULT outputs are not read by the firmware: HR-24 wires them into the
 * contactor coil in hardware, and SR-04 reads the fault register over SPI.
 * They are deliberately absent from this map. */

/* --- heat output (HR-06, HR-07, AD-05) --------------------------------- */
constexpr int KILN_PIN_SSR1 = 4;
constexpr int KILN_PIN_SSR2 = 5;
/* AD-05: a software-generated square wave into a charge pump, never a static
 * level and never a hardware PWM peripheral.  See hal_heat.cpp. */
constexpr int KILN_PIN_HEAT_EN = 6;

/* --- annunciation (HR-09) ---------------------------------------------- */
constexpr int KILN_PIN_ALARM = 7;

/* --- heater current (HR-11, HR-17) ------------------------------------- */
/* ADC1_CH0, and the only analogue input on the board; see above. */
constexpr int KILN_PIN_CURR_SENSE = 1;
constexpr int KILN_HAL_CURR_ADC_UNIT = 1;
constexpr int KILN_HAL_CURR_ADC_CHANNEL = 0;

/* --- interlocks -------------------------------------------------------- */
/* SR-31 / HR-21.  The lid contacts are in the coil circuit; this senses the
 * node through the R30/R31 divider, so it reads the *coil supply*, not a bare
 * switch: high means the lid is shut and the coil may be fed. */
constexpr int KILN_PIN_LID_SENSE = 38;

/* --- local interface (HR-04, HR-05) ------------------------------------ */
constexpr int KILN_PIN_I2C_SDA = 8;
constexpr int KILN_PIN_I2C_SCL = 18;
constexpr uint8_t KILN_HAL_OLED_ADDR = 0x3C;
constexpr int KILN_PIN_ENC_A = 16;
constexpr int KILN_PIN_ENC_B = 17;
constexpr int KILN_PIN_ENC_BTN = 15;

/* --- expansion --------------------------------------------------------- */
constexpr int KILN_PIN_EXP_IO2 = 2;
constexpr int KILN_PIN_EXP_IO42 = 42;

#endif /* KILN_HAL_BOARD_PINS_H */

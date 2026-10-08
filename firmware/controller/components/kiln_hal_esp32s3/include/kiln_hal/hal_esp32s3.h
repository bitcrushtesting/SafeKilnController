/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ESP32-S3 adapters -- architecture 5.2.
 *
 * What is here is the set that has no logic left in it: the log ring lives in
 * kiln_core/logring over port_flash, so this partition adapter is three calls
 * that pass straight through to esp_partition; the configuration schema lives in
 * kiln_core/configmodel, so the NVS adapter only moves a blob.  That is the
 * shape every adapter should have, and the reason the ring was split out of this
 * component in the first place -- the logic belongs where a host test can reach
 * it (SWA-01, SWA-14).
 *
 * Deliberately not here: the file store.  Like the log ring it is logic with
 * failure modes a host test has to be able to cut power on, so it lives in
 * kiln_core/fileslots over a second instance of this component's port_flash
 * (SWA-21).  This component contributes the partition, not the format.
 *
 * Pin assignments are in board_pins.h and nowhere else (SYS-HW-10).
 */
#ifndef KILN_HAL_ESP32S3_H
#define KILN_HAL_ESP32S3_H

#include "kiln/err.h"
#include "kiln_core/configmodel.h"
#include "kiln_ports/port_alarm.h"
#include "kiln_ports/port_clock.h"
#include "kiln_ports/port_counters.h"
#include "kiln_ports/port_current.h"
#include "kiln_ports/port_display.h"
#include "kiln_ports/port_door.h"
#include "kiln_ports/port_heat.h"
#include "kiln_ports/port_input.h"
#include "kiln_ports/port_net.h"
#include "kiln_ports/port_tc.h"
#include "kiln_ports/port_flash.h"
#include "kiln_ports/port_kvstore.h"
#include "kiln_ports/port_system.h"

/* The raw log partition of architecture 10.1 (`kilnlog`, type 0x40). */
#define KILN_HAL_LOG_PARTITION "kilnlog"
#define KILN_HAL_FS_PARTITION  "kilnfs"

/* Open the named data partition for the log ring.  Returns KILN_ERR_NOT_FOUND
 * when the partition table has no such entry, which is a build configuration
 * error rather than a runtime one -- and is why it is reported rather than
 * asserted: SWR-LOG-14 keeps firing without a log. */
kiln_err_t kiln_hal_flash_init(const char *partition_label, kiln_port_flash_t *out);

/* NVS, initialising the subsystem and recovering from a partition that needs
 * erasing (a version change, or a first boot on a used device). */
kiln_err_t kiln_hal_kvstore_init(kiln_port_kvstore_t *out);

/* esp_timer for the monotonic clock of SWR-NET-08, and the wall clock of
 * SWR-LOG-12 once SNTP has set it. */
void kiln_hal_clock_init(kiln_port_clock_t *out);

/* Reset cause, firmware identity and the task watchdog (SWR-NFR-15, SWR-SAF-14). */
void kiln_hal_system_init(kiln_port_system_t *out);

/* SWR-PROD-02: read the `prod` partition into a cache, once.  Safe to call on a
 * board that has never been programmed; the block is then reported as
 * unprogrammed.  Call it before kiln_hal_system_init so that the port's
 * prod_info answers from a populated cache. */
kiln_err_t kiln_hal_prod_init(void);
kiln_err_t kiln_hal_prod_get(kiln_prod_info_t *out);

/* --- M2 / M4b adapters -------------------------------------------------- */

/* The shared thermocouple SPI bus (SYS-HW-02, SYS-HW-03).  Called once; kiln_hal_tc_init
 * calls it for you if you have not. */
kiln_err_t kiln_hal_tc_bus_init(void);

/* One MAX31856: `which` is 0 for the chamber and 1 for the enclosure. */
kiln_err_t kiln_hal_tc_init(uint8_t which, kiln_port_tc_t *out);

/* --- the supervisor link (SWA-22) ----------------------------------------
 *
 * The chamber thermocouple is the supervisor's, and arrives over a one-wire
 * serial link.  These two move bytes and nothing else: what the bytes mean is
 * kiln_core/suplink's problem, where a host test can reach it.
 *
 * Receive only; no transmit pin is bound to the peripheral. */
kiln_err_t kiln_hal_suplink_init();
size_t     kiln_hal_suplink_read(uint8_t *out, size_t cap);

/* SSR channels and the heat-enable charge pump.  Read the banner in
 * hal_heat.cpp before touching enable_refresh: SWA-05 and SYS-SAF-02 rest on it. */
void kiln_hal_heat_init(kiln_port_heat_t *out);

/* SWR-SAF-18: let an acknowledged fault re-arm the output without a power cycle. */
void kiln_hal_heat_rearm(void);

/* Current transformer on ADC1 (SWR-CUR-01..SWR-CUR-05).  Reports one channel;
 * board_pins.h explains why HR-23's other two have nowhere to go yet. */
kiln_err_t kiln_hal_current_init(kiln_port_current_t *out);

/* Lid interlock sense (SWR-SAF-31, SYS-HW-21).  `interlock_fitted` is an installation
 * fact the board cannot read, so it is passed in; false raises warning 113. */
void kiln_hal_door_init(kiln_port_door_t *out, bool interlock_fitted);

/* Buzzer with the two patterns SWR-SAF-20 requires (SYS-HW-09). */
void kiln_hal_alarm_init(kiln_port_alarm_t *out);

/* Switching-operation counters (SWR-CUR-13, SWR-SAF-30). */
kiln_err_t kiln_hal_counters_init(kiln_port_counters_t *out);

/* SSD1306 over I2C (SYS-HW-04).  Returns KILN_OK even when the panel does not
 * answer: SWR-HMI-14 keeps controlling the kiln without a display, and
 * available() is how the application knows to raise warning 104. */
kiln_err_t kiln_hal_display_init(kiln_port_display_t *out);

/* Rotary encoder on the pulse counter unit, plus its button (SYS-HW-05). */
kiln_err_t kiln_hal_input_init(kiln_port_input_t *out);

/* WiFi station with AP fallback and SNTP (SWR-NET-01..SWR-NET-09, less
 * SWR-NET-04: mDNS is not in the IDF tree and UR-CON-04 forbids fetching it).
 * Nothing here is on the control path: SWR-NET-07 requires that losing the
 * network cannot alter a running firing. */
kiln_err_t kiln_hal_net_init(const kiln_config_t *cfg, kiln_port_net_t *out);

#endif

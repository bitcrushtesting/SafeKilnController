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
 * it (AD-01, AD-14).
 *
 * Not here yet: the MAX31856, SSD1306, encoder, heat output and CT front end
 * (milestones M2 and M4b's adapter half), and the LittleFS file store. LittleFS
 * is not in the IDF tree and CON-04 forbids a build-time fetch, so it has to be
 * vendored rather than pulled as a managed component.
 */
#ifndef KILN_HAL_ESP32S3_H
#define KILN_HAL_ESP32S3_H

#include "kiln/err.h"
#include "kiln_ports/port_clock.h"
#include "kiln_ports/port_flash.h"
#include "kiln_ports/port_kvstore.h"
#include "kiln_ports/port_system.h"

/* The raw log partition of architecture 10.1 (`kilnlog`, type 0x40). */
#define KILN_HAL_LOG_PARTITION "kilnlog"

/* Open the named data partition for the log ring.  Returns KILN_ERR_NOT_FOUND
 * when the partition table has no such entry, which is a build configuration
 * error rather than a runtime one -- and is why it is reported rather than
 * asserted: FR-LOG-14 keeps firing without a log. */
kiln_err_t kiln_hal_flash_init(const char *partition_label, kiln_port_flash_t *out);

/* NVS, initialising the subsystem and recovering from a partition that needs
 * erasing (a version change, or a first boot on a used device). */
kiln_err_t kiln_hal_kvstore_init(kiln_port_kvstore_t *out);

/* esp_timer for the monotonic clock of FR-NET-08, and the wall clock of
 * FR-LOG-12 once SNTP has set it. */
void kiln_hal_clock_init(kiln_port_clock_t *out);

/* Reset cause, firmware identity and the task watchdog (NFR-15, SR-14). */
void kiln_hal_system_init(kiln_port_system_t *out);

#endif

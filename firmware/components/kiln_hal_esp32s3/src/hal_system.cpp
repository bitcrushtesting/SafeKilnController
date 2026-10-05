/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * port_system: reset cause, firmware identity, the task watchdog -- NFR-15,
 * SR-14, SR-15, FR-UPD-06, NFR-11.
 */

#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kiln_hal/hal_esp32s3.h"

static const char *TAG = "hal_sys";

/* NFR-15 / SR-14 / SR-15: the vendor's reset reason, mapped to the enum the core
 * decides on.  The mapping lives here so that kiln_core/runstate never sees an
 * esp_reset_reason_t -- and so that a vendor renumbering is one edit. */
static kiln_reset_cause_t sys_reset_cause(void *ctx)
{
    (void)ctx;
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return KILN_RESET_POWER_ON;
    case ESP_RST_SW:        return KILN_RESET_SOFTWARE;
    case ESP_RST_PANIC:     return KILN_RESET_PANIC;
    case ESP_RST_TASK_WDT:  return KILN_RESET_TASK_WDT;
    case ESP_RST_INT_WDT:   return KILN_RESET_INT_WDT;
    case ESP_RST_WDT:       return KILN_RESET_RTC_WDT;
    case ESP_RST_BROWNOUT:  return KILN_RESET_BROWNOUT;
    case ESP_RST_DEEPSLEEP: return KILN_RESET_DEEPSLEEP;
    case ESP_RST_EXT:       return KILN_RESET_EXTERNAL;
    default:                return KILN_RESET_UNKNOWN;
    }
}

static kiln_err_t sys_fw_info(void *ctx, kiln_fw_info_t *out)
{
    (void)ctx;
    if (out == nullptr) {
        return KILN_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    /* FR-UPD-06: version, build time and git revision, which the build already
     * embeds in the app descriptor.
     *
     * Copied with an explicit precision rather than a bare %s: the descriptor's
     * fields are longer than the ones here, so the compiler is right that this
     * truncates.  Truncation is the intended behaviour -- these are display
     * strings -- but it has to be stated rather than risked. */
    const esp_app_desc_t *d = esp_app_get_description();
    if (d != nullptr) {
        snprintf(out->version, sizeof(out->version), "%.*s",
                 (int)sizeof(out->version) - 1, d->version);
        snprintf(out->build_time, sizeof(out->build_time), "%.11s %.8s",
                 d->date, d->time);
        snprintf(out->idf_version, sizeof(out->idf_version), "%.*s",
                 (int)sizeof(out->idf_version) - 1, d->idf_ver);
        /* The build sets `version` from `git describe` in a git checkout, so the
         * revision is in there rather than in a field of its own. */
        snprintf(out->git_rev, sizeof(out->git_rev), "%.*s",
                 (int)sizeof(out->git_rev) - 1, d->version);
    }
    snprintf(out->target, sizeof(out->target), "esp32s3");
    return KILN_OK;
}

static kiln_err_t sys_stats(void *ctx, kiln_sys_stats_t *out)
{
    (void)ctx;
    if (out == nullptr) {
        return KILN_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    out->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    out->heap_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    /* NFR-11/NFR-12: the minimum ever seen is the figure the soak test watches;
     * the instantaneous free heap says nothing about the worst moment. */
    out->heap_min_free = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    out->stack_min_free = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    return KILN_OK;
}

static kiln_err_t sys_wdt_subscribe(void *ctx)
{
    (void)ctx;
    /* SR-14: the calling task joins the task watchdog.  Already-subscribed is
     * success, so a task that is restarted does not fail here. */
    const esp_err_t e = esp_task_wdt_add(NULL);
    if (e == ESP_OK || e == ESP_ERR_INVALID_ARG) {
        return KILN_OK;
    }
    ESP_LOGE(TAG, "esp_task_wdt_add: %s", esp_err_to_name(e));
    return KILN_ERR_IO;
}

static void sys_wdt_feed(void *ctx)
{
    (void)ctx;
    (void)esp_task_wdt_reset();
}

static void sys_restart(void *ctx)
{
    (void)ctx;
    esp_restart();
}

void kiln_hal_system_init(kiln_port_system_t *out)
{
    if (out == nullptr) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->reset_cause   = sys_reset_cause;
    out->fw_info       = sys_fw_info;
    out->stats         = sys_stats;
    out->wdt_subscribe = sys_wdt_subscribe;
    out->wdt_feed      = sys_wdt_feed;
    out->restart       = sys_restart;
}

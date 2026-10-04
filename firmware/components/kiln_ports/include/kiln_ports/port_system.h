/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * System identity, reset cause and the watchdog -- NFR-15, SR-14, SR-15,
 * FR-UPD-06.
 *
 * NFR-15 requires a watchdog reset, brownout or panic to be *recorded with its
 * cause*, and SR-14 maps a watchdog reset that interrupted a firing to fault 15.
 * That decision is made in the core from a cause enum, so the enum -- not the
 * vendor's reset-reason numbering -- is what the port exposes.
 */
#ifndef KILN_PORT_SYSTEM_H
#define KILN_PORT_SYSTEM_H

#include <stddef.h>
#include "kiln/err.h"
#include "kiln/types.h"

typedef enum {
    KILN_RESET_UNKNOWN = 0,
    KILN_RESET_POWER_ON,
    KILN_RESET_SOFTWARE,      /* a requested restart, e.g. after an update   */
    KILN_RESET_PANIC,         /* NFR-15                                      */
    KILN_RESET_TASK_WDT,      /* SR-14                                       */
    KILN_RESET_INT_WDT,       /* SR-14                                       */
    KILN_RESET_RTC_WDT,       /* SR-14                                       */
    KILN_RESET_BROWNOUT,      /* SR-15                                       */
    KILN_RESET_DEEPSLEEP,
    KILN_RESET_EXTERNAL,
    KILN_RESET_COUNT,
} kiln_reset_cause_t;

/* True for a cause that means the previous boot ended abnormally, and therefore
 * that an interrupted run must be treated as a fault rather than a restart. */
static inline bool kiln_reset_was_abnormal(kiln_reset_cause_t c)
{
    return c == KILN_RESET_PANIC    || c == KILN_RESET_TASK_WDT ||
           c == KILN_RESET_INT_WDT  || c == KILN_RESET_RTC_WDT  ||
           c == KILN_RESET_BROWNOUT;
}

/* FR-UPD-06 */
typedef struct {
    char version[24];
    char build_time[24];
    char git_rev[16];
    char target[16];
    char idf_version[16];
} kiln_fw_info_t;

typedef struct {
    uint32_t uptime_s;
    uint32_t heap_free;
    uint32_t heap_min_free;      /* NFR-11: the figure the soak test watches */
    uint32_t stack_min_free;
    float    chip_temp_c;
} kiln_sys_stats_t;

typedef struct kiln_port_system {
    void *ctx;
    kiln_reset_cause_t (*reset_cause)(void *ctx);
    kiln_err_t (*fw_info)(void *ctx, kiln_fw_info_t *out);
    kiln_err_t (*stats)(void *ctx, kiln_sys_stats_t *out);
    /* SR-14: each supervised task checks in; the port owns the task watchdog. */
    kiln_err_t (*wdt_subscribe)(void *ctx);
    void       (*wdt_feed)(void *ctx);
    /* Deliberate restart (after a confirmed update, or an operator request). */
    void       (*restart)(void *ctx);
} kiln_port_system_t;

#endif /* KILN_PORT_SYSTEM_H */

/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * System identity, reset cause and the watchdog -- SWR-NFR-15, SWR-SAF-14, SWR-SAF-15,
 * SWR-UPD-06.
 *
 * SWR-NFR-15 requires a watchdog reset, brownout or panic to be *recorded with its
 * cause*, and SWR-SAF-14 maps a watchdog reset that interrupted a firing to fault 15.
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
    KILN_RESET_PANIC,         /* SWR-NFR-15                                      */
    KILN_RESET_TASK_WDT,      /* SWR-SAF-14                                       */
    KILN_RESET_INT_WDT,       /* SWR-SAF-14                                       */
    KILN_RESET_RTC_WDT,       /* SWR-SAF-14                                       */
    KILN_RESET_BROWNOUT,      /* SWR-SAF-15                                       */
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

/* SWR-UPD-06.
 *
 * `version` is semantic versioning derived from the release tag by the build
 * (firmware/controller/CMakeLists.txt), so it is as long as "10.20.30+123.a1b2c3d.dirty":
 * 32 rather than 24, because a version truncated in the middle of its build
 * metadata is worse than no build metadata. */
typedef struct {
    char version[32];
    char build_time[24];
    char git_rev[16];
    char target[16];
    char idf_version[16];
} kiln_fw_info_t;

/* SWR-PROD-01: the production data block, written once at manufacture and read
 * only here.  It is deliberately NOT part of kiln_config_t: the configuration is
 * the operator's and is erased with it (SWR-CFG-05, and the NVS format-change
 * path in hal_kvstore), whereas this identifies the unit for its whole life.
 *
 * All fields are NUL-terminated strings, including the date, because this block
 * is read to be displayed and reported rather than computed with, and a string
 * cannot acquire an epoch or a timezone it did not have at manufacture.
 * `programmed` is false when the block is absent or incomplete, which is the
 * normal state of a board that has not been through the production step. */
typedef struct {
    bool programmed;
    char manufacturer[32];
    char model[24];
    char revision[16];       /* board revision, e.g. "rev-C" */
    char serial[24];
    char production_date[11];   /* ISO 8601 date, "YYYY-MM-DD" */
} kiln_prod_info_t;

typedef struct {
    uint32_t uptime_s;
    uint32_t heap_free;
    uint32_t heap_min_free;      /* SWR-NFR-11: the figure the soak test watches */
    uint32_t stack_min_free;
    float    chip_temp_c;
} kiln_sys_stats_t;

typedef struct kiln_port_system {
    void *ctx;
    kiln_reset_cause_t (*reset_cause)(void *ctx);
    kiln_err_t (*fw_info)(void *ctx, kiln_fw_info_t *out);
    /* SWR-PROD-02.  Optional: a port that does not implement it leaves the
     * pointer null, and every caller already has to tolerate that. */
    kiln_err_t (*prod_info)(void *ctx, kiln_prod_info_t *out);
    kiln_err_t (*stats)(void *ctx, kiln_sys_stats_t *out);
    /* SWR-SAF-14: each supervised task checks in; the port owns the task watchdog. */
    kiln_err_t (*wdt_subscribe)(void *ctx);
    void       (*wdt_feed)(void *ctx);
    /* Deliberate restart (after a confirmed update, or an operator request). */
    void       (*restart)(void *ctx);
} kiln_port_system_t;

#endif /* KILN_PORT_SYSTEM_H */

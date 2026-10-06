/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The circular sample log (AD-08).  A raw flash partition of fixed 20 byte
 * records (AD-18: 16 before FR-CUR-09 added heater current), not a filesystem:
 * the wear pattern is provable and a power cut can damage at most the record in
 * flight.
 */
#ifndef KILN_PORT_LOGSTORE_H
#define KILN_PORT_LOGSTORE_H

#include <stddef.h>
#include "kiln/err.h"
#include "kiln/types.h"

constexpr size_t KILN_LOG_RECORD_BYTES = 20;  /* AD-18: 20 B since FR-CUR-09 added current */
constexpr size_t KILN_LOG_SECTOR_BYTES = 4096;
constexpr size_t KILN_LOG_HEADER_BYTES = 16;
constexpr size_t KILN_LOG_RECS_PER_SECTOR =
    (KILN_LOG_SECTOR_BYTES - KILN_LOG_HEADER_BYTES) / KILN_LOG_RECORD_BYTES;  /* 204 */

typedef struct {
    uint32_t sectors_total;
    uint32_t sectors_used;
    uint32_t records_total;     /* capacity                                  */
    uint32_t records_stored;
    uint32_t oldest_seq;
    uint32_t newest_seq;
    uint32_t write_errors;
    uint32_t dropped_samples;   /* queue overflow, FR-LOG-14                 */
    uint32_t erase_count;       /* since boot, for the NFR-14 audit          */
    bool     available;
} kiln_logstore_stats_t;

/* Visitor invoked in chronological order.  Returning false stops iteration. */
typedef bool (*kiln_logstore_visit_fn)(void *user,
                                       uint32_t run_id,
                                       const uint8_t rec[KILN_LOG_RECORD_BYTES]);

typedef struct kiln_port_logstore {
    void *ctx;
    kiln_err_t (*begin_run)(void *ctx, uint32_t run_id);
    kiln_err_t (*append)(void *ctx, const uint8_t rec[KILN_LOG_RECORD_BYTES]);
    /* run_id 0 = every run.  Iterates oldest to newest. */
    kiln_err_t (*iterate)(void *ctx, uint32_t run_id,
                          kiln_logstore_visit_fn fn, void *user);
    /* Newest record of a run, for FR-RUN-08 recovery (AD-09). */
    kiln_err_t (*last_record)(void *ctx, uint32_t run_id,
                              uint8_t rec[KILN_LOG_RECORD_BYTES]);
    kiln_err_t (*stats)(void *ctx, kiln_logstore_stats_t *out);
    kiln_err_t (*erase_all)(void *ctx);
} kiln_port_logstore_t;

#endif

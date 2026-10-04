/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The circular sample log -- AD-08, architecture 10.3, FR-LOG-05..FR-LOG-09,
 * FR-LOG-15.
 *
 * A raw flash partition holding a ring of sectors, each with a 16 byte header
 * and 204 fixed 20 byte records.  Not a filesystem: a filesystem adds metadata
 * writes, fragmentation and a torn-write failure mode across structures we do
 * not control, where a ring of fixed records has a provable wear pattern
 * (architecture 10.4) and a reader that a power cut cannot confuse.
 *
 * Three properties carry the weight, and each is a host test:
 *
 *   Head discovery (10.3).  On boot, read the sector headers and take the
 *   highest valid seq, then scan that sector for the first erased slot.  No
 *   separate metadata, so nothing can disagree with the data.
 *
 *   Wrap ordering.  The next sector is erased immediately *before* it is first
 *   written, never in advance.  Erasing ahead would leave a window in which a
 *   power loss destroys records the index still claims exist.
 *
 *   Torn records (FR-LOG-08).  A record whose CRC fails, or which is partly
 *   erased, terminates the scan of its sector and is skipped by readers.  At
 *   most the one record in flight is lost.
 *
 * Pure logic over kiln_port_flash: no clock, no allocation, and all state in the
 * caller's struct (AD-02, AD-03).
 */
#ifndef KILN_CORE_LOGRING_H
#define KILN_CORE_LOGRING_H

#include "kiln/err.h"
#include "kiln/types.h"
#include "kiln_core/logrec.h"
#include "kiln_ports/port_flash.h"
#include "kiln_ports/port_logstore.h"

typedef struct {
    const kiln_port_flash_t *flash;

    uint32_t sector_bytes;
    uint32_t sector_count;
    uint32_t recs_per_sector;

    /* Where the next record goes. */
    uint32_t head_sector;
    uint32_t head_slot;
    uint32_t head_seq;          /* seq of head_sector */

    /* The oldest sector still holding data, for the stats of FR-LOG-15. */
    uint32_t tail_sector;
    bool     wrapped;           /* the ring has overwritten at least once */

    uint32_t run_id;            /* the run being appended to */
    bool     run_open;

    /* FR-LOG-15 */
    uint32_t write_errors;
    uint32_t erase_count;
    uint32_t records_stored;
    bool     available;
} kiln_logring_t;

/* Mount an existing ring, discovering the head.  Returns KILN_OK on a clean
 * mount, KILN_ERR_CORRUPT when no valid sector header was found at all (an
 * unformatted or wiped partition, which is then formatted on the first
 * begin_run), and KILN_ERR_IO when the flash itself would not answer -- in which
 * case `available` is false and FR-LOG-14 says the firing continues anyway. */
kiln_err_t kiln_logring_mount(kiln_logring_t *r, const kiln_port_flash_t *flash);

/* FR-LOG-13: erase everything, irreversibly. */
kiln_err_t kiln_logring_erase_all(kiln_logring_t *r);

/* Open a run for appending.  Starts a fresh sector so that a run's records are
 * never interleaved with the previous run's, which is what lets iterate() filter
 * by run without storing an index. */
kiln_err_t kiln_logring_begin_run(kiln_logring_t *r, uint32_t run_id);

kiln_err_t kiln_logring_append(kiln_logring_t *r,
                               const uint8_t rec[KILN_LOG_RECORD_BYTES]);

/* Iterate oldest to newest; run_id 0 means every run.  Returning false from the
 * visitor stops. */
kiln_err_t kiln_logring_iterate(kiln_logring_t *r, uint32_t run_id,
                                kiln_logstore_visit_fn fn, void *user);

/* The newest intact record of a run, which is the power-loss journal of AD-09. */
kiln_err_t kiln_logring_last_record(kiln_logring_t *r, uint32_t run_id,
                                    uint8_t rec[KILN_LOG_RECORD_BYTES]);

kiln_err_t kiln_logring_stats(kiln_logring_t *r, kiln_logstore_stats_t *out);

/* Present the ring as a kiln_port_logstore_t, so the application is coded
 * against the port and a different store could be substituted. */
void kiln_logring_bind(kiln_logring_t *r, kiln_port_logstore_t *out);

/* Records the ring can hold, for the FR-LOG-07 capacity analysis. */
static inline uint32_t kiln_logring_capacity(const kiln_logring_t *r)
{
    return r->sector_count * r->recs_per_sector;
}

#endif

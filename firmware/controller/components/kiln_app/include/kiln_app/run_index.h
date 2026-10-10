/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Run record retention -- SWR-RUN-07, SWR-LOG-09, architecture 5.3 (`run_index`).
 *
 * One file per run in a ring of KILN_MAX_RUN_RECORDS slots, oldest evicted.
 * One file rather than a single index blob for the same reason the sample log is
 * a ring of records: a 20-record index is ~10 kB that has to be rewritten whole
 * on every run, where a slot file is written once and never touched again.
 *
 * Also carries the SWR-SAF-12 insulation baseline, because the baseline *is* the run
 * history -- the median duty-seconds to each band across previous comparable
 * runs -- and keeping it anywhere else would mean keeping it in agreement.
 */
#ifndef KILN_APP_RUN_INDEX_H
#define KILN_APP_RUN_INDEX_H

#include "kiln/err.h"
#include "kiln_core/runstate.h"
#include "kiln_core/safety.h"
#include "kiln_ports/port_filestore.h"

#define KILN_RUN_SLOTS KILN_MAX_RUN_RECORDS   /* SWR-LOG-09: at least 20 */

/* Append, evicting the oldest run once full (SWR-LOG-09). */
kiln_err_t kiln_run_index_append(const kiln_port_filestore_t *fs,
                                 const kiln_run_record_t *r);

kiln_err_t kiln_run_index_get_slot(const kiln_port_filestore_t *fs, uint8_t slot,
                                   kiln_run_record_t *out);
kiln_err_t kiln_run_index_find(const kiln_port_filestore_t *fs, uint32_t run_id,
                               kiln_run_record_t *out);
uint8_t    kiln_run_index_count(const kiln_port_filestore_t *fs);

/* The highest run_id ever stored, so run numbering continues across a reboot
 * instead of restarting and colliding with records already on disk. */
uint32_t kiln_run_index_next_run_id(const kiln_port_filestore_t *fs);

/* SWR-LOG-09: mark every stored run older than `oldest_logged_run_id` as having
 * lost its samples.  Called after mounting the ring, which is the only thing
 * that knows how far back the samples actually go. */
kiln_err_t kiln_run_index_mark_truncated(const kiln_port_filestore_t *fs,
                                         uint32_t oldest_logged_run_id);

/* SWR-SAF-12's baseline across the stored history. */
/* SWR-CFG-09: remove one slot's record, for the factory reset.
 *
 * Here rather than in the reset itself because this file owns the naming: the
 * file store has no enumeration (and deliberately so), so the only thing that
 * knows a run record is called "/r/07" is the code that writes it. Returns
 * KILN_ERR_NOT_FOUND for a slot that held nothing, which the caller counts
 * rather than treats as a failure. */
kiln_err_t kiln_run_index_erase_slot(const kiln_port_filestore_t *fs, uint8_t slot);

kiln_err_t kiln_run_index_baseline(const kiln_port_filestore_t *fs,
                                   uint8_t min_runs,
                                   kiln_insulation_baseline_t *out);

#endif

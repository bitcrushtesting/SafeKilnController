/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The 20 most recent firings, one record a slot, oldest overwritten first
 * (SWR-LOG-09).
 *
 * Every operation here walks all 20 slots: append reads them to find the oldest,
 * and mark_truncated and baseline read them to find the one they want.  That is
 * not as expensive as it looks -- the file store answers a read from its in-RAM
 * index and a 554 byte CRC, with one region write at the end of an append -- and
 * the walk is now the only reason this file reads at all.
 *
 * It is also the one place where the store being a fixed array rather than a
 * directory costs something: with a name to look up rather than a slot to scan,
 * each of the three would be one access instead of twenty.  Left as it is
 * because twenty accesses once per firing is not a cost worth a mechanism, and
 * recorded here because it is the natural thing to simplify if this file is ever
 * revisited for another reason.
 */

#include <stdio.h>
#include <string.h>
#include "kiln_app/run_index.h"
#include "kiln_core/logrec.h"      /* kiln_crc16 */

constexpr uint32_t RUN_MAGIC = 0x4E55521EU;  /* "\x1eRUN" */
#define RUN_BLOB_BYTES (8u + sizeof(kiln_run_record_t) + 2u)

namespace {

void slot_path(uint8_t slot, char out[KILN_PATH_MAX])
{
    /* As program_store: "/r/255" at most, 6 of KILN_PATH_MAX's 64. */
    (void)snprintf(out, KILN_PATH_MAX, "/r/%02u", (unsigned)slot);
}

kiln_err_t read_slot(const kiln_port_filestore_t *fs, uint8_t slot,
                            kiln_run_record_t *out)
{
    char path[KILN_PATH_MAX];
    slot_path(slot, path);

    uint8_t blob[RUN_BLOB_BYTES];
    size_t  len = 0;
    const kiln_err_t e = fs->read(fs->ctx, path, blob, sizeof(blob), &len);
    if (e != KILN_OK) {
        return e;
    }
    if (len != RUN_BLOB_BYTES) {
        return KILN_ERR_CORRUPT;
    }

    const uint32_t magic = (uint32_t)blob[0] | ((uint32_t)blob[1] << 8u) |
                           ((uint32_t)blob[2] << 16u) | ((uint32_t)blob[3] << 24u);
    if (magic != RUN_MAGIC) {
        return KILN_ERR_CORRUPT;
    }

    const uint16_t crc = (uint16_t)((uint32_t)blob[RUN_BLOB_BYTES - 2] |
                                    ((uint32_t)blob[RUN_BLOB_BYTES - 1] << 8u));
    if (kiln_crc16(blob, RUN_BLOB_BYTES - 2) != crc) {
        return KILN_ERR_CORRUPT;
    }

    memcpy(out, &blob[8], sizeof(*out));
    /* SWR-NFR-19: it came off storage, so the embedded program's strings are
     * untrusted until proven terminated. */
    out->program.name[KILN_PROGRAM_NAME_LEN - 1]        = '\0';
    out->program.description[KILN_PROGRAM_DESC_LEN - 1] = '\0';
    out->gain_set_name[KILN_CFG_GAINSET_LEN - 1]        = '\0';
    return KILN_OK;
}

kiln_err_t write_slot(const kiln_port_filestore_t *fs, uint8_t slot,
                             const kiln_run_record_t *r)
{
    uint8_t blob[RUN_BLOB_BYTES];
    memset(blob, 0, sizeof(blob));

    blob[0] = (uint8_t)RUN_MAGIC;
    blob[1] = (uint8_t)(RUN_MAGIC >> 8u);
    blob[2] = (uint8_t)(RUN_MAGIC >> 16u);
    blob[3] = (uint8_t)(RUN_MAGIC >> 24u);
    blob[4] = 1;                                     /* record schema */
    blob[5] = (uint8_t)(sizeof(kiln_run_record_t));
    blob[6] = (uint8_t)(sizeof(kiln_run_record_t) >> 8u);
    memcpy(&blob[8], r, sizeof(*r));

    const uint16_t crc = kiln_crc16(blob, RUN_BLOB_BYTES - 2);
    blob[RUN_BLOB_BYTES - 2] = (uint8_t)crc;
    blob[RUN_BLOB_BYTES - 1] = (uint8_t)(crc >> 8u);

    char path[KILN_PATH_MAX];
    slot_path(slot, path);
    return fs->write_atomic(fs->ctx, path, blob, sizeof(blob));
}

} // namespace

kiln_err_t kiln_run_index_append(const kiln_port_filestore_t *fs,
                                 const kiln_run_record_t *r)
{
    if ((fs == nullptr) || (fs->read == nullptr) || (fs->write_atomic == nullptr) ||
        (r == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    /* A free slot, or the one holding the oldest run.  "Oldest" by run_id and
     * not by wall time, because SWR-LOG-12 allows the clock to have been unset:
     * run_id is monotonic whatever the clock was doing. */
    uint8_t  target  = KILN_RUN_SLOTS;
    uint32_t lowest  = 0;
    bool     have_lowest = false;

    for (uint8_t slot = 0; slot < KILN_RUN_SLOTS; slot++) {
        kiln_run_record_t existing;
        const kiln_err_t e = read_slot(fs, slot, &existing);
        if (e != KILN_OK) { target = slot; break; }   /* free or unreadable */

        /* Replacing a record of the same run is an update, not an append. */
        if (existing.run_id == r->run_id) { target = slot; break; }

        if (!have_lowest || existing.run_id < lowest) {
            lowest      = existing.run_id;
            have_lowest = true;
            target      = slot;
        }
    }
    if (target >= KILN_RUN_SLOTS) {
        return KILN_ERR_NO_SPACE;
    }
    return write_slot(fs, target, r);
}

kiln_err_t kiln_run_index_get_slot(const kiln_port_filestore_t *fs, uint8_t slot,
                                   kiln_run_record_t *out)
{
    if ((fs == nullptr) || (fs->read == nullptr) || (out == nullptr) || slot >= KILN_RUN_SLOTS) {
        return KILN_ERR_INVALID_ARG;
    }
    return read_slot(fs, slot, out);
}

kiln_err_t kiln_run_index_find(const kiln_port_filestore_t *fs, uint32_t run_id,
                               kiln_run_record_t *out)
{
    if ((fs == nullptr) || (fs->read == nullptr) || (out == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    for (uint8_t slot = 0; slot < KILN_RUN_SLOTS; slot++) {
        kiln_run_record_t r;
        if (read_slot(fs, slot, &r) != KILN_OK) {
            continue;
        }
        if (r.run_id == run_id) { *out = r; return KILN_OK; }
    }
    return KILN_ERR_NOT_FOUND;
}

uint8_t kiln_run_index_count(const kiln_port_filestore_t *fs)
{
    if ((fs == nullptr) || (fs->read == nullptr)) {
        return 0;
    }

    uint8_t n = 0;
    for (uint8_t slot = 0; slot < KILN_RUN_SLOTS; slot++) {
        kiln_run_record_t r;
        if (read_slot(fs, slot, &r) == KILN_OK) {
            n++;
        }
    }
    return n;
}

uint32_t kiln_run_index_next_run_id(const kiln_port_filestore_t *fs)
{
    if ((fs == nullptr) || (fs->read == nullptr)) {
        return 1u;
    }

    uint32_t highest = 0;
    for (uint8_t slot = 0; slot < KILN_RUN_SLOTS; slot++) {
        kiln_run_record_t r;
        if (read_slot(fs, slot, &r) != KILN_OK) {
            continue;
        }
        if (r.run_id > highest) {
            highest = r.run_id;
        }
    }
    return highest + 1u;
}

kiln_err_t kiln_run_index_mark_truncated(const kiln_port_filestore_t *fs,
                                         uint32_t oldest_logged_run_id)
{
    if ((fs == nullptr) || (fs->read == nullptr) || (fs->write_atomic == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    for (uint8_t slot = 0; slot < KILN_RUN_SLOTS; slot++) {
        kiln_run_record_t r;
        if (read_slot(fs, slot, &r) != KILN_OK) {
            continue;
        }

        const bool truncated = r.run_id < oldest_logged_run_id;
        const bool marked    = (r.flags & KILN_RUN_FLAG_TRUNCATED) != 0;
        if (truncated == marked) {
            continue; /* nothing to rewrite */
        }

        if (truncated) {
            r.flags = (uint8_t)(r.flags | KILN_RUN_FLAG_TRUNCATED);
        }
        else {
            r.flags = (uint8_t)(r.flags & ~KILN_RUN_FLAG_TRUNCATED);
        }

        const kiln_err_t e = write_slot(fs, slot, &r);
        if (e != KILN_OK) {
            return e;
        }
    }
    return KILN_OK;
}

kiln_err_t kiln_run_index_baseline(const kiln_port_filestore_t *fs,
                                   uint8_t min_runs,
                                   kiln_insulation_baseline_t *out)
{
    if ((fs == nullptr) || (out == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    /* One record at a time would be cheaper in stack, but the median needs them
     * all at once, and 20 records is the documented bound. */
    static kiln_run_record_t records[KILN_RUN_SLOTS];
    uint8_t n = 0;

    for (uint8_t slot = 0; slot < KILN_RUN_SLOTS; slot++) {
        if (read_slot(fs, slot, &records[n]) == KILN_OK) {
            n++;
        }
    }
    return kiln_runstate_baseline(records, n, min_runs, out);
}

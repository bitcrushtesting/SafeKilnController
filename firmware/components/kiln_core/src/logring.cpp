/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <string.h>
#include "kiln_core/logring.h"

/* --- layout ------------------------------------------------------------- */

static uint32_t sector_offset(const kiln_logring_t *r, uint32_t sector)
{
    return sector * r->sector_bytes;
}

static uint32_t slot_offset(const kiln_logring_t *r, uint32_t sector, uint32_t slot)
{
    return sector_offset(r, sector) + KILN_LOG_HEADER_BYTES
         + slot * KILN_LOG_RECORD_BYTES;
}

static uint32_t next_sector(const kiln_logring_t *r, uint32_t sector)
{
    return (sector + 1u) % r->sector_count;
}

/* --- header ------------------------------------------------------------- */

static kiln_err_t read_hdr(kiln_logring_t *r, uint32_t sector,
                           kiln_log_sector_hdr_t *out)
{
    uint8_t buf[KILN_LOG_HEADER_BYTES];
    const kiln_err_t e = r->flash->read(r->flash->ctx, sector_offset(r, sector),
                                        buf, sizeof(buf));
    if (e != KILN_OK) {
        return e;
    }
    return kiln_logrec_decode_hdr(buf, out);
}

/* Erase the sector, then stamp its header.  In that order and never ahead of
 * time: see the note on wrap ordering in logring.h. */
static kiln_err_t claim_sector(kiln_logring_t *r, uint32_t sector, uint32_t seq,
                               uint32_t run_id)
{
    kiln_err_t e = r->flash->erase(r->flash->ctx, sector_offset(r, sector),
                                   r->sector_bytes);
    if (e != KILN_OK) { r->write_errors++; return e; }
    r->erase_count++;

    const kiln_log_sector_hdr_t h = { .magic = KILN_LOG_MAGIC,
                                      .seq = seq,
                                      .run_id = run_id,
                                      .format_version = KILN_LOG_FORMAT_VERSION };
    uint8_t buf[KILN_LOG_HEADER_BYTES];
    kiln_logrec_encode_hdr(&h, buf);

    e = r->flash->write(r->flash->ctx, sector_offset(r, sector), buf, sizeof(buf));
    if (e != KILN_OK) { r->write_errors++; return e; }
    return KILN_OK;
}

/* --- mount -------------------------------------------------------------- */

/* First erased slot in a sector, which is where appending resumes.  A torn or
 * corrupt record terminates the scan: everything beyond it in this sector is
 * unreachable anyway, because a reader stops there too (FR-LOG-08). */
static kiln_err_t scan_sector_head(kiln_logring_t *r, uint32_t sector,
                                   uint32_t *slot_out, uint32_t *valid_out)
{
    uint32_t valid = 0;
    for (uint32_t slot = 0; slot < r->recs_per_sector; slot++) {
        uint8_t rec[KILN_LOG_RECORD_BYTES];
        const kiln_err_t e = r->flash->read(r->flash->ctx,
                                            slot_offset(r, sector, slot),
                                            rec, sizeof(rec));
        if (e != KILN_OK) {
            return e;
        }

        kiln_log_sample_t s;
        const kiln_err_t d = kiln_logrec_decode(rec, &s);
        if (d == KILN_OK) { valid++; continue; }

        /* Erased: the head.  Corrupt: the head too, because the scan stops
         * here either way and writing past it would strand the gap. */
        *slot_out  = slot;
        *valid_out = valid;
        return KILN_OK;
    }
    *slot_out  = r->recs_per_sector;      /* full */
    *valid_out = valid;
    return KILN_OK;
}

kiln_err_t kiln_logring_mount(kiln_logring_t *r, const kiln_port_flash_t *flash)
{
    if ((r == nullptr) || (flash == nullptr) || (flash->info == nullptr) ||
        (flash->read == nullptr) || (flash->write == nullptr) || (flash->erase == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    memset(r, 0, sizeof(*r));
    r->flash = flash;

    kiln_flash_info_t info;
    kiln_err_t e = flash->info(flash->ctx, &info);
    if (e != KILN_OK) {
        return e;
    }

    if (info.sector_bytes == 0 || info.size_bytes < info.sector_bytes) {
        return KILN_ERR_INVALID_ARG;
    }
    r->sector_bytes    = info.sector_bytes;
    r->sector_count    = info.size_bytes / info.sector_bytes;
    r->recs_per_sector = (info.sector_bytes - KILN_LOG_HEADER_BYTES)
                       / KILN_LOG_RECORD_BYTES;
    if (r->recs_per_sector == 0 || r->sector_count == 0) {
        return KILN_ERR_INVALID_ARG;
    }

    /* Architecture 10.3: the highest valid seq is the head sector.  Sequence
     * numbers only ever increase, so the ring's order is defined by them and not
     * by position -- which is what makes wrap invisible to a reader. */
    bool     any   = false;
    uint32_t best_seq = 0, best_sector = 0;
    uint32_t low_seq  = 0, low_sector  = 0;
    uint32_t stored = 0;

    for (uint32_t s = 0; s < r->sector_count; s++) {
        kiln_log_sector_hdr_t h;
        const kiln_err_t he = read_hdr(r, s, &h);
        if (he == KILN_ERR_IO) { r->available = false; return KILN_ERR_IO; }
        if (he != KILN_OK) {
            continue; /* erased or foreign: skip */
        }

        uint32_t slot = 0, valid = 0;
        const kiln_err_t se = scan_sector_head(r, s, &slot, &valid);
        if (se != KILN_OK) { r->available = false; return se; }
        stored += valid;

        if (!any || h.seq > best_seq) { best_seq = h.seq; best_sector = s; }
        if (!any || h.seq < low_seq)  { low_seq  = h.seq; low_sector  = s; }
        any = true;
    }

    r->available      = true;
    r->records_stored = stored;

    if (!any) {
        /* Unformatted, or erased.  Nothing is wrong; the first run claims a
         * sector and the ring starts from there. */
        r->head_sector = 0;
        r->head_slot   = 0;
        r->head_seq    = 0;
        r->tail_sector = 0;
        return KILN_ERR_CORRUPT;
    }

    uint32_t slot = 0, valid = 0;
    e = scan_sector_head(r, best_sector, &slot, &valid);
    if (e != KILN_OK) {
        return e;
    }

    r->head_sector = best_sector;
    r->head_slot   = slot;
    r->head_seq    = best_seq;
    r->tail_sector = low_sector;
    r->wrapped     = (best_seq >= r->sector_count);

    /* The run the head sector belongs to, so an append after a reboot without a
     * begin_run still lands somewhere coherent. */
    kiln_log_sector_hdr_t h;
    if (read_hdr(r, best_sector, &h) == KILN_OK) {
        r->run_id = h.run_id;
    }

    return KILN_OK;
}

kiln_err_t kiln_logring_erase_all(kiln_logring_t *r)
{
    if ((r == nullptr) || (r->flash == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    const kiln_err_t e = r->flash->erase(r->flash->ctx, 0,
                                         (size_t)r->sector_count * r->sector_bytes);
    if (e != KILN_OK) { r->write_errors++; return e; }

    r->erase_count   += r->sector_count;
    r->head_sector    = 0;
    r->head_slot      = 0;
    r->head_seq       = 0;
    r->tail_sector    = 0;
    r->wrapped        = false;
    r->records_stored = 0;
    r->run_open       = false;
    return KILN_OK;
}

/* --- append ------------------------------------------------------------- */

kiln_err_t kiln_logring_begin_run(kiln_logring_t *r, uint32_t run_id)
{
    if ((r == nullptr) || (r->flash == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    if (!r->available) {
        return KILN_ERR_IO;
    }

    /* A run starts on its own sector.  That is what lets iterate() select by run
     * from the sector headers alone, with no index to keep consistent -- and an
     * index that can disagree with the data is the failure mode AD-08 exists to
     * avoid. */
    const uint32_t sector = (r->head_seq == 0 && r->head_slot == 0)
                          ? r->head_sector
                          : next_sector(r, r->head_sector);

    const kiln_err_t e = claim_sector(r, sector, r->head_seq + 1u, run_id);
    if (e != KILN_OK) {
        return e;
    }

    if (sector == r->tail_sector && r->head_seq > 0) {
        r->tail_sector = next_sector(r, sector);
        r->wrapped     = true;
    }
    r->head_sector = sector;
    r->head_slot   = 0;
    r->head_seq   += 1u;
    r->run_id      = run_id;
    r->run_open    = true;
    return KILN_OK;
}

kiln_err_t kiln_logring_append(kiln_logring_t *r,
                               const uint8_t rec[KILN_LOG_RECORD_BYTES])
{
    if ((r == nullptr) || (r->flash == nullptr) || (rec == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    if (!r->available) {
        return KILN_ERR_IO;
    }

    if (r->head_slot >= r->recs_per_sector) {
        /* Wrap.  Erase the next sector now, immediately before writing it --
         * never in advance (architecture 10.3). */
        const uint32_t sector = next_sector(r, r->head_sector);
        const kiln_err_t e = claim_sector(r, sector, r->head_seq + 1u, r->run_id);
        if (e != KILN_OK) {
            return e;
        }

        if (sector == r->tail_sector) {
            r->tail_sector = next_sector(r, sector);
            r->wrapped     = true;
            /* FR-LOG-06: the oldest data is gone, which is the contract, and the
             * run it belonged to is marked truncated by run_index (FR-LOG-09). */
            if (r->records_stored >= r->recs_per_sector) {
                r->records_stored -= r->recs_per_sector;
            } else {
                r->records_stored = 0;
            }
        }
        r->head_sector = sector;
        r->head_slot   = 0;
        r->head_seq   += 1u;
    }

    const kiln_err_t e = r->flash->write(r->flash->ctx,
                                         slot_offset(r, r->head_sector, r->head_slot),
                                         rec, KILN_LOG_RECORD_BYTES);
    if (e != KILN_OK) {
        /* FR-LOG-14: counted, reported, and never fatal to a firing. */
        r->write_errors++;
        return e;
    }

    r->head_slot++;
    r->records_stored++;
    return KILN_OK;
}

/* --- read -------------------------------------------------------------- */

/* Visit every sector in sequence order, oldest first.  Order comes from seq, so
 * a reader is indifferent to where the head happens to sit. */
static kiln_err_t for_each_sector(kiln_logring_t *r, uint32_t run_id,
                                  bool (*fn)(kiln_logring_t *, uint32_t sector,
                                             uint32_t run, void *user),
                                  void *user)
{
    /* The oldest sector is the one after the head, walking forward. */
    for (uint32_t i = 0; i < r->sector_count; i++) {
        const uint32_t sector = (r->head_sector + 1u + i) % r->sector_count;

        kiln_log_sector_hdr_t h;
        if (read_hdr(r, sector, &h) != KILN_OK) {
            continue;
        }
        if (run_id != 0 && h.run_id != run_id) {
            continue;
        }
        if (!fn(r, sector, h.run_id, user)) {
            return KILN_OK;
        }
    }
    return KILN_OK;
}

typedef struct {
    kiln_logstore_visit_fn fn;
    void                  *user;
    bool                   stopped;
} visit_ctx_t;

static bool visit_sector(kiln_logring_t *r, uint32_t sector, uint32_t run, void *user)
{
    visit_ctx_t *v = (visit_ctx_t *)user;

    for (uint32_t slot = 0; slot < r->recs_per_sector; slot++) {
        uint8_t rec[KILN_LOG_RECORD_BYTES];
        if (r->flash->read(r->flash->ctx, slot_offset(r, sector, slot),
                           rec, sizeof(rec)) != KILN_OK) {
            return false;
        }

        kiln_log_sample_t s;
        if (kiln_logrec_decode(rec, &s) != KILN_OK) {
            /* Erased or torn: the rest of this sector is unreachable. */
            return true;
        }
        if (!v->fn(v->user, run, rec)) { v->stopped = true; return false; }
    }
    return true;
}

kiln_err_t kiln_logring_iterate(kiln_logring_t *r, uint32_t run_id,
                                kiln_logstore_visit_fn fn, void *user)
{
    if ((r == nullptr) || (r->flash == nullptr) || (fn == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    if (!r->available) {
        return KILN_ERR_IO;
    }

    visit_ctx_t v = { .fn = fn, .user = user, .stopped = false };
    return for_each_sector(r, run_id, visit_sector, &v);
}

typedef struct {
    uint8_t  rec[KILN_LOG_RECORD_BYTES];
    bool     found;
} last_ctx_t;

static bool keep_last(void *user, uint32_t run, const uint8_t rec[KILN_LOG_RECORD_BYTES])
{
    (void)run;
    last_ctx_t *l = (last_ctx_t *)user;
    memcpy(l->rec, rec, KILN_LOG_RECORD_BYTES);
    l->found = true;
    return true;      /* keep going: the last one to arrive wins */
}

kiln_err_t kiln_logring_last_record(kiln_logring_t *r, uint32_t run_id,
                                    uint8_t rec[KILN_LOG_RECORD_BYTES])
{
    if ((r == nullptr) || (rec == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    last_ctx_t l = {};   /* found == false, and rec zeroed with it */
    const kiln_err_t e = kiln_logring_iterate(r, run_id, keep_last, &l);
    if (e != KILN_OK) {
        return e;
    }
    if (!l.found) {
        return KILN_ERR_NOT_FOUND;
    }

    memcpy(rec, l.rec, KILN_LOG_RECORD_BYTES);
    return KILN_OK;
}

kiln_err_t kiln_logring_stats(kiln_logring_t *r, kiln_logstore_stats_t *out)
{
    if ((r == nullptr) || (out == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->sectors_total   = r->sector_count;
    out->records_total   = kiln_logring_capacity(r);
    out->records_stored  = r->records_stored;
    out->write_errors    = r->write_errors;
    out->erase_count     = r->erase_count;
    out->available       = r->available;
    out->newest_seq      = r->head_seq;

    kiln_log_sector_hdr_t h;
    if (read_hdr(r, r->tail_sector, &h) == KILN_OK) {
        out->oldest_seq = h.seq;
    }

    /* Sectors holding data: the whole ring once it has wrapped, otherwise the
     * span from the tail to the head. */
    if (r->wrapped) {
        out->sectors_used = r->sector_count;
    } else {
        out->sectors_used = r->head_seq;
    }
    return KILN_OK;
}

/* --- port binding ------------------------------------------------------ */

static kiln_err_t ls_begin_run(void *ctx, uint32_t run_id)
{
    return kiln_logring_begin_run((kiln_logring_t *)ctx, run_id);
}
static kiln_err_t ls_append(void *ctx, const uint8_t rec[KILN_LOG_RECORD_BYTES])
{
    return kiln_logring_append((kiln_logring_t *)ctx, rec);
}
static kiln_err_t ls_iterate(void *ctx, uint32_t run_id,
                             kiln_logstore_visit_fn fn, void *user)
{
    return kiln_logring_iterate((kiln_logring_t *)ctx, run_id, fn, user);
}
static kiln_err_t ls_last(void *ctx, uint32_t run_id,
                          uint8_t rec[KILN_LOG_RECORD_BYTES])
{
    return kiln_logring_last_record((kiln_logring_t *)ctx, run_id, rec);
}
static kiln_err_t ls_stats(void *ctx, kiln_logstore_stats_t *out)
{
    return kiln_logring_stats((kiln_logring_t *)ctx, out);
}
static kiln_err_t ls_erase_all(void *ctx)
{
    return kiln_logring_erase_all((kiln_logring_t *)ctx);
}

void kiln_logring_bind(kiln_logring_t *r, kiln_port_logstore_t *out)
{
    if ((r == nullptr) || (out == nullptr)) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->ctx         = r;
    out->begin_run   = ls_begin_run;
    out->append      = ls_append;
    out->iterate     = ls_iterate;
    out->last_record = ls_last;
    out->stats       = ls_stats;
    out->erase_all   = ls_erase_all;
}

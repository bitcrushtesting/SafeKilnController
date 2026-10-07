/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <string.h>

#include "kiln_core/suplink.h"

namespace {

/* port_tc::configure.
 *
 * Refused, and that is the point.  After AD-22 the ESP32 does not configure
 * the chamber front end: the supervisor owns it, and FR-ACQ-02 fixes the type
 * to K precisely so that nothing on this side can change what the supervisor's
 * backstop means.  Returning an error rather than silently succeeding means a
 * caller that still thinks it owns the front end finds out. */
kiln_err_t sl_configure(void *ctx, kiln_tc_type_t type, uint8_t line_filter_hz)
{
    (void)ctx;
    (void)type;
    (void)line_filter_hz;
    return KILN_ERR_UNSUPPORTED;
}

kiln_err_t sl_read(void *ctx, kiln_tc_reading_t *out)
{
    /* const: reading the port does not advance or alter the link state.  The
     * staleness timer is driven by kiln_suplink_tick from the acquisition
     * cycle, so a read is a pure query. */
    const kiln_suplink_t *s = static_cast<const kiln_suplink_t *>(ctx);
    if ((s == nullptr) || (out == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    /* Same shape as the MAX31856 adapter's comms failure, so SR-04 needs no
     * new case: nothing has been heard, so the front end did not answer. */
    if (!s->have || !kiln_suplink_fresh(s)) {
        out->temp_c     = 0.0f;
        out->cj_c       = 0.0f;
        out->fault_bits = KILN_TC_FAULT_COMMS;
        return KILN_ERR_IO;
    }

    out->temp_c = s->last.chamber_c;
    out->cj_c   = s->last.cj_c;
    /* The supervisor's fault bits are KILN_TC_FAULT_* already: it reads the
     * same part and reports its status register unmodified. */
    out->fault_bits = s->last.fault_bits;
    return KILN_OK;
}

}  // namespace

void kiln_suplink_init(kiln_suplink_t *s)
{
    if (s == nullptr) {
        return;
    }
    const kiln_suplink_t zero = {};
    *s = zero;
    /* Starts stale, so heat is withheld until the supervisor has actually been
     * heard from.  The same reasoning as the supervisor's own start-up: absence
     * of evidence is not permission. */
    s->since_s = KILN_SUPLINK_STALE_S;
}

void kiln_suplink_feed(kiln_suplink_t *s, const uint8_t *data, size_t len)
{
    if ((s == nullptr) || (data == nullptr)) {
        return;
    }

    while (len > 0u) {
        /* Fill the window, dropping the oldest if the producer has outrun us.
         * The buffer holds two frames, so a whole frame can always assemble. */
        const size_t space = sizeof(s->rx) - s->rx_len;
        const size_t take  = (len < space) ? len : space;
        if (take > 0u) {
            memcpy(&s->rx[s->rx_len], data, take);
            s->rx_len += take;
            data += take;
            len  -= take;
        }

        /* Drain every complete frame in the window. */
        for (;;) {
            sup_report_t r = {};
            size_t skip = 0;
            const size_t used = sup_decode(s->rx, s->rx_len, &r, &skip);
            if (used == 0u) {
                /* Nothing yet.  sup_decode says how much can never become a
                 * frame; discarding it is what stops a buffer of noise
                 * wedging this loop. */
                if (skip > 0u && skip <= s->rx_len) {
                    memmove(s->rx, &s->rx[skip], s->rx_len - skip);
                    s->rx_len -= skip;
                }
                break;
            }
            s->crc_errors += (uint32_t)(skip / SUP_FRAME_BYTES);
            memmove(s->rx, &s->rx[used], s->rx_len - used);
            s->rx_len -= used;

            if (r.version != SUP_VERSION) {
                /* A supervisor speaking a different version is not a
                 * supervisor this firmware can interpret, and guessing at the
                 * fields would be worse than silence. */
                s->version_errors++;
                continue;
            }

            s->frames++;
            /* A sequence number that has not advanced means the supervisor is
             * repeating itself, which is a different failure from the link
             * being quiet: the bytes are arriving and the content is stale.
             * The timer is not reset, so it ages into a comms fault. */
            if (s->have && (r.seq == s->last.seq)) {
                s->repeats++;
                s->last = r;
                continue;
            }
            s->last    = r;
            s->have    = true;
            s->since_s = 0.0f;
        }

        if (s->rx_len == sizeof(s->rx)) {
            /* Full and still no frame: keep the tail that could start one. */
            const size_t keep = SUP_FRAME_BYTES - 1u;
            memmove(s->rx, &s->rx[s->rx_len - keep], keep);
            s->rx_len = keep;
        }
    }
}

void kiln_suplink_tick(kiln_suplink_t *s, float dt_s)
{
    if (s == nullptr) {
        return;
    }
    if (!(dt_s >= 0.0f)) {          /* also false for NaN */
        return;
    }
    /* Saturating, so a long gap cannot wrap back into "fresh". */
    if (s->since_s < 1.0e6f) {
        s->since_s += dt_s;
    }
}

bool kiln_suplink_fresh(const kiln_suplink_t *s)
{
    return (s != nullptr) && s->have && (s->since_s < KILN_SUPLINK_STALE_S);
}

bool kiln_suplink_status(const kiln_suplink_t *s, sup_report_t *out)
{
    if ((s == nullptr) || (out == nullptr) || !s->have) {
        return false;
    }
    *out = s->last;
    return true;
}

void kiln_suplink_bind(kiln_suplink_t *s, kiln_port_tc_t *out)
{
    if ((s == nullptr) || (out == nullptr)) {
        return;
    }
    out->ctx       = s;
    out->configure = sl_configure;
    out->read      = sl_read;
}

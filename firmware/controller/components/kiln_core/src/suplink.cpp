/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <string.h>

#include "kiln_core/suplink.h"

/* SWA-22's link carries fault_bits across a boundary that shares no code beyond
 * the wire header, so the two names for each bit have to be the same number.
 * A comment saying so was the previous arrangement; this is the version that
 * fails the build instead of misreporting a fault at runtime. */
static_assert(SUP_TC_FAULT_OPEN      == KILN_TC_FAULT_OPEN);
static_assert(SUP_TC_FAULT_SHORT_VCC == KILN_TC_FAULT_SHORT_VCC);
static_assert(SUP_TC_FAULT_SHORT_GND == KILN_TC_FAULT_SHORT_GND);
static_assert(SUP_TC_FAULT_CJ_RANGE  == KILN_TC_FAULT_CJ_RANGE);
static_assert(SUP_TC_FAULT_TC_RANGE  == KILN_TC_FAULT_TC_RANGE);
static_assert(SUP_TC_FAULT_OVUV      == KILN_TC_FAULT_OVUV);
static_assert(SUP_TC_FAULT_COMMS     == KILN_TC_FAULT_COMMS);

namespace {

/* port_tc::configure.
 *
 * Refused, and that is the point.  After SWA-22 the ESP32 does not configure
 * the chamber front end: the supervisor owns it, and SWR-ACQ-02 fixes the type
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

    /* Same shape as the MAX31856 adapter's comms failure, so SWR-SAF-04 needs no
     * new case: nothing has been heard, so the front end did not answer. */
    if (!s->have || !kiln_suplink_fresh(s)) {
        out->temp_c     = 0.0f;
        out->cj_c       = 0.0f;
        out->fault_bits = KILN_TC_FAULT_COMMS;
        return KILN_ERR_IO;
    }

    /* Tenths of a degree on the wire and in sup_report_t, degrees in a float
     * here.  The conversion lives on this side of the link deliberately: the
     * supervisor is a Cortex-M0+ with no FPU, where a float is a libgcc helper
     * call inside a safety function, and this is an ESP32 with hardware
     * floating point and a port contract already expressed in degrees. */
    out->temp_c = (float)s->last.chamber_dc / (float)SUP_DC_PER_C;
    out->cj_c   = (float)s->last.cj_dc / (float)SUP_DC_PER_C;
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
                    s->discarded += (uint32_t)skip;
                    memmove(s->rx, &s->rx[skip], s->rx_len - skip);
                    s->rx_len -= skip;
                }
                break;
            }
            /* Whatever sat in front of the accepted frame was noise. */
            s->discarded += (uint32_t)skip;
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
            s->discarded += (uint32_t)(s->rx_len - keep);
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

kiln_sup_reason_t kiln_suplink_reason(const kiln_suplink_t *s)
{
    if (s == nullptr) {
        return KILN_SUP_LINK_DEAD;
    }
    /* Never heard from, or heard from and then not: both are a dead link, and
     * neither can be reported by the supervisor itself, which is why this
     * reason exists on this side only.  It is checked first because a stale
     * frame's trip reason describes the past. */
    if (!s->have || !kiln_suplink_fresh(s)) {
        return KILN_SUP_LINK_DEAD;
    }
    switch (s->last.trip_reason) {
    case SUP_TRIP_NONE:         return KILN_SUP_OK;
    case SUP_TRIP_OVERTEMP:     return KILN_SUP_OVERTEMP;
    case SUP_TRIP_TC_FAULT:     return KILN_SUP_TC_FAULT;
    case SUP_TRIP_SENSOR_STALE: return KILN_SUP_SENSOR_STALE;
    case SUP_TRIP_SELF_TEST:    return KILN_SUP_SELF_TEST;
    case SUP_TRIP_TC_DISAGREE:  return KILN_SUP_TC_DISAGREE;
    case SUP_TRIP_PERMIT_STUCK: return KILN_SUP_OUTPUT_STUCK;
    case SUP_TRIP_COUNT:
    default:
        /* A reason this firmware does not know about, from a supervisor
         * speaking the same version.  Reported as a fault rather than as OK:
         * an unrecognised trip is still a trip. */
        return KILN_SUP_TC_FAULT;
    }
}

namespace {

bool sl_status(void *ctx, kiln_sup_status_t *out)
{
    const kiln_suplink_t *s = static_cast<const kiln_suplink_t *>(ctx);
    if ((s == nullptr) || (out == nullptr)) {
        return false;
    }
    const kiln_sup_status_t zero = {};
    *out = zero;
    out->reason     = kiln_suplink_reason(s);
    out->link_ok    = kiln_suplink_fresh(s);
    out->frames     = s->frames;
    out->discarded  = s->discarded;
    out->repeats    = s->repeats;
    if (s->have) {
        out->permitting = (s->last.flags & SUP_FLAG_PERMIT) != 0u;
        out->tripped    = (s->last.flags & SUP_FLAG_TRIPPED) != 0u;
    }
    /* A dead link is not a permission, whatever the last frame said. */
    if (!out->link_ok) {
        out->permitting = false;
    }
    return s->have;
}

}  // namespace

void kiln_suplink_bind_supervisor(kiln_suplink_t *s, kiln_port_supervisor_t *out)
{
    if ((s == nullptr) || (out == nullptr)) {
        return;
    }
    out->ctx    = s;
    out->status = sl_status;
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

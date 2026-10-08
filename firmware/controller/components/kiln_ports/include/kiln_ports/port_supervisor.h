/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What the independent supervisor is doing (SWA-22), for the operator's benefit.
 *
 * Optional: a build without a supervisor leaves it null and the interface
 * simply says nothing, the same way SWR-SAF-11 stands down without an enclosure
 * probe.
 *
 * ---------------------------------------------------------------------------
 * Why this has its own vocabulary
 * ---------------------------------------------------------------------------
 * The supervisor's trip reasons arrive over the wire as sup_trip_reason_t, and
 * this deliberately does not reuse that enum.  Two reasons.
 *
 * The wire format is versioned and belongs to the supervisor; what the operator
 * is shown belongs here, and translating between them once, in a tested
 * function, is better than every screen and route depending on the protocol
 * header.
 *
 * And one reason has no wire representation at all.  A supervisor that has gone
 * silent cannot report that it is silent, so KILN_SUP_LINK_DEAD is inferred on
 * this side.  It is the most important one to be able to show: it is the
 * difference between "the backstop fired" and "the backstop is missing".
 */
#ifndef KILN_PORT_SUPERVISOR_H
#define KILN_PORT_SUPERVISOR_H

#include "kiln/err.h"
#include "kiln/types.h"

typedef enum {
    KILN_SUP_OK = 0,            /* permitting, nothing latched               */
    KILN_SUP_OVERTEMP,          /* the hard-coded backstop fired             */
    KILN_SUP_TC_FAULT,          /* the front end reported a fault            */
    KILN_SUP_SENSOR_STALE,      /* a reading was working and stopped         */
    KILN_SUP_SELF_TEST,         /* start-up self-test failed; never permits  */
    KILN_SUP_LINK_DEAD,         /* inferred here: nothing is arriving        */
    KILN_SUP_REASON_COUNT
} kiln_sup_reason_t;

typedef struct {
    kiln_sup_reason_t reason;
    bool     permitting;        /* the supervisor is allowing heat right now  */
    bool     tripped;           /* latched; needs the button on the panel     */
    bool     link_ok;           /* a fresh frame arrived recently             */
    /* Diagnostics.  Climbing `discarded` with `frames` also climbing is a
     * wiring or noise problem; `frames` stopping altogether is a dead
     * supervisor.  Telling those apart in the field is the whole reason they
     * are reported.
     *
     * Bytes and not "CRC errors", deliberately.  Resynchronisation slides a
     * window until the CRC checks, so in a stream of noise there are no frame
     * boundaries to count and any frame-shaped number would be invented.
     * Discarded bytes is what can actually be measured. */
    uint32_t frames;        /* accepted                                    */
    uint32_t discarded;     /* bytes thrown away without forming a frame   */
    uint32_t repeats;       /* accepted, but the sequence had not advanced */
} kiln_sup_status_t;

typedef struct kiln_port_supervisor {
    void *ctx;
    /* False when nothing has ever been heard, so a caller can distinguish
     * "never spoke to us" from "spoke, and is unhappy". */
    bool (*status)(void *ctx, kiln_sup_status_t *out);
} kiln_port_supervisor_t;

#endif /* KILN_PORT_SUPERVISOR_H */

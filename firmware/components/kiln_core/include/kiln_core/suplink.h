/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The ESP32's end of the supervisor link (AD-22).
 *
 * After AD-22 the chamber thermocouple belongs to the supervisor, which reports
 * it ten times a second over a one-wire serial link.  This component turns that
 * byte stream into a `kiln_port_tc_t`, so nothing in the core had to change:
 * the control and safety paths still read a thermocouple port, and only the
 * thing behind the port moved.
 *
 * It lives in kiln_core rather than in the ESP32 adapter for the reason AD-19
 * and AD-21 give for the log ring and the file store: everything interesting
 * here is a decision with a failure mode.  Frames arrive split across reads,
 * line noise has to be resynchronised past, a repeated sequence number is a
 * different fault from silence, and a dead link has to present as something the
 * existing safety rules already understand.  Behind a UART driver none of that
 * is testable; here it is driven by a test feeding bytes.
 *
 * ---------------------------------------------------------------------------
 * A dead link is a thermocouple comms fault, deliberately
 * ---------------------------------------------------------------------------
 * When the link goes quiet, `read` reports `KILN_TC_FAULT_COMMS` and returns an
 * error, which is exactly what the MAX31856 adapter does when the part does not
 * answer.  So SR-04's existing grace period and fault latch handle a dead
 * supervisor with no new rule and no special case: from the core's point of
 * view the front end stopped answering, which is true.
 */
#ifndef KILN_CORE_SUPLINK_H
#define KILN_CORE_SUPLINK_H

#include <stddef.h>

#include "kiln/err.h"
#include "kiln/types.h"
#include "kiln_ports/port_supervisor.h"
#include "kiln_ports/port_tc.h"
#include "sup_proto.h"

/* Sized against the rate this is *pumped*, not the rate the supervisor sends.
 *
 * The supervisor reports every 100 ms, but the acquisition cycle drains the
 * UART every 250 ms (which is FR-ACQ-03's 4 Hz), so the timer advances in
 * 250 ms steps and a threshold near the report period would trip on one late
 * cycle.  0.75 s is three acquisition cycles: three missed pumps, or about
 * seven missed frames.
 *
 * It does not need to be shorter.  A quiet link is an *availability* problem,
 * not a protection one: the supervisor's own trip is local and entirely
 * unaffected by whether anyone is listening.  What this threshold decides is
 * how quickly the ESP32 stops believing a stale temperature, and it then feeds
 * FR-ACQ-12's grace (1 to 30 s, default 5 s), which is what actually latches a
 * fault.  Being well inside that grace is the requirement; being fast is not. */
#define KILN_SUPLINK_STALE_S 0.75f

typedef struct {
    /* Partial frames: a UART read can split one anywhere. */
    uint8_t      rx[SUP_FRAME_BYTES * 2u];
    size_t       rx_len;

    sup_report_t last;          /* the last accepted frame            */
    bool         have;          /* one has been accepted at all       */
    float        since_s;       /* since the last frame with a new seq */

    /* Diagnostics.  FR-NET-09 asks for link counters and these are the
     * supervisor link's equivalent: bytes discarded while frames still arrive
     * is noise or wiring, frames stopping is a dead supervisor. */
    uint32_t     frames;
    uint32_t     discarded;     /* bytes dropped without forming a frame */
    uint32_t     version_errors;
    uint32_t     repeats;       /* a frame whose seq had not advanced    */
} kiln_suplink_t;

void kiln_suplink_init(kiln_suplink_t *s);

/* Hand over whatever the UART produced.  Any number of bytes, including a
 * partial frame or several frames at once. */
void kiln_suplink_feed(kiln_suplink_t *s, const uint8_t *data, size_t len);

/* Advance the staleness timer.  Called once per acquisition cycle. */
void kiln_suplink_tick(kiln_suplink_t *s, float dt_s);

/* True when a frame with a fresh sequence number arrived recently enough. */
bool kiln_suplink_fresh(const kiln_suplink_t *s);

/* The supervisor's own state, for the HMI and the API to be able to say why
 * the heat went away.  Returns false if nothing has ever been received. */
bool kiln_suplink_status(const kiln_suplink_t *s, sup_report_t *out);

/* Bind as the chamber thermocouple port. */
void kiln_suplink_bind(kiln_suplink_t *s, kiln_port_tc_t *out);

/* Bind as the supervisor status port, so the HMI and the API can say which
 * condition tripped rather than only that a thermocouple is unhappy. */
void kiln_suplink_bind_supervisor(kiln_suplink_t *s, kiln_port_supervisor_t *out);

/* The wire reason translated into the operator's vocabulary, with the one
 * reason the wire cannot carry (a silent supervisor) inferred from staleness.
 * Exposed for its own test. */
kiln_sup_reason_t kiln_suplink_reason(const kiln_suplink_t *s);

#endif /* KILN_CORE_SUPLINK_H */

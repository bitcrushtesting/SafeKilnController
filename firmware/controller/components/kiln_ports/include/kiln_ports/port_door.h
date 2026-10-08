/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Door / lid interlock input (SYS-HW-21, SWR-SAF-31).
 *
 * Deliberately not part of port_input, which is the rotary encoder: that is an
 * HMI device whose events may be dropped, coalesced or arrive late.  This is a
 * safety input read by the safety supervisor every cycle, and conflating the
 * two would put a safety signal behind an event queue.
 *
 * WIRING IS PART OF THE CONTRACT.  The switch shall be **normally closed** --
 * closed contact means door shut -- so that a cut wire, a pulled connector or a
 * failed switch all read "open" and remove heat.  A normally-open switch would
 * make every one of those failures read "door shut", which is the wrong answer
 * from a safety input and the opposite of SYS-SAF-01.
 *
 * And note what this port is NOT: it is the controller's *knowledge* of the
 * door, not the interlock itself.  SYS-HW-21 additionally requires the switch to
 * break the contactor coil circuit in hardware, so the heater drops even if
 * this firmware is wrong, hung or malicious -- the same argument SWA-05 makes
 * for the charge pump.  This port exists so the controller can latch, annunciate
 * and log the event, not so it can be the only thing standing in the way.
 */
#ifndef KILN_PORT_DOOR_H
#define KILN_PORT_DOOR_H

#include "kiln/types.h"

typedef struct kiln_port_door {
    void *ctx;

    /* True when the door is open -- which, with the normally-closed wiring
     * above, is also what a broken circuit reads as.  An adapter that cannot
     * read the pin shall report open, never closed. */
    bool (*is_open)(void *ctx);

    /* False when no interlock is fitted on this board or it is configured off.
     * The supervisor then stands the rule down and raises warning 113, rather
     * than reading a floating pin as a door that keeps opening (SWR-CUR-12 takes
     * the same approach to a missing current transformer). */
    bool (*is_present)(void *ctx);
} kiln_port_door_t;

#endif /* KILN_PORT_DOOR_H */

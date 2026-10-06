/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Phase-count configuration input (HR-22, FR-CUR-15).
 *
 * One pin, read once at boot: **pulled up means three-phase, pulled down means
 * single-phase**.  A strap and not a configuration item, because it describes
 * how the kiln is *wired* rather than how the operator wants it run -- getting
 * it from the board means a controller moved between installations cannot carry
 * a stale setting with it, and nobody can make the two disagree over the API.
 *
 * It is read once rather than polled: the number of phases a kiln has does not
 * change while it is firing, and a loose strap flapping mid-run must not be
 * able to rescale the energy total or the deviation bands underneath SR-28.
 *
 * This affects measurement and reporting only.  It is NOT a safety input: the
 * per-phase rules of SR-25..SR-30 act on whichever channels are actually
 * delivering, so a strap that disagrees with the fitted transformers produces
 * warning 114 and not a wrong safety decision.
 */
#ifndef KILN_PORT_PHASE_H
#define KILN_PORT_PHASE_H

#include "kiln/types.h"

typedef struct kiln_port_phase {
    void *ctx;

    /* 1 or 3.  An adapter that cannot read the pin shall report 1: a
     * single-phase assumption under-reports power on a three-phase kiln, which
     * is visibly wrong and gets fixed, where over-reporting on a single-phase
     * kiln looks plausible and does not. */
    uint8_t (*count)(void *ctx);
} kiln_port_phase_t;

#endif /* KILN_PORT_PHASE_H */

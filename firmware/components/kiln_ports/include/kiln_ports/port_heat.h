/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The heating output stage.  AD-04: only the safety supervisor is permitted to
 * call set_duty() or enable_refresh(); the control path merely requests a duty.
 */
#ifndef KILN_PORT_HEAT_H
#define KILN_PORT_HEAT_H

#include "kiln/types.h"

typedef struct kiln_port_heat {
    void *ctx;
    /* Publish the duty the 10 ms window callback is to realise (AD-07). */
    void (*set_duty)(void *ctx, uint16_t permille);
    /* AD-05 / HR-07 / SR-02: one edge into the charge pump.  Stop calling this
     * and the contactor coil de-energises within ~1 s, with no code involved. */
    void (*enable_refresh)(void *ctx);
    /* Immediate: duty 0 and heat authority withdrawn (SR-16). */
    void (*force_off)(void *ctx);
    /* True once force_off has taken effect at the pin. */
    bool (*is_off)(void *ctx);
} kiln_port_heat_t;

#endif

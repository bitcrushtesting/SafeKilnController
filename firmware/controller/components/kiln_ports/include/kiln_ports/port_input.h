/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Rotary encoder with push button (HR-05, FR-HMI-09).
 */
#ifndef KILN_PORT_INPUT_H
#define KILN_PORT_INPUT_H

#include "kiln/types.h"

typedef enum {
    KILN_INPUT_NONE = 0,
    KILN_INPUT_CW,
    KILN_INPUT_CCW,
    KILN_INPUT_PRESS,        /* short press */
    KILN_INPUT_LONG_PRESS,   /* >= 400 ms   */
} kiln_input_event_t;

typedef struct kiln_port_input {
    void *ctx;
    /* Returns KILN_INPUT_NONE when the queue is empty. */
    kiln_input_event_t (*poll)(void *ctx);
} kiln_port_input_t;

#endif

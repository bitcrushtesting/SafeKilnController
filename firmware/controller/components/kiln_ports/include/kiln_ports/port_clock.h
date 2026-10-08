/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * SWA-02 / SWR-TST-03: the core never reads a clock.  Time arrives either as a dt
 * argument or through this port, so a 168 h firing simulates in milliseconds.
 */
#ifndef KILN_PORT_CLOCK_H
#define KILN_PORT_CLOCK_H

#include "kiln/types.h"

typedef struct kiln_port_clock {
    void    *ctx;
    /* SWR-NET-08: monotonic, never stepped by a wall-clock correction. */
    uint64_t (*now_monotonic_us)(void *ctx);
    /* SWR-LOG-12: wall clock, valid only once SNTP has succeeded. */
    bool     (*wall_valid)(void *ctx);
    uint64_t (*now_wall_utc_s)(void *ctx);
} kiln_port_clock_t;

#endif

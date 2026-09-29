/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Time-proportional output -- architecture section 7.3, FR-CTL-07, FR-CTL-08.
 *
 * An SSR cannot be driven at an analogue level, so a duty becomes on/off
 * intervals inside a fixed window.  This is pure logic; the 10 ms timer
 * callback that calls it lives in the HAL (AD-07).
 */
#ifndef KILN_CORE_WINDOW_H
#define KILN_CORE_WINDOW_H

#include "kiln/types.h"

typedef struct {
    uint32_t window_ms;    /* 500 .. 30000, default 2000 */
    uint32_t tick_ms;      /* callback period, default 10 */
    uint32_t min_on_ms;    /* 0 .. 2000, default 100     */
    uint32_t min_off_ms;
} kiln_window_cfg_t;

typedef struct {
    kiln_window_cfg_t cfg;
    uint32_t ticks_per_window;
    uint32_t tick;              /* position within the window */
    uint32_t since_change_ms;
    bool     on;
} kiln_window_t;

void kiln_window_init(kiln_window_t *w, const kiln_window_cfg_t *cfg);
void kiln_window_reconfigure(kiln_window_t *w, const kiln_window_cfg_t *cfg);

/* Quantise a requested duty to one the minimum on/off times can actually
 * realise: too short an on-time becomes fully off, too short an off-time
 * becomes fully on (FR-CTL-08). */
uint16_t kiln_window_quantise(const kiln_window_t *w, uint16_t duty_permille);

/* Advance one tick.  Returns the level the SSR pin should now hold.
 * authorised == false forces off immediately, overriding min_on_ms (AD-04). */
bool kiln_window_tick(kiln_window_t *w, uint16_t duty_permille, bool authorised);

#endif

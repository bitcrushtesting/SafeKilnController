/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "kiln_core/window.h"

static void recompute(kiln_window_t *w)
{
    if (w->cfg.tick_ms == 0) w->cfg.tick_ms = 10;
    if (w->cfg.window_ms < w->cfg.tick_ms) w->cfg.window_ms = w->cfg.tick_ms;

    w->ticks_per_window = w->cfg.window_ms / w->cfg.tick_ms;
    if (w->ticks_per_window == 0) w->ticks_per_window = 1;
}

void kiln_window_init(kiln_window_t *w, const kiln_window_cfg_t *cfg)
{
    const kiln_window_t zero = {0};
    *w = zero;
    w->cfg = *cfg;
    recompute(w);
}

void kiln_window_reconfigure(kiln_window_t *w, const kiln_window_cfg_t *cfg)
{
    w->cfg = *cfg;
    recompute(w);
    if (w->tick >= w->ticks_per_window) w->tick = 0;
}

uint16_t kiln_window_quantise(const kiln_window_t *w, uint16_t duty_permille)
{
    if (duty_permille == 0) return 0;
    if (duty_permille >= KILN_DUTY_MAX) return KILN_DUTY_MAX;

    const uint32_t on_ms  = (uint32_t)duty_permille * w->cfg.window_ms / KILN_DUTY_MAX;
    const uint32_t off_ms = w->cfg.window_ms - on_ms;

    if (on_ms  < w->cfg.min_on_ms)  return 0;
    if (off_ms < w->cfg.min_off_ms) return KILN_DUTY_MAX;
    return duty_permille;
}

bool kiln_window_tick(kiln_window_t *w, uint16_t duty_permille, bool authorised)
{
    bool want;

    if (!authorised) {
        /* Withdrawal of authority is immediate and outranks every minimum
         * time: SR-16 requires duty zero without waiting for a window edge. */
        if (w->on) {
            w->on = false;
            w->since_change_ms = 0;
        } else {
            w->since_change_ms += w->cfg.tick_ms;
        }
        w->tick = (w->tick + 1u) % w->ticks_per_window;
        return false;
    }

    const uint16_t duty = kiln_window_quantise(w, duty_permille);

    /* On for the first `duty` fraction of the window. */
    want = ((uint64_t)w->tick * KILN_DUTY_MAX)
         < ((uint64_t)duty * w->ticks_per_window);

    /* Minimum dwell in the present level, so the SSR cannot chatter. */
    if (want != w->on) {
        const uint32_t min_ms = w->on ? w->cfg.min_on_ms : w->cfg.min_off_ms;
        if (w->since_change_ms < min_ms) {
            want = w->on;
        }
    }

    if (want != w->on) {
        w->on = want;
        w->since_change_ms = 0;
    } else {
        w->since_change_ms += w->cfg.tick_ms;
    }

    w->tick = (w->tick + 1u) % w->ticks_per_window;
    return w->on;
}

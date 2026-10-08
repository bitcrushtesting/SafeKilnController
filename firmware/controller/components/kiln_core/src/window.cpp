/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "kiln_core/window.h"

void kiln_window_cfg_defaults(kiln_window_cfg_t *cfg)
{
    const kiln_window_cfg_t d = {
        .window_ms            = KILN_WINDOW_MS_DEFAULT,
        .tick_ms              = KILN_TICK_MS_DEFAULT,
        .min_on_ms            = 100u,
        .min_off_ms           = 100u,
        .preserve_off_window  = true,     /* SR-25 is the primary detection */
        .min_measure_off_ms   = 100u,     /* 20 ms settle + 40 ms burst + margin */
    };
    *cfg = d;
}

namespace {

/* Bound the configuration, and report whether anything had to move. */
bool validate(kiln_window_cfg_t *c)
{
    bool corrected = false;

    if (c->tick_ms == 0) { c->tick_ms = KILN_TICK_MS_DEFAULT; corrected = true; }

    if (c->window_ms < KILN_WINDOW_MS_MIN) { c->window_ms = KILN_WINDOW_MS_MIN; corrected = true; }
    if (c->window_ms > KILN_WINDOW_MS_MAX) { c->window_ms = KILN_WINDOW_MS_MAX; corrected = true; }
    if (c->window_ms < c->tick_ms)         { c->window_ms = c->tick_ms;         corrected = true; }

    /* FR-CTL-07: a whole number of ticks, or the duty resolution silently lies. */
    if (c->window_ms % c->tick_ms != 0) {
        c->window_ms -= c->window_ms % c->tick_ms;
        if (c->window_ms < c->tick_ms) {
            c->window_ms = c->tick_ms;
        }
        corrected = true;
    }

    if (c->min_on_ms > KILN_MIN_ON_MS_MAX)  { c->min_on_ms  = KILN_MIN_ON_MS_MAX; corrected = true; }
    if (c->min_off_ms > KILN_MIN_ON_MS_MAX) { c->min_off_ms = KILN_MIN_ON_MS_MAX; corrected = true; }

    if (c->preserve_off_window) {
        /* The reserved off interval has to be an interval the minimum off time
         * allows, or the two rules fight and the quantiser loses. */
        if (c->min_measure_off_ms < c->min_off_ms) {
            c->min_measure_off_ms = c->min_off_ms;
            corrected = true;
        }
        /* And it has to leave the window something to heat with. */
        if (c->min_measure_off_ms > c->window_ms / 2u) {
            c->min_measure_off_ms = c->window_ms / 2u;
            corrected = true;
        }
    }

    return corrected;
}

void recompute(kiln_window_t *w)
{
    w->ticks_per_window = w->cfg.window_ms / w->cfg.tick_ms;
    if (w->ticks_per_window == 0) {
        w->ticks_per_window = 1;
    }
}

} // namespace

kiln_err_t kiln_window_init(kiln_window_t *w, const kiln_window_cfg_t *cfg)
{
    if ((w == nullptr) || (cfg == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    const kiln_window_t zero = {};
    *w = zero;
    w->cfg = *cfg;
    const bool corrected = validate(&w->cfg);
    recompute(w);
    return corrected ? KILN_ERR_RANGE : KILN_OK;
}

kiln_err_t kiln_window_reconfigure(kiln_window_t *w, const kiln_window_cfg_t *cfg)
{
    if ((w == nullptr) || (cfg == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    w->cfg = *cfg;
    const bool corrected = validate(&w->cfg);
    recompute(w);
    if (w->tick >= w->ticks_per_window) {
        w->tick = 0;
    }
    return corrected ? KILN_ERR_RANGE : KILN_OK;
}

namespace {

/* The largest duty that still leaves min_measure_off_ms of off-time. */
uint16_t measurable_max_duty(const kiln_window_t *w)
{
    if (w->cfg.min_measure_off_ms >= w->cfg.window_ms) {
        return 0;
    }
    const uint32_t on_ms = w->cfg.window_ms - w->cfg.min_measure_off_ms;
    return (uint16_t)(on_ms * KILN_DUTY_MAX / w->cfg.window_ms);
}

} // namespace

uint16_t kiln_window_quantise(const kiln_window_t *w, uint16_t duty_permille)
{
    if (w == nullptr) {
        return 0;
    }
    if (duty_permille == 0) {
        return 0;
    }

    if (duty_permille >= KILN_DUTY_MAX) {
        return w->cfg.preserve_off_window ? measurable_max_duty(w) : KILN_DUTY_MAX;
    }

    const uint32_t on_ms  = (uint32_t)duty_permille * w->cfg.window_ms / KILN_DUTY_MAX;
    const uint32_t off_ms = w->cfg.window_ms - on_ms;

    if (on_ms < w->cfg.min_on_ms) {
        return 0;
    }

    if (off_ms < w->cfg.min_off_ms) {
        /* FR-CTL-08 would promote this to fully on.  With current monitoring in
         * play that silently deletes the leakage measurement SR-25 depends on, so
         * cap instead -- see kiln_window_cfg_t. */
        return w->cfg.preserve_off_window ? measurable_max_duty(w) : KILN_DUTY_MAX;
    }
    return duty_permille;
}

uint32_t kiln_window_level_remaining_ms(const kiln_window_t *w, uint16_t duty_permille)
{
    if ((w == nullptr) || w->ticks_per_window == 0) {
        return 0;
    }

    const uint16_t duty     = kiln_window_quantise(w, duty_permille);
    const uint32_t on_ticks = (uint32_t)((uint64_t)duty * w->ticks_per_window / KILN_DUTY_MAX);

    /* The level is on for the first on_ticks of the window and off for the rest,
     * so the remaining run of the present level is whichever boundary is next. */
    const uint32_t boundary = (w->tick < on_ticks) ? on_ticks : w->ticks_per_window;
    return (boundary - w->tick) * w->cfg.tick_ms;
}

bool kiln_window_tick(kiln_window_t *w, uint16_t duty_permille, bool authorised)
{
    bool want;

    if (w == nullptr) {
        return false;
    }

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
        if (w->on) {
            w->switch_count++; /* FR-CUR-13 */
        }
    } else {
        w->since_change_ms += w->cfg.tick_ms;
    }

    w->tick = (w->tick + 1u) % w->ticks_per_window;
    return w->on;
}

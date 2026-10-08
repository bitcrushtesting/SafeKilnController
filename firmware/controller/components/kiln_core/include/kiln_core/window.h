/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Time-proportional output -- architecture section 7.3, SWR-CTL-07, SWR-CTL-08.
 *
 * An SSR cannot be driven at an analogue level, so a duty becomes on/off
 * intervals inside a fixed window.  This is pure logic; the 10 ms timer
 * callback that calls it lives in the HAL (SWA-07).
 */
#ifndef KILN_CORE_WINDOW_H
#define KILN_CORE_WINDOW_H

#include "kiln/err.h"
#include "kiln/types.h"

constexpr uint32_t KILN_WINDOW_MS_MIN     = 500u;  /* SWR-CTL-07 */
constexpr uint32_t KILN_WINDOW_MS_MAX     = 30000u;
constexpr uint32_t KILN_WINDOW_MS_DEFAULT = 2000u;
constexpr uint32_t KILN_TICK_MS_DEFAULT   = 10u;   /* SWA-07 */
constexpr uint32_t KILN_MIN_ON_MS_MAX     = 2000u;

typedef struct {
    uint32_t window_ms;    /* 500 .. 30000, default 2000; multiple of tick_ms */
    uint32_t tick_ms;      /* callback period, default 10                     */
    uint32_t min_on_ms;    /* 0 .. 2000, default 100                          */
    uint32_t min_off_ms;

    /* SWR-CUR-04 interacts with SWR-CTL-08 here, and the interaction is easy to
     * miss: quantise promotes a duty whose off-time is shorter than min_off_ms to
     * a full 100 %, which removes the commanded-off interval that a leakage
     * measurement needs -- and a leakage measurement is how SWR-SAF-25 detects a relay
     * stuck on, the *primary* detection per requirements section 5.2.  Left
     * alone, the controller would go blind to a stuck relay in exactly the
     * conditions where it is working hardest.
     *
     * With this set, a duty that would otherwise be promoted is instead capped so
     * that min_measure_off_ms of off-time survives.  The cost is a fraction of a
     * percent of heating power at saturation; the gain is that the rule keeps
     * working.  min_measure_off_ms must be at least min_off_ms (so the off pulse
     * is realisable) and should cover the settle delay plus one burst. */
    bool     preserve_off_window;
    uint32_t min_measure_off_ms;
} kiln_window_cfg_t;

typedef struct {
    kiln_window_cfg_t cfg;
    uint32_t ticks_per_window;
    uint32_t tick;              /* position within the window */
    uint32_t since_change_ms;
    bool     on;
    uint32_t switch_count;      /* SWR-CUR-13: on-transitions since init */
} kiln_window_t;

/* Both return KILN_ERR_RANGE when the configuration had to be corrected, having
 * applied a usable correction -- SWR-NFR-17: a silently rewritten control parameter
 * is a defect, and the caller is the only one who can report it to the operator.
 *
 * SWR-CTL-07 in particular: ticks_per_window is an integer division, so a window
 * of 2505 ms at a 10 ms tick silently becomes 2500 ms and every duty computed
 * against it is quietly 0.2 % out.  A window that is not a whole number of ticks
 * is therefore rounded to one and reported, not accepted. */
kiln_err_t kiln_window_init(kiln_window_t *w, const kiln_window_cfg_t *cfg);
kiln_err_t kiln_window_reconfigure(kiln_window_t *w, const kiln_window_cfg_t *cfg);

void kiln_window_cfg_defaults(kiln_window_cfg_t *cfg);

/* Quantise a requested duty to one the minimum on/off times can actually
 * realise: too short an on-time becomes fully off, too short an off-time
 * becomes fully on (SWR-CTL-08) -- or, with preserve_off_window set, is capped so
 * a measurable off interval remains. */
uint16_t kiln_window_quantise(const kiln_window_t *w, uint16_t duty_permille);

/* Advance one tick.  Returns the level the SSR pin should now hold.
 * authorised == false forces off immediately, overriding min_on_ms (SWA-04). */
bool kiln_window_tick(kiln_window_t *w, uint16_t duty_permille, bool authorised);

/* Milliseconds the present level still has to run, which is what the current
 * sampler needs in order to decide whether a measurement fits (SWR-CUR-05). */
uint32_t kiln_window_level_remaining_ms(const kiln_window_t *w, uint16_t duty_permille);

#endif

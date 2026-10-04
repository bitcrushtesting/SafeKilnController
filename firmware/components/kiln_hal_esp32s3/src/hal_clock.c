/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * port_clock over esp_timer and the system wall clock.
 *
 * AD-02 keeps the core away from both: time arrives as a dt argument or through
 * this port, which is what lets a 168 h firing simulate in milliseconds.  The
 * two clocks are deliberately separate -- FR-NET-08 requires the monotonic one
 * never to be stepped by an SNTP correction, and a run whose elapsed time jumped
 * backwards would take its dwell timers with it.
 */

#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "esp_timer.h"
#include "kiln_hal/hal_esp32s3.h"

/* A wall-clock reading is only meaningful once something has set it.  The epoch
 * cutoff is crude but sufficient: SNTP sets a plausible date, and the boot
 * default is 1970. */
#define WALL_PLAUSIBLE_AFTER 1700000000ull   /* 2023-11-14 */

static uint64_t clk_mono_us(void *ctx)
{
    (void)ctx;
    /* Monotonic since boot, 64-bit, never stepped (FR-NET-08). */
    return (uint64_t)esp_timer_get_time();
}

static uint64_t clk_wall_s(void *ctx)
{
    (void)ctx;
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) return 0;
    return (uint64_t)tv.tv_sec;
}

static bool clk_wall_valid(void *ctx)
{
    /* FR-LOG-12: records logged before time sync are marked as such, so this has
     * to answer honestly rather than returning a number that looks like a date. */
    return clk_wall_s(ctx) >= WALL_PLAUSIBLE_AFTER;
}

void kiln_hal_clock_init(kiln_port_clock_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->now_monotonic_us = clk_mono_us;
    out->now_wall_utc_s   = clk_wall_s;
    out->wall_valid       = clk_wall_valid;
}

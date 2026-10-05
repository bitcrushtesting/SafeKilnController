/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The heating output stage.  AD-04: only the safety supervisor is permitted to
 * call set_duty() or enable_refresh(); the control path merely requests a duty.
 *
 * HR-12: the board switches two SSR channels (SSR1 on IO4, SSR2 on IO5), so the
 * duty is per channel.  A single-zone kiln -- ASM-02, and the only configuration
 * the control path currently produces -- drives both channels with the same
 * duty; kiln_port_heat_set_duty_all() is the call for that case, so a caller
 * cannot leave the second group unswitched by forgetting it exists.
 */
#ifndef KILN_PORT_HEAT_H
#define KILN_PORT_HEAT_H

#include "kiln/types.h"

typedef struct kiln_port_heat {
    void *ctx;

    /* How many channels this board actually populates (1 or 2). */
    uint8_t (*channel_count)(void *ctx);

    /* Publish the duty the window is to realise (AD-07).  Telemetry, and an
     * adapter that would rather own its own window than be driven tick by tick. */
    void (*set_duty)(void *ctx, uint8_t channel, uint16_t permille);

    /* The pin write for the present 10 ms tick, as decided by
     * kiln_window_tick().  Separate from set_duty because a duty is a *request*
     * and this is the pin: AD-17's current sampler has to know which of the two
     * it is measuring against, and only the pin can tell it. */
    void (*set_level)(void *ctx, uint8_t channel, bool on);

    /* AD-05 / HR-07 / SR-02: one edge into the charge pump.  Stop calling this
     * and the contactor coil de-energises within ~1 s, with no code involved.
     * Channel-independent: there is one contactor ahead of both SSRs. */
    void (*enable_refresh)(void *ctx);

    /* SR-27: de-assert heat enable so the contactor opens, without waiting for
     * the charge pump to decay.  This is the step whose effect the weld
     * discrimination measures. */
    void (*drop_contactor)(void *ctx);

    /* Immediate, every channel: duty 0 and heat authority withdrawn (SR-16). */
    void (*force_off)(void *ctx);

    /* True once force_off has taken effect at every pin. */
    bool (*is_off)(void *ctx);

    /* FR-CUR-13: switching operations observed at the pin since boot, which is
     * the only place that can count them exactly.  Channel KILN_HEAT_CHANNELS
     * is not valid here; the contactor's own count is in port_counters. */
    uint32_t (*switch_count)(void *ctx, uint8_t channel);
} kiln_port_heat_t;

/* Convenience for the single-zone case (ASM-02). */
static inline void kiln_port_heat_set_duty_all(const kiln_port_heat_t *h,
                                               uint16_t permille)
{
    if ((h == nullptr) || (h->set_duty == nullptr)) {
        return;
    }
    const uint8_t n = (h->channel_count != nullptr) ? h->channel_count(h->ctx) : 1u;
    for (uint8_t c = 0; c < n && c < KILN_HEAT_CHANNELS; c++) {
        h->set_duty(h->ctx, c, permille);
    }
}

#endif /* KILN_PORT_HEAT_H */

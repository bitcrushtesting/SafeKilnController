/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Heater current front end -- SWR-CUR-01..SWR-CUR-05, SWR-CUR-11, SWR-CUR-14.
 *
 * SWA-17: the measurement is gated to the commanded output state, so the port is
 * driven from the same 10 ms timer that owns the SSR pin and not by an
 * independent ADC loop.  The caller arms a burst when a commanded window opens
 * and collects it when the burst has run; nothing here blocks (SWR-CUR-14).
 *
 * The port hands over *raw ADC counts*, mid-rail biased exactly as SYS-HW-17's
 * conditioning delivers them.  Removing the bias, computing true RMS over whole
 * mains cycles and scaling to amps is `kiln_core/current`, which is where it can
 * be tested against a synthetic waveform (SWR-TST-13).  In particular the noise floor
 * must survive to the core: SWR-CUR-11 distinguishes an absent transformer from a
 * genuine zero by the absence of any signal *at all, including noise*, and an
 * adapter that helpfully squelched small values would destroy that distinction.
 */
#ifndef KILN_PORT_CURRENT_H
#define KILN_PORT_CURRENT_H

#include "kiln/err.h"
#include "kiln/types.h"

/* One burst must span a whole number of mains cycles (SWR-CUR-03).  The worst
 * case is the highest sample rate at the lowest mains frequency: 8 kHz / 50 Hz
 * is 160 samples per cycle, and two cycles give the averaging a comfortable
 * margin, so 512 bounds every configuration the hardware supports. */
constexpr size_t KILN_CUR_BURST_MAX     = 512;
constexpr uint32_t KILN_CUR_RATE_MIN_HZ = 1000u;  /* SWR-CUR-03 */

/* Which commanded interval a burst was taken in.  Not "what the current was" --
 * what the controller was *asking* for at the time, which is the whole content
 * of the measurement for SWR-SAF-25 and SWR-SAF-26. */
typedef enum {
    KILN_CUR_WINDOW_OFF = 0,    /* heating commanded off: leakage measurement */
    KILN_CUR_WINDOW_ON,         /* heating commanded on: conduction           */
} kiln_cur_window_t;

typedef struct {
    const uint16_t   *samples;        /* raw counts, biased about mid rail      */
    uint16_t          count;
    uint32_t          sample_rate_hz; /* achieved, not requested (SWR-CUR-03)    */
    uint64_t          t_start_us;
    kiln_cur_window_t window;         /* the phase this burst was armed for     */
    bool              truncated;      /* the window closed early: see SWR-CUR-05 */
} kiln_cur_burst_t;

typedef struct kiln_port_current {
    void *ctx;

    /* OQ-06 is open: one CT on a representative phase, or one per phase.  The
     * port is indexed so a second and third channel are additive rather than a
     * rewrite (architecture section 16). */
    uint8_t (*channel_count)(void *ctx);

    kiln_err_t (*configure)(void *ctx, uint8_t channel, uint32_t sample_rate_hz);

    /* Arm a capture of n_samples for the commanded window now opening.  Returns
     * KILN_ERR_BUSY if a burst is already in flight. */
    kiln_err_t (*start_burst)(void *ctx, uint8_t channel,
                              kiln_cur_window_t window, uint16_t n_samples);

    /* Collect it.  KILN_ERR_BUSY while the burst is still running, so the caller
     * polls from its own cycle and never waits (SWR-CUR-14). */
    kiln_err_t (*read_burst)(void *ctx, uint8_t channel, kiln_cur_burst_t *out);

    /* The commanded window closed before the burst completed: discard it rather
     * than mixing conduction and leakage samples in one RMS. */
    void (*abort_burst)(void *ctx, uint8_t channel);

    /* SWR-CUR-11: the front end's own view of whether a transformer is fitted --
     * the DC bias is within its plausible window (see tasklist A6).  The core
     * additionally checks for the absence of a noise floor, which catches a CT
     * that is plugged in but has no winding continuity. */
    bool (*present)(void *ctx, uint8_t channel);
} kiln_port_current_t;

#endif /* KILN_PORT_CURRENT_H */

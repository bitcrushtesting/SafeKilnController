/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Persisted switching-operation counters -- SWR-CUR-13, feeding the wear warning
 * of SWR-SAF-30.
 *
 * A separate port rather than a key in port_kvstore because the write pattern is
 * the problem these counters pose, not the storage: a 2 s window at 50 % duty
 * switches 1800 times an hour, and committing each one would burn through the
 * SWR-NFR-14 endurance budget by itself.  The adapter therefore accumulates in RAM
 * and flushes on a coarse boundary (run end, a count delta, shutdown), and the
 * port's shape says so -- `add` is cheap, `flush` is not.
 */
#ifndef KILN_PORT_COUNTERS_H
#define KILN_PORT_COUNTERS_H

#include "kiln/err.h"
#include "kiln/types.h"

typedef struct {
    uint32_t contactor_ops;
    uint32_t ssr_ops[KILN_HEAT_CHANNELS];   /* SYS-HW-12: per switched channel */
} kiln_switch_counters_t;

typedef struct kiln_port_counters {
    void *ctx;
    kiln_err_t (*load)(void *ctx, kiln_switch_counters_t *out);
    /* Accumulate in RAM.  Must not touch flash: called from the window tick. */
    void       (*add_contactor_ops)(void *ctx, uint32_t n);
    void       (*add_ssr_ops)(void *ctx, uint8_t channel, uint32_t n);
    /* Persist what has accumulated.  The only call here that may block. */
    kiln_err_t (*flush)(void *ctx);
    kiln_err_t (*reset)(void *ctx, const kiln_switch_counters_t *to);
} kiln_port_counters_t;

#endif /* KILN_PORT_COUNTERS_H */

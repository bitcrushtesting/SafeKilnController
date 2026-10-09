/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What the buzzer sounds like -- SWR-SAF-20, SWR-RUN-06, SYS-HW-09.
 *
 * The buzzer is a 5 V active part that makes its own tone, so the only thing
 * this device chooses is the rhythm.  SWR-SAF-20 asks for two annunciations
 * that an operator can tell apart without looking, and a rhythm is the whole of
 * how that is done.
 *
 *   KILN_ALARM_COMPLETE   three short chirps, then quiet for five seconds.
 *                         The firing is finished: that is good news about
 *                         something that took nine hours, and it wants a
 *                         notification rather than an alarm.  Brief by
 *                         construction -- the burst is over in half a second,
 *                         and the operator hears it once unless
 *                         `hmi.alarm_duration_s` is set long enough to let the
 *                         burst come round again.
 *
 *   KILN_ALARM_FAULT      six fast chirps, a short gap, and again, without
 *                         end.  Something has gone wrong with a kiln at
 *                         1 200 degC and the heat is already off; this one is
 *                         meant to be unpleasant, to carry through a wall, and
 *                         to keep going until somebody acknowledges it.  Twice
 *                         the chirp rate and ten times the duty of the
 *                         completion burst, so the two are not confusable even
 *                         through a door.
 *
 * ---------------------------------------------------------------------------
 * WHY THE RHYTHM IS IN THE CORE AND NOT IN THE ADAPTER
 * ---------------------------------------------------------------------------
 * It was in the adapter, inside an esp_timer callback, where no host test can
 * reach it: the one thing SWR-SAF-20 actually requires -- that the two are
 * audibly distinguishable -- was asserted by nobody, and "the fault pattern
 * continues until acknowledged" was a comment rather than a property.  Here it
 * is a table and a step function with no clock, no allocation and no hardware
 * (SWA-01, SWA-02), the adapter does nothing but set a pin and arm a timer for
 * the interval it is handed, and the rhythms are host-tested like everything
 * else that matters.
 */
#ifndef KILN_CORE_ALARMPTN_H
#define KILN_CORE_ALARMPTN_H

#include "kiln/types.h"
#include "kiln_ports/port_alarm.h"

/* Where a pattern has got to.  One step is one interval at one level. */
typedef struct {
    kiln_alarm_pattern_t pattern;
    uint8_t              step;
} kiln_alarm_seq_t;

/* The next interval: hold the buzzer at `on` for `hold_ms`, then call again.
 * `hold_ms == 0` means the pattern has finished and nothing further is due,
 * which for KILN_ALARM_OFF is immediate and for KILN_ALARM_FAULT never
 * happens. */
typedef struct {
    bool     on;
    uint32_t hold_ms;
} kiln_alarm_step_t;

/* Start a pattern from its first interval.  Re-starting the pattern that is
 * already running is the caller's decision to make, not this one's: see the
 * note in kiln_hal_alarm_init's adapter about why re-asserting a pattern must
 * not restart its beat. */
void kiln_alarm_seq_begin(kiln_alarm_seq_t *s, kiln_alarm_pattern_t pattern);

/* Advance by one interval.  Deterministic, total, and safe on a null sequence
 * or an out-of-range pattern, both of which answer "silent, nothing due" --
 * a buzzer that cannot be commanded must be quiet rather than stuck on. */
kiln_alarm_step_t kiln_alarm_seq_next(kiln_alarm_seq_t *s);

/* The total length of one cycle of a pattern, for the tests and for anything
 * sizing a timeout against it.  Zero for KILN_ALARM_OFF. */
uint32_t kiln_alarm_cycle_ms(kiln_alarm_pattern_t pattern);

/* How long a pattern sounds within one cycle, which is what makes two patterns
 * distinguishable at a distance rather than only on paper. */
uint32_t kiln_alarm_on_ms(kiln_alarm_pattern_t pattern);

#endif /* KILN_CORE_ALARMPTN_H */

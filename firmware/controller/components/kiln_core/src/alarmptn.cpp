/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The buzzer's two rhythms.  Why they are here and not in the adapter is in
 * alarmptn.h.
 */

#include "kiln_core/alarmptn.h"

namespace {

/* One interval of a pattern.  Written as a table rather than as a state
 * machine because the whole content of SWR-SAF-20 is these numbers, and a table
 * can be read against an oscilloscope by somebody who does not read C++. */
typedef struct {
    bool     on;
    uint16_t ms;
} interval_t;

/* Three chirps and five seconds of quiet.  The chirps are 90 ms, which is long
 * enough to be a note rather than a click and short enough that three of them
 * plus their gaps are over in half a second. */
const interval_t k_complete[] = {
    { true, 90 }, { false, 90 },
    { true, 90 }, { false, 90 },
    { true, 90 }, { false, 4550 },
};

/* Six chirps at twice that rate, a 400 ms gap, and round again.  60 ms on and
 * 60 ms off is about as insistent as an active buzzer gets before the bursts
 * run together into one tone and stop sounding urgent. */
const interval_t k_fault[] = {
    { true, 60 }, { false, 60 },
    { true, 60 }, { false, 60 },
    { true, 60 }, { false, 60 },
    { true, 60 }, { false, 60 },
    { true, 60 }, { false, 60 },
    { true, 60 }, { false, 400 },
};

const interval_t *table_for(kiln_alarm_pattern_t p, uint8_t *count)
{
    switch (p) {
    case KILN_ALARM_COMPLETE:
        *count = (uint8_t)(sizeof(k_complete) / sizeof(k_complete[0]));
        return k_complete;
    case KILN_ALARM_FAULT:
        *count = (uint8_t)(sizeof(k_fault) / sizeof(k_fault[0]));
        return k_fault;
    case KILN_ALARM_OFF:
    default:
        /* Including a pattern value that is not one of these: an enum carries
         * whatever was written through it, and silence is the answer that
         * cannot hurt. */
        *count = 0;
        return nullptr;
    }
}

} // namespace

void kiln_alarm_seq_begin(kiln_alarm_seq_t *s, kiln_alarm_pattern_t pattern)
{
    if (s == nullptr) {
        return;
    }
    s->pattern = pattern;
    s->step    = 0;
}

kiln_alarm_step_t kiln_alarm_seq_next(kiln_alarm_seq_t *s)
{
    kiln_alarm_step_t out = { false, 0u };
    if (s == nullptr) {
        return out;
    }

    uint8_t count = 0;
    const interval_t *tab = table_for(s->pattern, &count);
    if ((tab == nullptr) || (count == 0u)) {
        return out;
    }

    if (s->step >= count) {
        /* Past the end.  KILN_ALARM_FAULT cycles, because a fault that stopped
         * announcing itself after six seconds would be a fault nobody heard;
         * KILN_ALARM_COMPLETE also cycles, and is kept brief by the caller
         * stopping it -- the application turns the alarm off after
         * hmi.alarm_duration_s, whose default is one burst's worth, so the
         * normal case is that the operator hears it once.  Both are expressed
         * the same way here, and the difference is entirely the five seconds of
         * quiet in the completion table. */
        s->step = 0;
    }

    out.on      = tab[s->step].on;
    out.hold_ms = tab[s->step].ms;
    s->step     = (uint8_t)(s->step + 1u);
    return out;
}

uint32_t kiln_alarm_cycle_ms(kiln_alarm_pattern_t pattern)
{
    uint8_t count = 0;
    const interval_t *tab = table_for(pattern, &count);
    uint32_t total = 0;
    for (uint8_t i = 0; i < count; i++) {
        total += tab[i].ms;
    }
    return total;
}

uint32_t kiln_alarm_on_ms(kiln_alarm_pattern_t pattern)
{
    uint8_t count = 0;
    const interval_t *tab = table_for(pattern, &count);
    uint32_t total = 0;
    for (uint8_t i = 0; i < count; i++) {
        if (tab[i].on) {
            total += tab[i].ms;
        }
    }
    return total;
}

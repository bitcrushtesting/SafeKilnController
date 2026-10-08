/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "sup/trip.h"

namespace {

/* True only for a finite reading strictly above the backstop.  Written so a
 * NaN answers false here and is caught by the validity path instead: a
 * comparison against a NaN is false whichever way it is written, so the
 * absence of a usable reading must be its own trip and not a silent pass. */
bool over_temp(const sup_input_t *in)
{
    return in->chamber_valid && (in->chamber_c > SUP_OVERTEMP_C);
}

void latch(sup_t *s, sup_trip_reason_t why)
{
    if (!s->tripped) {
        s->tripped = true;
        s->reason  = why;
    }
}

}  // namespace

void sup_init(sup_t *s, bool selftest_ok)
{
    if (s == nullptr) {
        return;
    }
    const sup_t zero = {};
    *s = zero;
    s->selftest_ok = selftest_ok;
    if (!selftest_ok) {
        s->tripped = true;
        s->reason  = SUP_TRIP_SELF_TEST;
    }
}

void sup_step(sup_t *s, const sup_input_t *in, float dt_s)
{
    if ((s == nullptr) || (in == nullptr)) {
        return;
    }
    if (!(dt_s >= 0.0f)) {      /* also false for NaN */
        dt_s = 0.0f;
    }

    /* --- latching conditions -------------------------------------------- */

    /* The backstop does not wait and does not un-trip.  A backstop with a
     * grace period is a slower backstop, and one that clears itself is not a
     * backstop at all. */
    if (over_temp(in)) {
        latch(s, SUP_TRIP_OVERTEMP);
    }

    /* SWR-SAF-36: a failed diagnostic is the one condition that revokes the
     * self-test rather than merely tripping on it. Clearing selftest_ok is what
     * makes it unclearable, because sup_clear refuses to act without it, and
     * what keeps permit false for good: permit is conjunctive on selftest_ok.
     *
     * Ordered before the fault handling below so that a cycle which has both a
     * diagnostic failure and a thermocouple fault latches the diagnostic, which
     * is the more serious of the two and the one that explains the other. */
    if (!in->diag_ok) {
        s->selftest_ok = false;
        latch(s, SUP_TRIP_SELF_TEST);
    }

    /* A reported fault and an unusable reading are different causes with the
     * same consequence, and are reported separately so the ESP32 can say which
     * it was. */
    if (in->chamber_valid && (in->fault_bits == 0u)) {
        s->seen_valid = true;
    }

    const bool unusable = (in->fault_bits != 0u) || !in->chamber_valid;
    if (unusable) {
        s->fault_s += dt_s;
        /* A reported fault latches whether or not a reading ever arrived: the
         * front end is telling us something is wrong.  Staleness only latches
         * once a reading has been seen, so a front end that is still bringing
         * itself up withholds heat without demanding an acknowledgement. */
        if (s->fault_s >= SUP_FAULT_GRACE_S) {
            if (in->fault_bits != 0u) {
                latch(s, SUP_TRIP_TC_FAULT);
            } else if (s->seen_valid) {
                latch(s, SUP_TRIP_SENSOR_STALE);
            }
        }
    } else {
        s->fault_s = 0.0f;
    }

    /* --- the clear button ------------------------------------------------
     *
     * Edge triggered and held, for the reason given in sup_t: a line stuck low
     * must not be able to clear the latch, so it has to be seen released
     * before it counts, and clearing disarms it until it is released again. */
    if (!in->clear_pressed) {
        s->clear_armed = true;
        s->clear_s     = 0.0f;
    } else if (s->clear_armed) {
        s->clear_s += dt_s;
        if (s->clear_s >= SUP_CLEAR_HOLD_S) {
            sup_clear(s);
            s->clear_armed = false;
            s->clear_s     = 0.0f;
        }
    }

    /* --- permission ----------------------------------------------------- */

    /* Conjunctive, and evaluated fresh every cycle: every term must be true
     * for heat to be allowed, so a term that cannot be evaluated withholds it.
     * This is what makes start-up safe without a special case, since
     * chamber_valid is false until a conversion has been seen. */
    s->permit = s->selftest_ok
             && !s->tripped
             && in->chamber_valid
             && (in->fault_bits == 0u)
             && (in->chamber_c <= SUP_OVERTEMP_C);
}

void sup_clear(sup_t *s)
{
    if ((s == nullptr) || !s->selftest_ok) {
        return;     /* a failed self-test is not clearable by an operator */
    }
    s->tripped = false;
    s->reason  = SUP_TRIP_NONE;
    s->fault_s = 0.0f;
    /* permit stays false until the next sup_step re-establishes every term.
     * The arming state is deliberately not touched: sup_step owns it, and a
     * caller clearing the latch directly must not re-arm the button. */
    s->permit  = false;
}

uint8_t sup_flags(const sup_t *s, const sup_input_t *in)
{
    if ((s == nullptr) || (in == nullptr)) {
        return 0;
    }
    uint32_t f = 0;
    if (s->permit)        { f |= SUP_FLAG_PERMIT; }
    if (s->tripped)       { f |= SUP_FLAG_TRIPPED; }
    if (in->chamber_valid){ f |= SUP_FLAG_TC_VALID; }
    if (s->selftest_ok)   { f |= SUP_FLAG_SELFTEST_OK; }
    return (uint8_t)f;
}

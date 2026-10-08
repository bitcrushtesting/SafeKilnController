/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "sup/trip.h"

namespace {

/* SWR-SAF-37: the temperature the backstop acts on, which is the HIGHER of the
 * two valid readings.
 *
 * The higher, not the average and not the first. A couple reading low is the
 * dangerous failure: it is what lets a hot kiln look cool, and HZ-03 is about
 * exactly that. Taking the maximum means one low-reading couple cannot mask a
 * real over-temperature, which is the single biggest thing the second sensor
 * buys and the reason it is worth a second SPI bus. An average would let a
 * couple reading 200 degC low pull the pair 100 degC low, which is the failure
 * dressed up as redundancy. */
bool any_valid(const sup_input_t *in)
{
    return in->chamber_valid || in->chamber2_valid;
}

float effective_c(const sup_input_t *in)
{
    if (in->chamber_valid && in->chamber2_valid) {
        /* Written so a NaN in either cannot win: the comparison is false for a
         * NaN whichever way round it is, so the other reading is taken. */
        return (in->chamber2_c > in->chamber_c) ? in->chamber2_c : in->chamber_c;
    }
    if (in->chamber2_valid) {
        return in->chamber2_c;
    }
    return in->chamber_c;
}

/* True only for a finite reading strictly above the backstop.  Written so a
 * NaN answers false here and is caught by the validity path instead: a
 * comparison against a NaN is false whichever way it is written, so the
 * absence of a usable reading must be its own trip and not a silent pass. */
bool over_temp(const sup_input_t *in)
{
    return any_valid(in) && (effective_c(in) > SUP_OVERTEMP_C);
}

/* True when both couples are usable and differ by more than the band. False
 * when either is unusable: there is nothing to compare, and the unusable one
 * trips on its own account. */
bool disagreeing(const sup_input_t *in)
{
    if (!in->chamber_valid || !in->chamber2_valid) {
        return false;
    }
    const float d = in->chamber_c - in->chamber2_c;
    const float mag = (d < 0.0f) ? -d : d;
    /* `>` and not `>=`, and written so a NaN answers false: a NaN reading is a
     * validity problem, not a disagreement, and must not be reported as one. */
    return mag > SUP_DISAGREE_C;
}

void latch(sup_t *s, sup_trip_reason_t why)
{
    if (!s->tripped) {
        s->tripped = true;
        s->reason  = why;
    }
}

/* SWR-SAF-38: latch, and take the reason even from an existing trip.
 *
 * Normally the FIRST cause is kept, because it is the one that explains the
 * rest. A coil that stayed energised after the permit was withdrawn is the
 * exception, and the exception is worth stating: it means the supervisor cannot
 * interrupt the heater at all, which supersedes whatever it was trying to
 * interrupt it for. An operator shown "over-temperature" would press the clear
 * button; one shown "permit stuck" is told to isolate at the supply, which is
 * the only thing that helps. It is the same reasoning SWR-SAF-27 uses to make a
 * welded contactor the more severe of its pair.
 *
 * SUP_TRIP_SELF_TEST is NOT overridden, and that is deliberate too: a supervisor
 * that has failed its own diagnostics cannot be trusted to have correctly
 * concluded that the coil is stuck. The diagnostic failure is the thing to
 * report, because it is the thing that makes every other conclusion doubtful. */
void latch_escalate(sup_t *s, sup_trip_reason_t why)
{
    if (s->reason == SUP_TRIP_SELF_TEST) {
        s->tripped = true;
        return;
    }
    s->tripped = true;
    s->reason  = why;
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
    if (any_valid(in) && (in->fault_bits == 0u)) {
        s->seen_valid = true;
    }

    const bool unusable = (in->fault_bits != 0u) || !any_valid(in);
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

    /* --- SWR-SAF-37: the two couples disagree ---------------------------
     *
     * Confirmed over a window, like every other rule whose trip would otherwise
     * be one bad conversion away. Heat is withheld from the first cycle either
     * way, because permit is conjunctive below; the window delays the latch. */
    if (disagreeing(in)) {
        s->disagree_s += dt_s;
        if (s->disagree_s >= SUP_DISAGREE_S) {
            latch(s, SUP_TRIP_TC_DISAGREE);
        }
    } else {
        s->disagree_s = 0.0f;
    }

    /* --- SWR-SAF-38: the permit was withdrawn and the coil stayed on ------
     *
     * Only this direction is a fault. A permit that is commanded and NOT sensed
     * means the path is open somewhere else, the lid switch or a front end's
     * fault transistor, and that is heat not being delivered rather than heat
     * that cannot be stopped. The ESP32's own rules notice a kiln that will not
     * heat; nothing needs the supervisor to trip on it.
     *
     * The settle delay is measured from the permit being low, not from the
     * command changing, which is simpler and strictly more conservative: a coil
     * that is slow to drop gets the whole window, and one that never drops
     * latches. */
    if (!s->permit && in->permit_sense) {
        s->permit_stuck_s += dt_s;
        if (s->permit_stuck_s >= SUP_PERMIT_SETTLE_S) {
            latch_escalate(s, SUP_TRIP_PERMIT_STUCK);
        }
    } else {
        s->permit_stuck_s = 0.0f;
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
             && any_valid(in)
             && !disagreeing(in)
             && (in->fault_bits == 0u)
             && (effective_c(in) <= SUP_OVERTEMP_C);
}

void sup_clear(sup_t *s)
{
    if ((s == nullptr) || !s->selftest_ok) {
        return;     /* a failed self-test is not clearable by an operator */
    }
    s->tripped = false;
    s->reason  = SUP_TRIP_NONE;
    s->fault_s = 0.0f;
    s->disagree_s     = 0.0f;
    s->permit_stuck_s = 0.0f;
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
    if (in->chamber2_valid){ f |= SUP_FLAG_TC2_VALID; }
    if (s->selftest_ok)   { f |= SUP_FLAG_SELFTEST_OK; }
    return (uint8_t)f;
}

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

int32_t effective_q7(const sup_input_t *in)
{
    if (in->chamber_valid && in->chamber2_valid) {
        return (in->chamber2_q7 > in->chamber_q7) ? in->chamber2_q7 : in->chamber_q7;
    }
    if (in->chamber2_valid) {
        return in->chamber2_q7;
    }
    return in->chamber_q7;
}

/* Milliseconds, saturating.  A latch timer that wrapped would walk back below
 * its threshold and un-arm a condition that is still present, which is the one
 * arithmetic failure in this file that would cost protection rather than cause
 * a nuisance trip.  Saturation cannot: once a timer is at the top it stays
 * there, and every threshold here is far below it. */
uint32_t add_ms(uint32_t acc, uint32_t dt)
{
    const uint32_t sum = acc + dt;
    return (sum < acc) ? UINT32_MAX : sum;
}

/* True only for a usable reading strictly above the backstop.
 *
 * The validity term is still first and still load-bearing: the absence of a
 * reading must be its own trip, not a silent pass, and chamber_q7 is zero when
 * nothing has been read, which would otherwise look like a comfortable 0 degC.
 * What has gone is the reason the float version gave for the ordering, that a
 * comparison against a NaN is false whichever way it is written.  An int32_t
 * has no NaN, so the comparison is total and the ordering is now about the
 * zero-initialised snapshot alone. */
bool over_temp(const sup_input_t *in)
{
    return any_valid(in) && (effective_q7(in) > SUP_OVERTEMP);
}

/* True when both couples are usable and differ by more than the band. False
 * when either is unusable: there is nothing to compare, and the unusable one
 * trips on its own account. */
bool disagreeing(const sup_input_t *in)
{
    if (!in->chamber_valid || !in->chamber2_valid) {
        return false;
    }
    /* The magnitude of the difference, computed so that it is defined for
     * every pair of int32_t and not only for the 19-bit values the decode can
     * produce.
     *
     * Subtracting signed and negating the result would be the obvious way and
     * is wrong twice over: the subtraction overflows for a widely separated
     * pair, and negating INT32_MIN is undefined on its own.  Both are
     * unreachable through sup_tc_decode, which is exactly what makes them the
     * kind of assumption that survives until the day something else fills this
     * struct.  Taken high minus low in uint32_t the result is exact for any
     * pair, because a difference of two int32_t always fits in a uint32_t, and
     * unsigned arithmetic has no overflow to be undefined about. */
    const int32_t  a  = in->chamber_q7;
    const int32_t  b  = in->chamber2_q7;
    const int32_t  hi = (a > b) ? a : b;
    const int32_t  lo = (a > b) ? b : a;
    const uint32_t mag = (uint32_t)hi - (uint32_t)lo;
    /* `>` and not `>=`: the band is the largest difference still accepted. */
    return mag > (uint32_t)SUP_DISAGREE;
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

void sup_step(sup_t *s, const sup_input_t *in, uint32_t dt_ms)
{
    if ((s == nullptr) || (in == nullptr)) {
        return;
    }
    /* No clamp on dt_ms, and that is not an omission.  The float version
     * rejected a negative or NaN interval here; an unsigned integer can be
     * neither, and add_ms saturates, so there is no value of dt_ms this
     * function has to defend against. */

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
        s->fault_ms = add_ms(s->fault_ms, dt_ms);
        /* A reported fault latches whether or not a reading ever arrived: the
         * front end is telling us something is wrong.  Staleness only latches
         * once a reading has been seen, so a front end that is still bringing
         * itself up withholds heat without demanding an acknowledgement. */
        if (s->fault_ms >= SUP_FAULT_GRACE_MS) {
            if (in->fault_bits != 0u) {
                latch(s, SUP_TRIP_TC_FAULT);
            } else if (s->seen_valid) {
                latch(s, SUP_TRIP_SENSOR_STALE);
            }
        }
    } else {
        s->fault_ms = 0u;
    }

    /* --- SWR-SAF-37: the two couples disagree ---------------------------
     *
     * Confirmed over a window, like every other rule whose trip would otherwise
     * be one bad conversion away. Heat is withheld from the first cycle either
     * way, because permit is conjunctive below; the window delays the latch. */
    if (disagreeing(in)) {
        s->disagree_ms = add_ms(s->disagree_ms, dt_ms);
        if (s->disagree_ms >= SUP_DISAGREE_MS) {
            latch(s, SUP_TRIP_TC_DISAGREE);
        }
    } else {
        s->disagree_ms = 0u;
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
        s->permit_stuck_ms = add_ms(s->permit_stuck_ms, dt_ms);
        if (s->permit_stuck_ms >= SUP_PERMIT_SETTLE_MS) {
            latch_escalate(s, SUP_TRIP_PERMIT_STUCK);
        }
    } else {
        s->permit_stuck_ms = 0u;
    }

    /* --- the clear button ------------------------------------------------
     *
     * Edge triggered and held, for the reason given in sup_t: a line stuck low
     * must not be able to clear the latch, so it has to be seen released
     * before it counts, and clearing disarms it until it is released again. */
    if (!in->clear_pressed) {
        s->clear_armed = true;
        s->clear_ms    = 0u;
    } else if (s->clear_armed) {
        s->clear_ms = add_ms(s->clear_ms, dt_ms);
        if (s->clear_ms >= SUP_CLEAR_HOLD_MS) {
            sup_clear(s);
            s->clear_armed = false;
            s->clear_ms    = 0u;
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
             && (effective_q7(in) <= SUP_OVERTEMP);
}

void sup_clear(sup_t *s)
{
    if ((s == nullptr) || !s->selftest_ok) {
        return;     /* a failed self-test is not clearable by an operator */
    }
    s->tripped = false;
    s->reason  = SUP_TRIP_NONE;
    s->fault_ms        = 0u;
    s->disagree_ms     = 0u;
    s->permit_stuck_ms = 0u;
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

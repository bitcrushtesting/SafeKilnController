/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file trip.h
 * @brief The supervisor's trip logic: the whole safety function.
 *
 * @derivedfrom SWA-22, the independent safety supervisor.
 * @safetyclass EN IEC 60730-1 Annex H software class B or C, the control
 *              function under assessment. EN ISO 13849-1:2023 safety-related
 *              part; no performance level is claimed (`docs/safety.md`).
 *
 * The supervisor's trip logic (SWA-22, docs/safety-supervisor.md section 3).
 *
 * This is the whole safety function, and it is deliberately the only thing in
 * this component.  It is a pure function of an input snapshot plus its own
 * timers, with time injected, no globals and no platform headers, for the same
 * reason SWA-01 to SWA-03 say so on the other side of the link: a backstop whose
 * behaviour cannot be exercised on a development host is a backstop nobody has
 * tested.
 *
 * It shares no code with the ESP32's safety rules, which is the point of
 * SWA-22.  Any resemblance between the two is a resemblance, not reuse.
 *
 * Every threshold here is hard-coded.  There is no configuration, no
 * persistence and no receive path, so there is nothing to get into a state
 * that was not compiled in.
 */
#ifndef SUP_TRIP_H
#define SUP_TRIP_H

#include <stdbool.h>
#include <stdint.h>

#include "sup_proto.h"

/* The lid is NOT here, deliberately.  Its switch breaks the contactor coil in
 * hardware (SYS-HW-21), so it is already safe without any firmware involvement,
 * and SWR-SAF-31's latch is gated on "while a heating state is active", which only
 * the ESP32 knows.  A supervisor that latched on lid open regardless would
 * trip every time the kiln was loaded cold: a nuisance trip, which HZ-10 names
 * as how protections come to be disabled.  The lid sense goes to the ESP32,
 * which has both the context and the run state.
 *
 * --- the hard-coded thresholds ------------------------------------------
 *
 * Every temperature here is q7, 1/128 degC, and every interval is whole
 * milliseconds.  There is no floating point anywhere in this component, and
 * that is a property to preserve rather than an accident of how it was
 * written: this processor has no FPU, so a float comparison is a call into a
 * libgcc helper, and the backstop should not depend on a library the project
 * does not build or test.  docs/coding-standard.md section 5 has the argument.
 *
 * Thresholds are written as whole degrees through sup_c_to_q7 so they stay
 * readable as the numbers the safety concept states, and the multiply is the
 * compiler's.
 *
 * SUP_OVERTEMP must agree with KILN_SUPERVISOR_TRIP_C on the ESP32 side, and
 * must stay above that side's KILN_TEMP_CEILING_C of 1300 degC.  The 50 degC
 * between them is the margin (SWR-SAF-23); see safety-supervisor.md section 4 for
 * why the ceiling moved down rather than this moving up. */
/**
 * @brief Absolute over-temperature backstop: 1350 degC, in q7.
 *
 * @rangeof 1350 degC expressed as 1/128 degC counts. Compile-time constant;
 *          there is no configuration item for it and no way to raise it at
 *          run time, which is the property the safety and security arguments
 *          both rest on.
 *
 * @rationale
 * It must agree with `KILN_SUPERVISOR_TRIP_C` on the ESP32 and must stay above
 * that side's 1300 degC ceiling. The 50 degC between them is the margin: the
 * controller stops first and the supervisor is the backstop, so the two never
 * race to trip on the same reading. `docs/safety-supervisor.md` section 4 has
 * why the ceiling moved down rather than this moving up.
 *
 * @implements SWR-SAF-23
 * @verifiedby `test_trip.cpp`, at the threshold and one LSB either side.
 */
constexpr int32_t SUP_OVERTEMP = sup_c_to_q7(1350);

/* A reported front-end fault, or an unusable reading, must persist before it
 * latches: a single bad conversion on a noisy bus is not a dead sensor.  Heat
 * is withheld immediately either way, so the grace delays the *latch*, not the
 * protection. */
/**
 * @brief How long a front-end fault must persist before the trip latches: 1 s.
 *
 * @rangeof Whole milliseconds. Compile-time constant.
 *
 * @errorbehaviour
 * Heat is withheld on the **first** cycle that reports a fault. This interval
 * delays only the latch, so a single bad conversion on a noisy bus costs one
 * cycle of heating rather than an operator visit.
 *
 * @implements SWR-SAF-22
 * @verifiedby `test_trip.cpp`: a fault that clears inside the grace does not
 *             latch; one that persists does.
 */
constexpr uint32_t SUP_FAULT_GRACE_MS = 1000u;

/* How long the clear button must be held.  Long enough to be deliberate, short
 * enough not to be a puzzle.  See the note on edge triggering below. */
/**
 * @brief How long the clear button must be held before a latch is cleared: 500 ms.
 *
 * @rangeof Whole milliseconds. Compile-time constant.
 *
 * @rationale
 * A deliberate act rather than a contact bounce. The button acknowledges a
 * condition the operator has seen and dealt with, so the gesture should be one
 * nobody performs by brushing past the panel.
 *
 * @implements SWR-SAF-18
 * @verifiedby `test_trip.cpp`: a short press does not clear; a held one does.
 */
constexpr uint32_t SUP_CLEAR_HOLD_MS = 500u;

/* SWR-SAF-37: how far the two chamber couples may disagree, and for how long.
 *
 * 50 degC is wide, and deliberately so. Two type-K couples are each accurate to
 * a few degrees at kiln temperatures, but a kiln chamber is not isothermal: two
 * probes at different positions genuinely differ, by tens of degrees during a
 * ramp. A band tight enough to catch a small drift would stop healthy firings,
 * and HZ-10 is about what that leads to.
 *
 * The consequence has to be stated rather than left implicit: THIS CHECK CATCHES
 * A GROSSLY WRONG COUPLE, NOT A DRIFTING ONE. It covers the failure HZ-03 is
 * about, a couple reading plausibly but far from the truth, and it does not cover
 * a couple reading five degrees low.
 *
 * It also places a requirement on the installation that the band's width cannot
 * substitute for: the two couples must sit in the same thermowell, or close
 * enough that the comparison is about the sensors and not about the kiln. Two
 * probes at opposite ends of a chamber would need a band so wide as to detect
 * nothing. SWR-SAF-37 says so, and the commissioning documentation has to. */
/**
 * @brief How far two chamber couples may differ before it is a fault: 50 degC, in q7.
 *
 * @rangeof 50 degC expressed as 1/128 degC counts. Compile-time constant.
 *
 * @rationale
 * Wide on purpose, and the limitation has to be stated rather than implied: it
 * catches a **grossly** wrong couple, which is the hazard, and not one reading
 * five degrees low. A band tight enough for that would stop healthy firings,
 * and a nuisance trip is how a protection comes to be disabled.
 *
 * @implements SWR-SAF-37
 * @verifiedby `test_trip.cpp`: inside the band both couples are used; outside
 *             it the higher is acted on and the disagreement latches.
 */
constexpr int32_t  SUP_DISAGREE    = sup_c_to_q7(50);
/**
 * @brief How long two couples must disagree before the trip latches: 10 s.
 *
 * @rangeof Whole milliseconds. Compile-time constant.
 *
 * @errorbehaviour
 * Protection does not wait for this: the **higher** reading is acted on from
 * the first cycle of disagreement. The interval delays the latch only.
 *
 * @implements SWR-SAF-37
 * @verifiedby `test_trip.cpp`, at the boundary of the interval.
 */
constexpr uint32_t SUP_DISAGREE_MS = 10000u;

/* SWR-SAF-38: how long after withdrawing the permit the coil may still read as
 * energised before it counts as stuck.
 *
 * Longer than the contactor's drop-out plus the readback divider's settling, and
 * shorter than anything that matters. 2 s matches the interval SWR-SAF-27 already
 * uses for the ESP32's weld discrimination, for the same physical reason. */
/**
 * @brief How long the coil may read energised after the permit is withdrawn: 2 s.
 *
 * @rangeof Whole milliseconds. Compile-time constant.
 *
 * @rationale
 * The contactor is a mechanical device behind a charge pump that decays rather
 * than switches, so "energised" remains true for a while after the permit goes
 * away. This is the settling time that distinguishes that from a coil that is
 * stuck, which is a welded contactor and a different fault entirely.
 *
 * @implements SWR-SAF-22
 * @verifiedby `test_trip.cpp`: a readback that clears inside the interval is
 *             not a fault; one that persists latches.
 */
constexpr uint32_t SUP_PERMIT_SETTLE_MS = 2000u;

/**
 * @brief One cycle's view of the world, as handed to @ref sup_step.
 *
 * @rangeof Temperatures are q7 (1/128 degC). Every `bool` is **positive
 *          logic**, so a zero-filled snapshot means "nothing proven" and
 *          withholds heat. That is the convention the whole structure follows
 *          and the reason it is safe to forget a field.
 *
 * @errorbehaviour
 * The one deliberate exception to the convention is @ref permit_sense, and it
 * is documented on the field itself.
 *
 * @implements SWR-SAF-22, SWR-SAF-36, SWR-SAF-37, SWR-SAF-38
 */
typedef struct {
    int32_t  chamber_q7;    /**< Couple 1, linearised, 1/128 degC. */
    bool     chamber_valid; /**< A conversion completed and was in range. */

    /**
     * @brief Couple 2, on its own SPI bus and its own front end (SWR-SAF-37).
     *
     * Zero-initialised to invalid, so a caller that does not fit a second
     * couple gets single-channel behaviour rather than a false agreement
     * between a reading and a zero.
     */
    int32_t  chamber2_q7;
    bool     chamber2_valid; /**< Couple 2 produced a usable reading. */

    uint16_t fault_bits;    /**< `SUP_TC_FAULT_*` set, 0 for none. */
    bool     clear_pressed; /**< The local clear button, debounced by the cycle. */

    /**
     * @brief The coil drive node, sensed. True means energised (SWR-SAF-38).
     *
     * Positive logic again, but note what the safe default is here: a
     * zero-initialised input reads "not energised", which is the **benign**
     * value, because the dangerous direction is a coil that stays on after the
     * permit is withdrawn. A board with no readback fitted therefore reports
     * no fault rather than a permanent one, and SWR-SAF-38 is explicit that
     * the diagnostic is **absent** on such a board rather than passing.
     */
    bool     permit_sense;

    /**
     * @brief Every self-diagnostic passed this cycle (SWR-SAF-36).
     *
     * Positive logic, so that a zero-initialised input means "not proven
     * healthy" and withholds heat. Same convention as @ref chamber_valid and
     * for the same reason: the safe state has to be the one you get by
     * forgetting to set a field.
     *
     * A false here is heavier than a thermocouple fault. It clears
     * @ref sup_t::selftest_ok, which makes the trip unclearable by the button,
     * because a supervisor whose RAM, stack or program sequence has failed
     * cannot be trusted to evaluate the condition the operator would be
     * acknowledging.
     */
    bool     diag_ok;
} sup_input_t;

/**
 * @brief The trip logic's own state: the decision, and the timers behind it.
 *
 * @rangeof All intervals are whole milliseconds and saturate rather than wrap.
 *
 * @rationale
 * Held in a structure the caller owns rather than in globals, which is what
 * lets a host test construct any situation it likes and step it without a
 * kiln, a clock or a board.
 *
 * @implements SWR-SAF-18, SWR-SAF-22, SWR-SAF-36, SWR-SAF-37
 */
typedef struct {
    bool              permit;      /**< Heat is allowed *now*. */
    bool              tripped;     /**< Latched; needs a local clear. */
    sup_trip_reason_t reason;      /**< Why, latched with the trip. */
    bool              selftest_ok; /**< The start-up self-test has not been revoked. */
    uint32_t          fault_ms;    /**< How long the front-end fault has persisted. */

    /**
     * @brief Whether a usable reading has *ever* arrived.
     *
     * Before the first one the supervisor withholds permission but does not
     * latch: "the sensor never got going" and "the sensor was working and
     * stopped" are different, and only the second is a fault to acknowledge.
     * Without this the stale timer runs from the first cycle and latches at
     * boot whenever the front end's first conversion takes longer than the
     * grace, which is a trip with nothing wrong behind it.
     */
    bool              seen_valid;

    /**
     * @brief Whether the clear button has been seen released since it last acted.
     *
     * The button is edge triggered, and this is why. A latch cleared on the
     * pin **level** is not a latch: a button shorted to ground, or one stuck
     * down, would clear it on every cycle, and the supervisor would then
     * permit heat whenever the instantaneous condition happened to be good.
     * That is the latch defeated by a single solder bridge.
     *
     * So the input has to be seen released before it can clear anything, and
     * then held for @ref SUP_CLEAR_HOLD_MS. A shorted line never arms, because
     * it is never seen released, including at power-on.
     */
    bool              clear_armed;
    uint32_t          clear_ms;    /**< How long the armed button has been held. */

    /**
     * @brief How long the two couples have disagreed.
     *
     * Delays the **latch**, not the protection: the higher reading is acted on
     * from the first cycle either way.
     */
    uint32_t          disagree_ms;

    /**
     * @brief How long the coil has read energised while the permit was withdrawn.
     *
     * Delays the latch for the same reason as @ref disagree_ms, and gives the
     * charge pump time to decay before a slow release is called a stuck coil.
     */
    uint32_t          permit_stuck_ms;
} sup_t;

/**
 * @brief Initialise the trip state so that it comes up refusing heat.
 *
 * @param[out] s            Trip state to initialise. Must not be NULL.
 * @param[in]  selftest_ok  Result of the start-up self-test: the front ends
 *                          configured, a first conversion seen, and the
 *                          diagnostics of @ref selfcheck.h passed.
 *
 * @rangeof selftest_ok: `true` only when every start-up check passed. A caller
 *        that cannot determine this passes `false`.
 *
 * @errorbehaviour
 * The permit is **conjunctive**: it requires the self-test, a valid chamber
 * reading and no latched trip, all at once. After this call none of those
 * hold, so heat is refused until each is positively established. Absence of
 * evidence is not permission.
 *
 * @rationale
 * A state structure that is zero-initialised and then "configured" has a
 * window in which it is neither. Starting from refusal removes the window: the
 * dangerous direction requires work and the safe direction is the default,
 * which is the same convention `diag_ok` uses on the input snapshot.
 *
 * @sideeffects
 * None beyond `*s`. No allocation, no I/O, no globals.
 *
 * @reentrancy
 * Not reentrant with respect to the same `s`; the supervisor is single
 * threaded and calls it once, before the cycle starts.
 *
 * @implements SWR-SAF-22, SWR-SAF-23
 * @verifiedby `test_trip.cpp`, the start-up cases: a supervisor that has not
 *             seen a conversion does not permit heat even with a plausible
 *             temperature in hand.
 */
void sup_init(sup_t *s, bool selftest_ok);

/**
 * @brief Advance the trip logic by one cycle and decide whether heat is permitted.
 *
 * This is the unit the whole component exists for. It is a pure function of
 * `*in`, `dt_ms` and the timers inside `*s`: no globals, no platform headers,
 * no clock of its own.
 *
 * @param[in,out] s      Trip state, previously initialised by @ref sup_init.
 * @param[in]     in     The cycle's input snapshot: both chamber readings and
 *                       their validity, the front-end fault lines, the clear
 *                       button, the coil readback and `diag_ok`.
 * @param[in]     dt_ms  Elapsed time since the previous call.
 *
 * @rangeof dt_ms: whole milliseconds, 0 to UINT32_MAX. The nominal value is the
 *        100 ms cycle; any value is accepted and the latch timers saturate
 *        rather than wrap.
 * @rangeof in: every temperature is q7 (1/128 degC); every interval is whole
 *        milliseconds. `chamber_valid` and `diag_ok` are positive logic, so a
 *        zero-filled snapshot withholds heat.
 *
 * @statemachine
 * Permitting is conjunctive and latching is one-way within a power cycle:
 *
 *     PERMIT  --- any trip condition --->  LATCHED (heat refused)
 *     LATCHED --- clear armed and held, condition no longer true ---> PERMIT
 *
 * A latched trip is not cleared by the condition going away on its own, and
 * never by anything arriving over the link, because there is no receiver.
 *
 * @errorbehaviour
 * Every input is treated as untrustworthy until it says otherwise:
 * - a reading that is not valid is not a reading, and heat is refused;
 * - a reported front-end fault refuses heat on the first cycle and latches
 *   only after @ref SUP_FAULT_GRACE_MS, so one bad conversion on a noisy bus
 *   is not a dead sensor;
 * - two couples that disagree by more than @ref SUP_DISAGREE act on the
 *   **higher** reading immediately, and latch after @ref SUP_DISAGREE_MS;
 * - a coil that reads energised while the permit is withdrawn latches after
 *   @ref SUP_PERMIT_SETTLE_MS;
 * - a false `diag_ok` revokes the self-test, which is unclearable by the
 *   button (SWR-SAF-36).
 *
 * @rationale
 * `dt_ms` is unsigned, and that removes a guard rather than hiding one. The
 * earlier floating-point version had to begin by rejecting a negative or NaN
 * interval, because either would have run the latch timers backwards or
 * stalled them for ever. Neither value exists in a `uint32_t`: the condition
 * is unrepresentable instead of checked, which is a stronger property than a
 * check that somebody could delete.
 *
 * Acting on the higher of two valid readings is the half that does the safety
 * work. A couple reading low is the dangerous failure, because it lets a hot
 * kiln look cool, and an average would let one couple reading 200 degC low
 * pull the pair 100 degC low: the failure dressed as redundancy.
 *
 * @resources
 * No allocation and no recursion. Bounded in time: a fixed sequence of
 * comparisons with no loop over input.
 *
 * @reentrancy
 * Not reentrant with respect to the same `s`. Called from one place, once per
 * cycle, from the supervisor's only thread of control.
 *
 * @implements SWR-SAF-22, SWR-SAF-23, SWR-SAF-36, SWR-SAF-37
 * @verifiedby `test_trip.cpp`, which exercises each rule at its boundary in
 *             LSBs either side of the threshold, and reaches 100 % MC/DC over
 *             this unit (`tools/mcdc.sh`, floor 80 %).
 */
void sup_step(sup_t *s, const sup_input_t *in, uint32_t dt_ms);

/**
 * @brief Clear a latched trip.
 *
 * @param[in,out] s Trip state. Must not be NULL.
 *
 * @errorbehaviour
 * Clearing does not make the kiln safe; it makes the supervisor willing to
 * look again. The permit remains conjunctive, so if the condition that caused
 * the trip is still true the next @ref sup_step latches it again on the same
 * cycle. A revoked self-test (SWR-SAF-36) is **not** cleared by this: a
 * supervisor whose memory, stack, clock, image or program sequence has failed
 * is not in the category an operator can acknowledge.
 *
 * @rationale
 * @ref sup_step calls this itself once the clear button has been armed and
 * held, so the arming behaviour lives in the tested core rather than in the
 * board layer where nothing exercises it. It stays public because a test
 * should be able to drive the transition directly rather than through a
 * simulated button.
 *
 * There is no path to this from the serial link: the supervisor has no
 * receiver, which is why its independence is a property of the wiring and not
 * of a check in software.
 *
 * @sideeffects
 * Resets the latch and the timers behind it. Does not touch the self-test flag.
 *
 * @implements SWR-SAF-18, SWR-SAF-36
 * @verifiedby `test_trip.cpp`: a live condition relatches on the next cycle,
 *             and a revoked self-test survives a clear.
 */
void sup_clear(sup_t *s);

/**
 * @brief Build the status flags byte that goes on the wire.
 *
 * @param[in] s  Trip state.
 * @param[in] in The same input snapshot that was passed to @ref sup_step.
 * @return The flags byte of the frame described in @ref sup_proto.h.
 * @retval 0 No flag set: nothing valid, nothing permitted, nothing latched.
 *
 * @rangeof return value: each bit is one `SUP_FLAG_*` of @ref sup_proto.h.
 *
 * @rationale
 * Derived on demand rather than tracked in a field that is updated alongside
 * the state. A separate copy of the truth is a copy that can disagree with it,
 * and this one would disagree exactly when the frame mattered most: after a
 * trip.
 *
 * @sideeffects
 * None. Pure function of its arguments.
 *
 * @implements SWR-SAF-38
 * @verifiedby `test_proto.cpp` and `test_trip.cpp`: the flags a given state
 *             produces, including after a latch.
 */
uint8_t sup_flags(const sup_t *s, const sup_input_t *in);

#endif /* SUP_TRIP_H */

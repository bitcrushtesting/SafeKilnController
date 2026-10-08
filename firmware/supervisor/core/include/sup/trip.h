/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
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
 * SUP_OVERTEMP_C must agree with KILN_SUPERVISOR_TRIP_C on the ESP32 side, and
 * must stay above that side's KILN_TEMP_CEILING_C of 1300 degC.  The 50 degC
 * between them is the margin (SWR-SAF-23); see safety-supervisor.md section 4 for
 * why the ceiling moved down rather than this moving up. */
#define SUP_OVERTEMP_C      1350.0f

/* A reported front-end fault, or an unusable reading, must persist before it
 * latches: a single bad conversion on a noisy bus is not a dead sensor.  Heat
 * is withheld immediately either way, so the grace delays the *latch*, not the
 * protection. */
#define SUP_FAULT_GRACE_S   1.0f

/* How long the clear button must be held.  Long enough to be deliberate, short
 * enough not to be a puzzle.  See the note on edge triggering below. */
#define SUP_CLEAR_HOLD_S    0.5f

typedef struct {
    float    chamber_c;     /* linearised, degC                              */
    bool     chamber_valid; /* a conversion completed and was in range        */
    uint16_t fault_bits;    /* KILN_TC_FAULT_*, 0 for none                    */
    bool     clear_pressed; /* the local clear button, debounced by the cycle */
    /* SWR-SAF-36: every self-diagnostic passed this cycle.
     *
     * POSITIVE logic, so that a zero-initialised input means "not proven
     * healthy" and withholds heat. That is the same convention chamber_valid
     * uses and for the same reason: the safe state has to be the one you get
     * by forgetting to set a field.
     *
     * A false here is heavier than a thermocouple fault. It clears selftest_ok,
     * which makes the trip unclearable by the button, because a supervisor
     * whose RAM, stack or program sequence has failed cannot be trusted to
     * evaluate the condition the operator would be acknowledging. */
    bool     diag_ok;
} sup_input_t;

typedef struct {
    bool              permit;      /* heat is allowed *now*                   */
    bool              tripped;     /* latched, needs a local clear            */
    sup_trip_reason_t reason;      /* why, latched with the trip              */
    bool              selftest_ok;
    float             fault_s;     /* how long the fault has persisted        */
    /* Whether a usable reading has *ever* arrived.  Before the first one the
     * supervisor withholds permission but does not latch: "the sensor never
     * got going" and "the sensor was working and stopped" are different, and
     * only the second is a fault to acknowledge.  Without this the stale timer
     * runs from the first cycle and latches at boot if the front end's first
     * conversion takes longer than the grace, which is a trip with nothing
     * wrong behind it. */
    bool              seen_valid;
    /* The clear button is edge triggered, and these are why.
     *
     * A latch cleared on the pin *level* is not a latch: a button shorted to
     * ground, or one stuck down, would clear it on every cycle, and the
     * supervisor would then permit heat whenever the instantaneous condition
     * happened to be good.  That is the latch defeated by a single solder
     * bridge.
     *
     * So the input has to be seen *released* before it can clear anything
     * (`clear_armed`), and it has to be held (`clear_s`).  A shorted line
     * never arms, because it is never seen released, including at power-on. */
    bool              clear_armed;
    float             clear_s;
} sup_t;

/* Comes up refusing heat: chamber_valid is false until a conversion has been
 * seen, and permit is conjunctive, so absence of evidence is not permission. */
void sup_init(sup_t *s, bool selftest_ok);

/* One cycle.  dt_s is the elapsed time since the previous call. */
void sup_step(sup_t *s, const sup_input_t *in, float dt_s);

/* Clears a latched trip.
 *
 * sup_step() calls this itself when the clear button has been armed and held,
 * so the whole behaviour is in the tested core rather than in the board layer.
 * It stays public because a test should be able to drive it directly.
 *
 * There is no path to this from the link: the supervisor has no receiver. */
void sup_clear(sup_t *s);

/* The flags byte for the wire, derived rather than tracked separately. */
uint8_t sup_flags(const sup_t *s, const sup_input_t *in);

#endif /* SUP_TRIP_H */

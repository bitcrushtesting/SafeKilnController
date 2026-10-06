/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Kiln plant simulator -- TR-11, TR-12, TR-27, architecture section 14.3.
 *
 * First-order-plus-dead-time (ASM-01) for temperature, plus a heater-current
 * model consistent with the commanded output state, plus the fault injections
 * that each safety rule is meant to catch.  One plant drives both channels, so an
 * injected element failure shows up in degrees *and* in amps exactly as it would
 * on a real kiln -- which is the whole reason the current rules and the thermal
 * rules can be tested against the same scenario.
 *
 * Deterministic from a seed (TR-12): a failure replays exactly.
 *
 * This component implements kiln_ports interfaces, so it substitutes for the
 * hardware adapters at the composition root -- on the host for the integration
 * suite, and on the target when the firmware is built with the simulated plant
 * (which is how it runs under QEMU, where there is no MAX31856 and no ADC to
 * emulate).
 */
#ifndef KILN_SIM_H
#define KILN_SIM_H

#include "kiln/err.h"
#include "kiln/types.h"
#include "kiln_ports/port_counters.h"
#include "kiln_ports/port_current.h"
#include "kiln_ports/port_door.h"
#include "kiln_ports/port_filestore.h"
#include "kiln_ports/port_heat.h"
#include "kiln_ports/port_tc.h"

/* --- fault injection (TR-27, architecture section 14.3) ----------------- */

typedef enum {
    /* electrical */
    KILN_INJ_RELAY_FAIL_ON    = 1u << 0,   /* SR-25: current with duty 0      */
    KILN_INJ_RELAY_FAIL_OFF   = 1u << 1,   /* SR-26: no current with duty > 0 */
    KILN_INJ_CONTACTOR_WELD   = 1u << 2,   /* SR-27: current persists after
                                            * the contactor is commanded open */
    KILN_INJ_ELEMENT_PARTIAL  = 1u << 3,   /* SR-26, SR-28, SR-07            */
    KILN_INJ_OVERCURRENT      = 1u << 4,   /* SR-29                          */
    KILN_INJ_CT_DISCONNECTED  = 1u << 5,   /* FR-CUR-11, FR-CUR-12           */
    KILN_INJ_SSR_SHORTED      = 1u << 6,   /* SR-08 and SR-25 together       */

    /* sensing */
    KILN_INJ_TC_OPEN          = 1u << 7,   /* SR-04                          */
    KILN_INJ_TC_SHORT         = 1u << 8,
    KILN_INJ_TC_RANGE         = 1u << 9,
    KILN_INJ_TC_COMMS         = 1u << 10,
    KILN_INJ_TC_REVERSED      = 1u << 11,  /* SR-05                          */
    KILN_INJ_TC_STUCK         = 1u << 12,  /* SR-06                          */
    KILN_INJ_TC_DRIFT         = 1u << 13,  /* FR-ACQ-08, SR-12               */

    /* plant */
    KILN_INJ_LID_OPEN         = 1u << 14,  /* SR-07, FR-CTL-11               */
    KILN_INJ_CASE_HEATING     = 1u << 15,  /* SR-11                          */
    KILN_INJ_ELEMENT_OPEN     = 1u << 16,  /* SR-07 and SR-26                */

    /* SR-31.  Separate from KILN_INJ_LID_OPEN on purpose: that one is the
     * *thermal* model -- an open lid losing heat, which is what SR-07 sees --
     * whereas this is the interlock *switch* reading open.  Keeping them apart
     * is what lets a test exercise a switch that has failed open while the door
     * is shut, or a door genuinely open on a kiln with no interlock fitted.
     * A realistic "operator opened the door mid-firing" injects both. */
    KILN_INJ_DOOR_SWITCH_OPEN = 1u << 17,
    /* No interlock fitted at all -- stands SR-31 down, warning 113. */
    KILN_INJ_DOOR_ABSENT      = 1u << 18,
} kiln_inject_t;

typedef struct {
    /* thermal, architecture section 14.3:
     *   T[n+1] = T[n] + dt/tau * (K*u_delayed + T_ambient - T[n]) + noise */
    float    gain_c;             /* K: degC of steady-state rise at 100 % duty */
    float    tau_s;
    float    dead_time_s;
    float    ambient_c;
    /* Radiative loss rises steeply with temperature, so a kiln's gain falls as it
     * gets hotter -- the reason FR-TUN-13 offers several gain sets and the reason
     * one autotune at 600 degC does not suit 1250 degC.  Effective gain is
     * scaled by (1 - loss_frac * (T/1300)^4). */
    float    loss_frac;
    float    noise_c;            /* peak sensor noise                         */
    uint32_t seed;

    /* case channel */
    float    case_ambient_c;
    float    case_coupling;      /* fraction of chamber rise reaching the case */

    /* electrical */
    float    nominal_a;          /* at the reference temperature               */
    float    element_tc_per_c;   /* positive temperature coefficient           */
    float    leakage_a;          /* a healthy SSR's off-state leakage          */
    uint8_t  element_groups;     /* for KILN_INJ_ELEMENT_PARTIAL               */
    float    mains_hz;

    /* CT front end, matching the core's scaling so a synthesised burst reduces
     * back to the current that was simulated. */
    float    ct_a_per_v;
    float    adc_v_per_count;
    uint16_t bias_counts;
    float    noise_counts;       /* the floor FR-CUR-11 looks for              */

    /* AD-05 / HR-07 / SR-02: heat enable is a square wave into a charge pump,
     * not a level, so the coil de-energises unless something keeps refreshing
     * it.  Modelled as a decay timeout rather than as an RC curve: what the
     * firmware's correctness depends on is that a *stopped* safety task releases
     * the contactor, and that is exactly what a timeout captures.  Whether the
     * real circuit reaches the contactor's drop-out voltage inside NFR-04's one
     * second is a question about resistors, and belongs on the HIL jig (TR-17,
     * tasklist A7). */
    float    enable_decay_s;     /* default 1.0 */
} kiln_sim_cfg_t;

void kiln_sim_cfg_defaults(kiln_sim_cfg_t *cfg);

#define KILN_SIM_DEAD_SLOTS 256

typedef struct {
    kiln_sim_cfg_t cfg;

    /* plant */
    float    kiln_c;
    float    case_c;
    float    reported_c;         /* after injected sensor faults              */
    float    stuck_at_c;
    float    drift_c;
    double   t_s;

    /* dead-time ring of commanded duty */
    float    dead[KILN_SIM_DEAD_SLOTS];
    uint16_t dead_head;
    float    dead_accum_s;

    /* electrical */
    bool     heat_enable;        /* charge pump holding the coil up            */
    float    enable_age_s;       /* since the last refresh                     */
    bool     contactor_closed;   /* actual, which a weld decouples            */
    uint16_t duty_permille[KILN_HEAT_CHANNELS];
    bool     ssr_on[KILN_HEAT_CHANNELS];
    float    current_a;          /* instantaneous RMS over the present cycle  */

    uint32_t inject;
    uint32_t rng;

    /* port bookkeeping */
    kiln_cur_window_t burst_window;
    uint16_t          burst_n;
    bool              burst_armed;
    double            burst_due_s;
    uint16_t          burst_buf[KILN_CUR_BURST_MAX];
    uint32_t          burst_rate_hz;


    kiln_switch_counters_t counters;
    uint32_t switch_count[KILN_HEAT_CHANNELS];
    bool     forced_off;
} kiln_sim_t;

void kiln_sim_init(kiln_sim_t *s, const kiln_sim_cfg_t *cfg);

/* Advance the plant by dt_s.  Call this from the test loop or from the task that
 * stands in for the kiln; everything else is a port read. */
void kiln_sim_step(kiln_sim_t *s, float dt_s);

void kiln_sim_inject(kiln_sim_t *s, uint32_t faults);
void kiln_sim_clear(kiln_sim_t *s, uint32_t faults);

/* Place the kiln at a temperature directly, for a test that wants to start hot
 * rather than spend simulated hours getting there. */
void kiln_sim_set_temperature(kiln_sim_t *s, float kiln_c);

static inline float kiln_sim_temperature(const kiln_sim_t *s) { return s->kiln_c; }
static inline float kiln_sim_current(const kiln_sim_t *s)     { return s->current_a; }
static inline bool  kiln_sim_contactor(const kiln_sim_t *s)   { return s->contactor_closed; }

/* --- the ports the simulator implements --------------------------------- */

typedef struct {
    kiln_port_tc_t       tc;
    kiln_port_tc_t       case_tc;
    kiln_port_heat_t     heat;
    kiln_port_current_t  current;
    kiln_port_counters_t counters;
    kiln_port_door_t     door;
} kiln_sim_ports_t;

/* --- a RAM file store ---------------------------------------------------- */

/* Programs and run records need somewhere to go, and the LittleFS adapter does
 * not exist yet -- it is not in the IDF tree and CON-04 forbids pulling it at
 * build time, so it has to be vendored.  Until then this stands in, so the
 * persistence path of FR-PRG and FR-RUN-07 is exercised end to end rather than
 * skipped.
 *
 * It is RAM, so nothing survives a reboot.  That is a visible limitation of the
 * simulated build and not a design choice: everything above it behaves exactly as
 * it will against flash, which is the point of the port boundary.
 *
 * Sized for the three seeded examples plus a handful of run records -- about
 * 8 kB, which is affordable on a device with no PSRAM (NFR-11). */
#define KILN_SIM_FS_FILES    12
#define KILN_SIM_FS_FILE_MAX 640

typedef struct {
    struct {
        char    path[KILN_PATH_MAX];
        uint8_t data[KILN_SIM_FS_FILE_MAX];
        uint16_t len;
        bool    used;
    } files[KILN_SIM_FS_FILES];
} kiln_sim_fs_t;

void kiln_sim_fs_init(kiln_sim_fs_t *fs);
void kiln_sim_fs_bind(kiln_sim_fs_t *fs, kiln_port_filestore_t *out);

void kiln_sim_bind(kiln_sim_t *s, kiln_sim_ports_t *out);

/* The SSR pin level, published by whoever owns the 10 ms window tick (AD-07).
 * Separate from set_duty because the duty is a *request* and this is the pin:
 * the distinction is what lets the simulator model a window whose off interval
 * is too short to measure (FR-CUR-05). */
void kiln_sim_set_ssr(kiln_sim_t *s, uint8_t channel, bool on);

#endif /* KILN_SIM_H */

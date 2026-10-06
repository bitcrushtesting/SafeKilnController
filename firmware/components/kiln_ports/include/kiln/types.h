/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Domain types and hard limits shared across the layers.
 *
 * Units, fixed once here so stored values are never ambiguous:
 *   temperature   float, degrees Celsius          (FR-HMI-13: degF is display only)
 *   duty          uint16, per mille 0..1000
 *   rate          float, degrees Celsius per hour
 *   time          seconds (float dt) or microseconds (uint64 monotonic)
 */
#ifndef KILN_TYPES_H
#define KILN_TYPES_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- hard limits ------------------------------------------------------- */

/* SR-23: compile-time ceiling.  No configuration, program target or tuning
 * setpoint may exceed this, whatever the operator types. */
constexpr float KILN_TEMP_CEILING_C = 1350.0f;
constexpr float KILN_TEMP_FLOOR_C   = -20.0f;

constexpr size_t KILN_MAX_SEGMENTS     = 32;  /* FR-PRG-01 */
constexpr size_t KILN_PROGRAM_NAME_LEN = 32;
constexpr size_t KILN_PROGRAM_DESC_LEN = 96;
constexpr size_t KILN_MAX_PROGRAMS     = 20;  /* FR-PRG-04 */
constexpr size_t KILN_MAX_RUN_RECORDS  = 20;  /* FR-LOG-09 */
constexpr size_t KILN_MAX_GAIN_SETS    = 4;   /* FR-TUN-13 */

constexpr uint16_t KILN_DUTY_MAX = 1000u;  /* per mille */

/* HR-12: SSR1 and SSR2.  A single-zone kiln (ASM-02) drives both with the same
 * duty; the array dimension exists so independently switched element groups do
 * not need a second implementation. */
constexpr size_t KILN_HEAT_CHANNELS = 2;

constexpr uint8_t KILN_SEG_NONE = 0xFFu;

/* --- operating state (requirements section 2.2) ------------------------ */

typedef enum {
    KILN_STATE_IDLE = 0,
    KILN_STATE_RUNNING,
    KILN_STATE_PAUSED,
    KILN_STATE_MANUAL,
    KILN_STATE_AUTOTUNE,
    KILN_STATE_COMPLETE,
    KILN_STATE_FAULT,
    KILN_STATE_COUNT,       /* must stay <= 15: packed into a log nibble */
} kiln_state_t;

/* --- thermocouple ------------------------------------------------------ */

typedef enum {
    KILN_TC_TYPE_B = 0,
    KILN_TC_TYPE_E,
    KILN_TC_TYPE_J,
    KILN_TC_TYPE_K,         /* default, FR-ACQ-02 */
    KILN_TC_TYPE_N,
    KILN_TC_TYPE_R,
    KILN_TC_TYPE_S,
    KILN_TC_TYPE_T,
    KILN_TC_TYPE_COUNT,
} kiln_tc_type_t;

/* FR-ACQ-10: the fault detail the front end must expose.
 *
 * uint32_t, not the uint16_t field these are stored in: a narrower constant
 * promotes to *int* before a bitwise operator, so KILN_A | KILN_B would be a
 * signed operation (bugprone-signed-bitwise) at every site that composes a
 * mask.  The field's own width still bounds what can be stored.  The same
 * reasoning applies to every KILN_*_FLAG_* and KILN_CURF_* mask below. */
constexpr uint32_t KILN_TC_FAULT_OPEN      = 1u << 0u;  /* open circuit                   */
constexpr uint32_t KILN_TC_FAULT_SHORT_VCC = 1u << 1u;  /* short to supply                */
constexpr uint32_t KILN_TC_FAULT_SHORT_GND = 1u << 2u;  /* short to ground                */
constexpr uint32_t KILN_TC_FAULT_CJ_RANGE  = 1u << 3u;  /* cold junction out of range     */
constexpr uint32_t KILN_TC_FAULT_TC_RANGE  = 1u << 4u;  /* thermocouple out of range      */
constexpr uint32_t KILN_TC_FAULT_OVUV      = 1u << 5u;  /* over/under voltage             */
constexpr uint32_t KILN_TC_FAULT_COMMS     = 1u << 6u;  /* front end did not answer       */
constexpr uint32_t KILN_TC_FAULT_ANY       = 0x7Fu;

/* --- operator language (NFR-23) ----------------------------------------
 *
 * NFR-23 asks for operator text in one resource location so it can be
 * translated later.  This is "later": the fault, warning and state tables in
 * kiln_core/faults are indexed by language, and nothing else in the firmware
 * holds operator-facing prose.
 *
 * It is a presentation concern only.  Nothing in the control or safety path
 * reads it, and a language the build does not carry falls back to English
 * rather than showing an empty screen. */
typedef enum {
    KILN_LANG_EN = 0,
    KILN_LANG_DE,
    KILN_LANG_COUNT,
} kiln_lang_t;

/* --- fault codes (requirements appendix A; stable, never reused) -------- */

typedef enum {
    KILN_FAULT_NONE                = 0,
    KILN_FAULT_TC_OPEN             = 1,
    KILN_FAULT_TC_SHORT            = 2,
    KILN_FAULT_TC_RANGE            = 3,
    KILN_FAULT_CJ                  = 4,
    KILN_FAULT_TC_COMMS            = 5,
    KILN_FAULT_TC_REVERSED         = 6,
    KILN_FAULT_TC_STUCK            = 7,
    KILN_FAULT_RUNAWAY             = 8,
    KILN_FAULT_UNCOMMANDED_HEAT    = 9,
    KILN_FAULT_OVERTEMP            = 10,
    KILN_FAULT_SP_EXCURSION        = 11,
    KILN_FAULT_CASE_OVERTEMP       = 12,
    KILN_FAULT_CONTROL_DEADLINE    = 13,
    KILN_FAULT_SAFETY_DEADLINE     = 14,
    KILN_FAULT_WATCHDOG            = 15,
    KILN_FAULT_CASE_TC             = 16,
    KILN_FAULT_OPERATOR_ABORT      = 17,
    KILN_FAULT_TUNE_NO_CONVERGE    = 18,
    KILN_FAULT_RECOVERY_REFUSED    = 19,
    KILN_FAULT_CONFIG_STORAGE      = 20,
    KILN_FAULT_UNCOMMANDED_CURRENT = 21,   /* SR-25 */
    KILN_FAULT_CONTACTOR_WELDED    = 22,   /* SR-27 */
    KILN_FAULT_NO_HEATER_CURRENT   = 23,   /* SR-26 */
    KILN_FAULT_CURRENT_DEVIATION   = 24,   /* SR-28 */
    KILN_FAULT_OVERCURRENT         = 25,   /* SR-29 */
    KILN_FAULT_CT_FAULT            = 26,   /* FR-CUR-11 */
    KILN_FAULT_DOOR_OPEN           = 27,   /* SR-31 */
    KILN_FAULT_MAX                 = 28,
} kiln_fault_t;

/* Warnings do not stop a firing (requirements appendix A). Held as a bitmask,
 * so the numeric codes 101.. map onto bit positions. */
typedef enum {
    KILN_WARN_INSULATION      = 0,   /* 101  SR-12  */
    KILN_WARN_HOLDBACK        = 1,   /* 102  FR-CTL-11 */
    KILN_WARN_LOG_UNAVAIL     = 2,   /* 103  FR-LOG-14 */
    KILN_WARN_DISPLAY_UNAVAIL = 3,   /* 104  FR-HMI-14 */
    KILN_WARN_TIME_UNSYNCED   = 4,   /* 105  FR-LOG-12 */
    KILN_WARN_WIFI_DOWN       = 5,   /* 106  FR-NET-07 */
    KILN_WARN_DUTY_SATURATED  = 6,   /* 107  FR-CTL-15 */
    KILN_WARN_GAINS_UNTUNED   = 7,   /* 108  FR-TUN-11 */
    KILN_WARN_RELAY_WEAR      = 8,   /* 109  SR-30 */
    KILN_WARN_RELAY_SUSPECT   = 9,   /* 110  SR-30 */
    KILN_WARN_CURRENT_OFF     = 10,  /* 111  FR-CUR-12 */
    KILN_WARN_CURRENT_DEV     = 11,  /* 112  SR-28 */
    KILN_WARN_DOOR_OFF        = 12,  /* 113  SR-31 */
    KILN_WARN_COUNT           = 13,
} kiln_warn_bit_t;

constexpr uint16_t KILN_WARN_CODE_BASE = 101;
#define KILN_WARN_BIT(b)     (1u << (b))

/* --- program ----------------------------------------------------------- */

constexpr uint32_t KILN_SEG_FLAG_REQUIRE_ACK = 1u << 0u;  /* FR-PRG-03 */

typedef struct {
    uint16_t target_c;          /* 0 .. KILN_TEMP_CEILING_C                  */
    uint16_t rate_c_per_h;      /* 0 = as fast as the kiln allows, FR-CTL-10 */
    uint16_t dwell_min;         /* 0 .. 5999                                 */
    uint8_t  flags;
    uint8_t  reserved;
} kiln_segment_t;

constexpr uint32_t KILN_PROG_FLAG_READONLY = 1u << 0u;  /* FR-PRG-09 examples */

typedef struct {
    char           name[KILN_PROGRAM_NAME_LEN];
    char           description[KILN_PROGRAM_DESC_LEN];
    uint8_t        segment_count;
    uint8_t        flags;
    uint16_t       schema_version;
    kiln_segment_t segments[KILN_MAX_SEGMENTS];
} kiln_program_t;

constexpr uint16_t KILN_PROGRAM_SCHEMA_VERSION = 1;

/* --- per-cycle plant snapshot (AD-13: passed by value, never shared) ---- */

typedef struct {
    uint64_t t_mono_us;
    float    kiln_c;            /* filtered, calibrated  */
    float    kiln_c_raw;        /* calibrated, unfiltered (FR-ACQ-07) */
    float    case_c;
    float    cj_c;
    float    rate_c_per_h;
    float    setpoint_c;
    uint16_t duty_permille;
    float    current_a;            /* last valid RMS measurement, FR-CUR-02  */
    float    current_ref_a;        /* run reference, FR-CUR-08               */
    uint8_t  current_flags;        /* KILN_CURF_*                            */
    uint16_t tc_fault_bits;
    uint16_t case_fault_bits;
    uint8_t  state;             /* kiln_state_t */
    uint8_t  segment_index;
    uint8_t  segment_count;
    bool     heat_authorised;
    bool     holdback_active;
    bool     duty_saturated;
    bool     kiln_valid;        /* false while inside the FR-ACQ-12 grace period */
    bool     case_valid;
} kiln_snapshot_t;

/* --- heater current (FR-CUR) ------------------------------------------- */

/* What a current sample actually represents.  FR-CUR-04/05: without this a
 * reader cannot tell 0.0 A "the relay is correctly off" from 0.0 A "the window
 * was too short to measure". */
constexpr uint32_t KILN_CURF_CONDUCTION = 1u << 0u;  /* measured inside a commanded-on window  */
constexpr uint32_t KILN_CURF_LEAKAGE    = 1u << 1u;  /* measured inside a commanded-off window */
constexpr uint32_t KILN_CURF_SKIPPED    = 1u << 2u;  /* window too short to measure            */
constexpr uint32_t KILN_CURF_STALE      = 1u << 3u;  /* carried over from an earlier window    */
constexpr uint32_t KILN_CURF_CT_FAULT   = 1u << 4u;  /* transformer absent or shorted          */

/* --- small helpers ----------------------------------------------------- */

/* NFR-17, SR-01: a non-finite number must not be able to travel through the
 * control path.  Both comparisons in a naive clamp are false for a NaN, so a
 * clamp that does not say otherwise passes NaN straight through every range
 * check built on it -- and a NaN duty, integral or filter state is persistent
 * once it is in there.  These two are the only sanctioned way in. */
static inline bool kiln_is_finite(float v)
{
    return static_cast<int>(isfinite(v)) != 0;
}

/* Substitute a known-safe value for a non-finite one.  Used at the acquisition
 * boundary; the core asserts instead, because by then it is a caller bug. */
static inline float kiln_sanitisef(float v, float fallback)
{
    return kiln_is_finite(v) ? v : fallback;
}

/* Clamps, and maps a non-finite input to lo rather than propagating it.
 *
 * lo is the conservative end for the duties and rates this is used on, but it is
 * NOT conservative for a temperature -- a NaN reading clamped to the floor reads
 * as a cold kiln.  Temperatures are therefore gated by validity at the
 * acquisition boundary (tempfilt rejects a non-finite sample) and again in
 * safety (an invalid reading withholds heat), and never rely on this. */
static inline float kiln_clampf(float v, float lo, float hi)
{
    if (!kiln_is_finite(v)) {
        return lo;
    }
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Byte view of an object.  The fileslots CRC and the JSON escaper both have to
 * read an object's representation one byte at a time, which the standard
 * permits through unsigned char.  Named, and routed through void *, rather than
 * a reinterpret_cast at each site: the intent is "read these bytes", not "this
 * is secretly another type", and void * is already the currency the port layer
 * passes buffers in (AD-01).  cppcoreguidelines-pro-type-reinterpret-cast
 * therefore stays switched on -- see configmodel.cpp for the two sites that
 * genuinely need the stronger cast and say so. */
static inline const uint8_t *kiln_bytes_of(const void *p)
{
    return static_cast<const uint8_t *>(p);
}

/* Clamp in place, and report whether the value actually had to move.
 *
 * NFR-17 requires the clamp_cfg() functions in safety and current to tell the
 * caller that a configured threshold was corrected rather than silently fixing
 * it.  They cannot get that answer by memcmp-ing a before-and-after copy of the
 * config struct: both structs carry padding, and the standard does not
 * guarantee padding bytes propagate through a struct copy, so an unchanged
 * configuration could still compare unequal and report a spurious correction
 * (bugprone-suspicious-memory-comparison).  In safety code a false "clamped"
 * is a wrong answer, so each field reports for itself instead.
 *
 * A non-finite input always reports moved: kiln_clampf() maps it to lo, and
 * the NaN comparison is unequal, which is the honest answer. */
static inline bool kiln_clampf_moved(float *v, float lo, float hi)
{
    const float out   = kiln_clampf(*v, lo, hi);
    const bool  moved = out != *v;
    *v = out;
    return moved;
}

static inline uint16_t kiln_clampu16(int32_t v, uint16_t lo, uint16_t hi)
{
    if (v < (int32_t)lo) {
        return lo;
    }
    if (v > (int32_t)hi) {
        return hi;
    }
    return (uint16_t)v;
}

#endif /* KILN_TYPES_H */

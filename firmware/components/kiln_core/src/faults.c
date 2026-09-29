/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "kiln_core/faults.h"

typedef struct {
    const char *label;
    const char *cause;
    const char *req;
} fault_entry_t;

/* Indexed by kiln_fault_t; order is load-bearing and checked by a unit test. */
static const fault_entry_t k_faults[KILN_FAULT_MAX] = {
    [KILN_FAULT_NONE]             = {"OK",         "No fault", ""},
    [KILN_FAULT_TC_OPEN]          = {"TC OPEN",
        "Kiln thermocouple open circuit. Check the probe and its connections.", "SR-04"},
    [KILN_FAULT_TC_SHORT]         = {"TC SHORT",
        "Kiln thermocouple shorted. Check for damaged insulation.", "SR-04"},
    [KILN_FAULT_TC_RANGE]         = {"TC RANGE",
        "Kiln temperature outside the valid range for this thermocouple type.", "SR-04"},
    [KILN_FAULT_CJ]               = {"CJ FAULT",
        "Cold junction out of range. The controller itself may be too hot or too cold.", "SR-04"},
    [KILN_FAULT_TC_COMMS]         = {"TC COMMS",
        "No response from the thermocouple front end. Check wiring to the MAX31856.", "SR-04"},
    [KILN_FAULT_TC_REVERSED]      = {"TC REVERSED",
        "Temperature fell while heating at high duty: the thermocouple is probably "
        "connected with reversed polarity.", "SR-05"},
    [KILN_FAULT_TC_STUCK]         = {"TC STUCK",
        "Temperature did not change while heating. The probe reading is stuck.", "SR-06"},
    [KILN_FAULT_RUNAWAY]          = {"NO HEAT",
        "Heating at high duty produced no temperature rise: failed element, open "
        "contactor, open safety chain, or the lid is not closed.", "SR-07"},
    [KILN_FAULT_UNCOMMANDED_HEAT] = {"SSR SHORT",
        "Temperature rising with heating commanded off. The SSR is probably shorted. "
        "Isolate the kiln at the wall.", "SR-08"},
    [KILN_FAULT_OVERTEMP]         = {"OVERTEMP",
        "Kiln temperature exceeded the configured maximum.", "SR-09"},
    [KILN_FAULT_SP_EXCURSION]     = {"OVERSHOOT",
        "Temperature ran far above setpoint for too long. PID gains may be wrong.", "SR-10"},
    [KILN_FAULT_CASE_OVERTEMP]    = {"CASE HOT",
        "Controller enclosure too hot. Improve ventilation or move the controller.", "SR-11"},
    [KILN_FAULT_CONTROL_DEADLINE] = {"CTRL LATE",
        "The control loop missed its deadline. This is a firmware fault.", "SR-13"},
    [KILN_FAULT_SAFETY_DEADLINE]  = {"SAFE LATE",
        "The safety supervisor missed its deadline. This is a firmware fault.", "SR-13"},
    [KILN_FAULT_WATCHDOG]         = {"WATCHDOG",
        "The controller was reset by the watchdog during a firing.", "SR-14"},
    [KILN_FAULT_CASE_TC]          = {"CASE TC",
        "Enclosure thermocouple fault. Enclosure temperature cannot be supervised.", "SR-04"},
    [KILN_FAULT_OPERATOR_ABORT]   = {"ABORTED",
        "Firing aborted by the operator.", "FR-RUN-04"},
    [KILN_FAULT_TUNE_NO_CONVERGE] = {"TUNE FAIL",
        "Automatic tuning did not find a stable oscillation. Previous gains kept.",
        "FR-TUN-07"},
    [KILN_FAULT_RECOVERY_REFUSED] = {"NO RESUME",
        "Power was lost during a firing and the kiln had cooled too far to resume "
        "safely.", "FR-RUN-08"},
    [KILN_FAULT_CONFIG_STORAGE]   = {"STORAGE",
        "Configuration could not be read or written. Defaults are in use.", "FR-CFG-05"},
    [KILN_FAULT_UNCOMMANDED_CURRENT] = {"RELAY ON",
        "Heater current is flowing with heating commanded off. A relay is stuck on.",
        "SR-25"},
    [KILN_FAULT_CONTACTOR_WELDED] = {"WELDED",
        "Current continued after the contactor was commanded open: the contactor is "
        "welded shut. The controller can no longer interrupt the heaters. ISOLATE THE "
        "KILN AT ITS SUPPLY NOW.", "SR-27"},
    [KILN_FAULT_NO_HEATER_CURRENT] = {"NO CURRENT",
        "No heater current with heating commanded on: failed SSR, open contactor, "
        "blown heater fuse, open safety chain or open elements.", "SR-26"},
    [KILN_FAULT_CURRENT_DEVIATION] = {"CURR DEV",
        "Heater current differs sharply from this run's reference. An element group "
        "has probably failed.", "SR-28"},
    [KILN_FAULT_OVERCURRENT]      = {"OVERCURRENT",
        "Heater current above the configured maximum: shorted element, wrong wiring "
        "or a failed SSR.", "SR-29"},
    [KILN_FAULT_CT_FAULT]         = {"CT FAULT",
        "No signal from the current transformer. Check that it is fitted around one "
        "heater conductor and plugged in.", "FR-CUR-11"},
};

static const fault_entry_t k_warns[KILN_WARN_COUNT] = {
    [KILN_WARN_INSULATION]      = {"INSULATION",
        "This firing needed noticeably more energy than usual. Insulation or "
        "elements may be degrading.", "SR-12"},
    [KILN_WARN_HOLDBACK]        = {"HOLDBACK",
        "The kiln is behind its curve, so the schedule is being held back.",
        "FR-CTL-11"},
    [KILN_WARN_LOG_UNAVAIL]     = {"NO LOG",
        "Temperature logging is unavailable. The firing continues.", "FR-LOG-14"},
    [KILN_WARN_DISPLAY_UNAVAIL] = {"NO DISPLAY",
        "The display is not responding. The firing continues.", "FR-HMI-14"},
    [KILN_WARN_TIME_UNSYNCED]   = {"NO TIME",
        "Clock not synchronised; log timestamps are relative only.", "FR-LOG-12"},
    [KILN_WARN_WIFI_DOWN]       = {"NO WIFI",
        "WiFi is down. The firing is unaffected.", "FR-NET-07"},
    [KILN_WARN_DUTY_SATURATED]  = {"FULL POWER",
        "Heating has been at full power for a long time; the kiln may not keep up.",
        "FR-CTL-15"},
    [KILN_WARN_GAINS_UNTUNED]   = {"UNTUNED",
        "PID gains are factory defaults. Run automatic tuning for this kiln.",
        "FR-TUN-11"},
    [KILN_WARN_RELAY_WEAR]      = {"RELAY WEAR",
        "A relay has reached its configured switching-operation life limit. Plan to "
        "replace it.", "SR-30"},
    [KILN_WARN_RELAY_SUSPECT]   = {"RELAY?",
        "Heater current has intermittently disagreed with the commanded state. A "
        "relay may be starting to fail.", "SR-30"},
    [KILN_WARN_CURRENT_OFF]     = {"NO CT",
        "Current monitoring is disabled. Relay faults can only be inferred from "
        "temperature, which is far slower.", "FR-CUR-12"},
    [KILN_WARN_CURRENT_DEV]     = {"CURR LOW",
        "Heater current is drifting from this run's reference. Elements may be "
        "ageing.", "SR-28"},
};

static const char *k_states[KILN_STATE_COUNT] = {
    [KILN_STATE_IDLE]     = "IDLE",
    [KILN_STATE_RUNNING]  = "RUN",
    [KILN_STATE_PAUSED]   = "PAUSE",
    [KILN_STATE_MANUAL]   = "MANUAL",
    [KILN_STATE_AUTOTUNE] = "TUNE",
    [KILN_STATE_COMPLETE] = "DONE",
    [KILN_STATE_FAULT]    = "FAULT",
};

const char *kiln_fault_label(kiln_fault_t code)
{
    if ((unsigned)code >= KILN_FAULT_MAX || !k_faults[code].label) return "UNKNOWN";
    return k_faults[code].label;
}

const char *kiln_fault_cause(kiln_fault_t code)
{
    if ((unsigned)code >= KILN_FAULT_MAX || !k_faults[code].cause) return "Unknown fault code.";
    return k_faults[code].cause;
}

const char *kiln_fault_requirement(kiln_fault_t code)
{
    if ((unsigned)code >= KILN_FAULT_MAX || !k_faults[code].req) return "";
    return k_faults[code].req;
}

const char *kiln_warn_label(kiln_warn_bit_t bit)
{
    if ((unsigned)bit >= KILN_WARN_COUNT || !k_warns[bit].label) return "UNKNOWN";
    return k_warns[bit].label;
}

const char *kiln_warn_cause(kiln_warn_bit_t bit)
{
    if ((unsigned)bit >= KILN_WARN_COUNT || !k_warns[bit].cause) return "Unknown warning.";
    return k_warns[bit].cause;
}

const char *kiln_state_label(kiln_state_t state)
{
    if ((unsigned)state >= KILN_STATE_COUNT || !k_states[state]) return "?";
    return k_states[state];
}

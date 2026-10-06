/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <string.h>
#include "kiln_core/runstate.h"

void kiln_runstate_record_init(kiln_run_record_t *r, uint32_t run_id)
{
    if (r == nullptr) {
        return;
    }
    memset(r, 0, sizeof(*r));
    r->run_id     = run_id;
    r->end_reason = KILN_END_UNKNOWN;
    r->fault      = KILN_FAULT_NONE;
    memcpy(r->gain_set_name, "default", sizeof("default"));
}

void kiln_runstate_recovery_defaults(kiln_recovery_cfg_t *cfg)
{
    if (cfg == nullptr) {
        return;
    }
    cfg->policy         = KILN_RECOVERY_ABORT;   /* FR-RUN-08 default */
    cfg->max_outage_min = 15;
    cfg->band_c         = 50.0f;
}

/* --- FR-RUN-08 ---------------------------------------------------------- */

namespace {

kiln_recovery_decision_t decision(kiln_recover_action_t action,
                                         kiln_fault_t fault,
                                         const char *reason)
{
    kiln_recovery_decision_t d = {};
    d.action = action;
    d.fault  = fault;
    d.reason = reason;
    return d;
}

} // namespace

kiln_recovery_decision_t kiln_runstate_decide(const kiln_recovery_cfg_t *cfg,
                                              const kiln_log_sample_t *tail,
                                              float now_kiln_c,
                                              float outage_s,
                                              kiln_reset_cause_t cause)
{
    if (cfg == nullptr) {
        return decision(KILN_RECOVER_ABORT, KILN_FAULT_NONE, "no recovery policy");
    }

    /* Nothing was running, or the log has nothing decodable to resume from. */
    if (tail == nullptr) {
        return decision(KILN_RECOVER_NO_RUN, KILN_FAULT_NONE, "no interrupted run");
    }

    const kiln_state_t state = (kiln_state_t)tail->state;
    if (state != KILN_STATE_RUNNING && state != KILN_STATE_PAUSED) {
        return decision(KILN_RECOVER_NO_RUN, KILN_FAULT_NONE, "last run had ended");
    }

    kiln_recovery_decision_t d = decision(KILN_RECOVER_ABORT, KILN_FAULT_NONE, "");
    d.segment    = tail->segment;
    d.setpoint_c = tail->setpoint_c;
    d.kiln_c     = tail->kiln_filt_c;
    d.t_rel_ms   = tail->t_rel_ms;

    /* SR-14 / NFR-15.  A watchdog, panic or brownout reset means the firmware's
     * own state was in question at the moment it died; the run is ended and the
     * cause recorded, whatever the recovery policy would otherwise allow. */
    if (kiln_reset_was_abnormal(cause)) {
        d.action = KILN_RECOVER_REFUSED;
        d.fault  = (cause == KILN_RESET_BROWNOUT) ? KILN_FAULT_RECOVERY_REFUSED
                                                  : KILN_FAULT_WATCHDOG;
        d.reason = "the controller reset abnormally during the firing";
        return d;
    }

    /* The configured policy, which defaults to abort: resuming a firing is a
     * decision an operator opts into, not one the controller makes for them. */
    if (cfg->policy != KILN_RECOVERY_RESUME) {
        d.action = KILN_RECOVER_ABORT;
        d.reason = "power was lost during the firing and the policy is to abort";
        return d;
    }

    /* A negative outage means the wall clock could not say how long power was
     * off.  An unknown outage is not a short one. */
    const float limit_s = (float)cfg->max_outage_min * 60.0f;
    if (!kiln_is_finite(outage_s) || outage_s < 0.0f) {
        d.action = KILN_RECOVER_REFUSED;
        d.fault  = KILN_FAULT_RECOVERY_REFUSED;
        d.reason = "the length of the power outage could not be established";
        return d;
    }
    if (outage_s > limit_s) {
        d.action = KILN_RECOVER_REFUSED;
        d.fault  = KILN_FAULT_RECOVERY_REFUSED;
        d.reason = "power was off for longer than the resume limit";
        return d;
    }

    /* And the kiln must still be near where the curve left it: a kiln that has
     * cooled out of band cannot pick the program up without a thermal shock to
     * whatever is inside it. */
    if (!kiln_is_finite(now_kiln_c)) {
        d.action = KILN_RECOVER_REFUSED;
        d.fault  = KILN_FAULT_RECOVERY_REFUSED;
        d.reason = "the present temperature is not known";
        return d;
    }
    const float lag = tail->setpoint_c > now_kiln_c ? tail->setpoint_c - now_kiln_c
                                                    : now_kiln_c - tail->setpoint_c;
    if (lag > cfg->band_c) {
        d.action = KILN_RECOVER_REFUSED;
        d.fault  = KILN_FAULT_RECOVERY_REFUSED;
        d.reason = "the kiln had cooled too far from its setpoint to resume safely";
        return d;
    }

    d.action = KILN_RECOVER_RESUME;
    d.kiln_c = now_kiln_c;
    d.reason = "resuming the interrupted firing";
    return d;
}

/* --- SR-12 baseline ----------------------------------------------------- */

namespace {

uint32_t median_u32(uint32_t *v, uint8_t n)
{
    /* Insertion sort: n is at most KILN_MAX_RUN_RECORDS (20). */
    for (uint8_t i = 1; i < n; i++) {
        const uint32_t key = v[i];
        uint8_t j = i;
        while (j > 0 && v[j - 1] > key) { v[j] = v[j - 1]; j--; }
        v[j] = key;
    }
    return ((n & 1u) != 0u) ? v[n / 2] : (uint32_t)(((uint64_t)v[n / 2 - 1] + v[n / 2]) / 2u);
}

} // namespace

kiln_err_t kiln_runstate_baseline(const kiln_run_record_t *records, uint8_t count,
                                  uint8_t min_runs,
                                  kiln_insulation_baseline_t *out)
{
    if (out == nullptr) {
        return KILN_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    if ((records == nullptr) || count == 0) {
        return KILN_ERR_NOT_FOUND;
    }
    if (min_runs == 0) {
        min_runs = 1;
    }
    if (count > KILN_MAX_RUN_RECORDS) {
        count = KILN_MAX_RUN_RECORDS;
    }

    for (uint8_t b = 0; b < KILN_INSUL_BANDS; b++) {
        uint32_t pool[KILN_MAX_RUN_RECORDS];
        uint8_t  n = 0;

        for (uint8_t i = 0; i < count; i++) {
            /* Only runs that actually reached the band, and only runs that got
             * there normally: an aborted firing's duty-seconds to 900 degC are a
             * fact, but a faulted one's may have been accumulated while a rule was
             * already unhappy. */
            if (records[i].band_duty_s[b] == 0) {
                continue;
            }
            if (records[i].end_reason == KILN_END_FAULT) {
                continue;
            }
            pool[n++] = records[i].band_duty_s[b];
        }

        if (n >= min_runs) {
            out->duty_s[b] = median_u32(pool, n);
            out->valid[b]  = out->duty_s[b] > 0;
        }
    }
    return KILN_OK;
}

const char *kiln_run_end_str(kiln_run_end_t reason)
{
    switch (reason) {
    case KILN_END_COMPLETE:       return "completed";
    case KILN_END_OPERATOR_ABORT: return "aborted by the operator";
    case KILN_END_FAULT:          return "stopped by a fault";
    case KILN_END_POWER_LOSS:     return "interrupted by a power loss";
    case KILN_END_UNKNOWN:
    case KILN_END_COUNT:
    default:                      return "unknown";
    }
}

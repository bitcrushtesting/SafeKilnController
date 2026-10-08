/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 * kiln_core/runstate -- SWR-RUN-07, SWR-RUN-08, SWR-SAF-12's baseline, SWA-09.
 */

#include <string.h>
#include "kiln_check.h"
#include "kiln_core/runstate.h"

static kiln_recovery_cfg_t cfg(uint8_t policy)
{
    kiln_recovery_cfg_t c;
    kiln_runstate_recovery_defaults(&c);
    c.policy = policy;
    return c;
}

static kiln_log_sample_t tail(kiln_state_t state, float sp, float pv)
{
    kiln_log_sample_t s = {};
    s.t_rel_ms    = 3600000u;
    s.state       = (uint8_t)state;
    s.segment     = 2;
    s.setpoint_c  = sp;
    s.kiln_filt_c = pv;
    return s;
}

/* --- SWR-RUN-08 --------------------------------------------------------- */

KILN_TEST(swrrun08_defaults_to_abort)
{
    /* Resuming a firing is a decision an operator opts into. */
    const kiln_recovery_cfg_t c = cfg(KILN_RECOVERY_ABORT);
    CHECK_EQ_INT(c.policy, KILN_RECOVERY_ABORT);
    CHECK_EQ_UINT(c.max_outage_min, 15u);
    CHECK_NEAR(c.band_c, 50.0f, 0.01f);

    const kiln_log_sample_t t = tail(KILN_STATE_RUNNING, 900.0f, 895.0f);
    const kiln_recovery_decision_t d =
        kiln_runstate_decide(&c, &t, 895.0f, 60.0f, KILN_RESET_POWER_ON);

    CHECK_EQ_INT(d.action, KILN_RECOVER_ABORT);
    CHECK_EQ_INT(d.fault, KILN_FAULT_NONE);   /* an abort is not a fault */
    CHECK(d.reason && d.reason[0]);
}

KILN_TEST(swrrun08_resumes_when_the_outage_was_short_and_the_kiln_is_in_band)
{
    const kiln_recovery_cfg_t c = cfg(KILN_RECOVERY_RESUME);
    const kiln_log_sample_t t = tail(KILN_STATE_RUNNING, 900.0f, 898.0f);

    const kiln_recovery_decision_t d =
        kiln_runstate_decide(&c, &t, 880.0f, 300.0f, KILN_RESET_POWER_ON);

    CHECK_EQ_INT(d.action, KILN_RECOVER_RESUME);
    CHECK_EQ_UINT(d.segment, 2u);
    CHECK_NEAR(d.setpoint_c, 900.0f, 0.01f);
    CHECK_EQ_UINT(d.t_rel_ms, 3600000u);
}

KILN_TEST(swrrun08_refuses_an_outage_longer_than_the_limit)
{
    const kiln_recovery_cfg_t c = cfg(KILN_RECOVERY_RESUME);
    const kiln_log_sample_t t = tail(KILN_STATE_RUNNING, 900.0f, 898.0f);

    const kiln_recovery_decision_t d =
        kiln_runstate_decide(&c, &t, 898.0f, 16.0f * 60.0f, KILN_RESET_POWER_ON);

    CHECK_EQ_INT(d.action, KILN_RECOVER_REFUSED);
    CHECK_EQ_INT(d.fault, KILN_FAULT_RECOVERY_REFUSED);
}

KILN_TEST(swrrun08_refuses_a_kiln_that_has_cooled_out_of_band)
{
    const kiln_recovery_cfg_t c = cfg(KILN_RECOVERY_RESUME);
    const kiln_log_sample_t t = tail(KILN_STATE_RUNNING, 900.0f, 898.0f);

    /* Picking a program back up here would thermally shock whatever is inside. */
    const kiln_recovery_decision_t d =
        kiln_runstate_decide(&c, &t, 700.0f, 60.0f, KILN_RESET_POWER_ON);

    CHECK_EQ_INT(d.action, KILN_RECOVER_REFUSED);
    CHECK_EQ_INT(d.fault, KILN_FAULT_RECOVERY_REFUSED);
}

KILN_TEST(swrrun08_an_unknown_outage_is_not_a_short_one)
{
    const kiln_recovery_cfg_t c = cfg(KILN_RECOVERY_RESUME);
    const kiln_log_sample_t t = tail(KILN_STATE_RUNNING, 900.0f, 898.0f);

    /* SWR-LOG-12: the wall clock is only valid once SNTP has succeeded, so after
     * a power cut the length of the outage may simply not be knowable. */
    kiln_recovery_decision_t d =
        kiln_runstate_decide(&c, &t, 898.0f, -1.0f, KILN_RESET_POWER_ON);
    CHECK_EQ_INT(d.action, KILN_RECOVER_REFUSED);

    d = kiln_runstate_decide(&c, &t, 898.0f, 0.0f / 0.0f, KILN_RESET_POWER_ON);
    CHECK_EQ_INT(d.action, KILN_RECOVER_REFUSED);

    d = kiln_runstate_decide(&c, &t, 0.0f / 0.0f, 60.0f, KILN_RESET_POWER_ON);
    CHECK_EQ_INT(d.action, KILN_RECOVER_REFUSED);
}

KILN_TEST(swrsaf14_an_abnormal_reset_is_never_resumed)
{
    /* SWR-NFR-15: the firmware's own state was in question when it died, so the
     * policy does not get a vote. */
    const kiln_recovery_cfg_t c = cfg(KILN_RECOVERY_RESUME);
    const kiln_log_sample_t t = tail(KILN_STATE_RUNNING, 900.0f, 898.0f);

    const struct { kiln_reset_cause_t cause; kiln_fault_t fault; } cases[] = {
        { KILN_RESET_TASK_WDT, KILN_FAULT_WATCHDOG },
        { KILN_RESET_INT_WDT,  KILN_FAULT_WATCHDOG },
        { KILN_RESET_RTC_WDT,  KILN_FAULT_WATCHDOG },
        { KILN_RESET_PANIC,    KILN_FAULT_WATCHDOG },
        { KILN_RESET_BROWNOUT, KILN_FAULT_RECOVERY_REFUSED },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const kiln_recovery_decision_t d =
            kiln_runstate_decide(&c, &t, 898.0f, 10.0f, cases[i].cause);
        CHECK_EQ_INT(d.action, KILN_RECOVER_REFUSED);
        CHECK_EQ_INT(d.fault, cases[i].fault);
    }

    CHECK(kiln_reset_was_abnormal(KILN_RESET_TASK_WDT));
    CHECK(!kiln_reset_was_abnormal(KILN_RESET_POWER_ON));
    CHECK(!kiln_reset_was_abnormal(KILN_RESET_SOFTWARE));
}

KILN_TEST(swrrun08_a_boot_with_no_interrupted_run_is_an_ordinary_boot)
{
    const kiln_recovery_cfg_t c = cfg(KILN_RECOVERY_RESUME);

    kiln_recovery_decision_t d =
        kiln_runstate_decide(&c, NULL, 20.0f, 10.0f, KILN_RESET_POWER_ON);
    CHECK_EQ_INT(d.action, KILN_RECOVER_NO_RUN);
    CHECK_EQ_INT(d.fault, KILN_FAULT_NONE);

    /* SWA-09: the log tail is the journal, so a run that ended normally is
     * recognisable from the state in its last record. */
    const kiln_log_sample_t done = tail(KILN_STATE_COMPLETE, 900.0f, 898.0f);
    d = kiln_runstate_decide(&c, &done, 898.0f, 10.0f, KILN_RESET_POWER_ON);
    CHECK_EQ_INT(d.action, KILN_RECOVER_NO_RUN);

    const kiln_log_sample_t idle = tail(KILN_STATE_IDLE, 0.0f, 20.0f);
    d = kiln_runstate_decide(&c, &idle, 20.0f, 10.0f, KILN_RESET_POWER_ON);
    CHECK_EQ_INT(d.action, KILN_RECOVER_NO_RUN);
}

KILN_TEST(swrrun08_a_paused_run_is_recoverable_too)
{
    const kiln_recovery_cfg_t c = cfg(KILN_RECOVERY_RESUME);
    const kiln_log_sample_t t = tail(KILN_STATE_PAUSED, 900.0f, 890.0f);
    const kiln_recovery_decision_t d =
        kiln_runstate_decide(&c, &t, 890.0f, 60.0f, KILN_RESET_POWER_ON);
    CHECK_EQ_INT(d.action, KILN_RECOVER_RESUME);
}

KILN_TEST(a_missing_policy_aborts_rather_than_guessing)
{
    const kiln_log_sample_t t = tail(KILN_STATE_RUNNING, 900.0f, 898.0f);
    const kiln_recovery_decision_t d =
        kiln_runstate_decide(NULL, &t, 898.0f, 10.0f, KILN_RESET_POWER_ON);
    CHECK_EQ_INT(d.action, KILN_RECOVER_ABORT);
}

/* --- SWR-RUN-07 --------------------------------------------------------- */

KILN_TEST(swrrun07_a_record_starts_empty_with_its_run_id)
{
    kiln_run_record_t r;
    kiln_runstate_record_init(&r, 17u);

    CHECK_EQ_UINT(r.run_id, 17u);
    CHECK_EQ_INT(r.end_reason, KILN_END_UNKNOWN);
    CHECK_EQ_INT(r.fault, KILN_FAULT_NONE);
    CHECK_STR_EQ(r.gain_set_name, "default");
    CHECK_NEAR(r.current_ref_a, 0.0f, 0.001f);

    for (int i = 0; i <= KILN_END_POWER_LOSS; i++) {
        const char *s = kiln_run_end_str((kiln_run_end_t)i);
        CHECK(s && s[0]);
    }
}

/* --- SWR-SAF-12's baseline -------------------------------------------------- */

KILN_TEST(swrsaf12_the_baseline_is_a_median_across_previous_runs)
{
    kiln_run_record_t runs[5];
    for (int i = 0; i < 5; i++) {
        kiln_runstate_record_init(&runs[i], (uint32_t)i + 1u);
        runs[i].end_reason = KILN_END_COMPLETE;
        runs[i].band_duty_s[5] = (uint32_t)(900 + i * 50);   /* 900..1100 */
    }
    /* One firing with a very light load, which a mean would be pulled by. */
    runs[0].band_duty_s[5] = 100u;

    kiln_insulation_baseline_t bl;
    CHECK_OK(kiln_runstate_baseline(runs, 5, 3, &bl));

    CHECK(bl.valid[5]);
    CHECK_EQ_UINT(bl.duty_s[5], 1000u);      /* the median, not the mean */
    CHECK(!bl.valid[0]);                     /* no run recorded that band */
}

KILN_TEST(swrsaf12_a_band_needs_enough_runs_before_it_is_trusted)
{
    kiln_run_record_t runs[5];
    for (int i = 0; i < 5; i++) {
        kiln_runstate_record_init(&runs[i], (uint32_t)i + 1u);
        runs[i].end_reason = KILN_END_COMPLETE;
    }
    /* Only two firings ever reached 600 degC. */
    runs[0].band_duty_s[5] = 1000u;
    runs[1].band_duty_s[5] = 1100u;

    kiln_insulation_baseline_t bl;
    CHECK_OK(kiln_runstate_baseline(runs, 5, 3, &bl));
    CHECK(!bl.valid[5]);

    CHECK_OK(kiln_runstate_baseline(runs, 5, 2, &bl));
    CHECK(bl.valid[5]);
    CHECK_EQ_UINT(bl.duty_s[5], 1050u);
}

KILN_TEST(swrsaf12_a_faulted_run_does_not_contribute_to_the_baseline)
{
    /* Duty-seconds accumulated while a rule was already unhappy are not a
     * measurement of a healthy kiln. */
    kiln_run_record_t runs[4];
    for (int i = 0; i < 4; i++) {
        kiln_runstate_record_init(&runs[i], (uint32_t)i + 1u);
        runs[i].end_reason = KILN_END_COMPLETE;
        runs[i].band_duty_s[3] = 500u;
    }
    runs[0].end_reason     = KILN_END_FAULT;
    runs[0].band_duty_s[3] = 50000u;

    kiln_insulation_baseline_t bl;
    CHECK_OK(kiln_runstate_baseline(runs, 4, 2, &bl));
    CHECK(bl.valid[3]);
    CHECK_EQ_UINT(bl.duty_s[3], 500u);
}

KILN_TEST(the_baseline_validates_its_arguments)
{
    kiln_insulation_baseline_t bl;
    CHECK_ERR(kiln_runstate_baseline(NULL, 0, 1, &bl), KILN_ERR_NOT_FOUND);
    CHECK_ERR(kiln_runstate_baseline(NULL, 0, 1, NULL), KILN_ERR_INVALID_ARG);

    kiln_run_record_t r;
    kiln_runstate_record_init(&r, 1);
    CHECK_ERR(kiln_runstate_baseline(&r, 0, 1, &bl), KILN_ERR_NOT_FOUND);
}

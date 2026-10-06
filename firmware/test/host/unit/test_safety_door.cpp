/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * SR-31, the door / lid interlock.
 *
 * The rule has two tiers and they are tested separately, because conflating
 * them is exactly how this requirement gets implemented wrongly: heat comes off
 * on the *first* open sample with no delay, and the fault latches only after
 * the confirmation window.  A test that only checks the latch would pass against
 * an implementation that waited 200 ms before dropping the heater, which is not
 * what "immediately" means for a door.
 */

#include "kiln_check.h"
#include "kiln_core/faults.h"
#include "kiln_core/safety.h"

static kiln_safety_cfg_t cfg(void)
{
    kiln_safety_cfg_t c;
    kiln_safety_cfg_defaults(&c);
    return c;
}

static kiln_safety_input_t base(void)
{
    kiln_safety_input_t in = {};
    in.kiln_c          = 500.0f;
    in.case_c          = 30.0f;
    in.setpoint_c      = 500.0f;
    in.rate_c_per_h    = 100.0f;
    in.kiln_valid      = true;
    in.case_valid      = true;
    in.case_present    = true;
    in.heating_active  = true;
    in.door_monitoring = true;
    in.door_open       = false;
    return in;
}

/* --- tier one: immediate, unconditional ------------------------------- */

KILN_TEST(sr31_open_withholds_heat_on_the_very_first_sample)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);

    kiln_safety_input_t in = base();
    CHECK(kiln_safety_eval(&s, &in, 0.1f).heat_permitted);

    in.door_open = true;
    const kiln_safety_verdict_t v = kiln_safety_eval(&s, &in, 0.1f);
    CHECK_MSG(!v.heat_permitted, "heat must be withheld on the first open sample");
}

KILN_TEST(sr31_open_drops_the_contactor_not_merely_the_duty)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);

    kiln_safety_input_t in = base();
    in.door_open = true;
    const kiln_safety_verdict_t v = kiln_safety_eval(&s, &in, 0.1f);
    CHECK_MSG(v.drop_contactor,
              "the second interrupting device must open too, not just the SSR");
}

/* --- tier two: the latch ---------------------------------------------- */

KILN_TEST(sr31_latches_once_the_door_has_been_open_for_the_confirm_window)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);

    kiln_safety_input_t in = base();
    in.door_open = true;

    /* Default window is 200 ms; the first 100 ms cycle must not latch yet. */
    CHECK_EQ_INT(kiln_safety_eval(&s, &in, 0.1f).fault, KILN_FAULT_NONE);
    CHECK_EQ_INT(kiln_safety_eval(&s, &in, 0.1f).fault, KILN_FAULT_DOOR_OPEN);
}

KILN_TEST(sr31_a_single_glitched_sample_does_not_stop_a_healthy_firing)
{
    /* HZ-10: a rule that stops a healthy firing is worse than no rule, because
     * it gets switched off.  One sample of switching noise must cost a fraction
     * of a second of duty and nothing else. */
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);

    kiln_safety_input_t in = base();
    for (int i = 0; i < 20; i++) {
        in.door_open = (i == 7);           /* one bad sample */
        const kiln_safety_verdict_t v = kiln_safety_eval(&s, &in, 0.1f);
        CHECK_MSG(v.fault == KILN_FAULT_NONE, "a single sample must not latch");
        if (i == 7) {
            CHECK_MSG(!v.heat_permitted, "...but heat still comes off for it");
        }
    }
}

KILN_TEST(sr31_the_confirm_timer_resets_when_the_door_shuts)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);

    kiln_safety_input_t in = base();
    in.door_open = true;
    CHECK_EQ_INT(kiln_safety_eval(&s, &in, 0.1f).fault, KILN_FAULT_NONE);
    in.door_open = false;
    CHECK_EQ_INT(kiln_safety_eval(&s, &in, 0.1f).fault, KILN_FAULT_NONE);
    in.door_open = true;
    CHECK_MSG(kiln_safety_eval(&s, &in, 0.1f).fault == KILN_FAULT_NONE,
              "the window must start again, not resume where it left off");
}

KILN_TEST(sr31_open_while_idle_inhibits_but_does_not_latch)
{
    /* Opening the door of an idle kiln is what loading one looks like. */
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);

    kiln_safety_input_t in = base();
    in.heating_active = false;
    in.door_open      = true;

    for (int i = 0; i < 50; i++) {
        const kiln_safety_verdict_t v = kiln_safety_eval(&s, &in, 0.1f);
        CHECK_EQ_INT(v.fault, KILN_FAULT_NONE);
        CHECK_MSG(!v.heat_permitted, "heat stays withheld even when idle");
    }
}

/* --- not fitted -------------------------------------------------------- */

KILN_TEST(sr31_no_interlock_fitted_stands_the_rule_down_and_warns)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);

    kiln_safety_input_t in = base();
    in.door_monitoring = false;
    in.door_open       = true;        /* a floating pin must not stop the kiln */

    const kiln_safety_verdict_t v = kiln_safety_eval(&s, &in, 0.1f);
    CHECK_MSG(v.heat_permitted, "an absent interlock must not withhold heat");
    CHECK_EQ_INT(v.fault, KILN_FAULT_NONE);
    CHECK_MSG((v.warnings & KILN_WARN_BIT(KILN_WARN_DOOR_OFF)) != 0u,
              "warning 113 must say the interlock is missing");
}

KILN_TEST(sr31_a_fitted_and_shut_door_raises_no_warning)
{
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);

    kiln_safety_input_t in = base();
    const kiln_safety_verdict_t v = kiln_safety_eval(&s, &in, 0.1f);
    CHECK((v.warnings & KILN_WARN_BIT(KILN_WARN_DOOR_OFF)) == 0u);
}

/* --- SR-18: clearing --------------------------------------------------- */

KILN_TEST(sr31_cannot_be_cleared_while_the_door_is_still_open)
{
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_input_t in = base();
    in.door_open = true;
    CHECK_MSG(!kiln_safety_can_clear(&c, KILN_FAULT_DOOR_OPEN, &in),
              "SR-18: refuse while the triggering condition still holds");
}

KILN_TEST(sr31_clearable_once_the_door_is_shut)
{
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_input_t in = base();
    in.door_open = false;
    CHECK(kiln_safety_can_clear(&c, KILN_FAULT_DOOR_OPEN, &in));
}

/* --- ordering ----------------------------------------------------------- */

KILN_TEST(sr31_is_decided_before_the_deadline_rules)
{
    /* A door switch is a direct physical signal; it does not depend on the loop
     * having met its deadline.  With both conditions true the door wins, because
     * it is the one the operator is standing in front of. */
    kiln_safety_t s;
    const kiln_safety_cfg_t c = cfg();
    kiln_safety_init(&s, &c);

    kiln_safety_input_t in = base();
    in.door_open             = true;
    in.safety_deadline_missed = true;

    (void)kiln_safety_eval(&s, &in, 0.1f);
    const kiln_safety_verdict_t v = kiln_safety_eval(&s, &in, 0.1f);
    CHECK_EQ_INT(v.fault, KILN_FAULT_DOOR_OPEN);
    CHECK(!v.heat_permitted);
}

KILN_TEST(sr31_fault_has_a_label_a_cause_and_a_requirement)
{
    /* SR-19: every fault is presentable and documented. */
    CHECK_STR_EQ(kiln_fault_label(KILN_FAULT_DOOR_OPEN), "DOOR OPEN");
    CHECK(kiln_fault_cause(KILN_FAULT_DOOR_OPEN)[0] != '\0');
    CHECK_STR_EQ(kiln_fault_requirement(KILN_FAULT_DOOR_OPEN), "SR-31");
}

/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The buzzer's two rhythms -- SWR-SAF-20, SWR-RUN-06, SYS-HW-09.
 *
 * SWR-SAF-20's requirement is that fault and completion are *audibly
 * distinguishable*, which is a property of numbers and can therefore be
 * asserted rather than described. Until the pattern moved into the core it
 * lived in an esp_timer callback where nothing could reach it, and "the two
 * sound different" was a comment.
 */

#include "kiln_check.h"
#include "kiln_core/alarmptn.h"

namespace {

/* Walk a pattern for one full cycle and report what was heard. */
typedef struct {
    unsigned chirps;      /* on-intervals                     */
    uint32_t on_ms;
    uint32_t total_ms;
    uint32_t longest_gap; /* the silence that separates bursts */
    uint32_t shortest_on;
} heard_t;

heard_t listen(kiln_alarm_pattern_t p, unsigned max_steps)
{
    heard_t h = {0, 0, 0, 0, 0xFFFFFFFFu};
    kiln_alarm_seq_t s;
    kiln_alarm_seq_begin(&s, p);

    const uint32_t cycle = kiln_alarm_cycle_ms(p);
    for (unsigned i = 0; i < max_steps && h.total_ms < cycle; i++) {
        const kiln_alarm_step_t step = kiln_alarm_seq_next(&s);
        if (step.hold_ms == 0u) {
            break;
        }
        h.total_ms += step.hold_ms;
        if (step.on) {
            h.chirps++;
            h.on_ms += step.hold_ms;
            if (step.hold_ms < h.shortest_on) { h.shortest_on = step.hold_ms; }
        }
        else if (step.hold_ms > h.longest_gap) {
            h.longest_gap = step.hold_ms;
        }
    }
    return h;
}

} // namespace

/*
 * @relation(SWR-RUN-06, scope=function)
 */
KILN_TEST(swr_run_06_the_finished_firing_is_brief_and_over)
{
    const heard_t h = listen(KILN_ALARM_COMPLETE, 64);

    /* Three chirps, and the whole burst inside half a second: a firing that
     * took nine hours is finished, which is a notification and not an alarm. */
    CHECK_EQ_UINT(h.chirps, 3u);
    CHECK_EQ_UINT(h.on_ms, 270u);
    CHECK(h.on_ms + 180u <= 500u);          /* burst length, chirps and gaps  */

    /* And then it gets out of the way for five seconds, which is what makes it
     * brief in practice: the application turns the alarm off after
     * hmi.alarm_duration_s, and the default is shorter than this gap, so the
     * operator hears the burst once. */
    CHECK(h.longest_gap >= 4000u);
    CHECK_EQ_UINT(kiln_alarm_cycle_ms(KILN_ALARM_COMPLETE), 5000u);
}

/*
 * @relation(SWR-SAF-20, scope=function)
 */
KILN_TEST(swr_saf_20_a_safety_incident_is_announced_aggressively)
{
    const heard_t h = listen(KILN_ALARM_FAULT, 64);

    /* Six fast chirps and a short gap. Something is wrong with a kiln at
     * 1 200 degC: this is meant to be unpleasant and to carry through a wall. */
    CHECK_EQ_UINT(h.chirps, 6u);
    CHECK(h.longest_gap <= 500u);

    /* Chirping for a third of the time (360 ms of every 1 060), against a
     * twentieth for completion (270 ms of every 5 000). That ratio is what
     * "audibly distinguishable" means with an active buzzer that has one tone
     * and no volume control. */
    const uint32_t cycle = kiln_alarm_cycle_ms(KILN_ALARM_FAULT);
    CHECK_EQ_UINT(cycle, 1060u);
    CHECK_EQ_UINT(h.on_ms, 360u);
    CHECK(h.on_ms * 3u >= cycle);
}

/*
 * @relation(SWR-SAF-20, scope=function)
 */
KILN_TEST(swr_saf_20_the_two_patterns_cannot_be_confused)
{
    /* The requirement is a comparison, so it is asserted as one rather than as
     * two sets of numbers that happen to differ. */
    const uint32_t fault_cycle    = kiln_alarm_cycle_ms(KILN_ALARM_FAULT);
    const uint32_t complete_cycle = kiln_alarm_cycle_ms(KILN_ALARM_COMPLETE);

    /* Duty: the fault sounds for a third of its cycle, completion for a
     * twentieth of a cycle five times as long. */
    const uint32_t fault_duty    = kiln_alarm_on_ms(KILN_ALARM_FAULT) * 100u / fault_cycle;
    const uint32_t complete_duty = kiln_alarm_on_ms(KILN_ALARM_COMPLETE) * 100u / complete_cycle;
    CHECK(fault_duty >= 4u * complete_duty);

    /* Rate: the fault repeats several times a second, completion once every
     * few seconds. Both the rhythm and the repetition differ, so neither a
     * noisy workshop nor a closed door collapses one into the other. */
    CHECK(complete_cycle >= 4u * fault_cycle);

    /* And the chirps themselves are different lengths. */
    const heard_t f = listen(KILN_ALARM_FAULT, 64);
    const heard_t c = listen(KILN_ALARM_COMPLETE, 64);
    CHECK(f.shortest_on < c.shortest_on);
}

/*
 * @relation(SWR-SAF-20, scope=function)
 */
KILN_TEST(swr_saf_20_the_fault_pattern_never_stops_on_its_own)
{
    /* A fault is cleared by an acknowledgement, never by the buzzer giving up.
     * Ten cycles' worth of steps, and every one of them is still due. */
    kiln_alarm_seq_t s;
    kiln_alarm_seq_begin(&s, KILN_ALARM_FAULT);
    uint32_t sounded = 0;
    for (unsigned i = 0; i < 200u; i++) {
        const kiln_alarm_step_t step = kiln_alarm_seq_next(&s);
        CHECK(step.hold_ms > 0u);
        if (step.on) { sounded += step.hold_ms; }
    }
    /* Not merely "a step was returned": it was still making noise. */
    CHECK(sounded > 5000u);
}

/*
 * @relation(SWR-SAF-20, scope=function)
 */
KILN_TEST(swr_saf_20_off_is_silent_and_asks_for_nothing_further)
{
    kiln_alarm_seq_t s;
    kiln_alarm_seq_begin(&s, KILN_ALARM_OFF);
    const kiln_alarm_step_t step = kiln_alarm_seq_next(&s);
    CHECK_EQ_INT(step.on, false);
    CHECK_EQ_UINT(step.hold_ms, 0u);
    CHECK_EQ_UINT(kiln_alarm_cycle_ms(KILN_ALARM_OFF), 0u);

    /* Asked again, it says the same thing: an adapter that keeps calling must
     * not eventually be told to sound. */
    for (unsigned i = 0; i < 8u; i++) {
        const kiln_alarm_step_t again = kiln_alarm_seq_next(&s);
        CHECK_EQ_INT(again.on, false);
        CHECK_EQ_UINT(again.hold_ms, 0u);
    }
}

/*
 * @relation(SWR-SAF-20, scope=function)
 */
KILN_TEST(swr_saf_20_a_buzzer_that_cannot_be_commanded_stays_quiet)
{
    /* A null sequence, and a pattern value that is not one of the three: both
     * answer silence. The buzzer's failure direction is off -- a part that
     * cannot be commanded must not be a part that is stuck on, because the
     * operator's only remedy for that is the mains switch. */
    const kiln_alarm_step_t none = kiln_alarm_seq_next(nullptr);
    CHECK_EQ_INT(none.on, false);
    CHECK_EQ_UINT(none.hold_ms, 0u);
    kiln_alarm_seq_begin(nullptr, KILN_ALARM_FAULT);   /* must not fault */

    kiln_alarm_seq_t s;
    kiln_alarm_seq_begin(&s, (kiln_alarm_pattern_t)99);
    const kiln_alarm_step_t bogus = kiln_alarm_seq_next(&s);
    CHECK_EQ_INT(bogus.on, false);
    CHECK_EQ_UINT(bogus.hold_ms, 0u);
}

/*
 * @relation(SWR-SAF-20, scope=function)
 */
KILN_TEST(swr_saf_20_a_pattern_resumes_rather_than_restarting_mid_cycle)
{
    /* The adapter refuses to re-begin the pattern that is already running, so
     * that a status update ten times a second cannot hold the buzzer at its
     * first interval forever. What that relies on from here is that stepping
     * is the only thing that advances the sequence. */
    kiln_alarm_seq_t s;
    kiln_alarm_seq_begin(&s, KILN_ALARM_FAULT);
    const kiln_alarm_step_t first  = kiln_alarm_seq_next(&s);
    const kiln_alarm_step_t second = kiln_alarm_seq_next(&s);
    CHECK_EQ_INT(first.on, true);
    CHECK_EQ_INT(second.on, false);

    kiln_alarm_seq_begin(&s, KILN_ALARM_FAULT);
    const kiln_alarm_step_t again = kiln_alarm_seq_next(&s);
    CHECK_EQ_INT(again.on, true);      /* begun again, so back to the start */
}

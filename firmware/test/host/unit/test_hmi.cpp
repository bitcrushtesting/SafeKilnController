/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * kiln_hmi: screens, menu paths and the confirmation flows
 * (FR-HMI-02..FR-HMI-15).
 *
 * These run with no display, no application and no kiln, which is the reason
 * kiln_hmi returns an action rather than calling kiln_app: a menu path is then
 * a pure function of (view, event) and a test can walk it.
 *
 * Rendering is checked by counting lit pixels in a region rather than by
 * comparing whole golden images.  A golden image fails on every deliberate
 * layout change and tells you nothing about why; "the big number occupies the
 * top left" survives a nudge and still catches the thing that matters.
 */
#include <string.h>

#include "kiln_check.h"
#include "kiln_core/faults.h"
#include "kiln_hmi/hmi.h"

static kiln_hmi_view_t base_view(void)
{
    kiln_hmi_view_t v = {};
    v.snap.kiln_c       = 523.0f;
    v.snap.setpoint_c   = 530.0f;
    v.snap.rate_c_per_h = 120.0f;
    v.snap.duty_permille = 450;
    v.snap.state        = (uint8_t)KILN_STATE_RUNNING;
    v.snap.segment_index = 1;
    v.snap.segment_count = 5;
    v.snap.kiln_valid   = true;
    v.snap.case_valid   = true;
    v.fault             = KILN_FAULT_NONE;
    v.language          = KILN_LANG_EN;
    v.elapsed_s         = 3725;
    v.program_count     = 3;
    (void)snprintf(v.program_name[0], KILN_HMI_NAME_LEN, "Bisque 1000");
    (void)snprintf(v.program_name[1], KILN_HMI_NAME_LEN, "Glaze cone 6");
    (void)snprintf(v.program_name[2], KILN_HMI_NAME_LEN, "Test fire");
    (void)snprintf(v.hostname, sizeof(v.hostname), "kiln.local");
    (void)snprintf(v.ip, sizeof(v.ip), "192.168.1.40");
    (void)snprintf(v.version, sizeof(v.version), "v0.1-abc1234");
    return v;
}

/* Lit pixels inside a rectangle, which is how these tests look at the screen. */
static int ink(const kiln_hmi_t *h, int x0, int y0, int w, int hgt)
{
    int n = 0;
    for (int y = y0; y < y0 + hgt && y < KILN_DISPLAY_H; y++) {
        for (int x = x0; x < x0 + w && x < KILN_DISPLAY_W; x++) {
            const uint8_t b = kiln_hmi_frame(h)[(y / 8) * KILN_DISPLAY_W + x];
            if ((b & (1u << ((unsigned)y % 8u))) != 0u) { n++; }
        }
    }
    return n;
}

static kiln_hmi_action_t feed(kiln_hmi_t *h, const kiln_hmi_view_t *v,
                              kiln_input_event_t ev)
{
    return kiln_hmi_update(h, v, ev, 100);
}

/* --- the default screen -------------------------------------------------- */

KILN_TEST(frhmi03_the_chamber_temperature_is_the_largest_thing_on_the_screen)
{
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    const kiln_hmi_view_t v = base_view();
    (void)feed(&h, &v, KILN_INPUT_NONE);

    /* The big number lives in the top-left 64x24; everything else on the
     * screen is the small font.  If the large render ever silently fell back
     * to scale 1 this count would collapse. */
    const int big   = ink(&h, 0, 0, 64, 24);
    const int small = ink(&h, 0, 28, 128, 14);
    CHECK_MSG(big > small, "big font region has %d lit pixels, small has %d", big, small);
    CHECK_MSG(big > 150, "the large temperature looks too small: %d pixels", big);
}

KILN_TEST(frhmi02_current_and_target_are_both_present)
{
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    kiln_hmi_view_t v = base_view();
    (void)feed(&h, &v, KILN_INPUT_NONE);
    const int with_sp = ink(&h, 64, 0, 64, 10);

    /* Move only the setpoint: the top-right region must change. */
    v.snap.setpoint_c = 880.0f;
    (void)feed(&h, &v, KILN_INPUT_NONE);
    CHECK_MSG(ink(&h, 64, 0, 64, 10) != with_sp,
              "the target is not rendered, or not where it was expected");
}

KILN_TEST(fracq12_an_invalid_reading_is_not_shown_as_a_temperature)
{
    /* Inside the grace window there is no measurement.  Showing the last one
     * is how an operator comes to trust a number the firmware has disowned. */
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    kiln_hmi_view_t v = base_view();
    v.snap.kiln_valid = false;
    (void)feed(&h, &v, KILN_INPUT_NONE);

    const int dashes = ink(&h, 0, 0, 64, 24);
    v.snap.kiln_valid = true;
    (void)feed(&h, &v, KILN_INPUT_NONE);
    CHECK_MSG(ink(&h, 0, 0, 64, 24) != dashes,
              "an invalid reading renders the same as a valid one");
}

/* --- fault precedence ---------------------------------------------------- */

KILN_TEST(frhmi06_a_fault_takes_the_screen_from_any_other)
{
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    kiln_hmi_view_t v = base_view();

    (void)feed(&h, &v, KILN_INPUT_PRESS);            /* into the menu */
    CHECK_EQ_INT(h.screen, KILN_HMI_SCREEN_MENU);

    v.fault = KILN_FAULT_DOOR_OPEN;
    (void)feed(&h, &v, KILN_INPUT_NONE);
    CHECK_MSG(h.screen == KILN_HMI_SCREEN_FAULT,
              "a fault must take the screen even mid-menu");
}

KILN_TEST(frhmi06_the_fault_screen_cannot_be_dismissed_while_it_holds)
{
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    kiln_hmi_view_t v = base_view();
    v.fault = KILN_FAULT_OVERTEMP;

    for (int i = 0; i < 5; i++) {
        (void)feed(&h, &v, KILN_INPUT_PRESS);
        (void)feed(&h, &v, KILN_INPUT_LONG_PRESS);
        (void)feed(&h, &v, KILN_INPUT_CW);
        CHECK_EQ_INT(h.screen, KILN_HMI_SCREEN_FAULT);
    }

    /* It leaves of its own accord once the fault has actually gone. */
    v.fault = KILN_FAULT_NONE;
    (void)feed(&h, &v, KILN_INPUT_NONE);
    CHECK_EQ_INT(h.screen, KILN_HMI_SCREEN_MAIN);
}

KILN_TEST(frhmi10_a_press_on_the_fault_screen_asks_to_acknowledge)
{
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    kiln_hmi_view_t v = base_view();
    v.fault = KILN_FAULT_TC_OPEN;
    (void)feed(&h, &v, KILN_INPUT_NONE);

    const kiln_hmi_action_t a = feed(&h, &v, KILN_INPUT_PRESS);
    CHECK_EQ_INT(a.kind, KILN_HMI_ACT_ACK_FAULT);
    /* Asking is all the HMI does: SR-18 decides, in kiln_app. */
    CHECK_EQ_INT(h.screen, KILN_HMI_SCREEN_FAULT);
}

KILN_TEST(frhmi06_the_fault_cause_is_rendered_in_the_configured_language)
{
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    kiln_hmi_view_t v = base_view();
    v.fault = KILN_FAULT_DOOR_OPEN;

    v.language = KILN_LANG_EN;
    (void)feed(&h, &v, KILN_INPUT_NONE);
    const int en = ink(&h, 0, 14, 128, 40);

    v.language = KILN_LANG_DE;
    (void)feed(&h, &v, KILN_INPUT_NONE);
    CHECK_MSG(ink(&h, 0, 14, 128, 40) != en,
              "FR-HMI-15: the cause did not change with the language");
}

/* --- starting a firing, which is the path that matters ------------------- */

KILN_TEST(frhmi10_a_program_can_be_started_from_the_local_input_alone)
{
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    kiln_hmi_view_t v = base_view();
    v.snap.state = (uint8_t)KILN_STATE_IDLE;

    (void)feed(&h, &v, KILN_INPUT_PRESS);                /* main -> menu   */
    CHECK_EQ_INT(h.screen, KILN_HMI_SCREEN_MENU);
    CHECK_EQ_INT(h.menu_sel, 0);                         /* Start program  */

    (void)feed(&h, &v, KILN_INPUT_PRESS);                /* -> programs    */
    CHECK_EQ_INT(h.screen, KILN_HMI_SCREEN_PROGRAMS);

    (void)feed(&h, &v, KILN_INPUT_CW);                   /* pick the second */
    CHECK_EQ_INT(h.prog_sel, 1);

    (void)feed(&h, &v, KILN_INPUT_PRESS);                /* -> confirm      */
    CHECK_EQ_INT(h.screen, KILN_HMI_SCREEN_CONFIRM);

    (void)feed(&h, &v, KILN_INPUT_CW);                   /* NO -> YES       */
    const kiln_hmi_action_t a = feed(&h, &v, KILN_INPUT_PRESS);
    CHECK_EQ_INT(a.kind, KILN_HMI_ACT_START);
    CHECK_EQ_UINT(a.program_index, 1u);
    CHECK_EQ_INT(h.screen, KILN_HMI_SCREEN_MAIN);
}

KILN_TEST(frhmi11_starting_defaults_to_no_and_a_press_alone_does_nothing)
{
    /* The confirmation is only worth having if the lazy answer is the safe
     * one.  A press straight through must not start a kiln. */
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    kiln_hmi_view_t v = base_view();
    v.snap.state = (uint8_t)KILN_STATE_IDLE;

    (void)feed(&h, &v, KILN_INPUT_PRESS);
    (void)feed(&h, &v, KILN_INPUT_PRESS);
    (void)feed(&h, &v, KILN_INPUT_PRESS);
    CHECK_EQ_INT(h.screen, KILN_HMI_SCREEN_CONFIRM);
    CHECK_MSG(!h.confirm_yes, "the confirmation must default to NO");

    const kiln_hmi_action_t a = feed(&h, &v, KILN_INPUT_PRESS);
    CHECK_EQ_INT(a.kind, KILN_HMI_ACT_NONE);
}

KILN_TEST(frhmi11_aborting_also_confirms)
{
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    const kiln_hmi_view_t v = base_view();

    (void)feed(&h, &v, KILN_INPUT_PRESS);        /* menu        */
    (void)feed(&h, &v, KILN_INPUT_CW);
    (void)feed(&h, &v, KILN_INPUT_CW);           /* Abort       */
    CHECK_EQ_INT(h.menu_sel, 2);
    (void)feed(&h, &v, KILN_INPUT_PRESS);
    CHECK_EQ_INT(h.screen, KILN_HMI_SCREEN_CONFIRM);

    (void)feed(&h, &v, KILN_INPUT_CW);           /* YES         */
    const kiln_hmi_action_t a = feed(&h, &v, KILN_INPUT_PRESS);
    CHECK_EQ_INT(a.kind, KILN_HMI_ACT_ABORT);
}

KILN_TEST(frhmi10_pause_and_resume_follow_the_run_state)
{
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    kiln_hmi_view_t v = base_view();

    (void)feed(&h, &v, KILN_INPUT_PRESS);
    (void)feed(&h, &v, KILN_INPUT_CW);
    CHECK_EQ_INT(h.menu_sel, 1);
    kiln_hmi_action_t a = feed(&h, &v, KILN_INPUT_PRESS);
    CHECK_EQ_INT(a.kind, KILN_HMI_ACT_PAUSE);

    v.snap.state = (uint8_t)KILN_STATE_PAUSED;
    (void)feed(&h, &v, KILN_INPUT_PRESS);
    (void)feed(&h, &v, KILN_INPUT_CW);
    a = feed(&h, &v, KILN_INPUT_PRESS);
    CHECK_EQ_INT(a.kind, KILN_HMI_ACT_RESUME);
}

KILN_TEST(a_long_press_backs_out_without_doing_anything)
{
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    const kiln_hmi_view_t v = base_view();

    (void)feed(&h, &v, KILN_INPUT_PRESS);
    (void)feed(&h, &v, KILN_INPUT_PRESS);            /* programs */
    CHECK_EQ_INT(h.screen, KILN_HMI_SCREEN_PROGRAMS);
    const kiln_hmi_action_t a = feed(&h, &v, KILN_INPUT_LONG_PRESS);
    CHECK_EQ_INT(a.kind, KILN_HMI_ACT_NONE);
    CHECK_EQ_INT(h.screen, KILN_HMI_SCREEN_MENU);
}

KILN_TEST(an_empty_program_store_cannot_start_anything)
{
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    kiln_hmi_view_t v = base_view();
    v.program_count = 0;

    (void)feed(&h, &v, KILN_INPUT_PRESS);
    (void)feed(&h, &v, KILN_INPUT_PRESS);
    CHECK_EQ_INT(h.screen, KILN_HMI_SCREEN_PROGRAMS);
    const kiln_hmi_action_t a = feed(&h, &v, KILN_INPUT_PRESS);
    CHECK_EQ_INT(a.kind, KILN_HMI_ACT_NONE);
    CHECK_MSG(ink(&h, 0, 0, 128, 64) > 0, "the empty list should still say so");
}

/* --- the other screens --------------------------------------------------- */

KILN_TEST(frhmi07_the_network_screen_shows_where_the_web_interface_is)
{
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    kiln_hmi_view_t v = base_view();
    v.net_up = true;

    (void)feed(&h, &v, KILN_INPUT_PRESS);
    for (int i = 0; i < 3; i++) { (void)feed(&h, &v, KILN_INPUT_CW); }
    CHECK_EQ_INT(h.menu_sel, 3);
    (void)feed(&h, &v, KILN_INPUT_PRESS);
    CHECK_EQ_INT(h.screen, KILN_HMI_SCREEN_NETWORK);

    /* The address occupies the rows it is drawn on. */
    CHECK(ink(&h, 0, 24, 128, 24) > 0);
}

KILN_TEST(frhmi08_diagnostics_and_info_render)
{
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    kiln_hmi_view_t v = base_view();
    v.power_w = 5400.0; v.energy_wh = 12800.0;
    v.kp = 6.0f; v.ki = 0.02f; v.kd = 30.0f;

    (void)feed(&h, &v, KILN_INPUT_PRESS);
    for (int i = 0; i < 4; i++) { (void)feed(&h, &v, KILN_INPUT_CW); }
    (void)feed(&h, &v, KILN_INPUT_PRESS);
    CHECK_EQ_INT(h.screen, KILN_HMI_SCREEN_DIAG);
    CHECK(ink(&h, 0, 10, 128, 54) > 0);

    (void)feed(&h, &v, KILN_INPUT_PRESS);           /* back to the menu */
    (void)feed(&h, &v, KILN_INPUT_CW);
    (void)feed(&h, &v, KILN_INPUT_PRESS);
    CHECK_EQ_INT(h.screen, KILN_HMI_SCREEN_INFO);
    CHECK(ink(&h, 0, 10, 128, 54) > 0);
}

/* --- FR-HMI-12 ----------------------------------------------------------- */

KILN_TEST(frhmi12_the_display_dims_when_idle_and_wakes_on_input)
{
    kiln_hmi_t h; kiln_hmi_init(&h, 10);      /* 10 s */
    const kiln_hmi_view_t v = base_view();

    for (int i = 0; i < 99; i++) { (void)kiln_hmi_update(&h, &v, KILN_INPUT_NONE, 100); }
    CHECK_MSG(!kiln_hmi_dimmed(&h), "dimmed early");
    (void)kiln_hmi_update(&h, &v, KILN_INPUT_NONE, 100);
    CHECK_MSG(kiln_hmi_dimmed(&h), "did not dim after the timeout");
    CHECK_EQ_INT(ink(&h, 0, 0, 128, 64), 0);

    (void)feed(&h, &v, KILN_INPUT_CW);
    CHECK_MSG(!kiln_hmi_dimmed(&h), "input did not wake the display");
    CHECK(ink(&h, 0, 0, 128, 64) > 0);
}

KILN_TEST(frhmi12_a_fault_suspends_the_dim_timeout)
{
    /* A kiln that blanked its own fault screen would be worse than one with
     * no screen at all. */
    kiln_hmi_t h; kiln_hmi_init(&h, 10);
    kiln_hmi_view_t v = base_view();
    v.fault = KILN_FAULT_RUNAWAY;

    for (int i = 0; i < 300; i++) { (void)kiln_hmi_update(&h, &v, KILN_INPUT_NONE, 100); }
    CHECK_MSG(!kiln_hmi_dimmed(&h), "the fault screen was allowed to blank");
    CHECK(ink(&h, 0, 0, 128, 64) > 0);
}

KILN_TEST(frhmi12_a_zero_timeout_never_dims)
{
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    const kiln_hmi_view_t v = base_view();
    for (int i = 0; i < 1000; i++) { (void)kiln_hmi_update(&h, &v, KILN_INPUT_NONE, 100); }
    CHECK(!kiln_hmi_dimmed(&h));
}

/* --- FR-HMI-13 ----------------------------------------------------------- */

KILN_TEST(frhmi13_fahrenheit_is_a_display_conversion_only)
{
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    kiln_hmi_view_t v = base_view();
    (void)feed(&h, &v, KILN_INPUT_NONE);
    const int c = ink(&h, 0, 0, 80, 24);

    v.fahrenheit = true;
    (void)feed(&h, &v, KILN_INPUT_NONE);
    CHECK_MSG(ink(&h, 0, 0, 80, 24) != c, "degF rendered identically to degC");
    /* The snapshot it was handed is untouched: the conversion is in the
     * rendering and nowhere else. */
    CHECK_NEAR(v.snap.kiln_c, 523.0f, 0.001f);
}

/* --- robustness ---------------------------------------------------------- */

KILN_TEST(the_hmi_validates_its_own_arguments)
{
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    const kiln_hmi_view_t v = base_view();
    CHECK_EQ_INT(kiln_hmi_update(NULL, &v, KILN_INPUT_PRESS, 100).kind, KILN_HMI_ACT_NONE);
    CHECK_EQ_INT(kiln_hmi_update(&h, NULL, KILN_INPUT_PRESS, 100).kind, KILN_HMI_ACT_NONE);
}

KILN_TEST(drawing_clips_instead_of_running_off_the_buffer)
{
    /* This code shares an MCU with the safety supervisor.  A screen that draws
     * off the edge must lose a pixel, not corrupt memory. */
    kiln_fb_t fb;
    kiln_fb_clear(&fb);
    kiln_fb_pixel(&fb, -5, -5, true);
    kiln_fb_pixel(&fb, 999, 999, true);
    kiln_fb_text(&fb, 120, 60, "overflowing text", 3, true);
    kiln_fb_fill(&fb, -20, -20, 400, 400, true);
    kiln_fb_progress(&fb, -10, 60, 300, 20, 250);
    CHECK(true);   /* reaching here without ASan complaining is the assertion */
}

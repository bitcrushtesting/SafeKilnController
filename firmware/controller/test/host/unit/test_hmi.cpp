/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * kiln_hmi: screens, menu paths and the confirmation flows
 * (SWR-HMI-02..SWR-HMI-15).
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
#include "kiln_hmi/strings.h"

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

/*
 * @relation(SWR-HMI-03, scope=function)
 */
KILN_TEST(swrhmi03_the_chamber_temperature_is_the_largest_thing_on_the_screen)
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

/*
 * @relation(SWR-HMI-02, scope=function)
 */
KILN_TEST(swrhmi02_current_and_target_are_both_present)
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

/*
 * @relation(SWR-ACQ-12, scope=function)
 */
KILN_TEST(swracq12_an_invalid_reading_is_not_shown_as_a_temperature)
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

/*
 * @relation(SWR-HMI-06, scope=function)
 */
KILN_TEST(swrhmi06_a_fault_takes_the_screen_from_any_other)
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

/*
 * @relation(SWR-HMI-06, scope=function)
 */
KILN_TEST(swrhmi06_the_fault_screen_cannot_be_dismissed_while_it_holds)
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

/*
 * @relation(SWR-HMI-10, scope=function)
 */
KILN_TEST(swrhmi10_a_press_on_the_fault_screen_asks_to_acknowledge)
{
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    kiln_hmi_view_t v = base_view();
    v.fault = KILN_FAULT_TC_OPEN;
    (void)feed(&h, &v, KILN_INPUT_NONE);

    const kiln_hmi_action_t a = feed(&h, &v, KILN_INPUT_PRESS);
    CHECK_EQ_INT(a.kind, KILN_HMI_ACT_ACK_FAULT);
    /* Asking is all the HMI does: SWR-SAF-18 decides, in kiln_app. */
    CHECK_EQ_INT(h.screen, KILN_HMI_SCREEN_FAULT);
}

/*
 * @relation(SWR-HMI-06, scope=function)
 */
KILN_TEST(swrhmi06_the_fault_cause_is_rendered_in_the_configured_language)
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
              "SWR-HMI-15: the cause did not change with the language");
}

/* --- starting a firing, which is the path that matters ------------------- */

/*
 * @relation(SWR-HMI-10, scope=function)
 */
KILN_TEST(swrhmi10_a_program_can_be_started_from_the_local_input_alone)
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

/*
 * @relation(SWR-HMI-11, scope=function)
 */
KILN_TEST(swrhmi11_starting_defaults_to_no_and_a_press_alone_does_nothing)
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

/*
 * @relation(SWR-HMI-11, scope=function)
 */
KILN_TEST(swrhmi11_aborting_also_confirms)
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

/*
 * @relation(SWR-HMI-10, scope=function)
 */
KILN_TEST(swrhmi10_pause_and_resume_follow_the_run_state)
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

/*
 * @relation(SWR-HMI-07, scope=function)
 */
KILN_TEST(swrhmi07_the_network_screen_shows_where_the_web_interface_is)
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

/*
 * @relation(SWR-HMI-08, scope=function)
 */
KILN_TEST(swrhmi08_diagnostics_and_info_render)
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

/* --- SWR-HMI-12 ----------------------------------------------------------- */

/*
 * @relation(SWR-HMI-12, scope=function)
 */
KILN_TEST(swrhmi12_the_display_dims_when_idle_and_wakes_on_input)
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

/*
 * @relation(SWR-HMI-12, scope=function)
 */
KILN_TEST(swrhmi12_a_fault_suspends_the_dim_timeout)
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

/*
 * @relation(SWR-HMI-12, scope=function)
 */
KILN_TEST(swrhmi12_a_zero_timeout_never_dims)
{
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    const kiln_hmi_view_t v = base_view();
    for (int i = 0; i < 1000; i++) { (void)kiln_hmi_update(&h, &v, KILN_INPUT_NONE, 100); }
    CHECK(!kiln_hmi_dimmed(&h));
}

/* --- SWR-HMI-13 ----------------------------------------------------------- */

/*
 * @relation(SWR-HMI-13, scope=function)
 */
KILN_TEST(swrhmi13_fahrenheit_is_a_display_conversion_only)
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

/* --- the supervisor on the fault screen (SWA-22, R12) -------------------- */

/*
 * @relation(SWA-22, scope=function)
 */
KILN_TEST(swa22_the_fault_screen_differs_when_the_supervisor_tripped)
{
    /* The two latches are cleared differently -- the supervisor's by the button
     * on the panel -- so an operator shown the controller's instruction would
     * press the wrong thing.  No text search here, so the assertion is that the
     * rendered screen is not the same screen. */
    kiln_hmi_view_t v = base_view();
    v.snap.state = KILN_STATE_FAULT;
    v.fault      = KILN_FAULT_OVERTEMP;
    v.sup_fitted = true;

    kiln_hmi_t controller;
    kiln_hmi_init(&controller, 0);
    v.sup_tripped = false;                  /* the supervisor is content */
    v.sup_reason  = KILN_SUP_OK;
    (void)feed(&controller, &v, KILN_INPUT_NONE);

    kiln_hmi_t supervisor;
    kiln_hmi_init(&supervisor, 0);
    v.sup_tripped = true;
    v.sup_reason  = KILN_SUP_OVERTEMP;
    (void)feed(&supervisor, &v, KILN_INPUT_NONE);

    CHECK(memcmp(kiln_hmi_frame(&controller), kiln_hmi_frame(&supervisor),
                 KILN_DISPLAY_BYTES) != 0);
}

/*
 * @relation(SWA-22, scope=function)
 */
KILN_TEST(swa22_a_missing_supervisor_is_shown_differently_from_one_that_tripped)
{
    /* "The backstop fired" and "there is no backstop" are different sentences
     * and must not render the same. */
    kiln_hmi_view_t v = base_view();
    v.snap.state  = KILN_STATE_FAULT;
    v.fault       = KILN_FAULT_OVERTEMP;
    v.sup_fitted  = true;
    v.sup_tripped = true;

    kiln_hmi_t fired;
    kiln_hmi_init(&fired, 0);
    v.sup_reason = KILN_SUP_OVERTEMP;
    (void)feed(&fired, &v, KILN_INPUT_NONE);

    kiln_hmi_t absent;
    kiln_hmi_init(&absent, 0);
    v.sup_reason = KILN_SUP_LINK_DEAD;
    (void)feed(&absent, &v, KILN_INPUT_NONE);

    CHECK(memcmp(kiln_hmi_frame(&fired), kiln_hmi_frame(&absent),
                 KILN_DISPLAY_BYTES) != 0);
}

/* --- WiFi setup at the display (SWR-NET-11, SWR-NET-12) ------------------ */

static kiln_hmi_view_t net_view(void)
{
    kiln_hmi_view_t v = base_view();
    v.net_count = 3;
    (void)snprintf(v.net_list_ssid[0], KILN_HMI_SSID_LEN, "studio");
    v.net_list_rssi[0]    = -42;
    v.net_list_secured[0] = true;
    (void)snprintf(v.net_list_ssid[1], KILN_HMI_SSID_LEN, "kiln-shed");
    v.net_list_rssi[1]    = -67;
    v.net_list_secured[1] = true;
    (void)snprintf(v.net_list_ssid[2], KILN_HMI_SSID_LEN, "guest-open");
    v.net_list_rssi[2]    = -71;
    v.net_list_secured[2] = false;
    return v;
}

/* Walk to the network screen through the menu, the way an operator does. */
static void to_network(kiln_hmi_t *h, const kiln_hmi_view_t *v)
{
    (void)feed(h, v, KILN_INPUT_PRESS);                 /* main -> menu  */
    for (int i = 0; i < 3; i++) { (void)feed(h, v, KILN_INPUT_CW); }
    (void)feed(h, v, KILN_INPUT_PRESS);                 /* -> network    */
}

/*
 * @relation(SWR-NET-11, scope=function)
 */
KILN_TEST(swrnet11_the_network_screen_is_the_way_into_wifi_setup)
{
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    const kiln_hmi_view_t v = net_view();
    to_network(&h, &v);
    CHECK(h.screen == KILN_HMI_SCREEN_NETWORK);

    /* Pressing asks for a scan and moves to the list.  Without this there is
     * no route to WiFi setup at all, the access point having been removed. */
    const kiln_hmi_action_t a = feed(&h, &v, KILN_INPUT_PRESS);
    CHECK(a.kind == KILN_HMI_ACT_WIFI_SCAN);
    CHECK(h.screen == KILN_HMI_SCREEN_NETWORKS);
}

/*
 * @relation(SWR-NET-11, scope=function)
 */
KILN_TEST(swrnet11_scanning_and_finding_nothing_look_different)
{
    kiln_hmi_view_t v = net_view();
    v.net_count = 0;

    kiln_hmi_t busy; kiln_hmi_init(&busy, 0);
    to_network(&busy, &v);
    v.net_scanning = true;
    (void)feed(&busy, &v, KILN_INPUT_PRESS);

    kiln_hmi_t done; kiln_hmi_init(&done, 0);
    to_network(&done, &v);
    v.net_scanning = false;
    (void)feed(&done, &v, KILN_INPUT_PRESS);

    /* "none found" after two seconds of scanning is a different instruction to
     * the operator than "still looking". */
    CHECK(memcmp(kiln_hmi_frame(&busy), kiln_hmi_frame(&done),
                 KILN_DISPLAY_BYTES) != 0);
}

/*
 * @relation(SWR-NET-12, scope=function)
 */
KILN_TEST(swrnet12_an_open_network_needs_no_passphrase)
{
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    const kiln_hmi_view_t v = net_view();
    to_network(&h, &v);
    (void)feed(&h, &v, KILN_INPUT_PRESS);               /* -> the list   */

    (void)feed(&h, &v, KILN_INPUT_CW);
    (void)feed(&h, &v, KILN_INPUT_CW);                  /* the open one  */
    const kiln_hmi_action_t a = feed(&h, &v, KILN_INPUT_PRESS);

    CHECK(a.kind == KILN_HMI_ACT_WIFI_CONNECT);
    CHECK(a.network_index == 2);
    CHECK_MSG(kiln_hmi_passphrase(&h)[0] == '\0',
              "an open network must not carry a passphrase");
}

/*
 * @relation(SWR-NET-12, scope=function)
 */
KILN_TEST(swrnet12_a_secured_network_asks_for_a_passphrase_and_types_it)
{
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    const kiln_hmi_view_t v = net_view();
    to_network(&h, &v);
    (void)feed(&h, &v, KILN_INPUT_PRESS);               /* -> the list   */
    (void)feed(&h, &v, KILN_INPUT_PRESS);               /* "studio"      */
    CHECK(h.screen == KILN_HMI_SCREEN_PASSPHRASE);

    /* The knob starts on 'a'; eight presses give the shortest legal WPA
     * passphrase, which is the boundary SWR-NET-12 names. */
    for (int i = 0; i < 8; i++) { (void)feed(&h, &v, KILN_INPUT_PRESS); }
    CHECK_MSG(strcmp(kiln_hmi_passphrase(&h), "aaaaaaaa") == 0,
              "typed %s", kiln_hmi_passphrase(&h));

    /* One turn backwards from the first character reaches "connect", which is
     * why delete and connect sit at the end of the set rather than the start. */
    (void)feed(&h, &v, KILN_INPUT_CCW);
    const kiln_hmi_action_t a = feed(&h, &v, KILN_INPUT_PRESS);
    CHECK(a.kind == KILN_HMI_ACT_WIFI_CONNECT);
    CHECK(a.network_index == 0);
    CHECK(h.screen == KILN_HMI_SCREEN_NETWORK);
}

/*
 * @relation(SWR-NET-12, scope=function)
 */
KILN_TEST(swrnet12_a_passphrase_shorter_than_eight_is_not_accepted)
{
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    const kiln_hmi_view_t v = net_view();
    to_network(&h, &v);
    (void)feed(&h, &v, KILN_INPUT_PRESS);
    (void)feed(&h, &v, KILN_INPUT_PRESS);               /* passphrase    */

    for (int i = 0; i < 7; i++) { (void)feed(&h, &v, KILN_INPUT_PRESS); }
    (void)feed(&h, &v, KILN_INPUT_CCW);                 /* "connect"     */
    const kiln_hmi_action_t a = feed(&h, &v, KILN_INPUT_PRESS);

    /* WPA-PSK cannot be seven characters.  Accepting it would spend a join
     * attempt to discover what the standard already says. */
    CHECK(a.kind == KILN_HMI_ACT_NONE);
    CHECK(h.screen == KILN_HMI_SCREEN_PASSPHRASE);
}

/*
 * @relation(SWR-NET-12, scope=function)
 */
KILN_TEST(swrnet12_delete_removes_the_last_character)
{
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    const kiln_hmi_view_t v = net_view();
    to_network(&h, &v);
    (void)feed(&h, &v, KILN_INPUT_PRESS);
    (void)feed(&h, &v, KILN_INPUT_PRESS);

    (void)feed(&h, &v, KILN_INPUT_PRESS);               /* 'a'           */
    (void)feed(&h, &v, KILN_INPUT_CW);                  /* 'b'           */
    (void)feed(&h, &v, KILN_INPUT_PRESS);
    CHECK(strcmp(kiln_hmi_passphrase(&h), "ab") == 0);

    /* Two turns backwards from 'b' is "delete". */
    (void)feed(&h, &v, KILN_INPUT_CCW);
    (void)feed(&h, &v, KILN_INPUT_CCW);
    (void)feed(&h, &v, KILN_INPUT_CCW);
    (void)feed(&h, &v, KILN_INPUT_PRESS);
    CHECK_MSG(strcmp(kiln_hmi_passphrase(&h), "a") == 0,
              "after delete: %s", kiln_hmi_passphrase(&h));

    /* Deleting past the start is not an underflow. */
    (void)feed(&h, &v, KILN_INPUT_PRESS);
    (void)feed(&h, &v, KILN_INPUT_PRESS);
    CHECK(kiln_hmi_passphrase(&h)[0] == '\0');
}

/*
 * @relation(SWR-NET-12, scope=function)
 */
KILN_TEST(swrnet12_abandoning_entry_takes_the_passphrase_with_it)
{
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    const kiln_hmi_view_t v = net_view();
    to_network(&h, &v);
    (void)feed(&h, &v, KILN_INPUT_PRESS);
    (void)feed(&h, &v, KILN_INPUT_PRESS);
    for (int i = 0; i < 5; i++) { (void)feed(&h, &v, KILN_INPUT_PRESS); }
    CHECK(kiln_hmi_passphrase(&h)[0] != '\0');

    (void)feed(&h, &v, KILN_INPUT_LONG_PRESS);
    CHECK(h.screen == KILN_HMI_SCREEN_NETWORKS);
    CHECK_MSG(kiln_hmi_passphrase(&h)[0] == '\0',
              "half a passphrase was left behind for the next attempt");
}

/*
 * @relation(SWR-NET-12, scope=function)
 */
KILN_TEST(swrnet12_the_charset_reaches_every_printable_character)
{
    /* A passphrase this cannot type is a network this kiln cannot join, and
     * the device raises no access point to fall back to. */
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    const kiln_hmi_view_t v = net_view();
    to_network(&h, &v);
    (void)feed(&h, &v, KILN_INPUT_PRESS);
    (void)feed(&h, &v, KILN_INPUT_PRESS);

    bool seen[127] = { false };
    for (int i = 0; i < 200; i++) {
        kiln_hmi_t probe = h;
        probe.pass_len = 0;
        probe.pass[0]  = '\0';
        (void)feed(&probe, &v, KILN_INPUT_PRESS);
        const unsigned char c = (unsigned char)probe.pass[0];
        if (c >= 32u && c < 127u) { seen[c] = true; }
        (void)feed(&h, &v, KILN_INPUT_CW);
    }
    for (unsigned char c = 32; c < 127u; c++) {
        CHECK_MSG(seen[c], "the knob cannot reach '%c' (%u)", c, (unsigned)c);
    }
}

/*
 * @relation(SWR-NET-11, scope=function)
 */
KILN_TEST(swrnet11_the_list_scrolls_and_keeps_the_selection_visible)
{
    kiln_hmi_view_t v = net_view();
    v.net_count = KILN_HMI_MAX_NETWORKS;
    for (uint8_t i = 0; i < KILN_HMI_MAX_NETWORKS; i++) {
        (void)snprintf(v.net_list_ssid[i], KILN_HMI_SSID_LEN, "net-%u", (unsigned)i);
        v.net_list_secured[i] = true;
    }

    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    to_network(&h, &v);
    (void)feed(&h, &v, KILN_INPUT_PRESS);

    for (uint8_t i = 0; i < KILN_HMI_MAX_NETWORKS; i++) {
        CHECK_MSG(h.net_sel >= h.net_top && h.net_sel < h.net_top + 4u,
                  "selection %u is outside the window at %u", (unsigned)h.net_sel,
                  (unsigned)h.net_top);
        (void)feed(&h, &v, KILN_INPUT_CW);
    }
}

/* ===========================================================================
 * THE DISPLAY'S OWN WORDS  (SWR-NFR-23, tasklist O4 and J2)
 * ===========================================================================
 * kiln_core/faults has always carried faults and warnings in both languages,
 * so a device set to German announced its faults in German inside an English
 * frame: every screen title, the whole menu, both answers to a confirmation
 * and every footer were string literals at their draw sites.
 *
 * The table these check is the one a reviewer reads (J4 is still open: no
 * native speaker has been through it). What is asserted here is what a
 * reviewer cannot see by reading -- that nothing is missing, that nothing
 * overruns the panel, and that nothing contains a character the font cannot
 * draw.
 */

/*
 * @relation(SWR-NFR-23, scope=function)
 */
KILN_TEST(swrnfr23_every_string_exists_in_both_languages)
{
    for (unsigned id = 0; id < (unsigned)KILN_HMI_STR_COUNT; id++) {
        for (unsigned lang = 0; lang < (unsigned)KILN_LANG_COUNT; lang++) {
            const char *s = kiln_hmi_str((kiln_hmi_str_id_t)id, (kiln_lang_t)lang);
            CHECK_MSG(s != nullptr && s[0] != '\0',
                      "string %u is missing in language %u", id, lang);
        }
    }
}

/*
 * @relation(SWR-NFR-23, scope=function)
 */
KILN_TEST(swrnfr23_the_german_uses_only_glyphs_the_font_has)
{
    /* draw.cpp's 5x7 font is the 95 printable ASCII glyphs and nothing else:
     * no umlauts, no sharp s. So the German is transliterated -- AE OE UE SS,
     * as kiln_core/faults has always done -- and a single umlaut typed into
     * the table would reach the panel as a blank nobody sees until the device
     * is in a workshop. */
    for (unsigned id = 0; id < (unsigned)KILN_HMI_STR_COUNT; id++) {
        for (unsigned lang = 0; lang < (unsigned)KILN_LANG_COUNT; lang++) {
            const char *s = kiln_hmi_str((kiln_hmi_str_id_t)id, (kiln_lang_t)lang);
            for (const char *p = s; *p != '\0'; p++) {
                const unsigned char c = (unsigned char)*p;
                CHECK_MSG(c >= 0x20u && c <= 0x7Eu,
                          "string %u language %u has byte 0x%02X, which the font "
                          "cannot draw", id, lang, (unsigned)c);
            }
        }
    }
}

/*
 * @relation(SWR-NFR-23, scope=function)
 */
KILN_TEST(swrnfr23_no_label_overruns_the_panel)
{
    /* 21 characters at scale 1, and the framebuffer clips silently past that.
     * German runs about 15 per cent longer than English, so this is the bound
     * the translation has to be written against rather than something to
     * discover on hardware. */
    for (unsigned id = 0; id < (unsigned)KILN_HMI_STR_COUNT; id++) {
        for (unsigned lang = 0; lang < (unsigned)KILN_LANG_COUNT; lang++) {
            const char *s = kiln_hmi_str((kiln_hmi_str_id_t)id, (kiln_lang_t)lang);
            CHECK_MSG(strlen(s) <= KILN_HMI_COLS,
                      "string %u language %u is %u characters, over the %u the "
                      "panel has", id, lang, (unsigned)strlen(s),
                      (unsigned)KILN_HMI_COLS);
        }
    }
}

/*
 * @relation(SWR-NFR-23, scope=function)
 */
KILN_TEST(swrnfr23_an_unknown_language_falls_back_rather_than_blanking)
{
    /* types.h promises that a language the build does not carry shows English
     * rather than an empty screen. A blank menu on a kiln is worse than an
     * English one. */
    const char *en = kiln_hmi_str(KILN_HMI_STR_ABORT, KILN_LANG_EN);
    CHECK_STR_EQ(kiln_hmi_str(KILN_HMI_STR_ABORT, (kiln_lang_t)47), en);

    /* An identifier out of range is a programming error rather than a
     * configuration one, and answers with nothing: a wrong label on a kiln is
     * worse than a gap, which at least looks like what it is. */
    CHECK_STR_EQ(kiln_hmi_str((kiln_hmi_str_id_t)KILN_HMI_STR_COUNT, KILN_LANG_EN), "");
}

/*
 * @relation(SWR-NFR-23, scope=function)
 */
KILN_TEST(swrnfr23_the_screens_themselves_change_language)
{
    /* The table existing is not the point; the screens using it is. Each of
     * these is a screen that carried English literals until the table landed,
     * and the pixel count moving is what proves the draw site was changed. */
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    kiln_hmi_view_t v = base_view();

    /* The menu, which is the first screen an operator meets. */
    v.language = KILN_LANG_EN;
    (void)feed(&h, &v, KILN_INPUT_PRESS);
    CHECK_EQ_INT(h.screen, KILN_HMI_SCREEN_MENU);
    const int menu_en = ink(&h, 0, 0, 128, 64);
    v.language = KILN_LANG_DE;
    (void)feed(&h, &v, KILN_INPUT_NONE);
    CHECK_MSG(ink(&h, 0, 0, 128, 64) != menu_en, "the menu did not change");

    /* The confirmation, where the two answers are the whole screen. */
    v.language = KILN_LANG_EN;
    (void)feed(&h, &v, KILN_INPUT_NONE);
    (void)feed(&h, &v, KILN_INPUT_PRESS);           /* Start program  */
    (void)feed(&h, &v, KILN_INPUT_PRESS);           /* pick the first */
    CHECK_EQ_INT(h.screen, KILN_HMI_SCREEN_CONFIRM);
    const int confirm_en = ink(&h, 0, 0, 128, 64);
    v.language = KILN_LANG_DE;
    (void)feed(&h, &v, KILN_INPUT_NONE);
    CHECK_MSG(ink(&h, 0, 0, 128, 64) != confirm_en,
              "the confirmation did not change");
}

/*
 * @relation(SWR-NET-12, scope=function)
 */
KILN_TEST(swrnet12_a_failed_join_says_so_with_the_radios_reason)
{
    /* Until now the display returned to the network screen saying "not
     * connected", leaving the operator to guess between a mistyped
     * passphrase, an SSID out of range and a radio that never came up. With
     * the display the only route in -- there is no access point -- guessing is
     * the entire cost of getting it wrong. */
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    kiln_hmi_view_t v = base_view();
    v.net_up = false;

    (void)feed(&h, &v, KILN_INPUT_PRESS);
    for (int i = 0; i < 3; i++) { (void)feed(&h, &v, KILN_INPUT_CW); }
    (void)feed(&h, &v, KILN_INPUT_PRESS);
    CHECK_EQ_INT(h.screen, KILN_HMI_SCREEN_NETWORK);

    /* Nothing attempted yet: the third row says an attempt is in flight
     * rather than that it failed. */
    v.net_last_reason = 0;
    (void)feed(&h, &v, KILN_INPUT_NONE);
    const int joining = ink(&h, 0, 32, 128, 10);
    CHECK(joining > 0);

    /* Reason 15 is a failed four-way handshake, which is a wrong passphrase in
     * almost every case. The row has to change, and visibly. */
    v.net_last_reason = 15;
    (void)feed(&h, &v, KILN_INPUT_NONE);
    const int failed = ink(&h, 0, 32, 128, 10);
    CHECK_MSG(failed != joining,
              "the network screen shows the same thing whether a join is in "
              "progress or has failed");

    /* The code itself is on the screen: it is there to be read out to somebody
     * who can look it up, so a different code must render differently. */
    v.net_last_reason = 201;        /* no AP of that name answered */
    (void)feed(&h, &v, KILN_INPUT_NONE);
    CHECK_MSG(ink(&h, 0, 32, 128, 10) != failed,
              "the reason code is not rendered, only the fact of the failure");

    /* And once it is up, the row goes back to being the hostname. */
    v.net_up = true;
    (void)feed(&h, &v, KILN_INPUT_NONE);
    CHECK(ink(&h, 0, 32, 128, 10) > 0);
}

/*
 * @relation(SWR-NET-12, scope=function)
 */
KILN_TEST(swrnet12_the_knob_stays_where_the_last_character_left_it)
{
    /* A rotary encoder is a poor keyboard and 95 characters is a long way
     * round, so the one cheap improvement is not sending the knob back to 'a'
     * after every press: passphrases repeat characters and cluster inside a
     * region of the set, so resuming where the last press landed is usually
     * several turns saved and never more.
     *
     * This is a property of the entry screen that nothing asserted, and it is
     * the sort of thing a later edit silently reverses by adding one
     * "sensible" reset.
     */
    kiln_hmi_t h; kiln_hmi_init(&h, 0);
    kiln_hmi_view_t v = base_view();
    v.net_count = 1;
    v.net_list_secured[0] = true;
    (void)snprintf(v.net_list_ssid[0], KILN_HMI_SSID_LEN, "workshop");

    (void)feed(&h, &v, KILN_INPUT_PRESS);
    for (int i = 0; i < 3; i++) { (void)feed(&h, &v, KILN_INPUT_CW); }
    (void)feed(&h, &v, KILN_INPUT_PRESS);          /* network screen   */
    (void)feed(&h, &v, KILN_INPUT_PRESS);          /* the scan list    */
    CHECK_EQ_INT(h.screen, KILN_HMI_SCREEN_NETWORKS);
    (void)feed(&h, &v, KILN_INPUT_PRESS);          /* secured, so type */
    CHECK_EQ_INT(h.screen, KILN_HMI_SCREEN_PASSPHRASE);

    /* Entry starts at the first character, which is right for the first one. */
    CHECK_EQ_UINT(h.charset_sel, 0u);

    for (int i = 0; i < 7; i++) { (void)feed(&h, &v, KILN_INPUT_CW); }
    const uint8_t landed = h.charset_sel;
    CHECK(landed == 7u);
    (void)feed(&h, &v, KILN_INPUT_PRESS);
    CHECK_EQ_UINT(h.pass_len, 1u);
    CHECK_MSG(h.charset_sel == landed,
              "the knob went back to the start after a press, which costs the "
              "operator the whole way round again for a repeated character");

    /* And again, so the second press is not a special case either. */
    (void)feed(&h, &v, KILN_INPUT_PRESS);
    CHECK_EQ_UINT(h.pass_len, 2u);
    CHECK_EQ_UINT(h.charset_sel, landed);
    CHECK_EQ_INT(h.pass[0], h.pass[1]);       /* the same character twice */
}

/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Screens and the menu state machine (FR-HMI-02..FR-HMI-15).
 *
 * Pure: a frame and an action out, a view and an event in.  See hmi.h for why.
 */
#include <stdio.h>
#include <string.h>

#include "kiln_core/faults.h"
#include "kiln_hmi/hmi.h"

/* --- small helpers ------------------------------------------------------ */

namespace {

float to_display_c(const kiln_hmi_view_t *v, float c)
{
    /* FR-HMI-13: degF is a display conversion and nothing else.  Every stored,
     * logged and transmitted value stays in degC. */
    return v->fahrenheit ? (c * 9.0f / 5.0f + 32.0f) : c;
}

const char *unit_str(const kiln_hmi_view_t *v)
{
    return v->fahrenheit ? "F" : "C";
}

void fmt_temp(char *buf, size_t n, const kiln_hmi_view_t *v, float c, bool valid)
{
    if (!valid) {
        /* FR-ACQ-12: inside the grace window there is no measurement, and
         * showing the last one as though it were current is how an operator
         * ends up trusting a number the firmware has already disowned. */
        (void)snprintf(buf, n, "---");
        return;
    }
    (void)snprintf(buf, n, "%d", (int)(to_display_c(v, c) + (c < 0.0f ? -0.5f : 0.5f)));
}

void fmt_hms(char *buf, size_t n, uint32_t s)
{
    const uint32_t h = s / 3600u;
    const uint32_t m = (s % 3600u) / 60u;
    if (h > 0u) { (void)snprintf(buf, n, "%uh%02u", (unsigned)h, (unsigned)m); }
    else        { (void)snprintf(buf, n, "%um%02u", (unsigned)m, (unsigned)(s % 60u)); }
}

const char *state_text(const kiln_hmi_view_t *v)
{
    /* NFR-23 / FR-HMI-15: the one table, in the configured language. */
    return kiln_state_label((kiln_state_t)v->snap.state);
}

/* --- screens ------------------------------------------------------------ */

void draw_main(kiln_hmi_t *h, const kiln_hmi_view_t *v)
{
    char buf[32];

    /* FR-HMI-03: the chamber temperature in the largest font on the screen.
     * Scale 3 is 15x21 pixels per glyph, which is what "legible at 2 m" comes
     * to on a 128x64 panel of this size. */
    fmt_temp(buf, sizeof(buf), v, v->snap.kiln_c, v->snap.kiln_valid);
    kiln_fb_text(&h->fb, 0, 2, buf, 3, true);
    kiln_fb_text(&h->fb, kiln_fb_text_width(buf, 3) + 4, 2, unit_str(v), 1, true);

    /* FR-HMI-02: the target, simultaneously and unambiguously.  Prefixed, so
     * the two numbers cannot be read as one. */
    char t[16];
    fmt_temp(t, sizeof(t), v, v->snap.setpoint_c, true);
    (void)snprintf(buf, sizeof(buf), "SET %s", t);
    kiln_fb_text_right(&h->fb, KILN_DISPLAY_W - 1, 2, buf, 1);

    /* FR-HMI-04: state, and the segment when there is one. */
    if (v->snap.segment_count > 0u) {
        (void)snprintf(buf, sizeof(buf), "%s %u/%u", state_text(v),
                       (unsigned)(v->snap.segment_index + 1u),
                       (unsigned)v->snap.segment_count);
    } else {
        (void)snprintf(buf, sizeof(buf), "%s", state_text(v));
    }
    kiln_fb_text_right(&h->fb, KILN_DISPLAY_W - 1, 12, buf, 1);

    /* Rate of change, signed, because the sign is the information. */
    (void)snprintf(buf, sizeof(buf), "%+d%s/h", (int)v->snap.rate_c_per_h,
                   unit_str(v));
    kiln_fb_text(&h->fb, 0, 30, buf, 1, true);

    /* Elapsed, and remaining when the setpoint generator can say. */
    char e[12];
    fmt_hms(e, sizeof(e), v->elapsed_s);
    if (v->remaining_s > 0u) {
        char r[12];
        fmt_hms(r, sizeof(r), v->remaining_s);
        (void)snprintf(buf, sizeof(buf), "%s / -%s", e, r);
    } else {
        (void)snprintf(buf, sizeof(buf), "%s", e);
    }
    kiln_fb_text_right(&h->fb, KILN_DISPLAY_W - 1, 30, buf, 1);

    /* Duty, and the two states worth seeing at a glance. */
    (void)snprintf(buf, sizeof(buf), "%u%%", (unsigned)(v->snap.duty_permille / 10u));
    kiln_fb_text(&h->fb, 0, 40, buf, 1, true);
    if (v->snap.heat_authorised) { kiln_fb_text(&h->fb, 30, 40, "HEAT", 1, true); }
    if (v->snap.holdback_active) { kiln_fb_text(&h->fb, 62, 40, "HOLD", 1, true); }
    if (v->awaiting_ack)         { kiln_fb_text(&h->fb, 94, 40, "ACK?", 1, true); }

    /* FR-HMI-04's progress indication: segment position through the program. */
    uint8_t pct = 0;
    if (v->snap.segment_count > 0u) {
        pct = (uint8_t)(((uint32_t)v->snap.segment_index * 100u) /
                        v->snap.segment_count);
    }
    kiln_fb_progress(&h->fb, 0, 52, KILN_DISPLAY_W, 10, pct);

    /* A warning is not a fault and must not take the screen, but it must be
     * visible (FR-WEB-24's local counterpart). */
    if (v->warnings != 0u) {
        kiln_fb_text(&h->fb, 2, 54, "!", 1, false);
    }
}

void draw_fault(kiln_hmi_t *h, const kiln_hmi_view_t *v)
{
    /* FR-HMI-06: takes precedence over everything and is not dismissible while
     * the condition holds.  Inverted, because a fault screen that looks like
     * every other screen is a fault screen somebody walks past. */
    kiln_fb_fill(&h->fb, 0, 0, KILN_DISPLAY_W, 12, true);

    /* AD-22: when the supervisor is the reason, say so on the banner and show
     * *its* instruction instead of the controller's.  The two are cleared
     * differently -- the supervisor's by the button on the panel -- and an
     * operator shown the wrong instruction will press the wrong thing. */
    const bool sup_blame = v->sup_fitted && v->sup_tripped
                        && (v->sup_reason != KILN_SUP_OK);

    char buf[40];
    if (sup_blame) {
        (void)snprintf(buf, sizeof(buf), "SUPERVISOR");
    } else {
        (void)snprintf(buf, sizeof(buf), "FAULT %u", (unsigned)v->fault);
    }
    kiln_fb_text(&h->fb, 2, 2, buf, 1, false);
    kiln_fb_text_right(&h->fb, KILN_DISPLAY_W - 2, 2,
                       sup_blame ? kiln_sup_reason_label_in(v->sup_reason, v->language)
                                 : kiln_fault_label_in(v->fault, v->language), 1);

    /* The cause, wrapped to the panel.  Word wrapping rather than hard cuts:
     * these sentences are the operator's instructions, and a word split across
     * two lines in a hurry reads as a different word. */
    const char *cause = sup_blame
                      ? kiln_sup_reason_cause_in(v->sup_reason, v->language)
                      : kiln_fault_cause_in(v->fault, v->language);
    const int   cols  = KILN_DISPLAY_W / 6;
    int         y     = 16;
    while ((*cause != '\0') && y < (KILN_DISPLAY_H - 8)) {
        int take = 0, last_space = -1;
        while (take < cols && cause[take] != '\0') {
            if (cause[take] == ' ') { last_space = take; }
            take++;
        }
        if (cause[take] != '\0' && last_space > 0) { take = last_space; }

        char line[32];
        const int n = (take < (int)sizeof(line) - 1) ? take : (int)sizeof(line) - 1;
        memcpy(line, cause, (size_t)n);
        line[n] = '\0';
        kiln_fb_text(&h->fb, 0, y, line, 1, true);

        cause += take;
        while (*cause == ' ') { cause++; }
        y += 9;
    }
}

const char *menu_label(uint8_t i, const kiln_hmi_view_t *v)
{
    const bool running = (v->snap.state == (uint8_t)KILN_STATE_RUNNING);
    const bool paused  = (v->snap.state == (uint8_t)KILN_STATE_PAUSED);
    switch (i) {
    case 0: return "Start program";
    case 1: return running ? "Pause" : (paused ? "Resume" : "Pause/Resume");
    case 2: return "Abort";
    case 3: return "Network";
    case 4: return "Diagnostics";
    case 5: return "Info";
    default: return "";
    }
}

} // namespace
constexpr int MENU_ITEMS = 6;

namespace {

void draw_list(kiln_hmi_t *h, const char *title, uint8_t count, uint8_t sel,
                      uint8_t top, const char *(*label)(uint8_t, const kiln_hmi_view_t *),
                      const kiln_hmi_view_t *v)
{
    kiln_fb_text(&h->fb, 0, 0, title, 1, true);
    kiln_fb_hline(&h->fb, 0, 9, KILN_DISPLAY_W, true);

    /* Five rows fit below the rule; the window follows the selection rather
     * than the selection being clamped to the window. */
    const uint8_t rows = 5;
    for (uint8_t r = 0; r < rows; r++) {
        const uint8_t i = (uint8_t)(top + r);
        if (i >= count) { break; }
        const int y = 13 + r * 10;
        if (i == sel) { kiln_fb_text_inv(&h->fb, 2, y, label(i, v), 1); }
        else          { kiln_fb_text(&h->fb, 2, y, label(i, v), 1, true); }
    }
    if (count > rows) {
        /* A position mark, so a long list does not feel bottomless. */
        const int bar = (int)((KILN_DISPLAY_H - 14) * sel / (count > 1u ? count - 1u : 1u));
        kiln_fb_fill(&h->fb, KILN_DISPLAY_W - 3, 12 + bar, 3, 4, true);
    }
}

const kiln_hmi_view_t *s_prog_view;    /* for the label callback below */

const char *program_label(uint8_t i, const kiln_hmi_view_t *v)
{
    (void)v;
    return (i < s_prog_view->program_count) ? s_prog_view->program_name[i] : "";
}

void draw_confirm(kiln_hmi_t *h, const kiln_hmi_view_t *v)
{
    /* FR-HMI-11: starting and aborting both require this step. */
    const char *what = (h->pending == KILN_HMI_ACT_START) ? "Start firing?" : "Abort firing?";
    kiln_fb_text(&h->fb, 0, 4, what, 1, true);

    if (h->pending == KILN_HMI_ACT_START && h->pending_program < v->program_count) {
        kiln_fb_text(&h->fb, 0, 16, v->program_name[h->pending_program], 1, true);
    }

    if (h->confirm_yes) {
        kiln_fb_text_inv(&h->fb, 14, 40, "YES", 1);
        kiln_fb_text(&h->fb, 80, 40, "NO", 1, true);
    } else {
        kiln_fb_text(&h->fb, 14, 40, "YES", 1, true);
        kiln_fb_text_inv(&h->fb, 80, 40, "NO", 1);
    }
    kiln_fb_text(&h->fb, 0, 54, "turn to choose, press", 1, true);
}

void draw_network(kiln_hmi_t *h, const kiln_hmi_view_t *v)
{
    /* FR-HMI-07: where the web interface is, which is the question an operator
     * standing at the kiln actually has. */
    kiln_fb_text(&h->fb, 0, 0, "NETWORK", 1, true);
    kiln_fb_hline(&h->fb, 0, 9, KILN_DISPLAY_W, true);
    kiln_fb_text(&h->fb, 0, 14, v->net_up ? "connected" : "not connected", 1, true);
    kiln_fb_text(&h->fb, 0, 26, v->hostname, 1, true);
    kiln_fb_text(&h->fb, 0, 38, v->ip, 1, true);
    kiln_fb_text(&h->fb, 0, 54, "display only", 1, true);
}

void draw_diag(kiln_hmi_t *h, const kiln_hmi_view_t *v)
{
    char buf[32], t[12];
    kiln_fb_text(&h->fb, 0, 0, "DIAGNOSTICS", 1, true);
    kiln_fb_hline(&h->fb, 0, 9, KILN_DISPLAY_W, true);

    fmt_temp(t, sizeof(t), v, v->snap.case_c, v->snap.case_valid);
    (void)snprintf(buf, sizeof(buf), "case  %s%s", t, unit_str(v));
    kiln_fb_text(&h->fb, 0, 13, buf, 1, true);

    (void)snprintf(buf, sizeof(buf), "amps  %.1f", (double)v->snap.current_a);
    kiln_fb_text(&h->fb, 0, 23, buf, 1, true);

    /* FR-CUR-07, in the units a kiln owner thinks in. */
    (void)snprintf(buf, sizeof(buf), "power %.2fkW", v->power_w / 1000.0);
    kiln_fb_text(&h->fb, 0, 33, buf, 1, true);
    (void)snprintf(buf, sizeof(buf), "used  %.2fkWh", v->energy_wh / 1000.0);
    kiln_fb_text(&h->fb, 0, 43, buf, 1, true);

    (void)snprintf(buf, sizeof(buf), "duty  %u%%",
                   (unsigned)(v->snap.duty_permille / 10u));
    kiln_fb_text(&h->fb, 0, 53, buf, 1, true);
}

void draw_info(kiln_hmi_t *h, const kiln_hmi_view_t *v)
{
    char buf[40], up[12];
    kiln_fb_text(&h->fb, 0, 0, "INFO", 1, true);
    kiln_fb_hline(&h->fb, 0, 9, KILN_DISPLAY_W, true);
    kiln_fb_text(&h->fb, 0, 13, v->version, 1, true);

    fmt_hms(up, sizeof(up), v->uptime_s);
    (void)snprintf(buf, sizeof(buf), "up %s", up);
    kiln_fb_text(&h->fb, 0, 23, buf, 1, true);

    (void)snprintf(buf, sizeof(buf), "P%.1f I%.3f", (double)v->kp, (double)v->ki);
    kiln_fb_text(&h->fb, 0, 33, buf, 1, true);
    (void)snprintf(buf, sizeof(buf), "D%.0f", (double)v->kd);
    kiln_fb_text(&h->fb, 0, 43, buf, 1, true);

    /* FR-TUN-11: factory gains are not gains for *this* kiln, and the screen
     * that shows them is the right place to say so. */
    kiln_fb_text(&h->fb, 0, 53, v->gains_tuned ? "tuned" : "UNTUNED defaults", 1, true);
}

} // namespace

/* --- state machine ------------------------------------------------------ */

void kiln_hmi_init(kiln_hmi_t *h, uint16_t dim_timeout_s)
{
    if (h == NULL) { return; }
    memset(h, 0, sizeof(*h));
    h->screen        = KILN_HMI_SCREEN_MAIN;
    h->dim_timeout_s = dim_timeout_s;
    h->dirty         = true;
}

namespace {

kiln_hmi_action_t none(void)
{
    kiln_hmi_action_t a = {};
    a.kind = KILN_HMI_ACT_NONE;
    return a;
}

void step(uint8_t *sel, uint8_t count, bool forward)
{
    if (count == 0u) { return; }
    if (forward) { *sel = (uint8_t)((*sel + 1u) % count); }
    else         { *sel = (uint8_t)((*sel == 0u) ? (count - 1u) : (*sel - 1u)); }
}

} // namespace

kiln_hmi_action_t kiln_hmi_update(kiln_hmi_t *h, const kiln_hmi_view_t *view,
                                  kiln_input_event_t ev, uint32_t dt_ms)
{
    if ((h == NULL) || (view == NULL)) { return none(); }
    s_prog_view = view;

    kiln_hmi_action_t act = none();
    const bool faulted = (view->fault != KILN_FAULT_NONE);

    /* FR-HMI-12: the dim timeout, suspended while a fault is up.  A kiln that
     * blanked its own fault screen would be worse than one with no screen. */
    if (ev != KILN_INPUT_NONE || faulted) {
        h->idle_ms = 0;
        h->dimmed  = false;
    } else if (h->dim_timeout_s > 0u) {
        h->idle_ms += dt_ms;
        const bool was = h->dimmed;
        h->dimmed = (h->idle_ms >= (uint32_t)h->dim_timeout_s * 1000u);
        if (h->dimmed != was) { h->dirty = true; }
    }

    /* FR-HMI-06: the fault screen takes precedence over everything, including
     * whatever menu the operator was halfway through. */
    if (faulted && h->screen != KILN_HMI_SCREEN_FAULT) {
        h->screen = KILN_HMI_SCREEN_FAULT;
        h->dirty  = true;
    }

    if (ev != KILN_INPUT_NONE) {
        h->dirty = true;

        switch (h->screen) {
        case KILN_HMI_SCREEN_FAULT:
            /* FR-HMI-10: acknowledging is one of the things the local input
             * must be able to do.  Whether it is *allowed* is SR-18's
             * decision, made in kiln_app, not here: the HMI asks. */
            if (ev == KILN_INPUT_PRESS) { act.kind = KILN_HMI_ACT_ACK_FAULT; }
            break;

        case KILN_HMI_SCREEN_MAIN:
            if (ev == KILN_INPUT_PRESS) {
                if (view->awaiting_ack) {
                    act.kind = KILN_HMI_ACT_ACK_SEGMENT;
                } else {
                    h->screen   = KILN_HMI_SCREEN_MENU;
                    h->menu_sel = 0;
                }
            }
            break;

        case KILN_HMI_SCREEN_MENU:
            if (ev == KILN_INPUT_CW)         { step(&h->menu_sel, MENU_ITEMS, true); }
            else if (ev == KILN_INPUT_CCW)   { step(&h->menu_sel, MENU_ITEMS, false); }
            else if (ev == KILN_INPUT_LONG_PRESS) { h->screen = KILN_HMI_SCREEN_MAIN; }
            else if (ev == KILN_INPUT_PRESS) {
                switch (h->menu_sel) {
                case 0:
                    h->screen   = KILN_HMI_SCREEN_PROGRAMS;
                    h->prog_sel = 0;
                    h->prog_top = 0;
                    break;
                case 1:
                    act.kind = (view->snap.state == (uint8_t)KILN_STATE_PAUSED)
                                 ? KILN_HMI_ACT_RESUME : KILN_HMI_ACT_PAUSE;
                    h->screen = KILN_HMI_SCREEN_MAIN;
                    break;
                case 2:
                    /* FR-HMI-11: abort confirms. */
                    h->pending     = KILN_HMI_ACT_ABORT;
                    h->confirm_yes = false;   /* default to the safe answer */
                    h->screen      = KILN_HMI_SCREEN_CONFIRM;
                    break;
                case 3: h->screen = KILN_HMI_SCREEN_NETWORK; break;
                case 4: h->screen = KILN_HMI_SCREEN_DIAG;    break;
                default: h->screen = KILN_HMI_SCREEN_INFO;   break;
                }
            }
            break;

        case KILN_HMI_SCREEN_PROGRAMS:
            if (ev == KILN_INPUT_CW)       { step(&h->prog_sel, view->program_count, true); }
            else if (ev == KILN_INPUT_CCW) { step(&h->prog_sel, view->program_count, false); }
            else if (ev == KILN_INPUT_LONG_PRESS) { h->screen = KILN_HMI_SCREEN_MENU; }
            else if (ev == KILN_INPUT_PRESS && view->program_count > 0u) {
                /* FR-HMI-11: starting confirms. */
                h->pending         = KILN_HMI_ACT_START;
                h->pending_program = h->prog_sel;
                h->confirm_yes     = false;
                h->screen          = KILN_HMI_SCREEN_CONFIRM;
            }
            /* Keep the selection inside the visible window. */
            if (h->prog_sel < h->prog_top)            { h->prog_top = h->prog_sel; }
            else if (h->prog_sel >= h->prog_top + 5u) { h->prog_top = (uint8_t)(h->prog_sel - 4u); }
            break;

        case KILN_HMI_SCREEN_CONFIRM:
            if (ev == KILN_INPUT_CW || ev == KILN_INPUT_CCW) {
                h->confirm_yes = !h->confirm_yes;
            } else if (ev == KILN_INPUT_LONG_PRESS) {
                h->screen = KILN_HMI_SCREEN_MAIN;
            } else if (ev == KILN_INPUT_PRESS) {
                if (h->confirm_yes) {
                    act.kind          = h->pending;
                    act.program_index = h->pending_program;
                }
                h->screen = KILN_HMI_SCREEN_MAIN;
            }
            break;

        default:   /* NETWORK, DIAG, INFO: read-only, any press returns */
            if (ev == KILN_INPUT_PRESS || ev == KILN_INPUT_LONG_PRESS) {
                h->screen = KILN_HMI_SCREEN_MENU;
            }
            break;
        }
    }

    /* The fault screen is only left when the fault has actually gone, which is
     * FR-HMI-06's "not dismissible while the condition persists". */
    if (!faulted && h->screen == KILN_HMI_SCREEN_FAULT) {
        h->screen = KILN_HMI_SCREEN_MAIN;
        h->dirty  = true;
    }

    /* --- render ---------------------------------------------------------- */
    kiln_fb_clear(&h->fb);
    if (h->dimmed) {
        return act;              /* blank, and nothing to draw (FR-HMI-12) */
    }

    switch (h->screen) {
    case KILN_HMI_SCREEN_FAULT:    draw_fault(h, view);   break;
    case KILN_HMI_SCREEN_MENU:
        draw_list(h, "MENU", MENU_ITEMS, h->menu_sel, 0, menu_label, view);
        break;
    case KILN_HMI_SCREEN_PROGRAMS:
        if (view->program_count == 0u) {
            kiln_fb_text(&h->fb, 0, 0, "PROGRAMS", 1, true);
            kiln_fb_hline(&h->fb, 0, 9, KILN_DISPLAY_W, true);
            kiln_fb_text(&h->fb, 0, 20, "none stored", 1, true);
        } else {
            draw_list(h, "PROGRAMS", view->program_count, h->prog_sel, h->prog_top,
                      program_label, view);
        }
        break;
    case KILN_HMI_SCREEN_CONFIRM:  draw_confirm(h, view); break;
    case KILN_HMI_SCREEN_NETWORK:  draw_network(h, view); break;
    case KILN_HMI_SCREEN_DIAG:     draw_diag(h, view);    break;
    case KILN_HMI_SCREEN_INFO:     draw_info(h, view);    break;
    case KILN_HMI_SCREEN_MAIN:
    default:                       draw_main(h, view);    break;
    }
    return act;
}

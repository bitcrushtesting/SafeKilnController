/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Screens and the menu state machine (SWR-HMI-02..SWR-HMI-15).
 *
 * Pure: a frame and an action out, a view and an event in.  See hmi.h for why.
 */
#include <stdio.h>
#include <string.h>

#include "kiln_core/faults.h"
#include "kiln_hmi/hmi.h"
#include "kiln_hmi/strings.h"

/* --- small helpers ------------------------------------------------------ */

namespace {

/* SWR-NET-12.  A WPA passphrase is printable ASCII, so the knob has to reach
 * all of it: lowercase first because most passphrases are mostly lowercase,
 * then uppercase, digits and symbols, then the two rows that are not
 * characters at all.
 *
 * DEL and DONE live at the END of the set rather than the start.  At the start
 * they sit between the operator and the letters on every single character; at
 * the end they are one turn backwards from the first letter, which is where a
 * knob reaches them fastest. */
const char k_charset[] =
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "0123456789"
    "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~ ";

constexpr uint8_t K_CHARS   = (uint8_t)(sizeof(k_charset) - 1u);
constexpr uint8_t K_SEL_DEL = K_CHARS;          /* one past the last char   */
constexpr uint8_t K_SEL_OK  = (uint8_t)(K_CHARS + 1u);
constexpr uint8_t K_SEL_N   = (uint8_t)(K_CHARS + 2u);

float to_display_c(const kiln_hmi_view_t *v, float c)
{
    /* SWR-HMI-13: degF is a display conversion and nothing else.  Every stored,
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
        /* SWR-ACQ-12: inside the grace window there is no measurement, and
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

/* The one place the language is read, so a draw site is a lookup rather than a
 * conditional.  SWR-NFR-23 is a presentation concern: nothing downstream of
 * here knows what language the panel is in. */
const char *S(const kiln_hmi_view_t *v, kiln_hmi_str_id_t id)
{
    return kiln_hmi_str(id, v->language);
}

const char *state_text(const kiln_hmi_view_t *v)
{
    /* SWR-NFR-23 / SWR-HMI-15: the one table, in the configured language. */
    return kiln_state_label((kiln_state_t)v->snap.state);
}

/* --- screens ------------------------------------------------------------ */

void draw_main(kiln_hmi_t *h, const kiln_hmi_view_t *v)
{
    char buf[32];

    /* SWR-HMI-03: the chamber temperature in the largest font on the screen.
     * Scale 3 is 15x21 pixels per glyph, which is what "legible at 2 m" comes
     * to on a 128x64 panel of this size. */
    fmt_temp(buf, sizeof(buf), v, v->snap.kiln_c, v->snap.kiln_valid);
    kiln_fb_text(&h->fb, 0, 2, buf, 3, true);
    kiln_fb_text(&h->fb, kiln_fb_text_width(buf, 3) + 4, 2, unit_str(v), 1, true);

    /* SWR-HMI-02: the target, simultaneously and unambiguously.  Prefixed, so
     * the two numbers cannot be read as one. */
    char t[16];
    fmt_temp(t, sizeof(t), v, v->snap.setpoint_c, true);
    (void)snprintf(buf, sizeof(buf), "SET %s", t);
    kiln_fb_text_right(&h->fb, KILN_DISPLAY_W - 1, 2, buf, 1);

    /* SWR-HMI-04: state, and the segment when there is one. */
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
    if (v->snap.heat_authorised) { kiln_fb_text(&h->fb, 30, 40, S(v, KILN_HMI_STR_HEAT), 1, true); }
    if (v->snap.holdback_active) { kiln_fb_text(&h->fb, 62, 40, S(v, KILN_HMI_STR_HOLD), 1, true); }
    if (v->awaiting_ack)         { kiln_fb_text(&h->fb, 94, 40, S(v, KILN_HMI_STR_ACK), 1, true); }

    /* SWR-HMI-04's progress indication: segment position through the program. */
    uint8_t pct = 0;
    if (v->snap.segment_count > 0u) {
        pct = (uint8_t)(((uint32_t)v->snap.segment_index * 100u) /
                        v->snap.segment_count);
    }
    kiln_fb_progress(&h->fb, 0, 52, KILN_DISPLAY_W, 10, pct);

    /* A warning is not a fault and must not take the screen, but it must be
     * visible (SWR-WEB-24's local counterpart). */
    if (v->warnings != 0u) {
        kiln_fb_text(&h->fb, 2, 54, "!", 1, false);
    }
}

void draw_fault(kiln_hmi_t *h, const kiln_hmi_view_t *v)
{
    /* SWR-HMI-06: takes precedence over everything and is not dismissible while
     * the condition holds.  Inverted, because a fault screen that looks like
     * every other screen is a fault screen somebody walks past. */
    kiln_fb_fill(&h->fb, 0, 0, KILN_DISPLAY_W, 12, true);

    /* SWA-22: when the supervisor is the reason, say so on the banner and show
     * *its* instruction instead of the controller's.  The two are cleared
     * differently -- the supervisor's by the button on the panel -- and an
     * operator shown the wrong instruction will press the wrong thing. */
    const bool sup_blame = v->sup_fitted && v->sup_tripped
                        && (v->sup_reason != KILN_SUP_OK);

    char buf[40];
    if (sup_blame) {
        (void)snprintf(buf, sizeof(buf), "%s", S(v, KILN_HMI_STR_SUPERVISOR));
    } else {
        (void)snprintf(buf, sizeof(buf), "%s %u", S(v, KILN_HMI_STR_FAULT),
                       (unsigned)v->fault);
    }
    kiln_fb_text(&h->fb, 2, 2, buf, 1, false);
    kiln_fb_text_right(&h->fb, KILN_DISPLAY_W - 2, 2,
                       sup_blame ? kiln_sup_reason_label_in(v->sup_reason, v->language)
                                 : kiln_fault_label_in(v->fault, v->language), 1);

    /* The cause, wrapped to the panel.  Word wrapping rather than hard cuts:
     * these sentences are the operator's instructions, and a word split across
     * two lines in a hurry reads as a different word. */
    /* The PANEL form, not the full cause.  The full ones run to 279
     * characters, which is a paragraph in a browser and five lines of 21 on
     * this screen: what fell off the bottom was the end of the sentence, and
     * in an instruction the end of the sentence is the instruction. */
    const char *cause = sup_blame
                      ? kiln_sup_reason_panel_in(v->sup_reason, v->language)
                      : kiln_fault_panel_in(v->fault, v->language);
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
    case 0: return S(v, KILN_HMI_STR_START_PROGRAM);
    case 1: return running ? S(v, KILN_HMI_STR_PAUSE)
                           : (paused ? S(v, KILN_HMI_STR_RESUME)
                                     : S(v, KILN_HMI_STR_PAUSE_RESUME));
    case 2: return S(v, KILN_HMI_STR_ABORT);
    case 3: return S(v, KILN_HMI_STR_NETWORK);
    case 4: return S(v, KILN_HMI_STR_DIAGNOSTICS);
    case 5: return S(v, KILN_HMI_STR_INFO);
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
    /* SWR-HMI-11: starting and aborting both require this step. */
    const char *what = (h->pending == KILN_HMI_ACT_START)
                     ? S(v, KILN_HMI_STR_START_FIRING_Q)
                     : S(v, KILN_HMI_STR_ABORT_FIRING_Q);
    kiln_fb_text(&h->fb, 0, 4, what, 1, true);

    if (h->pending == KILN_HMI_ACT_START && h->pending_program < v->program_count) {
        kiln_fb_text(&h->fb, 0, 16, v->program_name[h->pending_program], 1, true);
    }

    if (h->confirm_yes) {
        kiln_fb_text_inv(&h->fb, 14, 40, S(v, KILN_HMI_STR_YES), 1);
        kiln_fb_text(&h->fb, 80, 40, S(v, KILN_HMI_STR_NO), 1, true);
    } else {
        kiln_fb_text(&h->fb, 14, 40, S(v, KILN_HMI_STR_YES), 1, true);
        kiln_fb_text_inv(&h->fb, 80, 40, S(v, KILN_HMI_STR_NO), 1);
    }
    kiln_fb_text(&h->fb, 0, 54, S(v, KILN_HMI_STR_TURN_THEN_PRESS), 1, true);
}

void draw_network(kiln_hmi_t *h, const kiln_hmi_view_t *v)
{
    /* SWR-HMI-07: where the web interface is, which is the question an operator
     * standing at the kiln actually has. */
    kiln_fb_text(&h->fb, 0, 0, S(v, KILN_HMI_STR_NETWORK), 1, true);
    kiln_fb_hline(&h->fb, 0, 9, KILN_DISPLAY_W, true);

    /* SWR-NET-12: what became of the last attempt.
     *
     * "not connected" on its own is the answer an operator cannot act on, and
     * with the display the only route in -- there is no access point -- acting
     * on it is the whole job.  So a join in progress says so, and a join that
     * the radio refused says so with the radio's reason code: 15 is a failed
     * four-way handshake, which is a wrong passphrase in almost every case,
     * 201 is "no AP of that name answered", and 2 and 4 are the AP dropping
     * the association.  The number is there to be read out over a phone to
     * somebody who can look it up, which beats a sentence that guesses. */
    const char *first = v->net_up ? S(v, KILN_HMI_STR_CONNECTED)
                                  : S(v, KILN_HMI_STR_NOT_CONNECTED);
    kiln_fb_text(&h->fb, 0, 13, first, 1, true);
    kiln_fb_text(&h->fb, 0, 23, v->net_up ? v->net_ssid : "", 1, true);

    if (!v->net_up && v->net_last_reason != 0u) {
        char why[32];
        (void)snprintf(why, sizeof(why), "%s (%u)", S(v, KILN_HMI_STR_JOIN_FAILED),
                       (unsigned)v->net_last_reason);
        kiln_fb_text(&h->fb, 0, 33, why, 1, true);
    }
    else if (!v->net_up) {
        /* No reason recorded: either nothing has been attempted yet, or an
         * attempt is in flight.  Both are "ask again in a moment" rather than
         * a failure, and saying so is what stops an operator abandoning a join
         * that was going to work. */
        kiln_fb_text(&h->fb, 0, 33, S(v, KILN_HMI_STR_JOINING), 1, true);
    }
    else {
        kiln_fb_text(&h->fb, 0, 33, v->hostname, 1, true);
    }
    kiln_fb_text(&h->fb, 0, 43, v->ip, 1, true);
    /* SWR-NET-11: this is the only route into WiFi setup, and it says so.  The
     * device raises no access point, so if this screen cannot be reached the
     * network cannot be configured at all. */
    kiln_fb_text(&h->fb, 0, 54, S(v, KILN_HMI_STR_PRESS_SETUP_WIFI), 1, true);
}

/* SWR-NET-11.  Four rows at a time, strongest first, with a bar for signal and
 * a mark for "this one wants a passphrase".  No SSID is truncated silently:
 * the row is as wide as the screen and a long name is cut with an ellipsis so
 * the operator can see that it was cut. */
void draw_networks(kiln_hmi_t *h, const kiln_hmi_view_t *v)
{
    kiln_fb_text(&h->fb, 0, 0, S(v, KILN_HMI_STR_NETWORKS), 1, true);
    kiln_fb_hline(&h->fb, 0, 9, KILN_DISPLAY_W, true);

    if (v->net_count == 0u) {
        kiln_fb_text(&h->fb, 0, 24,
                     v->net_scanning ? S(v, KILN_HMI_STR_SCANNING)
                                     : S(v, KILN_HMI_STR_NONE_FOUND), 1, true);
        kiln_fb_text(&h->fb, 0, 54, S(v, KILN_HMI_STR_HOLD_BACK), 1, true);
        return;
    }

    for (uint8_t row = 0; row < 4u; row++) {
        const uint8_t i = (uint8_t)(h->net_top + row);
        if (i >= v->net_count) { break; }

        char line[24];
        /* 18 characters of SSID is what fits beside the marks at this size. */
        /* The precision is the point: truncation here is deliberate, and
         * writing it as "%s" into a short buffer makes it look accidental to
         * the compiler, which says so (-Wformat-truncation, and GCC is right).
         * An explicit bound states that 18 characters is the design. */
        char name[19];
        (void)snprintf(name, sizeof(name), "%.*s", (int)(sizeof(name) - 1u),
                       v->net_list_ssid[i]);
        if (strlen(v->net_list_ssid[i]) > sizeof(name) - 1u) {
            name[sizeof(name) - 2u] = '.';
            name[sizeof(name) - 3u] = '.';
        }
        (void)snprintf(line, sizeof(line), "%c%s%s",
                       (i == h->net_sel) ? '>' : ' ',
                       name,
                       v->net_list_secured[i] ? " *" : "");
        kiln_fb_text(&h->fb, 0, (int)(13 + row * 10), line, 1, true);
    }

    /* Sized for the widest this can print rather than for the line it is
     * drawn on: the compiler is right that %u of an unsigned is ten digits,
     * and a truncated footer is a worse answer than a wide buffer. */
    char foot[40];
    (void)snprintf(foot, sizeof(foot), "%u %s %u  %s",
                   (unsigned)(h->net_sel + 1u), S(v, KILN_HMI_STR_OF),
                   (unsigned)v->net_count, S(v, KILN_HMI_STR_NEEDS_KEY));
    kiln_fb_text(&h->fb, 0, 54, foot, 1, true);
}

/* SWR-NET-12.  One knob, one character at a time.  The passphrase is shown in
 * clear: it is the operator's own network, they are standing in front of the
 * kiln, and a row of asterisks on a 128x64 display means typing it blind with
 * a rotary encoder, which is how it gets typed wrong four times. */
void draw_passphrase(kiln_hmi_t *h, const kiln_hmi_view_t *v)
{
    (void)v;
    kiln_fb_text(&h->fb, 0, 0, S(v, KILN_HMI_STR_PASSPHRASE), 1, true);
    kiln_fb_hline(&h->fb, 0, 9, KILN_DISPLAY_W, true);

    /* The tail, because that is where the cursor is: 20 characters fit and a
     * longer passphrase scrolls under them. */
    const uint8_t shown = (h->pass_len > 20u) ? 20u : h->pass_len;
    char tail[22];
    (void)snprintf(tail, sizeof(tail), "%s", h->pass + (h->pass_len - shown));
    kiln_fb_text(&h->fb, 0, 14, tail, 1, true);
    kiln_fb_text(&h->fb, (int)(shown * 6), 14, "_", 1, true);

    char pick[24];
    if (h->charset_sel == K_SEL_DEL) {
        (void)snprintf(pick, sizeof(pick), "%s", S(v, KILN_HMI_STR_DELETE));
    } else if (h->charset_sel == K_SEL_OK) {
        /* The length rule is shown where it is enforced, so "nothing happened
         * when I pressed" never has to be guessed at. */
        (void)snprintf(pick, sizeof(pick), "%s",
                       (h->pass_len >= 8u) ? S(v, KILN_HMI_STR_CONNECT)
                                           : S(v, KILN_HMI_STR_EIGHT_OR_MORE));
    } else {
        (void)snprintf(pick, sizeof(pick), "   %c", k_charset[h->charset_sel]);
    }
    kiln_fb_text(&h->fb, 0, 30, pick, 1, true);

    char foot[40];
    (void)snprintf(foot, sizeof(foot), "%u %s  %s", (unsigned)h->pass_len,
                   S(v, KILN_HMI_STR_CHARS), S(v, KILN_HMI_STR_HOLD_CANCEL));
    kiln_fb_text(&h->fb, 0, 54, foot, 1, true);
}

void draw_diag(kiln_hmi_t *h, const kiln_hmi_view_t *v)
{
    char buf[32], t[12];
    kiln_fb_text(&h->fb, 0, 0, S(v, KILN_HMI_STR_DIAGNOSTICS), 1, true);
    kiln_fb_hline(&h->fb, 0, 9, KILN_DISPLAY_W, true);

    fmt_temp(t, sizeof(t), v, v->snap.case_c, v->snap.case_valid);
    (void)snprintf(buf, sizeof(buf), "%-5s %s%s", S(v, KILN_HMI_STR_CASE), t,
                   unit_str(v));
    kiln_fb_text(&h->fb, 0, 13, buf, 1, true);

    (void)snprintf(buf, sizeof(buf), "%-5s %.1f", S(v, KILN_HMI_STR_AMPS),
                   (double)v->snap.current_a);
    kiln_fb_text(&h->fb, 0, 23, buf, 1, true);

    /* SWR-CUR-07, in the units a kiln owner thinks in. */
    (void)snprintf(buf, sizeof(buf), "%-5s %.2fkW", S(v, KILN_HMI_STR_POWER),
                   v->power_w / 1000.0);
    kiln_fb_text(&h->fb, 0, 33, buf, 1, true);
    (void)snprintf(buf, sizeof(buf), "%-5s %.2fkWh", S(v, KILN_HMI_STR_USED),
                   v->energy_wh / 1000.0);
    kiln_fb_text(&h->fb, 0, 43, buf, 1, true);

    (void)snprintf(buf, sizeof(buf), "%-5s %u%%", S(v, KILN_HMI_STR_DUTY),
                   (unsigned)(v->snap.duty_permille / 10u));
    kiln_fb_text(&h->fb, 0, 53, buf, 1, true);
}

void draw_info(kiln_hmi_t *h, const kiln_hmi_view_t *v)
{
    char buf[40], up[12];
    kiln_fb_text(&h->fb, 0, 0, S(v, KILN_HMI_STR_INFO), 1, true);
    kiln_fb_hline(&h->fb, 0, 9, KILN_DISPLAY_W, true);
    kiln_fb_text(&h->fb, 0, 13, v->version, 1, true);

    fmt_hms(up, sizeof(up), v->uptime_s);
    (void)snprintf(buf, sizeof(buf), "%s %s", S(v, KILN_HMI_STR_UPTIME), up);
    kiln_fb_text(&h->fb, 0, 23, buf, 1, true);

    (void)snprintf(buf, sizeof(buf), "P%.1f I%.3f", (double)v->kp, (double)v->ki);
    kiln_fb_text(&h->fb, 0, 33, buf, 1, true);
    (void)snprintf(buf, sizeof(buf), "D%.0f", (double)v->kd);
    kiln_fb_text(&h->fb, 0, 43, buf, 1, true);

    /* SWR-TUN-11: factory gains are not gains for *this* kiln, and the screen
     * that shows them is the right place to say so. */
    kiln_fb_text(&h->fb, 0, 53,
                 v->gains_tuned ? S(v, KILN_HMI_STR_TUNED)
                                : S(v, KILN_HMI_STR_UNTUNED), 1, true);
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

    /* SWR-HMI-12: the dim timeout, suspended while a fault is up.  A kiln that
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

    /* SWR-HMI-06: the fault screen takes precedence over everything, including
     * whatever menu the operator was halfway through. */
    if (faulted && h->screen != KILN_HMI_SCREEN_FAULT) {
        h->screen = KILN_HMI_SCREEN_FAULT;
        h->dirty  = true;
    }

    if (ev != KILN_INPUT_NONE) {
        h->dirty = true;

        switch (h->screen) {
        case KILN_HMI_SCREEN_FAULT:
            /* SWR-HMI-10: acknowledging is one of the things the local input
             * must be able to do.  Whether it is *allowed* is SWR-SAF-18's
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
                    /* SWR-HMI-11: abort confirms. */
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
                /* SWR-HMI-11: starting confirms. */
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

        case KILN_HMI_SCREEN_NETWORK:
            /* SWR-NET-11: the status screen is where setting up WiFi starts,
             * because it is where an operator goes when the network is the
             * thing on their mind. */
            if (ev == KILN_INPUT_LONG_PRESS) {
                h->screen = KILN_HMI_SCREEN_MENU;
            } else if (ev == KILN_INPUT_PRESS) {
                act.kind    = KILN_HMI_ACT_WIFI_SCAN;
                h->net_sel  = 0;
                h->net_top  = 0;
                h->screen   = KILN_HMI_SCREEN_NETWORKS;
            }
            break;

        case KILN_HMI_SCREEN_NETWORKS:
            if (ev == KILN_INPUT_CW)       { step(&h->net_sel, view->net_count, true); }
            else if (ev == KILN_INPUT_CCW) { step(&h->net_sel, view->net_count, false); }
            else if (ev == KILN_INPUT_LONG_PRESS) {
                /* Back, and a second chance to scan: an empty list usually
                 * means the scan ran before the radio was ready. */
                h->screen = KILN_HMI_SCREEN_NETWORK;
            } else if (ev == KILN_INPUT_PRESS && view->net_count > 0u &&
                       h->net_sel < view->net_count) {
                if (view->net_list_secured[h->net_sel]) {
                    h->pass_len    = 0;
                    h->pass[0]     = '\0';
                    h->charset_sel = 0;
                    h->screen      = KILN_HMI_SCREEN_PASSPHRASE;
                } else {
                    /* An open network needs no passphrase, and asking for one
                     * would be a blank screen the operator has to guess past. */
                    h->pass_len       = 0;
                    h->pass[0]        = '\0';
                    act.kind          = KILN_HMI_ACT_WIFI_CONNECT;
                    act.network_index = h->net_sel;
                    h->screen         = KILN_HMI_SCREEN_NETWORK;
                }
            }
            if (h->net_sel < h->net_top)            { h->net_top = h->net_sel; }
            else if (h->net_sel >= h->net_top + 4u) { h->net_top = (uint8_t)(h->net_sel - 3u); }
            break;

        case KILN_HMI_SCREEN_PASSPHRASE:
            if (ev == KILN_INPUT_CW)       { step(&h->charset_sel, K_SEL_N, true); }
            else if (ev == KILN_INPUT_CCW) { step(&h->charset_sel, K_SEL_N, false); }
            else if (ev == KILN_INPUT_LONG_PRESS) {
                /* Abandon it, and take the typed passphrase with it rather
                 * than leaving half of one in memory for the next attempt. */
                h->pass_len = 0;
                h->pass[0]  = '\0';
                h->screen   = KILN_HMI_SCREEN_NETWORKS;
            } else if (ev == KILN_INPUT_PRESS) {
                if (h->charset_sel == K_SEL_DEL) {
                    if (h->pass_len > 0u) {
                        h->pass_len--;
                        h->pass[h->pass_len] = '\0';
                    }
                } else if (h->charset_sel == K_SEL_OK) {
                    /* WPA-PSK is 8 to 63 characters.  A shorter one cannot be
                     * right, and accepting it here would spend the join
                     * attempt and the operator's patience to find that out. */
                    if (h->pass_len >= 8u) {
                        act.kind          = KILN_HMI_ACT_WIFI_CONNECT;
                        act.network_index = h->net_sel;
                        h->screen         = KILN_HMI_SCREEN_NETWORK;
                    }
                } else if (h->pass_len + 1u < KILN_HMI_PASS_LEN) {
                    h->pass[h->pass_len] = k_charset[h->charset_sel];
                    h->pass_len++;
                    h->pass[h->pass_len] = '\0';
                }
            }
            break;

        default:   /* DIAG, INFO: read-only, any press returns */
            if (ev == KILN_INPUT_PRESS || ev == KILN_INPUT_LONG_PRESS) {
                h->screen = KILN_HMI_SCREEN_MENU;
            }
            break;
        }
    }

    /* The fault screen is only left when the fault has actually gone, which is
     * SWR-HMI-06's "not dismissible while the condition persists". */
    if (!faulted && h->screen == KILN_HMI_SCREEN_FAULT) {
        h->screen = KILN_HMI_SCREEN_MAIN;
        h->dirty  = true;
    }

    /* --- render ---------------------------------------------------------- */
    kiln_fb_clear(&h->fb);
    if (h->dimmed) {
        return act;              /* blank, and nothing to draw (SWR-HMI-12) */
    }

    switch (h->screen) {
    case KILN_HMI_SCREEN_FAULT:    draw_fault(h, view);   break;
    case KILN_HMI_SCREEN_MENU:
        draw_list(h, kiln_hmi_str(KILN_HMI_STR_MENU, view->language),
                      MENU_ITEMS, h->menu_sel, 0, menu_label, view);
        break;
    case KILN_HMI_SCREEN_PROGRAMS:
        if (view->program_count == 0u) {
            kiln_fb_text(&h->fb, 0, 0,
                         kiln_hmi_str(KILN_HMI_STR_PROGRAMS, view->language),
                         1, true);
            kiln_fb_hline(&h->fb, 0, 9, KILN_DISPLAY_W, true);
            kiln_fb_text(&h->fb, 0, 20,
                         kiln_hmi_str(KILN_HMI_STR_NONE_STORED, view->language),
                         1, true);
        } else {
            draw_list(h, kiln_hmi_str(KILN_HMI_STR_PROGRAMS, view->language),
                          view->program_count, h->prog_sel, h->prog_top,
                      program_label, view);
        }
        break;
    case KILN_HMI_SCREEN_CONFIRM:  draw_confirm(h, view); break;
    case KILN_HMI_SCREEN_NETWORK:    draw_network(h, view);    break;
    case KILN_HMI_SCREEN_NETWORKS:   draw_networks(h, view);   break;
    case KILN_HMI_SCREEN_PASSPHRASE: draw_passphrase(h, view); break;
    case KILN_HMI_SCREEN_DIAG:     draw_diag(h, view);    break;
    case KILN_HMI_SCREEN_INFO:     draw_info(h, view);    break;
    case KILN_HMI_SCREEN_MAIN:
    default:                       draw_main(h, view);    break;
    }
    return act;
}

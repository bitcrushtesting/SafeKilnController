/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * kiln_core/faults -- SWR-SAF-19 and requirements Appendix A.  The table's order is
 * load-bearing (the codes are stable and never reused), so this is the
 * consistency test architecture section 5.1 asks for.
 */

#include <string.h>
#include "kiln_check.h"
#include "kiln/err.h"
#include "kiln_core/faults.h"

/*
 * @relation(SWR-SAF-19, scope=function)
 */
KILN_TEST(swrsaf19_every_fault_code_has_a_label_a_cause_and_a_requirement)
{
    for (int c = KILN_FAULT_TC_OPEN; c < KILN_FAULT_MAX; c++) {
        const kiln_fault_t code = (kiln_fault_t)c;

        const char *label = kiln_fault_label(code);
        CHECK_MSG(strcmp(label, "UNKNOWN") != 0, "fault %d has no label", c);
        /* The local display is 128 px wide, so the short label has to be short. */
        CHECK_MSG(strlen(label) <= 12u, "fault %d label \"%s\" is too long", c, label);

        const char *cause = kiln_fault_cause(code);
        CHECK_MSG(strcmp(cause, "Unknown fault code.") != 0, "fault %d has no cause", c);
        /* An operator-facing cause is a sentence, not a word. */
        CHECK_MSG(strlen(cause) > 20u, "fault %d cause is too terse", c);

        const char *req = kiln_fault_requirement(code);
        CHECK_MSG(req[0] != '\0', "fault %d traces to no requirement", c);
    }
}

KILN_TEST(the_current_fault_codes_are_the_ones_appendix_a_allocates)
{
    CHECK_EQ_INT(KILN_FAULT_UNCOMMANDED_CURRENT, 21);
    CHECK_EQ_INT(KILN_FAULT_CONTACTOR_WELDED,    22);
    CHECK_EQ_INT(KILN_FAULT_NO_HEATER_CURRENT,   23);
    CHECK_EQ_INT(KILN_FAULT_CURRENT_DEVIATION,   24);
    CHECK_EQ_INT(KILN_FAULT_OVERCURRENT,         25);
    CHECK_EQ_INT(KILN_FAULT_CT_FAULT,            26);

    CHECK_STR_EQ(kiln_fault_requirement(KILN_FAULT_UNCOMMANDED_CURRENT), "SWR-SAF-25");
    CHECK_STR_EQ(kiln_fault_requirement(KILN_FAULT_CONTACTOR_WELDED),    "SWR-SAF-27");
    CHECK_STR_EQ(kiln_fault_requirement(KILN_FAULT_NO_HEATER_CURRENT),   "SWR-SAF-26");
    CHECK_STR_EQ(kiln_fault_requirement(KILN_FAULT_CURRENT_DEVIATION),   "SWR-SAF-28");
    CHECK_STR_EQ(kiln_fault_requirement(KILN_FAULT_OVERCURRENT),         "SWR-SAF-29");
    CHECK_STR_EQ(kiln_fault_requirement(KILN_FAULT_CT_FAULT),            "SWR-CUR-11");
}

/*
 * @relation(SWR-SAF-27, scope=function)
 */
KILN_TEST(swrsaf27_tells_the_operator_to_isolate_the_kiln)
{
    /* The one fault whose operator instruction is an action on the supply, not a
     * diagnosis.  If this text ever loses it, the fault becomes unactionable. */
    const char *cause = kiln_fault_cause(KILN_FAULT_CONTACTOR_WELDED);
    CHECK(strstr(cause, "ISOLATE") != NULL);
    CHECK(strstr(cause, "welded") != NULL);
}

KILN_TEST(every_warning_bit_has_a_label_and_a_cause)
{
    for (int b = 0; b < KILN_WARN_COUNT; b++) {
        const kiln_warn_bit_t bit = (kiln_warn_bit_t)b;
        CHECK_MSG(strcmp(kiln_warn_label(bit), "UNKNOWN") != 0,
                  "warning bit %d has no label", b);
        CHECK_MSG(strcmp(kiln_warn_cause(bit), "Unknown warning.") != 0,
                  "warning bit %d has no cause", b);
        CHECK_MSG(strlen(kiln_warn_label(bit)) <= 12u,
                  "warning bit %d label is too long", b);
    }
}

KILN_TEST(warning_codes_start_at_101_and_match_appendix_a)
{
    CHECK_EQ_UINT(kiln_warn_code(KILN_WARN_INSULATION),  101u);
    CHECK_EQ_UINT(kiln_warn_code(KILN_WARN_RELAY_WEAR),  109u);
    CHECK_EQ_UINT(kiln_warn_code(KILN_WARN_RELAY_SUSPECT), 110u);
    CHECK_EQ_UINT(kiln_warn_code(KILN_WARN_CURRENT_OFF), 111u);
    CHECK_EQ_UINT(kiln_warn_code(KILN_WARN_CURRENT_DEV), 112u);
}

KILN_TEST(out_of_range_codes_answer_safely)
{
    CHECK_STR_EQ(kiln_fault_label((kiln_fault_t)999), "UNKNOWN");
    CHECK_STR_EQ(kiln_fault_cause((kiln_fault_t)999), "Unknown fault code.");
    CHECK_STR_EQ(kiln_fault_requirement((kiln_fault_t)999), "");
    CHECK_STR_EQ(kiln_warn_label((kiln_warn_bit_t)99), "UNKNOWN");
    CHECK_STR_EQ(kiln_state_label((kiln_state_t)99), "?");
}

KILN_TEST(every_state_has_a_label_that_fits_the_display)
{
    for (int s = 0; s < KILN_STATE_COUNT; s++) {
        const char *l = kiln_state_label((kiln_state_t)s);
        CHECK(l[0] != '?');
        CHECK(strlen(l) <= 6u);
    }
    /* KILN_STATE_COUNT is packed into a log nibble. */
    CHECK(KILN_STATE_COUNT <= 15);
}

KILN_TEST(every_error_code_has_a_message)
{
    for (int e = 0; e < KILN_ERR_COUNT; e++) {
        const char *s = kiln_err_str((kiln_err_t)e);
        CHECK(s != NULL);
        CHECK_MSG(strcmp(s, "unknown error") != 0, "error %d has no message", e);
    }
    CHECK_STR_EQ(kiln_err_str((kiln_err_t)999), "unknown error");
}

/* --- SWR-NFR-23: German --------------------------------------------------- */

/*
 * @relation(SWR-NFR-23, scope=function)
 */
KILN_TEST(swrnfr23_every_fault_has_a_german_label_and_cause)
{
    for (int c = 0; c < KILN_FAULT_MAX; c++) {
        const kiln_fault_t f = (kiln_fault_t)c;
        const char *l = kiln_fault_label_in(f, KILN_LANG_DE);
        const char *m = kiln_fault_cause_in(f, KILN_LANG_DE);
        CHECK_MSG(l[0] != '\0', "fault %d has no German label", c);
        CHECK_MSG(m[0] != '\0', "fault %d has no German cause", c);
    }
}

/*
 * @relation(SWR-NFR-23, scope=function)
 */
KILN_TEST(swrnfr23_every_warning_has_a_german_label_and_cause)
{
    for (int b = 0; b < KILN_WARN_COUNT; b++) {
        const kiln_warn_bit_t w = (kiln_warn_bit_t)b;
        CHECK_MSG(kiln_warn_label_in(w, KILN_LANG_DE)[0] != '\0',
                  "warning %d has no German label", b);
        CHECK_MSG(kiln_warn_cause_in(w, KILN_LANG_DE)[0] != '\0',
                  "warning %d has no German cause", b);
    }
}

/*
 * @relation(SWR-NFR-23, scope=function)
 */
KILN_TEST(swrnfr23_german_text_actually_differs_from_english)
{
    /* A table copied from the English column would pass the emptiness checks
     * above and be useless.  Every cause must actually have been translated;
     * a handful of labels legitimately coincide (OK, WATCHDOG, PHASE?). */
    int same = 0;
    for (int c = 1; c < KILN_FAULT_MAX; c++) {
        const kiln_fault_t f = (kiln_fault_t)c;
        if (strcmp(kiln_fault_cause_in(f, KILN_LANG_EN),
                   kiln_fault_cause_in(f, KILN_LANG_DE)) == 0) {
            same++;
        }
    }
    CHECK_MSG(same == 0, "%d fault causes are identical in both languages", same);
}

/*
 * @relation(SWR-NFR-23, scope=function)
 */
KILN_TEST(swrnfr23_the_unsuffixed_calls_stay_english)
{
    /* Diagnostics, the log and requirement traceability should read the same
     * whoever filed the report. */
    CHECK_STR_EQ(kiln_fault_label(KILN_FAULT_DOOR_OPEN),
                 kiln_fault_label_in(KILN_FAULT_DOOR_OPEN, KILN_LANG_EN));
    CHECK(strcmp(kiln_fault_label(KILN_FAULT_DOOR_OPEN),
                 kiln_fault_label_in(KILN_FAULT_DOOR_OPEN, KILN_LANG_DE)) != 0);
}

/*
 * @relation(SWR-NFR-23, scope=function)
 */
KILN_TEST(swrnfr23_an_unknown_language_falls_back_to_english)
{
    /* An operator who sees English has a worse day than one who sees German;
     * an operator who sees nothing cannot act at all. */
    CHECK_STR_EQ(kiln_fault_label_in(KILN_FAULT_OVERTEMP, (kiln_lang_t)99),
                 kiln_fault_label_in(KILN_FAULT_OVERTEMP, KILN_LANG_EN));
    CHECK_STR_EQ(kiln_lang_tag((kiln_lang_t)99), "en");
    CHECK_STR_EQ(kiln_lang_tag(KILN_LANG_DE), "de");
}

/*
 * @relation(SWR-NFR-23, scope=function)
 */
KILN_TEST(swrnfr23_german_labels_still_fit_the_display)
{
    /* FR-HMI: the label column is 16 characters on a 128 px display, and a
     * translation that overflows it is a translation that cannot be shown. */
    for (int c = 0; c < KILN_FAULT_MAX; c++) {
        const char *l = kiln_fault_label_in((kiln_fault_t)c, KILN_LANG_DE);
        CHECK_MSG(strlen(l) <= 16, "German label '%s' is %zu chars, max 16",
                  l, strlen(l));
    }
    for (int b = 0; b < KILN_WARN_COUNT; b++) {
        const char *l = kiln_warn_label_in((kiln_warn_bit_t)b, KILN_LANG_DE);
        CHECK_MSG(strlen(l) <= 16, "German warning label '%s' is %zu chars, max 16",
                  l, strlen(l));
    }
}

/* ===========================================================================
 * WHAT THE PANEL CAN ACTUALLY SHOW  (SWR-NFR-23, SWR-HMI-06, tasklist M4/J1)
 * ===========================================================================
 * Two properties of this table that nothing checked, both of which failed.
 *
 * The 5x7 font in kiln_hmi carries the 95 printable ASCII glyphs and nothing
 * else. The labels had always been transliterated -- VERSCHWEISST, KEINE
 * WAERME -- and the long causes had not: 31 of them contained umlauts, which
 * reach the panel as blanks. A German operator read a fault screen with holes
 * in the instruction.
 *
 * And the fault screen holds five rows of 21 characters. The full causes run
 * to 279, because the browser shows them as a paragraph and does so well. On
 * the panel the tail was simply dropped, and for KILN_FAULT_CONTACTOR_WELDED
 * the tail was "ISOLATE THE KILN AT ITS SUPPLY NOW".
 *
 * These two tests are the reason the panel form exists, and they are written
 * to name the offender: a bound nobody can fail is not a bound, and one that
 * fails without saying which string is a bound nobody can fix.
 */

/* The fault screen's own word wrap, which is what the bound has to be measured
 * against rather than a character count: it breaks on spaces, so a long word
 * can waste most of a row. Returns the rows used, or 0 if a single word cannot
 * fit a row at all. */
static unsigned wrapped_rows(const char *s)
{
    unsigned rows = 0;
    while (*s != '\0') {
        unsigned take = 0, last_space = 0;
        while (take < KILN_PANEL_COLS && s[take] != '\0') {
            if (s[take] == ' ') { last_space = take; }
            take++;
        }
        if (s[take] != '\0' && last_space > 0) { take = last_space; }
        else if (s[take] != '\0' && last_space == 0) {
            /* A word longer than a row. The screen cuts it mid-word, which the
             * wrap's own comment calls out as unacceptable: a word split in a
             * hurry reads as a different word, and German compounds are where
             * this happens -- "Thermoelement-Messeingang" is 25 characters.
             * Reported as its own failure rather than counted as rows. */
            return 0;
        }
        rows++;
        s += take;
        while (*s == ' ') { s++; }
    }
    return rows;
}

/*
 * @relation(SWR-NFR-23, scope=function)
 */
KILN_TEST(swrnfr23_every_string_in_the_table_is_ascii_the_font_can_draw)
{
    for (unsigned lang = 0; lang < (unsigned)KILN_LANG_COUNT; lang++) {
        const kiln_lang_t l = (kiln_lang_t)lang;
        for (unsigned f = 0; f < (unsigned)KILN_FAULT_MAX; f++) {
            const char *strs[] = { kiln_fault_label_in((kiln_fault_t)f, l),
                                   kiln_fault_cause_in((kiln_fault_t)f, l),
                                   kiln_fault_panel_in((kiln_fault_t)f, l) };
            for (size_t i = 0; i < 3; i++) {
                for (const char *c = strs[i]; *c != '\0'; c++) {
                    const unsigned char u = (unsigned char)*c;
                    CHECK_MSG(u >= 0x20u && u <= 0x7Eu,
                              "fault %u lang %u string %zu has byte 0x%02X: "
                              "the 5x7 font cannot draw it", f, lang, i,
                              (unsigned)u);
                }
            }
        }
        for (unsigned w = 0; w < (unsigned)KILN_WARN_COUNT; w++) {
            const char *strs[] = { kiln_warn_label_in((kiln_warn_bit_t)w, l),
                                   kiln_warn_cause_in((kiln_warn_bit_t)w, l),
                                   kiln_warn_panel_in((kiln_warn_bit_t)w, l) };
            for (size_t i = 0; i < 3; i++) {
                for (const char *c = strs[i]; *c != '\0'; c++) {
                    const unsigned char u = (unsigned char)*c;
                    CHECK_MSG(u >= 0x20u && u <= 0x7Eu,
                              "warning %u lang %u string %zu has byte 0x%02X",
                              w, lang, i, (unsigned)u);
                }
            }
        }
        for (unsigned r = 0; r < (unsigned)KILN_SUP_REASON_COUNT; r++) {
            const char *strs[] = { kiln_sup_reason_label_in((kiln_sup_reason_t)r, l),
                                   kiln_sup_reason_cause_in((kiln_sup_reason_t)r, l),
                                   kiln_sup_reason_panel_in((kiln_sup_reason_t)r, l) };
            for (size_t i = 0; i < 3; i++) {
                for (const char *c = strs[i]; *c != '\0'; c++) {
                    const unsigned char u = (unsigned char)*c;
                    CHECK_MSG(u >= 0x20u && u <= 0x7Eu,
                              "sup reason %u lang %u string %zu has byte 0x%02X",
                              r, lang, i, (unsigned)u);
                }
            }
        }
    }
}

/*
 * @relation(SWR-HMI-06, scope=function)
 */
KILN_TEST(swrhmi06_every_panel_cause_fits_the_fault_screen_whole)
{
    /* SWR-HMI-06 says the fault screen is not dismissible while the condition
     * holds, which is only worth anything if what it shows is complete. */
    for (unsigned lang = 0; lang < (unsigned)KILN_LANG_COUNT; lang++) {
        const kiln_lang_t l = (kiln_lang_t)lang;

        for (unsigned f = 0; f < (unsigned)KILN_FAULT_MAX; f++) {
            const char *s = kiln_fault_panel_in((kiln_fault_t)f, l);
            const unsigned rows = wrapped_rows(s);
            CHECK_MSG(rows > 0 && rows <= KILN_PANEL_ROWS,
                      "fault %u lang %u: %s (%u chars) -- \"%s\"", f, lang,
                      rows == 0 ? "a word is longer than a row"
                                : "too many rows for the panel",
                      (unsigned)strlen(s), s);
        }
        for (unsigned w = 0; w < (unsigned)KILN_WARN_COUNT; w++) {
            const char *s = kiln_warn_panel_in((kiln_warn_bit_t)w, l);
            const unsigned rows = wrapped_rows(s);
            CHECK_MSG(rows > 0 && rows <= KILN_PANEL_ROWS,
                      "warning %u lang %u needs %u rows (%u chars): \"%s\"",
                      w, lang, rows, (unsigned)strlen(s), s);
        }
        for (unsigned r = 0; r < (unsigned)KILN_SUP_REASON_COUNT; r++) {
            const char *s = kiln_sup_reason_panel_in((kiln_sup_reason_t)r, l);
            const unsigned rows = wrapped_rows(s);
            CHECK_MSG(rows > 0 && rows <= KILN_PANEL_ROWS,
                      "sup reason %u lang %u needs %u rows (%u chars): \"%s\"",
                      r, lang, rows, (unsigned)strlen(s), s);
        }
    }
}

/*
 * @relation(SWR-NFR-23, scope=function)
 */
KILN_TEST(swrnfr23_the_panel_form_says_the_same_thing_as_the_cause)
{
    /* A short form is allowed to be terser; it is not allowed to be about
     * something else, and it is not allowed to be empty. The check that can be
     * automated is that where a panel form exists it is genuinely shorter than
     * the cause it replaces, and that the fallback works where none exists --
     * the rest is review, which is what the table being one readable block is
     * for. */
    for (unsigned lang = 0; lang < (unsigned)KILN_LANG_COUNT; lang++) {
        const kiln_lang_t l = (kiln_lang_t)lang;
        for (unsigned f = 0; f < (unsigned)KILN_FAULT_MAX; f++) {
            const char *full  = kiln_fault_cause_in((kiln_fault_t)f, l);
            const char *panel = kiln_fault_panel_in((kiln_fault_t)f, l);
            CHECK(panel[0] != '\0');
            if (panel != full) {
                CHECK_MSG(strlen(panel) < strlen(full),
                          "fault %u lang %u has a panel form no shorter than "
                          "its cause, so it buys nothing", f, lang);
            }
        }
    }
}

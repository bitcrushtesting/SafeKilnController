/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Fault and warning text (SWR-SAF-19, requirements appendix A).  A single table so
 * the display, the API and the documentation cannot disagree.
 *
 * SWR-NFR-23: the table is indexed by kiln_lang_t, and it is the only place in the
 * firmware holding operator-facing prose.  Adding a language is a column here
 * and nowhere else.
 *
 * The unsuffixed calls return English and are what diagnostics, the log and
 * the requirement traceability use: a fault code quoted in a bug report should
 * read the same whoever filed it.  The _in() forms take a language and are
 * what the API uses to talk to the operator.
 *
 * ---------------------------------------------------------------------------
 * WHY THERE IS A SECOND, SHORTER FORM
 * ---------------------------------------------------------------------------
 * The full causes run to 279 characters.  In a browser that is a paragraph and
 * reads well; on the fault screen it is **five lines of 21 characters**, and
 * what falls off the bottom of a 128x64 panel is the end of the sentence --
 * which in an instruction is the instruction.  "ISOLATE THE KILN AT ITS SUPPLY
 * NOW" was the part being silently dropped.
 *
 * So each entry may carry a panel form as well: the same meaning, trimmed to
 * what the screen can hold, action first.  Where the full cause already fits,
 * there is no second string to get out of step with the first.  The _panel_in()
 * calls return the short form when there is one and the full cause when there
 * is not, so the display asks for one thing and always gets something it can
 * render completely.
 *
 * Both forms are plain ASCII, including the German.  The 5x7 font in kiln_hmi
 * carries the 95 printable ASCII glyphs and nothing else, so German is
 * transliterated -- AE OE UE SS.  That had been true of the labels and not of
 * the causes: 31 of them contained umlauts that reached the panel as blanks.
 * A test now holds the whole table to it.
 */
#ifndef KILN_CORE_FAULTS_H
#define KILN_CORE_FAULTS_H

#include "kiln/types.h"
#include "kiln_ports/port_supervisor.h"

/* Short label, fits the 128 px display. */
const char *kiln_fault_label(kiln_fault_t code);
const char *kiln_fault_label_in(kiln_fault_t code, kiln_lang_t lang);
/* Full operator-facing cause, for the API and the browser. */
const char *kiln_fault_cause(kiln_fault_t code);
const char *kiln_fault_cause_in(kiln_fault_t code, kiln_lang_t lang);
/* The same thing trimmed to what the fault screen can display in full. */
const char *kiln_fault_panel_in(kiln_fault_t code, kiln_lang_t lang);
/* The requirement that mandates the detection, for diagnostics and traceability. */
const char *kiln_fault_requirement(kiln_fault_t code);

const char *kiln_warn_label(kiln_warn_bit_t bit);
const char *kiln_warn_label_in(kiln_warn_bit_t bit, kiln_lang_t lang);
const char *kiln_warn_cause(kiln_warn_bit_t bit);
const char *kiln_warn_cause_in(kiln_warn_bit_t bit, kiln_lang_t lang);
const char *kiln_warn_panel_in(kiln_warn_bit_t bit, kiln_lang_t lang);
static inline uint16_t kiln_warn_code(kiln_warn_bit_t bit)
{
    return (uint16_t)(KILN_WARN_CODE_BASE + (uint16_t)bit);
}

const char *kiln_state_label(kiln_state_t state);

/* --- the independent supervisor (SWA-22) ---------------------------------
 *
 * Why the supervisor stopped the kiln, in the operator's language.  Separate
 * from the fault table because these are not this firmware's faults: the
 * supervisor acted on its own, and the operator needs to be told that the
 * button on the panel is what clears it, not the usual acknowledgement. */
const char *kiln_sup_reason_label(kiln_sup_reason_t r);
const char *kiln_sup_reason_label_in(kiln_sup_reason_t r, kiln_lang_t lang);
const char *kiln_sup_reason_cause(kiln_sup_reason_t r);
const char *kiln_sup_reason_cause_in(kiln_sup_reason_t r, kiln_lang_t lang);
const char *kiln_sup_reason_panel_in(kiln_sup_reason_t r, kiln_lang_t lang);

/* What the fault screen can actually render: five rows of 21 characters, from
 * the 128x64 panel at scale 1 with the banner above and the wrap stepping 9 px
 * a line.  Exported because the bound belongs to the display and the strings
 * that have to respect it live here; a test asserts every panel form fits it
 * under the same word wrap the screen uses. */
constexpr unsigned KILN_PANEL_COLS = 21u;
constexpr unsigned KILN_PANEL_ROWS = 5u;

/* The BCP-47-ish tag for a language, for the API and the HTML lang attribute. */
const char *kiln_lang_tag(kiln_lang_t lang);

#endif

/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Fault and warning text (SR-19, requirements appendix A).  A single table so
 * the display, the API and the documentation cannot disagree.
 *
 * NFR-23: the table is indexed by kiln_lang_t, and it is the only place in the
 * firmware holding operator-facing prose.  Adding a language is a column here
 * and nowhere else.
 *
 * The unsuffixed calls return English and are what diagnostics, the log and
 * the requirement traceability use: a fault code quoted in a bug report should
 * read the same whoever filed it.  The _in() forms take a language and are
 * what the display and the API use to talk to the operator.
 */
#ifndef KILN_CORE_FAULTS_H
#define KILN_CORE_FAULTS_H

#include "kiln/types.h"
#include "kiln_ports/port_supervisor.h"

/* Short label, fits the 128 px display. */
const char *kiln_fault_label(kiln_fault_t code);
const char *kiln_fault_label_in(kiln_fault_t code, kiln_lang_t lang);
/* Full operator-facing cause. */
const char *kiln_fault_cause(kiln_fault_t code);
const char *kiln_fault_cause_in(kiln_fault_t code, kiln_lang_t lang);
/* The requirement that mandates the detection, for diagnostics and traceability. */
const char *kiln_fault_requirement(kiln_fault_t code);

const char *kiln_warn_label(kiln_warn_bit_t bit);
const char *kiln_warn_label_in(kiln_warn_bit_t bit, kiln_lang_t lang);
const char *kiln_warn_cause(kiln_warn_bit_t bit);
const char *kiln_warn_cause_in(kiln_warn_bit_t bit, kiln_lang_t lang);
static inline uint16_t kiln_warn_code(kiln_warn_bit_t bit)
{
    return (uint16_t)(KILN_WARN_CODE_BASE + (uint16_t)bit);
}

const char *kiln_state_label(kiln_state_t state);

/* --- the independent supervisor (AD-22) ---------------------------------
 *
 * Why the supervisor stopped the kiln, in the operator's language.  Separate
 * from the fault table because these are not this firmware's faults: the
 * supervisor acted on its own, and the operator needs to be told that the
 * button on the panel is what clears it, not the usual acknowledgement. */
const char *kiln_sup_reason_label(kiln_sup_reason_t r);
const char *kiln_sup_reason_label_in(kiln_sup_reason_t r, kiln_lang_t lang);
const char *kiln_sup_reason_cause(kiln_sup_reason_t r);
const char *kiln_sup_reason_cause_in(kiln_sup_reason_t r, kiln_lang_t lang);

/* The BCP-47-ish tag for a language, for the API and the HTML lang attribute. */
const char *kiln_lang_tag(kiln_lang_t lang);

#endif

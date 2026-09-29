/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Fault and warning text (SR-19, requirements appendix A).  A single table so
 * the display, the API and the documentation cannot disagree.
 */
#ifndef KILN_CORE_FAULTS_H
#define KILN_CORE_FAULTS_H

#include "kiln/types.h"

/* Short label, fits the 128 px display. */
const char *kiln_fault_label(kiln_fault_t code);
/* Full operator-facing cause. */
const char *kiln_fault_cause(kiln_fault_t code);
/* The requirement that mandates the detection, for diagnostics and traceability. */
const char *kiln_fault_requirement(kiln_fault_t code);

const char *kiln_warn_label(kiln_warn_bit_t bit);
const char *kiln_warn_cause(kiln_warn_bit_t bit);
static inline uint16_t kiln_warn_code(kiln_warn_bit_t bit)
{
    return (uint16_t)(KILN_WARN_CODE_BASE + (uint16_t)bit);
}

const char *kiln_state_label(kiln_state_t state);

#endif

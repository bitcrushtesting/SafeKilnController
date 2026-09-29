/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Firing program model and validation -- FR-PRG-01, FR-PRG-02, FR-PRG-05.
 *
 * JSON encode/decode is deliberately NOT here: it is a presentation concern and
 * lives in kiln_web, so this component stays free of any parser dependency and
 * remains trivially host-testable.  (Refines architecture section 5.1.)
 */
#ifndef KILN_CORE_PROFILE_H
#define KILN_CORE_PROFILE_H

#include "kiln/err.h"
#include "kiln/types.h"

#define KILN_DWELL_MIN_MAX       5999u    /* FR-PRG-02 */
#define KILN_RATE_MAX_C_PER_H    9999u    /* FR-CTL-10 */
#define KILN_PROGRAM_MAX_DURATION_S (168u * 3600u)  /* FR-PRG-05 */

typedef enum {
    KILN_PROG_OK = 0,
    KILN_PROG_ERR_NO_SEGMENTS,
    KILN_PROG_ERR_TOO_MANY_SEGMENTS,
    KILN_PROG_ERR_TARGET_ABOVE_MAX,
    KILN_PROG_ERR_TARGET_ABOVE_CEILING,
    KILN_PROG_ERR_RATE_RANGE,
    KILN_PROG_ERR_DWELL_RANGE,
    KILN_PROG_ERR_NAME_EMPTY,
    KILN_PROG_ERR_TOO_LONG,
    KILN_PROG_ERR_SCHEMA,
} kiln_prog_valid_t;

typedef struct {
    kiln_prog_valid_t code;
    uint8_t           segment;     /* offending segment, KILN_SEG_NONE if n/a */
} kiln_prog_validation_t;

/* FR-PRG-05.  max_temp_c is the configured kiln maximum; the compile-time
 * ceiling of SR-23 is enforced regardless of what is passed in. */
kiln_prog_validation_t kiln_profile_validate(const kiln_program_t *p, float max_temp_c);
const char *kiln_profile_validation_str(kiln_prog_valid_t code);

/* FR-PRG-06: predicted duration assuming the kiln keeps up, starting from
 * start_c (the present temperature, or ambient before a run begins). */
uint32_t kiln_profile_duration_s(const kiln_program_t *p, float start_c);

/* Peak target across the program, for a pre-start sanity display. */
float kiln_profile_peak_c(const kiln_program_t *p);

void kiln_profile_init_empty(kiln_program_t *p, const char *name);

/* Built-in read-only examples (FR-PRG-09).  Returns count; index < count. */
uint8_t kiln_profile_example_count(void);
kiln_err_t kiln_profile_example(uint8_t index, kiln_program_t *out);

#endif

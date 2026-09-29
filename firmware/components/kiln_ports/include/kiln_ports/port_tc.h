/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Thermocouple front end (MAX31856 on target; simulator or stub in tests).
 */
#ifndef KILN_PORT_TC_H
#define KILN_PORT_TC_H

#include "kiln/err.h"
#include "kiln/types.h"

typedef struct {
    float    temp_c;        /* hot junction, linearised, CJ compensated  */
    float    cj_c;          /* cold junction (FR-ACQ-04)                 */
    uint16_t fault_bits;    /* KILN_TC_FAULT_*  (FR-ACQ-10)              */
} kiln_tc_reading_t;

typedef struct kiln_port_tc {
    void *ctx;
    /* FR-ACQ-02 / FR-ACQ-06 */
    kiln_err_t (*configure)(void *ctx, kiln_tc_type_t type, uint8_t line_filter_hz);
    kiln_err_t (*read)(void *ctx, kiln_tc_reading_t *out);
} kiln_port_tc_t;

#endif

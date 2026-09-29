/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Buzzer / auxiliary relay (HR-09).  SR-20 requires fault and completion to be
 * audibly distinguishable, hence a pattern rather than a level.
 */
#ifndef KILN_PORT_ALARM_H
#define KILN_PORT_ALARM_H

#include "kiln/types.h"

typedef enum {
    KILN_ALARM_OFF = 0,
    KILN_ALARM_COMPLETE,    /* slow intermittent  */
    KILN_ALARM_FAULT,       /* fast urgent        */
} kiln_alarm_pattern_t;

typedef struct kiln_port_alarm {
    void *ctx;
    void (*set)(void *ctx, kiln_alarm_pattern_t pattern);
} kiln_port_alarm_t;

#endif

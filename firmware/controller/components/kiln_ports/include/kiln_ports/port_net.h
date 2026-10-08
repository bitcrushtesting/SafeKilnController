/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Connectivity status for the HMI and the API (SWR-NET-09).
 */
#ifndef KILN_PORT_NET_H
#define KILN_PORT_NET_H

#include "kiln/err.h"
#include "kiln/types.h"

typedef enum {
    KILN_NET_DOWN = 0,
    KILN_NET_CONNECTING,
    KILN_NET_STA_CONNECTED,
    KILN_NET_AP_FALLBACK,      /* SWR-NET-02 */
} kiln_net_state_t;

typedef struct {
    kiln_net_state_t state;
    char             ssid[33];
    char             ip[16];
    char             hostname[32];
    int8_t           rssi;
    uint32_t         disconnect_count;
    uint32_t         uptime_s;
    bool             time_synced;
} kiln_net_status_t;

typedef struct kiln_port_net {
    void *ctx;
    kiln_err_t (*status)(void *ctx, kiln_net_status_t *out);
} kiln_port_net_t;

#endif

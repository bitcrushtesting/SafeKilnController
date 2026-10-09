/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Connectivity: status, scanning and joining a network (SWR-NET-09, SWR-NET-11,
 * SWR-NET-12).
 *
 * ---------------------------------------------------------------------------
 * There is no access point here, and that is the design
 * ---------------------------------------------------------------------------
 * The device used to fall back to its own AP and serve a provisioning page on
 * it (the old SWR-NET-02).  That is gone: a kiln controller that raises an
 * access point is a second network interface, with its own passphrase to get
 * wrong, reachable by anybody in the building, serving a page whose whole
 * purpose is to accept credentials.  The display and the encoder are already
 * there, the operator is already standing at them, and what they can do there
 * no passer-by can do at all.
 *
 * So the device scans, the operator picks a network and types the passphrase on
 * the local display, and the only radio the firmware ever brings up is a
 * station.  Whoever can set up the WiFi is whoever can reach the kiln.
 */
#ifndef KILN_PORT_NET_H
#define KILN_PORT_NET_H

#include "kiln/err.h"
#include "kiln/types.h"

typedef enum {
    KILN_NET_DOWN = 0,
    KILN_NET_CONNECTING,
    KILN_NET_STA_CONNECTED,
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
    /* SWR-NET-12.  The radio's own reason for the last disconnect or refused
     * join -- IEEE 802.11 reason codes as the driver reports them, plus the
     * vendor range above 200 that carries "no AP found" and "handshake
     * timeout", which are the two an operator at the display will actually
     * meet.  Zero means none has been recorded, which is also what a device
     * that has never tried to associate says.
     *
     * Carried because the display had nothing to show when a join failed: it
     * returned to the network screen saying "not connected", leaving the
     * operator to guess between a typed passphrase that was wrong, an SSID
     * that is out of range, and a radio that never came up.  With the setup at
     * the display the only route in (there is no access point), guessing is the
     * whole cost of getting it wrong. */
    uint8_t          last_reason;
} kiln_net_status_t;

/* The most networks a scan will report.  Sixteen rather than "all of them":
 * the display shows a handful, the records are ~80 bytes each inside the
 * driver, and a dense band can return fifty. */
constexpr uint8_t KILN_NET_SCAN_MAX = 16;

/* One network the scan found.  `secured` drives whether the operator is asked
 * for a passphrase at all, so it is carried rather than inferred from an
 * authmode enum the core would have to know about. */
typedef struct {
    char   ssid[33];
    int8_t rssi;
    bool   secured;
} kiln_net_ap_t;

typedef struct kiln_port_net {
    void *ctx;
    kiln_err_t (*status)(void *ctx, kiln_net_status_t *out);

    /* SWR-NET-11.  `scan_begin` returns at once; `scan_busy` says whether the
     * radio is still looking, so the display can say "scanning" rather than
     * "no networks found" during the two seconds it takes.  A scan runs while
     * a firing runs: it is a receive-only operation on the other core and
     * SWR-NET-07 already forbids the network from touching a run. */
    kiln_err_t (*scan_begin)(void *ctx);
    bool       (*scan_busy)(void *ctx);
    /* Writes at most `max` entries, strongest first, and returns how many. */
    uint8_t    (*scan_results)(void *ctx, kiln_net_ap_t *out, uint8_t max);

    /* SWR-NET-12.  Join this network now.  The caller persists the credentials;
     * this only changes what the radio is doing, so a failed join leaves the
     * stored configuration to be corrected rather than silently kept. */
    kiln_err_t (*connect)(void *ctx, const char *ssid, const char *pass);
} kiln_port_net_t;

#endif

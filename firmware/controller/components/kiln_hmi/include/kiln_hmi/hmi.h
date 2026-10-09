/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Local display and encoder: screen composition, menus, confirmation flows
 * (SWR-HMI-02..SWR-HMI-15, architecture 5.4).
 *
 * ---------------------------------------------------------------------------
 * This component commands nothing
 * ---------------------------------------------------------------------------
 * kiln_hmi takes a view of the world and an encoder event, renders a frame and
 * returns an **action** for the caller to carry out.  It never calls kiln_app.
 *
 * That is not ceremony.  It means every screen and every menu path is a pure
 * function of (view, event) and can be driven by a host test without an
 * application, a display or a kiln -- which is SWR-TST-01 applied to the one part
 * of the firmware that would otherwise need a person looking at it.  It also
 * keeps the thing that *decides* to start a firing in kiln_app, where the
 * state machine and its guards already live, rather than splitting that
 * decision across two components.
 *
 * The caller's loop is:
 *
 *     kiln_hmi_action_t a = kiln_hmi_update(&hmi, &view, event, dt_ms);
 *     if (a.kind != KILN_HMI_ACT_NONE) { ...execute against kiln_app... }
 *     if (kiln_hmi_dirty(&hmi)) { display->present(ctx, kiln_hmi_frame(&hmi), ...); }
 */
#ifndef KILN_HMI_H
#define KILN_HMI_H

#include "kiln/types.h"
#include "kiln_core/faults.h"
#include "kiln_hmi/draw.h"
#include "kiln_ports/port_input.h"

constexpr size_t KILN_HMI_MAX_PROGRAMS = 20;  /* FR-PRG: the store's slot count */
constexpr size_t KILN_HMI_NAME_LEN     = 24;

/* SWR-NET-11.  Twelve because the screen shows four at a time and a studio sees
 * a handful: a list long enough to need a minute of turning is a list nobody
 * reads to the end.  The strongest are kept, which is the order the scan
 * returns them in. */
constexpr size_t KILN_HMI_MAX_NETWORKS = 12;
constexpr size_t KILN_HMI_SSID_LEN     = 33;  /* 32 octets and a NUL          */
constexpr size_t KILN_HMI_PASS_LEN     = 64;  /* WPA-PSK is 8..63 and a NUL   */

/* What the operator may set in motion from the local input (SWR-HMI-10). */
typedef enum {
    KILN_HMI_ACT_NONE = 0,
    KILN_HMI_ACT_START,          /* program_index selects the stored program */
    KILN_HMI_ACT_PAUSE,
    KILN_HMI_ACT_RESUME,
    KILN_HMI_ACT_ABORT,
    KILN_HMI_ACT_ACK_FAULT,
    KILN_HMI_ACT_ACK_SEGMENT,
    /* SWR-NET-11, SWR-NET-12.  The caller runs the scan and the join, because
     * this component commands nothing; it only says what the operator asked
     * for.  CONNECT names the network by its index into the view's scan
     * results, and the passphrase is read with kiln_hmi_passphrase() rather
     * than copied through this struct, which is returned by value ten times a
     * second. */
    KILN_HMI_ACT_WIFI_SCAN,
    KILN_HMI_ACT_WIFI_CONNECT,
} kiln_hmi_action_kind_t;

typedef struct {
    kiln_hmi_action_kind_t kind;
    uint8_t                program_index;
    uint8_t                network_index;
} kiln_hmi_action_t;

/* Everything the screens display, gathered by the caller once per refresh.
 * A view and not a pointer to the application, so a test can describe any
 * situation it likes without constructing one. */
typedef struct {
    kiln_snapshot_t snap;

    kiln_fault_t    fault;
    uint32_t        warnings;        /* KILN_WARN_BIT mask */

    /* SWA-22.  The supervisor acts on its own, so when it is the reason the
     * kiln stopped, the fault screen has to say so: its latch is cleared by
     * the button on the panel and not by the acknowledgement the operator has
     * learned for every other fault. */
    bool              sup_fitted;
    kiln_sup_reason_t sup_reason;
    bool              sup_tripped;
    kiln_lang_t     language;        /* SWR-NFR-23, SWR-HMI-15  */
    bool            fahrenheit;      /* SWR-HMI-13          */

    /* SWR-HMI-04 */
    uint32_t        elapsed_s;
    uint32_t        remaining_s;
    bool            awaiting_ack;

    /* SWR-HMI-07 */
    bool            net_up;
    char            hostname[32];
    char            ip[16];
    char            net_ssid[KILN_HMI_SSID_LEN];   /* the one it is joined to */

    /* SWR-NET-11: what the last scan found.  `net_scanning` is the difference
     * between "no networks here" and "ask again in a moment", which is the
     * whole of what an operator needs during the two seconds a scan takes. */
    bool            net_scanning;
    uint8_t         net_count;
    char            net_list_ssid[KILN_HMI_MAX_NETWORKS][KILN_HMI_SSID_LEN];
    int8_t          net_list_rssi[KILN_HMI_MAX_NETWORKS];
    bool            net_list_secured[KILN_HMI_MAX_NETWORKS];

    /* SWR-HMI-08 */
    char            version[24];
    uint32_t        uptime_s;
    float           kp, ki, kd;
    bool            gains_tuned;
    double          power_w;
    double          energy_wh;

    /* Program selection (SWR-HMI-10) */
    uint8_t         program_count;
    char            program_name[KILN_HMI_MAX_PROGRAMS][KILN_HMI_NAME_LEN];
} kiln_hmi_view_t;

/* Screens.  Exposed because the tests assert on which one is showing, which is
 * cheaper and far clearer than inferring it from pixels. */
typedef enum {
    KILN_HMI_SCREEN_MAIN = 0,
    KILN_HMI_SCREEN_FAULT,
    KILN_HMI_SCREEN_MENU,
    KILN_HMI_SCREEN_PROGRAMS,
    KILN_HMI_SCREEN_CONFIRM,
    KILN_HMI_SCREEN_NETWORK,
    KILN_HMI_SCREEN_NETWORKS,     /* SWR-NET-11: the scan list             */
    KILN_HMI_SCREEN_PASSPHRASE,   /* SWR-NET-12: one knob, one character   */
    KILN_HMI_SCREEN_DIAG,
    KILN_HMI_SCREEN_INFO,
    KILN_HMI_SCREEN_COUNT,
} kiln_hmi_screen_t;

typedef struct {
    kiln_hmi_screen_t screen;
    uint8_t           menu_sel;
    uint8_t           prog_sel;
    uint8_t           prog_top;        /* first visible row, for scrolling */
    bool              confirm_yes;
    kiln_hmi_action_kind_t pending;    /* what CONFIRM will do if accepted */
    uint8_t           pending_program;

    /* SWR-NET-11, SWR-NET-12 */
    uint8_t           net_sel;         /* row in the scan list             */
    uint8_t           net_top;         /* first visible row                */
    uint8_t           pass_len;
    uint8_t           charset_sel;     /* which character the knob is on   */
    char              pass[KILN_HMI_PASS_LEN];

    /* SWR-HMI-12 */
    uint32_t          idle_ms;
    uint16_t          dim_timeout_s;
    bool              dimmed;

    bool              dirty;
    kiln_fb_t         fb;
} kiln_hmi_t;

void kiln_hmi_init(kiln_hmi_t *h, uint16_t dim_timeout_s);

/* One refresh.  Advances the state machine, renders, and returns whatever the
 * operator asked for.  dt_ms drives only the dim timeout; nothing else here
 * depends on time, which is what keeps the screens testable (SWA-02). */
kiln_hmi_action_t kiln_hmi_update(kiln_hmi_t *h, const kiln_hmi_view_t *view,
                                  kiln_input_event_t ev, uint32_t dt_ms);

static inline const uint8_t *kiln_hmi_frame(const kiln_hmi_t *h) { return h->fb.px; }
static inline bool kiln_hmi_dirty(const kiln_hmi_t *h)           { return h->dirty; }
static inline void kiln_hmi_clear_dirty(kiln_hmi_t *h)           { h->dirty = false; }
/* SWR-HMI-12: blanked after the timeout, and never while a fault is up. */
static inline bool kiln_hmi_dimmed(const kiln_hmi_t *h)          { return h->dimmed; }

/* The passphrase the operator has typed, valid when a KILN_HMI_ACT_WIFI_CONNECT
 * is returned and empty for an open network.  It stays in this struct rather
 * than travelling in the action because an action is returned by value on every
 * refresh, and because the one copy that exists should be the one being
 * edited. */
static inline const char *kiln_hmi_passphrase(const kiln_hmi_t *h) { return h->pass; }

#endif /* KILN_HMI_H */

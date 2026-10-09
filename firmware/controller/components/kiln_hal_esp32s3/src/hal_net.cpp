/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * WiFi station, scanning, joining and SNTP (SWR-NET-01..SWR-NET-12,
 * except SWR-NET-04: see the note on mDNS below).
 *
 * ---------------------------------------------------------------------------
 * SWR-NET-07 is the requirement that shapes this file
 * ---------------------------------------------------------------------------
 * "Loss of WiFi, of the internet, or of time sync shall not interrupt, pause
 * or otherwise alter a running firing."  So nothing here blocks, nothing here
 * is on the control path, and nothing here can fail in a way the application
 * has to care about.  The whole component runs on core 0 (SWA-15) and the
 * application reads a status struct; there is no call from this file into
 * kiln_app at all.
 *
 * The retry is the usual place this requirement gets broken.  An adapter that
 * retried in a tight loop, or blocked a task waiting for an association, would
 * satisfy "connects to WiFi" and quietly violate SWR-NET-07.  Reconnection here
 * is driven by events with an exponential backoff (SWR-NET-03), so a kiln in a
 * shed with no reception spends its firing idle rather than busy.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "kiln_hal/hal_esp32s3.h"

namespace {

const char *TAG = "hal_net";

} // namespace

/* SWR-NET-03: 1 s doubling to 60 s.  The cap matters more than the curve: a
 * kiln may fire for a week, and a device that kept trying every second for
 * that long would spend real power on it. */
constexpr uint32_t RETRY_MIN_MS = 1000u;
constexpr uint32_t RETRY_MAX_MS = 60000u;

namespace {

typedef struct {
    kiln_net_state_t state;
    char      ssid[33];
    char      ip[16];
    char      hostname[32];
    int8_t    rssi;
    uint32_t  disconnects;
    uint8_t   last_reason;        /* SWR-NET-09 */
    int64_t   up_since_us;
    bool      time_synced;

    uint32_t  retry_ms;
    int64_t   first_attempt_us;
    bool      scanning;           /* SWR-NET-11 */
    bool      sta_configured;
    esp_timer_handle_t retry_timer;
} net_t;

net_t s_net;

/* The configuration strings are longer than the fields WiFi gives them, so a
 * plain copy can truncate.  Truncation here is not cosmetic: an SSID cut at 32
 * characters simply never associates, and the operator is left looking at a
 * kiln that will not join a network it can see.  Refusing and saying so is the
 * only useful behaviour. */
/* dst is void *, not char *: the fields this fills are wifi_config_t's uint8_t
 * SSID and passphrase arrays, and taking them as void * means the callers need
 * no cast at all -- memcpy below wants void * regardless. */
bool copy_checked(void *dst, size_t cap, const char *src, const char *what)
{
    const size_t n = strlen(src);
    if (n >= cap) {
        ESP_LOGE(TAG, "%s is %u characters; the radio allows %u", what,
                 (unsigned)n, (unsigned)(cap - 1u));
        return false;
    }
    memcpy(dst, src, n + 1u);
    return true;
}

/* --- scanning and joining (SWR-NET-11, SWR-NET-12) ------------------------ */

/* There is no access point here any more.  It used to come up when the station
 * could not join, serving a provisioning page, and with it came a second
 * passphrase to get wrong and a second radio anybody in the building could
 * reach.  The operator sets WiFi up at the display instead, which is a place
 * only somebody at the kiln can use. */

kiln_err_t net_scan_begin(void *ctx)
{
    (void)ctx;
    if (s_net.scanning) {
        return KILN_OK;
    }
    /* Active, all channels, and NOT blocking: a blocking scan would stall this
     * task for two seconds, and the display has to keep saying "scanning". */
    wifi_scan_config_t sc = {};
    sc.show_hidden = false;
    if (esp_wifi_scan_start(&sc, false) != ESP_OK) {
        return KILN_ERR_IO;
    }
    s_net.scanning = true;
    return KILN_OK;
}

bool net_scan_busy(void *ctx)
{
    (void)ctx;
    return s_net.scanning;
}

uint8_t net_scan_results(void *ctx, kiln_net_ap_t *out, uint8_t max)
{
    (void)ctx;
    if ((out == nullptr) || (max == 0u)) {
        return 0;
    }
    uint16_t found = 0;
    if (esp_wifi_scan_get_ap_num(&found) != ESP_OK || found == 0u) {
        return 0;
    }
    if (found > max) { found = max; }

    /* One record at a time rather than an array of wifi_ap_record_t on the
     * stack: the IDF record is ~80 bytes and twelve of them is a kilobyte this
     * task does not have (architecture 13.4). */
    static wifi_ap_record_t recs[KILN_NET_SCAN_MAX];
    uint16_t n = (found > KILN_NET_SCAN_MAX) ? KILN_NET_SCAN_MAX : found;
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
        return 0;
    }

    uint8_t kept = 0;
    for (uint16_t i = 0; i < n && kept < max; i++) {
        if (recs[i].ssid[0] == '\0') {
            continue;               /* hidden: nothing to show and nothing to pick */
        }
        (void)snprintf(out[kept].ssid, sizeof(out[kept].ssid), "%s",
                       reinterpret_cast<const char *>(recs[i].ssid));
        out[kept].rssi    = recs[i].rssi;
        out[kept].secured = (recs[i].authmode != WIFI_AUTH_OPEN);
        kept++;
    }
    return kept;
}

kiln_err_t net_connect(void *ctx, const char *ssid, const char *pass)
{
    (void)ctx;
    if ((ssid == nullptr) || (ssid[0] == '\0')) {
        return KILN_ERR_INVALID_ARG;
    }
    /* NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) */
    wifi_config_t sta = {};
    if (!copy_checked(sta.sta.ssid, sizeof(sta.sta.ssid), ssid, "ssid")) {
        return KILN_ERR_RANGE;
    }
    if ((pass != nullptr) && (pass[0] != '\0') &&
        !copy_checked(sta.sta.password, sizeof(sta.sta.password), pass, "passphrase")) {
        return KILN_ERR_RANGE;
    }

    (void)esp_wifi_disconnect();
    (void)esp_wifi_set_mode(WIFI_MODE_STA);
    if (esp_wifi_set_config(WIFI_IF_STA, &sta) != ESP_OK) {
        return KILN_ERR_IO;
    }
    s_net.sta_configured = true;
    s_net.retry_ms       = RETRY_MIN_MS;
    s_net.state          = KILN_NET_CONNECTING;
    (void)copy_checked(s_net.ssid, sizeof(s_net.ssid), ssid, "ssid");
    ESP_LOGI(TAG, "joining '%s' as asked at the display", ssid);
    return (esp_wifi_connect() == ESP_OK) ? KILN_OK : KILN_ERR_IO;
}

/* --- events ------------------------------------------------------------- */

void retry_now(void *arg)
{
    (void)arg;
    if (s_net.state != KILN_NET_STA_CONNECTED) {
        (void)esp_wifi_connect();
    }
}

void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    /* The configuration was read here to decide when to raise the access
     * point.  There is no access point now, and the handler needs nothing from
     * it; the argument stays because the event loop was registered with it and
     * a future handler may want it. */
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_net.state            = KILN_NET_CONNECTING;
        s_net.first_attempt_us = esp_timer_get_time();
        (void)esp_wifi_connect();
        return;
    }

    /* SWR-NET-11: the scan is asynchronous, and this is the only thing that
     * knows it has finished.  Without it the display says "scanning" for ever
     * and the results are never read. */
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        s_net.scanning = false;
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d =
            static_cast<const wifi_event_sta_disconnected_t *>(data);
        s_net.last_reason = (d != nullptr) ? (uint8_t)d->reason : 0u;
        if (s_net.state == KILN_NET_STA_CONNECTED) {
            s_net.disconnects++;        /* SWR-NET-09 */
        }
        s_net.state  = KILN_NET_CONNECTING;
        s_net.ip[0]  = '\0';

        /* No fallback to bring up: a kiln whose network changed is told the
         * new one at the display (SWR-NET-11), and meanwhile it fires exactly
         * as well as it did before, which SWR-NET-07 requires anyway.
         *
         * The retry below therefore runs for ever rather than until something
         * else takes over, which is SWR-NET-03 read literally. */
        /* SWR-NET-03: exponential backoff, event-driven, never a spin. */
        (void)esp_timer_stop(s_net.retry_timer);
        (void)esp_timer_start_once(s_net.retry_timer,
                                   (uint64_t)s_net.retry_ms * 1000ull);
        s_net.retry_ms = (s_net.retry_ms * 2u > RETRY_MAX_MS)
                           ? RETRY_MAX_MS : (s_net.retry_ms * 2u);
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = static_cast<const ip_event_got_ip_t *>(data);
        s_net.state       = KILN_NET_STA_CONNECTED;
        s_net.retry_ms    = RETRY_MIN_MS;
        s_net.up_since_us = esp_timer_get_time();
        if (e != nullptr) {
            /* The cast is inside IDF's own IP2STR(); there is no cast here.
             * NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast) */
            (void)snprintf(s_net.ip, sizeof(s_net.ip), IPSTR, IP2STR(&e->ip_info.ip));
        }
        ESP_LOGI(TAG, "connected, %s", s_net.ip);
    }
}

void on_time_sync(struct timeval *tv)
{
    (void)tv;
    /* SWR-LOG-12 and warning 105.  Wall time is for timestamps only: every
     * control and safety deadline uses the monotonic base (SWR-NET-08), so a
     * step here cannot move a runaway timer. */
    s_net.time_synced = true;
    ESP_LOGI(TAG, "wall clock synchronised");
}

/* --- port_net ----------------------------------------------------------- */

kiln_err_t net_status(void *ctx, kiln_net_status_t *out)
{
    const net_t *n = static_cast<const net_t *>(ctx);
    if ((n == nullptr) || (out == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->state = n->state;
    (void)snprintf(out->ssid, sizeof(out->ssid), "%s", n->ssid);
    (void)snprintf(out->ip, sizeof(out->ip), "%s", n->ip);
    (void)snprintf(out->hostname, sizeof(out->hostname), "%s", n->hostname);
    out->disconnect_count = n->disconnects;
    out->time_synced      = n->time_synced;
    out->uptime_s = (n->state == KILN_NET_STA_CONNECTED && n->up_since_us > 0)
                      ? (uint32_t)((esp_timer_get_time() - n->up_since_us) / 1000000)
                      : 0u;

    /* RSSI is only meaningful while associated, and the call costs a register
     * read, so it is taken here rather than cached stale.
     *
     * The struct is filled by esp_wifi_sta_get_ap_info; the zero-init only
     * matters so that a failed call leaves a defined value, and the enum it
     * contains has no zero enumerator.
     * NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) */
    wifi_ap_record_t ap = {};
    if (n->state == KILN_NET_STA_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        out->rssi = ap.rssi;
    }
    return KILN_OK;
}

} // namespace

kiln_err_t kiln_hal_net_init(const kiln_config_t *cfg, kiln_port_net_t *out)
{
    if ((cfg == nullptr) || (out == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    memset(&s_net, 0, sizeof(s_net));
    s_net.state            = KILN_NET_DOWN;
    s_net.retry_ms         = RETRY_MIN_MS;
    (void)snprintf(s_net.hostname, sizeof(s_net.hostname), "%s",
                   (cfg->hostname[0] != '\0') ? cfg->hostname : "safekiln");

    out->ctx          = &s_net;
    out->status       = net_status;
    out->scan_begin   = net_scan_begin;
    out->scan_busy    = net_scan_busy;
    out->scan_results = net_scan_results;
    out->connect      = net_connect;

    if (esp_netif_init() != ESP_OK) {
        return KILN_ERR_IO;
    }
    if (esp_event_loop_create_default() != ESP_OK) {
        return KILN_ERR_IO;             /* already created is fine upstream */
    }
    (void)esp_netif_create_default_wifi_sta();

    const wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&ic) != ESP_OK) {
        return KILN_ERR_IO;
    }

    esp_timer_create_args_t ta = {};
    ta.callback        = retry_now;
    ta.dispatch_method = ESP_TIMER_TASK;
    ta.name            = "wifi_retry";
    (void)esp_timer_create(&ta, &s_net.retry_timer);

    (void)esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              on_wifi, (void *)cfg, nullptr);
    (void)esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              on_wifi, (void *)cfg, nullptr);

    /* SWR-NET-01 / SWR-NET-02: with no stored credentials there is nothing to
     * connect to, so the AP comes up immediately rather than after a timeout
     * that could only ever expire. */
    if (cfg->wifi_ssid[0] != '\0') {
        /* NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) */
        wifi_config_t sta = {};
        const bool ok =
            copy_checked(sta.sta.ssid, sizeof(sta.sta.ssid),
                         cfg->wifi_ssid, "net.wifi_ssid") &&
            copy_checked(sta.sta.password, sizeof(sta.sta.password),
                         cfg->wifi_pass, "net.wifi_pass");
        if (ok) {
            (void)esp_wifi_set_mode(WIFI_MODE_STA);
            (void)esp_wifi_set_config(WIFI_IF_STA, &sta);
            s_net.sta_configured = true;
            (void)copy_checked(s_net.ssid, sizeof(s_net.ssid), cfg->wifi_ssid, "ssid");
        } else {
            /* Credentials that cannot be used are the same as none: the radio
             * stays a station with nothing to join, and the operator corrects
             * them at the display (SWR-NET-12). */
            ESP_LOGW(TAG, "stored credentials are unusable; waiting for the display");
            (void)esp_wifi_set_mode(WIFI_MODE_STA);
        }
    } else {
        ESP_LOGW(TAG, "no stored credentials; set the network up at the display");
        (void)esp_wifi_set_mode(WIFI_MODE_STA);
    }

    if (esp_wifi_start() != ESP_OK) {
        return KILN_ERR_IO;
    }

    /* SWR-NET-04 wants kiln.local, and it is **not implemented here**.
     *
     * mDNS left the ESP-IDF tree for the component manager, and UR-CON-04 forbids
     * a build-time fetch from an unpinned source -- the same constraint that
     * has LittleFS waiting to be vendored (tasklist E7).  Adding a managed
     * dependency to get a convenience feature would be the wrong trade against
     * a constraint the project applies to everything else, so the device is
     * reachable by IP until mDNS is vendored deliberately.
     *
     * The address is on the network screen (SWR-HMI-07), which is where an
     * operator standing at the kiln would look for it anyway. */

    /* SWR-NET-06.  Best effort by design: no route to the internet is normal in
     * a workshop, and warning 105 says so rather than anything failing. */
    if (cfg->ntp_server[0] != '\0') {
        esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG(cfg->ntp_server);
        sc.sync_cb = on_time_sync;
        (void)esp_netif_sntp_init(&sc);
    }
    if (cfg->timezone[0] != '\0') {
        setenv("TZ", cfg->timezone, 1);
        tzset();
    }
    return KILN_OK;
}

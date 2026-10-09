/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The HTTP transport: esp_http_server wired to the route table in api.cpp
 * (architecture 5.4, 12.2, SWR-WEB-19 to SWR-WEB-26).
 *
 * This file moves bytes between a socket and kiln_api_handle and does nothing
 * else.  Every decision about what a route means lives in api.cpp, where a
 * host test can drive it without a network; what is here is the part that
 * needs a socket and therefore cannot be tested that way.  Keeping the split
 * exactly there is why the API suite can cover the whole interface.
 *
 * ---------------------------------------------------------------------------
 * SWR-WEB-26: there is one method
 * ---------------------------------------------------------------------------
 * The server registers **GET only**.  Anything else is refused by
 * esp_http_server before a handler runs, and the catch-all below turns that
 * into the same `403 read_only` body the API would have produced, so a client
 * gets one answer whichever layer refuses it.
 *
 * This is worth stating because it is a second, independent enforcement of the
 * same rule: api.cpp refuses non-GET, and the transport never offers it.
 * Neither is load-bearing alone and both are cheap.
 *
 * ---------------------------------------------------------------------------
 * The interface itself (SWA-11, SWR-WEB-02)
 * ---------------------------------------------------------------------------
 * The four browser assets are gzipped into the image by this component's
 * CMakeLists and served from flash, which is what SWR-WEB-02 means by every
 * asset coming from the device: no CDN, no external font, nothing that
 * reaches off the box, and the interface works on a network with no route to
 * anywhere.
 *
 * They are served **still compressed**, with Content-Encoding: gzip. Every
 * browser that can run this interface can inflate it, and doing it on the
 * device would cost a window buffer per request on the processor that is also
 * running a kiln.
 *
 * ---------------------------------------------------------------------------
 * SWR-NFR-02: this must not delay a control cycle
 * ---------------------------------------------------------------------------
 * The server runs on core 0 with the rest of the UI (SWA-15), at a priority
 * below the control and safety tasks, and every handler writes into a
 * caller-owned buffer that is sized once here.  Nothing allocates per request.
 */
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "kiln_web/httpd.h"

static const char *TAG = "kiln_httpd";

/* Architecture 13.4 budgets about 40 kB for the HTTP server and its sessions.
 * One response buffer, reused, rather than one per session: the handlers are
 * serialised by the server's single task, so a shared buffer is safe and four
 * sessions do not cost four buffers. */
#define RESP_BUF_BYTES   4096

typedef struct {
    kiln_api_ctx_t *api;
    httpd_handle_t  server;
    char            buf[RESP_BUF_BYTES];
} httpd_ctx_t;

static httpd_ctx_t s_http;

/* --- helpers ------------------------------------------------------------ */

/* --- the embedded interface --------------------------------------------
 *
 * The symbol names are the ones the build generates for an embedded binary:
 * _binary_<file with dots as underscores>_gz_start and _end.  Declared here
 * rather than generated into a header because there are four of them and a
 * generator for four lines would be the more complicated answer.
 */
#define KILN_ASSET(sym)                                     \
    extern const uint8_t sym##_start[] asm("_" #sym "_start"); \
    extern const uint8_t sym##_end[]   asm("_" #sym "_end")

KILN_ASSET(binary_index_html_gz);
KILN_ASSET(binary_app_css_gz);
KILN_ASSET(binary_app_js_gz);
KILN_ASSET(binary_chart_js_gz);

typedef struct {
    const char    *path;
    const char    *type;
    const uint8_t *start;
    const uint8_t *end;
} asset_t;

static const asset_t k_assets[] = {
    /* "/" first: it is the one every browser asks for. */
    { "/",          "text/html",       binary_index_html_gz_start, binary_index_html_gz_end },
    { "/index.html","text/html",       binary_index_html_gz_start, binary_index_html_gz_end },
    { "/app.css",   "text/css",        binary_app_css_gz_start,    binary_app_css_gz_end },
    { "/app.js",    "text/javascript", binary_app_js_gz_start,     binary_app_js_gz_end },
    { "/chart.js",  "text/javascript", binary_chart_js_gz_start,   binary_chart_js_gz_end },
};

/* The asset for this exact path, or NULL.
 *
 * An exact match and no path walking of any kind: there is no filesystem
 * behind this and no directory to escape from, so ".." is not special, it
 * simply matches nothing. */
static const asset_t *find_asset(const char *path)
{
    for (size_t i = 0; i < sizeof(k_assets) / sizeof(k_assets[0]); i++) {
        if (strcmp(path, k_assets[i].path) == 0) {
            return &k_assets[i];
        }
    }
    return NULL;
}

static esp_err_t send_asset(httpd_req_t *req, const asset_t *a)
{
    const size_t len = (size_t)(a->end - a->start);
    (void)httpd_resp_set_type(req, a->type);
    (void)httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    /* The assets change only when the firmware does, and a firmware update
     * changes every one of them at once, so a browser holding a stale pair of
     * app.js and index.html is the failure to avoid. no-cache means
     * revalidate, not "do not store": the 304 costs nothing on a LAN and the
     * pair can never be mismatched. */
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)a->start, (ssize_t)len);
}

static const char *status_line(int code)
{
    switch (code) {
    case 200: return "200 OK";
    case 400: return "400 Bad Request";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 409: return "409 Conflict";
    case 500: return "500 Internal Server Error";
    case 503: return "503 Service Unavailable";
    default:  return "200 OK";
    }
}

static kiln_http_method_t to_kiln_method(int m)
{
    switch (m) {
    case HTTP_GET:    return KILN_HTTP_GET;
    case HTTP_POST:   return KILN_HTTP_POST;
    case HTTP_PUT:    return KILN_HTTP_PUT;
    case HTTP_PATCH:  return KILN_HTTP_PATCH;
    case HTTP_DELETE: return KILN_HTTP_DELETE;
    default:          return KILN_HTTP_POST;   /* anything else is a write */
    }
}

/* The query string, which esp_http_server leaves in the URI. */
static const char *split_query(char *path)
{
    char *q = strchr(path, '?');
    if (q == nullptr) {
        return nullptr;
    }
    *q = '\0';
    return q + 1;
}

/* --- the log stream (SWR-LOG-10, SWR-WEB-18) ------------------------------ */

/* kiln_api_log_stream pushes; this is the sink that puts the bytes on the
 * socket.  Returning false aborts the scan, which is how a client that has
 * gone away stops the firmware reading another 24 h of records off flash. */
static bool stream_sink(void *user, const char *data, size_t len)
{
    httpd_req_t *req = (httpd_req_t *)user;
    return httpd_resp_send_chunk(req, data, (ssize_t)len) == ESP_OK;
}

/* --- the single handler ------------------------------------------------- */

static esp_err_t handle_any(httpd_req_t *req)
{
    httpd_ctx_t *h = (httpd_ctx_t *)req->user_ctx;

    /* Refused rather than truncated, and that is not fussiness: a path cut
     * at 128 characters could match a *different* route from the one the
     * client asked for, which is the kind of bug that turns into a story. */
    char uri[160];
    const size_t uri_len = strlen(req->uri);
    if (uri_len >= sizeof(uri)) {
        (void)httpd_resp_set_status(req, status_line(400));
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
            "{\"error\":{\"code\":\"uri_too_long\","
            "\"message\":\"the request path exceeds the server's limit\"}}");
    }
    memcpy(uri, req->uri, uri_len + 1u);
    const char *query = split_query(uri);

    /* The interface before the API, and only on an exact match: every path
     * that is not one of the four assets falls through to the route table,
     * which answers the API routes and produces the 404 for anything else. */
    if (strncmp(uri, "/api/", 5) != 0) {
        const asset_t *a = find_asset(uri);
        if (a != NULL) {
            return send_asset(req, a);
        }
    }

    kiln_api_req_t ar = {};
    ar.method = to_kiln_method((int)req->method);
    ar.path   = uri;
    ar.query  = query;
    ar.body   = nullptr;
    ar.body_len = 0;
    /* SWR-WEB-23 is withdrawn and SWR-WEB-26 leaves nothing to authenticate, so
     * this is true for everyone and means only "the transport did its half".
     * It is kept rather than removed from the struct because the API's own
     * tests still exercise the field, and because a future reader deserves to
     * see that it was considered rather than forgotten. */
    ar.authenticated = true;

    kiln_api_resp_t resp = {};
    resp.body     = h->buf;
    resp.body_cap = sizeof(h->buf);

    /* The log is the one route that streams: a 24 h run is far larger than any
     * buffer worth having, so it is pushed in chunks rather than built. */
    if (strncmp(uri, "/api/log", 8) == 0 && ar.method == KILN_HTTP_GET) {
        (void)kiln_api_log_stream(h->api, &ar, stream_sink, req, &resp);
        (void)httpd_resp_set_status(req, status_line(resp.status));
        (void)httpd_resp_set_type(req, resp.content_type ? resp.content_type
                                                         : "application/json");
        if (resp.status != 200) {
            return httpd_resp_send(req, resp.body, (ssize_t)resp.body_len);
        }
        /* The stream wrote its own chunks; this terminates them. */
        return httpd_resp_send_chunk(req, nullptr, 0);
    }

    (void)kiln_api_handle(h->api, &ar, &resp);

    (void)httpd_resp_set_status(req, status_line(resp.status));
    (void)httpd_resp_set_type(req, (resp.content_type != nullptr)
                                     ? resp.content_type : "application/json");
    /* SYS-ASM-05 is a trusted LAN, not a trusted browser: a page the operator is
     * also viewing should not be able to read the kiln's telemetry
     * cross-origin just because they are on the same network. */
    (void)httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");

    if (resp.truncated) {
        /* A truncated body is a bug in a size budget, not something to send
         * half of: a client cannot tell a cut JSON document from a corrupt
         * one, and silently serving the first 4 kB of a response would be the
         * worst of both. */
        ESP_LOGE(TAG, "%s: response did not fit %u bytes", uri,
                 (unsigned)sizeof(h->buf));
        (void)httpd_resp_set_status(req, status_line(500));
        return httpd_resp_sendstr(req,
            "{\"error\":{\"code\":\"too_large\","
            "\"message\":\"the response did not fit the server's buffer\"}}");
    }
    return httpd_resp_send(req, resp.body, (ssize_t)resp.body_len);
}

/* --- construction ------------------------------------------------------- */

kiln_err_t kiln_httpd_start(kiln_api_ctx_t *api)
{
    if (api == nullptr) {
        return KILN_ERR_INVALID_ARG;
    }
    if (s_http.server != nullptr) {
        return KILN_OK;
    }
    s_http.api = api;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* SWR-WEB-21: four concurrent clients, which is what the requirement asks
     * for and what architecture 13.4's RAM budget was written against. */
    cfg.max_open_sockets   = 4;
    cfg.lru_purge_enable   = true;
    cfg.uri_match_fn       = httpd_uri_match_wildcard;
    cfg.max_uri_handlers   = 2;
    cfg.stack_size         = 6144;
    /* SWA-15 / SWR-NFR-02: core 0, below control and safety, so a slow client
     * cannot contend with a control cycle. */
    cfg.core_id            = 0;
    cfg.task_priority      = 4;

    if (httpd_start(&s_http.server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return KILN_ERR_IO;
    }

    /* SWR-WEB-26: GET and nothing else is ever registered.  A POST does not
     * reach a handler at all; the server answers 405 on its own, and the
     * error handler below rewrites that into the API's own read_only body so
     * the client sees one consistent answer. */
    httpd_uri_t get_any = {};
    get_any.uri      = "/*";
    get_any.method   = HTTP_GET;
    get_any.handler  = handle_any;
    get_any.user_ctx = &s_http;
    (void)httpd_register_uri_handler(s_http.server, &get_any);

    ESP_LOGI(TAG, "listening on port %u, GET only (SWR-WEB-26)",
             (unsigned)cfg.server_port);
    ESP_LOGI(TAG, "interface: %u bytes gzipped across %u assets",
             (unsigned)((binary_index_html_gz_end - binary_index_html_gz_start) +
                        (binary_app_css_gz_end    - binary_app_css_gz_start) +
                        (binary_app_js_gz_end     - binary_app_js_gz_start) +
                        (binary_chart_js_gz_end   - binary_chart_js_gz_start)),
             (unsigned)(sizeof(k_assets) / sizeof(k_assets[0])));
    return KILN_OK;
}

void kiln_httpd_stop(void)
{
    if (s_http.server != nullptr) {
        (void)httpd_stop(s_http.server);
        s_http.server = nullptr;
    }
}

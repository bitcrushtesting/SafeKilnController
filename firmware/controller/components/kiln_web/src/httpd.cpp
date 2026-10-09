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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

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

/* --- the telemetry stream (SWR-WEB-05) ----------------------------------
 *
 * SWR-WEB-05 asks for live values at least once a second by push rather than
 * by polling, and the browser already asks for it: app.js opens an
 * EventSource on /api/events and listens for a `telemetry` event.  Until now
 * nothing answered, so the interface fell back on nothing and the dashboard
 * went stale after four seconds exactly as SWR-WEB-25 says it should.
 *
 * ---------------------------------------------------------------------------
 * WHY A TASK, AND NOT A LOOP IN THE HANDLER
 * ---------------------------------------------------------------------------
 * esp_http_server runs one task and serialises handlers on it.  A handler that
 * sat in a loop pushing a frame a second would therefore hold that task for as
 * long as the browser tab stayed open, and the other three sockets of
 * SWR-WEB-21 would never be served again: the kiln would appear to hang the
 * moment somebody left a dashboard open and then tried to load the log.
 *
 * So the handler does not stream.  It hands the socket to the server's async
 * machinery (httpd_req_async_handler_begin), parks the request in the table
 * below, and returns, which frees the server task immediately.  One low
 * priority task then pushes to every parked subscriber once a second.
 *
 * Bounded at two subscribers of the four sockets SWR-WEB-21 allows.  A stream
 * holds its socket indefinitely, so letting every socket become a stream would
 * leave nothing for the page itself, the log or an update check; two is a
 * dashboard on a phone and one on a laptop, which is the case this is for.
 * A third is refused with 503 and a Retry-After rather than queued, because a
 * client that is told to come back does, and one left hanging does not.
 */
#define SSE_MAX_SUBS      2
#define SSE_PERIOD_MS     1000
#define SSE_BUF_BYTES     2048
/* A comment frame every fifteen pushes, which is what keeps an idle proxy or a
 * phone's radio from dropping a stream that is working. */
#define SSE_PING_EVERY    15

typedef struct {
    httpd_req_t *req;        /* the async copy; NULL when the slot is free */
    uint32_t     pushes;
} sse_sub_t;

static sse_sub_t      s_subs[SSE_MAX_SUBS];
static SemaphoreHandle_t s_subs_lock;
static TaskHandle_t   s_sse_task;
static char           s_sse_buf[SSE_BUF_BYTES];

/* Drop a subscriber and give its socket back to the server.  Called with the
 * lock held, from the pushing task only: the handler never closes a request it
 * has already parked, because the task may be writing to it. */
static void sse_release(sse_sub_t *s)
{
    if (s->req != NULL) {
        (void)httpd_req_async_handler_complete(s->req);
        s->req = NULL;
    }
    s->pushes = 0;
}

/* One frame to one subscriber.  Returns false when the socket is gone, which
 * is the only way a stream ends: the client closes the tab, the radio drops,
 * or the page is reloaded, and none of those is an error worth logging at
 * anything above debug. */
static bool sse_push(sse_sub_t *s, const char *frame, size_t len)
{
    const esp_err_t e = httpd_resp_send_chunk(s->req, frame, (ssize_t)len);
    return e == ESP_OK;
}

static void sse_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SSE_PERIOD_MS));

        if (xSemaphoreTake(s_subs_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        bool any = false;
        for (int i = 0; i < SSE_MAX_SUBS; i++) {
            if (s_subs[i].req != NULL) { any = true; break; }
        }
        if (!any) {
            (void)xSemaphoreGive(s_subs_lock);
            continue;
        }

        /* Serialised once for every subscriber: the payload is identical, and
         * building it per socket would be the same work twice on the processor
         * that is also running a kiln. */
        char payload[1024];
        const size_t plen = kiln_api_telemetry_event(s_http.api, payload,
                                                     sizeof(payload));
        size_t flen = 0;
        if (plen > 0u) {
            flen = kiln_api_sse_frame(s_sse_buf, sizeof(s_sse_buf), "telemetry",
                                      payload, plen);
        }
        if (flen == 0u) {
            /* Nothing to say, or it did not fit.  A half frame would
             * desynchronise the stream for good, so the second is skipped as
             * silently as the first and SWR-WEB-25 shows the dashboard going
             * stale, which is the truth. */
            ESP_LOGW(TAG, "sse: no frame this second (payload %u bytes)",
                     (unsigned)plen);
            (void)xSemaphoreGive(s_subs_lock);
            continue;
        }

        for (int i = 0; i < SSE_MAX_SUBS; i++) {
            sse_sub_t *s = &s_subs[i];
            if (s->req == NULL) {
                continue;
            }
            if (!sse_push(s, s_sse_buf, flen)) {
                ESP_LOGD(TAG, "sse: subscriber %d gone after %u pushes",
                         i, (unsigned)s->pushes);
                sse_release(s);
                continue;
            }
            s->pushes++;
            if ((s->pushes % SSE_PING_EVERY) == 0u) {
                char ping[32];
                const size_t n = kiln_api_sse_comment(ping, sizeof(ping), "ping");
                if ((n > 0u) && !sse_push(s, ping, n)) {
                    sse_release(s);
                }
            }
        }
        (void)xSemaphoreGive(s_subs_lock);
    }
}

/* Park this request as a subscriber.  Returns false when there is no room, in
 * which case the caller answers rather than this. */
static bool sse_subscribe(httpd_req_t *req)
{
    if ((s_subs_lock == NULL) || (s_sse_task == NULL)) {
        return false;
    }
    httpd_req_t *copy = NULL;
    if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
        return false;
    }

    bool parked = false;
    if (xSemaphoreTake(s_subs_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < SSE_MAX_SUBS; i++) {
            if (s_subs[i].req == NULL) {
                s_subs[i].req    = copy;
                s_subs[i].pushes = 0;
                parked = true;
                break;
            }
        }
        (void)xSemaphoreGive(s_subs_lock);
    }
    if (!parked) {
        (void)httpd_req_async_handler_complete(copy);
    }
    return parked;
}

static esp_err_t handle_events(httpd_req_t *req)
{
    (void)httpd_resp_set_type(req, "text/event-stream");
    /* No caching and no buffering anywhere in between: a cached event stream is
     * a dashboard showing a firing that finished yesterday. */
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    (void)httpd_resp_set_hdr(req, "X-Accel-Buffering", "no");
    (void)httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");

    if (!sse_subscribe(req)) {
        /* Full.  Said plainly, with how long to wait: app.js reconnects with
         * backoff on any error, so a 503 costs the client a second and a
         * silent drop costs it the same with no explanation in the log. */
        (void)httpd_resp_set_status(req, status_line(503));
        (void)httpd_resp_set_type(req, "application/json");
        (void)httpd_resp_set_hdr(req, "Retry-After", "5");
        return httpd_resp_sendstr(req,
            "{\"error\":{\"code\":\"too_many_streams\","
            "\"message\":\"the device streams to two clients at a time\"}}");
    }

    /* Parked.  The first frame comes from the pushing task within the second,
     * and returning here releases the server task for everybody else. */
    return ESP_OK;
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

    /* The telemetry stream, which does not answer here at all: it parks the
     * socket and returns, so this task is free for the next request. */
    if ((strcmp(uri, "/api/events") == 0) && (ar.method == KILN_HTTP_GET)) {
        return handle_events(req);
    }

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
    /* SWR-WEB-21 asks for four concurrent clients.  Six sockets, because a
     * client with the dashboard open holds one of them parked on the telemetry
     * stream and still needs another to fetch the log or the config: four with
     * two of them streaming would be the requirement met on paper and not in a
     * workshop.  Architecture 13.4's RAM budget carries the extra two. */
    cfg.max_open_sockets   = 6;
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

    /* SWR-WEB-05's pushing task.  Priority 3, below the server's 4 and well
     * below control and safety: a frame that is late is a dashboard that
     * updates in 1.1 s, and SWR-NFR-02 does not negotiate. */
    if (s_subs_lock == NULL) {
        s_subs_lock = xSemaphoreCreateMutex();
    }
    if ((s_subs_lock != NULL) && (s_sse_task == NULL)) {
        if (xTaskCreatePinnedToCore(sse_task, "kiln_sse", 3072, NULL, 3,
                                    &s_sse_task, 0) != pdPASS) {
            s_sse_task = NULL;
            /* The interface still works: app.js falls back to showing the
             * dashboard going stale, which is SWR-WEB-25 and is honest. */
            ESP_LOGE(TAG, "sse task would not start; /api/events will answer 503");
        }
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
    /* The task goes first, and its subscribers with it: stopping the server
     * under a task that is writing to one of its sockets is a use-after-free
     * with a kiln attached. */
    if (s_sse_task != nullptr) {
        vTaskDelete(s_sse_task);
        s_sse_task = nullptr;
    }
    if (s_subs_lock != nullptr) {
        for (int i = 0; i < SSE_MAX_SUBS; i++) {
            sse_release(&s_subs[i]);
        }
    }
    if (s_http.server != nullptr) {
        (void)httpd_stop(s_http.server);
        s_http.server = nullptr;
    }
}

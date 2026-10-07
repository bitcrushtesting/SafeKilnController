/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A development harness: the real firmware logic, a simulated kiln, and enough
 * HTTP to drive the web interface from a browser with no device involved.
 *
 * This exists because AD-01 makes it almost free.  kiln_core is platform-free,
 * kiln_app already runs against kiln_sim, and kiln_web's API layer has no
 * transport in it -- so the only thing missing was a socket.  What the browser
 * talks to here is the same handler code the device runs; only the bytes arrive
 * differently.
 *
 * Deliberately NOT a production web server.  It is single-threaded, serves one
 * request at a time, has no TLS, and lives in firmware/host/ where nothing can
 * link it into an image.  esp_http_server is the real transport; this is for
 * seeing the UI and for driving the API suite by hand.
 *
 *   firmware/host/webhost/build.sh && ./webhost --port 8080 --accel 60
 */

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "kiln_app/app.h"
#include "kiln_app/program_store.h"
#include "kiln_app/run_index.h"
#include "kiln_core/faults.h"
#include "kiln_core/logring.h"
#include "kiln_core/profile.h"
#include "kiln_hal_host/hal_host.h"
#include "kiln_sim/sim.h"
#include "kiln_web/api.h"

/* --- the device ---------------------------------------------------------- */

constexpr size_t LOG_SECTORS  = 128u;
constexpr size_t SECTOR_BYTES = 4096u;
constexpr size_t BODY_CAP     = 65536u;

namespace {

uint8_t               g_flash_storage[LOG_SECTORS * SECTOR_BYTES];
kiln_host_flash_t     g_flash;
kiln_port_flash_t     g_flash_port;
kiln_host_kv_t        g_kv;
kiln_port_kvstore_t   g_kv_port;
kiln_host_fs_t        g_fs;
kiln_port_filestore_t g_fs_port;
kiln_host_clock_t     g_clk;
kiln_port_clock_t     g_clk_port;
kiln_logring_t        g_ring;
kiln_port_logstore_t  g_log_port;
kiln_sim_t            g_sim;
kiln_sim_ports_t      g_sim_ports;
kiln_app_t            g_app;
kiln_api_ctx_t        g_api;
char                  g_body[BODY_CAP];

double g_accel   = 60.0;
char   g_web_dir[512] = "web";
volatile sig_atomic_t g_stop = 0;

void on_signal(int sig) { (void)sig; g_stop = 1; }

double now_s(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

void device_init(void)
{
    kiln_host_flash_init(&g_flash, g_flash_storage, sizeof(g_flash_storage),
                         SECTOR_BYTES);
    kiln_host_flash_bind(&g_flash, &g_flash_port);
    kiln_host_kv_init(&g_kv);
    kiln_host_kv_bind(&g_kv, &g_kv_port);
    kiln_host_fs_init(&g_fs);
    kiln_host_fs_bind(&g_fs, &g_fs_port);
    kiln_host_clock_init(&g_clk, (uint64_t)time(NULL), true);
    kiln_host_clock_bind(&g_clk, &g_clk_port);

    (void)kiln_logring_mount(&g_ring, &g_flash_port);
    kiln_logring_bind(&g_ring, &g_log_port);

    kiln_sim_cfg_t sc;
    kiln_sim_cfg_defaults(&sc);
    /* The charge pump is hardware and does not accelerate -- see the note in
     * main.c, which found this the hard way. */
    sc.enable_decay_s = 1.0f * (float)g_accel;
    kiln_sim_init(&g_sim, &sc);
    kiln_sim_bind(&g_sim, &g_sim_ports);

    kiln_app_ports_t ports = {};
    ports.tc        = &g_sim_ports.tc;
    ports.case_tc   = &g_sim_ports.case_tc;
    ports.heat      = &g_sim_ports.heat;
    ports.current   = &g_sim_ports.current;
    ports.counters  = &g_sim_ports.counters;
    ports.logstore  = &g_log_port;
    ports.kvstore   = &g_kv_port;
    ports.filestore = &g_fs_port;
    ports.clock     = &g_clk_port;

    kiln_config_t cfg;
    kiln_config_defaults(&cfg);
    /* The same gains main.c uses, derived for this plant. */
    cfg.kp = 1.5f; cfg.ki = 0.006f; cfg.kd = 90.0f;
    cfg.log_interval_s = 5;

    if (kiln_app_init(&g_app, &ports, &cfg) != KILN_OK) {
        (void)fprintf(stderr, "kiln_app_init failed\n");
        exit(1);
    }
    (void)kiln_app_boot(&g_app, KILN_RESET_POWER_ON, -1.0f);

    g_api.app       = &g_app;
    g_api.filestore = &g_fs_port;
    g_api.logstore  = &g_log_port;
    g_api.ring      = &g_ring;
}

/* Advance the device by `dt` seconds of wall time. */
void device_tick(double dt)
{
    static double acc_window = 0, acc_safety = 0, acc_acquire = 0, acc_control = 0;

    acc_window += dt;
    while (acc_window >= 0.010) {
        acc_window -= 0.010;
        kiln_sim_step(&g_sim, (float)(0.010 * g_accel));
        kiln_host_clock_advance(&g_clk, (uint64_t)(0.010 * g_accel * 1e6));
        kiln_app_window_tick(&g_app, 10);
    }
    acc_acquire += dt;
    while (acc_acquire >= 0.250) {
        acc_acquire -= 0.250;
        kiln_app_acquire_cycle(&g_app, (float)(0.250 * g_accel));
    }
    acc_safety += dt;
    while (acc_safety >= 0.100) {
        acc_safety -= 0.100;
        kiln_app_safety_cycle(&g_app, (float)(0.100 * g_accel));
    }
    acc_control += dt;
    while (acc_control >= 1.000) {
        acc_control -= 1.000;
        kiln_app_control_cycle(&g_app, (float)(1.000 * g_accel));
    }
    (void)kiln_app_log_drain(&g_app, 16);
}

} // namespace

/* --- tiny HTTP ---------------------------------------------------------- */

constexpr int MAX_CLIENTS = 16;
constexpr size_t REQ_CAP     = KILN_API_MAX_BODY + 4096;

namespace {

typedef struct {
    int    fd;
    char   in[REQ_CAP];
    size_t in_len;
    bool   sse;          /* an open /api/events stream */
    bool   in_use;
} client_t;

client_t g_clients[MAX_CLIENTS];

bool send_all(int fd, const char *data, size_t len)
{
    while (len > 0) {
        const ssize_t n = send(fd, data, len, 0);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
                continue;
            }
            return false;
        }
        data += n;
        len  -= (size_t)n;
    }
    return true;
}

bool send_str(int fd, const char *s) { return send_all(fd, s, strlen(s)); }

void send_headers(int fd, int status, const char *content_type,
                         long content_length, const char *extra)
{
    const char *reason = status == 200 ? "OK"
                       : status == 201 ? "Created"
                       : status == 400 ? "Bad Request"
                       : status == 401 ? "Unauthorized"
                       : status == 404 ? "Not Found"
                       : status == 405 ? "Method Not Allowed"
                       : status == 409 ? "Conflict"
                       : status == 413 ? "Payload Too Large"
                       : status == 500 ? "Internal Server Error"
                       : status == 503 ? "Service Unavailable"
                       : "Error";
    char h[512];
    int n = snprintf(h, sizeof(h),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Cache-Control: no-store\r\n"
                     "Connection: close\r\n",
                     status, reason, content_type);
    if (content_length >= 0) {
        n += snprintf(h + n, sizeof(h) - (size_t)n, "Content-Length: %ld\r\n",
                      content_length);
    } else {
        n += snprintf(h + n, sizeof(h) - (size_t)n, "Transfer-Encoding: chunked\r\n");
    }
    if (extra != nullptr) {
        n += snprintf(h + n, sizeof(h) - (size_t)n, "%s", extra);
    }
    (void)snprintf(h + n, sizeof(h) - (size_t)n, "\r\n");
    (void)send_str(fd, h);
}

/* Chunked writer for the log stream, so this harness exercises the same push
 * interface the device's transport does. */
typedef struct { int fd; bool ok; } chunk_sink_t;

bool chunk_write(void *user, const char *data, size_t len)
{
    chunk_sink_t *c = static_cast<chunk_sink_t *>(user);
    if (!c->ok || len == 0) {
        return c->ok;
    }

    char hdr[32];
    const int n = snprintf(hdr, sizeof(hdr), "%zx\r\n", len);
    if (!send_all(c->fd, hdr, (size_t)n) ||
        !send_all(c->fd, data, len) ||
        !send_all(c->fd, "\r\n", 2)) {
        c->ok = false;
    }
    return c->ok;
}

const char *mime_for(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot == nullptr) {
        return "application/octet-stream";
    }
    if (strcmp(dot, ".html") == 0) {
        return "text/html; charset=utf-8";
    }
    if (strcmp(dot, ".js") == 0) {
        return "text/javascript; charset=utf-8";
    }
    if (strcmp(dot, ".css") == 0) {
        return "text/css; charset=utf-8";
    }
    if (strcmp(dot, ".svg") == 0) {
        return "image/svg+xml";
    }
    if (strcmp(dot, ".json") == 0) {
        return "application/json";
    }
    if (strcmp(dot, ".ico") == 0) {
        return "image/x-icon";
    }
    return "application/octet-stream";
}

/* Serve a file from the web directory.  Rejects anything with a `..` in it: this
 * is a development tool, but a path traversal is a path traversal. */
bool serve_static(int fd, const char *path)
{
    if (strstr(path, "..") != nullptr) {
        return false;
    }

    char full[1024];
    /* Truncation would only fail to find a file that was never asked for, and
     * the ".." check above is what keeps the path inside g_web_dir. */
    if (strcmp(path, "/") == 0) {
        (void)snprintf(full, sizeof(full), "%s/index.html", g_web_dir);
    } else {
        (void)snprintf(full, sizeof(full), "%s%s", g_web_dir, path);
    }

    FILE *f = fopen(full, "rb");
    if (f == nullptr) {
        return false;
    }

    struct stat st;
    if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
        (void)fclose(f);
        return false;
    }

    send_headers(fd, 200, mime_for(full), (long)st.st_size, NULL);
    char buf[8192];
    for (;;) {
        const size_t n = fread(buf, 1, sizeof(buf), f);
        if (n > 0 && !send_all(fd, buf, n)) {
            break;
        }
        if (n < sizeof(buf)) {
            /* A short read is either the end of the file or an error, and after
             * an error the stream position is indeterminate -- so neither is a
             * reason to go round again.  The loop this replaces kept calling
             * fread() on a stream it had already read to EOF
             * (clang-analyzer-unix.Stream). */
            break;
        }
    }
    (void)fclose(f);
    return true;
}

kiln_http_method_t method_of(const char *m)
{
    if (strcmp(m, "GET") == 0) { return KILN_HTTP_GET; }
    if (strcmp(m, "POST") == 0) { return KILN_HTTP_POST; }
    if (strcmp(m, "PUT") == 0) { return KILN_HTTP_PUT; }
    if (strcmp(m, "PATCH") == 0) { return KILN_HTTP_PATCH; }
    if (strcmp(m, "DELETE") == 0) { return KILN_HTTP_DELETE; }
    return KILN_HTTP_METHOD_COUNT;
}

void handle_request(client_t *c)
{
    /* Request line. */
    char method[8] = "", target[512] = "";
    if (sscanf(c->in, "%7s %511s", method, target) != 2) {
        send_headers(c->fd, 400, "text/plain", 0, NULL);
        return;
    }
    const kiln_http_method_t m = method_of(method);
    if (m == KILN_HTTP_METHOD_COUNT) {
        send_headers(c->fd, 405, "text/plain", 0, NULL);
        return;
    }

    char *query = strchr(target, '?');
    if (query != nullptr) {
        *query = '\0';   /* terminate the path here ... */
        query++;         /* ... and the query string starts after it */
    }

    /* Body, if any. */
    const char *body = strstr(c->in, "\r\n\r\n");
    size_t body_len = 0;
    if (body != nullptr) {
        body += 4;
        body_len = c->in_len - (size_t)(body - c->in);
    }

    /* FR-WEB-05: the SSE stream. */
    if (strcmp(target, "/api/events") == 0 && m == KILN_HTTP_GET) {
        send_headers(c->fd, 200, "text/event-stream", -1,
                     "X-Accel-Buffering: no\r\n");
        c->sse = true;
        return;
    }

    if (strncmp(target, "/api/", 5) == 0) {
        kiln_api_req_t req = {};
        req.method   = m;
        req.path     = target;
        req.query    = query;
        req.body     = body;
        req.body_len = body_len;
        /* No password is configured in the harness, so everything is permitted.
         * FR-WEB-23's enforcement is the transport's job and is tested in the
         * API suite rather than here. */
        req.authenticated = true;

        kiln_api_resp_t resp = {};
        resp.body     = g_body;
        resp.body_cap = sizeof(g_body);

        if (strcmp(target, "/api/log") == 0 && m == KILN_HTTP_GET) {
            chunk_sink_t sink = { .fd = c->fd, .ok = true };
            /* Headers must go out before the body, so the status comes from
             * begin-time validation. */
            kiln_api_resp_t meta = {};
            meta.body     = g_body;
            meta.body_cap = sizeof(g_body);

            /* The status is decided inside begin, before anything is written, so
             * a rejected query still arrives as an error -- but the headers have
             * to go first, so a bad query is reported as a 200 carrying the error
             * envelope.  The device's transport can do better because
             * esp_http_server lets it set the status after the handler returns;
             * this is a harness, and the API suite covers the status codes. */
            send_headers(c->fd, 200, "application/json", -1, NULL);
            (void)kiln_api_log_stream(&g_api, &req, chunk_write, &sink, &meta);
            if (sink.ok && meta.streaming) {
                (void)send_str(c->fd, "0\r\n\r\n");
            } else if (sink.ok) {
                /* begin() refused: send the error envelope it produced. */
                chunk_write(&sink, meta.body, meta.body_len);
                (void)send_str(c->fd, "0\r\n\r\n");
            }
            return;
        }

        (void)kiln_api_handle(&g_api, &req, &resp);
        send_headers(c->fd, resp.status, resp.content_type, (long)resp.body_len, NULL);
        (void)send_all(c->fd, resp.body, resp.body_len);
        return;
    }

    if (m == KILN_HTTP_GET && serve_static(c->fd, target)) {
        return;
    }

    send_headers(c->fd, 404, "text/plain", 13, NULL);
    (void)send_str(c->fd, "not found\r\n");
}

void push_sse(void)
{
    static char ev[8192];
    const size_t n = kiln_api_telemetry_event(&g_api, ev, sizeof(ev));
    if (n == 0) {
        return;
    }

    char frame[8600];
    const int fn = snprintf(frame, sizeof(frame), "event: telemetry\ndata: %s\n\n", ev);
    if (fn <= 0) {
        return;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!g_clients[i].in_use || !g_clients[i].sse) {
            continue;
        }
        if (!send_all(g_clients[i].fd, frame, (size_t)fn)) {
            close(g_clients[i].fd);
            g_clients[i].in_use = false;
        }
    }
}

} // namespace

int main(int argc, char **argv)
{
    int port = 8080;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--accel") == 0 && i + 1 < argc) {
            g_accel = atof(argv[++i]);
        }
        else if (strcmp(argv[i], "--web") == 0 && i + 1 < argc) {
            (void)snprintf(g_web_dir, sizeof(g_web_dir), "%s", argv[++i]);
        }
        else {
            (void)fprintf(stderr,
                "Safe Kiln Controller development harness -- real firmware logic, simulated kiln.\n"
                "\n"
                "  --port N    listen port (default 8080)\n"
                "  --accel X   simulated time multiplier (default 60)\n"
                "  --web DIR   directory of web assets (default ./web)\n"
                "\n"
                "NOT a production server: single-threaded, no TLS, no authentication.\n");
            return 1;
        }
    }
    if (g_accel < 1.0) {
        g_accel = 1.0;
    }

    /* The previous handlers are of no interest -- this harness installs these
     * once at startup and never restores them. */
    (void)signal(SIGINT, on_signal);
    (void)signal(SIGPIPE, SIG_IGN);

    device_init();

    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* localhost only */
    addr.sin_port        = htons((uint16_t)port);

    /* reinterpret_cast: the sockets API takes the generic sockaddr and the
     * caller supplies the family-specific one.  There is no other way to call
     * bind(), which is why this is the one such cast in the file.
     * NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) */
    if (bind(listener, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        perror("bind");
        return 1;
    }
    if (listen(listener, 8) != 0) { perror("listen"); return 1; }

    printf("Safe Kiln Controller harness on http://127.0.0.1:%d  (simulated kiln, %gx time)\n",
           port, g_accel);
    printf("Serving assets from %s\n", g_web_dir);
    printf("Ctrl-C to stop.\n");

    double last = now_s(), last_sse = last;

    while (g_stop == 0) {
        struct pollfd pfd[MAX_CLIENTS + 1];
        int nfd = 0;
        pfd[nfd].fd = listener; pfd[nfd].events = POLLIN; nfd++;

        int map[MAX_CLIENTS];
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!g_clients[i].in_use || g_clients[i].sse) {
                continue;
            }
            map[nfd - 1] = i;
            pfd[nfd].fd = g_clients[i].fd;
            pfd[nfd].events = POLLIN;
            nfd++;
        }

        (void)poll(pfd, (unsigned)nfd, 10);

        const double t = now_s();
        device_tick(t - last);
        last = t;

        if (t - last_sse >= 1.0) { push_sse(); last_sse = t; }

        if (((unsigned)pfd[0].revents & (unsigned)POLLIN) != 0u) {
            const int fd = accept(listener, NULL, NULL);
            if (fd >= 0) {
                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (!g_clients[i].in_use) {
                        slot = i;
                        break;
                    }
                }
                if (slot < 0) {
                    close(fd);
                } else {
                    memset(&g_clients[slot], 0, sizeof(g_clients[slot]));
                    g_clients[slot].fd     = fd;
                    g_clients[slot].in_use = true;
                }
            }
        }

        for (int k = 1; k < nfd; k++) {
            /* poll()'s revents is a short and the POLL* macros are signed
             * ints, so the mask is composed in unsigned. */
            if (((unsigned)pfd[k].revents &
                 ((unsigned)POLLIN | (unsigned)POLLHUP | (unsigned)POLLERR)) == 0u) {
                continue;
            }
            client_t *c = &g_clients[map[k - 1]];

            const ssize_t n = recv(c->fd, c->in + c->in_len,
                                   sizeof(c->in) - c->in_len - 1u, 0);
            if (n <= 0) { close(c->fd); c->in_use = false; continue; }
            c->in_len += (size_t)n;
            c->in[c->in_len] = '\0';

            /* Headers complete?  Then check for a declared body and wait for it. */
            const char *hdr_end = strstr(c->in, "\r\n\r\n");
            if (hdr_end == nullptr) {
                if (c->in_len + 1 >= sizeof(c->in)) { close(c->fd); c->in_use = false; }
                continue;
            }
            const char *cl = strcasestr(c->in, "Content-Length:");
            if (cl != nullptr) {
                const long want = strtol(cl + 15, NULL, 10);
                const size_t have = c->in_len - (size_t)(hdr_end + 4 - c->in);
                if (want > 0 && have < (size_t)want) {
                    continue;
                }
            }

            handle_request(c);
            if (!c->sse) { close(c->fd); c->in_use = false; }
        }
    }

    printf("\nstopping\n");
    close(listener);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].in_use) {
            close(g_clients[i].fd);
        }
    }
    return 0;
}

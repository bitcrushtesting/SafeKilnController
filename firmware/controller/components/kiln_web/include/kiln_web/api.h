/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The REST API of architecture 12.1 -- FR-WEB-19, FR-WEB-20, and the
 * request-handling rules of 12.2.
 *
 * Free of esp_http_server on purpose.  A handler takes a method, a path, a query
 * string and a body, and writes a status and a body: no socket, no task, no
 * platform.  That is what lets the whole API be driven by a host test (and by the
 * host harness that serves the UI without a device) while the target adapter
 * reduces to moving bytes between a socket and these functions.
 *
 * AD-16: the UI uses only this API, with no privileged back channel, so anything
 * the UI can do is in this table and is documented by it.
 *
 * FR-WEB-26: this is an *observation* surface.  No route here can put heat into
 * the kiln, write configuration or clear a latched fault -- and the handlers
 * that used to are deleted, not disabled, so the capability is absent from the
 * image rather than switched off in it.  Program authoring is the one write
 * that remains, because a stored curve cannot heat anything until somebody
 * starts it at the kiln.  Refusals are 403 `read_only`.
 *
 * 12.2's rules, which are the security-relevant ones:
 *   - every handler writes into a caller-owned buffer and reports truncation;
 *     nothing accumulates and nothing allocates proportionally to input (NFR-19)
 *   - long responses stream in bounded chunks rather than being built whole
 *   - handlers never reach into kiln_core; they go through kiln_app
 */
#ifndef KILN_WEB_API_H
#define KILN_WEB_API_H

#include <stddef.h>
#include "kiln/err.h"
#include "kiln_app/app.h"
#include "kiln_core/logring.h"
#include "kiln_ports/port_net.h"
#include "kiln_ports/port_system.h"
#include "kiln_ports/port_update.h"

/* 12.2: a declared maximum body size per handler, enforced before parsing. */
constexpr size_t KILN_API_MAX_BODY   = 2048;
constexpr size_t KILN_API_MAX_TOKENS = 192;
constexpr size_t KILN_API_MAX_POINTS = 2000;  /* FR-LOG-10's caller budget, bounded */

typedef enum {
    KILN_HTTP_GET = 0,
    KILN_HTTP_POST,
    KILN_HTTP_PUT,
    KILN_HTTP_PATCH,
    KILN_HTTP_DELETE,
    KILN_HTTP_METHOD_COUNT,
} kiln_http_method_t;

typedef struct {
    kiln_app_t                  *app;
    const kiln_port_filestore_t *filestore;
    const kiln_port_logstore_t  *logstore;
    const kiln_port_system_t    *system;
    const kiln_port_net_t       *net;
    const kiln_port_update_t    *update;

    /* FR-LOG-15 reports store health, which only the ring itself knows. */
    kiln_logring_t *ring;
} kiln_api_ctx_t;

typedef struct {
    kiln_http_method_t method;
    const char        *path;        /* without the query string */
    const char        *query;       /* may be NULL */
    const char        *body;        /* may be NULL */
    size_t             body_len;
    /* FR-WEB-23: the transport decides this, because only it can see the header
     * and apply the constant-time comparison and back-off of NFR-19. */
    bool               authenticated;
} kiln_api_req_t;

typedef struct {
    int         status;
    const char *content_type;
    char       *body;              /* caller-owned */
    size_t      body_cap;
    size_t      body_len;
    bool        truncated;         /* the response did not fit */
    /* Set when the response is a stream: the caller drives kiln_api_log_next
     * instead of sending body. */
    bool        streaming;
} kiln_api_resp_t;

/* Dispatch.  Always fills a valid response, including for an unknown route; the
 * return value is for the caller's own logging and is never the thing the client
 * sees. */
kiln_err_t kiln_api_handle(kiln_api_ctx_t *ctx, const kiln_api_req_t *req,
                           kiln_api_resp_t *resp);

/* FR-WEB-20: `{"error":{"code":"...","message":"..."}}`.  Exposed because the
 * transport needs it for the errors it generates itself -- an oversized body, a
 * failed authentication -- and two spellings of an error envelope is one too
 * many. */
void kiln_api_error(kiln_api_resp_t *resp, int status, const char *code,
                    const char *message);

/* --- the log stream (FR-LOG-10, FR-WEB-18) ------------------------------ */

/* Push, not pull, and that is the whole design.
 *
 * A pull interface would have to hold every decimated bucket until the caller
 * asked for it -- 800 points is about 38 kB, against the 40 kB architecture 13.4
 * budgets for the entire HTTP server and four sessions.  Pushing means one pass
 * over the ring, one bucket in RAM, and a formatting buffer measured in
 * hundreds of bytes.
 *
 * The transport supplies `write`, which sends to its socket.  Returning false
 * from it aborts the stream, which is how a disconnected client stops the scan
 * rather than being written to for another 24 h of records. */
typedef bool (*kiln_api_write_fn)(void *user, const char *data, size_t len);

/* Query: run, from, to, max_points, format=json|csv.
 *
 * `resp` is filled with the status and content type *before* anything is
 * written, because the transport has to send headers first.  On a non-200 the
 * body holds the error and nothing is streamed. */
kiln_err_t kiln_api_log_stream(kiln_api_ctx_t *ctx, const kiln_api_req_t *req,
                               kiln_api_write_fn write, void *user,
                               kiln_api_resp_t *resp);

/* --- helpers the transport needs --------------------------------------- */

/* A query parameter, URL-decoded.  Returns false when absent or when it would
 * not fit, rather than truncating. */
bool kiln_api_query_get(const char *query, const char *key, char *out, size_t cap);
bool kiln_api_query_uint(const char *query, const char *key, uint32_t *out);

/* FR-WEB-23: which routes change state and therefore need authentication.
 * Decided here rather than in the transport, so adding a route cannot
 * accidentally leave it unprotected.
 *
 * After FR-WEB-26 the set of state-changing routes is program authoring alone;
 * everything else non-GET is refused outright.  The method test is kept rather
 * than narrowed to those routes, because it still fails safe: a new write route
 * is authenticated by default. */
bool kiln_api_needs_auth(const kiln_api_req_t *req);

/* The SSE payload for one telemetry event (FR-WEB-05).  Separate from
 * /api/status only in that it is what the stream pushes; the shape is identical,
 * so the UI has one parser. */
size_t kiln_api_telemetry_event(kiln_api_ctx_t *ctx, char *buf, size_t cap);

#endif

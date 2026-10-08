/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GET /api/log -- SWR-LOG-10, SWR-LOG-11, SWR-WEB-11, SWR-WEB-18.
 *
 * Decimation happens as the records stream past, emitting each bucket the moment
 * the next sample crosses out of it, so the memory cost is one bucket rather than
 * all of them.  That needs the time span up front, which is why there is a first
 * pass to find it: two cheap scans of flash against 38 kB of buffer is not a
 * close call on a device with no PSRAM.
 *
 * SWR-LOG-11 is why the bucket carries min *and* max for every series: a brief
 * excursion has to survive downsampling, which is the entire reason a chart of a
 * 24 h firing can be trusted.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "kiln_web/api.h"
#include "kiln_web/json.h"

/* Formatting buffer.  Sized for one bucket's worth of JSON with room to spare;
 * nothing here grows with the query. */
constexpr size_t CHUNK_BYTES = 512;

namespace {

typedef struct {
    kiln_api_write_fn write;
    void             *user;
    bool              failed;
    char              buf[CHUNK_BYTES];
    size_t            len;
} sink_t;

void sink_flush(sink_t *s)
{
    if (s->failed || s->len == 0) {
        return;
    }
    if (!s->write(s->user, s->buf, s->len)) {
        s->failed = true;
    }
    s->len = 0;
}

void sink_put(sink_t *s, const char *data, size_t n)
{
    if (s->failed) {
        return;
    }
    while (n > 0) {
        if (s->len == sizeof(s->buf)) {
            sink_flush(s);
        }
        if (s->failed) {
            return;
        }
        const size_t room = sizeof(s->buf) - s->len;
        const size_t take = n < room ? n : room;
        memcpy(&s->buf[s->len], data, take);
        s->len += take;
        data   += take;
        n      -= take;
    }
}

void sink_str(sink_t *s, const char *t) { sink_put(s, t, strlen(t)); }

void sink_fmt(sink_t *s, const char *fmt, ...)
{
    if (s->failed) {
        return;
    }
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0) {
        sink_put(s, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1u);
    }
}

} // namespace

/* --- pass one: the time span -------------------------------------------- */

namespace {

typedef struct {
    uint32_t run_id;
    uint32_t from_ms, to_ms;
    uint32_t t_min, t_max;
    uint32_t count;
    bool     any;
} span_ctx_t;

bool span_visit(void *user, uint32_t run, const uint8_t rec[KILN_LOG_RECORD_BYTES])
{
    span_ctx_t *s = static_cast<span_ctx_t *>(user);
    (void)run;

    kiln_log_sample_t sm;
    if (kiln_logrec_decode(rec, &sm) != KILN_OK) {
        return true;
    }
    if (sm.t_rel_ms < s->from_ms) {
        return true;
    }
    if (s->to_ms > s->from_ms && sm.t_rel_ms > s->to_ms) {
        return true;
    }

    if (!s->any || sm.t_rel_ms < s->t_min) {
        s->t_min = sm.t_rel_ms;
    }
    if (!s->any || sm.t_rel_ms > s->t_max) {
        s->t_max = sm.t_rel_ms;
    }
    s->any = true;
    s->count++;
    return true;
}

} // namespace

/* --- pass two: decimate and emit --------------------------------------- */

namespace {

typedef struct {
    sink_t           *sink;
    uint32_t          from_ms, to_ms;
    uint32_t          bucket_ms;
    uint32_t          max_points;
    bool              csv;

    kiln_log_bucket_t bucket;
    bool              have_bucket;
    uint32_t          bucket_index;
    uint32_t          emitted;
    uint32_t          accepted;
} emit_ctx_t;

void emit_bucket(emit_ctx_t *e)
{
    if (!e->have_bucket) {
        return;
    }
    const kiln_log_bucket_t *b = &e->bucket;

    if (e->csv) {
        /* SWR-WEB-18.  One row per bucket, with the extrema of each series, so a
         * spreadsheet shows the same excursions the chart does. */
        sink_fmt(e->sink,
                 "%u,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.2f,%.2f,%u,%u,%u,%u\n",
                 (unsigned)b->t_rel_ms,
                 (double)b->kiln_min_c, (double)b->kiln_max_c,
                 (double)b->sp_min_c,   (double)b->sp_max_c,
                 (double)b->case_min_c, (double)b->case_max_c,
                 (double)b->cur_min_a,  (double)b->cur_max_a,
                 (unsigned)b->duty_min, (unsigned)b->duty_max,
                 (unsigned)b->state,    (unsigned)b->count);
    } else {
        /* Arrays rather than objects: the key names would otherwise be about
         * two thirds of the payload, and SWR-WEB-11 gives the whole response 2 s
         * on a phone.  The order is documented by the "columns" field. */
        sink_fmt(e->sink, "%s[%u,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.2f,%.2f,%u,%u,%u]",
                 (e->emitted != 0u) ? "," : "", (unsigned)b->t_rel_ms, (double)b->kiln_min_c,
                 (double)b->kiln_max_c, (double)b->sp_min_c, (double)b->sp_max_c,
                 (double)b->case_min_c, (double)b->case_max_c, (double)b->cur_min_a,
                 (double)b->cur_max_a, (unsigned)b->duty_min, (unsigned)b->duty_max,
                 (unsigned)b->state);
    }
    e->emitted++;
    e->have_bucket = false;
}

void bucket_start(emit_ctx_t *e, const kiln_log_sample_t *s, uint32_t index)
{
    kiln_log_bucket_t *b = &e->bucket;
    memset(b, 0, sizeof(*b));
    b->t_rel_ms   = s->t_rel_ms;
    b->kiln_min_c = b->kiln_max_c = s->kiln_filt_c;
    b->sp_min_c   = b->sp_max_c   = s->setpoint_c;
    b->case_min_c = b->case_max_c = s->case_c;
    b->cur_min_a  = b->cur_max_a  = s->current_a;
    b->duty_min   = b->duty_max   = s->duty_permille;
    b->state      = s->state;
    b->flags      = s->flags;
    b->current_flags = s->current_flags;
    b->count      = 1;

    e->bucket_index = index;
    e->have_bucket  = true;
}

void bucket_add(emit_ctx_t *e, const kiln_log_sample_t *s)
{
    kiln_log_bucket_t *b = &e->bucket;
    if (s->kiln_filt_c < b->kiln_min_c) {
        b->kiln_min_c = s->kiln_filt_c;
    }
    if (s->kiln_filt_c > b->kiln_max_c) {
        b->kiln_max_c = s->kiln_filt_c;
    }
    if (s->setpoint_c < b->sp_min_c) {
        b->sp_min_c = s->setpoint_c;
    }
    if (s->setpoint_c > b->sp_max_c) {
        b->sp_max_c = s->setpoint_c;
    }
    if (s->case_c < b->case_min_c) {
        b->case_min_c = s->case_c;
    }
    if (s->case_c > b->case_max_c) {
        b->case_max_c = s->case_c;
    }
    if (s->current_a < b->cur_min_a) {
        b->cur_min_a = s->current_a;
    }
    if (s->current_a > b->cur_max_a) {
        b->cur_max_a = s->current_a;
    }
    if (s->duty_permille < b->duty_min) {
        b->duty_min = s->duty_permille;
    }
    if (s->duty_permille > b->duty_max) {
        b->duty_max = s->duty_permille;
    }
    b->flags         = (uint8_t)(b->flags | s->flags);
    b->current_flags = (uint8_t)(b->current_flags | s->current_flags);
    b->state         = s->state;
    b->count++;
}

bool emit_visit(void *user, uint32_t run, const uint8_t rec[KILN_LOG_RECORD_BYTES])
{
    emit_ctx_t *e = static_cast<emit_ctx_t *>(user);
    (void)run;
    if (e->sink->failed) {
        return false; /* the client went away */
    }

    kiln_log_sample_t s;
    if (kiln_logrec_decode(rec, &s) != KILN_OK) {
        return true;
    }
    if (s.t_rel_ms < e->from_ms) {
        return true;
    }
    if (e->to_ms > e->from_ms && s.t_rel_ms > e->to_ms) {
        return true;
    }

    uint32_t index = (s.t_rel_ms - e->from_ms) / e->bucket_ms;
    /* The sample at exactly to_ms divides into index == max_points, which would
     * make a query for N points return N + 1 -- and overrun a caller that sized
     * its array to N.  It belongs in the last bucket. */
    if (index >= e->max_points) {
        index = e->max_points - 1u;
    }
    e->accepted++;

    if (!e->have_bucket) {
        bucket_start(e, &s, index);
        return true;
    }
    if (index == e->bucket_index) {
        bucket_add(e, &s);
        return true;
    }
    /* Crossed into a new bucket: the previous one is complete and can go out. */
    emit_bucket(e);
    bucket_start(e, &s, index);
    return true;
}

} // namespace

/* --- entry point -------------------------------------------------------- */

kiln_err_t kiln_api_log_stream(kiln_api_ctx_t *ctx, const kiln_api_req_t *req,
                               kiln_api_write_fn write, void *user,
                               kiln_api_resp_t *resp)
{
    if ((ctx == nullptr) || (req == nullptr) || (write == nullptr) || (resp == nullptr) ||
        (resp->body == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    resp->streaming = false;
    if ((ctx->logstore == nullptr) || (ctx->logstore->iterate == nullptr)) {
        kiln_api_error(resp, 503, "no_storage", "the log store is unavailable");
        return KILN_ERR_IO;
    }

    uint32_t run_id = 0, from_ms = 0, to_ms = 0, max_points = 800;
    (void)kiln_api_query_uint(req->query, "run", &run_id);
    (void)kiln_api_query_uint(req->query, "from", &from_ms);
    (void)kiln_api_query_uint(req->query, "to", &to_ms);
    (void)kiln_api_query_uint(req->query, "max_points", &max_points);

    char format[8] = "json";
    (void)kiln_api_query_get(req->query, "format", format, sizeof(format));
    const bool csv = strcmp(format, "csv") == 0;
    if (!csv && strcmp(format, "json") != 0) {
        kiln_api_error(resp, 400, "invalid_value", "format must be json or csv");
        return KILN_ERR_INVALID_ARG;
    }

    if (max_points == 0) {
        max_points = 1;
    }
    if (max_points > KILN_API_MAX_POINTS) {
        max_points = KILN_API_MAX_POINTS;
    }
    if (to_ms > 0 && to_ms < from_ms) {
        kiln_api_error(resp, 400, "invalid_range", "to must not precede from");
        return KILN_ERR_INVALID_ARG;
    }

    /* Pass one: the actual extent of what is stored for this query. */
    span_ctx_t span = {};
    span.run_id  = run_id;
    span.from_ms = from_ms;
    span.to_ms   = to_ms;
    const kiln_err_t se = ctx->logstore->iterate(ctx->logstore->ctx, run_id,
                                                 span_visit, &span);
    if (se != KILN_OK) {
        kiln_api_error(resp, 503, "log_unreadable", "the log store could not be read");
        return se;
    }

    sink_t sink = {};
    sink.write = write;
    sink.user  = user;

    resp->status       = 200;
    resp->content_type = csv ? "text/csv" : "application/json";
    resp->streaming    = true;
    resp->body_len     = 0;
    resp->truncated    = false;

    if (!span.any) {
        /* An empty result, not an error: a run whose samples the ring has
         * overwritten is a real and expected case (SWR-LOG-09). */
        if (csv) {
            sink_str(&sink, "t_rel_ms,kiln_min_c,kiln_max_c,sp_min_c,sp_max_c,"
                            "case_min_c,case_max_c,cur_min_a,cur_max_a,"
                            "duty_min,duty_max,state,samples\n");
        } else {
            sink_str(&sink, "{\"run\":");
            sink_fmt(&sink, "%u", (unsigned)run_id);
            sink_str(&sink, ",\"points\":0,\"samples\":0,\"series\":[]}");
        }
        sink_flush(&sink);
        return sink.failed ? KILN_ERR_IO : KILN_OK;
    }

    const uint32_t span_from = from_ms > span.t_min ? from_ms : span.t_min;
    const uint32_t span_to   = (to_ms > 0 && to_ms < span.t_max) ? to_ms : span.t_max;
    uint32_t bucket_ms = (span_to > span_from)
                       ? (span_to - span_from) / max_points : 0u;
    if (bucket_ms == 0) {
        bucket_ms = 1u;
    }

    emit_ctx_t emit = {};
    emit.sink       = &sink;
    emit.from_ms    = span_from;
    emit.to_ms      = span_to;
    emit.bucket_ms  = bucket_ms;
    emit.max_points = max_points;
    emit.csv        = csv;

    if (csv) {
        sink_str(&sink, "t_rel_ms,kiln_min_c,kiln_max_c,sp_min_c,sp_max_c,"
                        "case_min_c,case_max_c,cur_min_a,cur_max_a,"
                        "duty_min,duty_max,state,samples\n");
    } else {
        sink_fmt(&sink, "{\"run\":%u,\"from_ms\":%u,\"to_ms\":%u,\"bucket_ms\":%u,"
                        "\"samples\":%u,"
                        "\"columns\":[\"t_rel_ms\",\"kiln_min_c\",\"kiln_max_c\","
                        "\"sp_min_c\",\"sp_max_c\",\"case_min_c\",\"case_max_c\","
                        "\"cur_min_a\",\"cur_max_a\",\"duty_min\",\"duty_max\","
                        "\"state\"],\"series\":[",
                 (unsigned)run_id, (unsigned)span_from, (unsigned)span_to,
                 (unsigned)bucket_ms, (unsigned)span.count);
    }

    const kiln_err_t ee = ctx->logstore->iterate(ctx->logstore->ctx, run_id,
                                                 emit_visit, &emit);
    emit_bucket(&emit);          /* the last one, which nothing crossed out of */

    if (!csv) {
        sink_fmt(&sink, "],\"points\":%u}", (unsigned)emit.emitted);
    }
    sink_flush(&sink);

    if (sink.failed) {
        return KILN_ERR_IO;
    }
    return ee;
}

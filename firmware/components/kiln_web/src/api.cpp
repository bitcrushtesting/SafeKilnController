/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <stdio.h>
#include <string.h>
#include <iterator>
#include "kiln_app/program_store.h"
#include "kiln_app/run_index.h"
#include "kiln_core/faults.h"
#include "kiln_core/profile.h"
#include "kiln_web/api.h"
#include "kiln_web/json.h"

/* --- small helpers ------------------------------------------------------ */

namespace {

void resp_begin(kiln_api_resp_t *r, kiln_json_t *j)
{
    r->status       = 200;
    r->content_type = "application/json";
    r->truncated    = false;
    r->streaming    = false;
    kiln_json_init(j, r->body, r->body_cap);
}

kiln_err_t resp_end(kiln_api_resp_t *r, kiln_json_t *j)
{
    r->body_len  = kiln_json_len(j);
    r->truncated = !kiln_json_ok(j);
    if (r->truncated) {
        /* A truncated body is not a response.  Saying so with a 500 is better
         * than sending half a document the client will fail to parse and blame
         * itself for. */
        kiln_api_error(r, 500, "response_too_large",
                       "the response did not fit the device's buffer");
        return KILN_ERR_NO_SPACE;
    }
    return KILN_OK;
}

} // namespace

void kiln_api_error(kiln_api_resp_t *resp, int status, const char *code,
                    const char *message)
{
    if ((resp == nullptr) || (resp->body == nullptr) || resp->body_cap == 0) {
        return;
    }

    kiln_json_t j;
    resp->status       = status;
    resp->content_type = "application/json";
    resp->streaming    = false;
    kiln_json_init(&j, resp->body, resp->body_cap);

    /* FR-WEB-20: a machine-readable code and a human-readable message. */
    kiln_json_obj_open(&j);
    kiln_json_key(&j, "error");
    kiln_json_obj_open(&j);
    kiln_json_kv_str(&j, "code", (code != nullptr) ? code : "error");
    kiln_json_kv_str(&j, "message", (message != nullptr) ? message : "");
    kiln_json_obj_close(&j);
    kiln_json_obj_close(&j);

    resp->body_len  = kiln_json_len(&j);
    resp->truncated = !kiln_json_ok(&j);
}

namespace {

/* Map a kiln_err_t from a command onto the status a client should see.  One
 * mapping, so two endpoints cannot disagree about what KILN_ERR_STATE means. */
void resp_from_err(kiln_api_resp_t *r, kiln_err_t e, const char *what)
{
    switch (e) {
    case KILN_OK:
        break;
    case KILN_ERR_STATE:
        kiln_api_error(r, 409, "not_allowed_now", what);
        break;
    case KILN_ERR_RANGE:
    case KILN_ERR_INVALID_ARG:
        kiln_api_error(r, 400, "invalid_request", what);
        break;
    case KILN_ERR_NOT_FOUND:
        kiln_api_error(r, 404, "not_found", what);
        break;
    case KILN_ERR_NO_SPACE:
        kiln_api_error(r, 507, "no_space", what);
        break;
    case KILN_ERR_UNSUPPORTED:
        kiln_api_error(r, 501, "unsupported", what);
        break;
    default:
        kiln_api_error(r, 500, "internal", what);
        break;
    }
}

bool path_is(const char *path, const char *want)
{
    return strcmp(path, want) == 0;
}

/* `/api/programs/7/preview` -> prefix "/api/programs/", id 7, tail "preview". */
bool path_split_id(const char *path, const char *prefix,
                          uint32_t *id_out, const char **tail_out)
{
    const size_t plen = strlen(prefix);
    if (strncmp(path, prefix, plen) != 0) {
        return false;
    }

    const char *p = path + plen;
    if (*p < '0' || *p > '9') {
        return false;
    }

    uint32_t id = 0;
    while (*p >= '0' && *p <= '9') {
        id = id * 10u + (uint32_t)(*p - '0');
        if (id > 100000u) {
            return false;
        }
        p++;
    }
    *id_out = id;

    if (*p == '\0') { *tail_out = ""; return true; }
    if (*p != '/') {
        return false;
    }
    *tail_out = p + 1;
    return true;
}

int hexv(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

} // namespace

bool kiln_api_query_get(const char *query, const char *key, char *out, size_t cap)
{
    if ((query == nullptr) || (key == nullptr) || (out == nullptr) || cap == 0) {
        return false;
    }

    const size_t klen = strlen(key);
    const char  *p    = query;

    while ((*p) != 0) {
        const char *amp = strchr(p, '&');
        const char *end = (amp != nullptr) ? amp : p + strlen(p);
        const char *eq  = static_cast<const char *>(memchr(p, '=', (size_t)(end - p)));

        if ((eq != nullptr) && (size_t)(eq - p) == klen && strncmp(p, key, klen) == 0) {
            size_t o = 0;
            for (const char *v = eq + 1; v < end; v++) {
                if (o + 1 >= cap) {
                    return false; /* refuse, not truncate */
                }
                if (*v == '+') { out[o++] = ' '; continue; }
                if (*v == '%' && v + 2 < end) {
                    const int h = hexv(v[1]), l = hexv(v[2]);
                    if (h >= 0 && l >= 0) {
                        out[o++] = (char)(((unsigned)h << 4u) | (unsigned)l);
                        v += 2;
                        continue;
                    }
                }
                out[o++] = *v;
            }
            out[o] = '\0';
            return true;
        }
        if (amp == nullptr) {
            break;
        }
        p = amp + 1;
    }
    return false;
}

bool kiln_api_query_uint(const char *query, const char *key, uint32_t *out)
{
    char v[16];
    if (!kiln_api_query_get(query, key, v, sizeof(v))) {
        return false;
    }
    if (v[0] == '\0') {
        return false;
    }

    uint32_t n = 0;
    for (const char *p = v; (*p) != 0; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        if (n > 429496729u) {
            return false;
        }
        n = n * 10u + (uint32_t)(*p - '0');
    }
    *out = n;
    return true;
}

bool kiln_api_needs_auth(const kiln_api_req_t *req)
{
    if (req == nullptr) {
        return true;
    }
    /* FR-WEB-23 says state-changing endpoints.  Decided by method rather than by
     * an enumerated list, so a route added later is protected by default rather
     * than by remembering to add it. */
    return req->method != KILN_HTTP_GET;
}

/* --- GET /api/status (FR-RUN-05) --------------------------------------- */

namespace {

/* FR-WEB-26: the web interface is an observation surface.  Nothing reachable
 * over the network may put heat into the kiln.
 *
 * 403 and not 405.  405 means "wrong verb for this URL" and invites a client to
 * try another one; the truth here is that the operation does not exist on this
 * interface at any verb, and the operator has to walk to the kiln.  The message
 * says so, because a UI that reports "method not allowed" teaches nobody where
 * the control actually is.
 *
 * The handlers behind these routes are *deleted*, not disabled: the firmware
 * cannot start a firing over HTTP because the code to do it is not in the
 * image, which is a stronger claim than a flag that could be flipped back.
 */
kiln_err_t reject_read_only(kiln_api_resp_t *resp, const char *what)
{
    char msg[200];
    /* Truncation would only shorten a diagnostic that is already advisory, and
     * every caller passes a short literal; msg stays NUL-terminated either way. */
    (void)snprintf(msg, sizeof(msg),
                   "%s is not available over the network; use the controls on the kiln",
                   what);
    kiln_api_error(resp, 403, "read_only", msg);
    return KILN_ERR_UNSUPPORTED;
}

void write_warnings(kiln_json_t *j, uint32_t mask, kiln_lang_t lang)
{
    kiln_json_key(j, "warnings");
    kiln_json_arr_open(j);
    for (uint8_t b = 0; b < KILN_WARN_COUNT; b++) {
        if ((mask & KILN_WARN_BIT(b)) == 0u) {
            continue;
        }
        kiln_json_obj_open(j);
        /* NFR-23: the code is the stable thing a client should key on; the
         * text is a courtesy, rendered in the configured language. */
        kiln_json_kv_uint(j, "code", kiln_warn_code((kiln_warn_bit_t)b));
        kiln_json_kv_str(j, "label", kiln_warn_label_in((kiln_warn_bit_t)b, lang));
        kiln_json_kv_str(j, "message", kiln_warn_cause_in((kiln_warn_bit_t)b, lang));
        kiln_json_obj_close(j);
    }
    kiln_json_arr_close(j);
}

void write_status(kiln_api_ctx_t *ctx, kiln_json_t *j)
{
    const kiln_app_t *a = ctx->app;
    kiln_snapshot_t s;
    kiln_app_snapshot(a, &s);

    kiln_json_obj_open(j);

    kiln_json_kv_str(j, "state", kiln_state_label((kiln_state_t)s.state));
    kiln_json_kv_uint(j, "state_code", s.state);

    /* FR-WEB-25 depends on the client being able to tell stale from current, so
     * validity travels with the value rather than being inferred from it. */
    kiln_json_key(j, "kiln_c");
    if (s.kiln_valid) {
        kiln_json_num(j, s.kiln_c, 1);
    }
    else {
        kiln_json_null(j);
    }
    kiln_json_kv_num(j, "kiln_raw_c", s.kiln_c_raw, 1);
    kiln_json_kv_bool(j, "kiln_valid", s.kiln_valid);

    kiln_json_key(j, "case_c");
    if (s.case_valid) {
        kiln_json_num(j, s.case_c, 1);
    }
    else {
        kiln_json_null(j);
    }

    kiln_json_kv_num(j, "setpoint_c", s.setpoint_c, 1);
    kiln_json_kv_num(j, "rate_c_per_h", s.rate_c_per_h, 1);
    kiln_json_kv_uint(j, "duty_permille", s.duty_permille);
    kiln_json_kv_bool(j, "heat_authorised", s.heat_authorised);
    kiln_json_kv_bool(j, "holdback", s.holdback_active);
    kiln_json_kv_bool(j, "duty_saturated", s.duty_saturated);

    kiln_json_kv_num(j, "current_a", s.current_a, 2);
    kiln_json_kv_num(j, "current_ref_a", s.current_ref_a, 2);
    kiln_json_kv_uint(j, "current_flags", s.current_flags);

    kiln_json_kv_uint(j, "segment", s.segment_index + 1u);
    kiln_json_kv_uint(j, "segment_count", s.segment_count);
    kiln_json_kv_uint(j, "elapsed_s", (unsigned long long)a->run_elapsed_s);
    kiln_json_kv_uint(j, "remaining_s", kiln_setpoint_remaining_s(&a->sp));
    kiln_json_kv_uint(j, "segment_remaining_s", kiln_setpoint_segment_remaining_s(&a->sp));
    kiln_json_kv_bool(j, "awaiting_ack", kiln_setpoint_awaiting_ack(&a->sp));
    kiln_json_kv_uint(j, "run_id", a->record.run_id);
    kiln_json_kv_str(j, "program", a->sp.started ? a->sp.prog.name : "");

    /* FR-WEB-24: the banner needs the fault and its operator text in one place. */
    kiln_json_key(j, "fault");
    if (a->fault == KILN_FAULT_NONE) {
        kiln_json_null(j);
    } else {
        kiln_json_obj_open(j);
        kiln_json_kv_uint(j, "code", a->fault);
        kiln_json_kv_str(j, "label", kiln_fault_label(a->fault));
        kiln_json_kv_str(j, "message", kiln_fault_cause(a->fault));
        kiln_json_kv_str(j, "requirement", kiln_fault_requirement(a->fault));
        kiln_json_obj_close(j);
    }
    write_warnings(j, a->warnings, (kiln_lang_t)ctx->app->cfg.language);

    kiln_json_obj_close(j);
}

/* --- GET /api/info (FR-UPD-06) ----------------------------------------- */

void write_info(kiln_api_ctx_t *ctx, kiln_json_t *j)
{
    kiln_json_obj_open(j);

    /* NFR-23: what language the device is rendering in, so the client can
     * match it and set the document language for a screen reader. */
    kiln_json_kv_str(j, "language", kiln_lang_tag((kiln_lang_t)ctx->app->cfg.language));

    kiln_fw_info_t fw = {};
    if ((ctx->system != nullptr) && (ctx->system->fw_info != nullptr)) {
        (void)ctx->system->fw_info(ctx->system->ctx, &fw);
    }
    kiln_json_kv_str(j, "version", fw.version);
    kiln_json_kv_str(j, "build_time", fw.build_time);
    kiln_json_kv_str(j, "git_rev", fw.git_rev);
    kiln_json_kv_str(j, "target", (fw.target[0] != 0) ? fw.target : "host");
    kiln_json_kv_str(j, "idf_version", fw.idf_version);

    kiln_sys_stats_t st = {};
    if ((ctx->system != nullptr) && (ctx->system->stats != nullptr)) {
        (void)ctx->system->stats(ctx->system->ctx, &st);
    }
    kiln_json_kv_uint(j, "uptime_s", st.uptime_s);
    kiln_json_kv_uint(j, "heap_free", st.heap_free);
    kiln_json_kv_uint(j, "heap_min_free", st.heap_min_free);

    /* FR-TUN-11: gains, and where they came from -- factory defaults are not
     * gains for *this* kiln, and the UI has to be able to say so. */
    kiln_json_key(j, "gains");
    kiln_json_obj_open(j);
    kiln_json_kv_num(j, "kp", ctx->app->cfg.kp, 4);
    kiln_json_kv_num(j, "ki", ctx->app->cfg.ki, 5);
    kiln_json_kv_num(j, "kd", ctx->app->cfg.kd, 3);
    kiln_json_kv_str(j, "set", ctx->app->cfg.gain_set_name);
    kiln_json_kv_bool(j, "tuned",
                      (ctx->app->warnings & KILN_WARN_BIT(KILN_WARN_GAINS_UNTUNED)) == 0u);
    kiln_json_obj_close(j);

    /* Both limits, never one: the configurable ceiling is meaningless without
     * the backstop above it (AD-22, SR-23). */
    kiln_json_kv_uint(j, "temp_ceiling_c", (unsigned long long)KILN_TEMP_CEILING_C);
    kiln_json_kv_uint(j, "supervisor_trip_c",
                      (unsigned long long)KILN_SUPERVISOR_TRIP_C);
    kiln_json_obj_close(j);
}

/* --- /api/config (FR-CFG-01..08, FR-WEB-17) ---------------------------- */

/* The whole point of configmodel's table: the API projection is generated from
 * it, so an item cannot exist in the firmware and be missing from the UI. */
void write_config(kiln_api_ctx_t *ctx, kiln_json_t *j)
{
    kiln_json_obj_open(j);
    kiln_json_key(j, "items");
    kiln_json_arr_open(j);

    for (uint16_t i = 0; i < kiln_config_item_count(); i++) {
        const kiln_cfg_item_t *it = kiln_config_item(i);
        kiln_json_obj_open(j);
        kiln_json_kv_str(j, "key", it->key);
        kiln_json_kv_str(j, "unit", it->unit);
        kiln_json_kv_str(j, "requirement", it->req);

        if (it->type == KILN_CFG_T_STRING) {
            kiln_json_kv_str(j, "type", "string");
            kiln_json_kv_uint(j, "max_len", it->str_cap - 1u);
            /* FR-CFG-07: a secret is never serialised outward, only whether it
             * is set.  There is no mode in which this endpoint returns one. */
            if ((it->flags & KILN_CFG_F_SECRET) != 0u) {
                const char *v = NULL;
                (void)kiln_config_get_str(&ctx->app->cfg, it, &v);
                kiln_json_kv_bool(j, "set", (v != nullptr) && v[0] != '\0');
            }
            else {
                const char *v = NULL;
                (void)kiln_config_get_str(&ctx->app->cfg, it, &v);
                kiln_json_kv_str(j, "value", (v != nullptr) ? v : "");
                kiln_json_kv_str(j, "default", (it->def_str != nullptr) ? it->def_str : "");
            }
        } else {
            double v = 0.0;
            (void)kiln_config_get_num(&ctx->app->cfg, it, &v);

            switch (it->type) {
            case KILN_CFG_T_BOOL:  kiln_json_kv_str(j, "type", "bool");  break;
            case KILN_CFG_T_ENUM:  kiln_json_kv_str(j, "type", "enum");  break;
            case KILN_CFG_T_FLOAT: kiln_json_kv_str(j, "type", "float"); break;
            default:               kiln_json_kv_str(j, "type", "int");   break;
            }
            if (it->type == KILN_CFG_T_BOOL) {
                kiln_json_kv_bool(j, "value", v != 0.0);
                kiln_json_kv_bool(j, "default", it->def != 0.0);
            } else {
                const int dp = (it->type == KILN_CFG_T_FLOAT) ? 4 : 0;
                kiln_json_kv_num(j, "value", v, dp);
                kiln_json_kv_num(j, "default", it->def, dp);
                kiln_json_kv_num(j, "min", it->min, dp);
                kiln_json_kv_num(j, "max", it->max, dp);
            }
            if (it->type == KILN_CFG_T_ENUM && (it->enum_names != nullptr)) {
                kiln_json_key(j, "options");
                kiln_json_arr_open(j);
                for (uint8_t k = 0; k < it->enum_count; k++) {
                    kiln_json_str(j, it->enum_names[k]);
                }
                kiln_json_arr_close(j);
            }
        }

        /* FR-CFG-04 and FR-CFG-08, so the UI can mark and disable rather than
         * letting the operator discover a refusal after pressing save. */
        kiln_json_kv_bool(j, "reboot_required", (it->flags & KILN_CFG_F_REBOOT) != 0);
        kiln_json_kv_bool(j, "locked_while_running",
            (it->flags & (KILN_CFG_F_SAFETY | KILN_CFG_F_LOCKED_RUNNING)) != 0);
        kiln_json_kv_bool(j, "secret", (it->flags & KILN_CFG_F_SECRET) != 0);
        kiln_json_obj_close(j);
    }
    kiln_json_arr_close(j);
    kiln_json_obj_close(j);
}


/* --- /api/programs (FR-PRG-07, FR-WEB-12, FR-WEB-13) ------------------- */

void write_program(kiln_json_t *j, const kiln_program_t *p, uint8_t id,
                          float max_temp_c)
{
    kiln_json_obj_open(j);
    kiln_json_kv_uint(j, "id", id);
    kiln_json_kv_str(j, "name", p->name);
    kiln_json_kv_str(j, "description", p->description);
    kiln_json_kv_bool(j, "readonly", (p->flags & KILN_PROG_FLAG_READONLY) != 0);
    kiln_json_kv_uint(j, "segment_count", p->segment_count);
    kiln_json_kv_num(j, "peak_c", kiln_profile_peak_c(p), 0);
    kiln_json_kv_uint(j, "duration_s", kiln_profile_duration_s(p, 20.0f));

    const kiln_prog_validation_t v = kiln_profile_validate(p, max_temp_c);
    kiln_json_kv_bool(j, "valid", v.code == KILN_PROG_OK);
    if (v.code != KILN_PROG_OK) {
        kiln_json_kv_str(j, "invalid_reason", kiln_profile_validation_str(v.code));
        kiln_json_kv_int(j, "invalid_segment",
                         v.segment == KILN_SEG_NONE ? -1 : (int)v.segment);
    }

    kiln_json_key(j, "segments");
    kiln_json_arr_open(j);
    for (uint8_t i = 0; i < p->segment_count && i < KILN_MAX_SEGMENTS; i++) {
        kiln_json_obj_open(j);
        kiln_json_kv_uint(j, "target_c", p->segments[i].target_c);
        kiln_json_kv_uint(j, "rate_c_per_h", p->segments[i].rate_c_per_h);
        kiln_json_kv_uint(j, "dwell_min", p->segments[i].dwell_min);
        kiln_json_kv_bool(j, "require_ack",
            (p->segments[i].flags & KILN_SEG_FLAG_REQUIRE_ACK) != 0);
        kiln_json_obj_close(j);
    }
    kiln_json_arr_close(j);
    kiln_json_obj_close(j);
}




/* --- /api/run and friends ---------------------------------------------- */

kiln_err_t ok_response(kiln_api_resp_t *resp)
{
    kiln_json_t j;
    resp_begin(resp, &j);
    kiln_json_obj_open(&j);
    kiln_json_kv_bool(&j, "ok", true);
    kiln_json_obj_close(&j);
    return resp_end(resp, &j);
}


/* --- /api/manual, /api/fault/ack, /api/tune ---------------------------- */


kiln_err_t handle_tune(kiln_api_ctx_t *ctx, const kiln_api_req_t *req,
                              const char *tail, kiln_api_resp_t *resp)
{
    const kiln_autotune_t *at = &ctx->app->tune;

    /* FR-WEB-26: watching a tune converge is reading; starting one heats the
     * kiln to its relay-oscillation amplitude, which is firing by another name. */
    if (req->method != KILN_HTTP_GET) {
        return reject_read_only(resp, "starting or cancelling automatic tuning");
    }

    if (req->method == KILN_HTTP_GET && path_is(tail, "")) {
        /* FR-TUN-10: phase, cycles and the candidate gain sets, so the operator
         * can watch it converge rather than waiting in the dark. */
        kiln_json_t j;
        resp_begin(resp, &j);
        kiln_json_obj_open(&j);
        kiln_json_kv_str(&j, "phase", kiln_autotune_phase_str(at->phase));
        kiln_json_kv_uint(&j, "cycles", at->cycle_count);
        kiln_json_kv_uint(&j, "required_cycles", at->cfg.required_cycles);
        kiln_json_kv_num(&j, "elapsed_s", at->elapsed_s, 0);
        kiln_json_kv_num(&j, "setpoint_c", at->cfg.setpoint_c, 1);
        kiln_json_kv_num(&j, "ku", at->ku, 4);
        kiln_json_kv_num(&j, "tu", at->tu, 1);
        kiln_json_kv_bool(&j, "done", kiln_autotune_done(at));
        kiln_json_kv_bool(&j, "succeeded", kiln_autotune_succeeded(at));
        if (at->phase == KILN_TUNE_FAILED) {
            kiln_json_kv_str(&j, "fail_reason", kiln_fault_cause(at->fail_reason));
        }
        kiln_json_key(&j, "candidates");
        kiln_json_arr_open(&j);
        for (int r = 0; r < KILN_TUNE_RULE_COUNT; r++) {
            kiln_json_obj_open(&j);
            kiln_json_kv_str(&j, "rule", kiln_autotune_rule_str((kiln_tune_rule_t)r));
            kiln_json_kv_num(&j, "kp", at->gains[r].kp, 4);
            kiln_json_kv_num(&j, "ki", at->gains[r].ki, 5);
            kiln_json_kv_num(&j, "kd", at->gains[r].kd, 3);
            kiln_json_obj_close(&j);
        }
        kiln_json_arr_close(&j);
        kiln_json_obj_close(&j);
        return resp_end(resp, &j);
    }

    if (req->method != KILN_HTTP_POST) {
        kiln_api_error(resp, 405, "method_not_allowed", "use GET or POST");
        return KILN_ERR_UNSUPPORTED;
    }

    if (path_is(tail, "")) {
        static kiln_json_tok_t toks[KILN_API_MAX_TOKENS];
        const int ntok = kiln_json_parse(req->body, req->body_len, toks, KILN_API_MAX_TOKENS);
        double sp = 0.0;
        if (ntok < 1 || !kiln_json_get_num(req->body, toks, ntok, 0, "setpoint_c", &sp)) {
            kiln_api_error(resp, 400, "invalid_request", "setpoint_c is required");
            return KILN_ERR_INVALID_ARG;
        }
        const kiln_err_t e = kiln_app_autotune(ctx->app, (float)sp);
        if (e != KILN_OK && e != KILN_ERR_RANGE) {
            resp_from_err(resp, e, "tuning cannot start in this state");
            return e;
        }
        kiln_json_t j;
        resp_begin(resp, &j);
        kiln_json_obj_open(&j);
        kiln_json_kv_bool(&j, "ok", true);
        /* KILN_ERR_RANGE here means it is running, but clamped below what was
         * asked for (SR-23).  Silently tuning somewhere else would be worse. */
        kiln_json_kv_num(&j, "setpoint_c", ctx->app->tune.cfg.setpoint_c, 1);
        kiln_json_kv_bool(&j, "clamped", e == KILN_ERR_RANGE);
        kiln_json_obj_close(&j);
        return resp_end(resp, &j);
    }

    if (path_is(tail, "cancel")) {
        kiln_autotune_cancel(&ctx->app->tune);
        (void)kiln_app_abort(ctx->app);
        return ok_response(resp);
    }

    if (path_is(tail, "accept")) {
        /* FR-TUN-09: nothing is stored until the operator picks a rule. */
        if (!kiln_autotune_succeeded(at)) {
            kiln_api_error(resp, 409, "no_result", "there is no tuning result to accept");
            return KILN_ERR_STATE;
        }
        static kiln_json_tok_t toks[KILN_API_MAX_TOKENS];
        const int ntok = kiln_json_parse(req->body, req->body_len, toks, KILN_API_MAX_TOKENS);
        char rule[32] = "tyreus-luyben";
        (void)kiln_json_get_str(req->body, toks, ntok, 0, "rule", rule, sizeof(rule));

        int which = -1;
        for (int r = 0; r < KILN_TUNE_RULE_COUNT; r++) {
            if (strcmp(rule, kiln_autotune_rule_str((kiln_tune_rule_t)r)) == 0) {
                which = r;
            }
        }
        if (which < 0) {
            kiln_api_error(resp, 400, "invalid_value", "unknown tuning rule");
            return KILN_ERR_INVALID_ARG;
        }

        kiln_config_t next = ctx->app->cfg;
        next.kp = at->gains[which].kp;
        next.ki = at->gains[which].ki;
        next.kd = at->gains[which].kd;
        const kiln_cfg_item_t *bad = NULL;
        const kiln_err_t e = kiln_app_apply_config(ctx->app, &next, &bad);
        if (e != KILN_OK) {
            resp_from_err(resp, e, "the tuned gains were rejected");
            return e;
        }
        ctx->app->warnings &= ~KILN_WARN_BIT(KILN_WARN_GAINS_UNTUNED);
        (void)kiln_app_idle(ctx->app);
        return ok_response(resp);
    }

    kiln_api_error(resp, 404, "not_found", "no such tune route");
    return KILN_ERR_NOT_FOUND;
}

/* --- /api/runs, /api/current, /api/storage, /api/net ------------------- */

/* FR-PRG-07: the stored programs, read only. */
kiln_err_t handle_program_list(kiln_api_ctx_t *ctx, kiln_api_resp_t *resp)
{
    if (ctx->filestore == nullptr) {
        kiln_api_error(resp, 503, "no_storage", "program storage is unavailable");
        return KILN_ERR_IO;
    }
    kiln_json_t j;
    resp_begin(resp, &j);
    kiln_json_obj_open(&j);
    kiln_json_key(&j, "programs");
    kiln_json_arr_open(&j);
    const uint8_t n = kiln_program_store_count(ctx->filestore);
    for (uint8_t i = 0; i < n; i++) {
        kiln_program_t prog;
        if (kiln_program_store_get_slot(ctx->filestore, i, &prog) != KILN_OK) {
            continue;
        }
        write_program(&j, &prog, i, ctx->app->cfg.max_temp_c);
    }
    kiln_json_arr_close(&j);
    kiln_json_obj_close(&j);
    return resp_end(resp, &j);
}

/* One stored program by slot.  A missing slot is 404 and not an empty list:
 * a client asking for a program that is not there has made a different
 * mistake from one asking what programs exist. */
kiln_err_t handle_program_one(kiln_api_ctx_t *ctx, uint8_t id,
                                     kiln_api_resp_t *resp)
{
    if (ctx->filestore == nullptr) {
        kiln_api_error(resp, 503, "no_storage", "program storage is unavailable");
        return KILN_ERR_IO;
    }
    kiln_program_t prog;
    if (kiln_program_store_get_slot(ctx->filestore, id, &prog) != KILN_OK) {
        kiln_api_error(resp, 404, "not_found", "no program in that slot");
        return KILN_ERR_NOT_FOUND;
    }
    kiln_json_t j;
    resp_begin(resp, &j);
    write_program(&j, &prog, id, ctx->app->cfg.max_temp_c);
    return resp_end(resp, &j);
}

kiln_err_t handle_runs(kiln_api_ctx_t *ctx, kiln_api_resp_t *resp)
{
    if (ctx->filestore == nullptr) {
        kiln_api_error(resp, 503, "no_storage", "run history is unavailable");
        return KILN_ERR_IO;
    }
    /* Newest first, which is the order the history view wants. */
    kiln_run_record_t recs[KILN_RUN_SLOTS];
    uint8_t n = 0;
    for (uint8_t slot = 0; slot < KILN_RUN_SLOTS; slot++) {
        if (kiln_run_index_get_slot(ctx->filestore, slot, &recs[n]) == KILN_OK) {
            n++;
        }
    }
    for (uint8_t i = 0; i + 1 < n; i++) {
        for (uint8_t k = 0; k + 1 < n - i; k++) {
            if (recs[k].run_id < recs[k + 1].run_id) {
                const kiln_run_record_t t = recs[k];
                recs[k] = recs[k + 1];
                recs[k + 1] = t;
            }
        }
    }

    kiln_json_t j;
    resp_begin(resp, &j);
    kiln_json_obj_open(&j);
    kiln_json_key(&j, "runs");
    kiln_json_arr_open(&j);
    for (uint8_t i = 0; i < n; i++) {
        const kiln_run_record_t *r = &recs[i];
        kiln_json_obj_open(&j);
        kiln_json_kv_uint(&j, "run_id", r->run_id);
        kiln_json_kv_str(&j, "program", r->program.name);
        kiln_json_kv_uint(&j, "start_utc_s", r->start_wall_utc_s);
        kiln_json_kv_uint(&j, "end_utc_s", r->end_wall_utc_s);
        kiln_json_kv_uint(&j, "duration_s", r->duration_s);
        kiln_json_kv_num(&j, "peak_c", r->peak_c, 1);
        kiln_json_kv_num(&j, "current_ref_a", r->current_ref_a, 2);
        kiln_json_kv_num(&j, "energy_wh", r->energy_wh, 0);
        kiln_json_kv_str(&j, "end_reason", kiln_run_end_str((kiln_run_end_t)r->end_reason));
        if (r->fault != KILN_FAULT_NONE) {
            kiln_json_kv_str(&j, "fault", kiln_fault_label((kiln_fault_t)r->fault));
        }
        /* FR-LOG-09: say so, rather than letting an empty chart look like a bug. */
        kiln_json_kv_bool(&j, "samples_truncated",
                          (r->flags & KILN_RUN_FLAG_TRUNCATED) != 0);
        kiln_json_obj_close(&j);
    }
    kiln_json_arr_close(&j);
    kiln_json_obj_close(&j);
    return resp_end(resp, &j);
}

kiln_err_t handle_current(kiln_api_ctx_t *ctx, const kiln_api_req_t *req,
                                 const char *tail, kiln_api_resp_t *resp)
{
    const kiln_app_t     *app = ctx->app;
    const kiln_current_t *c   = &app->cur;

    if (req->method == KILN_HTTP_GET && path_is(tail, "")) {
        kiln_json_t j;
        resp_begin(resp, &j);
        kiln_json_obj_open(&j);

        kiln_json_kv_bool(&j, "enabled", c->cfg.enabled);
        kiln_json_kv_bool(&j, "available", kiln_current_available(c));
        kiln_json_kv_num(&j, "current_a", kiln_current_amps(c), 2);
        kiln_json_kv_num(&j, "reference_a", kiln_current_ref(c), 2);
        kiln_json_kv_bool(&j, "reference_valid", kiln_current_ref_valid(c));
        kiln_json_kv_bool(&j, "reference_rejected", c->ref_rejected);
        kiln_json_kv_uint(&j, "flags", kiln_current_flags(c));

        /* FR-CUR-07, with the assumption stated as the requirement demands. */
        kiln_json_kv_num(&j, "apparent_va", kiln_app_apparent_va(app), 0);
        kiln_json_kv_num(&j, "energy_wh", kiln_app_energy_wh(app), 1);
        kiln_json_kv_str(&j, "power_basis",
                         "apparent power, resistive load assumed");
        kiln_json_kv_uint(&j, "measurements", c->measurements);
        kiln_json_kv_uint(&j, "skipped", c->skipped);

        float dev = 0.0f;
        kiln_json_key(&j, "deviation");
        if (kiln_current_deviation(c, &dev)) {
            kiln_json_num(&j, dev, 4);
        }
        else {
            kiln_json_null(&j);
        }

        /* FR-CUR-13 */
        kiln_json_key(&j, "switching");
        kiln_json_obj_open(&j);
        kiln_json_kv_uint(&j, "contactor_ops", ctx->app->counters.contactor_ops);
        kiln_json_key(&j, "ssr_ops");
        kiln_json_arr_open(&j);
        for (uint8_t ch = 0; ch < KILN_HEAT_CHANNELS; ch++) {
            kiln_json_uint(&j, ctx->app->counters.ssr_ops[ch]);
        }
        kiln_json_arr_close(&j);
        kiln_json_kv_uint(&j, "contactor_life_ops", ctx->app->cfg.contactor_life_ops);
        kiln_json_kv_uint(&j, "ssr_life_ops", ctx->app->cfg.ssr_life_ops);
        kiln_json_obj_close(&j);

        kiln_json_obj_close(&j);
        return resp_end(resp, &j);
    }

    if (path_is(tail, "calibrate")) {
        /* FR-WEB-26: the calibration reference feeds SR-28's deviation bands. */
        return reject_read_only(resp, "calibrating the current transformer");
    }

    kiln_api_error(resp, 404, "not_found", "no such current route");
    return KILN_ERR_NOT_FOUND;
}

kiln_err_t handle_storage(kiln_api_ctx_t *ctx, kiln_api_resp_t *resp)
{
    kiln_json_t j;
    resp_begin(resp, &j);
    kiln_json_obj_open(&j);

    /* FR-LOG-15 */
    kiln_json_key(&j, "log");
    kiln_json_obj_open(&j);
    kiln_logstore_stats_t st = {};
    if ((ctx->ring != nullptr) && kiln_logring_stats(ctx->ring, &st) == KILN_OK) {
        kiln_json_kv_bool(&j, "available", st.available);
        kiln_json_kv_uint(&j, "records_stored", st.records_stored);
        kiln_json_kv_uint(&j, "records_total", st.records_total);
        kiln_json_kv_uint(&j, "sectors_used", st.sectors_used);
        kiln_json_kv_uint(&j, "sectors_total", st.sectors_total);
        kiln_json_kv_uint(&j, "erase_count", st.erase_count);
        kiln_json_kv_uint(&j, "write_errors", st.write_errors);
        kiln_json_kv_uint(&j, "hours_at_default_interval",
                          st.records_total * 10u / 3600u);
    }
    else {
        kiln_json_kv_bool(&j, "available", false);
    }
    kiln_json_kv_uint(&j, "dropped_samples", ctx->app->log_dropped);
    kiln_json_kv_uint(&j, "queue_errors", ctx->app->log_errors);
    kiln_json_obj_close(&j);

    kiln_json_key(&j, "files");
    kiln_json_obj_open(&j);
    if ((ctx->filestore != nullptr) && (ctx->filestore->usage != nullptr)) {
        size_t total = 0, used = 0;
        if (ctx->filestore->usage(ctx->filestore->ctx, &total, &used) == KILN_OK) {
            kiln_json_kv_uint(&j, "total_bytes", total);
            kiln_json_kv_uint(&j, "used_bytes", used);
        }
        kiln_json_kv_uint(&j, "programs", kiln_program_store_count(ctx->filestore));
        kiln_json_kv_uint(&j, "runs", kiln_run_index_count(ctx->filestore));
    }
    else {
        kiln_json_kv_bool(&j, "available", false);
    }
    kiln_json_obj_close(&j);

    kiln_json_obj_close(&j);
    return resp_end(resp, &j);
}

kiln_err_t handle_net(kiln_api_ctx_t *ctx, kiln_api_resp_t *resp)
{
    kiln_json_t j;
    resp_begin(resp, &j);
    kiln_json_obj_open(&j);

    kiln_net_status_t ns = {};
    if ((ctx->net != nullptr) && (ctx->net->status != nullptr) &&
        ctx->net->status(ctx->net->ctx, &ns) == KILN_OK) {
        static const char *const states[] = { "down", "connecting", "connected",
                                              "ap_fallback" };
        /* The table must cover the enum, checked at compile time rather than by
         * a runtime bound the compiler can prove is always true.  The runtime
         * guard stays for a value an adapter may have corrupted, and compares
         * the underlying integer so it remains a real test. */
        static_assert(std::size(states) == KILN_NET_AP_FALLBACK + 1,
                      "states[] must cover every kiln_net_state_t");
        const size_t state_idx = static_cast<size_t>(ns.state);
        kiln_json_kv_str(&j, "state",
            state_idx < std::size(states) ? states[state_idx] : "unknown");
        kiln_json_kv_str(&j, "ssid", ns.ssid);
        kiln_json_kv_str(&j, "ip", ns.ip);
        kiln_json_kv_str(&j, "hostname", ns.hostname);
        kiln_json_kv_int(&j, "rssi", ns.rssi);
        kiln_json_kv_uint(&j, "disconnects", ns.disconnect_count);
        kiln_json_kv_bool(&j, "time_synced", ns.time_synced);
    }
    else {
        kiln_json_kv_str(&j, "state", "unavailable");
    }
    kiln_json_obj_close(&j);
    return resp_end(resp, &j);
}

} // namespace

/* --- telemetry (FR-WEB-05) --------------------------------------------- */

size_t kiln_api_telemetry_event(kiln_api_ctx_t *ctx, char *buf, size_t cap)
{
    if ((ctx == nullptr) || (ctx->app == nullptr) || (buf == nullptr) || cap == 0) {
        return 0;
    }

    kiln_json_t j;
    kiln_json_init(&j, buf, cap);
    write_status(ctx, &j);
    return kiln_json_ok(&j) ? kiln_json_len(&j) : 0;
}

/* --- dispatch ---------------------------------------------------------- */

kiln_err_t kiln_api_handle(kiln_api_ctx_t *ctx, const kiln_api_req_t *req,
                           kiln_api_resp_t *resp)
{
    if ((ctx == nullptr) || (ctx->app == nullptr) || (req == nullptr) || (req->path == nullptr) ||
        (resp == nullptr) || (resp->body == nullptr)) {
        if (resp != nullptr) {
            kiln_api_error(resp, 500, "internal", "bad dispatch");
        }
        return KILN_ERR_INVALID_ARG;
    }

    /* 12.2: the declared maximum, enforced before anything parses.  A body that
     * did not fit was never fully read, so acting on its prefix would be acting
     * on a truncated request. */
    if (req->body_len > KILN_API_MAX_BODY) {
        kiln_api_error(resp, 413, "body_too_large",
                       "the request body exceeds the device's limit");
        return KILN_ERR_NO_SPACE;
    }

    /* FR-WEB-23.  Checked here so a new state-changing route is protected by
     * default rather than by the author remembering. */
    if (kiln_api_needs_auth(req) && !req->authenticated) {
        kiln_api_error(resp, 401, "unauthorized", "authentication is required");
        return KILN_ERR_STATE;
    }

    const char *p = req->path;
    uint32_t    id = 0;
    const char *tail = NULL;

    if (path_is(p, "/api/status")) {
        if (req->method != KILN_HTTP_GET) {
            goto method;
        }
        kiln_json_t j;
        resp_begin(resp, &j);
        write_status(ctx, &j);
        return resp_end(resp, &j);
    }
    if (path_is(p, "/api/info")) {
        if (req->method != KILN_HTTP_GET) {
            goto method;
        }
        kiln_json_t j;
        resp_begin(resp, &j);
        write_info(ctx, &j);
        return resp_end(resp, &j);
    }
    if (path_is(p, "/api/config")) {
        if (req->method == KILN_HTTP_GET) {
            kiln_json_t j;
            resp_begin(resp, &j);
            write_config(ctx, &j);
            return resp_end(resp, &j);
        }
        return reject_read_only(resp, "changing configuration");
    }
    if (path_is(p, "/api/config/defaults")) {
        return reject_read_only(resp, "resetting configuration to defaults");
    }
    /* FR-WEB-26: stored programs are readable and nothing more.  Authoring
     * went the way of the rest of the writes once it was clear the password
     * guarding it could not be set from anywhere. */
    if (path_is(p, "/api/programs")) {
        if (req->method != KILN_HTTP_GET) {
            return reject_read_only(resp, "creating, editing or deleting a program");
        }
        return handle_program_list(ctx, resp);
    }
    if (path_split_id(p, "/api/programs/", &id, &tail)) {
        if (req->method != KILN_HTTP_GET) {
            return reject_read_only(resp, "creating, editing or deleting a program");
        }
        return handle_program_one(ctx, id, resp);
    }
    /* FR-WEB-26.  Run *state* is readable at /api/status; run *control* is not
     * reachable from here at all. */
    if (path_is(p, "/api/run") || strncmp(p, "/api/run/", 9) == 0) {
        return reject_read_only(resp, "starting, pausing, resuming or aborting a firing");
    }
    if (path_is(p, "/api/manual")) {
        return reject_read_only(resp, "manual heating");
    }
    /* FR-WEB-26 and SR-17 pulling the same way: acknowledging a fault re-arms a
     * kiln that has already failed once, and the operator should be looking at
     * it when they do.  Reading the latched fault stays available at
     * /api/status, which is what the FR-WEB-24 banner needs. */
    if (path_is(p, "/api/fault/ack")) {
        return reject_read_only(resp, "acknowledging a fault");
    }
    if (path_is(p, "/api/tune")) {
        return handle_tune(ctx, req, "", resp);
    }
    if (strncmp(p, "/api/tune/", 10) == 0) {
        return handle_tune(ctx, req, p + 10, resp);
    }
    if (path_is(p, "/api/runs")) {
        if (req->method != KILN_HTTP_GET) {
            goto method;
        }
        return handle_runs(ctx, resp);
    }
    if (path_is(p, "/api/current")) {
        return handle_current(ctx, req, "", resp);
    }
    if (strncmp(p, "/api/current/", 13) == 0) {
        return handle_current(ctx, req, p + 13, resp);
    }
    if (path_is(p, "/api/storage")) {
        if (req->method != KILN_HTTP_GET) {
            goto method;
        }
        return handle_storage(ctx, resp);
    }
    if (path_is(p, "/api/net")) {
        if (req->method != KILN_HTTP_GET) {
            goto method;
        }
        return handle_net(ctx, resp);
    }
    if (path_is(p, "/api/log")) {
        if (req->method == KILN_HTTP_DELETE) {
            if (ctx->ring == nullptr) {
                kiln_api_error(resp, 503, "no_storage", "the log store is unavailable");
                return KILN_ERR_IO;
            }
            /* FR-LOG-13: irreversible, and the UI is required to say so before
             * it gets here. */
            const kiln_err_t e = kiln_logring_erase_all(ctx->ring);
            if (e != KILN_OK) { resp_from_err(resp, e, "the log could not be erased"); return e; }
            return ok_response(resp);
        }
        /* GET is a stream; the transport calls kiln_api_log_begin. */
        kiln_api_error(resp, 500, "internal",
                       "log queries must go through kiln_api_log_begin");
        return KILN_ERR_UNSUPPORTED;
    }

    kiln_api_error(resp, 404, "not_found", "no such endpoint");
    return KILN_ERR_NOT_FOUND;

method:
    kiln_api_error(resp, 405, "method_not_allowed", "that method is not allowed here");
    return KILN_ERR_UNSUPPORTED;
}

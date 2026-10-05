/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The REST API of architecture 12.1 against the real application and a simulated
 * kiln -- FR-WEB-13, FR-WEB-19, FR-WEB-20, FR-WEB-23, FR-LOG-10, FR-WEB-18,
 * and the request-handling rules of 12.2.
 *
 * This is the "API suite green" half of M6's exit criterion. It runs on the host
 * because the API layer has no transport in it: a handler takes a method, a path
 * and a body, which is exactly what a test can hand it.
 */

#include <stdio.h>
#include <string.h>
#include "kiln_check.h"
#include "kiln_app/program_store.h"
#include "kiln_app/run_index.h"
#include "kiln_core/logring.h"
#include "kiln_core/profile.h"
#include "kiln_hal_host/hal_host.h"
#include "kiln_sim/sim.h"
#include "kiln_web/api.h"
#include "kiln_web/json.h"

#define LOG_SECTORS  24u
#define SECTOR_BYTES 4096u
#define BODY_CAP     16384u

typedef struct {
    uint8_t               flash_storage[LOG_SECTORS * SECTOR_BYTES];
    kiln_host_flash_t     flash;
    kiln_port_flash_t     flash_port;
    kiln_host_kv_t        kv;
    kiln_port_kvstore_t   kv_port;
    kiln_host_fs_t        fs;
    kiln_port_filestore_t fs_port;
    kiln_host_clock_t     clk;
    kiln_port_clock_t     clk_port;
    kiln_logring_t        ring;
    kiln_port_logstore_t  log_port;

    kiln_sim_t            sim;
    kiln_sim_ports_t      sim_ports;
    kiln_app_t            app;
    kiln_api_ctx_t        api;

    char                  body[BODY_CAP];
    double                t_s, na, ns, nc;
} rig_t;

static void rig_init(rig_t *r)
{
    memset(r, 0, sizeof(*r));

    kiln_host_flash_init(&r->flash, r->flash_storage, sizeof(r->flash_storage),
                         SECTOR_BYTES);
    kiln_host_flash_bind(&r->flash, &r->flash_port);
    kiln_host_kv_init(&r->kv);
    kiln_host_kv_bind(&r->kv, &r->kv_port);
    kiln_host_fs_init(&r->fs);
    kiln_host_fs_bind(&r->fs, &r->fs_port);
    kiln_host_clock_init(&r->clk, 1767225600ull, true);
    kiln_host_clock_bind(&r->clk, &r->clk_port);

    (void)kiln_logring_mount(&r->ring, &r->flash_port);
    kiln_logring_bind(&r->ring, &r->log_port);

    kiln_sim_cfg_t sc;
    kiln_sim_cfg_defaults(&sc);
    sc.tau_s = 300.0f; sc.dead_time_s = 10.0f; sc.noise_c = 0.1f;
    kiln_sim_init(&r->sim, &sc);
    kiln_sim_bind(&r->sim, &r->sim_ports);

    kiln_app_ports_t ports = {};
    ports.tc        = &r->sim_ports.tc;
    ports.case_tc   = &r->sim_ports.case_tc;
    ports.heat      = &r->sim_ports.heat;
    ports.current   = &r->sim_ports.current;
    ports.counters  = &r->sim_ports.counters;
    ports.logstore  = &r->log_port;
    ports.kvstore   = &r->kv_port;
    ports.filestore = &r->fs_port;
    ports.clock     = &r->clk_port;

    kiln_config_t cfg;
    kiln_config_defaults(&cfg);
    cfg.kp = 6.0f; cfg.ki = 0.02f; cfg.kd = 30.0f;
    cfg.filter_tau_s = 1.0f; cfg.holdback_band_c = 0.0f;
    cfg.log_interval_s = 1;
    CHECK_OK(kiln_app_init(&r->app, &ports, &cfg));
    CHECK_OK(kiln_app_boot(&r->app, KILN_RESET_POWER_ON, -1.0f));

    r->api.app       = &r->app;
    r->api.filestore = &r->fs_port;
    r->api.logstore  = &r->log_port;
    r->api.ring      = &r->ring;
}

static void rig_step(rig_t *r)
{
    kiln_sim_step(&r->sim, 0.01f);
    r->t_s += 0.01;
    kiln_host_clock_advance(&r->clk, 10000u);
    kiln_app_window_tick(&r->app, 10);
    if (r->t_s >= r->na) { kiln_app_acquire_cycle(&r->app, 0.25f); r->na += 0.25; }
    if (r->t_s >= r->ns) { kiln_app_safety_cycle(&r->app, 0.1f);   r->ns += 0.1;  }
    if (r->t_s >= r->nc) { kiln_app_control_cycle(&r->app, 1.0f);  r->nc += 1.0;  }
    (void)kiln_app_log_drain(&r->app, 8);
}

static void rig_run(rig_t *r, double seconds)
{
    const int n = (int)(seconds / 0.01);
    for (int i = 0; i < n; i++) {
        rig_step(r);
    }
}

/* One request.  Always authenticated unless a test says otherwise, because
 * FR-WEB-23 is tested on its own and every other test is about the handler. */
static kiln_api_resp_t call(rig_t *r, kiln_http_method_t m, const char *path,
                            const char *query, const char *body)
{
    kiln_api_req_t req = {};
    req.method        = m;
    req.path          = path;
    req.query         = query;
    req.body          = body;
    req.body_len = (body != nullptr) ? strlen(body) : 0;
    req.authenticated = true;

    kiln_api_resp_t resp = {};
    resp.body     = r->body;
    resp.body_cap = sizeof(r->body);
    (void)kiln_api_handle(&r->api, &req, &resp);
    return resp;
}

/* Parse the response and return the token array, so assertions read as JSON
 * rather than as substring searches.
 *
 * Generous: /api/config projects every one of the ~70 configuration items with a
 * dozen members each, and an 800-point log query is ten thousand numbers.  Both
 * are legitimate responses, so the test's budget has to exceed them or the test
 * is measuring its own array size. */
#define RTOKS 40000
static kiln_json_tok_t g_toks[RTOKS];

static int parse_resp(const kiln_api_resp_t *resp)
{
    const int n = kiln_json_parse(resp->body, resp->body_len, g_toks, RTOKS);
    CHECK_MSG(n > 0, "the response was not parseable JSON: %.200s", resp->body);
    return n;
}

static double num_of(const kiln_api_resp_t *resp, int n, const char *key)
{
    double d = 0.0;
    CHECK_MSG(kiln_json_get_num(resp->body, g_toks, n, 0, key, &d),
              "no numeric member %s in %.200s", key, resp->body);
    return d;
}

static void str_of(const kiln_api_resp_t *resp, int n, const char *key,
                   char *out, size_t cap)
{
    CHECK_MSG(kiln_json_get_str(resp->body, g_toks, n, 0, key, out, cap),
              "no string member %s in %.200s", key, resp->body);
}

/* FR-WEB-20: every error is {"error":{"code","message"}}. */
/* By value, so `expect_error(call(...))` reads naturally. */
static void expect_error(kiln_api_resp_t r, int status, const char *code)
{
    CHECK_EQ_INT(r.status, status);
    CHECK_STR_EQ(r.content_type, "application/json");

    const int n = kiln_json_parse(r.body, r.body_len, g_toks, RTOKS);
    CHECK_MSG(n > 0, "error body not parseable: %.200s", r.body);

    const int err = kiln_json_find(r.body, g_toks, n, 0, "error");
    CHECK_MSG(err > 0, "no error object in %.200s", r.body);

    char got[64] = "";
    CHECK(kiln_json_get_str(r.body, g_toks, n, err, "code", got, sizeof(got)));
    CHECK_MSG(strcmp(got, code) == 0, "code was \"%s\", wanted \"%s\"", got, code);

    char msg[200] = "";
    CHECK(kiln_json_get_str(r.body, g_toks, n, err, "message", msg, sizeof(msg)));
    /* A message that says nothing is worse than no message, because it looks
     * like the API tried to explain and failed. */
    CHECK_MSG(strlen(msg) > 10, "the message was too terse: \"%s\"", msg);
}

/* --- status, info (FR-RUN-05, FR-UPD-06) ------------------------------- */

KILN_TEST(frrun05_status_exposes_everything_the_requirement_lists)
{
    static rig_t r;
    rig_init(&r);
    rig_run(&r, 2.0);

    const kiln_api_resp_t resp = call(&r, KILN_HTTP_GET, "/api/status", NULL, NULL);
    CHECK_EQ_INT(resp.status, 200);
    const int n = parse_resp(&resp);

    char state[16];
    str_of(&resp, n, "state", state, sizeof(state));
    CHECK_STR_EQ(state, "IDLE");

    /* FR-RUN-05's list. */
    (void)num_of(&resp, n, "kiln_c");
    (void)num_of(&resp, n, "setpoint_c");
    (void)num_of(&resp, n, "duty_permille");
    (void)num_of(&resp, n, "rate_c_per_h");
    (void)num_of(&resp, n, "segment");
    (void)num_of(&resp, n, "segment_count");
    (void)num_of(&resp, n, "elapsed_s");
    (void)num_of(&resp, n, "remaining_s");
    (void)num_of(&resp, n, "segment_remaining_s");
    CHECK(kiln_json_find(resp.body, g_toks, n, 0, "holdback") > 0);
    CHECK(kiln_json_find(resp.body, g_toks, n, 0, "warnings") > 0);
    CHECK(kiln_json_find(resp.body, g_toks, n, 0, "fault") > 0);
}

KILN_TEST(frweb25_validity_travels_with_the_value_so_stale_is_distinguishable)
{
    static rig_t r;
    rig_init(&r);
    kiln_sim_inject(&r.sim, KILN_INJ_TC_COMMS);
    rig_run(&r, 2.0);

    const kiln_api_resp_t resp = call(&r, KILN_HTTP_GET, "/api/status", NULL, NULL);
    const int n = parse_resp(&resp);

    /* A reading the front end has disowned is null, not the last good number
     * dressed up as current. */
    const int ki = kiln_json_find(resp.body, g_toks, n, 0, "kiln_c");
    CHECK(ki > 0);
    double d = 0.0;
    CHECK(!kiln_json_num_at(resp.body, g_toks, ki, &d));

    bool valid = true;
    CHECK(kiln_json_get_bool(resp.body, g_toks, n, 0, "kiln_valid", &valid));
    CHECK(!valid);
}

KILN_TEST(frupd06_info_reports_identity_and_gain_provenance)
{
    static rig_t r;
    rig_init(&r);

    const kiln_api_resp_t resp = call(&r, KILN_HTTP_GET, "/api/info", NULL, NULL);
    CHECK_EQ_INT(resp.status, 200);
    const int n = parse_resp(&resp);

    CHECK(kiln_json_find(resp.body, g_toks, n, 0, "version") > 0);
    CHECK(kiln_json_find(resp.body, g_toks, n, 0, "target") > 0);

    /* FR-TUN-11: factory defaults are not gains for *this* kiln, and the UI has
     * to be able to say so. */
    const int g = kiln_json_find(resp.body, g_toks, n, 0, "gains");
    CHECK(g > 0);
    double kp = 0.0;
    CHECK(kiln_json_get_num(resp.body, g_toks, n, g, "kp", &kp));
    CHECK_NEAR(kp, 6.0, 0.001);
    bool tuned = true;
    CHECK(kiln_json_get_bool(resp.body, g_toks, n, g, "tuned", &tuned));
}

/* --- configuration (FR-CFG, FR-WEB-17) --------------------------------- */

KILN_TEST(frweb17_config_is_generated_from_the_schema_with_units_and_ranges)
{
    static rig_t r;
    rig_init(&r);

    const kiln_api_resp_t resp = call(&r, KILN_HTTP_GET, "/api/config", NULL, NULL);
    CHECK_EQ_INT(resp.status, 200);
    const int n = parse_resp(&resp);

    const int items = kiln_json_find(resp.body, g_toks, n, 0, "items");
    CHECK(items > 0);
    /* Every item in the firmware's table, so one cannot exist and be missing
     * from the UI. */
    CHECK_EQ_INT(g_toks[items].size, (int)kiln_config_item_count());

    /* The first item carries the shape the form generator needs. */
    const int first = items + 1;
    char key[48];
    CHECK(kiln_json_get_str(resp.body, g_toks, n, first, "key", key, sizeof(key)));
    CHECK(kiln_json_find(resp.body, g_toks, n, first, "unit") > 0);
    CHECK(kiln_json_find(resp.body, g_toks, n, first, "min") > 0);
    CHECK(kiln_json_find(resp.body, g_toks, n, first, "max") > 0);
    CHECK(kiln_json_find(resp.body, g_toks, n, first, "default") > 0);
    CHECK(kiln_json_find(resp.body, g_toks, n, first, "reboot_required") > 0);
    CHECK(kiln_json_find(resp.body, g_toks, n, first, "locked_while_running") > 0);
}

KILN_TEST(frcfg07_a_secret_is_never_returned_only_whether_it_is_set)
{
    static rig_t r;
    rig_init(&r);

    CHECK_EQ_INT(call(&r, KILN_HTTP_PUT, "/api/config", NULL,
                      "{\"security.web_password\":\"hunter2\"}").status, 200);

    const kiln_api_resp_t resp = call(&r, KILN_HTTP_GET, "/api/config", NULL, NULL);
    /* The one assertion that matters: the password is not in the response at
     * all, in any field, however the item is rendered. */
    CHECK_MSG(strstr(resp.body, "hunter2") == NULL,
              "the stored password was serialised outward");

    const int n = parse_resp(&resp);
    const int items = kiln_json_find(resp.body, g_toks, n, 0, "items");
    bool found = false;
    int i = items + 1;
    for (int k = 0; k < g_toks[items].size && i < n; k++) {
        char key[48] = "";
        if (kiln_json_get_str(resp.body, g_toks, n, i, "key", key, sizeof(key)) &&
            strcmp(key, "security.web_password") == 0) {
            bool set = false;
            CHECK(kiln_json_get_bool(resp.body, g_toks, n, i, "set", &set));
            CHECK(set);
            CHECK(kiln_json_find(resp.body, g_toks, n, i, "value") < 0);
            found = true;
        }
        const int end = g_toks[i].end;
        i++;
        while (i < n && g_toks[i].start < end) {
            i++;
        }
    }
    CHECK(found);
}

KILN_TEST(frcfg03_a_write_is_atomic_and_names_the_offending_item)
{
    static rig_t r;
    rig_init(&r);
    const float before_holdback = r.app.cfg.holdback_band_c;

    /* One good change and one impossible one in the same body. */
    const kiln_api_resp_t resp = call(&r, KILN_HTTP_PUT, "/api/config", NULL,
        "{\"control.holdback_band_c\":40,\"safety.max_temp_c\":9000}");
    expect_error(resp, 400, "out_of_range");
    CHECK_MSG(strstr(resp.body, "safety.max_temp_c") != NULL,
              "the message did not name the item: %.200s", resp.body);

    /* Neither change landed, which is what atomic means. */
    CHECK_NEAR(r.app.cfg.holdback_band_c, before_holdback, 0.001f);
    CHECK_NEAR(r.app.cfg.max_temp_c, 1280.0f, 0.01f);
}

KILN_TEST(frcfg08_a_safety_item_is_refused_while_running_and_says_which)
{
    static rig_t r;
    rig_init(&r);
    rig_run(&r, 2.0);
    CHECK_EQ_INT(call(&r, KILN_HTTP_POST, "/api/run", NULL,
                      "{\"program_id\":1}").status, 200);

    const kiln_api_resp_t resp = call(&r, KILN_HTTP_PUT, "/api/config", NULL,
                                      "{\"safety.max_temp_c\":1100}");
    expect_error(resp, 409, "locked_while_running");
    CHECK(strstr(resp.body, "safety.max_temp_c") != NULL);

    /* Something not safety-relevant still applies, or nothing would be editable
     * during the many hours a firing takes. */
    CHECK_EQ_INT(call(&r, KILN_HTTP_PUT, "/api/config", NULL,
                      "{\"hmi.dim_timeout_s\":120}").status, 200);
}

KILN_TEST(an_unknown_configuration_key_is_rejected_by_name)
{
    static rig_t r;
    rig_init(&r);
    const kiln_api_resp_t resp = call(&r, KILN_HTTP_PUT, "/api/config", NULL,
                                      "{\"safety.make_it_hotter\":1}");
    expect_error(resp, 400, "unknown_item");
    CHECK(strstr(resp.body, "safety.make_it_hotter") != NULL);
}

KILN_TEST(frcfg05_configuration_written_through_the_api_is_persisted)
{
    static rig_t r;
    rig_init(&r);
    const uint32_t sets = r.kv.sets;
    CHECK_EQ_INT(call(&r, KILN_HTTP_PUT, "/api/config", NULL,
                      "{\"log.interval_s\":42}").status, 200);
    CHECK(r.kv.sets > sets);

    kiln_config_t stored;
    CHECK_OK(kiln_settings_load(&r.kv_port, &stored));
    CHECK_EQ_UINT(stored.log_interval_s, 42u);
}

/* --- programs (FR-PRG-07, FR-WEB-12, FR-WEB-13) ------------------------ */

KILN_TEST(frprg09_the_examples_are_listed_and_marked_readonly)
{
    static rig_t r;
    rig_init(&r);

    const kiln_api_resp_t resp = call(&r, KILN_HTTP_GET, "/api/programs", NULL, NULL);
    CHECK_EQ_INT(resp.status, 200);
    const int n = parse_resp(&resp);

    const int arr = kiln_json_find(resp.body, g_toks, n, 0, "programs");
    CHECK(arr > 0);
    CHECK_EQ_INT(g_toks[arr].size, (int)kiln_profile_example_count());

    const int first = arr + 1;
    bool ro = false;
    CHECK(kiln_json_get_bool(resp.body, g_toks, n, first, "readonly", &ro));
    CHECK(ro);
    CHECK(kiln_json_find(resp.body, g_toks, n, first, "segments") > 0);
    CHECK(kiln_json_find(resp.body, g_toks, n, first, "duration_s") > 0);
    CHECK(kiln_json_find(resp.body, g_toks, n, first, "peak_c") > 0);
}

KILN_TEST(frweb13_the_server_revalidates_and_never_trusts_the_client)
{
    static rig_t r;
    rig_init(&r);

    /* A target above the configured maximum.  A client that skipped its own
     * validation, or lied, must not be able to store this. */
    const kiln_api_resp_t resp = call(&r, KILN_HTTP_POST, "/api/programs", NULL,
        "{\"name\":\"too hot\",\"segments\":[{\"target_c\":1340,\"rate_c_per_h\":100}]}");
    expect_error(resp, 400, "invalid_program");

    const kiln_api_resp_t list = call(&r, KILN_HTTP_GET, "/api/programs", NULL, NULL);
    CHECK(strstr(list.body, "too hot") == NULL);
}

KILN_TEST(frprg07_a_program_round_trips_through_the_api)
{
    static rig_t r;
    rig_init(&r);

    const kiln_api_resp_t created = call(&r, KILN_HTTP_POST, "/api/programs", NULL,
        "{\"name\":\"my glaze\",\"description\":\"cone 6\",\"segments\":["
        "{\"target_c\":150,\"rate_c_per_h\":100},"
        "{\"target_c\":1100,\"rate_c_per_h\":150},"
        "{\"target_c\":1222,\"rate_c_per_h\":60,\"dwell_min\":15,\"require_ack\":true}]}");
    CHECK_EQ_INT(created.status, 201);

    /* Find its id in the list, then read it back. */
    const kiln_api_resp_t list = call(&r, KILN_HTTP_GET, "/api/programs", NULL, NULL);
    const int n = parse_resp(&list);
    const int arr = kiln_json_find(list.body, g_toks, n, 0, "programs");
    int id = -1;
    int i = arr + 1;
    for (int k = 0; k < g_toks[arr].size && i < n; k++) {
        char name[40] = "";
        double d = 0.0;
        if (kiln_json_get_str(list.body, g_toks, n, i, "name", name, sizeof(name)) &&
            strcmp(name, "my glaze") == 0 &&
            kiln_json_get_num(list.body, g_toks, n, i, "id", &d)) {
            id = (int)d;
        }
        const int end = g_toks[i].end;
        i++;
        while (i < n && g_toks[i].start < end) {
            i++;
        }
    }
    CHECK(id >= 0);

    char path[48];
    snprintf(path, sizeof(path), "/api/programs/%d", id);
    const kiln_api_resp_t got = call(&r, KILN_HTTP_GET, path, NULL, NULL);
    CHECK_EQ_INT(got.status, 200);
    const int gn = parse_resp(&got);

    char desc[64];
    str_of(&got, gn, "description", desc, sizeof(desc));
    CHECK_STR_EQ(desc, "cone 6");
    CHECK_NEAR(num_of(&got, gn, "segment_count"), 3.0, 0.001);
    CHECK_NEAR(num_of(&got, gn, "peak_c"), 1222.0, 0.001);

    bool valid = false;
    CHECK(kiln_json_get_bool(got.body, g_toks, gn, 0, "valid", &valid));
    CHECK(valid);

    /* FR-PRG-06: the planned curve the chart draws as the intended firing. */
    snprintf(path, sizeof(path), "/api/programs/%d/preview", id);
    const kiln_api_resp_t prev = call(&r, KILN_HTTP_GET, path, NULL, NULL);
    CHECK_EQ_INT(prev.status, 200);
    const int pn = parse_resp(&prev);
    const int curve = kiln_json_find(prev.body, g_toks, pn, 0, "curve");
    CHECK(curve > 0);
    CHECK(g_toks[curve].size >= 4);

    /* Delete. */
    snprintf(path, sizeof(path), "/api/programs/%d", id);
    CHECK_EQ_INT(call(&r, KILN_HTTP_DELETE, path, NULL, NULL).status, 200);
    CHECK_EQ_INT(call(&r, KILN_HTTP_GET, path, NULL, NULL).status, 404);
}

KILN_TEST(frprg09_an_example_cannot_be_edited_but_can_be_copied)
{
    static rig_t r;
    rig_init(&r);

    const kiln_api_resp_t put = call(&r, KILN_HTTP_PUT, "/api/programs/0", NULL,
        "{\"name\":\"hijacked\",\"segments\":[{\"target_c\":500,\"rate_c_per_h\":100}]}");
    expect_error(put, 409, "readonly");
    CHECK_EQ_INT(call(&r, KILN_HTTP_DELETE, "/api/programs/0", NULL, NULL).status, 409);

    /* A copy is editable, which is how FR-PRG-09 stays a protection rather than
     * an obstruction. */
    const kiln_api_resp_t copy = call(&r, KILN_HTTP_POST, "/api/programs/0/copy",
                                      NULL, NULL);
    CHECK_EQ_INT(copy.status, 201);
    const int n = parse_resp(&copy);
    char name[48];
    str_of(&copy, n, "name", name, sizeof(name));
    CHECK(strstr(name, "copy") != NULL);
}

KILN_TEST(a_malformed_program_body_is_rejected_with_a_useful_message)
{
    static rig_t r;
    rig_init(&r);

    expect_error(call(&r, KILN_HTTP_POST, "/api/programs", NULL, "not json"),
                 400, "bad_json");
    expect_error(call(&r, KILN_HTTP_POST, "/api/programs", NULL, "{}"),
                 400, "invalid_value");
    expect_error(call(&r, KILN_HTTP_POST, "/api/programs", NULL,
                       "{\"name\":\"x\"}"), 400, "invalid_value");
    expect_error(call(&r, KILN_HTTP_POST, "/api/programs", NULL,
                       "{\"name\":\"x\",\"segments\":[{\"rate_c_per_h\":1}]}"),
                 400, "invalid_value");
    expect_error(call(&r, KILN_HTTP_POST, "/api/programs", NULL,
                       "{\"name\":\"x\",\"segments\":[{\"target_c\":1e9}]}"),
                 400, "out_of_range");
}

/* --- run control (FR-RUN) ---------------------------------------------- */

KILN_TEST(frrun_the_whole_run_lifecycle_goes_through_the_api)
{
    static rig_t r;
    rig_init(&r);
    rig_run(&r, 2.0);

    CHECK_EQ_INT(call(&r, KILN_HTTP_POST, "/api/run", NULL,
                      "{\"program_id\":1}").status, 200);
    CHECK_EQ_INT(r.app.state, KILN_STATE_RUNNING);
    rig_run(&r, 5.0);

    CHECK_EQ_INT(call(&r, KILN_HTTP_POST, "/api/run/pause", NULL, NULL).status, 200);
    CHECK_EQ_INT(r.app.state, KILN_STATE_PAUSED);
    CHECK_EQ_INT(call(&r, KILN_HTTP_POST, "/api/run/resume", NULL, NULL).status, 200);
    CHECK_EQ_INT(r.app.state, KILN_STATE_RUNNING);
    CHECK_EQ_INT(call(&r, KILN_HTTP_POST, "/api/run/abort", NULL, NULL).status, 200);
    CHECK_EQ_INT(r.app.state, KILN_STATE_IDLE);

    /* And the refusals carry a reason the operator can act on. */
    expect_error(call(&r, KILN_HTTP_POST, "/api/run/pause", NULL, NULL),
                 409, "not_allowed_now");
    expect_error(call(&r, KILN_HTTP_POST, "/api/run", NULL, "{\"program_id\":99}"),
                 404, "not_found");
    expect_error(call(&r, KILN_HTTP_POST, "/api/run", NULL, "{}"),
                 400, "invalid_request");
}

KILN_TEST(frrun10_a_latched_fault_blocks_a_start_and_the_message_says_why)
{
    static rig_t r;
    rig_init(&r);
    rig_run(&r, 2.0);
    kiln_sim_inject(&r.sim, KILN_INJ_TC_OPEN);
    for (int i = 0; i < 20000 && r.app.fault == KILN_FAULT_NONE; i++) {
        rig_step(&r);
    }
    CHECK(r.app.fault != KILN_FAULT_NONE);

    const kiln_api_resp_t resp = call(&r, KILN_HTTP_POST, "/api/run", NULL,
                                      "{\"program_id\":1}");
    expect_error(resp, 409, "not_allowed_now");
    CHECK_MSG(strstr(resp.body, "fault") != NULL,
              "the message did not mention the fault: %.200s", resp.body);

    /* SR-18: the acknowledgement is refused while the condition holds, and says
     * what the condition is. */
    const kiln_api_resp_t ack = call(&r, KILN_HTTP_POST, "/api/fault/ack", NULL, NULL);
    expect_error(ack, 409, "still_faulted");

    kiln_sim_clear(&r.sim, KILN_INJ_TC_OPEN);
    rig_run(&r, 1.0);
    CHECK_EQ_INT(call(&r, KILN_HTTP_POST, "/api/fault/ack", NULL, NULL).status, 200);
    CHECK_EQ_INT(r.app.fault, KILN_FAULT_NONE);
}

KILN_TEST(frweb24_the_banner_data_carries_the_operator_text)
{
    static rig_t r;
    rig_init(&r);
    rig_run(&r, 2.0);
    kiln_sim_inject(&r.sim, KILN_INJ_TC_OPEN);
    for (int i = 0; i < 20000 && r.app.fault == KILN_FAULT_NONE; i++) {
        rig_step(&r);
    }

    const kiln_api_resp_t resp = call(&r, KILN_HTTP_GET, "/api/status", NULL, NULL);
    const int n = parse_resp(&resp);
    const int f = kiln_json_find(resp.body, g_toks, n, 0, "fault");
    CHECK(f > 0);
    CHECK_EQ_INT(g_toks[f].type, KILN_JSON_OBJECT);

    char label[32] = "", msg[200] = "";
    CHECK(kiln_json_get_str(resp.body, g_toks, n, f, "label", label, sizeof(label)));
    CHECK(kiln_json_get_str(resp.body, g_toks, n, f, "message", msg, sizeof(msg)));
    CHECK_STR_EQ(label, "TC OPEN");
    /* The banner shows what to do about it, not just that something happened. */
    CHECK(strlen(msg) > 30);
}

KILN_TEST(frprg10_only_unstarted_segments_can_be_patched)
{
    static rig_t r;
    rig_init(&r);
    rig_run(&r, 2.0);
    CHECK_EQ_INT(call(&r, KILN_HTTP_POST, "/api/run", NULL,
                      "{\"program_id\":1}").status, 200);
    rig_run(&r, 5.0);

    /* Changing the running segment is refused. */
    const kiln_api_resp_t bad = call(&r, KILN_HTTP_PATCH, "/api/run/segments", NULL,
        "{\"name\":\"Glaze cone 6\",\"segments\":[{\"target_c\":999,\"rate_c_per_h\":100}]}");
    expect_error(bad, 409, "not_allowed_now");
}

KILN_TEST(frctl14_manual_mode_is_bounded)
{
    static rig_t r;
    rig_init(&r);
    rig_run(&r, 2.0);

    CHECK_EQ_INT(call(&r, KILN_HTTP_POST, "/api/manual", NULL,
                      "{\"duty_permille\":400}").status, 200);
    CHECK_EQ_INT(r.app.state, KILN_STATE_MANUAL);
    CHECK_EQ_UINT(r.app.duty_manual, 400u);

    expect_error(call(&r, KILN_HTTP_POST, "/api/manual", NULL,
                       "{\"duty_permille\":5000}"), 400, "out_of_range");
    expect_error(call(&r, KILN_HTTP_POST, "/api/manual", NULL, "{}"),
                 400, "invalid_request");
}

/* --- tuning (FR-TUN-09, FR-TUN-10) ------------------------------------- */

KILN_TEST(frtun10_tuning_progress_and_candidates_are_exposed)
{
    static rig_t r;
    rig_init(&r);
    rig_run(&r, 2.0);

    const kiln_api_resp_t started = call(&r, KILN_HTTP_POST, "/api/tune", NULL,
                                         "{\"setpoint_c\":600}");
    CHECK_EQ_INT(started.status, 200);
    CHECK_EQ_INT(r.app.state, KILN_STATE_AUTOTUNE);

    const kiln_api_resp_t resp = call(&r, KILN_HTTP_GET, "/api/tune", NULL, NULL);
    CHECK_EQ_INT(resp.status, 200);
    const int n = parse_resp(&resp);

    char phase[24];
    str_of(&resp, n, "phase", phase, sizeof(phase));
    CHECK_STR_EQ(phase, "approach");
    CHECK(kiln_json_find(resp.body, g_toks, n, 0, "candidates") > 0);

    /* FR-TUN-09: nothing is stored until a rule is accepted, so accepting with
     * no result is refused rather than writing zeros over the gains. */
    expect_error(call(&r, KILN_HTTP_POST, "/api/tune/accept", NULL,
                       "{\"rule\":\"tyreus-luyben\"}"), 409, "no_result");

    CHECK_EQ_INT(call(&r, KILN_HTTP_POST, "/api/tune/cancel", NULL, NULL).status, 200);
}

KILN_TEST(sr23_a_tuning_setpoint_above_the_maximum_is_clamped_and_says_so)
{
    static rig_t r;
    rig_init(&r);
    rig_run(&r, 2.0);

    const kiln_api_resp_t resp = call(&r, KILN_HTTP_POST, "/api/tune", NULL,
                                      "{\"setpoint_c\":5000}");
    CHECK_EQ_INT(resp.status, 200);
    const int n = parse_resp(&resp);

    /* Running, but not where it was asked to: silently tuning somewhere else
     * would be worse than refusing. */
    bool clamped = false;
    CHECK(kiln_json_get_bool(resp.body, g_toks, n, 0, "clamped", &clamped));
    CHECK(clamped);
    CHECK_NEAR(num_of(&resp, n, "setpoint_c"), 1280.0, 0.5);
}

/* --- current, storage, runs -------------------------------------------- */

KILN_TEST(frcur_the_current_endpoint_reports_measurement_and_wear)
{
    static rig_t r;
    rig_init(&r);
    rig_run(&r, 2.0);
    CHECK_EQ_INT(call(&r, KILN_HTTP_POST, "/api/run", NULL,
                      "{\"program_id\":1}").status, 200);
    rig_run(&r, 20.0);

    const kiln_api_resp_t resp = call(&r, KILN_HTTP_GET, "/api/current", NULL, NULL);
    CHECK_EQ_INT(resp.status, 200);
    const int n = parse_resp(&resp);

    (void)num_of(&resp, n, "current_a");
    (void)num_of(&resp, n, "reference_a");
    (void)num_of(&resp, n, "apparent_va");
    (void)num_of(&resp, n, "energy_wh");

    /* FR-CUR-07 requires the resistive-load assumption to be stated wherever the
     * figure is presented, so it travels with it. */
    char basis[80];
    str_of(&resp, n, "power_basis", basis, sizeof(basis));
    CHECK(strstr(basis, "resistive") != NULL);

    /* FR-CUR-13 */
    const int sw = kiln_json_find(resp.body, g_toks, n, 0, "switching");
    CHECK(sw > 0);
    CHECK(kiln_json_find(resp.body, g_toks, n, sw, "contactor_ops") > 0);
    CHECK(kiln_json_find(resp.body, g_toks, n, sw, "ssr_ops") > 0);

    /* FR-CUR-06: calibration needs a real conduction measurement, and refusing
     * says what is missing. */
    const kiln_api_resp_t cal = call(&r, KILN_HTTP_POST, "/api/current/calibrate",
                                     NULL, "{\"known_a\":0}");
    CHECK_EQ_INT(cal.status, 400);
}

KILN_TEST(frlog15_storage_health_is_reported)
{
    static rig_t r;
    rig_init(&r);
    rig_run(&r, 2.0);
    CHECK_EQ_INT(call(&r, KILN_HTTP_POST, "/api/run", NULL,
                      "{\"program_id\":1}").status, 200);
    rig_run(&r, 20.0);

    const kiln_api_resp_t resp = call(&r, KILN_HTTP_GET, "/api/storage", NULL, NULL);
    CHECK_EQ_INT(resp.status, 200);
    const int n = parse_resp(&resp);

    const int log = kiln_json_find(resp.body, g_toks, n, 0, "log");
    CHECK(log > 0);
    double stored = 0.0;
    CHECK(kiln_json_get_num(resp.body, g_toks, n, log, "records_stored", &stored));
    CHECK(stored > 0.0);
    CHECK(kiln_json_find(resp.body, g_toks, n, log, "records_total") > 0);
    CHECK(kiln_json_find(resp.body, g_toks, n, log, "erase_count") > 0);
    CHECK(kiln_json_find(resp.body, g_toks, n, log, "write_errors") > 0);
}

KILN_TEST(frlog09_the_run_list_is_newest_first_and_marks_truncation)
{
    static rig_t r;
    rig_init(&r);
    for (uint32_t id = 1; id <= 3; id++) {
        kiln_run_record_t rec;
        kiln_runstate_record_init(&rec, id);
        rec.end_reason = KILN_END_COMPLETE;
        rec.peak_c     = 900.0f + (float)id;
        kiln_profile_init_empty(&rec.program, "fired");
        if (id == 1) {
            rec.flags |= KILN_RUN_FLAG_TRUNCATED;
        }
        CHECK_OK(kiln_run_index_append(&r.fs_port, &rec));
    }

    const kiln_api_resp_t resp = call(&r, KILN_HTTP_GET, "/api/runs", NULL, NULL);
    CHECK_EQ_INT(resp.status, 200);
    const int n = parse_resp(&resp);

    const int arr = kiln_json_find(resp.body, g_toks, n, 0, "runs");
    CHECK(arr > 0);
    CHECK_EQ_INT(g_toks[arr].size, 3);

    /* Newest first, which is the order the history view wants. */
    double id0 = 0.0;
    CHECK(kiln_json_get_num(resp.body, g_toks, n, arr + 1, "run_id", &id0));
    CHECK_NEAR(id0, 3.0, 0.001);

    CHECK(strstr(resp.body, "samples_truncated") != NULL);
}

/* --- the log stream (FR-LOG-10, FR-LOG-11, FR-WEB-11, FR-WEB-18) ------- */

typedef struct { char buf[262144]; size_t len; bool fail_after; size_t fail_at; } sink_t;

static bool sink_write(void *user, const char *data, size_t len)
{
    sink_t *s = (sink_t *)user;
    if (s->fail_after && s->len >= s->fail_at) {
        return false;
    }
    if (s->len + len >= sizeof(s->buf)) {
        return false;
    }
    memcpy(&s->buf[s->len], data, len);
    s->len += len;
    s->buf[s->len] = '\0';
    return true;
}

static sink_t g_sink;

static kiln_api_resp_t stream_log(rig_t *r, const char *query)
{
    memset(&g_sink, 0, sizeof(g_sink));

    kiln_api_req_t req = {};
    req.method        = KILN_HTTP_GET;
    req.path          = "/api/log";
    req.query         = query;
    req.authenticated = true;

    kiln_api_resp_t resp = {};
    resp.body     = r->body;
    resp.body_cap = sizeof(r->body);
    (void)kiln_api_log_stream(&r->api, &req, sink_write, &g_sink, &resp);
    return resp;
}

/* A firing with enough samples to be worth decimating. */
static uint32_t seed_a_run(rig_t *r)
{
    rig_run(r, 2.0);
    CHECK_EQ_INT(call(r, KILN_HTTP_POST, "/api/run", NULL, "{\"program_id\":1}").status, 200);
    const uint32_t run_id = r->app.record.run_id;
    rig_run(r, 400.0);
    return run_id;
}

KILN_TEST(frlog10_a_log_query_decimates_to_the_requested_point_count)
{
    static rig_t r;
    rig_init(&r);
    const uint32_t run = seed_a_run(&r);

    char q[64];
    snprintf(q, sizeof(q), "run=%u&max_points=50", run);
    const kiln_api_resp_t resp = stream_log(&r, q);

    CHECK_EQ_INT(resp.status, 200);
    CHECK(resp.streaming);
    CHECK_STR_EQ(resp.content_type, "application/json");

    const int n = kiln_json_parse(g_sink.buf, g_sink.len, g_toks, RTOKS);
    CHECK_MSG(n > 0, "the stream was not valid JSON: %.300s", g_sink.buf);

    double points = 0.0, samples = 0.0;
    CHECK(kiln_json_get_num(g_sink.buf, g_toks, n, 0, "points", &points));
    CHECK(kiln_json_get_num(g_sink.buf, g_toks, n, 0, "samples", &samples));
    CHECK_MSG(points <= 50.0, "got %g points for max_points=50", points);
    CHECK(points > 1.0);
    /* Decimated, not truncated: every sample was considered. */
    CHECK(samples > points);

    const int series = kiln_json_find(g_sink.buf, g_toks, n, 0, "series");
    CHECK(series > 0);
    CHECK_EQ_INT(g_toks[series].size, (int)points);
    /* The column order is documented in the response, so the client is not
     * guessing at an array of numbers. */
    CHECK(kiln_json_find(g_sink.buf, g_toks, n, 0, "columns") > 0);
}

KILN_TEST(frlog11_decimation_preserves_a_brief_excursion)
{
    /* The property the whole chart rests on: a short spike must not be averaged
     * away, or a firing that overshot looks clean. */
    static rig_t r;
    rig_init(&r);
    rig_run(&r, 2.0);
    CHECK_OK(kiln_logring_begin_run(&r.ring, 77));

    for (uint32_t i = 0; i < 2000; i++) {
        kiln_log_sample_t s = {};
        s.t_rel_ms    = i * 1000u;
        s.kiln_filt_c = (i == 900u) ? 999.0f : 500.0f;   /* one sample, far above */
        s.setpoint_c  = 500.0f;
        s.state       = KILN_STATE_RUNNING;
        uint8_t rec[KILN_LOG_RECORD_BYTES];
        kiln_logrec_encode(&s, rec);
        CHECK_OK(kiln_logring_append(&r.ring, rec));
    }

    const kiln_api_resp_t resp = stream_log(&r, "run=77&max_points=20");
    CHECK_EQ_INT(resp.status, 200);
    /* 20 buckets of 100 samples each: a mean would hide it completely. */
    CHECK_MSG(strstr(g_sink.buf, "999.0") != NULL,
              "the excursion was averaged away: %.400s", g_sink.buf);
}

KILN_TEST(frweb18_csv_is_offered_with_a_header_row)
{
    static rig_t r;
    rig_init(&r);
    const uint32_t run = seed_a_run(&r);

    char q[64];
    snprintf(q, sizeof(q), "run=%u&max_points=20&format=csv", run);
    const kiln_api_resp_t resp = stream_log(&r, q);

    CHECK_EQ_INT(resp.status, 200);
    CHECK_STR_EQ(resp.content_type, "text/csv");
    CHECK(strncmp(g_sink.buf, "t_rel_ms,kiln_min_c,kiln_max_c", 30) == 0);

    /* One header plus at most max_points rows. */
    size_t rows = 0;
    for (size_t i = 0; i < g_sink.len; i++) {
        if (g_sink.buf[i] == '\n') {
            rows++;
        }
    }
    CHECK(rows > 1);
    CHECK(rows <= 21);
}

KILN_TEST(frweb11_a_24_hour_run_is_served_as_a_few_hundred_points)
{
    /* FR-WEB-11 gives a mid-range phone 2 s to render a 24 h run, which is only
     * possible if the device does the decimating.  8640 samples in, 800 out. */
    static rig_t r;
    rig_init(&r);
    rig_run(&r, 1.0);
    CHECK_OK(kiln_logring_begin_run(&r.ring, 24));

    const uint32_t samples = 24u * 3600u / 10u;      /* 10 s interval */
    for (uint32_t i = 0; i < samples; i++) {
        kiln_log_sample_t s = {};
        s.t_rel_ms    = i * 10000u;
        s.kiln_filt_c = 20.0f + (float)i * 0.1f;
        s.setpoint_c  = s.kiln_filt_c + 2.0f;
        s.state       = KILN_STATE_RUNNING;
        uint8_t rec[KILN_LOG_RECORD_BYTES];
        kiln_logrec_encode(&s, rec);
        CHECK_OK(kiln_logring_append(&r.ring, rec));
    }

    const kiln_api_resp_t resp = stream_log(&r, "run=24&max_points=800");
    CHECK_EQ_INT(resp.status, 200);

    const int n = kiln_json_parse(g_sink.buf, g_sink.len, g_toks, RTOKS);
    CHECK(n > 0);
    double points = 0.0;
    CHECK(kiln_json_get_num(g_sink.buf, g_toks, n, 0, "points", &points));
    CHECK(points <= 800.0);
    CHECK(points > 700.0);

    /* And the payload is a size a phone can parse quickly -- roughly 60 bytes a
     * point rather than the ~140 an object per point would cost. */
    CHECK_MSG(g_sink.len < 70000, "the response was %zu bytes", g_sink.len);
}

KILN_TEST(an_empty_or_absent_run_streams_an_empty_result_not_an_error)
{
    static rig_t r;
    rig_init(&r);
    rig_run(&r, 1.0);

    /* FR-LOG-09: a run whose samples the ring overwrote is a real case, and the
     * chart has to be able to tell it from a failure. */
    const kiln_api_resp_t resp = stream_log(&r, "run=4242");
    CHECK_EQ_INT(resp.status, 200);

    const int n = kiln_json_parse(g_sink.buf, g_sink.len, g_toks, RTOKS);
    CHECK(n > 0);
    double points = 1.0;
    CHECK(kiln_json_get_num(g_sink.buf, g_toks, n, 0, "points", &points));
    CHECK_NEAR(points, 0.0, 0.001);
}

KILN_TEST(a_bad_log_query_is_rejected)
{
    static rig_t r;
    rig_init(&r);
    rig_run(&r, 1.0);

    expect_error(stream_log(&r, "format=xml"), 400, "invalid_value");
    expect_error(stream_log(&r, "from=5000&to=1000"), 400, "invalid_range");
}

KILN_TEST(a_client_that_disconnects_stops_the_scan)
{
    /* Otherwise a vanished client costs a full scan of 24 h of flash. */
    static rig_t r;
    rig_init(&r);
    const uint32_t run = seed_a_run(&r);

    memset(&g_sink, 0, sizeof(g_sink));
    g_sink.fail_after = true;
    g_sink.fail_at    = 200;

    kiln_api_req_t req = {};
    req.method = KILN_HTTP_GET;
    req.path   = "/api/log";
    char q[64];
    snprintf(q, sizeof(q), "run=%u&max_points=500", run);
    req.query = q;
    req.authenticated = true;

    kiln_api_resp_t resp = {};
    resp.body     = r.body;
    resp.body_cap = sizeof(r.body);
    const kiln_err_t e = kiln_api_log_stream(&r.api, &req, sink_write, &g_sink, &resp);

    CHECK_ERR(e, KILN_ERR_IO);
    CHECK(g_sink.len < 2000);
}

KILN_TEST(frlog13_the_log_can_be_erased)
{
    static rig_t r;
    rig_init(&r);
    const uint32_t run = seed_a_run(&r);

    char q[48];
    snprintf(q, sizeof(q), "run=%u", run);
    CHECK(stream_log(&r, q).status == 200);
    const size_t before = g_sink.len;
    CHECK(before > 100);

    CHECK_EQ_INT(call(&r, KILN_HTTP_DELETE, "/api/log", NULL, NULL).status, 200);
    CHECK(stream_log(&r, q).status == 200);
    CHECK(g_sink.len < before);
}

/* --- 12.2's request-handling rules ------------------------------------- */

KILN_TEST(frweb23_state_changing_endpoints_require_authentication)
{
    static rig_t r;
    rig_init(&r);
    rig_run(&r, 2.0);

    kiln_api_req_t req = {};
    req.authenticated = false;

    kiln_api_resp_t resp = {};
    resp.body     = r.body;
    resp.body_cap = sizeof(r.body);

    /* A read is allowed; everything that changes state is not. */
    req.method = KILN_HTTP_GET;
    req.path   = "/api/status";
    (void)kiln_api_handle(&r.api, &req, &resp);
    CHECK_EQ_INT(resp.status, 200);

    const struct { kiln_http_method_t m; const char *p; } guarded[] = {
        { KILN_HTTP_POST,   "/api/run" },
        { KILN_HTTP_POST,   "/api/run/abort" },
        { KILN_HTTP_POST,   "/api/manual" },
        { KILN_HTTP_POST,   "/api/fault/ack" },
        { KILN_HTTP_PUT,    "/api/config" },
        { KILN_HTTP_POST,   "/api/config/defaults" },
        { KILN_HTTP_POST,   "/api/programs" },
        { KILN_HTTP_DELETE, "/api/programs/5" },
        { KILN_HTTP_POST,   "/api/tune" },
        { KILN_HTTP_DELETE, "/api/log" },
        { KILN_HTTP_POST,   "/api/current/calibrate" },
    };
    for (size_t i = 0; i < sizeof(guarded) / sizeof(guarded[0]); i++) {
        req.method   = guarded[i].m;
        req.path     = guarded[i].p;
        req.body     = "{}";
        req.body_len = 2;
        (void)kiln_api_handle(&r.api, &req, &resp);
        CHECK_MSG(resp.status == 401, "%s was reachable unauthenticated",
                  guarded[i].p);
    }

    /* And the rule is by method, so a route added later is protected by default
     * rather than by someone remembering to list it. */
    kiln_api_req_t nw = { .method = KILN_HTTP_POST, .path = "/api/something/new" };
    CHECK(kiln_api_needs_auth(&nw));
    kiln_api_req_t rd = { .method = KILN_HTTP_GET, .path = "/api/anything" };
    CHECK(!kiln_api_needs_auth(&rd));
}

KILN_TEST(nfr19_an_oversized_body_is_refused_before_it_is_parsed)
{
    static rig_t r;
    rig_init(&r);

    static char big[KILN_API_MAX_BODY * 2];
    memset(big, 'x', sizeof(big) - 1);
    big[0] = '{';
    big[sizeof(big) - 1] = '\0';

    kiln_api_req_t req = {};
    req.method        = KILN_HTTP_POST;
    req.path          = "/api/programs";
    req.body          = big;
    req.body_len      = sizeof(big) - 1;
    req.authenticated = true;

    kiln_api_resp_t resp = {};
    resp.body     = r.body;
    resp.body_cap = sizeof(r.body);
    (void)kiln_api_handle(&r.api, &req, &resp);

    /* A body that did not fit was never fully read, so acting on its prefix
     * would be acting on a truncated request. */
    expect_error(resp, 413, "body_too_large");
}

KILN_TEST(frweb20_unknown_routes_and_methods_use_the_error_envelope)
{
    static rig_t r;
    rig_init(&r);

    expect_error(call(&r, KILN_HTTP_GET, "/api/nope", NULL, NULL), 404, "not_found");
    expect_error(call(&r, KILN_HTTP_GET, "/api/programs/999", NULL, NULL),
                 404, "not_found");
    expect_error(call(&r, KILN_HTTP_DELETE, "/api/status", NULL, NULL),
                 405, "method_not_allowed");
    expect_error(call(&r, KILN_HTTP_POST, "/api/info", NULL, "{}"),
                 405, "method_not_allowed");
}

KILN_TEST(a_response_that_does_not_fit_says_so_rather_than_truncating)
{
    /* Half a JSON document is not a response; the client would fail to parse it
     * and have no way to know the device had run out of room. */
    static rig_t r;
    rig_init(&r);

    char tiny[48];
    kiln_api_req_t req = { .method = KILN_HTTP_GET, .path = "/api/config",
                           .authenticated = true };
    kiln_api_resp_t resp = {};
    resp.body     = tiny;
    resp.body_cap = sizeof(tiny);
    (void)kiln_api_handle(&r.api, &req, &resp);

    CHECK_EQ_INT(resp.status, 500);
    CHECK(strstr(resp.body, "response_too_large") != NULL);
}

KILN_TEST(query_parameters_are_decoded_and_bounded)
{
    char v[16];
    CHECK(kiln_api_query_get("a=1&name=my%20glaze&b=2", "name", v, sizeof(v)));
    CHECK_STR_EQ(v, "my glaze");
    CHECK(kiln_api_query_get("x=a+b", "x", v, sizeof(v)));
    CHECK_STR_EQ(v, "a b");
    CHECK(!kiln_api_query_get("a=1", "b", v, sizeof(v)));
    CHECK(!kiln_api_query_get(NULL, "a", v, sizeof(v)));

    /* Refused rather than truncated, like everywhere else. */
    char small[4];
    CHECK(!kiln_api_query_get("k=abcdefgh", "k", small, sizeof(small)));

    uint32_t n = 0;
    CHECK(kiln_api_query_uint("max_points=800", "max_points", &n));
    CHECK_EQ_UINT(n, 800u);
    CHECK(!kiln_api_query_uint("max_points=12x", "max_points", &n));
    CHECK(!kiln_api_query_uint("max_points=", "max_points", &n));
}

KILN_TEST(frweb05_the_telemetry_event_matches_the_status_shape)
{
    /* One shape, so the UI has one parser for the stream and the one-shot. */
    static rig_t r;
    rig_init(&r);
    rig_run(&r, 2.0);

    char ev[4096];
    const size_t n = kiln_api_telemetry_event(&r.api, ev, sizeof(ev));
    CHECK(n > 0);

    kiln_json_tok_t t[RTOKS];
    const int tn = kiln_json_parse(ev, n, t, RTOKS);
    CHECK(tn > 0);
    CHECK_EQ_INT(t[0].type, KILN_JSON_OBJECT);
    CHECK(kiln_json_find(ev, t, tn, 0, "kiln_c") > 0);
    CHECK(kiln_json_find(ev, t, tn, 0, "state") > 0);

    /* And a buffer too small produces nothing rather than a half event. */
    char tiny[16];
    CHECK_EQ_UINT(kiln_api_telemetry_event(&r.api, tiny, sizeof(tiny)), 0u);
}

KILN_TEST(the_api_validates_its_own_arguments)
{
    static rig_t r;
    rig_init(&r);

    kiln_api_resp_t resp = {};
    resp.body     = r.body;
    resp.body_cap = sizeof(r.body);
    kiln_api_req_t req = { .method = KILN_HTTP_GET, .path = "/api/status",
                           .authenticated = true };

    CHECK_ERR(kiln_api_handle(NULL, &req, &resp), KILN_ERR_INVALID_ARG);
    CHECK_ERR(kiln_api_handle(&r.api, NULL, &resp), KILN_ERR_INVALID_ARG);
    CHECK_ERR(kiln_api_handle(&r.api, &req, NULL), KILN_ERR_INVALID_ARG);

    kiln_api_req_t nopath = { .method = KILN_HTTP_GET, .authenticated = true };
    CHECK_ERR(kiln_api_handle(&r.api, &nopath, &resp), KILN_ERR_INVALID_ARG);
}

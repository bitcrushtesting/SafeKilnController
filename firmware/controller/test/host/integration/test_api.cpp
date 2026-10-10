/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The REST API of architecture 12.1 against the real application and a simulated
 * kiln -- SWR-WEB-13, SWR-WEB-19, SWR-WEB-20, SWR-WEB-23, SWR-LOG-10, SWR-WEB-18,
 * and the request-handling rules of 12.2.
 *
 * This is the "API suite green" half of M6's exit criterion. It runs on the host
 * because the API layer has no transport in it: a handler takes a method, a path
 * and a body, which is exactly what a test can hand it.
 */

#include <stdio.h>
#include <string.h>
#include "kiln_check.h"
#include "kiln_app/run_index.h"
#include "kiln_core/logring.h"
#include "kiln_core/profile.h"
#include "kiln_hal_host/hal_host.h"
#include "kiln_sim/sim.h"
#include "kiln_web/api.h"
#include "kiln_web/json.h"

constexpr size_t LOG_SECTORS  = 24u;
constexpr size_t SECTOR_BYTES = 4096u;
constexpr size_t BODY_CAP     = 16384u;

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
    /* SWR-SAF-31: bound so /api/status reports a door at all.  With no port
     * the snapshot reports no monitoring, which is a real state of a real
     * kiln but not the one most of these tests are about. */
    ports.door      = &r->sim_ports.door;
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

/* SWR-WEB-26: the web interface cannot start a firing, heat the kiln, change
 * configuration or acknowledge a fault, so a test that needs any of those sets
 * it up the way the local HMI does -- through kiln_app, not over HTTP.  That is
 * not a workaround for the restriction; it is the restriction, expressed as the
 * only remaining way to reach those commands.
 */
static void local_start(rig_t *r, uint8_t program_id)
{
    /* From the image, which is where the programs are and where the local HMI
     * reads them from too. */
    kiln_program_t prog;
    CHECK_OK(kiln_profile_example(program_id, &prog));
    CHECK_OK(kiln_app_start(&r->app, &prog));
}

static void local_tune(rig_t *r, float setpoint_c)
{
    CHECK_OK(kiln_app_autotune(&r->app, setpoint_c));
}

static void local_manual(rig_t *r, uint16_t permille)
{
    CHECK_OK(kiln_app_manual(&r->app, permille));
}

/* Apply one configuration change locally, by name, through the same model the
 * API projection reads. */
static kiln_err_t local_set_cfg_str(rig_t *r, const char *key, const char *value)
{
    kiln_config_t c = r->app.cfg;
    const kiln_cfg_item_t *it = kiln_config_find(key);
    if (it == NULL) {
        return KILN_ERR_NOT_FOUND;
    }
    const kiln_err_t e = kiln_config_set_str(&c, it, value);
    if (e != KILN_OK) {
        return e;
    }
    const kiln_cfg_item_t *bad = NULL;
    return kiln_app_apply_config(&r->app, &c, &bad);
}

static kiln_err_t local_set_cfg(rig_t *r, const char *key, double value)
{
    kiln_config_t c = r->app.cfg;
    const kiln_cfg_item_t *it = kiln_config_find(key);
    if (it == NULL) {
        return KILN_ERR_NOT_FOUND;
    }
    const kiln_err_t e = kiln_config_set_num(&c, it, value);
    if (e != KILN_OK) {
        return e;
    }
    const kiln_cfg_item_t *bad = NULL;
    return kiln_app_apply_config(&r->app, &c, &bad);
}

/* One request.  Always authenticated unless a test says otherwise, because
 * SWR-WEB-23 is tested on its own and every other test is about the handler. */
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

/* Every route the web interface may no longer reach, so the restriction is
 * asserted in one place rather than re-derived per test. */
static void expect_read_only(rig_t *r, kiln_http_method_t m, const char *path,
                             const char *body)
{
    const kiln_api_resp_t resp = call(r, m, path, NULL, body);
    CHECK_EQ_INT(resp.status, 403);
    CHECK_MSG(strstr(resp.body, "read_only") != NULL,
              "%s must be refused as read_only, got: %s", path, resp.body);
}


/* Parse the response and return the token array, so assertions read as JSON
 * rather than as substring searches.
 *
 * Generous: /api/config projects every one of the ~70 configuration items with a
 * dozen members each, and an 800-point log query is ten thousand numbers.  Both
 * are legitimate responses, so the test's budget has to exceed them or the test
 * is measuring its own array size. */
constexpr size_t RTOKS = 40000;
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

/* SWR-WEB-20: every error is {"error":{"code","message"}}. */
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

/* --- status, info (SWR-RUN-05, SWR-UPD-06) ------------------------------- */

/*
 * @relation(SWR-RUN-05, scope=function)
 */
KILN_TEST(swrrun05_status_exposes_everything_the_requirement_lists)
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

    /* SWR-RUN-05's list. */
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

    /* SWR-WEB-04, SWR-SAF-31. A string and not a boolean, so that a client
     * cannot read an unmonitored door as a shut one: the three cases have
     * three different remedies, and "there is no switch" is the one warning
     * 113 exists to make visible. */
    char door[16];
    str_of(&resp, n, "door", door, sizeof(door));
    CHECK_STR_EQ(door, "shut");
}

/*
 * @relation(SWR-SAF-31, scope=function)
 */
KILN_TEST(swrsaf31_the_api_reports_all_three_door_states)
{
    static rig_t r;
    rig_init(&r);
    rig_run(&r, 1.0);

    char door[16];
    kiln_api_resp_t resp;

    kiln_sim_inject(&r.sim, KILN_INJ_DOOR_SWITCH_OPEN);
    rig_run(&r, 1.0);
    resp = call(&r, KILN_HTTP_GET, "/api/status", NULL, NULL);
    str_of(&resp, parse_resp(&resp), "door", door, sizeof(door));
    CHECK_STR_EQ(door, "open");

    kiln_sim_clear(&r.sim, KILN_INJ_DOOR_SWITCH_OPEN);
    rig_run(&r, 1.0);
    resp = call(&r, KILN_HTTP_GET, "/api/status", NULL, NULL);
    str_of(&resp, parse_resp(&resp), "door", door, sizeof(door));
    CHECK_STR_EQ(door, "shut");

    /* A kiln with no interlock fitted: the switch is absent, so the reading
     * means nothing and the API says that rather than guessing. */
    kiln_sim_inject(&r.sim, KILN_INJ_DOOR_ABSENT);
    rig_run(&r, 1.0);
    resp = call(&r, KILN_HTTP_GET, "/api/status", NULL, NULL);
    str_of(&resp, parse_resp(&resp), "door", door, sizeof(door));
    CHECK_STR_EQ(door, "unmonitored");
}

/*
 * @relation(SWR-WEB-25, scope=function)
 */
KILN_TEST(swrweb25_validity_travels_with_the_value_so_stale_is_distinguishable)
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

/*
 * @relation(SWR-UPD-06, scope=function)
 */
KILN_TEST(swrupd06_info_reports_identity_and_gain_provenance)
{
    static rig_t r;
    rig_init(&r);

    const kiln_api_resp_t resp = call(&r, KILN_HTTP_GET, "/api/info", NULL, NULL);
    CHECK_EQ_INT(resp.status, 200);
    const int n = parse_resp(&resp);

    CHECK(kiln_json_find(resp.body, g_toks, n, 0, "version") > 0);
    CHECK(kiln_json_find(resp.body, g_toks, n, 0, "target") > 0);

    /* SWR-TUN-11: factory defaults are not gains for *this* kiln, and the UI has
     * to be able to say so. */
    const int g = kiln_json_find(resp.body, g_toks, n, 0, "gains");
    CHECK(g > 0);
    double kp = 0.0;
    CHECK(kiln_json_get_num(resp.body, g_toks, n, g, "kp", &kp));
    CHECK_NEAR(kp, 6.0, 0.001);
    bool tuned = true;
    CHECK(kiln_json_get_bool(resp.body, g_toks, n, g, "tuned", &tuned));
}

/* --- the production data block (SWR-PROD-03) ----------------------------- */

namespace {

/* A system port that reports a programmed unit, and one that reports an
 * unprogrammed one.  The real reader is hal_prod.cpp over NVS, which no host
 * test can reach; what is testable here, and what a client actually depends on,
 * is the shape of the payload in both states. */
kiln_err_t fake_prod_programmed(void *ctx, kiln_prod_info_t *out)
{
    (void)ctx;
    memset(out, 0, sizeof(*out));
    out->programmed = true;
    (void)snprintf(out->manufacturer, sizeof(out->manufacturer), "Bitcrush Testing");
    (void)snprintf(out->model, sizeof(out->model), "SafeKiln-1");
    (void)snprintf(out->revision, sizeof(out->revision), "rev-C");
    (void)snprintf(out->serial, sizeof(out->serial), "SK1-2026-000042");
    (void)snprintf(out->production_date, sizeof(out->production_date), "2026-10-07");
    return KILN_OK;
}

kiln_err_t fake_prod_blank(void *ctx, kiln_prod_info_t *out)
{
    (void)ctx;
    memset(out, 0, sizeof(*out));
    return KILN_OK;
}

}  // namespace

/*
 * @relation(SWR-PROD-03, scope=function)
 */
KILN_TEST(swrprod03_info_reports_the_production_block_when_programmed)
{
    static rig_t r;
    rig_init(&r);

    static kiln_port_system_t sys;
    memset(&sys, 0, sizeof(sys));
    sys.prod_info = fake_prod_programmed;
    r.api.system = &sys;

    const kiln_api_resp_t resp = call(&r, KILN_HTTP_GET, "/api/info", NULL, NULL);
    CHECK_EQ_INT(resp.status, 200);
    const int n = parse_resp(&resp);

    const int p = kiln_json_find(resp.body, g_toks, n, 0, "production");
    CHECK(p > 0);

    bool programmed = false;
    CHECK(kiln_json_get_bool(resp.body, g_toks, n, p, "programmed", &programmed));
    CHECK(programmed);

    char buf[40];
    CHECK(kiln_json_get_str(resp.body, g_toks, n, p, "serial", buf, sizeof(buf)));
    CHECK_STR_EQ(buf, "SK1-2026-000042");
    CHECK(kiln_json_get_str(resp.body, g_toks, n, p, "model", buf, sizeof(buf)));
    CHECK_STR_EQ(buf, "SafeKiln-1");
    CHECK(kiln_json_get_str(resp.body, g_toks, n, p, "production_date", buf, sizeof(buf)));
    CHECK_STR_EQ(buf, "2026-10-07");
}

/*
 * @relation(SWR-PROD-04, scope=function)
 */
KILN_TEST(swrprod04_an_unprogrammed_unit_says_so_rather_than_omitting_the_block)
{
    static rig_t r;
    rig_init(&r);

    static kiln_port_system_t sys;
    memset(&sys, 0, sizeof(sys));
    sys.prod_info = fake_prod_blank;
    r.api.system = &sys;

    const kiln_api_resp_t resp = call(&r, KILN_HTTP_GET, "/api/info", NULL, NULL);
    CHECK_EQ_INT(resp.status, 200);
    const int n = parse_resp(&resp);

    /* The object is present either way: a client must never have to tell an
     * absent key apart from absent data. */
    const int p = kiln_json_find(resp.body, g_toks, n, 0, "production");
    CHECK(p > 0);
    bool programmed = true;
    CHECK(kiln_json_get_bool(resp.body, g_toks, n, p, "programmed", &programmed));
    CHECK(!programmed);
}

/*
 * @relation(SWR-PROD-04, scope=function)
 */
KILN_TEST(swrprod04_a_port_without_prod_info_still_answers_with_the_block)
{
    static rig_t r;
    rig_init(&r);
    /* No system port at all, which is the rig's normal state and the one the
     * host build has: the handler must not dereference a null function
     * pointer, and must still emit the object. */
    const kiln_api_resp_t resp = call(&r, KILN_HTTP_GET, "/api/info", NULL, NULL);
    CHECK_EQ_INT(resp.status, 200);
    const int n = parse_resp(&resp);

    const int p = kiln_json_find(resp.body, g_toks, n, 0, "production");
    CHECK(p > 0);
    bool programmed = true;
    CHECK(kiln_json_get_bool(resp.body, g_toks, n, p, "programmed", &programmed));
    CHECK(!programmed);
}

/* --- configuration (FR-CFG, SWR-WEB-17) --------------------------------- */

/*
 * @relation(SWR-WEB-17, scope=function)
 */
KILN_TEST(swrweb17_config_is_generated_from_the_schema_with_units_and_ranges)
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

/*
 * @relation(SWR-CFG-07, scope=function)
 */
KILN_TEST(swrcfg07_a_secret_is_never_returned_only_whether_it_is_set)
{
    static rig_t r;
    rig_init(&r);

    CHECK_OK(local_set_cfg_str(&r, "net.wifi_pass", "hunter2"));

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
            strcmp(key, "net.wifi_pass") == 0) {
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

/*
 * @relation(SWR-WEB-26, scope=function)
 */
KILN_TEST(swrweb26_configuration_cannot_be_written_over_the_api)
{
    /* The configured maximum temperature and the safety thresholds live here,
     * so this is "adjusting the temperature" by the most direct route there is.
     * SWR-CFG-03's atomicity and SWR-CFG-08's locking are properties of the
     * config model and are tested in test_configmodel; what is tested here is
     * that neither is reachable from the network. */
    static rig_t r;
    rig_init(&r);
    const float before = r.app.cfg.max_temp_c;

    expect_read_only(&r, KILN_HTTP_PUT, "/api/config",
                     "{\"safety.max_temp_c\":1100}");
    expect_read_only(&r, KILN_HTTP_POST, "/api/config/defaults", NULL);
    CHECK_NEAR(r.app.cfg.max_temp_c, before, 0.01f);

    /* Still fully readable, which is what the settings screen needs. */
    CHECK_EQ_INT(call(&r, KILN_HTTP_GET, "/api/config", NULL, NULL).status, 200);
}

/*
 * @relation(SWR-CFG-08, scope=function)
 */
KILN_TEST(swrcfg08_a_safety_item_is_still_refused_while_running_locally)
{
    /* The restriction did not move with the interface: applying config at the
     * kiln is bounded by SWR-CFG-08 exactly as it was through the API. */
    static rig_t r;
    rig_init(&r);
    rig_run(&r, 2.0);
    local_start(&r, 1);

    CHECK_ERR(local_set_cfg(&r, "safety.max_temp_c", 1100.0), KILN_ERR_STATE);
    CHECK_OK(local_set_cfg(&r, "hmi.dim_timeout_s", 120.0));
}

KILN_TEST(an_unknown_configuration_key_is_not_found)
{
    static rig_t r;
    rig_init(&r);
    CHECK_ERR(local_set_cfg(&r, "safety.make_it_hotter", 1.0), KILN_ERR_NOT_FOUND);
}

/*
 * @relation(SWR-CFG-05, scope=function)
 */
KILN_TEST(swrcfg05_configuration_written_locally_is_persisted)
{
    static rig_t r;
    rig_init(&r);
    const uint32_t sets = r.kv.sets;
    CHECK_OK(local_set_cfg(&r, "log.interval_s", 42.0));
    CHECK(r.kv.sets > sets);

    kiln_config_t stored;
    CHECK_OK(kiln_settings_load(&r.kv_port, &stored));
    CHECK_EQ_UINT(stored.log_interval_s, 42u);
}

/* --- programs (SWR-PRG-07, SWR-WEB-12, SWR-WEB-13) ------------------------ */

/*
 * @relation(SWR-PRG-09, scope=function)
 */
KILN_TEST(swrprg09_the_examples_are_listed_and_marked_readonly)
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

/*
 * @relation(SWR-WEB-26, scope=function)
 */
KILN_TEST(swrweb26_an_unknown_program_slot_is_404_and_not_an_empty_list)
{
    /* Asking for a program that is not there is a different mistake from
     * asking what programs exist, and the status has to say which. */
    static rig_t r;
    rig_init(&r);
    const kiln_api_resp_t missing = call(&r, KILN_HTTP_GET, "/api/programs/99", NULL, NULL);
    expect_error(missing, 404, "not_found");
}

/*
 * @relation(SWR-WEB-26, scope=function)
 */
KILN_TEST(swrweb26_programs_are_readable_and_nothing_more)
{
    /* Authoring went the way of the other writes on 2026-10-06: the password
     * that would have guarded it could not be set from anywhere, and an
     * authenticated write whose gate can never be armed is an unauthenticated
     * write with extra steps. */
    static rig_t r;
    rig_init(&r);

    const kiln_api_resp_t list = call(&r, KILN_HTTP_GET, "/api/programs", NULL, NULL);
    CHECK_EQ_INT(list.status, 200);
    CHECK_MSG(strstr(list.body, "segments") != NULL,
              "the seeded examples should still be readable: %.120s", list.body);

    const kiln_api_resp_t one = call(&r, KILN_HTTP_GET, "/api/programs/1", NULL, NULL);
    CHECK_EQ_INT(one.status, 200);

    expect_read_only(&r, KILN_HTTP_POST,   "/api/programs",   "{\"name\":\"x\"}");
    expect_read_only(&r, KILN_HTTP_PUT,    "/api/programs/1", "{\"name\":\"x\"}");
    expect_read_only(&r, KILN_HTTP_DELETE, "/api/programs/1", NULL);
    expect_read_only(&r, KILN_HTTP_POST,   "/api/programs/1/copy", NULL);
}

/*
 * @relation(SWR-PRG-09, scope=function)
 */
KILN_TEST(swrprg09_a_read_only_example_is_marked_as_such)
{
    /* SWR-PRG-09's protection is now moot over the API, since nothing can be
     * edited at all, but the flag still has to reach a client so the local
     * interface can show it. */
    static rig_t r;
    rig_init(&r);
    const kiln_api_resp_t list = call(&r, KILN_HTTP_GET, "/api/programs", NULL, NULL);
    CHECK_EQ_INT(list.status, 200);
    CHECK(strstr(list.body, "readonly") != NULL);
}

/*
 * @relation(SWR-WEB-26, scope=function)
 */
KILN_TEST(swrweb26_a_body_on_a_read_only_route_changes_nothing)
{
    /* There is no parser left to confuse: the method is refused before the
     * body is looked at, which is also why an oversized or malformed body on
     * a write route can no longer reach any parsing code. */
    static rig_t r;
    rig_init(&r);
    expect_read_only(&r, KILN_HTTP_POST, "/api/programs",
                     "{\"name\":\"\x01\x02 not json at all");
    const kiln_api_resp_t list = call(&r, KILN_HTTP_GET, "/api/programs", NULL, NULL);
    CHECK_EQ_INT(list.status, 200);
}

/* --- run control (FR-RUN) ---------------------------------------------- */

/*
 * @relation(SWR-WEB-26, scope=function)
 */
KILN_TEST(swrweb26_run_control_is_not_reachable_from_the_api)
{
    /* SWR-WEB-26.  The lifecycle itself is unchanged and still tested -- at the
     * app layer, in test_integration -- but no part of it is reachable over
     * HTTP.  Start, pause, resume and abort happen at the kiln. */
    static rig_t r;
    rig_init(&r);
    rig_run(&r, 2.0);

    expect_read_only(&r, KILN_HTTP_POST, "/api/run", "{\"program_id\":1}");
    CHECK_MSG(r.app.state == KILN_STATE_IDLE, "the refusal must not have started anything");

    local_start(&r, 1);
    CHECK_EQ_INT(r.app.state, KILN_STATE_RUNNING);
    rig_run(&r, 5.0);

    /* Running, and still unreachable. */
    expect_read_only(&r, KILN_HTTP_POST, "/api/run/pause",  NULL);
    expect_read_only(&r, KILN_HTTP_POST, "/api/run/resume", NULL);
    expect_read_only(&r, KILN_HTTP_POST, "/api/run/abort",  NULL);
    expect_read_only(&r, KILN_HTTP_PATCH, "/api/run/segments", "{\"segments\":[]}");
    CHECK_MSG(r.app.state == KILN_STATE_RUNNING,
              "a refused request must not have changed the run");

    /* Reading the run is still exactly as available as it was. */
    CHECK_EQ_INT(call(&r, KILN_HTTP_GET, "/api/status", NULL, NULL).status, 200);
}

/*
 * @relation(SWR-WEB-26, scope=function)
 */
KILN_TEST(swrweb26_a_latched_fault_cannot_be_acknowledged_over_the_api)
{
    /* SWR-SAF-17 and SWR-WEB-26 pull the same way: clearing a fault re-arms a kiln
     * that has already failed, and the operator should be in front of it. */
    static rig_t r;
    rig_init(&r);
    rig_run(&r, 2.0);
    kiln_sim_inject(&r.sim, KILN_INJ_TC_OPEN);
    for (int i = 0; i < 20000 && r.app.fault == KILN_FAULT_NONE; i++) {
        rig_step(&r);
    }
    CHECK(r.app.fault != KILN_FAULT_NONE);

    expect_read_only(&r, KILN_HTTP_POST, "/api/fault/ack", NULL);
    CHECK_MSG(r.app.fault != KILN_FAULT_NONE, "the fault must still be latched");

    /* The fault is still *readable*, which is what the SWR-WEB-24 banner needs. */
    const kiln_api_resp_t st = call(&r, KILN_HTTP_GET, "/api/status", NULL, NULL);
    CHECK_EQ_INT(st.status, 200);
    CHECK(strstr(st.body, "fault") != NULL);

    /* And it clears locally, once the condition has gone (SWR-SAF-18). */
    kiln_sim_clear(&r.sim, KILN_INJ_TC_OPEN);
    rig_run(&r, 1.0);
    CHECK_OK(kiln_app_clear_fault(&r.app));
    CHECK_EQ_INT(r.app.fault, KILN_FAULT_NONE);
}

/*
 * @relation(SWR-WEB-24, scope=function)
 */
KILN_TEST(swrweb24_the_banner_data_carries_the_operator_text)
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

/*
 * @relation(SWR-WEB-26, scope=function)
 */
KILN_TEST(swrweb26_the_running_program_cannot_be_edited_from_the_api)
{
    /* SWR-PRG-10 let the remaining segments of a running program be adjusted.
     * That is "adjust the temperature" of a kiln that is already hot, so it is
     * withdrawn from the web with the rest of run control. */
    static rig_t r;
    rig_init(&r);
    rig_run(&r, 2.0);
    local_start(&r, 1);
    rig_run(&r, 5.0);

    expect_read_only(&r, KILN_HTTP_PATCH, "/api/run/segments",
        "{\"name\":\"Glaze cone 6\",\"segments\":[{\"target_c\":999,\"rate_c_per_h\":100}]}");
    CHECK_EQ_INT(r.app.state, KILN_STATE_RUNNING);
}

/*
 * @relation(SWR-WEB-26, scope=function)
 */
KILN_TEST(swrweb26_manual_heating_is_not_reachable_from_the_api)
{
    /* Manual mode puts duty straight into the elements; it is the most direct
     * "make the kiln hot" there is.  The bounding of SWR-CTL-14 is unchanged and
     * tested at the app layer -- it is simply no longer reachable from here. */
    static rig_t r;
    rig_init(&r);
    rig_run(&r, 2.0);

    expect_read_only(&r, KILN_HTTP_POST, "/api/manual", "{\"duty_permille\":400}");
    CHECK_MSG(r.app.state == KILN_STATE_IDLE, "the refusal must not have started heating");
    CHECK_EQ_UINT(r.app.duty_manual, 0u);

    /* Locally it still works, and is still bounded. */
    local_manual(&r, 400);
    CHECK_EQ_INT(r.app.state, KILN_STATE_MANUAL);
    CHECK_EQ_UINT(r.app.duty_manual, 400u);
    CHECK_ERR(kiln_app_manual(&r.app, 5000u), KILN_ERR_RANGE);
}

/* --- tuning (SWR-TUN-09, SWR-TUN-10) ------------------------------------- */

/*
 * @relation(SWR-TUN-10, scope=function)
 */
KILN_TEST(swrtun10_tuning_progress_and_candidates_are_exposed)
{
    static rig_t r;
    rig_init(&r);
    rig_run(&r, 2.0);

    local_tune(&r, 600.0f);
    CHECK_EQ_INT(r.app.state, KILN_STATE_AUTOTUNE);

    const kiln_api_resp_t resp = call(&r, KILN_HTTP_GET, "/api/tune", NULL, NULL);
    CHECK_EQ_INT(resp.status, 200);
    const int n = parse_resp(&resp);

    char phase[24];
    str_of(&resp, n, "phase", phase, sizeof(phase));
    CHECK_STR_EQ(phase, "approach");
    CHECK(kiln_json_find(resp.body, g_toks, n, 0, "candidates") > 0);

    /* SWR-TUN-09: nothing is stored until a rule is accepted, so accepting with
     * no result is refused rather than writing zeros over the gains. */
    /* Accepting a gain set and cancelling are both commands, so both are gone
     * from the API even while the tune itself remains readable. */
    expect_read_only(&r, KILN_HTTP_POST, "/api/tune/accept", "{\"rule\":\"tyreus-luyben\"}");
    expect_read_only(&r, KILN_HTTP_POST, "/api/tune/cancel", NULL);
}

/*
 * @relation(SWR-WEB-26, scope=function)
 */
KILN_TEST(swrweb26_autotune_cannot_be_started_from_the_api)
{
    /* Autotune drives the kiln through relay oscillation at its setpoint: it is
     * firing by another name, so it goes the same way as /api/run. */
    static rig_t r;
    rig_init(&r);
    rig_run(&r, 2.0);

    expect_read_only(&r, KILN_HTTP_POST, "/api/tune", "{\"setpoint_c\":600}");
    expect_read_only(&r, KILN_HTTP_POST, "/api/tune/accept", NULL);
    expect_read_only(&r, KILN_HTTP_POST, "/api/tune/cancel", NULL);
    CHECK_MSG(r.app.state == KILN_STATE_IDLE, "the refusal must not have started tuning");

    /* SWR-SAF-23 is a property of the command, not of the transport, and still
     * holds on the path that remains.  KILN_ERR_RANGE here means "running, but
     * clamped below what was asked" -- the tune starts at the configured
     * maximum and says so, rather than silently tuning somewhere else. */
    CHECK_ERR(kiln_app_autotune(&r.app, 5000.0f), KILN_ERR_RANGE);
    CHECK_EQ_INT(r.app.state, KILN_STATE_AUTOTUNE);
    CHECK_NEAR(r.app.tune.cfg.setpoint_c, 1280.0f, 0.5f);
}

/* --- current, storage, runs -------------------------------------------- */

KILN_TEST(swrcur_the_current_endpoint_reports_measurement_and_wear)
{
    static rig_t r;
    rig_init(&r);
    rig_run(&r, 2.0);
    local_start(&r, 1);
    rig_run(&r, 20.0);

    const kiln_api_resp_t resp = call(&r, KILN_HTTP_GET, "/api/current", NULL, NULL);
    CHECK_EQ_INT(resp.status, 200);
    const int n = parse_resp(&resp);

    (void)num_of(&resp, n, "current_a");
    (void)num_of(&resp, n, "reference_a");
    (void)num_of(&resp, n, "apparent_va");
    (void)num_of(&resp, n, "energy_wh");

    /* SWR-CUR-07 requires the resistive-load assumption to be stated wherever the
     * figure is presented, so it travels with it. */
    char basis[80];
    str_of(&resp, n, "power_basis", basis, sizeof(basis));
    CHECK(strstr(basis, "resistive") != NULL);

    /* SWR-CUR-13 */
    const int sw = kiln_json_find(resp.body, g_toks, n, 0, "switching");
    CHECK(sw > 0);
    CHECK(kiln_json_find(resp.body, g_toks, n, sw, "contactor_ops") > 0);
    CHECK(kiln_json_find(resp.body, g_toks, n, sw, "ssr_ops") > 0);

    /* SWR-WEB-26: the calibration reference feeds SWR-SAF-28's deviation bands, so it
     * is set at the kiln.  SWR-CUR-06's own refusal is tested in test_current. */
    expect_read_only(&r, KILN_HTTP_POST, "/api/current/calibrate", "{\"known_a\":0}");
}

/*
 * @relation(SWR-LOG-15, scope=function)
 */
KILN_TEST(swrlog15_storage_health_is_reported)
{
    static rig_t r;
    rig_init(&r);
    rig_run(&r, 2.0);
    local_start(&r, 1);
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

/*
 * @relation(SWR-LOG-09, scope=function)
 */
KILN_TEST(swrlog09_the_run_list_is_newest_first_and_marks_truncation)
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

/* --- the log stream (SWR-LOG-10, SWR-LOG-11, SWR-WEB-11, SWR-WEB-18) ------- */

typedef struct { char buf[262144]; size_t len; bool fail_after; size_t fail_at; } sink_t;

static bool sink_write(void *user, const char *data, size_t len)
{
    sink_t *s = static_cast<sink_t *>(user);
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
    local_start(r, 1);
    const uint32_t run_id = r->app.record.run_id;
    rig_run(r, 400.0);
    return run_id;
}

/*
 * @relation(SWR-LOG-10, scope=function)
 */
KILN_TEST(swrlog10_a_log_query_decimates_to_the_requested_point_count)
{
    static rig_t r;
    rig_init(&r);
    const uint32_t run = seed_a_run(&r);

    char q[64];
    (void)snprintf(q, sizeof(q), "run=%u&max_points=50", run);
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

/*
 * @relation(SWR-LOG-11, scope=function)
 */
KILN_TEST(swrlog11_decimation_preserves_a_brief_excursion)
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

/*
 * @relation(SWR-WEB-18, scope=function)
 */
KILN_TEST(swrweb18_csv_is_offered_with_a_header_row)
{
    static rig_t r;
    rig_init(&r);
    const uint32_t run = seed_a_run(&r);

    char q[64];
    (void)snprintf(q, sizeof(q), "run=%u&max_points=20&format=csv", run);
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

/*
 * @relation(SWR-WEB-11, scope=function)
 */
KILN_TEST(swrweb11_a_24_hour_run_is_served_as_a_few_hundred_points)
{
    /* SWR-WEB-11 gives a mid-range phone 2 s to render a 24 h run, which is only
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

    /* SWR-LOG-09: a run whose samples the ring overwrote is a real case, and the
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
    (void)snprintf(q, sizeof(q), "run=%u&max_points=500", run);
    req.query = q;
    req.authenticated = true;

    kiln_api_resp_t resp = {};
    resp.body     = r.body;
    resp.body_cap = sizeof(r.body);
    const kiln_err_t e = kiln_api_log_stream(&r.api, &req, sink_write, &g_sink, &resp);

    CHECK_ERR(e, KILN_ERR_IO);
    CHECK(g_sink.len < 2000);
}

/*
 * @relation(SWR-LOG-13, scope=function)
 */
KILN_TEST(swrlog13_the_log_can_be_erased)
{
    static rig_t r;
    rig_init(&r);
    const uint32_t run = seed_a_run(&r);

    char q[48];
    (void)snprintf(q, sizeof(q), "run=%u", run);
    CHECK(stream_log(&r, q).status == 200);
    const size_t before = g_sink.len;
    CHECK(before > 100);

    CHECK_EQ_INT(call(&r, KILN_HTTP_DELETE, "/api/log", NULL, NULL).status, 200);
    CHECK(stream_log(&r, q).status == 200);
    CHECK(g_sink.len < before);
}

/* --- 12.2's request-handling rules ------------------------------------- */

/*
 * @relation(SWR-WEB-23, scope=function)
 */
KILN_TEST(swrweb23_state_changing_endpoints_require_authentication)
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
    const kiln_api_req_t nw = {.method = KILN_HTTP_POST, .path = "/api/something/new",
                               .query = nullptr, .body = nullptr, .body_len = 0,
                               .authenticated = false};
    CHECK(kiln_api_needs_auth(&nw));
    const kiln_api_req_t rd = {.method = KILN_HTTP_GET, .path = "/api/anything",
                               .query = nullptr, .body = nullptr, .body_len = 0,
                               .authenticated = false};
    CHECK(!kiln_api_needs_auth(&rd));
}

/*
 * @relation(SWR-NFR-19, scope=function)
 */
KILN_TEST(swrnfr19_an_oversized_body_is_refused_before_it_is_parsed)
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

/*
 * @relation(SWR-WEB-20, scope=function)
 */
KILN_TEST(swrweb20_unknown_routes_and_methods_use_the_error_envelope)
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
    const kiln_api_req_t req = {
        .method = KILN_HTTP_GET, .path = "/api/config", .query = nullptr,
        .body = nullptr, .body_len = 0, .authenticated = true};
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

/*
 * @relation(SWR-WEB-05, scope=function)
 */
KILN_TEST(swrweb05_the_telemetry_event_matches_the_status_shape)
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
    const kiln_api_req_t req = {
        .method = KILN_HTTP_GET, .path = "/api/status", .query = nullptr,
        .body = nullptr, .body_len = 0, .authenticated = true};

    CHECK_ERR(kiln_api_handle(NULL, &req, &resp), KILN_ERR_INVALID_ARG);
    CHECK_ERR(kiln_api_handle(&r.api, NULL, &resp), KILN_ERR_INVALID_ARG);
    CHECK_ERR(kiln_api_handle(&r.api, &req, NULL), KILN_ERR_INVALID_ARG);

    const kiln_api_req_t nopath = {.method = KILN_HTTP_GET, .path = nullptr,
                                   .query = nullptr, .body = nullptr,
                                   .body_len = 0, .authenticated = true};
    CHECK_ERR(kiln_api_handle(&r.api, &nopath, &resp), KILN_ERR_INVALID_ARG);
}

/* ===========================================================================
 * THE EVENT STREAM'S FRAMING  (SWR-WEB-05, tasklist P2)
 * ===========================================================================
 * The payload has been tested since it was written; what was missing was
 * anything to send it over. The transport cannot be host-tested -- it is
 * esp_http_server sockets and a FreeRTOS task -- so the part of SSE that can
 * be got wrong lives here instead: the framing. A frame that is half written,
 * or that carries a newline into a data field, does not fail visibly. It
 * desynchronises the stream for every frame after it, on a browser in a
 * workshop, where nobody is reading a log.
 */

/*
 * @relation(SWR-WEB-05, scope=function)
 */
KILN_TEST(swrweb05_a_frame_is_what_the_browser_is_listening_for)
{
    /* app.js opens an EventSource on /api/events and listens for a named
     * `telemetry` event, so the frame has to carry the name and end with the
     * blank line that completes it. */
    char buf[128];
    const char *data = "{\"kiln_c\":523.5}";
    const size_t n = kiln_api_sse_frame(buf, sizeof(buf), "telemetry",
                                        data, strlen(data));
    CHECK(n > 0);
    CHECK_STR_EQ(buf, "event: telemetry\ndata: {\"kiln_c\":523.5}\n\n");
    CHECK_EQ_UINT(n, strlen(buf));

    /* Without a name it is still a frame, which is what a client using
     * onmessage rather than addEventListener receives. */
    const size_t m = kiln_api_sse_frame(buf, sizeof(buf), nullptr, data, strlen(data));
    CHECK_STR_EQ(buf, "data: {\"kiln_c\":523.5}\n\n");
    CHECK_EQ_UINT(m, strlen(buf));
}

/*
 * @relation(SWR-WEB-05, scope=function)
 */
KILN_TEST(swrweb05_a_frame_that_does_not_fit_is_not_written_at_all)
{
    /* Half an SSE frame is worse than no frame: the client reads the next
     * frame's `event:` line as part of this one's data and never recovers,
     * because there is no resynchronisation in the protocol other than a blank
     * line that has already been consumed. */
    char buf[32];
    memset(buf, 'x', sizeof(buf));
    const char *data = "{\"this\":\"does not fit in thirty-two bytes at all\"}";
    CHECK_EQ_UINT(kiln_api_sse_frame(buf, sizeof(buf), "telemetry",
                                     data, strlen(data)), 0u);
    /* Nothing written: not a prefix, not a terminator. */
    for (size_t i = 0; i < sizeof(buf); i++) { CHECK_EQ_INT(buf[i], 'x'); }

    /* Exactly one byte short still writes nothing. */
    const char *small = "{\"a\":1}";
    const size_t need = strlen("data: ") + strlen(small) + 2u + 1u;
    char tight[64];
    CHECK_EQ_UINT(kiln_api_sse_frame(tight, need - 1u, nullptr, small,
                                     strlen(small)), 0u);
    CHECK_EQ_UINT(kiln_api_sse_frame(tight, need, nullptr, small,
                                     strlen(small)), need - 1u);
}

/*
 * @relation(SWR-WEB-05, scope=function)
 */
KILN_TEST(swrweb05_a_newline_in_the_payload_is_refused_not_passed_on)
{
    /* A newline inside a data field is SSE's own way of writing a multi-line
     * payload, so passing one through would move the frame boundary into the
     * middle of a JSON document. Nothing this device serialises contains one;
     * the check is here because the consequence of being wrong about that is
     * silent and remote. */
    char buf[128];
    const char *lf = "{\"a\":1}\n{\"b\":2}";
    CHECK_EQ_UINT(kiln_api_sse_frame(buf, sizeof(buf), "telemetry", lf, strlen(lf)), 0u);

    const char *cr = "{\"a\":1}\r";
    CHECK_EQ_UINT(kiln_api_sse_frame(buf, sizeof(buf), "telemetry", cr, strlen(cr)), 0u);

    /* And the event name is not a way round it either. */
    CHECK_EQ_UINT(kiln_api_sse_comment(buf, sizeof(buf), "two\nlines"), 0u);
}

/*
 * @relation(SWR-WEB-05, scope=function)
 */
KILN_TEST(swrweb05_the_keepalive_is_a_comment_and_carries_no_data)
{
    /* A proxy or a phone's radio drops an idle TCP connection, and a stream
     * pushing once a second is only idle because the device stopped. The
     * keepalive has to be invisible to the client's event handlers, which in
     * SSE means a comment line. */
    char buf[32];
    const size_t n = kiln_api_sse_comment(buf, sizeof(buf), "ping");
    CHECK(n > 0);
    CHECK_STR_EQ(buf, ": ping\n\n");
    CHECK_EQ_UINT(n, strlen(buf));
    CHECK_EQ_INT(buf[0], ':');          /* not "data:", so no event fires */
}

/*
 * @relation(SWR-WEB-05, scope=function)
 */
KILN_TEST(swrweb05_the_real_telemetry_payload_frames_cleanly)
{
    /* The two halves together, against the payload the device actually
     * produces rather than a short literal: this is what proves the status
     * JSON has no newline in it, which is the assumption the refusal above
     * rests on. */
    static rig_t r;
    rig_init(&r);
    rig_run(&r, 2.0);

    char payload[1024];
    const size_t plen = kiln_api_telemetry_event(&r.api, payload, sizeof(payload));
    CHECK(plen > 0);

    char frame[2048];
    const size_t flen = kiln_api_sse_frame(frame, sizeof(frame), "telemetry",
                                           payload, plen);
    CHECK_MSG(flen > 0, "the device's own telemetry does not frame: %u bytes",
              (unsigned)plen);

    /* The frame ends the way SSE requires and nowhere else. */
    CHECK_EQ_INT(frame[flen - 1], '\n');
    CHECK_EQ_INT(frame[flen - 2], '\n');
    CHECK_EQ_UINT(flen, strlen(frame));

    /* And the data line still parses as the status object, so the UI's one
     * parser is one parser. */
    const char *p = strstr(frame, "data: ");
    CHECK(p != nullptr);
    if (p == nullptr) {
        /* CHECK records and carries on, so without this the next line
         * dereferences the null it just reported and takes the runner with it,
         * turning a failed assertion into a crash with no summary. */
        return;
    }
    p += 6;
    kiln_json_tok_t t[RTOKS];
    const int tn = kiln_json_parse(p, strlen(p) - 2u, t, RTOKS);
    CHECK(tn > 0);
    CHECK(kiln_json_find(p, t, tn, 0, "kiln_c") > 0);

    /* The transport's buffer is 2 kB and the frame has to fit it with the
     * event line on top, which is the sizing this test exists to pin. */
    CHECK_MSG(flen < 2048u, "the frame is %u bytes and httpd's SSE buffer is "
                            "2048", (unsigned)flen);
}

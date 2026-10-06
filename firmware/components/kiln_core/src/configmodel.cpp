/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <string.h>
#include "kiln_core/configmodel.h"
#include "kiln_core/logrec.h"      /* kiln_crc16 */

/* --- enum name tables -------------------------------------------------- */

static const char *const k_recovery_names[] = { "abort", "resume" };
static const char *const k_wifi_names[]     = { "sta", "ap", "sta_ap_fallback" };
static const char *const k_units_names[]    = { "C", "F" };
static const char *const k_lang_names[]     = { "en", "de" };
static const char *const k_tc_names[]       = { "B", "E", "J", "K", "N", "R", "S", "T" };
static const char *const k_rule_names[]     = { "ziegler-nichols", "tyreus-luyben" };

/* --- the table (FR-CFG-01, FR-CFG-02) ---------------------------------- */

#define OFF(field) ((uint16_t)offsetof(kiln_config_t, field))

#define NUM(k, u, r, t, field, lo, hi, d, fl) \
    { .key = (k), .unit = (u), .req = (r), .type = (t), .offset = OFF(field), \
      .str_cap = 0, .min = (lo), .max = (hi), .def = (d), .def_str = NULL, \
      .enum_names = NULL, .enum_count = 0, .flags = (fl) }

#define ENUMI(k, r, field, names, n, d, fl) \
    { .key = (k), .unit = "", .req = (r), .type = KILN_CFG_T_ENUM, .offset = OFF(field), \
      .str_cap = 0, .min = 0, .max = (double)((n) - 1), .def = (d), .def_str = NULL, \
      .enum_names = (names), .enum_count = (n), .flags = (fl) }

#define STR(k, r, field, cap, d, fl) \
    { .key = (k), .unit = "", .req = (r), .type = KILN_CFG_T_STRING, .offset = OFF(field), \
      .str_cap = (cap), .min = 0, .max = 0, .def = 0, .def_str = (d), \
      .enum_names = NULL, .enum_count = 0, .flags = (fl) }

#define SAFE   KILN_CFG_F_SAFETY
#define LOCKED KILN_CFG_F_LOCKED_RUNNING
#define BOOTR  KILN_CFG_F_REBOOT
#define SECRET KILN_CFG_F_SECRET

static const kiln_cfg_item_t k_items[] = {
    /* safety -- every one of these is SAFE, so FR-CFG-08 refuses it mid-run */
    NUM("safety.max_temp_c",          "degC",  "SR-09,SR-23", KILN_CFG_T_FLOAT, max_temp_c,              0,    KILN_TEMP_CEILING_C, 1280, SAFE),
    NUM("safety.max_case_temp_c",     "degC",  "SR-11",       KILN_CFG_T_FLOAT, max_case_temp_c,        40,    90,       70,    SAFE),
    NUM("safety.overtemp_margin_c",   "degC",  "SR-09",       KILN_CFG_T_FLOAT, overtemp_margin_c,       1,    50,       10,    SAFE),
    NUM("safety.runaway_duty",        "permille","SR-07",     KILN_CFG_T_U16,   runaway_duty_permille, 100,  1000,      800,    SAFE),
    NUM("safety.runaway_window_s",    "s",     "SR-07",       KILN_CFG_T_FLOAT, runaway_window_s,       60,  3600,      900,    SAFE),
    NUM("safety.runaway_min_rate",    "degC/h","SR-07",       KILN_CFG_T_FLOAT, runaway_min_rate_c_per_h, 1,  200,       10,    SAFE),
    ENUMI("safety.recovery_policy",            "FR-RUN-08",   recovery_policy, k_recovery_names, KILN_RECOVERY_COUNT, KILN_RECOVERY_ABORT, SAFE),
    NUM("safety.recovery_max_outage_min","min","FR-RUN-08",   KILN_CFG_T_U16,   recovery_max_outage_min, 1,  120,       15,    SAFE),
    NUM("safety.recovery_band_c",     "degC",  "FR-RUN-08",   KILN_CFG_T_FLOAT, recovery_band_c,         5,   200,      50,    SAFE),

    /* control */
    NUM("control.kp",                 "%/degC","FR-CTL-03",   KILN_CFG_T_FLOAT, kp,                      0,   100,       2.0, LOCKED),
    NUM("control.ki",                 "%/degC.s","FR-CTL-03", KILN_CFG_T_FLOAT, ki,                      0,    10,       0.01,LOCKED),
    NUM("control.kd",                 "%.s/degC","FR-CTL-03", KILN_CFG_T_FLOAT, kd,                      0, 10000,      30.0,LOCKED),
    NUM("control.loop_period_ms",     "ms",    "NFR-01",      KILN_CFG_T_U16,   loop_period_ms,        100,  5000,     1000, BOOTR | LOCKED),
    NUM("control.window_ms",          "ms",    "FR-CTL-07",   KILN_CFG_T_U32,   window_ms,             500, 30000,     2000, LOCKED),
    NUM("control.min_on_ms",          "ms",    "FR-CTL-08",   KILN_CFG_T_U32,   min_on_ms,               0,  2000,      100, LOCKED),
    NUM("control.min_off_ms",         "ms",    "FR-CTL-08",   KILN_CFG_T_U32,   min_off_ms,              0,  2000,      100, LOCKED),
    NUM("control.duty_max",           "permille","FR-CTL-16", KILN_CFG_T_U16,   duty_max_permille,     100,  1000,     1000, LOCKED),
    NUM("control.holdback_band_c",    "degC",  "FR-CTL-11",   KILN_CFG_T_FLOAT, holdback_band_c,         0,   200,       25,  0),
    NUM("control.dwell_tol_c",        "degC",  "FR-CTL-12",   KILN_CFG_T_FLOAT, dwell_tol_c,           0.5,    50,        5,  0),

    /* sensing */
    ENUMI("sense.tc_type",                     "FR-ACQ-02",   tc_type,      k_tc_names, KILN_TC_TYPE_COUNT, KILN_TC_TYPE_K, SAFE | BOOTR),
    ENUMI("sense.case_tc_type",                "FR-ACQ-02",   case_tc_type, k_tc_names, KILN_TC_TYPE_COUNT, KILN_TC_TYPE_K, SAFE | BOOTR),
    NUM("sense.line_filter_hz",       "Hz",    "FR-ACQ-06",   KILN_CFG_T_U16,   line_filter_hz,         50,    60,       50,  LOCKED),
    NUM("sense.filter_tau_s",         "s",     "FR-ACQ-07",   KILN_CFG_T_FLOAT, filter_tau_s,            0,    30,        2,  0),
    NUM("sense.cal_offset_c",         "degC",  "FR-ACQ-08",   KILN_CFG_T_FLOAT, cal_offset_c,          -50,    50,        0,  SAFE),
    NUM("sense.cal_gain",             "",      "FR-ACQ-08",   KILN_CFG_T_FLOAT, cal_gain,             0.90,  1.10,        1,  SAFE),
    NUM("sense.case_cal_offset_c",    "degC",  "FR-ACQ-08",   KILN_CFG_T_FLOAT, case_cal_offset_c,     -50,    50,        0,  SAFE),
    NUM("sense.case_cal_gain",        "",      "FR-ACQ-08",   KILN_CFG_T_FLOAT, case_cal_gain,        0.90,  1.10,        1,  SAFE),
    NUM("sense.rate_window_s",        "s",     "FR-ACQ-11",   KILN_CFG_T_U16,   rate_window_s,          10,   300,      120, 0),
    NUM("sense.case_present",         "",      "SR-11",       KILN_CFG_T_BOOL,  case_present,            0,     1,        1, SAFE | BOOTR),

    /* tuning */
    NUM("tune.setpoint_c",            "degC",  "FR-TUN-03",   KILN_CFG_T_FLOAT, tune_setpoint_c,       100,  KILN_TEMP_CEILING_C, 600, LOCKED),
    NUM("tune.amplitude",             "permille","FR-TUN-04", KILN_CFG_T_U16,   tune_amplitude_permille,100, 1000,      500, LOCKED),
    NUM("tune.hysteresis_c",          "degC",  "FR-TUN-04",   KILN_CFG_T_FLOAT, tune_hysteresis_c,     0.1,    20,        1, LOCKED),
    NUM("tune.peak_threshold_c",      "degC",  "FR-TUN-05",   KILN_CFG_T_FLOAT, tune_peak_threshold_c, 0.05,   20,     1.00, LOCKED),
    ENUMI("tune.rule",                         "FR-TUN-06",   tune_rule, k_rule_names, 2, 1 /* tyreus-luyben */, LOCKED),
    NUM("tune.timeout_s",             "s",     "FR-TUN-07",   KILN_CFG_T_FLOAT, tune_timeout_s,        600, 28800,     7200, LOCKED),
    STR("tune.gain_set",                       "FR-TUN-13",   gain_set_name, KILN_CFG_GAINSET_LEN, "default", 0),

    /* current -- FR-CUR.  Safety-relevant: these set the thresholds SR-25..SR-30
     * trip on, and changing one mid-firing changes what the supervisor believes. */
    NUM("current.enabled",            "",      "FR-CUR-12",   KILN_CFG_T_BOOL,  current_enabled,         0,     1,        1, SAFE | BOOTR),
    NUM("current.ct_a_per_v",         "A/V",   "FR-CUR-06",   KILN_CFG_T_FLOAT, ct_a_per_v,              1,  1000,      120, SAFE),
    NUM("current.cal_gain",           "",      "FR-CUR-06",   KILN_CFG_T_FLOAT, current_cal_gain,     0.50,  2.00,        1, SAFE),
    NUM("current.zero_offset_a",      "A",     "FR-CUR-06",   KILN_CFG_T_FLOAT, current_zero_offset_a,  -5,     5,        0, SAFE),
    NUM("current.nominal_a",          "A",     "FR-CUR-02",   KILN_CFG_T_FLOAT, nominal_a,               1,    60,       30, SAFE),
    NUM("current.mains_v",            "V",     "FR-CUR-07",   KILN_CFG_T_FLOAT, mains_v,                90,   480,      230, 0),
    NUM("current.mains_hz",           "Hz",    "FR-CUR-03",   KILN_CFG_T_U16,   mains_hz,               50,    60,       50, SAFE | BOOTR),
    NUM("current.sample_rate_hz",     "Hz",    "FR-CUR-03",   KILN_CFG_T_U32,   current_sample_rate_hz,1000, 20000,    4000, SAFE | BOOTR),
    NUM("current.settle_ms",          "ms",    "FR-CUR-04",   KILN_CFG_T_U16,   current_settle_ms,       0,   200,       20, SAFE),
    NUM("current.fail_on_threshold_a","A",     "SR-25",       KILN_CFG_T_FLOAT, fail_on_threshold_a,   0.1,    10,      0.5, SAFE),
    NUM("current.fail_on_windows",    "",      "SR-25",       KILN_CFG_T_U16,   fail_on_windows,         1,    10,        2, SAFE),
    NUM("current.fail_off_fraction",  "",      "SR-26",       KILN_CFG_T_FLOAT, fail_off_fraction,    0.05,  0.90,     0.20, SAFE),
    NUM("current.fail_off_window_s",  "s",     "SR-26",       KILN_CFG_T_FLOAT, fail_off_window_s,       5,   300,       30, SAFE),
    NUM("current.weld_wait_s",        "s",     "SR-27",       KILN_CFG_T_FLOAT, weld_wait_s,           0.5,     5,        2, SAFE),
    NUM("current.deviation_warn",     "",      "SR-28",       KILN_CFG_T_FLOAT, deviation_warn_frac,  0.02,  0.50,     0.10, SAFE),
    NUM("current.deviation_fault",    "",      "SR-28",       KILN_CFG_T_FLOAT, deviation_fault_frac, 0.05,  0.90,     0.25, SAFE),
    NUM("current.overcurrent_a",      "A",     "SR-29",       KILN_CFG_T_FLOAT, overcurrent_a,           1,   100,       36, SAFE),
    NUM("current.element_tc_per_c",   "1/degC","SR-28,OQ-07", KILN_CFG_T_FLOAT, element_tc_per_c,        0,  0.01,        0, SAFE),
    NUM("current.contactor_life_ops", "",      "SR-30",       KILN_CFG_T_U32,   contactor_life_ops,   1000, 10000000, 100000, 0),
    NUM("current.ssr_life_ops",       "",      "SR-30",       KILN_CFG_T_U32,   ssr_life_ops,         1000, 2000000000, 10000000, 0),

    /* logging */
    NUM("log.interval_s",             "s",     "FR-LOG-03",   KILN_CFG_T_U16,   log_interval_s,          1,   600,       10, 0),

    /* HMI */
    ENUMI("hmi.units",                         "FR-HMI-13",   units, k_units_names, KILN_UNITS_COUNT, KILN_UNITS_C, 0),
    ENUMI("hmi.language",                      "NFR-23",      language, k_lang_names, KILN_LANG_COUNT, KILN_LANG_EN, 0),
    NUM("hmi.dim_timeout_s",          "s",     "FR-HMI-12",   KILN_CFG_T_U16,   dim_timeout_s,           0,  3600,       60, 0),
    NUM("hmi.alarm_duration_s",       "s",     "FR-RUN-06",   KILN_CFG_T_U16,   alarm_duration_s,        0,   600,       30, 0),

    /* network */
    ENUMI("net.wifi_mode",                     "FR-NET-02",   wifi_mode, k_wifi_names, KILN_WIFI_MODE_COUNT, KILN_WIFI_STA_AP_FALLBACK, BOOTR),
    STR("net.wifi_ssid",                       "FR-NET-01",   wifi_ssid, KILN_CFG_STR_LEN,  "",            BOOTR),
    STR("net.wifi_pass",                       "FR-NET-01",   wifi_pass, KILN_CFG_PASS_LEN, "",            BOOTR | SECRET),
    STR("net.ap_ssid",                         "FR-NET-02",   ap_ssid,   KILN_CFG_STR_LEN,  "kilncontrol", BOOTR),
    STR("net.ap_pass",                         "FR-NET-02",   ap_pass,   KILN_CFG_PASS_LEN, "",            BOOTR | SECRET),
    STR("net.hostname",                        "FR-NET-04",   hostname,  KILN_CFG_HOST_LEN, "kilncontrol", BOOTR),
    STR("net.ntp_server",                      "FR-LOG-12",   ntp_server,KILN_CFG_HOST_LEN, "pool.ntp.org", 0),
    STR("net.timezone",                        "FR-LOG-12",   timezone,  KILN_CFG_HOST_LEN, "UTC0",         0),

    /* security */
    STR("security.web_password",               "FR-CFG-07",   web_password, KILN_CFG_PASS_LEN, "", SECRET),
};

uint16_t kiln_config_item_count(void)
{
    return (uint16_t)(sizeof(k_items) / sizeof(k_items[0]));
}

const kiln_cfg_item_t *kiln_config_item(uint16_t index)
{
    return index < kiln_config_item_count() ? &k_items[index] : NULL;
}

const kiln_cfg_item_t *kiln_config_find(const char *key)
{
    if (key == nullptr) {
        return NULL;
    }
    for (uint16_t i = 0; i < kiln_config_item_count(); i++) {
        if (strcmp(k_items[i].key, key) == 0) {
            return &k_items[i];
        }
    }
    return NULL;
}

/* --- typed access ------------------------------------------------------- */

static void       *field_of(kiln_config_t *c, const kiln_cfg_item_t *it)
{
    return (void *)((char *)c + it->offset);
}
static const void *cfield_of(const kiln_config_t *c, const kiln_cfg_item_t *it)
{
    return (const void *)((const char *)c + it->offset);
}

kiln_err_t kiln_config_get_num(const kiln_config_t *cfg, const kiln_cfg_item_t *it,
                               double *out)
{
    if ((cfg == nullptr) || (it == nullptr) || (out == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    const void *f = cfield_of(cfg, it);
    switch (it->type) {
    case KILN_CFG_T_BOOL:  *out = *(const bool *)f ? 1.0 : 0.0;       return KILN_OK;
    case KILN_CFG_T_ENUM:  *out = (double)*(const uint8_t *)f;        return KILN_OK;
    case KILN_CFG_T_U16:   *out = (double)*(const uint16_t *)f;       return KILN_OK;
    case KILN_CFG_T_U32:   *out = (double)*(const uint32_t *)f;       return KILN_OK;
    case KILN_CFG_T_FLOAT: *out = (double)*(const float *)f;          return KILN_OK;
    case KILN_CFG_T_STRING:
    default:               return KILN_ERR_UNSUPPORTED;
    }
}

kiln_err_t kiln_config_set_num(kiln_config_t *cfg, const kiln_cfg_item_t *it,
                               double value)
{
    if ((cfg == nullptr) || (it == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    if (it->type == KILN_CFG_T_STRING) {
        return KILN_ERR_UNSUPPORTED;
    }

    /* FR-CFG-03: the declared range is the only gate, and it is applied here so
     * that no caller can get a value in by another door. */
    if (value < it->min || value > it->max) {
        return KILN_ERR_RANGE;
    }

    void *f = field_of(cfg, it);
    switch (it->type) {
    case KILN_CFG_T_BOOL:  *(bool *)f     = (value != 0.0);           return KILN_OK;
    case KILN_CFG_T_ENUM:  *(uint8_t *)f  = (uint8_t)value;           return KILN_OK;
    case KILN_CFG_T_U16:   *(uint16_t *)f = (uint16_t)value;          return KILN_OK;
    case KILN_CFG_T_U32:   *(uint32_t *)f = (uint32_t)value;          return KILN_OK;
    case KILN_CFG_T_FLOAT: *(float *)f    = (float)value;             return KILN_OK;
    default:               return KILN_ERR_UNSUPPORTED;
    }
}

kiln_err_t kiln_config_get_str(const kiln_config_t *cfg, const kiln_cfg_item_t *it,
                               const char **out)
{
    if ((cfg == nullptr) || (it == nullptr) || (out == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    if (it->type != KILN_CFG_T_STRING) {
        return KILN_ERR_UNSUPPORTED;
    }
    *out = (const char *)cfield_of(cfg, it);
    return KILN_OK;
}

kiln_err_t kiln_config_set_str(kiln_config_t *cfg, const kiln_cfg_item_t *it,
                               const char *value)
{
    if ((cfg == nullptr) || (it == nullptr) || (value == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    if (it->type != KILN_CFG_T_STRING) {
        return KILN_ERR_UNSUPPORTED;
    }

    const size_t n = strlen(value);
    if (n >= it->str_cap) {
        return KILN_ERR_RANGE; /* truncation is not an answer */
    }

    char *f = (char *)field_of(cfg, it);
    memset(f, 0, it->str_cap);
    memcpy(f, value, n);
    return KILN_OK;
}

/* --- defaults, validation, application ---------------------------------- */

void kiln_config_defaults(kiln_config_t *cfg)
{
    if (cfg == nullptr) {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->schema_version = KILN_CFG_SCHEMA_VERSION;

    for (uint16_t i = 0; i < kiln_config_item_count(); i++) {
        const kiln_cfg_item_t *it = &k_items[i];
        if (it->type == KILN_CFG_T_STRING) {
            (void)kiln_config_set_str(cfg, it, (it->def_str != nullptr) ? it->def_str : "");
        } else {
            (void)kiln_config_set_num(cfg, it, it->def);
        }
    }
}

kiln_err_t kiln_config_validate(const kiln_config_t *cfg, const kiln_cfg_item_t **bad)
{
    if (bad != nullptr) {
        *bad = NULL;
    }
    if (cfg == nullptr) {
        return KILN_ERR_INVALID_ARG;
    }

    if (cfg->schema_version != KILN_CFG_SCHEMA_VERSION) {
        return KILN_ERR_UNSUPPORTED;
    }

    for (uint16_t i = 0; i < kiln_config_item_count(); i++) {
        const kiln_cfg_item_t *it = &k_items[i];

        if (it->type == KILN_CFG_T_STRING) {
            /* NFR-19: a fixed array that arrived over the network is only a
             * string if it contains a NUL. */
            const char *s = (const char *)cfield_of(cfg, it);
            bool term = false;
            for (uint16_t k = 0; k < it->str_cap; k++) {
                if (s[k] == '\0') { term = true; break; }
            }
            if (!term) {
                if (bad != nullptr) {
                    *bad = it;
                }
                return KILN_ERR_RANGE;
            }
            continue;
        }

        double v = 0.0;
        if (kiln_config_get_num(cfg, it, &v) != KILN_OK) {
            if (bad != nullptr) {
                *bad = it;
            }
            return KILN_ERR_INVALID_ARG;
        }
        if (v < it->min || v > it->max) {
            if (bad != nullptr) {
                *bad = it;
            }
            return KILN_ERR_RANGE;
        }
    }
    return KILN_OK;
}

/* Does this item differ between the two configurations? */
static bool item_differs(const kiln_config_t *a, const kiln_config_t *b,
                         const kiln_cfg_item_t *it)
{
    if (it->type == KILN_CFG_T_STRING) {
        return memcmp(cfield_of(a, it), cfield_of(b, it), it->str_cap) != 0;
    }
    double va = 0.0, vb = 0.0;
    (void)kiln_config_get_num(a, it, &va);
    (void)kiln_config_get_num(b, it, &vb);
    return va != vb;
}

kiln_err_t kiln_config_apply(kiln_config_t *cfg, const kiln_config_t *incoming,
                             bool running, const kiln_cfg_item_t **bad)
{
    if (bad != nullptr) {
        *bad = NULL;
    }
    if ((cfg == nullptr) || (incoming == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    /* FR-CFG-03: validate the whole thing before touching anything. */
    const kiln_err_t e = kiln_config_validate(incoming, bad);
    if (e != KILN_OK) {
        return e;
    }

    /* FR-CFG-08: safety-relevant items are refused mid-run.  Only *changes* are
     * refused, so a client that sends the whole configuration back with one
     * unrelated edit is not rejected for the items it left alone -- which is how
     * every real API client behaves. */
    if (running) {
        for (uint16_t i = 0; i < kiln_config_item_count(); i++) {
            const kiln_cfg_item_t *it = &k_items[i];
            if ((it->flags & (KILN_CFG_F_SAFETY | KILN_CFG_F_LOCKED_RUNNING)) == 0u) {
                continue;
            }
            if (item_differs(cfg, incoming, it)) {
                if (bad != nullptr) {
                    *bad = it;
                }
                return KILN_ERR_STATE;
            }
        }
    }

    *cfg = *incoming;
    cfg->schema_version = KILN_CFG_SCHEMA_VERSION;
    return KILN_OK;
}

bool kiln_config_reboot_required(const kiln_config_t *a, const kiln_config_t *b)
{
    if ((a == nullptr) || (b == nullptr)) {
        return false;
    }
    for (uint16_t i = 0; i < kiln_config_item_count(); i++) {
        const kiln_cfg_item_t *it = &k_items[i];
        if (((it->flags & KILN_CFG_F_REBOOT) != 0u) && item_differs(a, b, it)) {
            return true;
        }
    }
    return false;
}

/* --- persistence -------------------------------------------------------- */

/* Fixed-layout blob: magic, schema version, payload size, payload, CRC-16.
 * Carrying the size means a migration from an older schema is a memcpy of what
 * the old struct had plus defaults for the rest, with no parsing involved. */
typedef struct {
    uint32_t magic;
    uint16_t schema_version;
    uint16_t payload_bytes;
} cfg_blob_hdr_t;

#define CFG_HDR_BYTES 8u
#define CFG_CRC_BYTES 2u

size_t kiln_config_blob_size(void)
{
    return CFG_HDR_BYTES + sizeof(kiln_config_t) + CFG_CRC_BYTES;
}

static void put_u16le(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t get_u16le(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

kiln_err_t kiln_config_encode(const kiln_config_t *cfg, void *out, size_t cap, size_t *len)
{
    if ((cfg == nullptr) || (out == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    if (cap < kiln_config_blob_size()) {
        return KILN_ERR_NO_SPACE;
    }

    uint8_t *p = (uint8_t *)out;
    put_u32le(&p[0], KILN_CFG_BLOB_MAGIC);
    put_u16le(&p[4], KILN_CFG_SCHEMA_VERSION);
    put_u16le(&p[6], (uint16_t)sizeof(kiln_config_t));
    memcpy(&p[CFG_HDR_BYTES], cfg, sizeof(kiln_config_t));

    const size_t crc_at = CFG_HDR_BYTES + sizeof(kiln_config_t);
    put_u16le(&p[crc_at], kiln_crc16(p, crc_at));

    if (len != nullptr) {
        *len = kiln_config_blob_size();
    }
    return KILN_OK;
}

kiln_err_t kiln_config_decode(const void *blob, size_t len, kiln_config_t *cfg)
{
    if (cfg == nullptr) {
        return KILN_ERR_INVALID_ARG;
    }

    /* There is no state in which the controller may run without a configuration,
     * so every failure path below still leaves a usable one behind. */
    kiln_config_defaults(cfg);

    if ((blob == nullptr) || len < CFG_HDR_BYTES + CFG_CRC_BYTES) {
        return KILN_ERR_CORRUPT;
    }

    const uint8_t *p = (const uint8_t *)blob;
    if (get_u32le(&p[0]) != KILN_CFG_BLOB_MAGIC) {
        return KILN_ERR_CORRUPT;
    }

    const uint16_t ver     = get_u16le(&p[4]);
    const uint16_t payload = get_u16le(&p[6]);

    if ((size_t)CFG_HDR_BYTES + payload + CFG_CRC_BYTES > len) {
        return KILN_ERR_CORRUPT;
    }

    const size_t crc_at = CFG_HDR_BYTES + payload;
    if (kiln_crc16(p, crc_at) != get_u16le(&p[crc_at])) {
        return KILN_ERR_CORRUPT;
    }

    /* FR-CFG-05: a schema from the future cannot be interpreted -- a field could
     * have changed meaning, not merely been added -- so defaults plus a warning. */
    if (ver > KILN_CFG_SCHEMA_VERSION) {
        return KILN_ERR_CORRUPT;
    }

    /* Equal or older: take what the stored payload holds, leave the rest at the
     * defaults already in place.  That is exactly "migrate, filling new items
     * with defaults". */
    const size_t take = payload < sizeof(kiln_config_t) ? payload : sizeof(kiln_config_t);
    memcpy(cfg, &p[CFG_HDR_BYTES], take);
    cfg->schema_version = KILN_CFG_SCHEMA_VERSION;

    /* A blob that passed its CRC can still hold a value outside a range this
     * firmware tightened.  Clamp the offenders back rather than discard the whole
     * configuration, and report the migration so the caller writes it back. */
    bool repaired = false;
    for (uint16_t i = 0; i < kiln_config_item_count(); i++) {
        const kiln_cfg_item_t *it = &k_items[i];
        if (it->type == KILN_CFG_T_STRING) {
            char *s = (char *)field_of(cfg, it);
            s[it->str_cap - 1] = '\0';
            continue;
        }
        double v = 0.0;
        if (kiln_config_get_num(cfg, it, &v) != KILN_OK) {
            continue;
        }
        if (v < it->min || v > it->max) {
            (void)kiln_config_set_num(cfg, it, it->def);
            repaired = true;
        }
    }

    if (ver != KILN_CFG_SCHEMA_VERSION || payload != sizeof(kiln_config_t) || repaired) {
        return KILN_ERR_UNSUPPORTED;   /* migrated: caller should persist it */
    }
    return KILN_OK;
}

/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Configuration schema -- FR-CFG-01..FR-CFG-08, architecture section 11.
 *
 * One static table, one row per item, carrying key, type, unit, range, default
 * and flags.  Everything else is *derived* from it: NVS persistence, the JSON
 * projection for the API, range validation, the web form and the documentation
 * table.  Adding a setting is adding a row -- which is the only way a project
 * with this many knobs keeps the API, the UI and the docs agreeing with the code.
 *
 * The items are addressed by byte offset into kiln_config_t rather than by a
 * parallel enum, so a row and its field cannot drift apart: a mismatched type is
 * a compile error at the row, and the host suite walks the whole table checking
 * that every field is reachable and that every default is inside its own range.
 */
#ifndef KILN_CORE_CONFIGMODEL_H
#define KILN_CORE_CONFIGMODEL_H

#include <stddef.h>
#include "kiln/err.h"
#include "kiln/types.h"

#define KILN_CFG_SCHEMA_VERSION 1

#define KILN_CFG_STR_LEN     33       /* WiFi SSID is 32 + NUL */
#define KILN_CFG_PASS_LEN    65
#define KILN_CFG_HOST_LEN    32
#define KILN_CFG_GAINSET_LEN 24

typedef enum {
    KILN_CFG_T_BOOL = 0,
    KILN_CFG_T_U16,
    KILN_CFG_T_U32,
    KILN_CFG_T_FLOAT,
    KILN_CFG_T_ENUM,      /* stored as uint8_t, named by enum_names */
    KILN_CFG_T_STRING,
} kiln_cfg_type_t;

/* FR-CFG-07: never serialised outward; the API reports only "set": true|false. */
#define KILN_CFG_F_SECRET            (1u << 0)
/* FR-CFG-04: takes effect only after a restart. */
#define KILN_CFG_F_REBOOT            (1u << 1)
/* FR-CFG-08: safety-relevant, so refused while a run is in progress. */
#define KILN_CFG_F_SAFETY            (1u << 2)
/* Not safety-relevant, but meaningless to change mid-run. */
#define KILN_CFG_F_LOCKED_RUNNING    (1u << 3)

typedef struct {
    const char            *key;        /* stable API and NVS name            */
    const char            *unit;       /* "degC", "A", "ms", "" ...           */
    const char            *req;        /* the requirement it serves (TR-22)   */
    kiln_cfg_type_t        type;
    uint16_t               offset;     /* byte offset into kiln_config_t      */
    uint16_t               str_cap;    /* KILN_CFG_T_STRING only              */
    double                 min, max;   /* numeric range, inclusive            */
    double                 def;        /* numeric default                     */
    const char            *def_str;    /* string default                      */
    const char *const     *enum_names;
    uint8_t                enum_count;
    uint8_t                flags;
} kiln_cfg_item_t;

/* --- the configuration itself (FR-CFG-02) ------------------------------- */

typedef enum {
    KILN_RECOVERY_ABORT = 0,       /* FR-RUN-08 default */
    KILN_RECOVERY_RESUME,
    KILN_RECOVERY_COUNT,
} kiln_recovery_policy_t;

typedef enum {
    KILN_WIFI_STA = 0,
    KILN_WIFI_AP,
    KILN_WIFI_STA_AP_FALLBACK,     /* FR-NET-02 */
    KILN_WIFI_MODE_COUNT,
} kiln_wifi_mode_t;

typedef enum {
    KILN_UNITS_C = 0,
    KILN_UNITS_F,                  /* FR-HMI-13: display only */
    KILN_UNITS_COUNT,
} kiln_units_t;

typedef struct {
    uint16_t schema_version;

    /* safety */
    float    max_temp_c;
    float    max_case_temp_c;
    float    overtemp_margin_c;
    uint16_t runaway_duty_permille;
    float    runaway_window_s;
    float    runaway_min_rate_c_per_h;
    uint8_t  recovery_policy;            /* kiln_recovery_policy_t */
    uint16_t recovery_max_outage_min;
    float    recovery_band_c;

    /* control */
    float    kp, ki, kd;
    uint16_t loop_period_ms;
    uint32_t window_ms;
    uint32_t min_on_ms;
    uint32_t min_off_ms;
    uint16_t duty_max_permille;
    float    holdback_band_c;
    float    dwell_tol_c;

    /* sensing */
    uint8_t  tc_type;                    /* kiln_tc_type_t */
    uint8_t  case_tc_type;
    uint16_t line_filter_hz;
    float    filter_tau_s;
    float    cal_offset_c;
    float    cal_gain;
    float    case_cal_offset_c;
    float    case_cal_gain;
    uint16_t rate_window_s;
    bool     case_present;

    /* tuning */
    float    tune_setpoint_c;
    uint16_t tune_amplitude_permille;
    float    tune_hysteresis_c;
    float    tune_peak_threshold_c;
    uint8_t  tune_rule;                  /* kiln_tune_rule_t */
    float    tune_timeout_s;
    char     gain_set_name[KILN_CFG_GAINSET_LEN];

    /* current (FR-CUR) */
    bool     current_enabled;
    float    ct_a_per_v;
    float    current_cal_gain;
    float    current_zero_offset_a;
    float    nominal_a;
    float    mains_v;
    uint16_t mains_hz;
    uint32_t current_sample_rate_hz;
    uint16_t current_settle_ms;
    float    fail_on_threshold_a;
    uint16_t fail_on_windows;
    float    fail_off_fraction;
    float    fail_off_window_s;
    float    weld_wait_s;
    float    deviation_warn_frac;
    float    deviation_fault_frac;
    float    overcurrent_a;
    float    element_tc_per_c;
    uint32_t contactor_life_ops;
    uint32_t ssr_life_ops;

    /* logging */
    uint16_t log_interval_s;

    /* HMI */
    uint8_t  units;                      /* kiln_units_t */
    uint8_t  language;                   /* kiln_lang_t (NFR-23) */
    uint16_t dim_timeout_s;
    uint16_t alarm_duration_s;

    /* network */
    uint8_t  wifi_mode;                  /* kiln_wifi_mode_t */
    char     wifi_ssid[KILN_CFG_STR_LEN];
    char     wifi_pass[KILN_CFG_PASS_LEN];
    char     ap_ssid[KILN_CFG_STR_LEN];
    char     ap_pass[KILN_CFG_PASS_LEN];
    char     hostname[KILN_CFG_HOST_LEN];
    char     ntp_server[KILN_CFG_HOST_LEN];
    char     timezone[KILN_CFG_HOST_LEN];

    /* security */
    char     web_password[KILN_CFG_PASS_LEN];
} kiln_config_t;

void kiln_config_defaults(kiln_config_t *cfg);

/* The table. */
uint16_t               kiln_config_item_count(void);
const kiln_cfg_item_t *kiln_config_item(uint16_t index);
const kiln_cfg_item_t *kiln_config_find(const char *key);

/* Typed access by item, for the API and the UI.  Numeric accessors work for
 * BOOL, U16, U32, FLOAT and ENUM; strings use the string pair. */
kiln_err_t kiln_config_get_num(const kiln_config_t *cfg, const kiln_cfg_item_t *it,
                               double *out);
kiln_err_t kiln_config_set_num(kiln_config_t *cfg, const kiln_cfg_item_t *it,
                               double value);
kiln_err_t kiln_config_get_str(const kiln_config_t *cfg, const kiln_cfg_item_t *it,
                               const char **out);
kiln_err_t kiln_config_set_str(kiln_config_t *cfg, const kiln_cfg_item_t *it,
                               const char *value);

/* FR-CFG-03: validate every item.  On failure names the offending item, so the
 * API can report which key was wrong rather than "invalid configuration". */
kiln_err_t kiln_config_validate(const kiln_config_t *cfg, const kiln_cfg_item_t **bad);

/* FR-CFG-03 and FR-CFG-08: apply `incoming` over `cfg` atomically -- the whole
 * write is validated first and rejected whole, so a bad item cannot leave half a
 * configuration applied.  With running true, any item flagged safety-relevant or
 * locked-while-running that actually differs is refused.
 *
 * Returns KILN_ERR_RANGE (with *bad set) for an out-of-range item and
 * KILN_ERR_STATE (with *bad set) for one that may not change right now. */
kiln_err_t kiln_config_apply(kiln_config_t *cfg, const kiln_config_t *incoming,
                             bool running, const kiln_cfg_item_t **bad);

/* FR-CFG-04: does moving from `a` to `b` need a restart? */
bool kiln_config_reboot_required(const kiln_config_t *a, const kiln_config_t *b);

/* --- persistence (FR-CFG-05) -------------------------------------------- */

/* The stored blob is the struct plus a header and a CRC.  Fixed layout, so a
 * migration is a question of version rather than of parsing. */
#define KILN_CFG_BLOB_MAGIC 0x47464343U   /* "CCFG" */

size_t     kiln_config_blob_size(void);
kiln_err_t kiln_config_encode(const kiln_config_t *cfg, void *out, size_t cap, size_t *len);

/* Decode, migrating an older schema by filling unknown items with defaults.
 *
 * Returns KILN_OK for a current blob, KILN_ERR_UNSUPPORTED when it was migrated
 * from an older schema (the caller should write it back), and KILN_ERR_CORRUPT
 * for a bad CRC, a bad magic or a newer schema -- in all three of which cases
 * *cfg is left at defaults and the caller raises warning 20 / fault 20.  Never
 * leaves *cfg in an unusable state, because there is no state in which the
 * controller may run without a configuration. */
kiln_err_t kiln_config_decode(const void *blob, size_t len, kiln_config_t *cfg);

#endif

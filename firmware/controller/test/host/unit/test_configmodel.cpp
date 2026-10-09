/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 * kiln_core/configmodel -- SWR-CFG-01..SWR-CFG-08, SWR-NFR-19.
 */

#include <stddef.h>
#include <string.h>
#include "kiln_check.h"
#include "kiln_core/configmodel.h"
#include "kiln_core/logrec.h"   /* kiln_crc16, to forge a stored blob */

/*
 * @relation(SWR-CFG-01, scope=function)
 */
KILN_TEST(swrcfg01_every_item_is_reachable_and_its_default_is_in_range)
{
    /* The table is the schema: a row whose offset or type is wrong would make
     * every derived thing -- NVS, the API, the web form, the docs -- wrong in the
     * same place, so walking it is the test that matters most here. */
    kiln_config_t cfg;
    kiln_config_defaults(&cfg);

    const uint16_t n = kiln_config_item_count();
    CHECK(n > 40);

    for (uint16_t i = 0; i < n; i++) {
        const kiln_cfg_item_t *it = kiln_config_item(i);
        CHECK(it != NULL);
        CHECK_MSG(it->key && it->key[0], "item %u has no key", i);
        CHECK_MSG(it->req && it->req[0], "item %s traces to no requirement", it->key);
        CHECK_MSG(it->unit != NULL, "item %s has no unit", it->key);

        /* Addressable by key, and the same row. */
        CHECK(kiln_config_find(it->key) == it);

        if (it->type == KILN_CFG_T_STRING) {
            CHECK_MSG(it->str_cap > 1u, "item %s has no capacity", it->key);
            const char *s = NULL;
            CHECK_OK(kiln_config_get_str(&cfg, it, &s));
            CHECK(s != NULL);
            continue;
        }

        CHECK_MSG(it->min <= it->max, "item %s has an inverted range", it->key);

        double v = 0.0;
        CHECK_OK(kiln_config_get_num(&cfg, it, &v));
        CHECK_MSG(v >= it->min && v <= it->max,
                  "item %s default %g is outside [%g, %g]", it->key, v, it->min, it->max);
        /* A float field cannot hold every double default exactly, so compare at
         * the precision the field actually has. */
        const double slack = (it->type == KILN_CFG_T_FLOAT)
                           ? 1e-6 * (it->def < 0.0 ? -it->def : it->def) + 1e-9
                           : 0.0;
        CHECK_MSG(v >= it->def - slack && v <= it->def + slack,
                  "item %s took %g, not its default %g", it->key, v, it->def);
    }
}

/*
 * @relation(SWR-CFG-01, scope=function)
 */
KILN_TEST(swrcfg01_no_two_items_share_a_key_or_an_offset)
{
    const uint16_t n = kiln_config_item_count();
    for (uint16_t i = 0; i < n; i++) {
        for (uint16_t j = (uint16_t)(i + 1u); j < n; j++) {
            const kiln_cfg_item_t *a = kiln_config_item(i);
            const kiln_cfg_item_t *b = kiln_config_item(j);
            CHECK_MSG(strcmp(a->key, b->key) != 0, "duplicate key %s", a->key);
            CHECK_MSG(a->offset != b->offset,
                      "items %s and %s share an offset", a->key, b->key);
        }
    }
}

/*
 * @relation(SWR-CFG-02, scope=function)
 */
KILN_TEST(swrcfg02_the_minimum_item_set_is_present)
{
    /* The list SWR-CFG-02 enumerates, by group. */
    const char *required[] = {
        "safety.max_temp_c", "safety.max_case_temp_c", "safety.runaway_window_s",
        "safety.recovery_policy",
        "control.kp", "control.ki", "control.kd", "control.loop_period_ms",
        "control.window_ms", "control.min_on_ms", "control.min_off_ms",
        "control.duty_max", "control.holdback_band_c", "control.dwell_tol_c",
        "sense.case_tc_type", "sense.line_filter_hz", "sense.filter_tau_s",
        "sense.cal_offset_c", "sense.cal_gain",
        "tune.amplitude", "tune.hysteresis_c", "tune.rule", "tune.timeout_s",
        "current.ct_a_per_v", "current.nominal_a", "current.mains_v",
        "current.fail_on_threshold_a", "current.settle_ms",
        "current.fail_off_fraction", "current.deviation_warn",
        "current.overcurrent_a", "current.contactor_life_ops", "current.enabled",
        "log.interval_s",
        "hmi.units", "hmi.dim_timeout_s", "hmi.alarm_duration_s",
        "net.wifi_ssid", "net.hostname", "net.ntp_server",
        "net.timezone",
    };

    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        CHECK_MSG(kiln_config_find(required[i]) != NULL,
                  "SWR-CFG-02 requires an item %s", required[i]);
    }

    /* And one item SWR-ACQ-02 requires NOT to exist. The chamber front end
     * belongs to the supervisor, which linearises for type K; an item that let
     * this side change that would make the backstop mean something else, and
     * one that was offered and then ignored would be worse, because an
     * operator can set it and believe it. */
    CHECK_MSG(kiln_config_find("sense.tc_type") == NULL,
              "SWR-ACQ-02: the chamber TC type is configurable again");
}

/*
 * @relation(SWR-CFG-03, scope=function)
 */
KILN_TEST(swrcfg03_a_write_outside_the_declared_range_is_refused)
{
    kiln_config_t cfg;
    kiln_config_defaults(&cfg);

    const kiln_cfg_item_t *it = kiln_config_find("safety.max_temp_c");
    CHECK(it != NULL);
    CHECK_ERR(kiln_config_set_num(&cfg, it, 5000.0), KILN_ERR_RANGE);
    CHECK_ERR(kiln_config_set_num(&cfg, it, -1.0), KILN_ERR_RANGE);
    CHECK_OK(kiln_config_set_num(&cfg, it, 1100.0));
    CHECK_NEAR(cfg.max_temp_c, 1100.0f, 0.01f);

    /* SWR-SAF-23's ceiling is the table's maximum for this item. */
    CHECK_NEAR(it->max, (double)KILN_TEMP_CEILING_C, 0.01);
}

/*
 * @relation(SWR-CFG-03, scope=function)
 */
KILN_TEST(swrcfg03_the_whole_write_is_rejected_atomically)
{
    kiln_config_t live, incoming;
    kiln_config_defaults(&live);
    kiln_config_defaults(&incoming);

    /* One good change and one impossible one, in the same write. */
    incoming.holdback_band_c = 40.0f;
    incoming.max_temp_c      = 9000.0f;

    const kiln_cfg_item_t *bad = NULL;
    CHECK_ERR(kiln_config_apply(&live, &incoming, false, &bad), KILN_ERR_RANGE);
    CHECK(bad != NULL);
    CHECK_STR_EQ(bad->key, "safety.max_temp_c");

    /* The good change did not land either, which is what "atomically" means. */
    CHECK_NEAR(live.holdback_band_c, 25.0f, 0.01f);
}

/*
 * @relation(SWR-CFG-08, scope=function)
 */
KILN_TEST(swrcfg08_safety_items_are_refused_while_a_run_is_in_progress)
{
    kiln_config_t live, incoming;
    kiln_config_defaults(&live);

    incoming = live;
    incoming.max_temp_c = 1100.0f;

    const kiln_cfg_item_t *bad = NULL;
    CHECK_ERR(kiln_config_apply(&live, &incoming, true, &bad), KILN_ERR_STATE);
    CHECK(bad != NULL);
    CHECK(bad->flags & KILN_CFG_F_SAFETY);
    CHECK_NEAR(live.max_temp_c, 1280.0f, 0.01f);

    /* Idle, the same write is fine. */
    CHECK_OK(kiln_config_apply(&live, &incoming, false, &bad));
    CHECK_NEAR(live.max_temp_c, 1100.0f, 0.01f);
}

/*
 * @relation(SWR-CFG-08, scope=function)
 */
KILN_TEST(swrcfg08_an_unchanged_safety_item_does_not_block_an_unrelated_edit)
{
    /* Every real API client sends the whole configuration back with one edit in
     * it, so refusing on *presence* rather than on *change* would make nothing
     * editable mid-run. */
    kiln_config_t live, incoming;
    kiln_config_defaults(&live);
    incoming = live;
    incoming.dim_timeout_s = 120;        /* not safety-relevant */

    const kiln_cfg_item_t *bad = NULL;
    CHECK_OK(kiln_config_apply(&live, &incoming, true, &bad));
    CHECK_EQ_UINT(live.dim_timeout_s, 120u);
}

/*
 * @relation(SWR-CFG-04, scope=function)
 */
KILN_TEST(swrcfg04_reboot_required_is_reported_for_the_items_that_need_one)
{
    kiln_config_t a, b;
    kiln_config_defaults(&a);
    b = a;
    CHECK(!kiln_config_reboot_required(&a, &b));

    b.loop_period_ms = 500;              /* flagged reboot-required */
    CHECK(kiln_config_reboot_required(&a, &b));

    b = a;
    b.holdback_band_c = 40.0f;           /* applies live */
    CHECK(!kiln_config_reboot_required(&a, &b));

    b = a;
    CHECK_OK(kiln_config_set_str(&b, kiln_config_find("net.hostname"), "kiln2"));
    CHECK(kiln_config_reboot_required(&a, &b));
}

/*
 * @relation(SWR-CFG-07, scope=function)
 */
KILN_TEST(swrcfg07_secrets_are_flagged_so_they_are_never_serialised_outward)
{
    /* security.web_password was removed on 2026-10-06: SWR-WEB-26 left nothing
     * over the network to authenticate, so there was nothing for it to guard. */
    /* net.ap_pass went with the provisioning access point (SWR-NET-11): the
       device raises no AP, so there is no second passphrase to keep. */
    const char *secrets[] = { "net.wifi_pass" };
    for (size_t i = 0; i < sizeof(secrets) / sizeof(secrets[0]); i++) {
        const kiln_cfg_item_t *it = kiln_config_find(secrets[i]);
        CHECK_MSG(it != NULL, "%s is not in the schema", secrets[i]);
        /* `continue` rather than fall through: CHECK records a failure and
         * keeps going, so dereferencing here would turn a missing item into a
         * segfault and hide which item was missing. */
        if (it == NULL) {
            continue;
        }
        CHECK_MSG(it->flags & KILN_CFG_F_SECRET, "%s is not flagged secret", secrets[i]);
    }
}

/*
 * @relation(SWR-NFR-19, scope=function)
 */
KILN_TEST(swrnfr19_a_string_write_that_would_truncate_is_refused)
{
    kiln_config_t cfg;
    kiln_config_defaults(&cfg);

    const kiln_cfg_item_t *it = kiln_config_find("net.wifi_ssid");
    char too_long[128];
    memset(too_long, 'x', sizeof(too_long));
    too_long[sizeof(too_long) - 1] = '\0';

    /* Silently truncating a WiFi SSID produces a network that cannot be joined
     * and no indication of why. */
    CHECK_ERR(kiln_config_set_str(&cfg, it, too_long), KILN_ERR_RANGE);
    CHECK_OK(kiln_config_set_str(&cfg, it, "my-kiln-network"));

    const char *got = NULL;
    CHECK_OK(kiln_config_get_str(&cfg, it, &got));
    CHECK_STR_EQ(got, "my-kiln-network");
}

/*
 * @relation(SWR-NFR-19, scope=function)
 */
KILN_TEST(swrnfr19_an_unterminated_stored_string_fails_validation)
{
    kiln_config_t cfg;
    kiln_config_defaults(&cfg);
    memset(cfg.hostname, 'h', sizeof(cfg.hostname));

    const kiln_cfg_item_t *bad = NULL;
    CHECK_ERR(kiln_config_validate(&cfg, &bad), KILN_ERR_RANGE);
    CHECK(bad != NULL);
    CHECK_STR_EQ(bad->key, "net.hostname");
}

KILN_TEST(typed_accessors_refuse_the_wrong_type)
{
    kiln_config_t cfg;
    kiln_config_defaults(&cfg);

    const kiln_cfg_item_t *num = kiln_config_find("control.kp");
    const kiln_cfg_item_t *str = kiln_config_find("net.hostname");

    double v = 0.0;
    const char *s = NULL;
    CHECK_ERR(kiln_config_get_num(&cfg, str, &v), KILN_ERR_UNSUPPORTED);
    CHECK_ERR(kiln_config_set_num(&cfg, str, 1.0), KILN_ERR_UNSUPPORTED);
    CHECK_ERR(kiln_config_get_str(&cfg, num, &s), KILN_ERR_UNSUPPORTED);
    CHECK_ERR(kiln_config_set_str(&cfg, num, "x"), KILN_ERR_UNSUPPORTED);

    CHECK_ERR(kiln_config_get_num(NULL, num, &v), KILN_ERR_INVALID_ARG);
    CHECK_ERR(kiln_config_set_num(&cfg, NULL, 1.0), KILN_ERR_INVALID_ARG);
    CHECK(kiln_config_find(NULL) == NULL);
    CHECK(kiln_config_item(60000) == NULL);
}

/* --- persistence, SWR-CFG-05 -------------------------------------------- */

/*
 * @relation(SWR-CFG-05, scope=function)
 */
KILN_TEST(swrcfg05_a_current_blob_round_trips)
{
    kiln_config_t out, in;
    kiln_config_defaults(&in);
    in.max_temp_c = 1150.0f;
    CHECK_OK(kiln_config_set_str(&in, kiln_config_find("net.hostname"), "kiln-a"));

    uint8_t blob[1024];
    size_t len = 0;
    CHECK(kiln_config_blob_size() <= sizeof(blob));
    CHECK_OK(kiln_config_encode(&in, blob, sizeof(blob), &len));
    CHECK_EQ_UINT(len, kiln_config_blob_size());

    CHECK_OK(kiln_config_decode(blob, len, &out));
    CHECK_NEAR(out.max_temp_c, 1150.0f, 0.01f);
    CHECK_STR_EQ(out.hostname, "kiln-a");
}

/*
 * @relation(SWR-CFG-05, scope=function)
 */
KILN_TEST(swrcfg05_a_corrupt_blob_falls_back_to_defaults)
{
    kiln_config_t in, out;
    kiln_config_defaults(&in);
    in.max_temp_c = 1150.0f;

    uint8_t blob[1024];
    size_t len = 0;
    CHECK_OK(kiln_config_encode(&in, blob, sizeof(blob), &len));

    blob[20] ^= 0xFFu;
    CHECK_ERR(kiln_config_decode(blob, len, &out), KILN_ERR_CORRUPT);
    /* There is no state in which the controller may run without a
     * configuration, so a failure still leaves a usable one. */
    CHECK_NEAR(out.max_temp_c, 1280.0f, 0.01f);
    CHECK_OK(kiln_config_validate(&out, NULL));

    /* Bad magic, a truncated blob, and nothing at all. */
    CHECK_OK(kiln_config_encode(&in, blob, sizeof(blob), &len));
    blob[0] ^= 0xFFu;
    CHECK_ERR(kiln_config_decode(blob, len, &out), KILN_ERR_CORRUPT);
    CHECK_ERR(kiln_config_decode(blob, 4, &out), KILN_ERR_CORRUPT);
    CHECK_ERR(kiln_config_decode(NULL, 0, &out), KILN_ERR_CORRUPT);
    CHECK_OK(kiln_config_validate(&out, NULL));
}

/*
 * @relation(SWR-CFG-05, scope=function)
 */
KILN_TEST(swrcfg05_an_older_schema_is_migrated_with_defaults_for_new_items)
{
    kiln_config_t in, out;
    kiln_config_defaults(&in);
    in.max_temp_c = 1150.0f;

    uint8_t blob[1024];
    size_t len = 0;
    CHECK_OK(kiln_config_encode(&in, blob, sizeof(blob), &len));

    /* Forge an older, shorter payload: version 0 holding only the first 64 bytes
     * of the struct, which is what a previous firmware would have written. */
    const uint16_t short_payload = 64;
    blob[4] = 0; blob[5] = 0;
    blob[6] = (uint8_t)short_payload; blob[7] = (uint8_t)(short_payload >> 8u);
    const size_t crc_at = 8u + short_payload;
    const uint16_t crc = kiln_crc16(blob, crc_at);
    blob[crc_at]     = (uint8_t)crc;
    blob[crc_at + 1] = (uint8_t)(crc >> 8u);

    CHECK_ERR(kiln_config_decode(blob, crc_at + 2u, &out), KILN_ERR_UNSUPPORTED);
    /* The stored prefix survived; everything beyond it is at its default. */
    CHECK_NEAR(out.max_temp_c, 1150.0f, 0.01f);
    CHECK_EQ_UINT(out.schema_version, KILN_CFG_SCHEMA_VERSION);
    CHECK_OK(kiln_config_validate(&out, NULL));
    CHECK_EQ_UINT(out.log_interval_s, 10u);
}

/*
 * @relation(SWR-CFG-05, scope=function)
 */
KILN_TEST(swrcfg05_a_newer_schema_is_not_guessed_at)
{
    kiln_config_t in, out;
    kiln_config_defaults(&in);

    uint8_t blob[1024];
    size_t len = 0;
    CHECK_OK(kiln_config_encode(&in, blob, sizeof(blob), &len));

    /* A field could have changed meaning, not merely been added. */
    blob[4] = KILN_CFG_SCHEMA_VERSION + 1u;
    const size_t crc_at = 8u + sizeof(kiln_config_t);
    const uint16_t crc = kiln_crc16(blob, crc_at);
    blob[crc_at] = (uint8_t)crc;
    blob[crc_at + 1] = (uint8_t)(crc >> 8u);

    CHECK_ERR(kiln_config_decode(blob, len, &out), KILN_ERR_CORRUPT);
    CHECK_OK(kiln_config_validate(&out, NULL));
}

/*
 * @relation(SWR-CFG-05, scope=function)
 */
KILN_TEST(swrcfg05_a_value_outside_a_tightened_range_is_repaired)
{
    kiln_config_t in, out;
    kiln_config_defaults(&in);

    uint8_t blob[1024];
    size_t len = 0;

    /* A firmware that allowed 90 A nominal, read by one that allows 60. */
    in.nominal_a = 300.0f;
    CHECK_OK(kiln_config_encode(&in, blob, sizeof(blob), &len));

    CHECK_ERR(kiln_config_decode(blob, len, &out), KILN_ERR_UNSUPPORTED);
    CHECK_NEAR(out.nominal_a, 30.0f, 0.01f);
    CHECK_OK(kiln_config_validate(&out, NULL));
}

KILN_TEST(encode_refuses_a_buffer_that_is_too_small)
{
    kiln_config_t cfg;
    kiln_config_defaults(&cfg);
    uint8_t tiny[8];
    CHECK_ERR(kiln_config_encode(&cfg, tiny, sizeof(tiny), NULL), KILN_ERR_NO_SPACE);
    CHECK_ERR(kiln_config_encode(NULL, tiny, sizeof(tiny), NULL), KILN_ERR_INVALID_ARG);
}

/* ===========================================================================
 * SCHEMA 2: A REMOVED FIELD  (SWR-CFG-05, SWR-ACQ-02, tasklist R9)
 * ===========================================================================
 * Schema 1 carried `sense.tc_type` as one byte immediately before
 * `case_tc_type`. SWR-ACQ-02 took it away, because the chamber front end
 * belongs to the supervisor and an item that let this side change the type
 * would change what the supervisor's backstop means.
 *
 * Every earlier migration added fields, which a prefix copy handles. A removal
 * in the middle shifts every byte after the hole, and a prefix copy would land
 * the whole sensing, control and network block one byte out of place: the
 * stored line filter frequency would read as half of itself plus half a
 * calibration offset, and the CRC would have passed on the way in, so nothing
 * would report it. The device would come back from an update quietly
 * mis-calibrated.
 *
 * This forges a real version 1 blob and checks the fields on both sides of the
 * hole, because that is the only way to know the splice is right.
 */

/*
 * @relation(SWR-CFG-05, scope=function)
 */
KILN_TEST(swrcfg05_a_version_1_blob_survives_the_removed_field)
{
    /* The version 1 layout is the version 2 layout with one extra byte at
     * offsetof(case_tc_type). Built by taking a version 2 encoding apart and
     * inserting the byte, which is exactly what the old firmware wrote. */
    kiln_config_t in;
    kiln_config_defaults(&in);

    /* Distinctive values on both sides of the hole, chosen in range so that a
     * clamp cannot hide a misplaced byte. */
    in.max_temp_c      = 1150.0f;        /* before  */
    in.case_tc_type    = (uint8_t)KILN_TC_TYPE_N;
    in.line_filter_hz  = 60u;            /* after, and the one a shift ruins */
    in.filter_tau_s    = 7.5f;
    in.cal_offset_c    = -3.25f;
    in.cal_gain        = 1.002f;
    in.log_interval_s  = 30u;
    in.dim_timeout_s   = 45u;
    CHECK_OK(kiln_config_validate(&in, nullptr));

    uint8_t v2[1024];
    size_t v2len = 0;
    CHECK_OK(kiln_config_encode(&in, v2, sizeof(v2), &v2len));

    constexpr size_t HDR = 8u;
    const size_t hole = offsetof(kiln_config_t, case_tc_type);
    const size_t v1_payload = sizeof(kiln_config_t) + 1u;

    uint8_t v1[1200];
    memset(v1, 0, sizeof(v1));
    /* Header: magic as encoded, version 1, the longer payload. */
    memcpy(v1, v2, HDR);
    v1[4] = 1u; v1[5] = 0u;
    v1[6] = (uint8_t)(v1_payload & 0xFFu);
    v1[7] = (uint8_t)(v1_payload >> 8u);
    /* Payload: everything before the hole, the removed byte, then the rest. */
    memcpy(&v1[HDR], &v2[HDR], hole);
    v1[HDR + hole] = (uint8_t)KILN_TC_TYPE_S;   /* the field that went away */
    memcpy(&v1[HDR + hole + 1u], &v2[HDR + hole], sizeof(kiln_config_t) - hole);
    const size_t crc_at = HDR + v1_payload;
    const uint16_t crc = kiln_crc16(v1, crc_at);
    v1[crc_at]      = (uint8_t)crc;
    v1[crc_at + 1u] = (uint8_t)(crc >> 8u);

    kiln_config_t out;
    kiln_config_defaults(&out);
    /* Migrated, so the caller is told to write it back. */
    CHECK_ERR(kiln_config_decode(v1, crc_at + 2u, &out), KILN_ERR_UNSUPPORTED);
    CHECK_EQ_UINT(out.schema_version, KILN_CFG_SCHEMA_VERSION);

    /* Before the hole. */
    CHECK_NEAR(out.max_temp_c, 1150.0f, 0.01f);

    /* At the hole: the enclosure type, which in version 1 sat one byte later. */
    CHECK_EQ_UINT(out.case_tc_type, (uint8_t)KILN_TC_TYPE_N);

    /* After the hole, which is where a prefix copy would have gone wrong. */
    CHECK_EQ_UINT(out.line_filter_hz, 60u);
    CHECK_NEAR(out.filter_tau_s, 7.5f, 0.001f);
    CHECK_NEAR(out.cal_offset_c, -3.25f, 0.001f);
    CHECK_NEAR(out.cal_gain, 1.002f, 0.0001f);
    CHECK_EQ_UINT(out.log_interval_s, 30u);
    CHECK_EQ_UINT(out.dim_timeout_s, 45u);

    /* And the whole thing is still a valid configuration, which is what the
     * device is about to run a kiln on. */
    CHECK_OK(kiln_config_validate(&out, nullptr));

    /* The migration is not a one-way street into nonsense: re-encoded and
     * decoded, it is now a clean version 2 blob. */
    uint8_t again[1024];
    size_t again_len = 0;
    CHECK_OK(kiln_config_encode(&out, again, sizeof(again), &again_len));
    kiln_config_t back;
    CHECK_OK(kiln_config_decode(again, again_len, &back));
    CHECK_EQ_UINT(back.line_filter_hz, 60u);
    CHECK_EQ_UINT(back.case_tc_type, (uint8_t)KILN_TC_TYPE_N);
}

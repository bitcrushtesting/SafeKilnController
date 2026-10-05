/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The JSON writer and reader -- NFR-11, NFR-19, FR-WEB-20.
 *
 * The reader parses request bodies, so it is the firmware's largest piece of
 * untrusted-input surface after the log codec. Its job is to reject, never to
 * guess: `12abc` is not 12, and a truncated string is not a short string.
 */

#include <string.h>
#include "kiln_check.h"
#include "kiln_web/json.h"

/* --- writing ----------------------------------------------------------- */

KILN_TEST(the_writer_produces_the_document_it_was_told_to)
{
    char buf[256];
    kiln_json_t j;
    kiln_json_init(&j, buf, sizeof(buf));

    kiln_json_obj_open(&j);
    kiln_json_kv_str(&j, "state", "RUN");
    kiln_json_kv_num(&j, "kiln_c", 987.68, 1);
    kiln_json_kv_uint(&j, "duty", 650);
    kiln_json_kv_bool(&j, "heating", true);
    kiln_json_key(&j, "warnings");
    kiln_json_arr_open(&j);
    kiln_json_uint(&j, 107);
    kiln_json_uint(&j, 112);
    kiln_json_arr_close(&j);
    kiln_json_key(&j, "fault");
    kiln_json_null(&j);
    kiln_json_obj_close(&j);

    CHECK(kiln_json_ok(&j));
    CHECK_STR_EQ(kiln_json_text(&j),
        "{\"state\":\"RUN\",\"kiln_c\":987.7,\"duty\":650,\"heating\":true,"
        "\"warnings\":[107,112],\"fault\":null}");
}

KILN_TEST(nested_objects_and_arrays_are_comma_separated_correctly)
{
    char buf[256];
    kiln_json_t j;
    kiln_json_init(&j, buf, sizeof(buf));

    kiln_json_obj_open(&j);
    kiln_json_key(&j, "segments");
    kiln_json_arr_open(&j);
    for (int i = 0; i < 2; i++) {
        kiln_json_obj_open(&j);
        kiln_json_kv_int(&j, "target_c", 100 * (i + 1));
        kiln_json_obj_close(&j);
    }
    kiln_json_arr_close(&j);
    kiln_json_kv_int(&j, "count", 2);
    kiln_json_obj_close(&j);

    CHECK(kiln_json_ok(&j));
    CHECK_STR_EQ(kiln_json_text(&j),
        "{\"segments\":[{\"target_c\":100},{\"target_c\":200}],\"count\":2}");
}

KILN_TEST(nfr11_overflow_is_sticky_and_never_writes_past_the_buffer)
{
    /* The pattern the handlers rely on: write the whole response without
     * checking each call, then check once.  That only works if a failed write
     * poisons everything after it. */
    char buf[24];
    memset(buf, 0x7F, sizeof(buf));
    kiln_json_t j;
    kiln_json_init(&j, buf, sizeof(buf));

    kiln_json_obj_open(&j);
    for (int i = 0; i < 50; i++) {
        kiln_json_kv_int(&j, "a_long_key_name", 123456);
    }
    kiln_json_obj_close(&j);

    CHECK(!kiln_json_ok(&j));
    CHECK(kiln_json_len(&j) < sizeof(buf));
    CHECK_EQ_UINT(buf[sizeof(buf) - 1], 0x7F);   /* untouched */
    CHECK_EQ_UINT(strlen(buf), kiln_json_len(&j));
}

KILN_TEST(a_zero_length_buffer_is_refused_rather_than_written_to)
{
    kiln_json_t j;
    kiln_json_init(&j, NULL, 0);
    CHECK(!kiln_json_ok(&j));
    kiln_json_obj_open(&j);
    kiln_json_kv_int(&j, "x", 1);
    CHECK(!kiln_json_ok(&j));
}

KILN_TEST(strings_are_escaped_so_untrusted_text_cannot_break_the_document)
{
    /* A program name arrives from the network, so this is not theoretical: an
     * unescaped quote or newline turns a response into a parse error at the
     * client, and a control character makes it invalid JSON outright. */
    char buf[256];
    kiln_json_t j;
    kiln_json_init(&j, buf, sizeof(buf));

    kiln_json_obj_open(&j);
    kiln_json_kv_str(&j, "name", "he said \"fire\"\n\tfast\\hot\x01");
    kiln_json_obj_close(&j);

    CHECK(kiln_json_ok(&j));
    CHECK_STR_EQ(kiln_json_text(&j),
        "{\"name\":\"he said \\\"fire\\\"\\n\\tfast\\\\hot\\u0001\"}");
}

KILN_TEST(a_non_finite_number_becomes_null_rather_than_invalid_json)
{
    /* JSON has no NaN, so emitting one would make the whole document
     * unparseable -- and a NaN arriving here means something upstream is wrong,
     * which null says and `nan` does not. */
    char buf[128];
    kiln_json_t j;
    kiln_json_init(&j, buf, sizeof(buf));

    kiln_json_obj_open(&j);
    kiln_json_kv_num(&j, "a", 0.0 / 0.0, 2);
    kiln_json_kv_num(&j, "b", 1.0 / 0.0, 2);
    kiln_json_kv_num(&j, "c", 12.5, 2);
    kiln_json_obj_close(&j);

    CHECK(kiln_json_ok(&j));
    CHECK_STR_EQ(kiln_json_text(&j), "{\"a\":null,\"b\":null,\"c\":12.50}");
}

KILN_TEST(fixed_decimals_rather_than_float_noise)
{
    char buf[64];
    kiln_json_t j;
    kiln_json_init(&j, buf, sizeof(buf));
    kiln_json_num(&j, 987.5999755859375, 1);
    CHECK_STR_EQ(kiln_json_text(&j), "987.6");
}

KILN_TEST(exceeding_the_depth_limit_is_an_overflow_not_a_stack_overflow)
{
    char buf[256];
    kiln_json_t j;
    kiln_json_init(&j, buf, sizeof(buf));
    for (int i = 0; i < KILN_JSON_MAX_DEPTH + 4; i++) {
        kiln_json_arr_open(&j);
    }
    CHECK(!kiln_json_ok(&j));
}

/* --- reading ----------------------------------------------------------- */

#define TOKS 64

KILN_TEST(the_reader_finds_and_converts_members)
{
    const char *js = "{\"program_id\":7,\"name\":\"bisque\",\"ack\":true,"
                     "\"rate\":12.5,\"nested\":{\"x\":1}}";
    kiln_json_tok_t t[TOKS];
    const int n = kiln_json_parse(js, strlen(js), t, TOKS);
    CHECK(n > 0);
    CHECK_EQ_INT(t[0].type, KILN_JSON_OBJECT);

    double d = 0.0;
    CHECK(kiln_json_get_num(js, t, n, 0, "program_id", &d));
    CHECK_NEAR(d, 7.0, 0.0001);
    CHECK(kiln_json_get_num(js, t, n, 0, "rate", &d));
    CHECK_NEAR(d, 12.5, 0.0001);

    bool b = false;
    CHECK(kiln_json_get_bool(js, t, n, 0, "ack", &b));
    CHECK(b);

    char s[32];
    CHECK(kiln_json_get_str(js, t, n, 0, "name", s, sizeof(s)));
    CHECK_STR_EQ(s, "bisque");

    CHECK(!kiln_json_get_num(js, t, n, 0, "absent", &d));
}

KILN_TEST(only_direct_members_are_found_so_a_nested_key_cannot_shadow_one)
{
    /* If `max_temp_c` nested inside another object satisfied a top-level lookup,
     * a crafted body could set a value the handler thought it was reading from
     * the top level. */
    const char *js = "{\"outer\":{\"max_temp_c\":9999},\"max_temp_c\":1100}";
    kiln_json_tok_t t[TOKS];
    const int n = kiln_json_parse(js, strlen(js), t, TOKS);
    CHECK(n > 0);

    double d = 0.0;
    CHECK(kiln_json_get_num(js, t, n, 0, "max_temp_c", &d));
    CHECK_NEAR(d, 1100.0, 0.001);
}

KILN_TEST(nfr19_a_number_must_be_a_number_all_the_way_through)
{
    /* Accepting the prefix of `1.2.3` is how a malformed body becomes a
     * plausible configuration value. */
    const char *bad[] = { "{\"v\":1.2.3}", "{\"v\":5x}", "{\"v\":--1}",
                          "{\"v\":1e}", "{\"v\":0x10}" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        kiln_json_tok_t t[TOKS];
        const int n = kiln_json_parse(bad[i], strlen(bad[i]), t, TOKS);
        double d = 0.0;
        const bool got = (n > 0) && kiln_json_get_num(bad[i], t, n, 0, "v", &d);
        CHECK_MSG(!got, "%s was accepted as %g", bad[i], d);
    }
}

KILN_TEST(nfr19_malformed_documents_are_rejected_not_half_parsed)
{
    const char *bad[] = {
        "{",                      /* truncated         */
        "{\"a\":}",               /* missing value     */
        "{\"a\":[}",              /* mismatched close  */
        "}",                      /* nothing open      */
        "{\"a\":\"unterminated",  /* unterminated      */
        "{\"a\":\"bad\\xescape\"}",
        "{\"a\":\"raw\ncontrol\"}",
        "{\"a\":\"\\u00zz\"}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        kiln_json_tok_t t[TOKS];
        const int n = kiln_json_parse(bad[i], strlen(bad[i]), t, TOKS);
        CHECK_MSG(n < 0, "document %zu parsed with %d tokens: %s", i, n, bad[i]);
    }
}

KILN_TEST(running_out_of_tokens_is_reported_rather_than_overrunning)
{
    /* A body deeper or wider than the caller allowed for.  The parser must say
     * so, not write past the array it was given. */
    char js[512] = "[";
    for (int i = 0; i < 100; i++) {
        strcat(js, "1,");
    }
    strcat(js, "1]");

    kiln_json_tok_t t[8];
    CHECK_EQ_INT(kiln_json_parse(js, strlen(js), t, 8), -2);
}

KILN_TEST(nfr19_a_string_that_does_not_fit_is_refused_not_truncated)
{
    /* Half a program name is worse than a rejected request, and a silently
     * shortened WiFi password produces a network that cannot be joined with no
     * indication why. */
    const char *js = "{\"name\":\"a rather long program name indeed\"}";
    kiln_json_tok_t t[TOKS];
    const int n = kiln_json_parse(js, strlen(js), t, TOKS);
    CHECK(n > 0);

    char small[8];
    CHECK(!kiln_json_get_str(js, t, n, 0, "name", small, sizeof(small)));

    char big[64];
    CHECK(kiln_json_get_str(js, t, n, 0, "name", big, sizeof(big)));
    CHECK_STR_EQ(big, "a rather long program name indeed");
}

KILN_TEST(escapes_are_unescaped_on_the_way_in)
{
    const char *js = "{\"s\":\"q\\\"b\\\\s\\nl\\tt\\u0041\"}";
    kiln_json_tok_t t[TOKS];
    const int n = kiln_json_parse(js, strlen(js), t, TOKS);
    CHECK(n > 0);

    char s[32];
    CHECK(kiln_json_get_str(js, t, n, 0, "s", s, sizeof(s)));
    CHECK_STR_EQ(s, "q\"b\\s\nl\ttA");
}

KILN_TEST(a_non_ascii_escape_is_refused_rather_than_mangled)
{
    /* Everything this API carries is checked against fixed byte arrays
     * elsewhere; inventing a UTF-8 encoder here would be more surface than the
     * feature is worth, so a higher code point is a rejected request. */
    const char *js = "{\"s\":\"\\u00e9\"}";
    kiln_json_tok_t t[TOKS];
    const int n = kiln_json_parse(js, strlen(js), t, TOKS);
    CHECK(n > 0);

    char s[32];
    CHECK(!kiln_json_get_str(js, t, n, 0, "s", s, sizeof(s)));
}

KILN_TEST(an_empty_body_is_not_an_object)
{
    kiln_json_tok_t t[TOKS];
    CHECK(kiln_json_parse("", 0, t, TOKS) <= 0);
    CHECK_EQ_INT(kiln_json_parse(NULL, 0, t, TOKS), -1);
    CHECK_EQ_INT(kiln_json_parse("{}", 2, NULL, TOKS), -1);
}

KILN_TEST(arrays_of_objects_are_walked_correctly)
{
    const char *js = "{\"segments\":["
                     "{\"target_c\":100,\"dwell_min\":30},"
                     "{\"target_c\":600,\"dwell_min\":0},"
                     "{\"target_c\":999,\"dwell_min\":20}]}";
    kiln_json_tok_t t[TOKS];
    const int n = kiln_json_parse(js, strlen(js), t, TOKS);
    CHECK(n > 0);

    const int arr = kiln_json_find(js, t, n, 0, "segments");
    CHECK(arr > 0);
    CHECK_EQ_INT(t[arr].type, KILN_JSON_ARRAY);
    CHECK_EQ_INT(t[arr].size, 3);

    const uint16_t want[] = { 100, 600, 999 };
    int i = arr + 1;
    for (int k = 0; k < 3; k++) {
        double d = 0.0;
        CHECK(kiln_json_get_num(js, t, n, i, "target_c", &d));
        CHECK_NEAR(d, (double)want[k], 0.001);

        const int end = t[i].end;
        i++;
        while (i < n && t[i].start < end) {
            i++;
        }
    }
}

KILN_TEST(the_reader_survives_arbitrary_bytes)
{
    /* The same property the log codec is swept for, for the same reason: this is
     * where untrusted input lands. Under ASan this is the test that matters. */
    uint32_t rng = 0xC0FFEEu;
    for (int iter = 0; iter < 200000; iter++) {
        char js[64];
        const size_t len = 1u + (rng % (sizeof(js) - 1u));
        for (size_t i = 0; i < len; i++) {
            rng = rng * 1103515245u + 12345u;
            /* Biased toward structural characters, so the parser's state machine
             * is actually exercised rather than rejecting on byte one. */
            static const char alphabet[] = "{}[]\",:0123456789.truefalsn \\ux-+e";
            js[i] = alphabet[(rng >> 16) % (sizeof(alphabet) - 1u)];
        }
        js[len] = '\0';

        kiln_json_tok_t t[32];
        const int n = kiln_json_parse(js, len, t, 32);
        if (n <= 0) {
            continue;
        }

        /* Anything the parser accepted must be safely readable. */
        for (int k = 0; k < n; k++) {
            CHECK(t[k].start >= 0);
            CHECK(t[k].end <= (int)len);
            CHECK(t[k].end >= t[k].start);
            double d = 0.0;
            bool b = false;
            char s[64];
            (void)kiln_json_num_at(js, t, k, &d);
            (void)kiln_json_bool_at(js, t, k, &b);
            (void)kiln_json_str_at(js, t, k, s, sizeof(s));
        }
        if (t[0].type == KILN_JSON_OBJECT) {
            double d = 0.0;
            (void)kiln_json_get_num(js, t, n, 0, "anything", &d);
        }
    }
}

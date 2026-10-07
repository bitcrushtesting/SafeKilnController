/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kiln/types.h"      /* kiln_bytes_of */
#include "kiln_web/json.h"

/* --- writing ----------------------------------------------------------- */

void kiln_json_init(kiln_json_t *j, char *buf, size_t cap)
{
    if (j == nullptr) {
        return;
    }
    memset(j, 0, sizeof(*j));
    j->buf = buf;
    j->cap = cap;
    if ((buf != nullptr) && cap > 0) {
        buf[0] = '\0';
    }
    if ((buf == nullptr) || cap == 0) {
        j->overflow = true;
    }
}

namespace {

/* Every write goes through here, so the bound is enforced in one place and the
 * overflow flag cannot be bypassed. */
void put(kiln_json_t *j, const char *s, size_t n)
{
    if ((j == nullptr) || j->overflow) {
        return;
    }
    if (j->len + n + 1 > j->cap) { j->overflow = true; return; }
    memcpy(&j->buf[j->len], s, n);
    j->len += n;
    j->buf[j->len] = '\0';
}

void putc_(kiln_json_t *j, char c) { put(j, &c, 1); }

/* A comma before anything but the first item at this depth. */
void separate(kiln_json_t *j)
{
    if ((j == nullptr) || j->overflow) {
        return;
    }
    if (j->depth < KILN_JSON_MAX_DEPTH && j->had_item[j->depth]) {
        putc_(j, ',');
    }
    if (j->depth < KILN_JSON_MAX_DEPTH) {
        j->had_item[j->depth] = true;
    }
}

void open_container(kiln_json_t *j, char c)
{
    if ((j == nullptr) || j->overflow) {
        return;
    }
    separate(j);
    putc_(j, c);
    if ((size_t)j->depth + 1U >= KILN_JSON_MAX_DEPTH) { j->overflow = true; return; }
    j->depth++;
    j->had_item[j->depth] = false;
}

void close_container(kiln_json_t *j, char c)
{
    if ((j == nullptr) || j->overflow) {
        return;
    }
    if (j->depth == 0) { j->overflow = true; return; }   /* unbalanced */
    j->depth--;
    putc_(j, c);
}

} // namespace

void kiln_json_obj_open(kiln_json_t *j)  { open_container(j, '{'); }
void kiln_json_obj_close(kiln_json_t *j) { close_container(j, '}'); }
void kiln_json_arr_open(kiln_json_t *j)  { open_container(j, '['); }
void kiln_json_arr_close(kiln_json_t *j) { close_container(j, ']'); }

namespace {

/* Escape per RFC 8259.  Control characters below 0x20 must be escaped or the
 * document is invalid -- and a program name arrives from the network, so this is
 * not a theoretical case (NFR-19). */
void put_escaped(kiln_json_t *j, const char *s)
{
    putc_(j, '"');
    /* Scanned as bytes: the switch below compares against values under 0x20 and
     * `char` is signed on both toolchains, so a byte above 0x7F would compare
     * negative if this walked char. */
    for (const uint8_t *p = kiln_bytes_of(s); (*p) != 0u; p++) {
        switch (*p) {
        case '"':  put(j, "\\\"", 2); break;
        case '\\': put(j, "\\\\", 2); break;
        case '\n': put(j, "\\n", 2);  break;
        case '\r': put(j, "\\r", 2);  break;
        case '\t': put(j, "\\t", 2);  break;
        case '\b': put(j, "\\b", 2);  break;
        case '\f': put(j, "\\f", 2);  break;
        default:
            if (*p < 0x20u) {
                char u[7];
                /* *p < 0x20, so "\\u00xx" is exactly 6 characters plus the NUL:
                 * u is sized to the one output this can produce. */
                (void)snprintf(u, sizeof(u), "\\u%04x", (unsigned)*p);
                put(j, u, 6);
            } else {
                putc_(j, (char)*p);
            }
            break;
        }
    }
    putc_(j, '"');
}

} // namespace

void kiln_json_key(kiln_json_t *j, const char *key)
{
    if ((j == nullptr) || j->overflow || (key == nullptr)) {
        return;
    }
    separate(j);
    put_escaped(j, key);
    putc_(j, ':');
    /* The value that follows is part of this member, not a sibling. */
    if (j->depth < KILN_JSON_MAX_DEPTH) {
        j->had_item[j->depth] = false;
    }
}

namespace {

/* A key resets had_item so the value is not comma-separated from its own key;
 * after the value, the member is complete. */
void value_written(kiln_json_t *j)
{
    if ((j != nullptr) && j->depth < KILN_JSON_MAX_DEPTH) {
        j->had_item[j->depth] = true;
    }
}

} // namespace

void kiln_json_str(kiln_json_t *j, const char *v)
{
    if ((j == nullptr) || j->overflow) {
        return;
    }
    separate(j);
    if (v != nullptr) {
        put_escaped(j, v);
    }
    else {
        put(j, "null", 4);
    }
    value_written(j);
}

void kiln_json_bool(kiln_json_t *j, bool v)
{
    if ((j == nullptr) || j->overflow) {
        return;
    }
    separate(j);
    put(j, v ? "true" : "false", v ? 4u : 5u);
    value_written(j);
}

void kiln_json_null(kiln_json_t *j)
{
    if ((j == nullptr) || j->overflow) {
        return;
    }
    separate(j);
    put(j, "null", 4);
    value_written(j);
}

void kiln_json_int(kiln_json_t *j, long long v)
{
    if ((j == nullptr) || j->overflow) {
        return;
    }
    separate(j);
    char b[24];
    const int n = snprintf(b, sizeof(b), "%lld", v);
    if (n > 0) {
        put(j, b, (size_t)n);
    }
    value_written(j);
}

void kiln_json_uint(kiln_json_t *j, unsigned long long v)
{
    if ((j == nullptr) || j->overflow) {
        return;
    }
    separate(j);
    char b[24];
    const int n = snprintf(b, sizeof(b), "%llu", v);
    if (n > 0) {
        put(j, b, (size_t)n);
    }
    value_written(j);
}

void kiln_json_num(kiln_json_t *j, double v, int decimals)
{
    if ((j == nullptr) || j->overflow) {
        return;
    }
    separate(j);

    /* JSON has no NaN or Infinity, so emitting one would make the whole document
     * unparseable -- and a NaN reaching here at all means something upstream is
     * wrong, which null communicates and `nan` does not. */
    if (!isfinite(v)) {
        put(j, "null", 4);
        value_written(j);
        return;
    }
    if (decimals < 0) {
        decimals = 0;
    }
    if (decimals > 9) {
        decimals = 9;
    }

    char b[40];
    const int n = snprintf(b, sizeof(b), "%.*f", decimals, v);
    if (n > 0 && (size_t)n < sizeof(b)) {
        put(j, b, (size_t)n);
    }
    value_written(j);
}

void kiln_json_raw(kiln_json_t *j, const char *text)
{
    if ((j == nullptr) || j->overflow || (text == nullptr)) {
        return;
    }
    separate(j);
    put(j, text, strlen(text));
    value_written(j);
}

void kiln_json_kv_str(kiln_json_t *j, const char *key, const char *v)
{
    kiln_json_key(j, key);
    kiln_json_str(j, v);
}
void kiln_json_kv_bool(kiln_json_t *j, const char *key, bool v)
{
    kiln_json_key(j, key);
    kiln_json_bool(j, v);
}
void kiln_json_kv_int(kiln_json_t *j, const char *key, long long v)
{
    kiln_json_key(j, key);
    kiln_json_int(j, v);
}
void kiln_json_kv_uint(kiln_json_t *j, const char *key, unsigned long long v)
{
    kiln_json_key(j, key);
    kiln_json_uint(j, v);
}
void kiln_json_kv_num(kiln_json_t *j, const char *key, double v, int decimals)
{
    kiln_json_key(j, key);
    kiln_json_num(j, v, decimals);
}

/* --- reading ----------------------------------------------------------- */

namespace {

typedef struct {
    const char      *js;
    size_t           len;
    size_t           pos;
    kiln_json_tok_t *toks;
    int              max;
    int              count;
    int              parent;
} parser_t;

kiln_json_tok_t *alloc_tok(parser_t *p)
{
    if (p->count >= p->max) {
        return NULL;
    }
    kiln_json_tok_t *t = &p->toks[p->count++];
    t->type   = KILN_JSON_UNDEFINED;
    t->start  = -1;
    t->end    = -1;
    t->size   = 0;
    t->parent = p->parent;
    return t;
}

/* A bare number, true, false or null.  Terminated by whitespace or structure;
 * anything else is malformed, which matters because the alternative is silently
 * accepting `12abc` as 12. */
int parse_primitive(parser_t *p)
{
    const size_t start = p->pos;

    for (; p->pos < p->len; p->pos++) {
        const char c = p->js[p->pos];
        if (c == ',' || c == '}' || c == ']' ||
            c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            break;
        }
        /* Printable, non-structural characters only. */
        if ((unsigned char)c < 32 || (unsigned char)c >= 127) {
            return -1;
        }
    }
    if (p->pos == start) {
        return -1;
    }

    kiln_json_tok_t *t = alloc_tok(p);
    if (t == nullptr) {
        return -2;
    }
    t->type  = KILN_JSON_PRIMITIVE;
    t->start = (int)start;
    t->end   = (int)p->pos;
    p->pos--;                      /* the caller advances */
    return 0;
}

int parse_string(parser_t *p)
{
    p->pos++;                      /* opening quote */
    const size_t start = p->pos;

    for (; p->pos < p->len; p->pos++) {
        const char c = p->js[p->pos];

        if (c == '"') {
            kiln_json_tok_t *t = alloc_tok(p);
            if (t == nullptr) {
                return -2;
            }
            t->type  = KILN_JSON_STRING;
            t->start = (int)start;
            t->end   = (int)p->pos;
            return 0;
        }
        if (c == '\\' && p->pos + 1 < p->len) {
            p->pos++;
            switch (p->js[p->pos]) {
            case '"': case '/': case '\\': case 'b':
            case 'f': case 'r': case 'n':  case 't':
                break;
            case 'u':
                if (p->pos + 4 >= p->len) {
                    return -3;
                }
                for (int i = 1; i <= 4; i++) {
                    const char h = p->js[p->pos + i];
                    if ((h < '0' || h > '9') && (h < 'a' || h > 'f') && (h < 'A' || h > 'F')) {
                        return -1;
                    }
                }
                p->pos += 4;
                break;
            default:
                return -1;         /* not an escape JSON defines */
            }
        } else if ((unsigned char)c < 0x20u) {
            return -1;             /* a raw control character is malformed */
        }
    }
    return -3;                     /* unterminated */
}

/* Defined below; needed by the pairing check at the end of the parse. */
int skip(const kiln_json_tok_t *toks, int ntok, int idx);

} // namespace

int kiln_json_parse(const char *js, size_t len, kiln_json_tok_t *toks, int max_toks)
{
    if ((js == nullptr) || (toks == nullptr) || max_toks <= 0) {
        return -1;
    }

    parser_t p = { .js = js, .len = len, .pos = 0, .toks = toks,
                   .max = max_toks, .count = 0, .parent = -1 };

    for (; p.pos < p.len; p.pos++) {
        const char c = js[p.pos];

        switch (c) {
        case '{':
        case '[': {
            kiln_json_tok_t *t = alloc_tok(&p);
            if (t == nullptr) {
                return -2;
            }
            if (p.parent >= 0) {
                toks[p.parent].size++;
            }
            t->type   = (c == '{') ? KILN_JSON_OBJECT : KILN_JSON_ARRAY;
            t->start  = (int)p.pos;
            p.parent  = p.count - 1;
            break;
        }
        case '}':
        case ']': {
            const kiln_json_type_t want = (c == '}') ? KILN_JSON_OBJECT : KILN_JSON_ARRAY;
            /* Close the nearest unclosed container, and insist it is the right
             * kind -- `{"a":[}` must not parse. */
            int i = p.count - 1;
            for (; i >= 0; i--) {
                if (toks[i].start != -1 && toks[i].end == -1) {
                    if (toks[i].type != want) {
                        return -1;
                    }
                    toks[i].end = (int)p.pos + 1;
                    p.parent    = toks[i].parent;
                    break;
                }
            }
            if (i == -1) {
                return -1; /* nothing was open */
            }
            break;
        }
        case '"': {
            const int r = parse_string(&p);
            if (r < 0) {
                return r;
            }
            if (p.parent >= 0) {
                toks[p.parent].size++;
            }
            break;
        }
        case ' ': case '\t': case '\n': case '\r':
            break;
        case ':':
            /* The key's value belongs to the key's object, which is already the
             * parent, so there is nothing to re-point. */
            break;
        case ',':
            break;
        default: {
            const int r = parse_primitive(&p);
            if (r < 0) {
                return r;
            }
            if (p.parent >= 0) {
                toks[p.parent].size++;
            }
            break;
        }
        }
    }

    /* Anything still open is a truncated document, not a malformed one: for a
     * request body that means a short read, which the caller may want to
     * distinguish. */
    for (int i = 0; i < p.count; i++) {
        if (toks[i].start != -1 && toks[i].end == -1) {
            return -3;
        }
    }

    /* An object's children must be key/value pairs, each key a string.  Without
     * this `{"a":}` parses happily as an object with one child, and a handler
     * reading "a" gets whatever followed it. */
    for (int i = 0; i < p.count; i++) {
        if (toks[i].type != KILN_JSON_OBJECT) {
            continue;
        }
        if (toks[i].size % 2 != 0) {
            return -1;
        }

        int child = i + 1;
        for (int m = 0; m < toks[i].size; m += 2) {
            if (child >= p.count || toks[child].parent != i) {
                return -1;
            }
            if (toks[child].type != KILN_JSON_STRING) {
                return -1; /* key */
            }

            const int value = skip(toks, p.count, child);
            if (value >= p.count || toks[value].parent != i) {
                return -1;
            }
            child = skip(toks, p.count, value);
        }
    }
    return p.count;
}

namespace {

/* Skip the whole subtree rooted at `idx`, returning the next sibling index. */
int skip(const kiln_json_tok_t *toks, int ntok, int idx)
{
    if (idx < 0 || idx >= ntok) {
        return ntok;
    }

    const int end = toks[idx].end;
    int i = idx + 1;
    while (i < ntok && toks[i].start < end) {
        i++;
    }
    return i;
}

} // namespace

int kiln_json_find(const char *js, const kiln_json_tok_t *toks, int ntok,
                   int obj, const char *key)
{
    if ((js == nullptr) || (toks == nullptr) || (key == nullptr)) {
        return -1;
    }
    if (obj < 0 || obj >= ntok || toks[obj].type != KILN_JSON_OBJECT) {
        return -1;
    }

    const size_t klen = strlen(key);

    /* Members alternate key, value.  Only direct members, so a nested object
     * cannot shadow a key the caller expected at this level. */
    int i = obj + 1;
    for (int n = 0; n < toks[obj].size && i < ntok; n++) {
        if (toks[i].parent != obj || toks[i].type != KILN_JSON_STRING) {
            break;
        }

        const int vi = i + 1;
        if (vi >= ntok) {
            break;
        }

        const size_t len = (size_t)(toks[i].end - toks[i].start);
        if (len == klen && strncmp(&js[toks[i].start], key, klen) == 0) {
            return vi;
        }

        i = skip(toks, ntok, vi);
    }
    return -1;
}

namespace {

/* RFC 8259's number grammar:
 *
 *   -? (0 | [1-9][0-9]*) (\.[0-9]+)? ([eE][+-]?[0-9]+)?
 *
 * Checked explicitly rather than left to strtod, which is far more permissive
 * than JSON: it accepts C99 hex floats, so `0x10` would arrive as 16; it accepts
 * `infinity` and `nan`; and it accepts a leading `+`.  None of those is a JSON
 * number, and quietly converting one is how a malformed body becomes a plausible
 * configuration value. */
bool is_json_number(const char *s, size_t n)
{
    size_t i = 0;
    if (i < n && s[i] == '-') {
        i++;
    }

    /* Integer part, with no leading zeros. */
    if (i >= n) {
        return false;
    }
    if (s[i] == '0') {
        i++;
    } else if (s[i] >= '1' && s[i] <= '9') {
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            i++;
        }
    } else {
        return false;
    }

    /* Fraction: at least one digit after the point. */
    if (i < n && s[i] == '.') {
        i++;
        if (i >= n || s[i] < '0' || s[i] > '9') {
            return false;
        }
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            i++;
        }
    }

    /* Exponent: at least one digit after the optional sign. */
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) {
            i++;
        }
        if (i >= n || s[i] < '0' || s[i] > '9') {
            return false;
        }
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            i++;
        }
    }
    return i == n;
}

} // namespace

bool kiln_json_num_at(const char *js, const kiln_json_tok_t *toks, int idx, double *out)
{
    if ((js == nullptr) || (toks == nullptr) || idx < 0 || (out == nullptr)) {
        return false;
    }
    if (toks[idx].type != KILN_JSON_PRIMITIVE) {
        return false;
    }

    char b[32];
    const size_t n = (size_t)(toks[idx].end - toks[idx].start);
    if (n == 0 || n >= sizeof(b)) {
        return false;
    }
    if (!is_json_number(&js[toks[idx].start], n)) {
        return false;
    }

    memcpy(b, &js[toks[idx].start], n);
    b[n] = '\0';

    char *endp = NULL;
    const double v = strtod(b, &endp);
    if ((endp == nullptr) || *endp != '\0') {
        return false;
    }
    /* A number the grammar allows can still overflow to infinity. */
    if (!isfinite(v)) {
        return false;
    }
    *out = v;
    return true;
}

bool kiln_json_bool_at(const char *js, const kiln_json_tok_t *toks, int idx, bool *out)
{
    if ((js == nullptr) || (toks == nullptr) || idx < 0 || (out == nullptr)) {
        return false;
    }
    if (toks[idx].type != KILN_JSON_PRIMITIVE) {
        return false;
    }

    const size_t n = (size_t)(toks[idx].end - toks[idx].start);
    const char  *s = &js[toks[idx].start];
    if (n == 4 && strncmp(s, "true", 4) == 0)  { *out = true;  return true; }
    if (n == 5 && strncmp(s, "false", 5) == 0) { *out = false; return true; }
    return false;
}

namespace {

int hexval(char c)
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

bool kiln_json_str_at(const char *js, const kiln_json_tok_t *toks, int idx,
                      char *out, size_t cap)
{
    if ((js == nullptr) || (toks == nullptr) || idx < 0 || (out == nullptr) || cap == 0) {
        return false;
    }
    if (toks[idx].type != KILN_JSON_STRING) {
        return false;
    }

    size_t o = 0;
    for (int i = toks[idx].start; i < toks[idx].end; i++) {
        /* Reject rather than truncate: half a program name is worse than a
         * refused request, and silently shortening a WiFi password produces a
         * network that cannot be joined with no indication why. */
        if (o + 1 >= cap) {
            return false;
        }

        char const c = js[i];
        if (c != '\\') { out[o++] = c; continue; }

        if (++i >= toks[idx].end) {
            return false;
        }
        switch (js[i]) {
        case '"':  out[o++] = '"';  break;
        case '\\': out[o++] = '\\'; break;
        case '/':  out[o++] = '/';  break;
        case 'b':  out[o++] = '\b'; break;
        case 'f':  out[o++] = '\f'; break;
        case 'n':  out[o++] = '\n'; break;
        case 'r':  out[o++] = '\r'; break;
        case 't':  out[o++] = '\t'; break;
        case 'u': {
            if (i + 4 >= toks[idx].end) {
                return false;
            }
            unsigned v = 0;
            for (int k = 1; k <= 4; k++) {
                const int h = hexval(js[i + k]);
                if (h < 0) {
                    return false;
                }
                v = (v << 4u) | (unsigned)h;
            }
            i += 4;
            /* Only the ASCII range is reproduced.  Everything this API carries
             * -- program names, SSIDs, hostnames -- is checked against fixed
             * byte arrays elsewhere, and inventing a UTF-8 encoder here would be
             * more surface than the feature is worth.  A higher code point is
             * refused rather than mangled. */
            if (v == 0u || v > 0x7Fu) {
                return false;
            }
            out[o++] = (char)v;
            break;
        }
        default: return false;
        }
    }
    out[o] = '\0';
    return true;
}

bool kiln_json_get_num(const char *js, const kiln_json_tok_t *toks, int ntok,
                       int obj, const char *key, double *out)
{
    const int i = kiln_json_find(js, toks, ntok, obj, key);
    return i >= 0 && kiln_json_num_at(js, toks, i, out);
}

bool kiln_json_get_bool(const char *js, const kiln_json_tok_t *toks, int ntok,
                        int obj, const char *key, bool *out)
{
    const int i = kiln_json_find(js, toks, ntok, obj, key);
    return i >= 0 && kiln_json_bool_at(js, toks, i, out);
}

bool kiln_json_get_str(const char *js, const kiln_json_tok_t *toks, int ntok,
                       int obj, const char *key, char *out, size_t cap)
{
    const int i = kiln_json_find(js, toks, ntok, obj, key);
    return i >= 0 && kiln_json_str_at(js, toks, i, out, cap);
}

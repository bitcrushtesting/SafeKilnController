/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * JSON writing and reading, bounded -- SWR-NFR-11, SWR-NFR-19, architecture 12.2.
 *
 * No allocation anywhere, and nothing proportional to input: the writer fills a
 * buffer the caller owns and refuses to exceed it, the reader tokenises into a
 * fixed array the caller owns.  Both are needed because every request body on
 * this device is untrusted (SWR-NFR-19) and the device has 512 kB of RAM and no
 * PSRAM.
 *
 * The writer's overflow flag is *sticky*: once a write would not fit, nothing
 * more is emitted and kiln_json_ok() stays false until the buffer is reset.  A
 * handler therefore writes its whole response without checking each call, then
 * checks once -- which is the only pattern that does not invite a forgotten
 * check in the middle of a nested structure.
 */
#ifndef KILN_WEB_JSON_H
#define KILN_WEB_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

constexpr size_t KILN_JSON_MAX_DEPTH = 12;

/* --- writing ----------------------------------------------------------- */

typedef struct {
    char   *buf;
    size_t  cap;            /* including room for the terminator */
    size_t  len;
    bool    overflow;
    uint8_t depth;
    bool    had_item[KILN_JSON_MAX_DEPTH];   /* a comma is needed before the next */
} kiln_json_t;

void kiln_json_init(kiln_json_t *j, char *buf, size_t cap);

static inline bool kiln_json_ok(const kiln_json_t *j) {
    return (j != nullptr) && !j->overflow;
}
static inline const char *kiln_json_text(const kiln_json_t *j) { return j->buf; }
static inline size_t kiln_json_len(const kiln_json_t *j) { return j->len; }

void kiln_json_obj_open(kiln_json_t *j);
void kiln_json_obj_close(kiln_json_t *j);
void kiln_json_arr_open(kiln_json_t *j);
void kiln_json_arr_close(kiln_json_t *j);

/* A key, then exactly one value. */
void kiln_json_key(kiln_json_t *j, const char *key);

void kiln_json_str(kiln_json_t *j, const char *v);
void kiln_json_bool(kiln_json_t *j, bool v);
void kiln_json_null(kiln_json_t *j);
void kiln_json_int(kiln_json_t *j, long long v);
void kiln_json_uint(kiln_json_t *j, unsigned long long v);
/* Fixed decimals rather than %g: a temperature of 987.6 should read as 987.6 and
 * not as 987.5999755859375, and a non-finite value emits null rather than the
 * `nan` that would make the document unparseable. */
void kiln_json_num(kiln_json_t *j, double v, int decimals);

/* key + value in one call, which is what most of a response is. */
void kiln_json_kv_str(kiln_json_t *j, const char *key, const char *v);
void kiln_json_kv_bool(kiln_json_t *j, const char *key, bool v);
void kiln_json_kv_int(kiln_json_t *j, const char *key, long long v);
void kiln_json_kv_uint(kiln_json_t *j, const char *key, unsigned long long v);
void kiln_json_kv_num(kiln_json_t *j, const char *key, double v, int decimals);

/* Raw, pre-escaped text.  For splicing an already-built fragment; the caller is
 * responsible for it being valid JSON. */
void kiln_json_raw(kiln_json_t *j, const char *text);

/* --- reading ----------------------------------------------------------- */

typedef enum {
    KILN_JSON_UNDEFINED = 0,
    KILN_JSON_OBJECT,
    KILN_JSON_ARRAY,
    KILN_JSON_STRING,
    KILN_JSON_PRIMITIVE,    /* number, true, false, null */
} kiln_json_type_t;

typedef struct {
    kiln_json_type_t type;
    int              start;   /* byte offset of the value, string without quotes */
    int              end;     /* one past the last byte */
    int              size;    /* members for an object, elements for an array */
    int              parent;  /* index of the containing token, -1 at the root */
} kiln_json_tok_t;

/* Tokenise.  Returns the number of tokens, or a negative value:
 *   -1 malformed
 *   -2 ran out of tokens (the document is deeper or wider than the caller allowed)
 *   -3 truncated (a valid prefix, which for a request body means a short read)
 *
 * Does not copy anything: tokens point into `js`, which must outlive them. */
int kiln_json_parse(const char *js, size_t len, kiln_json_tok_t *toks, int max_toks);

/* Index of the value of `key` inside object token `obj`, or -1.  Only direct
 * members, so a nested object cannot shadow a key the caller expected at the
 * top level. */
int kiln_json_find(const char *js, const kiln_json_tok_t *toks, int ntok,
                   int obj, const char *key);

bool kiln_json_num_at(const char *js, const kiln_json_tok_t *toks, int idx, double *out);
bool kiln_json_bool_at(const char *js, const kiln_json_tok_t *toks, int idx, bool *out);
/* Copies and NUL-terminates, unescaping \" \\ \/ \n \r \t \b \f and \uXXXX for
 * the ASCII range.  Returns false if it does not fit, rather than truncating:
 * a half-copied program name is worse than a rejected request. */
bool kiln_json_str_at(const char *js, const kiln_json_tok_t *toks, int idx,
                      char *out, size_t cap);

/* Convenience: look up a direct member of `obj` and convert it. */
bool kiln_json_get_num(const char *js, const kiln_json_tok_t *toks, int ntok,
                       int obj, const char *key, double *out);
bool kiln_json_get_bool(const char *js, const kiln_json_tok_t *toks, int ntok,
                        int obj, const char *key, bool *out);
bool kiln_json_get_str(const char *js, const kiln_json_tok_t *toks, int ntok,
                       int obj, const char *key, char *out, size_t cap);

#endif

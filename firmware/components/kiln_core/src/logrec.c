/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <string.h>
#include "kiln_core/logrec.h"

/* --- checksums ---------------------------------------------------------- */

/* CRC-8/ATM (x^8 + x^2 + x + 1), table-free: 15 bytes per record is far too
 * little work to justify 256 bytes of table. */
uint8_t kiln_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (uint8_t)((crc & 0x80u) ? ((crc << 1) ^ 0x07u) : (crc << 1));
        }
    }
    return crc;
}

/* CRC-16/CCITT-FALSE */
uint16_t kiln_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (uint16_t)((crc & 0x8000u) ? ((crc << 1) ^ 0x1021u) : (crc << 1));
        }
    }
    return crc;
}

/* --- fixed point helpers ----------------------------------------------- */

/* 0.1 degC resolution, saturating rather than wrapping. */
static int16_t enc_temp(float c)
{
    float v = c * 10.0f;
    if (v > 32767.0f)  v = 32767.0f;
    if (v < -32768.0f) v = -32768.0f;
    return (int16_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

static float dec_temp(int16_t v) { return (float)v / 10.0f; }

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);       p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put_i16(uint8_t *p, int16_t v)
{
    p[0] = (uint8_t)((uint16_t)v); p[1] = (uint8_t)((uint16_t)v >> 8);
}
static int16_t get_i16(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* --- record ------------------------------------------------------------- */

void kiln_logrec_encode(const kiln_log_sample_t *s, uint8_t out[KILN_LOG_RECORD_BYTES])
{
    memset(out, 0, KILN_LOG_RECORD_BYTES);

    put_u32(&out[0],  s->t_rel_ms);
    put_i16(&out[4],  enc_temp(s->kiln_raw_c));
    put_i16(&out[6],  enc_temp(s->kiln_filt_c));
    put_i16(&out[8],  enc_temp(s->setpoint_c));
    put_i16(&out[10], enc_temp(s->case_c));

    /* Current in 10 mA steps: 0 .. 655.35 A, comfortably beyond FR-CUR-02's
     * 60 A range and finer than its 0.1 A resolution requirement. */
    float ca = s->current_a * 100.0f;
    if (ca < 0.0f)      ca = 0.0f;
    if (ca > 65535.0f)  ca = 65535.0f;
    put_u16(&out[12], (uint16_t)(ca + 0.5f));

    /* Duty as 0..200 in half-percent steps: one byte is ample for a value the
     * window can only realise in ~0.5% increments anyway. */
    uint16_t duty200 = (uint16_t)((s->duty_permille + 2u) / 5u);
    if (duty200 > 200u) duty200 = 200u;
    out[14] = (uint8_t)duty200;

    out[15] = s->segment;
    out[16] = (uint8_t)((s->state & 0x0Fu) | (s->flags & 0xF0u));
    out[17] = s->current_flags;
    /* FR-LOG-04.  This was AD-18's reserved byte; an event code is what it was
     * being reserved for. */
    out[18] = (uint8_t)(s->event < KILN_LOGE_COUNT ? s->event : KILN_LOGE_SAMPLE);
    out[19] = kiln_crc8(out, KILN_LOG_RECORD_BYTES - 1);
}

bool kiln_logrec_is_erased(const uint8_t rec[KILN_LOG_RECORD_BYTES])
{
    for (size_t i = 0; i < KILN_LOG_RECORD_BYTES; i++) {
        if (rec[i] != 0xFFu) return false;
    }
    return true;
}

kiln_err_t kiln_logrec_decode(const uint8_t rec[KILN_LOG_RECORD_BYTES],
                              kiln_log_sample_t *out)
{
    if (!rec || !out) return KILN_ERR_INVALID_ARG;
    if (kiln_logrec_is_erased(rec)) return KILN_ERR_NOT_FOUND;

    if (kiln_crc8(rec, KILN_LOG_RECORD_BYTES - 1) != rec[19]) {
        return KILN_ERR_CORRUPT;      /* torn write, or bit rot */
    }

    out->t_rel_ms      = get_u32(&rec[0]);
    out->kiln_raw_c    = dec_temp(get_i16(&rec[4]));
    out->kiln_filt_c   = dec_temp(get_i16(&rec[6]));
    out->setpoint_c    = dec_temp(get_i16(&rec[8]));
    out->case_c        = dec_temp(get_i16(&rec[10]));
    out->current_a     = (float)get_u16(&rec[12]) / 100.0f;
    /* The encoder clamps to 200 half-percent steps, but a decoder reads bytes
     * that may have come from a future encoder or from a corruption that happens
     * to satisfy an 8-bit CRC -- and 255 * 5 is 1275, outside the duty range
     * every consumer of this struct assumes. */
    {
        const uint16_t duty = (uint16_t)(rec[14] * 5u);
        out->duty_permille = duty > KILN_DUTY_MAX ? (uint16_t)KILN_DUTY_MAX : duty;
    }
    out->segment       = rec[15];
    out->state         = (uint8_t)(rec[16] & 0x0Fu);
    out->flags         = (uint8_t)(rec[16] & 0xF0u);
    out->current_flags = rec[17];
    out->event         = rec[18] < KILN_LOGE_COUNT ? rec[18] : (uint8_t)KILN_LOGE_SAMPLE;
    return KILN_OK;
}

/* --- sector header ----------------------------------------------------- */

void kiln_logrec_encode_hdr(const kiln_log_sector_hdr_t *h,
                            uint8_t out[KILN_LOG_HEADER_BYTES])
{
    memset(out, 0, KILN_LOG_HEADER_BYTES);
    put_u32(&out[0],  KILN_LOG_MAGIC);
    put_u32(&out[4],  h->seq);
    put_u32(&out[8],  h->run_id);
    put_u16(&out[12], h->format_version);
    put_u16(&out[14], kiln_crc16(out, KILN_LOG_HEADER_BYTES - 2));
}

kiln_err_t kiln_logrec_decode_hdr(const uint8_t in[KILN_LOG_HEADER_BYTES],
                                  kiln_log_sector_hdr_t *out)
{
    if (!in || !out) return KILN_ERR_INVALID_ARG;

    if (kiln_crc16(in, KILN_LOG_HEADER_BYTES - 2) != get_u16(&in[14])) {
        return KILN_ERR_CORRUPT;
    }
    out->magic = get_u32(&in[0]);
    if (out->magic != KILN_LOG_MAGIC) return KILN_ERR_CORRUPT;

    out->seq            = get_u32(&in[4]);
    out->run_id         = get_u32(&in[8]);
    out->format_version = get_u16(&in[12]);
    if (out->format_version != KILN_LOG_FORMAT_VERSION) return KILN_ERR_UNSUPPORTED;
    return KILN_OK;
}

/* --- decimation -------------------------------------------------------- */

kiln_err_t kiln_decimator_init(kiln_decimator_t *d, kiln_log_bucket_t *storage,
                               uint16_t max_points, uint32_t from_ms, uint32_t to_ms)
{
    /* Validated before the memset, not after: kiln_decimator_push checks all
     * three of its pointers, and an initialiser that writes through a pointer it
     * has not checked is the less careful of the pair. */
    if (!d || !storage) return KILN_ERR_INVALID_ARG;

    memset(d, 0, sizeof(*d));
    d->buckets  = storage;
    d->capacity = max_points ? max_points : 1u;

    /* The storage too, not just the state.  push distinguishes a fresh bucket
     * from an accumulating one by its count being zero, so uninitialised caller
     * storage means the first sample in each bucket never initialises its
     * min/max and the series come out as whatever was on the stack. */
    memset(storage, 0, (size_t)d->capacity * sizeof(*storage));
    d->from_ms  = from_ms;
    d->to_ms    = to_ms;

    if (to_ms > from_ms) {
        const uint32_t span = to_ms - from_ms;
        d->bucket_ms = span / d->capacity;
        if (d->bucket_ms == 0) d->bucket_ms = 1;
    } else {
        /* Unbounded: start at the finest width and let folding find the scale. */
        d->unbounded = true;
        d->bucket_ms = 1u;
    }
    return KILN_OK;
}

static void bucket_merge(kiln_log_bucket_t *dst, const kiln_log_bucket_t *src)
{
    if (src->count == 0) return;
    if (dst->count == 0) {
        *dst = *src;
        return;
    }
    /* The earlier bucket's timestamp wins; the extrema of both survive, which is
     * the property FR-LOG-11 needs to hold at every zoom level. */
    if (src->kiln_min_c < dst->kiln_min_c) dst->kiln_min_c = src->kiln_min_c;
    if (src->kiln_max_c > dst->kiln_max_c) dst->kiln_max_c = src->kiln_max_c;
    if (src->sp_min_c   < dst->sp_min_c)   dst->sp_min_c   = src->sp_min_c;
    if (src->sp_max_c   > dst->sp_max_c)   dst->sp_max_c   = src->sp_max_c;
    if (src->case_min_c < dst->case_min_c) dst->case_min_c = src->case_min_c;
    if (src->case_max_c > dst->case_max_c) dst->case_max_c = src->case_max_c;
    if (src->cur_min_a  < dst->cur_min_a)  dst->cur_min_a  = src->cur_min_a;
    if (src->cur_max_a  > dst->cur_max_a)  dst->cur_max_a  = src->cur_max_a;
    if (src->duty_min   < dst->duty_min)   dst->duty_min   = src->duty_min;
    if (src->duty_max   > dst->duty_max)   dst->duty_max   = src->duty_max;
    dst->flags         = (uint8_t)(dst->flags | src->flags);
    dst->current_flags = (uint8_t)(dst->current_flags | src->current_flags);
    dst->state         = src->state;      /* the later state in the pair */
    dst->count         = (uint16_t)(dst->count + src->count);
}

/* Double the bucket width, folding pairs: 0+1 -> 0, 2+3 -> 1, and so on. */
static void fold_pairs(kiln_decimator_t *d)
{
    const uint16_t used = d->used;
    uint16_t out = 0;

    for (uint16_t i = 0; i < used; i += 2u, out++) {
        kiln_log_bucket_t merged = d->buckets[i];
        if ((uint16_t)(i + 1u) < used) bucket_merge(&merged, &d->buckets[i + 1u]);
        d->buckets[out] = merged;
    }
    for (uint16_t i = out; i < used; i++) {
        const kiln_log_bucket_t empty = {0};
        d->buckets[i] = empty;
    }

    d->used = out;
    d->bucket_ms *= 2u;
    d->folds++;
}

void kiln_decimator_push(kiln_decimator_t *d, const kiln_log_sample_t *s)
{
    if (!d || !d->buckets || !s) return;
    if (s->t_rel_ms < d->from_ms) return;
    if (d->to_ms > d->from_ms && s->t_rel_ms > d->to_ms) return;

    if (d->bucket_ms == 0) d->bucket_ms = 1u;   /* cannot happen; cheap to hold */

    const uint32_t off = s->t_rel_ms - d->from_ms;
    uint32_t       i   = off / d->bucket_ms;

    if (i >= d->capacity) {
        if (d->unbounded) {
            /* Make room by halving the resolution, as many times as it takes --
             * at most 32, since bucket_ms doubles each time. */
            while (i >= d->capacity && d->bucket_ms <= (UINT32_MAX / 2u)) {
                fold_pairs(d);
                i = off / d->bucket_ms;
            }
        }
        /* A bounded range cannot grow: a sample at exactly to_ms belongs in the
         * last bucket. */
        if (i >= d->capacity) i = (uint32_t)(d->capacity - 1u);
    }
    const uint16_t idx = (uint16_t)i;

    kiln_log_bucket_t *b = &d->buckets[idx];

    if (b->count == 0) {
        b->t_rel_ms   = s->t_rel_ms;
        b->kiln_min_c = b->kiln_max_c = s->kiln_filt_c;
        b->sp_min_c   = b->sp_max_c   = s->setpoint_c;
        b->case_min_c = b->case_max_c = s->case_c;
        b->cur_min_a  = b->cur_max_a  = s->current_a;
        b->duty_min   = b->duty_max   = s->duty_permille;
        b->state      = s->state;
        b->flags      = s->flags;
        b->current_flags = s->current_flags;
        if (idx + 1u > d->used) d->used = (uint16_t)(idx + 1u);
    } else {
        if (s->kiln_filt_c   < b->kiln_min_c) b->kiln_min_c = s->kiln_filt_c;
        if (s->kiln_filt_c   > b->kiln_max_c) b->kiln_max_c = s->kiln_filt_c;
        if (s->setpoint_c    < b->sp_min_c)   b->sp_min_c   = s->setpoint_c;
        if (s->setpoint_c    > b->sp_max_c)   b->sp_max_c   = s->setpoint_c;
        if (s->case_c        < b->case_min_c) b->case_min_c = s->case_c;
        if (s->case_c        > b->case_max_c) b->case_max_c = s->case_c;
        if (s->current_a     < b->cur_min_a)  b->cur_min_a  = s->current_a;
        if (s->current_a     > b->cur_max_a)  b->cur_max_a  = s->current_a;
        if (s->duty_permille < b->duty_min)   b->duty_min   = s->duty_permille;
        if (s->duty_permille > b->duty_max)   b->duty_max   = s->duty_permille;
        b->flags = (uint8_t)(b->flags | s->flags);
        b->current_flags = (uint8_t)(b->current_flags | s->current_flags);
        b->state = s->state;     /* last state in the bucket */
    }
    b->count++;
    d->accepted++;
}

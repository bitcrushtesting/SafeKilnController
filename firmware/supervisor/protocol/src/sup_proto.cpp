/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <math.h>
#include <string.h>

#include "sup_proto.h"

namespace {

/* Tenths of a degree, saturating, and NaN-safe: a non-finite reading must not
 * reach the cast, which is undefined for it.
 *
 * The NaN test comes first and on its own.  Folding it into the range check
 * relies on a NaN failing every comparison, which is true and is exactly the
 * kind of cleverness that gets simplified away by someone who does not know
 * why it was written. */
constexpr float   ENC_LIMIT_C = 3276.7f;
constexpr int16_t ENC_SAT_HI  = 32767;
constexpr int16_t ENC_SAT_LO  = -32767;

int16_t enc_temp(float c)
{
    if (isnan(c)) {
        return 0;               /* a defined value; not a hot kiln */
    }
    if (c > ENC_LIMIT_C) {
        return ENC_SAT_HI;
    }
    if (c < -ENC_LIMIT_C) {
        return ENC_SAT_LO;
    }
    return static_cast<int16_t>(lroundf(c * SUP_TEMP_SCALE));
}

void put_u16(uint8_t *p, uint16_t v)
{
    /* Widened first: a uint16_t promotes to int before it is shifted. */
    const uint32_t w = v;
    p[0] = (uint8_t)(w & 0xFFu);
    p[1] = (uint8_t)((w >> 8u) & 0xFFu);
}

uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8u));
}

}  // namespace

uint16_t sup_crc16(const uint8_t *data, size_t len)
{
    if (data == nullptr) {
        return 0xFFFFu;
    }
    /* Accumulated in uint32_t: a uint16_t operand promotes to int before it is
     * shifted, which would make the round signed arithmetic. */
    uint32_t c = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        c ^= (uint32_t)data[i] << 8u;
        for (unsigned b = 0; b < 8u; b++) {
            c = ((c & 0x8000u) != 0u) ? ((c << 1u) ^ 0x1021u) : (c << 1u);
            c &= 0xFFFFu;
        }
    }
    return (uint16_t)c;
}

size_t sup_encode(const sup_report_t *r, uint8_t *out, size_t cap)
{
    if ((r == nullptr) || (out == nullptr) || (cap < SUP_FRAME_BYTES)) {
        return 0;
    }
    out[0] = (uint8_t)SUP_SOF;
    out[1] = (uint8_t)SUP_VERSION;
    out[2] = r->seq;
    put_u16(&out[3], (uint16_t)enc_temp(r->chamber_c));
    put_u16(&out[5], (uint16_t)enc_temp(r->cj_c));
    put_u16(&out[7], r->fault_bits);
    out[9]  = r->flags;
    out[10] = (uint8_t)r->trip_reason;
    put_u16(&out[11], sup_crc16(out, 11u));
    return SUP_FRAME_BYTES;
}

size_t sup_decode(const uint8_t *buf, size_t len, sup_report_t *out, size_t *skip)
{
    if (skip != nullptr) {
        *skip = 0;
    }
    if ((buf == nullptr) || (out == nullptr)) {
        return 0;
    }

    /* Slide until a frame validates.  The SOF alone is not trusted: one byte
     * in 256 of line noise looks like one, so the CRC is what decides, and
     * anything before an accepted frame is reported as skippable. */
    for (size_t i = 0; (i + SUP_FRAME_BYTES) <= len; i++) {
        const uint8_t *f = &buf[i];
        if (f[0] != (uint8_t)SUP_SOF) {
            continue;
        }
        if (get_u16(&f[11]) != sup_crc16(f, 11u)) {
            continue;
        }
        out->version     = f[1];
        out->seq         = f[2];
        out->chamber_c   = (float)(int16_t)get_u16(&f[3]) / SUP_TEMP_SCALE;
        out->cj_c        = (float)(int16_t)get_u16(&f[5]) / SUP_TEMP_SCALE;
        out->fault_bits  = get_u16(&f[7]);
        out->flags       = f[9];
        out->trip_reason = (sup_trip_reason_t)f[10];
        if (skip != nullptr) {
            *skip = i;
        }
        return i + SUP_FRAME_BYTES;
    }

    /* Nothing yet.  Keep the last SUP_FRAME_BYTES-1 bytes: a frame may still
     * be arriving, and the rest can never become one. */
    if ((skip != nullptr) && (len >= SUP_FRAME_BYTES)) {
        *skip = len - (SUP_FRAME_BYTES - 1u);
    }
    return 0;
}

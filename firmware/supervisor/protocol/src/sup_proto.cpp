/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "sup_proto.h"

/* No <math.h> and no <string.h>: with the temperatures integer there is nothing
 * left in this file that a C library supplies.  That is not tidiness, it is the
 * point -- see docs/coding-standard.md section 5 on why a safety function
 * should not depend on a library it did not build. */

namespace {

/* The largest q7 that still fits int16_t tenths, and the saturation values.
 *
 * 3276.7 degC is the top of the encoded range; above it the reading saturates
 * rather than wrapping, because a wrapped temperature is a plausible wrong
 * number and a saturated one is obviously an extreme.  Nothing a type K couple
 * and this front end can produce comes close: the clamp exists so that the
 * function is defined for every int32_t, including a value no sensor path can
 * generate, and so that the multiply below cannot overflow.
 *
 * 419417 is 3276.7 * 128, truncated. */
constexpr int32_t ENC_LIMIT_Q7 = 419417;
constexpr int16_t ENC_SAT_HI   = 32767;
constexpr int16_t ENC_SAT_LO   = -32767;

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

int16_t sup_q7_to_dc(int32_t q7)
{
    /* Clamped before the multiply, which is what keeps the multiply in range:
     * five times the limit is 2 097 085, well inside int32_t, and five times an
     * unclamped int32_t would not be. */
    if (q7 > ENC_LIMIT_Q7) {
        return ENC_SAT_HI;
    }
    if (q7 < -ENC_LIMIT_Q7) {
        return ENC_SAT_LO;
    }

    /* 10/128 == 5/64.  Rounded away from zero at the half, symmetrically, by
     * adding half a divisor to the magnitude: C's division truncates towards
     * zero, so the two signs have to be written out rather than sharing one
     * expression.  Dividing a negative numerator and expecting a shift to do
     * the right thing is the mistake this avoids. */
    const int32_t num = q7 * 5;
    const int32_t dc  = (num >= 0) ? ((num + 32) / 64) : -(((-num) + 32) / 64);
    return (int16_t)dc;
}

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
    put_u16(&out[3], (uint16_t)r->chamber_dc);
    put_u16(&out[5], (uint16_t)r->cj_dc);
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
        out->chamber_dc  = (int16_t)get_u16(&f[3]);
        out->cj_dc       = (int16_t)get_u16(&f[5]);
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

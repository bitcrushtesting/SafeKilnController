/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The supervisor link wire format (SWA-22, docs/safety-supervisor.md section 5).
 *
 * One header, two consumers: the STM32 supervisor that emits these frames and
 * the ESP32 adapter that consumes them.  The layout is declared once because a
 * wire format described in two places eventually disagrees with itself.
 *
 * The link is **simplex**.  The supervisor transmits; it has no receive path at
 * all.  With the thermocouple type fixed to K (SWR-ACQ-02) there is nothing the
 * ESP32 could usefully tell it, and having no receiver is a stronger claim than
 * having one and being trusted not to act on what arrives.
 *
 * Sharing this header does not weaken the independence SWA-22 buys.  It is
 * declarative: no trip logic crosses the boundary, and the only shared
 * behaviour is the CRC, whose failure mode is a rejected frame, which both
 * sides already treat as silence.
 */
#ifndef SUP_PROTO_H
#define SUP_PROTO_H

#include <stddef.h>
#include <stdint.h>

/* --- frame ---------------------------------------------------------------
 *
 *   off  size  field
 *   0    1     SOF, 0xA5
 *   1    1     version
 *   2    1     seq          wraps; a repeat or a gap is detectable
 *   3    2     chamber_dc   int16 LE, 0.1 degC
 *   5    2     cj_dc        int16 LE, 0.1 degC
 *   7    2     fault_bits   uint16 LE, KILN_TC_FAULT_* values
 *   9    1     flags        SUP_FLAG_*
 *   10   1     trip_reason  sup_trip_reason_t
 *
 * Two trip reasons were appended for the second couple and the permit readback.
 * The LAYOUT is unchanged and so is SUP_VERSION: appending enum values is
 * backward compatible, and the ESP32's decoder already treats a reason it does
 * not recognise as a trip rather than as OK. The second couple's reading is not
 * carried separately; chamber_dc is the higher of the two, which is the value
 * the backstop acts on. Reporting both would widen the frame, and nothing on the
 * ESP32 side needs the pair yet.
 *   11   2     crc16 LE     CCITT-FALSE over bytes 0..10
 *
 * 13 bytes at 10 Hz is 130 B/s, which is nothing at 115200 baud.  Fixed length
 * means there is no length field to be wrong about, and the receiver resyncs by
 * sliding a window until the CRC checks rather than trusting the SOF alone. */
constexpr size_t  SUP_FRAME_BYTES = 13u;
constexpr uint8_t SUP_SOF         = 0xA5u;
constexpr uint8_t SUP_VERSION     = 1u;

/* Temperatures travel as tenths of a degree, the same encoding the log record
 * uses, so the two never need converting between each other. */
constexpr int32_t SUP_DC_PER_C = 10;

/* --- the supervisor's internal temperature unit ---------------------------
 *
 * 1/128 degC, signed, held in an int32_t, and called q7 after the seven
 * fractional bits.  Everything inside the supervisor measures temperature in
 * it: the decode produces it, the trip logic compares it, and the only
 * conversion is to tenths at the wire.
 *
 * It is the MAX31856's own unit, which is the whole reason to pick it.  The
 * part reports the linearised hot junction as a 19-bit signed value with
 * 2^-7 degC per LSB, so the decode is a shift and nothing else: no scaling, no
 * rounding, and no value that the part can produce and this cannot hold.  The
 * cold junction's 2^-6 degC becomes q7 by a left shift of one, which is exact.
 *
 * This is here, in the header both the trip core and the front-end decode
 * include, because a unit named in two places is a unit that eventually means
 * two things.  It is not on the wire: the wire is tenths, as above.
 *
 * Why integers at all, since this used to be float: on a Cortex-M0+ there is no
 * FPU, so every compare and multiply was a libgcc soft-float call, and the
 * supervisor's safety function depended on a library it did not build.  See
 * docs/coding-standard.md section 5. */
constexpr int32_t SUP_Q7_PER_C = 128;

/* Degrees to q7, for a threshold written as a whole number of degrees.
 *
 * A constexpr function rather than a macro: it is typed, it is scoped, and it
 * evaluates its argument once, none of which a macro can promise.  The whole
 * supervisor is written this way now -- there is not one object-like macro left
 * in core or protocol -- because a macro has no type for the compiler to check
 * and the high-integrity profile says so (cppcoreguidelines-macro-to-enum).
 * There is deliberately no float overload. */
constexpr int32_t sup_c_to_q7(int32_t c)
{
    return c * SUP_Q7_PER_C;
}

/* Reported in every frame, including after a trip: the supervisor keeps talking
 * so the ESP32 can say *why* the heat went away. */
typedef enum {
    SUP_TRIP_NONE = 0,
    SUP_TRIP_OVERTEMP,      /* above the hard-coded backstop                 */
    SUP_TRIP_TC_FAULT,      /* front end reported a fault, past its grace     */
    SUP_TRIP_SENSOR_STALE,  /* a reading was working and stopped              */
    SUP_TRIP_SELF_TEST,     /* start-up self-test failed; never permits       */
    SUP_TRIP_TC_DISAGREE,   /* the two chamber couples do not agree           */
    SUP_TRIP_PERMIT_STUCK,  /* the permit was withdrawn and the coil stayed on */
    SUP_TRIP_COUNT
} sup_trip_reason_t;

/* uint32_t, not the uint8_t field they live in: a narrower constant promotes
 * to *int* before a bitwise operator, so composing a mask would be signed
 * arithmetic.  Same reasoning as the KILN_TC_FAULT_* masks in kiln/types.h. */
/* The values the fault_bits field carries.
 *
 * These were previously named only in a comment pointing at KILN_TC_FAULT_* in
 * kiln/types.h, which is the one kind of coupling a wire format must not have:
 * a field whose meaning is written down on one side of a link and referred to
 * from the other eventually disagrees across it, and the disagreement is a
 * misreported fault rather than a build error.
 *
 * So they are declared here, in the header both sides already share, and
 * kiln_core/suplink.cpp static_asserts that each one equals its KILN_TC_FAULT_*
 * counterpart. The agreement is now the compiler's to check.
 *
 * uint32_t for the same reason as the flags below: a narrower constant promotes
 * to int before a bitwise operator. */
constexpr uint32_t SUP_TC_FAULT_OPEN      = 1u << 0u; /* open circuit          */
constexpr uint32_t SUP_TC_FAULT_SHORT_VCC = 1u << 1u; /* short to supply       */
constexpr uint32_t SUP_TC_FAULT_SHORT_GND = 1u << 2u; /* short to ground       */
constexpr uint32_t SUP_TC_FAULT_CJ_RANGE  = 1u << 3u; /* cold junction range   */
constexpr uint32_t SUP_TC_FAULT_TC_RANGE  = 1u << 4u; /* thermocouple range    */
constexpr uint32_t SUP_TC_FAULT_OVUV      = 1u << 5u; /* over/under voltage    */
constexpr uint32_t SUP_TC_FAULT_COMMS     = 1u << 6u; /* front end silent      */

constexpr uint32_t SUP_FLAG_PERMIT      = 1u << 0u; /* permitting heat now     */
constexpr uint32_t SUP_FLAG_TRIPPED     = 1u << 1u; /* latched; local clear    */
constexpr uint32_t SUP_FLAG_TC_VALID    = 1u << 2u; /* reading is usable       */
constexpr uint32_t SUP_FLAG_SELFTEST_OK = 1u << 3u;
/* SWR-SAF-37: the second chamber couple is usable. Clear means the supervisor is
 * running on one channel: still protecting, but with no cross-check, so the
 * ESP32 should say so rather than let a degraded state look like a healthy one. */
constexpr uint32_t SUP_FLAG_TC2_VALID   = 1u << 4u;

/* Tenths of a degree, as on the wire, rather than degrees in a float.
 *
 * The struct carries the encoded unit so that sup_encode is a byte copy and
 * holds no arithmetic at all: the one place a temperature is scaled is
 * sup_q7_to_dc below, which is where the rounding and the saturation can be
 * tested on their own.  The consumer converts to whatever it wants at its own
 * boundary, which on the ESP32 is a processor with an FPU. */
typedef struct {
    uint8_t           version;
    uint8_t           seq;
    int16_t           chamber_dc;
    int16_t           cj_dc;
    uint16_t          fault_bits;
    uint8_t           flags;
    sup_trip_reason_t trip_reason;
} sup_report_t;

/* q7 to tenths of a degree: rounded to nearest, away from zero at the half,
 * and saturating at the ends of int16_t.
 *
 * Total by construction, which is the property that matters and the reason it
 * is a function with its own tests rather than an expression at the call site.
 * The float version it replaces had to begin by asking whether its argument was
 * a NaN, because the cast of one is undefined; an int32_t has no such value, so
 * every input now has a defined output and the check is gone rather than merely
 * passing.
 *
 * The scaling needs no division: 10/128 is 5/64 exactly, so it is a multiply by
 * five and a shift of six. */
int16_t sup_q7_to_dc(int32_t q7);

/* CCITT-FALSE, the same polynomial and seed the log records use.  Carried here
 * rather than shared with kiln_core so the supervisor depends on nothing of
 * the ESP32's; the two implementations are checked against one vector in the
 * host tests of both projects. */
uint16_t sup_crc16(const uint8_t *data, size_t len);

/* Writes exactly SUP_FRAME_BYTES.  Returns bytes written, or 0 if out is null. */
size_t sup_encode(const sup_report_t *r, uint8_t *out, size_t cap);

/* Decodes one frame from the front of `buf`.
 *
 * Returns the number of bytes consumed, which is 0 when there is not yet a
 * valid frame at the front.  `*skip` is set to how many bytes the caller
 * should discard before trying again: that is how a receiver resyncs after
 * line noise without needing an escape scheme. */
size_t sup_decode(const uint8_t *buf, size_t len, sup_report_t *out, size_t *skip);

#endif /* SUP_PROTO_H */

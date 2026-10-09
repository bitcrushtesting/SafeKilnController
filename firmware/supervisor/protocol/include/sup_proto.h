/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file sup_proto.h
 * @brief The supervisor link wire format, and the units either side of it.
 *
 * @derivedfrom SWA-22, the independent safety supervisor, and
 *              `docs/safety-supervisor.md` section 5.
 * @safetyclass Carries the output of a safety function. A frame that decodes
 *              wrongly is a misreported trip, so the encoding is specified
 *              here once and checked by both sides' tests.
 *
 * One header, two consumers: the STM32 supervisor that emits these frames and
 * the ESP32 adapter that consumes them. The layout is declared once because a
 * wire format described in two places eventually disagrees with itself.
 *
 * The link is **simplex**. The supervisor transmits; it has no receive path at
 * all. With the thermocouple type fixed to K (SWR-ACQ-02) there is nothing the
 * ESP32 could usefully tell it, and having no receiver is a stronger claim
 * than having one and being trusted not to act on what arrives.
 *
 * Sharing this header does not weaken the independence SWA-22 buys. It is
 * declarative: no trip logic crosses the boundary, and the only shared
 * behaviour is the CRC, whose failure mode is a rejected frame, which both
 * sides already treat as silence.
 *
 * @par The frame
 *
 *      off  size  field
 *      0    1     SOF, 0xA5
 *      1    1     version
 *      2    1     seq          wraps; a repeat or a gap is detectable
 *      3    2     chamber_dc   int16 LE, 0.1 degC
 *      5    2     cj_dc        int16 LE, 0.1 degC
 *      7    2     fault_bits   uint16 LE, SUP_TC_FAULT_* values
 *      9    1     flags        SUP_FLAG_*
 *      10   1     trip_reason  sup_trip_reason_t
 *      11   2     crc16 LE     CCITT-FALSE over bytes 0..10
 *
 * 13 bytes at 10 Hz is 130 B/s, which is nothing at 115200 baud. Fixed length
 * means there is no length field to be wrong about, and the receiver resyncs
 * by sliding a window until the CRC checks rather than trusting the SOF alone.
 *
 * Two trip reasons were appended for the second couple and the permit
 * readback. The **layout is unchanged** and so is @ref SUP_VERSION : appending
 * enum values is backward compatible, and the ESP32's decoder already treats a
 * reason it does not recognise as a trip rather than as OK. The second
 * couple's reading is not carried separately; `chamber_dc` is the higher of
 * the two, which is the value the backstop acts on. Reporting both would widen
 * the frame, and nothing on the ESP32 side needs the pair yet.
 */
#ifndef SUP_PROTO_H
#define SUP_PROTO_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Length of one frame on the wire: 13 bytes, always.
 * @rangeof Compile-time constant. There is no short frame and no long one.
 * @rationale
 * Fixed length means there is no length field to be wrong about, and no way
 * for a corrupted one to make the receiver wait for bytes that never come.
 * @implements SWR-SAF-38
 * @verifiedby `test_proto.cpp`: every encode writes exactly this many bytes.
 */
constexpr size_t  SUP_FRAME_BYTES = 13u;

/**
 * @brief Start-of-frame byte, 0xA5.
 * @rangeof Compile-time constant.
 * @rationale
 * An alternating bit pattern rather than 0x00 or 0xFF, both of which a stuck
 * or floating line produces on its own. It is a hint for resynchronisation and
 * not a guarantee: @ref sup_decode trusts the CRC, not this byte.
 * @verifiedby `test_proto.cpp`, including a payload byte that happens to equal
 *             the SOF.
 */
constexpr uint8_t SUP_SOF         = 0xA5u;

/**
 * @brief Wire format version carried in every frame.
 * @rangeof 1. Compile-time constant.
 * @rationale
 * Appending trip reasons did **not** bump this, deliberately: the layout is
 * unchanged and a decoder that does not know a reason already treats it as a
 * trip. A version bump is for a change that moves a field, and reserving it
 * for that keeps it meaningful.
 * @errorbehaviour
 * A receiver seeing a version it does not know should discard the frame, which
 * is the same thing it does with silence.
 * @implements SWR-SAF-38
 */
constexpr uint8_t SUP_VERSION     = 1u;

/**
 * @brief Wire temperature unit: tenths of a degree Celsius.
 * @rangeof Compile-time constant, 10 counts per degree.
 * @rationale
 * The same encoding the log record uses, so the two never need converting
 * between each other. The wire is tenths and the supervisor's interior is q7;
 * @ref sup_q7_to_dc is the single place the two meet.
 */
constexpr int32_t SUP_DC_PER_C = 10;

/**
 * @brief The supervisor's internal temperature unit: 1/128 degC, signed.
 *
 * @rangeof Compile-time constant, 128 counts per degree. Held in an `int32_t`
 *          and called q7 after its seven fractional bits.
 *
 * @rationale
 * It is the MAX31856's own unit, which is the whole reason to pick it. The
 * part reports the linearised hot junction as a 19-bit signed value with
 * 2^-7 degC per LSB, so the decode is a shift and nothing else: no scaling, no
 * rounding, and no value the part can produce that this cannot hold. The cold
 * junction's 2^-6 degC becomes q7 by a left shift of one, which is exact.
 *
 * It is declared here, in the header both the trip core and the front-end
 * decode include, because a unit named in two places is a unit that eventually
 * means two things.
 *
 * Why integers at all, since this used to be float: on a Cortex-M0+ there is
 * no FPU, so every compare and multiply was a libgcc soft-float call, and the
 * supervisor's safety function depended on a library it did not build.
 * `docs/coding-standard.md` section 5 has the argument.
 *
 * @verifiedby `test_proto.cpp`, over the conversions in both directions.
 */
constexpr int32_t SUP_Q7_PER_C = 128;

/**
 * @brief Convert whole degrees Celsius to q7.
 *
 * @param[in] c Temperature in whole degrees Celsius.
 * @return The same temperature in 1/128 degC counts.
 *
 * @rangeof `c` is any `int32_t` whose product with 128 is representable, which
 *          covers every temperature a kiln or a thermocouple can reach by four
 *          orders of magnitude. Callers pass literals.
 *
 * @errorbehaviour
 * None to speak of: the operation is a multiply with no failure mode in the
 * intended range. It is deliberately **not** defended against overflow,
 * because every call site in the firmware passes a compile-time literal and a
 * runtime check would suggest otherwise.
 *
 * @rationale
 * A `constexpr` function rather than a macro: it is typed, it is scoped, and
 * it evaluates its argument once, none of which a macro can promise. The whole
 * supervisor is written this way now, with not one object-like macro left in
 * `core` or `protocol`, because a macro has no type for the compiler to check
 * and the high-integrity profile says so. There is deliberately no float
 * overload.
 *
 * Thresholds are written through this so they stay readable as the numbers the
 * safety concept states, and the multiply is the compiler's.
 *
 * @verifiedby `test_proto.cpp`, round-tripped against @ref sup_q7_to_dc.
 */
constexpr int32_t sup_c_to_q7(int32_t c)
{
    return c * SUP_Q7_PER_C;
}

/**
 * @brief Why the supervisor tripped, reported in every frame.
 *
 * @rationale
 * Reported even after a trip, and especially then: the supervisor keeps
 * talking so the ESP32 can say *why* the heat went away. A backstop that goes
 * silent when it acts leaves the operator with a dead kiln and no sentence to
 * read.
 *
 * @errorbehaviour
 * Values are appended, never renumbered. A decoder meeting a reason it does
 * not know treats it as a trip rather than as OK, so a newer supervisor talking
 * to an older controller fails towards refusing heat.
 *
 * @implements SWR-SAF-38
 */
typedef enum {
    SUP_TRIP_NONE = 0,      /**< Not tripped. */
    SUP_TRIP_OVERTEMP,      /**< Above the hard-coded backstop. */
    SUP_TRIP_TC_FAULT,      /**< Front end reported a fault, past its grace. */
    SUP_TRIP_SENSOR_STALE,  /**< A reading was working and stopped. */
    SUP_TRIP_SELF_TEST,     /**< Start-up self-test failed; never permits. */
    SUP_TRIP_TC_DISAGREE,   /**< The two chamber couples do not agree. */
    SUP_TRIP_PERMIT_STUCK,  /**< Permit withdrawn and the coil stayed on. */
    SUP_TRIP_COUNT          /**< Count of reasons; not a reason. */
} sup_trip_reason_t;

/**
 * @name Front-end fault bits, carried in the frame's `fault_bits` field
 *
 * @rationale
 * These were previously named only in a comment pointing at `KILN_TC_FAULT_*`
 * in `kiln/types.h`, which is the one kind of coupling a wire format must not
 * have: a field whose meaning is written down on one side of a link and
 * referred to from the other eventually disagrees across it, and the
 * disagreement is a misreported fault rather than a build error.
 *
 * So they are declared here, in the header both sides already share, and
 * `kiln_core/suplink.cpp` `static_assert`s that each equals its
 * `KILN_TC_FAULT_*` counterpart. The agreement is the compiler's to check.
 *
 * `uint32_t` rather than the `uint16_t` field they live in: a narrower
 * constant promotes to **int** before a bitwise operator, so composing a mask
 * would be signed arithmetic.
 *
 * @implements SWR-SAF-22
 * @verifiedby `test_max31856.cpp` decodes each one from the register bits;
 *             `kiln_core/suplink.cpp` asserts they match the other side.
 * @{
 */
constexpr uint32_t SUP_TC_FAULT_OPEN      = 1u << 0u; /**< Open circuit. */
constexpr uint32_t SUP_TC_FAULT_SHORT_VCC = 1u << 1u; /**< Short to supply. */
constexpr uint32_t SUP_TC_FAULT_SHORT_GND = 1u << 2u; /**< Short to ground. */
constexpr uint32_t SUP_TC_FAULT_CJ_RANGE  = 1u << 3u; /**< Cold junction out of range. */
constexpr uint32_t SUP_TC_FAULT_TC_RANGE  = 1u << 4u; /**< Thermocouple out of range. */
constexpr uint32_t SUP_TC_FAULT_OVUV      = 1u << 5u; /**< Over or under voltage. */
constexpr uint32_t SUP_TC_FAULT_COMMS     = 1u << 6u; /**< Front end silent. */
/** @} */

/**
 * @name Status flags, carried in the frame's `flags` field
 *
 * @rationale
 * Derived from the trip state on demand by `sup_flags` rather than tracked
 * alongside it: a second copy of the truth can disagree with the first, and it
 * would disagree exactly when the frame mattered most, which is after a trip.
 *
 * `uint32_t` for the same promotion reason as the fault bits above.
 *
 * @implements SWR-SAF-38
 * @verifiedby `test_trip.cpp` and `test_proto.cpp`.
 * @{
 */
constexpr uint32_t SUP_FLAG_PERMIT      = 1u << 0u; /**< Permitting heat right now. */
constexpr uint32_t SUP_FLAG_TRIPPED     = 1u << 1u; /**< Latched; cleared locally only. */
constexpr uint32_t SUP_FLAG_TC_VALID    = 1u << 2u; /**< Chamber reading is usable. */
constexpr uint32_t SUP_FLAG_SELFTEST_OK = 1u << 3u; /**< Start-up self-test passed. */
/**
 * @brief The second chamber couple is usable.
 *
 * Clear means the supervisor is running on one channel: still protecting, but
 * with no cross-check, so the ESP32 should say so rather than let a degraded
 * state look like a healthy one (SWR-SAF-37).
 */
constexpr uint32_t SUP_FLAG_TC2_VALID   = 1u << 4u;
/** @} */

/**
 * @brief One decoded report: the frame's fields, in the frame's own units.
 *
 * @rationale
 * Temperatures are tenths of a degree, as on the wire, rather than degrees in
 * a float. The struct carries the encoded unit so that @ref sup_encode is a
 * byte copy and holds no arithmetic at all: the one place a temperature is
 * scaled is @ref sup_q7_to_dc, which is where the rounding and the saturation
 * can be tested on their own. The consumer converts to whatever it wants at
 * its own boundary, which on the ESP32 is a processor with an FPU.
 */
typedef struct {
    uint8_t           version;      /**< @ref SUP_VERSION as sent. */
    uint8_t           seq;          /**< Wraps; a repeat or a gap is detectable. */
    int16_t           chamber_dc;   /**< Chamber temperature, 0.1 degC, the higher of the two couples. */
    int16_t           cj_dc;        /**< Cold junction temperature, 0.1 degC. */
    uint16_t          fault_bits;   /**< `SUP_TC_FAULT_*` set. */
    uint8_t           flags;        /**< `SUP_FLAG_*` set. */
    sup_trip_reason_t trip_reason;  /**< Why it tripped, or @ref SUP_TRIP_NONE. */
} sup_report_t;

/**
 * @brief Convert q7 to tenths of a degree, rounded and saturating.
 *
 * @param[in] q7 Temperature in 1/128 degC counts.
 * @return The same temperature in 0.1 degC, clamped to `int16_t`.
 * @retval 32767 The input was above what the wire can carry.
 * @retval -32767 The input was below it.
 *
 * @rangeof Accepts the whole of `int32_t`. Rounds to nearest, away from zero
 *          at the half, and saturates at the ends of `int16_t`.
 *
 * @errorbehaviour
 * **Total by construction**, which is the property that matters and the reason
 * this is a function with its own tests rather than an expression at the call
 * site. The float version it replaces had to begin by asking whether its
 * argument was a NaN, because the cast of one is undefined; an `int32_t` has no
 * such value, so every input now has a defined output and the check is gone
 * rather than merely passing.
 *
 * @rationale
 * The scaling needs no division: 10/128 is 5/64 exactly, so it is a multiply
 * by five and a shift of six.
 *
 * @verifiedby `test_proto.cpp`, at the saturation boundaries in both
 *             directions and at the rounding half.
 */
int16_t sup_q7_to_dc(int32_t q7);

/**
 * @brief CRC-16 CCITT-FALSE over a buffer.
 *
 * @param[in] data Bytes to sum. Must not be NULL when `len` is non-zero.
 * @param[in] len  Number of bytes.
 * @return The checksum.
 *
 * @rangeof Any length. The polynomial and seed are the ones the log records
 *          use.
 *
 * @rationale
 * Carried here rather than shared with `kiln_core` so the supervisor depends
 * on nothing of the ESP32's. The two implementations are checked against one
 * common vector in the host tests of both projects, which is the cheapest
 * arrangement that keeps them equal without coupling them.
 *
 * @verifiedby `test_proto.cpp` against a known vector, and the same vector in
 *             the controller's suite.
 */
uint16_t sup_crc16(const uint8_t *data, size_t len);

/**
 * @brief Encode one report into a frame.
 *
 * @param[in]  r   Report to encode.
 * @param[out] out Buffer of at least @ref SUP_FRAME_BYTES bytes.
 * @param[in]  cap Capacity of `out`.
 * @return Bytes written.
 * @retval 0 `out` was NULL, or `cap` was too small to hold a frame.
 *
 * @errorbehaviour
 * Writes nothing and returns zero rather than writing a partial frame. A
 * truncated frame would fail the receiver's CRC anyway, so the only thing a
 * partial write could add is a buffer overrun.
 *
 * @sideeffects
 * Writes exactly @ref SUP_FRAME_BYTES bytes to `out` on success. No allocation.
 *
 * @verifiedby `test_proto.cpp`, including the undersized-buffer and NULL cases.
 */
size_t sup_encode(const sup_report_t *r, uint8_t *out, size_t cap);

/**
 * @brief Decode one frame from the front of a buffer.
 *
 * @param[in]  buf  Received bytes.
 * @param[in]  len  How many are available.
 * @param[out] out  Filled in only when a whole valid frame was found.
 * @param[out] skip How many bytes the caller should discard before trying
 *                  again.
 * @return Bytes consumed.
 * @retval 0 There is not yet a valid frame at the front of `buf`.
 *
 * @errorbehaviour
 * The CRC decides, not the SOF byte. When the front of the buffer is not a
 * valid frame, `*skip` says how far to slide the window, which is how a
 * receiver resynchronises after line noise without needing an escape scheme.
 * A payload byte that happens to equal @ref SUP_SOF therefore costs a
 * resynchronisation and never a wrong decode.
 *
 * @rationale
 * Returning a consumed count and a skip count, rather than a boolean, keeps
 * the buffer management in the caller where the buffer is, and keeps this
 * function free of state between calls. It can be tested by handing it bytes.
 *
 * @verifiedby `test_proto.cpp`: a clean frame, a frame behind noise, a frame
 *             with one bit flipped, and a buffer holding only part of one.
 */
size_t sup_decode(const uint8_t *buf, size_t len, sup_report_t *out, size_t *skip);

#endif /* SUP_PROTO_H */

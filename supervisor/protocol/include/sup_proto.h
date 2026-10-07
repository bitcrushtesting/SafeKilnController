/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The supervisor link wire format (AD-22, docs/safety-supervisor.md section 5).
 *
 * One header, two consumers: the STM32 supervisor that emits these frames and
 * the ESP32 adapter that consumes them.  The layout is declared once because a
 * wire format described in two places eventually disagrees with itself.
 *
 * The link is **simplex**.  The supervisor transmits; it has no receive path at
 * all.  With the thermocouple type fixed to K (FR-ACQ-02) there is nothing the
 * ESP32 could usefully tell it, and having no receiver is a stronger claim than
 * having one and being trusted not to act on what arrives.
 *
 * Sharing this header does not weaken the independence AD-22 buys.  It is
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
 *   11   2     crc16 LE     CCITT-FALSE over bytes 0..10
 *
 * 13 bytes at 10 Hz is 130 B/s, which is nothing at 115200 baud.  Fixed length
 * means there is no length field to be wrong about, and the receiver resyncs by
 * sliding a window until the CRC checks rather than trusting the SOF alone. */
#define SUP_FRAME_BYTES 13u
#define SUP_SOF         0xA5u
#define SUP_VERSION     1u

/* Temperatures travel as tenths of a degree, the same encoding the log record
 * uses, so the two never need converting between each other. */
#define SUP_TEMP_SCALE  10.0f

/* Reported in every frame, including after a trip: the supervisor keeps talking
 * so the ESP32 can say *why* the heat went away. */
typedef enum {
    SUP_TRIP_NONE = 0,
    SUP_TRIP_OVERTEMP,      /* above the hard-coded backstop                 */
    SUP_TRIP_TC_FAULT,      /* front end reported a fault, past its grace     */
    SUP_TRIP_LID_OPEN,      /* lid open, past its confirmation period         */
    SUP_TRIP_SENSOR_STALE,  /* no good conversion within the grace period     */
    SUP_TRIP_SELF_TEST,     /* start-up self-test failed; never permits       */
    SUP_TRIP_COUNT
} sup_trip_reason_t;

#define SUP_FLAG_PERMIT      (1u << 0) /* supervisor is permitting heat now   */
#define SUP_FLAG_LID_OPEN    (1u << 1)
#define SUP_FLAG_TRIPPED     (1u << 2) /* latched; needs a local clear        */
#define SUP_FLAG_TC_VALID    (1u << 3) /* chamber reading is usable           */
#define SUP_FLAG_SELFTEST_OK (1u << 4)

typedef struct {
    uint8_t           version;
    uint8_t           seq;
    float             chamber_c;
    float             cj_c;
    uint16_t          fault_bits;
    uint8_t           flags;
    sup_trip_reason_t trip_reason;
} sup_report_t;

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

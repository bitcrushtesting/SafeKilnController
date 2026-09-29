/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Log record codec and decimation -- architecture sections 10.2, 10.5.
 *
 * Fixed 20 byte records (AD-18), 4 byte aligned so the flash driver never needs
 * a read-modify-write, with a CRC8 per record so a power cut damages at most the
 * record in flight (FR-LOG-08).
 */
#ifndef KILN_CORE_LOGREC_H
#define KILN_CORE_LOGREC_H

#include <stddef.h>
#include "kiln/err.h"
#include "kiln/types.h"
#include "kiln_ports/port_logstore.h"

#define KILN_LOG_MAGIC          0x474F4C4BU    /* "KLOG" little endian */
#define KILN_LOG_FORMAT_VERSION 1

/* Flags packed into the high nibble of state_flags. */
#define KILN_LOGF_HOLDBACK   (1u << 4)
#define KILN_LOGF_SATURATED  (1u << 5)
#define KILN_LOGF_TC_FAULT   (1u << 6)
#define KILN_LOGF_WALL_VALID (1u << 7)

typedef struct {
    uint32_t t_rel_ms;      /* since run start; 49 days of range */
    float    kiln_raw_c;
    float    kiln_filt_c;
    float    setpoint_c;
    float    case_c;
    float    current_a;     /* FR-CUR-09 */
    uint16_t duty_permille;
    uint8_t  segment;       /* KILN_SEG_NONE when not applicable */
    uint8_t  state;         /* kiln_state_t */
    uint8_t  flags;         /* KILN_LOGF_* (high nibble semantics) */
    uint8_t  current_flags; /* KILN_CURF_* */
} kiln_log_sample_t;

/* Encode / decode one record.  decode returns KILN_ERR_CORRUPT on a bad CRC or
 * an all-0xFF (erased, or torn) slot. */
void       kiln_logrec_encode(const kiln_log_sample_t *s, uint8_t out[KILN_LOG_RECORD_BYTES]);
kiln_err_t kiln_logrec_decode(const uint8_t rec[KILN_LOG_RECORD_BYTES], kiln_log_sample_t *out);

/* True for a slot that has never been written. */
bool kiln_logrec_is_erased(const uint8_t rec[KILN_LOG_RECORD_BYTES]);

typedef struct {
    uint32_t magic;
    uint32_t seq;           /* monotonic sector sequence: defines ring order */
    uint32_t run_id;
    uint16_t format_version;
} kiln_log_sector_hdr_t;

void       kiln_logrec_encode_hdr(const kiln_log_sector_hdr_t *h, uint8_t out[KILN_LOG_HEADER_BYTES]);
kiln_err_t kiln_logrec_decode_hdr(const uint8_t in[KILN_LOG_HEADER_BYTES], kiln_log_sector_hdr_t *out);

uint8_t  kiln_crc8(const uint8_t *data, size_t len);
uint16_t kiln_crc16(const uint8_t *data, size_t len);

/* --- decimation (FR-LOG-10, FR-LOG-11) ---------------------------------- */

/* One output bucket.  Min and max are carried for every series so that a brief
 * excursion survives downsampling instead of being averaged away -- which is
 * the whole point of FR-LOG-11. */
typedef struct {
    uint32_t t_rel_ms;          /* first sample in the bucket */
    float    kiln_min_c, kiln_max_c;
    float    sp_min_c,   sp_max_c;
    float    case_min_c, case_max_c;
    float    cur_min_a,  cur_max_a;
    uint16_t duty_min, duty_max;
    uint16_t count;
    uint8_t  state;
    uint8_t  flags;             /* OR of the bucket's flags */
    uint8_t  current_flags;     /* OR of the bucket's current flags */
} kiln_log_bucket_t;

typedef struct {
    kiln_log_bucket_t *buckets;
    uint16_t           capacity;     /* max_points requested by the caller */
    uint16_t           used;
    uint32_t           from_ms, to_ms;
    uint32_t           bucket_ms;
    uint32_t           accepted;     /* samples that landed in a bucket */
} kiln_decimator_t;

/* from_ms/to_ms bound the range; max_points is the caller's pixel budget.
 * Passing to_ms == 0 means "unbounded", resolved on the first sample. */
void kiln_decimator_init(kiln_decimator_t *d, kiln_log_bucket_t *storage,
                         uint16_t max_points, uint32_t from_ms, uint32_t to_ms);
void kiln_decimator_push(kiln_decimator_t *d, const kiln_log_sample_t *s);

#endif

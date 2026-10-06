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

constexpr uint32_t KILN_LOG_MAGIC          = 0x474F4C4BU;  /* "KLOG" little endian */
constexpr uint16_t KILN_LOG_FORMAT_VERSION = 1;

/* Flags packed into the high nibble of state_flags. */
constexpr uint32_t KILN_LOGF_HOLDBACK   = 1u << 4u;
constexpr uint32_t KILN_LOGF_SATURATED  = 1u << 5u;
constexpr uint32_t KILN_LOGF_TC_FAULT   = 1u << 6u;
constexpr uint32_t KILN_LOGF_WALL_VALID = 1u << 7u;

/* FR-LOG-04: besides the periodic sample, a record is written out of band on
 * every state transition, fault, warning, configuration change and operator
 * action.  The code says which, so the log can be read as a narrative rather
 * than as a temperature series with unexplained steps in it -- and it costs
 * nothing: it occupies the byte AD-18's layout was already carrying as
 * reserved. */
typedef enum {
    KILN_LOGE_SAMPLE = 0,       /* the periodic sample of FR-LOG-03 */
    KILN_LOGE_RUN_START,
    KILN_LOGE_RUN_END,
    KILN_LOGE_STATE_CHANGE,
    KILN_LOGE_FAULT,
    KILN_LOGE_WARNING,
    KILN_LOGE_CONFIG_CHANGE,
    KILN_LOGE_OPERATOR,
    KILN_LOGE_COUNT,
} kiln_log_event_t;

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
    uint8_t  event;         /* kiln_log_event_t (FR-LOG-04) */
} kiln_log_sample_t;

/* Encode / decode one record.
 *
 * decode distinguishes the two ways a slot can fail to hold a record, because
 * the difference matters to the reader: KILN_ERR_NOT_FOUND for an all-0xFF slot,
 * which is simply the end of what has been written, and KILN_ERR_CORRUPT for a
 * bad CRC, which is a torn write or bit rot and means iteration should skip this
 * record and carry on rather than stop. */
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

constexpr uint16_t KILN_CRC16_INIT = 0xFFFFu;

uint8_t  kiln_crc8(const uint8_t *data, size_t len);
uint16_t kiln_crc16(const uint8_t *data, size_t len);
uint16_t kiln_crc16_update(uint16_t crc, const uint8_t *data, size_t len);

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
    uint16_t           folds;        /* doublings so far, for diagnostics */
    bool               unbounded;    /* to_ms was 0: width grows as needed */
} kiln_decimator_t;

/* from_ms/to_ms bound the range; max_points is the caller's pixel budget.
 *
 * to_ms == 0 means "unbounded" -- the usual case for "show me this run", where
 * the caller does not know the extent before reading it.  The bucket width then
 * starts fine and *doubles* whenever the buckets fill, folding adjacent pairs
 * together as it goes.  That costs one O(capacity) pass per doubling, which is
 * about seventeen passes for a 10 h run, and in exchange an open-ended query
 * returns a downsampled view of the whole run instead of the first N samples of
 * it -- which is what FR-LOG-10 asks for and what a chart needs.
 *
 * Extrema survive folding: a bucket's min and max are the min and max of the two
 * it was built from, so the brief excursion FR-LOG-11 cares about is still there
 * at any zoom level.
 *
 * Zeroes `storage` for `max_points` buckets, so the caller may pass ordinary
 * uninitialised stack memory.
 *
 * Returns KILN_ERR_INVALID_ARG for a NULL target or storage. */
kiln_err_t kiln_decimator_init(kiln_decimator_t *d, kiln_log_bucket_t *storage,
                               uint16_t max_points, uint32_t from_ms, uint32_t to_ms);
void kiln_decimator_push(kiln_decimator_t *d, const kiln_log_sample_t *s);

#endif

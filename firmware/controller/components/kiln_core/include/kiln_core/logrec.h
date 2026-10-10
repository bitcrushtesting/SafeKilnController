/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Log record codec and decimation -- architecture sections 10.2, 10.5.
 *
 * Fixed 20 byte records (SWA-18), 4 byte aligned so the flash driver never needs
 * a read-modify-write, with a CRC8 per record so a power cut damages at most the
 * record in flight (SWR-LOG-08).
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

/* SWR-LOG-04: besides the periodic sample, a record is written out of band on
 * every state transition, fault, warning, configuration change and operator
 * action.  The code says which, so the log can be read as a narrative rather
 * than as a temperature series with unexplained steps in it -- and it costs
 * nothing: it occupies the byte SWA-18's layout was already carrying as
 * reserved. */
typedef enum {
    KILN_LOGE_SAMPLE = 0,       /* the periodic sample of SWR-LOG-03 */
    KILN_LOGE_RUN_START,
    KILN_LOGE_RUN_END,
    KILN_LOGE_STATE_CHANGE,
    KILN_LOGE_FAULT,
    KILN_LOGE_WARNING,
    KILN_LOGE_CONFIG_CHANGE,
    KILN_LOGE_OPERATOR,

    /* SWR-LOG-16: security-relevant events, distinguishable from the process
     * events above because they are the ones an owner may switch off.
     *
     * New codes rather than a flag, and appended rather than inserted: the
     * event byte is persisted, so 0 to 7 keep the meaning every log already
     * written gives them.  A reader that does not know these codes shows the
     * number, which is why logdump names an unknown event rather than
     * dropping the record.
     *
     * The update-path events the requirement also lists -- an offer, a
     * confirmation, an install, a refusal with its version and reason, a
     * failed signature, a rollback -- are NOT here, and not by oversight: a
     * version string and a refusal reason do not fit a 20 byte record
     * (SWA-18), so they arrive with the update path itself (tasklist H2) and
     * will need either a second record type or their own channel. Recording
     * that here is better than inventing a code now that carries no detail. */
    KILN_LOGE_SEC_RESET,        /* an abnormal reset cause (SWR-NFR-15)      */
    KILN_LOGE_SEC_ERASE,        /* a factory reset (SWR-CFG-09)              */
    KILN_LOGE_SEC_NET_JOIN,     /* a network join commanded at the display   */

    KILN_LOGE_COUNT,
} kiln_log_event_t;

/* Is this one of the events SWR-LOG-16 lets the owner switch off?
 *
 * The configuration change is included: it is a process event and a security
 * event at once, and the requirement lists it under security, which is the
 * reading that matters -- somebody who does not want their device recording
 * what they change about it has said so.
 *
 * A fault, a state change or a sample is NOT included, whatever the owner
 * asks: those are the evidence of what a kiln did, which is a safety record
 * rather than a record of access to the device. */
static inline bool kiln_log_event_is_security(kiln_log_event_t e)
{
    return (e == KILN_LOGE_CONFIG_CHANGE) || (e == KILN_LOGE_SEC_RESET) ||
           (e == KILN_LOGE_SEC_ERASE)     || (e == KILN_LOGE_SEC_NET_JOIN);
}

typedef struct {
    uint32_t t_rel_ms;      /* since run start; 49 days of range */
    float    kiln_raw_c;
    float    kiln_filt_c;
    float    setpoint_c;
    float    case_c;
    float    current_a;     /* SWR-CUR-09 */
    uint16_t duty_permille;
    uint8_t  segment;       /* KILN_SEG_NONE when not applicable */
    uint8_t  state;         /* kiln_state_t */
    uint8_t  flags;         /* KILN_LOGF_* (high nibble semantics) */
    uint8_t  current_flags; /* KILN_CURF_* */
    uint8_t  event;         /* kiln_log_event_t (SWR-LOG-04) */
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

/* --- decimation (SWR-LOG-10, SWR-LOG-11) ---------------------------------- */

/* One output bucket.  Min and max are carried for every series so that a brief
 * excursion survives downsampling instead of being averaged away -- which is
 * the whole point of SWR-LOG-11. */
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
 * it -- which is what SWR-LOG-10 asks for and what a chart needs.
 *
 * Extrema survive folding: a bucket's min and max are the min and max of the two
 * it was built from, so the brief excursion SWR-LOG-11 cares about is still there
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

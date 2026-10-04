/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <string.h>
#include "kiln_app/settings.h"
#include "kiln_core/logrec.h"      /* kiln_crc16 */

/* --- configuration ----------------------------------------------------- */

kiln_err_t kiln_settings_load(const kiln_port_kvstore_t *kv, kiln_config_t *cfg)
{
    if (!cfg) return KILN_ERR_INVALID_ARG;

    /* Defaults first, so every failure path below leaves something usable. */
    kiln_config_defaults(cfg);
    if (!kv || !kv->get) return KILN_ERR_INVALID_ARG;

    uint8_t blob[sizeof(kiln_config_t) + 32];
    size_t  len = 0;

    const kiln_err_t e = kv->get(kv->ctx, KILN_NVS_NAMESPACE, KILN_NVS_KEY_CONFIG,
                                 blob, sizeof(blob), &len);
    if (e == KILN_ERR_NOT_FOUND) return KILN_ERR_NOT_FOUND;   /* first boot */
    if (e != KILN_OK)            return KILN_ERR_CORRUPT;

    /* configmodel owns the decode, the migration and the range repair. */
    return kiln_config_decode(blob, len, cfg);
}

kiln_err_t kiln_settings_save(const kiln_port_kvstore_t *kv, const kiln_config_t *cfg)
{
    if (!kv || !kv->set || !cfg) return KILN_ERR_INVALID_ARG;

    uint8_t blob[sizeof(kiln_config_t) + 32];
    size_t  len = 0;
    kiln_err_t e = kiln_config_encode(cfg, blob, sizeof(blob), &len);
    if (e != KILN_OK) return e;

    e = kv->set(kv->ctx, KILN_NVS_NAMESPACE, KILN_NVS_KEY_CONFIG, blob, len);
    if (e != KILN_OK) return e;

    /* Committed explicitly: NVS buffers, and a configuration that is only in
     * RAM has not been saved however the call looked. */
    if (kv->commit) return kv->commit(kv->ctx, KILN_NVS_NAMESPACE);
    return KILN_OK;
}

/* --- latched fault ------------------------------------------------------ */

/* A fixed little-endian layout rather than the struct, so a compiler or a
 * padding change cannot silently orphan a fault that is already in flash. */
#define FAULT_BLOB_BYTES 48
#define FAULT_MAGIC      0x544C4621U   /* "!FLT" */

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put_u64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static void put_f32(uint8_t *p, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    put_u32(p, bits);
}
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
static float get_f32(const uint8_t *p)
{
    const uint32_t bits = get_u32(p);
    float v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

kiln_err_t kiln_settings_save_fault(const kiln_port_kvstore_t *kv,
                                    const kiln_latched_fault_t *f)
{
    if (!kv || !kv->set || !f) return KILN_ERR_INVALID_ARG;

    uint8_t b[FAULT_BLOB_BYTES];
    memset(b, 0, sizeof(b));

    put_u32(&b[0], FAULT_MAGIC);
    b[4] = f->fault;
    b[5] = f->state;
    b[6] = f->flags;
    b[7] = 0;
    put_u32(&b[8],  f->run_id);
    put_u32(&b[12], f->t_rel_ms);
    put_u64(&b[16], f->wall_utc_s);
    put_f32(&b[24], f->kiln_c);
    put_f32(&b[28], f->setpoint_c);
    put_f32(&b[32], f->case_c);
    put_f32(&b[36], f->current_a);
    put_u16(&b[40], f->duty_permille);
    put_u32(&b[42], f->warnings);
    put_u16(&b[46], kiln_crc16(b, FAULT_BLOB_BYTES - 2));

    kiln_err_t e = kv->set(kv->ctx, KILN_NVS_NAMESPACE, KILN_NVS_KEY_FAULT,
                           b, sizeof(b));
    if (e != KILN_OK) return e;

    /* SR-17 says before the alarm sounds, which means before this returns: an
     * uncommitted fault is one a power loss in the next second loses. */
    if (kv->commit) return kv->commit(kv->ctx, KILN_NVS_NAMESPACE);
    return KILN_OK;
}

kiln_err_t kiln_settings_load_fault(const kiln_port_kvstore_t *kv,
                                    kiln_latched_fault_t *out)
{
    if (!kv || !kv->get || !out) return KILN_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    uint8_t b[FAULT_BLOB_BYTES];
    size_t  len = 0;
    const kiln_err_t e = kv->get(kv->ctx, KILN_NVS_NAMESPACE, KILN_NVS_KEY_FAULT,
                                 b, sizeof(b), &len);
    if (e != KILN_OK) return e;
    if (len != FAULT_BLOB_BYTES) return KILN_ERR_CORRUPT;
    if (get_u32(&b[0]) != FAULT_MAGIC) return KILN_ERR_CORRUPT;
    if (kiln_crc16(b, FAULT_BLOB_BYTES - 2) != get_u16(&b[46])) return KILN_ERR_CORRUPT;

    out->fault         = b[4];
    out->state         = b[5];
    out->flags         = b[6];
    out->run_id        = get_u32(&b[8]);
    out->t_rel_ms      = get_u32(&b[12]);
    out->wall_utc_s    = get_u64(&b[16]);
    out->kiln_c        = get_f32(&b[24]);
    out->setpoint_c    = get_f32(&b[28]);
    out->case_c        = get_f32(&b[32]);
    out->current_a     = get_f32(&b[36]);
    out->duty_permille = get_u16(&b[40]);
    out->warnings      = get_u32(&b[42]);

    if (out->fault == KILN_FAULT_NONE) return KILN_ERR_NOT_FOUND;
    return KILN_OK;
}

kiln_err_t kiln_settings_clear_fault(const kiln_port_kvstore_t *kv)
{
    if (!kv || !kv->erase) return KILN_ERR_INVALID_ARG;

    const kiln_err_t e = kv->erase(kv->ctx, KILN_NVS_NAMESPACE, KILN_NVS_KEY_FAULT);
    if (e != KILN_OK && e != KILN_ERR_NOT_FOUND) return e;
    if (kv->commit) return kv->commit(kv->ctx, KILN_NVS_NAMESPACE);
    return KILN_OK;
}

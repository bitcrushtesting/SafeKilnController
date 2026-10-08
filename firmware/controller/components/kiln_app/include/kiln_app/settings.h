/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Configuration and latched-fault persistence -- SWR-CFG-05, SWR-SAF-17,
 * architecture 10 and SWA-10 (configuration in NVS, because it is small, typed,
 * and benefits from the wear levelling).
 *
 * The schema, validation and migration all live in kiln_core/configmodel; this
 * is only the storage side of it, which is why it is here and not there: the
 * core may not know what NVS is.
 */
#ifndef KILN_APP_SETTINGS_H
#define KILN_APP_SETTINGS_H

#include "kiln/err.h"
#include "kiln_core/configmodel.h"
#include "kiln_ports/port_kvstore.h"

#define KILN_NVS_NAMESPACE  "kiln"
#define KILN_NVS_KEY_CONFIG "config"
#define KILN_NVS_KEY_FAULT  "fault"

/* Load the stored configuration.
 *
 *   KILN_OK              current schema, used as-is
 *   KILN_ERR_NOT_FOUND   nothing stored: first boot, *cfg is defaults
 *   KILN_ERR_UNSUPPORTED migrated from an older schema or repaired; *cfg is
 *                        usable and the caller should write it back
 *   KILN_ERR_CORRUPT     unreadable: *cfg is defaults and SWR-CFG-05 wants a
 *                        warning raised (fault 20 if storage itself failed)
 *
 * *cfg is left usable in every case, because there is no state in which the
 * controller may run without a configuration. */
kiln_err_t kiln_settings_load(const kiln_port_kvstore_t *kv, kiln_config_t *cfg);

kiln_err_t kiln_settings_save(const kiln_port_kvstore_t *kv, const kiln_config_t *cfg);

/* --- the latched fault (SWR-SAF-17) ------------------------------------------ */

/* SWR-SAF-17 requires a latched fault to be written with its code, a snapshot and a
 * timestamp *before the alarm sounds*, so that an immediate power loss cannot
 * lose it.  The snapshot is what makes it diagnosable afterwards: a bare code
 * tells the operator something stopped the firing, not what the kiln was doing
 * at the time. */
typedef struct {
    uint8_t  fault;            /* kiln_fault_t */
    uint8_t  state;            /* kiln_state_t it tripped in */
    uint8_t  flags;
    uint32_t run_id;
    uint32_t t_rel_ms;         /* into the run */
    uint64_t wall_utc_s;       /* 0 when the clock was never synced */
    float    kiln_c;
    float    setpoint_c;
    float    case_c;
    float    current_a;
    uint16_t duty_permille;
    uint32_t warnings;
} kiln_latched_fault_t;

kiln_err_t kiln_settings_save_fault(const kiln_port_kvstore_t *kv,
                                    const kiln_latched_fault_t *f);
/* KILN_ERR_NOT_FOUND when nothing is latched, which is the normal case. */
kiln_err_t kiln_settings_load_fault(const kiln_port_kvstore_t *kv,
                                    kiln_latched_fault_t *out);
kiln_err_t kiln_settings_clear_fault(const kiln_port_kvstore_t *kv);

#endif

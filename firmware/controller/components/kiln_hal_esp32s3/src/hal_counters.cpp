/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Persisted switching-operation counters (SWR-CUR-13, feeding SWR-SAF-30).
 *
 * The port's shape is the design: `add` is cheap and `flush` is not, because
 * the write pattern is the problem rather than the storage.  A 2 s window at
 * 50 % duty switches 1800 times an hour, and committing each one would spend
 * the SWR-NFR-14 endurance budget on telemetry.  So this accumulates in RAM and
 * commits on a coarse boundary, and `add` never touches flash -- it is called
 * from the 10 ms window tick.
 *
 * NVS rather than the log partition: these are a handful of counters that want
 * wear levelling and a named key, which is exactly SWA-10's reasoning for
 * putting configuration there.
 */
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "kiln_hal/hal_esp32s3.h"

namespace {

const char *TAG = "hal_counters";

} // namespace

#define CTR_NAMESPACE "kiln_ctr"
#define CTR_KEY       "switch"

namespace {

typedef struct {
    kiln_switch_counters_t live;      /* loaded total plus this session's adds */
    kiln_switch_counters_t committed; /* what is actually in flash             */
    bool                   loaded;
} ctr_t;

ctr_t s_ctr;

bool differs(const kiln_switch_counters_t *a, const kiln_switch_counters_t *b)
{
    if (a->contactor_ops != b->contactor_ops) {
        return true;
    }
    for (uint8_t c = 0; c < KILN_HEAT_CHANNELS; c++) {
        if (a->ssr_ops[c] != b->ssr_ops[c]) {
            return true;
        }
    }
    return false;
}

kiln_err_t ctr_load(void *ctx, kiln_switch_counters_t *out)
{
    ctr_t *s = static_cast<ctr_t *>(ctx);
    if ((s == nullptr) || (out == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    if (nvs_open(CTR_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        /* No namespace yet is a first boot, not a failure: a new contactor has
         * done no operations.  Reporting an error here would make SWR-SAF-30
         * unavailable on every device until something had written once. */
        memset(&s->live, 0, sizeof(s->live));
        s->committed = s->live;
        s->loaded    = true;
        *out         = s->live;
        return KILN_OK;
    }

    size_t     len = sizeof(s->live);
    const bool ok  = (nvs_get_blob(h, CTR_KEY, &s->live, &len) == ESP_OK) &&
                     (len == sizeof(s->live));
    nvs_close(h);

    if (!ok) {
        /* A blob of the wrong size is a layout change, and guessing which
         * fields moved would corrupt a wear total that decides when somebody
         * replaces a contactor.  Start from zero and say so. */
        ESP_LOGW(TAG, "stored counters unreadable or wrong size, starting from zero");
        memset(&s->live, 0, sizeof(s->live));
    }
    s->committed = s->live;
    s->loaded    = true;
    *out         = s->live;
    return KILN_OK;
}

void ctr_add_contactor(void *ctx, uint32_t n)
{
    ctr_t *s = static_cast<ctr_t *>(ctx);
    if (s != nullptr) {
        s->live.contactor_ops += n;        /* RAM only: see the header comment */
    }
}

void ctr_add_ssr(void *ctx, uint8_t channel, uint32_t n)
{
    ctr_t *s = static_cast<ctr_t *>(ctx);
    if ((s != nullptr) && (channel < KILN_HEAT_CHANNELS)) {
        s->live.ssr_ops[channel] += n;
    }
}

kiln_err_t ctr_flush(void *ctx)
{
    ctr_t *s = static_cast<ctr_t *>(ctx);
    if (s == nullptr) {
        return KILN_ERR_INVALID_ARG;
    }
    if (!s->loaded || !differs(&s->live, &s->committed)) {
        return KILN_OK;               /* nothing to say, so no erase cycle */
    }

    nvs_handle_t h;
    if (nvs_open(CTR_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return KILN_ERR_IO;
    }
    kiln_err_t e = KILN_OK;
    if (nvs_set_blob(h, CTR_KEY, &s->live, sizeof(s->live)) != ESP_OK ||
        nvs_commit(h) != ESP_OK) {
        e = KILN_ERR_IO;
    }
    nvs_close(h);

    if (e == KILN_OK) {
        s->committed = s->live;
    }
    return e;
}

kiln_err_t ctr_reset(void *ctx, const kiln_switch_counters_t *to)
{
    ctr_t *s = static_cast<ctr_t *>(ctx);
    if ((s == nullptr) || (to == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    /* For a replaced contactor or SSR: SWR-SAF-30's life limit is about one physical
     * part, so fitting a new one has to be able to zero its count. */
    s->live   = *to;
    s->loaded = true;
    return ctr_flush(ctx);
}

} // namespace

kiln_err_t kiln_hal_counters_init(kiln_port_counters_t *out)
{
    if (out == nullptr) {
        return KILN_ERR_INVALID_ARG;
    }
    memset(&s_ctr, 0, sizeof(s_ctr));

    out->ctx               = &s_ctr;
    out->load              = ctr_load;
    out->add_contactor_ops = ctr_add_contactor;
    out->add_ssr_ops       = ctr_add_ssr;
    out->flush             = ctr_flush;
    out->reset             = ctr_reset;
    return KILN_OK;
}

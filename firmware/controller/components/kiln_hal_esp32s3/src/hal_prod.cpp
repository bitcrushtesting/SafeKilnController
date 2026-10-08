/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The production data block (SWR-PROD-01, SWR-PROD-02): manufacturer, model,
 * board revision, serial number and production date, written once at
 * manufacture by tools/prod-data.py and read here.
 *
 * Three properties this file exists to hold:
 *
 * 1. It is a *separate NVS partition*, `prod`, not a namespace in `nvs`.
 *    hal_kvstore erases `nvs` wholesale when NVS reports a format change, which
 *    is correct for configuration and unacceptable for a serial number.
 *
 * 2. It is opened NVS_READONLY, always.  Nothing in the firmware may write it:
 *    the only writer is the production tool, over a programmer, at manufacture.
 *    A device that could rewrite its own serial number has no identity.
 *
 * 3. It is read once, at init, into a cached struct.  The block cannot change
 *    while the firmware runs, and a getter that cannot touch flash cannot delay
 *    the safety cycle (SWR-NFR-02) no matter who calls it or how often.
 */

#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "kiln_hal/hal_esp32s3.h"

namespace {

const char *TAG = "hal_prod";

/* The partition label in partitions.csv and the namespace inside it.  The
 * production tool writes this same pair; they are stated once each, here and in
 * that tool, and the tool's --self-test checks its own half. */
const char *PROD_PARTITION = "prod";
const char *PROD_NAMESPACE = "prod";

kiln_prod_info_t s_prod;      /* zeroed: `programmed` false until proven true */
bool             s_ready;

/* nvs_get_str with the destination's own capacity as the bound, reporting
 * whether the key was there and fitted.  A truncated serial number is a wrong
 * serial number, so a value too long to fit is a failure, not a trim. */
bool get_str(nvs_handle_t h, const char *key, char *out, size_t cap)
{
    size_t len = cap;
    esp_err_t e = nvs_get_str(h, key, out, &len);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "%s: %s", key, esp_err_to_name(e));
        out[0] = '\0';
        return false;
    }
    return out[0] != '\0';
}

}  // namespace

kiln_err_t kiln_hal_prod_init(void)
{
    memset(&s_prod, 0, sizeof(s_prod));
    s_ready = true;              /* the cache is valid even when it is empty */

    /* A board that has not been through the production step has no `prod`
     * partition contents, and must still boot: this is information, not a
     * fault.  SWR-PROD-04 is explicit that an unprogrammed block is reported as
     * unprogrammed rather than guessed at or treated as an error. */
    esp_err_t e = nvs_flash_init_partition(PROD_PARTITION);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "no `%s` partition (%s); unit is unidentified",
                 PROD_PARTITION, esp_err_to_name(e));
        return KILN_OK;
    }

    nvs_handle_t h;
    e = nvs_open_from_partition(PROD_PARTITION, PROD_NAMESPACE, NVS_READONLY, &h);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "`%s` present but not programmed (%s)",
                 PROD_PARTITION, esp_err_to_name(e));
        return KILN_OK;
    }

    const bool ok =
        get_str(h, "manufacturer", s_prod.manufacturer, sizeof(s_prod.manufacturer)) &&
        get_str(h, "model",        s_prod.model,        sizeof(s_prod.model))        &&
        get_str(h, "revision",     s_prod.revision,     sizeof(s_prod.revision))    &&
        get_str(h, "serial",       s_prod.serial,       sizeof(s_prod.serial))       &&
        get_str(h, "prod_date",    s_prod.production_date, sizeof(s_prod.production_date));
    nvs_close(h);

    /* All five or none.  A block missing its serial number is not a unit with a
     * partial identity, it is a unit whose identity cannot be trusted, and
     * SWR-PROD-04 would rather say so than report half of it. */
    s_prod.programmed = ok;
    if (ok) {
        ESP_LOGI(TAG, "%s %s %s serial %s, made %s",
                 s_prod.manufacturer, s_prod.model, s_prod.revision,
                 s_prod.serial, s_prod.production_date);
    } else {
        ESP_LOGW(TAG, "production data incomplete; unit is unidentified");
        memset(&s_prod, 0, sizeof(s_prod));
    }
    return KILN_OK;
}

kiln_err_t kiln_hal_prod_get(kiln_prod_info_t *out)
{
    if (out == nullptr) {
        return KILN_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        memset(out, 0, sizeof(*out));
        return KILN_ERR_NOT_FOUND;
    }
    *out = s_prod;
    return KILN_OK;
}

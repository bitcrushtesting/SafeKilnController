/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * port_kvstore over NVS (AD-10).  Blobs only: the configuration is typed by
 * kiln_core/configmodel, and splitting it into NVS-typed entries would put the
 * schema in two places.
 */

#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "kiln_hal/hal_esp32s3.h"

static const char *TAG = "hal_nvs";

static kiln_err_t map_err(esp_err_t e)
{
    switch (e) {
    case ESP_OK:                      return KILN_OK;
    case ESP_ERR_NVS_NOT_FOUND:       return KILN_ERR_NOT_FOUND;
    case ESP_ERR_NVS_INVALID_LENGTH:  return KILN_ERR_NO_SPACE;
    case ESP_ERR_NVS_NOT_ENOUGH_SPACE:return KILN_ERR_NO_SPACE;
    case ESP_ERR_INVALID_ARG:         return KILN_ERR_INVALID_ARG;
    default:                          return KILN_ERR_IO;
    }
}

static kiln_err_t kv_get(void *ctx, const char *ns, const char *key,
                         void *out, size_t cap, size_t *out_len)
{
    (void)ctx;
    if (!ns || !key || !out) return KILN_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t e = nvs_open(ns, NVS_READONLY, &h);
    if (e != ESP_OK) return map_err(e);

    size_t len = cap;
    e = nvs_get_blob(h, key, out, &len);
    nvs_close(h);

    if (e != ESP_OK) return map_err(e);
    if (out_len) *out_len = len;
    return KILN_OK;
}

static kiln_err_t kv_set(void *ctx, const char *ns, const char *key,
                         const void *data, size_t len)
{
    (void)ctx;
    if (!ns || !key || !data) return KILN_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t e = nvs_open(ns, NVS_READWRITE, &h);
    if (e != ESP_OK) return map_err(e);

    e = nvs_set_blob(h, key, data, len);
    nvs_close(h);
    return map_err(e);
}

static kiln_err_t kv_erase(void *ctx, const char *ns, const char *key)
{
    (void)ctx;
    if (!ns || !key) return KILN_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t e = nvs_open(ns, NVS_READWRITE, &h);
    if (e != ESP_OK) return map_err(e);

    e = nvs_erase_key(h, key);
    nvs_close(h);
    return map_err(e);
}

static kiln_err_t kv_commit(void *ctx, const char *ns)
{
    (void)ctx;
    if (!ns) return KILN_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t e = nvs_open(ns, NVS_READWRITE, &h);
    if (e != ESP_OK) return map_err(e);

    e = nvs_commit(h);
    nvs_close(h);
    return map_err(e);
}

kiln_err_t kiln_hal_kvstore_init(kiln_port_kvstore_t *out)
{
    if (!out) return KILN_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* A first boot on a used device, or an NVS format change.  Erasing loses
         * the configuration, which FR-CFG-05 already covers: defaults plus a
         * warning beats refusing to boot. */
        ESP_LOGW(TAG, "NVS needs erasing (%s); configuration will return to defaults",
                 esp_err_to_name(e));
        e = nvs_flash_erase();
        if (e == ESP_OK) e = nvs_flash_init();
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(e));
        return map_err(e);
    }

    out->get    = kv_get;
    out->set    = kv_set;
    out->erase  = kv_erase;
    out->commit = kv_commit;
    return KILN_OK;
}

/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * port_flash over esp_partition.  Deliberately logic-free: everything
 * interesting about the ring is in kiln_core/logring, where a host test can cut
 * power mid-write.
 */

#include <string.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "spi_flash_mmap.h"   /* SPI_FLASH_SEC_SIZE */
#include "kiln_hal/hal_esp32s3.h"

static const char *TAG = "hal_flash";

/* One partition, resolved once at init.  A pointer into the partition table is
 * stable for the life of the application. */
static const esp_partition_t *s_part;

static kiln_err_t map_err(esp_err_t e)
{
    switch (e) {
    case ESP_OK:                return KILN_OK;
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_INVALID_SIZE:  return KILN_ERR_INVALID_ARG;
    case ESP_ERR_NOT_FOUND:     return KILN_ERR_NOT_FOUND;
    default:                    return KILN_ERR_IO;
    }
}

static kiln_err_t hal_info(void *ctx, kiln_flash_info_t *out)
{
    const esp_partition_t *p = (const esp_partition_t *)ctx;
    if ((p == nullptr) || (out == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    out->size_bytes   = p->size;
    /* SPI NOR erase granularity.  Hard-coded in the sense that the whole
     * partition layout of architecture 10.1 assumes it; if it ever differs the
     * ring's arithmetic needs to know, so it comes from the SDK constant rather
     * than from a literal here. */
    out->sector_bytes = SPI_FLASH_SEC_SIZE;
    return KILN_OK;
}

static kiln_err_t hal_read(void *ctx, uint32_t offset, void *out, size_t len)
{
    const esp_partition_t *p = (const esp_partition_t *)ctx;
    if ((p == nullptr) || (out == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    return map_err(esp_partition_read(p, offset, out, len));
}

static kiln_err_t hal_write(void *ctx, uint32_t offset, const void *data, size_t len)
{
    const esp_partition_t *p = (const esp_partition_t *)ctx;
    if ((p == nullptr) || (data == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    return map_err(esp_partition_write(p, offset, data, len));
}

static kiln_err_t hal_erase(void *ctx, uint32_t offset, size_t len)
{
    const esp_partition_t *p = (const esp_partition_t *)ctx;
    if (p == nullptr) {
        return KILN_ERR_INVALID_ARG;
    }
    return map_err(esp_partition_erase_range(p, offset, len));
}

kiln_err_t kiln_hal_flash_init(const char *partition_label, kiln_port_flash_t *out)
{
    if (out == nullptr) {
        return KILN_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    /* The log partition is a custom data type (0x40) per architecture 10.1, not
     * a filesystem, so it is found by label rather than by subtype. */
    /* 0x40 is deliberately not a named esp_partition_type_t: ESP-IDF reserves
     * 0x40..0xFE for application-defined types, which is exactly what this is. */
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    s_part = esp_partition_find_first((esp_partition_type_t)0x40,
                                      ESP_PARTITION_SUBTYPE_ANY,
                                      partition_label);
    if (s_part == nullptr) {
        ESP_LOGE(TAG, "no partition '%s' in the table", partition_label);
        return KILN_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "log partition '%s': %u bytes at 0x%06x, %u byte sectors",
             partition_label, (unsigned)s_part->size, (unsigned)s_part->address,
             (unsigned)SPI_FLASH_SEC_SIZE);

    out->ctx   = (void *)s_part;
    out->info  = hal_info;
    out->read  = hal_read;
    out->write = hal_write;
    out->erase = hal_erase;
    return KILN_OK;
}

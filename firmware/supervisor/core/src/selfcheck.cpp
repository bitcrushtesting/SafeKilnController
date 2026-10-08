/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "sup/selfcheck.h"

bool sup_clock_ok(uint32_t rcc_cr, uint32_t rcc_cfgr)
{
    const uint32_t sw  = rcc_cfgr & SUP_RCC_CFGR_SW_Msk;
    const uint32_t sws = (rcc_cfgr & SUP_RCC_CFGR_SWS_Msk) >> SUP_RCC_CFGR_SWS_Pos;

    if (sws != sw) {
        return false;       /* the switch did not take, or something moved it */
    }
    if ((rcc_cr & SUP_RCC_CR_HSIRDY) == 0u) {
        return false;       /* the oscillator we time from is not running */
    }
    if ((rcc_cr & SUP_RCC_CR_PLLON) != 0u) {
        return false;       /* the system clock is not the 16 MHz assumed */
    }
    return true;
}

uint32_t sup_crc32(const void *data, size_t len)
{
    /* Bitwise rather than table-driven: 256 words of table would be a quarter
     * of this firmware's RAM budget or a sixteenth of its flash, to speed up a
     * check that runs once per boot. The loop is 4 kB * 8 iterations, which at
     * 16 MHz is under ten milliseconds and happens before the first permit. */
    const uint8_t *p = static_cast<const uint8_t *>(data);
    uint32_t crc = 0xFFFFFFFFu;
    if (p == nullptr) {
        return crc;
    }
    for (size_t i = 0; i < len; i++) {
        crc ^= static_cast<uint32_t>(p[i]);
        for (unsigned b = 0; b < 8u; b++) {
            /* The reflected CRC-32 polynomial, 0xEDB88320. */
            const uint32_t mask = (crc & 1u) != 0u ? 0xEDB88320u : 0u;
            crc = (crc >> 1u) ^ mask;
        }
    }
    return ~crc;
}

bool sup_flash_ok(const void *data, size_t len, uint32_t expected)
{
    if (expected == SUP_CRC_UNPROGRAMMED) {
        return false;       /* never stamped; integrity is unknown, not fine */
    }
    if ((data == nullptr) || (len == 0u)) {
        return false;
    }
    return sup_crc32(data, len) == expected;
}

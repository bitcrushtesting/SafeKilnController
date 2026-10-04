/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Raw access to a flash partition -- the substrate AD-08's circular log ring is
 * built on.
 *
 * Why this exists rather than the ring living in the adapter, as architecture
 * 5.2 originally had it: the ring's interesting behaviour is all *decision*
 * logic with failure modes that FR-LOG-06 and FR-LOG-08 name explicitly --
 * head discovery across 512 sector headers, erase-immediately-before-write
 * ordering so that a power cut cannot destroy data the index still claims
 * exists, and terminating a sector scan at a torn record.  None of that is
 * testable behind esp_partition.  Split here, the ring is `kiln_core/logring`
 * and is driven on the host through a fake that can cut power mid-write, while
 * the adapter reduces to three calls that pass straight through and have no
 * logic left to get wrong.
 *
 * The contract is NOR flash's, not a convenience:
 *   - erase sets every bit in a sector to 1 (0xFF)
 *   - a write may only clear bits, so a region must be erased before it can be
 *     written with anything other than what it already holds
 *   - a write interrupted by power loss leaves its target bytes undefined,
 *     which is precisely what the per-record CRC of FR-LOG-08 is for
 */
#ifndef KILN_PORT_FLASH_H
#define KILN_PORT_FLASH_H

#include <stddef.h>
#include "kiln/err.h"
#include "kiln/types.h"

typedef struct {
    uint32_t size_bytes;
    uint32_t sector_bytes;    /* erase granularity */
} kiln_flash_info_t;

typedef struct kiln_port_flash {
    void *ctx;

    kiln_err_t (*info)(void *ctx, kiln_flash_info_t *out);

    /* Offsets are relative to the start of the partition.  Reads are
     * unrestricted; a write must target erased space; erase takes a
     * sector-aligned region. */
    kiln_err_t (*read)(void *ctx, uint32_t offset, void *out, size_t len);
    kiln_err_t (*write)(void *ctx, uint32_t offset, const void *data, size_t len);
    kiln_err_t (*erase)(void *ctx, uint32_t offset, size_t len);
} kiln_port_flash_t;

#endif /* KILN_PORT_FLASH_H */

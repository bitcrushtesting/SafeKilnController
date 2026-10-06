/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Named blobs in a raw flash partition -- AD-10, FR-PRG-04, FR-LOG-09.
 *
 * This is what backs kiln_port_filestore.  It is not a filesystem, for the same
 * reason AD-08 gave when it refused one for the log: a filesystem adds metadata
 * writes, fragmentation and a torn-write failure mode across structures we do
 * not control.  The two callers above it -- program_store and run_index -- do
 * not need one either.  Both address a fixed array of numbered slots, `/p/00`
 * to `/p/19` and `/r/00` upwards, never an arbitrary name, and between them
 * they call read, write_atomic, remove and usage.  `list` and `exists` are in
 * the port but have no caller.  A filesystem would be a general mechanism paid
 * for in failure modes and bought for a fixed-size array.
 *
 * The medium is divided into equal regions of two erase sectors.  One file
 * lives in one region and alternates between its two sectors, so the copy being
 * replaced is never the copy being erased.  That is where the atomicity in
 * `write_atomic` comes from: there is no rename, and nothing to rename.
 *
 * Within a sector:
 *
 *   0   u32 magic      written last
 *   4   u32 seq        written last, higher wins
 *   8   u16 len        written last
 *   10  u16 crc        written last, over name and payload
 *   12  char name[64]  written first
 *   76  payload        written first
 *
 * The split is the whole design.  The name and the payload go down in one
 * write; the twelve byte commit header goes down in a second.  A power cut
 * before the second write leaves the magic erased, so the half-written copy is
 * not a copy at all and the previous one still carries the highest seq.  A
 * power cut during the second write is caught by the CRC.  There is no instant
 * at which a reader sees a torn file, which is the property FR-RUN-08 needs and
 * the one the host tests cut power to prove.
 *
 * Pure logic over kiln_port_flash: no clock, no allocation, all state in the
 * caller's struct (AD-02, AD-03).
 */
#ifndef KILN_CORE_FILESLOTS_H
#define KILN_CORE_FILESLOTS_H

#include "kiln/err.h"
#include "kiln/types.h"
#include "kiln_ports/port_filestore.h"
#include "kiln_ports/port_flash.h"

/* 20 programs plus 20 run records is 40; the rest is headroom so a new kind of
 * file does not force a partition change (architecture 10.1). */
constexpr size_t KILN_FILESLOT_REGIONS_MAX = 64u;

/* Byte offsets above.  Payload capacity is sector_bytes - KILN_FILESLOT_HDR. */
constexpr uint32_t KILN_FILESLOT_HDR = 76u;

typedef struct {
    char     name[KILN_PATH_MAX];
    uint32_t seq;      /* of the live copy */
    uint16_t len;
    uint16_t crc;      /* so a read can re-check without the header */
    uint8_t  copy;     /* 0 or 1: which sector of the region is live */
    bool     used;
} kiln_fileslot_entry_t;

typedef struct {
    const kiln_port_flash_t *flash;
    uint32_t sector_bytes;
    uint32_t region_bytes;      /* two sectors */
    uint16_t region_count;
    uint16_t payload_max;
    bool     mounted;
    kiln_fileslot_entry_t entry[KILN_FILESLOT_REGIONS_MAX];
} kiln_fileslots_t;

/* Scan every region and build the index.  A region whose two copies are both
 * invalid is free; where both are valid the higher seq wins and the loser is
 * left alone, because it is the next write's target and erasing it here would
 * spend an erase cycle on every boot. */
kiln_err_t kiln_fileslots_mount(kiln_fileslots_t *fs, const kiln_port_flash_t *flash);

/* Erase every region.  FR-CFG-12's factory reset. */
kiln_err_t kiln_fileslots_format(kiln_fileslots_t *fs);

/* Present the mounted store as the port the application holds. */
void kiln_fileslots_bind(kiln_fileslots_t *fs, kiln_port_filestore_t *out);

/* How many regions hold a file, for the log line at boot and the tests. */
uint16_t kiln_fileslots_used_regions(const kiln_fileslots_t *fs);

#endif /* KILN_CORE_FILESLOTS_H */

/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Named blobs in a raw flash partition -- SWA-10, SWR-PRG-04, SWR-LOG-09.
 *
 * This is what backs kiln_port_filestore.  It is not a filesystem, for the same
 * reason SWA-08 gave when it refused one for the log: a filesystem adds metadata
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
 * at which a reader sees a torn file, which is the property SWR-RUN-08 needs and
 * the one the host tests cut power to prove.
 *
 * ---------------------------------------------------------------------------
 * WEAR, AND WHAT THE STORE DOES ABOUT IT
 * ---------------------------------------------------------------------------
 * A NOR sector at the end of its life does not report an error.  It takes the
 * write, returns success, and holds something other than what it was given.
 * An implementation that trusts the return code therefore discovers the loss
 * at the next read, which on this device is after a firing rather than during
 * the write it could still have redirected.
 *
 * So every write is proved before it is believed: the copy is read back and
 * re-validated through the same path a mount uses, and only then does the
 * in-RAM index point at it.  A copy that does not verify retires its region --
 * the region is excluded from further allocation for this power cycle -- and
 * the write is re-attempted in a free region at a higher seq.  The caller gets
 * KILN_OK, because the file is stored; what changed is where.
 *
 * Retirement needs no bad-block table, because the medium carries the evidence
 * on its own.  Once the migrated copy has verified, the retired region is
 * erased, best effort:
 *
 *   - if the erase succeeds the stale copy is gone, which is what keeps a later
 *     remove from being undone at the next mount by an old copy of the file
 *     that nobody asked to keep.  The region returns to the pool at the next
 *     boot and will be retired again the next time it fails, which for a sector
 *     that can still be erased is the right answer rather than a life sentence
 *     passed on one bad write;
 *   - if the erase fails, which is what a sector that is actually gone does,
 *     the stale copy stays at a LOWER seq than the migrated one.  Two regions
 *     then claim one name, and mount resolves it the only way it can be true:
 *     the lower seq is the write that failed, so its region is retired again
 *     and the higher seq is the file.  Retirement survives the power cycle
 *     exactly in the case that deserves it.
 *
 * A power cut between the verify and the erase leaves both copies on the
 * medium, which is the same state and is resolved the same way.
 *
 * What this does NOT do is spread wear.  A region is chosen by index and keeps
 * the file for as long as it works; at 40 regions of two sectors, written
 * single-digit times a year against a 100 000 cycle rating, the endurance
 * arithmetic of architecture 10.4 has four orders of magnitude of headroom and
 * levelling would be mechanism bought for nothing.  Retirement is in because
 * its failure mode is losing a run record, which is cheap to prevent and
 * annoying to explain.
 *
 * Pure logic over kiln_port_flash: no clock, no allocation, all state in the
 * caller's struct (SWA-02, SWA-03).
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

/* How many regions one write may be attempted in before it gives up and
 * returns the medium's error.  Three rather than "all of them": each attempt
 * spends an erase cycle and a write on a part that is already misbehaving, and
 * a store that cannot place a file in three separate regions has a problem that
 * a fourth attempt does not fix.  One is too few, because the first retirement
 * is exactly the case this exists for. */
constexpr unsigned KILN_FILESLOT_WRITE_TRIES = 3u;

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
    /* Retired: a region whose write did not verify, or which mount found
     * holding the losing copy of a name.  Never allocated again until a
     * format.  One byte a region rather than a bitmap, because 64 bytes of RAM
     * is not worth the shift-and-mask at every allocation. */
    bool     retired[KILN_FILESLOT_REGIONS_MAX];
} kiln_fileslots_t;

/* Scan every region and build the index.  A region whose two copies are both
 * invalid is free; where both are valid the higher seq wins and the loser is
 * left alone, because it is the next write's target and erasing it here would
 * spend an erase cycle on every boot.
 *
 * Two REGIONS holding the same name is the trace a retirement leaves, and is
 * resolved here: higher seq is the file, and the other region is retired.  See
 * the wear note above.
 *
 * Mount reads and CRCs every copy, which is 768 reads and 47 kB on the medium
 * this firmware produces and 8 192 reads and 512 kB on a full one of
 * maximum-size files; both are measured in test_fileslots.cpp.  That is the
 * cost of knowing at start-up what is on the medium, and it is paid once. */
kiln_err_t kiln_fileslots_mount(kiln_fileslots_t *fs, const kiln_port_flash_t *flash);

/* Erase every region and un-retire all of them.  FR-CFG-12's factory reset: a
 * part that failed a verify may have been failing for a reason that a full
 * erase clears, and refusing to try again after an explicit factory reset would
 * leave the operator with no way to find out. */
kiln_err_t kiln_fileslots_format(kiln_fileslots_t *fs);

/* Present the mounted store as the port the application holds. */
void kiln_fileslots_bind(kiln_fileslots_t *fs, kiln_port_filestore_t *out);

/* How many regions hold a file, for the log line at boot and the tests. */
uint16_t kiln_fileslots_used_regions(const kiln_fileslots_t *fs);

/* How many regions have been retired.  Logged at boot: a store that is quietly
 * losing regions is a part on its way out, and the number is the only warning
 * anybody gets. */
uint16_t kiln_fileslots_retired_regions(const kiln_fileslots_t *fs);

#endif /* KILN_CORE_FILESLOTS_H */

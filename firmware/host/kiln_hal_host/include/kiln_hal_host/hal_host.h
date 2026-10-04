/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host adapters for the storage and clock ports -- the fakes architecture 5.2
 * lists against each port, gathered into one component so the test suites and
 * any host-side harness share them instead of each growing their own.
 *
 * Host-only: the CMake has no ESP_PLATFORM path, so none of this can reach a
 * target image (TR-10).
 *
 * The flash fake is the interesting one.  It enforces NOR semantics rather than
 * behaving like memory -- a write may only clear bits, and writing over
 * un-erased space is an error, not a silent overwrite -- because a ring that
 * only works on forgiving storage is a ring that works until it is flashed.  It
 * can also cut power part-way through a write, which is the one thing a real
 * device does that a test otherwise cannot ask for, and the whole subject of
 * FR-LOG-08.
 */
#ifndef KILN_HAL_HOST_H
#define KILN_HAL_HOST_H

#include <stddef.h>
#include "kiln/err.h"
#include "kiln/types.h"
#include "kiln_ports/port_clock.h"
#include "kiln_ports/port_filestore.h"
#include "kiln_ports/port_flash.h"
#include "kiln_ports/port_kvstore.h"

/* --- flash ------------------------------------------------------------- */

#define KILN_HOST_FLASH_MAX_BYTES (2u * 1024u * 1024u)

typedef struct {
    uint8_t  *data;              /* caller-provided, so the size is the test's choice */
    uint32_t  size_bytes;
    uint32_t  sector_bytes;

    /* Counters, for the endurance analysis of architecture 10.4. */
    uint32_t  erases;
    uint32_t  writes;
    uint32_t  reads;
    uint32_t  bytes_written;

    /* Fault injection. */
    uint32_t  fail_write_after;  /* 0 = never: writes fail once this many have succeeded */
    uint32_t  fail_erase_after;
    bool       powered;          /* false once power has been cut           */

    /* Cut power during the Nth write (1-based), after copying `cut_bytes` of it.
     * That is exactly a torn record: some bytes landed, the rest did not. */
    uint32_t  cut_power_at_write;
    uint32_t  cut_bytes;
} kiln_host_flash_t;

/* `storage` must be at least size_bytes and is set to 0xFF (erased). */
void kiln_host_flash_init(kiln_host_flash_t *f, uint8_t *storage,
                          uint32_t size_bytes, uint32_t sector_bytes);
void kiln_host_flash_bind(kiln_host_flash_t *f, kiln_port_flash_t *out);

/* Power back on after a cut, leaving the medium exactly as it was -- which is
 * what a reboot looks like to the ring. */
void kiln_host_flash_power_on(kiln_host_flash_t *f);

/* --- key/value store --------------------------------------------------- */

#define KILN_HOST_KV_ENTRIES   16
#define KILN_HOST_KV_NS_LEN    16
#define KILN_HOST_KV_KEY_LEN   24
#define KILN_HOST_KV_VALUE_MAX 1024

typedef struct {
    char     ns[KILN_HOST_KV_NS_LEN];
    char     key[KILN_HOST_KV_KEY_LEN];
    uint8_t  value[KILN_HOST_KV_VALUE_MAX];
    size_t   len;
    bool     used;
} kiln_host_kv_entry_t;

typedef struct {
    kiln_host_kv_entry_t entries[KILN_HOST_KV_ENTRIES];
    uint32_t             sets;
    uint32_t             commits;
    bool                 fail_writes;    /* FR-CFG-05 / fault 20 */
} kiln_host_kv_t;

void kiln_host_kv_init(kiln_host_kv_t *kv);
void kiln_host_kv_bind(kiln_host_kv_t *kv, kiln_port_kvstore_t *out);

/* --- file store -------------------------------------------------------- */

#define KILN_HOST_FS_FILES     32
#define KILN_HOST_FS_FILE_MAX  2048

typedef struct {
    char    path[KILN_PATH_MAX];
    uint8_t data[KILN_HOST_FS_FILE_MAX];
    size_t  len;
    bool    used;
} kiln_host_file_t;

typedef struct {
    kiln_host_file_t files[KILN_HOST_FS_FILES];
    uint32_t         writes;
    bool             fail_writes;
    /* Replace is atomic-by-rename on the target; cutting power here leaves the
     * *old* content intact, which is the property that matters and the one a
     * non-atomic implementation loses. */
    uint32_t         cut_power_at_write;
    bool             powered;
} kiln_host_fs_t;

void kiln_host_fs_init(kiln_host_fs_t *fs);
void kiln_host_fs_bind(kiln_host_fs_t *fs, kiln_port_filestore_t *out);

/* --- clock ------------------------------------------------------------- */

/* AD-02: the virtual clock the tests drive.  A 168 h soak costs no wall time. */
typedef struct {
    uint64_t mono_us;
    uint64_t wall_utc_s;
    bool     wall_valid;        /* FR-LOG-12: false until SNTP has succeeded */
} kiln_host_clock_t;

void kiln_host_clock_init(kiln_host_clock_t *c, uint64_t wall_utc_s, bool wall_valid);
void kiln_host_clock_bind(kiln_host_clock_t *c, kiln_port_clock_t *out);
void kiln_host_clock_advance(kiln_host_clock_t *c, uint64_t us);

#endif

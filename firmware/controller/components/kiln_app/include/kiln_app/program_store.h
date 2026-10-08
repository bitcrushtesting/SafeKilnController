/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Stored firing programs -- SWR-PRG-04, SWR-PRG-07, SWR-PRG-08, SWR-PRG-09,
 * SWA-10 (programs in LittleFS: they are user files that want names, import and
 * export, and atomic replace).
 *
 * Addressed by slot rather than by a filename derived from the name.  Names
 * arrive from the network (SWR-NFR-19), and turning untrusted UTF-8 into a path is a
 * directory-traversal bug waiting to be written; a fixed set of slots has no
 * such surface, and the name lives inside the file where it is data rather than
 * structure.  Twenty slots is SWR-PRG-04's limit, so a lookup by name is a scan
 * of twenty short reads -- nothing worth an index that could disagree.
 */
#ifndef KILN_APP_PROGRAM_STORE_H
#define KILN_APP_PROGRAM_STORE_H

#include "kiln/err.h"
#include "kiln/types.h"
#include "kiln_ports/port_filestore.h"

#define KILN_PROGRAM_SLOTS KILN_MAX_PROGRAMS   /* SWR-PRG-04 */

/* Seed the built-in read-only examples if they are not present (SWR-PRG-09).
 * Idempotent, so it can run on every boot. */
kiln_err_t kiln_program_store_seed(const kiln_port_filestore_t *fs);

/* Save under its own name, replacing an existing program of that name.
 *
 * Validates first (SWR-PRG-05), refuses to overwrite a read-only example
 * (SWR-PRG-09), and returns KILN_ERR_NO_SPACE when all slots are taken. */
kiln_err_t kiln_program_store_save(const kiln_port_filestore_t *fs,
                                   const kiln_program_t *p, float max_temp_c);

kiln_err_t kiln_program_store_load(const kiln_port_filestore_t *fs,
                                   const char *name, kiln_program_t *out);

/* Refuses a read-only example (SWR-PRG-09). */
kiln_err_t kiln_program_store_delete(const kiln_port_filestore_t *fs, const char *name);

/* Read a slot directly, for enumeration.  KILN_ERR_NOT_FOUND for an empty slot. */
kiln_err_t kiln_program_store_get_slot(const kiln_port_filestore_t *fs,
                                       uint8_t slot, kiln_program_t *out);

uint8_t kiln_program_store_count(const kiln_port_filestore_t *fs);

#endif

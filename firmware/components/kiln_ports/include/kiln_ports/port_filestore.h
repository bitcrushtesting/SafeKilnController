/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Named files (LittleFS on target): programs and run records.  Writes must be
 * atomic-by-rename so a power cut cannot leave a half-written program.
 */
#ifndef KILN_PORT_FILESTORE_H
#define KILN_PORT_FILESTORE_H

#include <stddef.h>
#include "kiln/err.h"
#include "kiln/types.h"

#define KILN_PATH_MAX 64

typedef struct kiln_port_filestore {
    void *ctx;
    kiln_err_t (*read)(void *ctx, const char *path, void *out, size_t cap, size_t *len);
    kiln_err_t (*write_atomic)(void *ctx, const char *path, const void *data, size_t len);
    kiln_err_t (*remove)(void *ctx, const char *path);
    kiln_err_t (*exists)(void *ctx, const char *path);
    /* Enumerate; returning false from fn stops. */
    kiln_err_t (*list)(void *ctx, const char *dir,
                       bool (*fn)(void *user, const char *name, size_t size),
                       void *user);
    kiln_err_t (*usage)(void *ctx, size_t *total, size_t *used);
} kiln_port_filestore_t;

#endif

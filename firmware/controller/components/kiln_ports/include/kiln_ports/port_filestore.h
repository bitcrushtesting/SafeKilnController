/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Named blobs: programs and run records.  A write must be atomic, so that a
 * power cut cannot leave a half-written program.
 *
 * Provided by kiln_core/fileslots over a raw flash partition (SWA-21), which
 * gets the atomicity from alternating between two copies rather than from a
 * rename.  There is no filesystem behind this on the target, and nothing above
 * it needs one: both callers address a fixed set of numbered slots.
 */
#ifndef KILN_PORT_FILESTORE_H
#define KILN_PORT_FILESTORE_H

#include <stddef.h>
#include "kiln/err.h"
#include "kiln/types.h"

/* Including the terminator, and the stored name field is exactly this, so a
 * longer path is REFUSED with KILN_ERR_INVALID_ARG rather than truncated.
 * Storing a file under a name the caller did not ask for is the worse of the
 * two failures; both callers produce five-character slot paths, so nothing
 * generates one today. */
constexpr size_t KILN_PATH_MAX = 64;

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

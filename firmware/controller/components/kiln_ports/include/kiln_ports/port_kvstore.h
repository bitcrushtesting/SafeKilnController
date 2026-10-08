/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Key/value blobs (NVS on target): configuration and the latched fault.
 */
#ifndef KILN_PORT_KVSTORE_H
#define KILN_PORT_KVSTORE_H

#include <stddef.h>
#include "kiln/err.h"

typedef struct kiln_port_kvstore {
    void *ctx;
    kiln_err_t (*get)(void *ctx, const char *ns, const char *key,
                      void *out, size_t cap, size_t *out_len);
    kiln_err_t (*set)(void *ctx, const char *ns, const char *key,
                      const void *data, size_t len);
    kiln_err_t (*erase)(void *ctx, const char *ns, const char *key);
    kiln_err_t (*commit)(void *ctx, const char *ns);
} kiln_port_kvstore_t;

#endif

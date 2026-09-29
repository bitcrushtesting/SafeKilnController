/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Result codes shared by every layer.  NFR-17: no failure is silently dropped,
 * so every fallible operation returns one of these.
 */
#ifndef KILN_ERR_H
#define KILN_ERR_H

typedef enum {
    KILN_OK = 0,
    KILN_ERR_INVALID_ARG,
    KILN_ERR_RANGE,
    KILN_ERR_STATE,
    KILN_ERR_IO,
    KILN_ERR_TIMEOUT,
    KILN_ERR_NOT_FOUND,
    KILN_ERR_NO_SPACE,
    KILN_ERR_CORRUPT,
    KILN_ERR_UNSUPPORTED,
    KILN_ERR_BUSY,
    KILN_ERR_COUNT,
} kiln_err_t;

/* Implemented in kiln_core/src/err.c (kiln_ports is headers only). */
const char *kiln_err_str(kiln_err_t err);

#endif /* KILN_ERR_H */

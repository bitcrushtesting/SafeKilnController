/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "kiln/err.h"

static const char *const k_err[KILN_ERR_COUNT] = {
    [KILN_OK]                  = "ok",
    [KILN_ERR_INVALID_ARG]     = "invalid argument",
    [KILN_ERR_RANGE]           = "out of range",
    [KILN_ERR_STATE]           = "not allowed in this state",
    [KILN_ERR_IO]              = "io error",
    [KILN_ERR_TIMEOUT]         = "timeout",
    [KILN_ERR_NOT_FOUND]       = "not found",
    [KILN_ERR_NO_SPACE]        = "no space",
    [KILN_ERR_CORRUPT]         = "corrupt data",
    [KILN_ERR_UNSUPPORTED]     = "unsupported",
    [KILN_ERR_BUSY]            = "busy",
};

const char *kiln_err_str(kiln_err_t err)
{
    if ((unsigned)err >= KILN_ERR_COUNT || !k_err[err]) return "unknown error";
    return k_err[err];
}

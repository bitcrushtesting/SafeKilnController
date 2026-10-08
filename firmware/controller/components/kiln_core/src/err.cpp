/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "kiln/err.h"

/* A switch rather than the array the C version indexed by designator.  C++ has
 * no array designators, and the straight translation -- a positional array --
 * would silently shift every string if an enumerator were ever inserted in the
 * middle.  Under -Wswitch-enum -Werror this form is strictly stronger than what
 * C gave us: a new kiln_err_t is a *compile error* until it is given a string,
 * rather than a lookup that quietly returns the wrong one. */
const char *kiln_err_str(kiln_err_t err)
{
    switch (err) {
    case KILN_OK:              return "ok";
    case KILN_ERR_INVALID_ARG: return "invalid argument";
    case KILN_ERR_RANGE:       return "out of range";
    case KILN_ERR_STATE:       return "not allowed in this state";
    case KILN_ERR_IO:          return "io error";
    case KILN_ERR_TIMEOUT:     return "timeout";
    case KILN_ERR_NOT_FOUND:   return "not found";
    case KILN_ERR_NO_SPACE:    return "no space";
    case KILN_ERR_CORRUPT:     return "corrupt data";
    case KILN_ERR_UNSUPPORTED: return "unsupported";
    case KILN_ERR_BUSY:        return "busy";
    case KILN_ERR_COUNT:       break;   /* not a value, only the bound */
    }
    return "unknown error";
}

/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The HTTP transport for the REST API (architecture 12.2).
 *
 * Target-only: it needs esp_http_server.  Everything it serves is decided in
 * api.cpp, which has no platform dependency and is covered by the host API
 * suite; this is the socket half and nothing more.
 */
#ifndef KILN_WEB_HTTPD_H
#define KILN_WEB_HTTPD_H

#include "kiln_web/api.h"

/* Start the server on port 80.  The context must outlive it, which in practice
 * means it is static in the composition root. */
kiln_err_t kiln_httpd_start(kiln_api_ctx_t *api);
void       kiln_httpd_stop(void);

#endif /* KILN_WEB_HTTPD_H */

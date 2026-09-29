/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * 128x64 monochrome OLED (HR-04).  The HMI renders into a framebuffer that the
 * port merely transfers, so screens are verifiable by golden-image comparison
 * on the host without any display present.
 */
#ifndef KILN_PORT_DISPLAY_H
#define KILN_PORT_DISPLAY_H

#include "kiln/err.h"
#include "kiln/types.h"

#define KILN_DISPLAY_W       128
#define KILN_DISPLAY_H       64
#define KILN_DISPLAY_PAGES   (KILN_DISPLAY_H / 8)
#define KILN_DISPLAY_BYTES   (KILN_DISPLAY_W * KILN_DISPLAY_PAGES)

typedef struct kiln_port_display {
    void *ctx;
    kiln_err_t (*present)(void *ctx, const uint8_t *fb, size_t len);
    kiln_err_t (*set_contrast)(void *ctx, uint8_t contrast);
    bool       (*available)(void *ctx);   /* FR-HMI-14 */
} kiln_port_display_t;

#endif

/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Framebuffer primitives for the 128x64 monochrome panel (HR-04).
 *
 * Deliberately tiny and deliberately pure: no platform header, no allocation,
 * no display.  The HMI composes a frame here and port_display transfers it,
 * which is what lets every screen be compared against a golden image on the
 * host with no hardware present (architecture 5.4, TR-01).
 *
 * Memory layout is the SSD1306's own: one byte spans eight rows of a page, bit
 * 0 at the top, pages stacked down the screen.  Keeping the panel's layout
 * here rather than converting in the adapter means the adapter is a transfer
 * and nothing else.
 */
#ifndef KILN_HMI_DRAW_H
#define KILN_HMI_DRAW_H

#include "kiln_ports/port_display.h"

typedef struct {
    uint8_t px[KILN_DISPLAY_BYTES];
} kiln_fb_t;

void kiln_fb_clear(kiln_fb_t *fb);
void kiln_fb_pixel(kiln_fb_t *fb, int x, int y, bool on);
void kiln_fb_hline(kiln_fb_t *fb, int x, int y, int w, bool on);
void kiln_fb_vline(kiln_fb_t *fb, int x, int y, int h, bool on);
void kiln_fb_rect(kiln_fb_t *fb, int x, int y, int w, int h, bool on);
void kiln_fb_fill(kiln_fb_t *fb, int x, int y, int w, int h, bool on);

/* 5x7 glyphs on a 6x8 cell.  `scale` multiplies both axes, so the large
 * temperature of FR-HMI-03 is this font at scale 3 rather than a second table
 * to get wrong: one font, one place to fix a glyph. */
void kiln_fb_text(kiln_fb_t *fb, int x, int y, const char *s, int scale, bool on);
void kiln_fb_text_inv(kiln_fb_t *fb, int x, int y, const char *s, int scale);

int  kiln_fb_text_width(const char *s, int scale);

/* Right-aligned, for columns of numbers that would otherwise jitter. */
void kiln_fb_text_right(kiln_fb_t *fb, int right_x, int y, const char *s, int scale);

/* A horizontal bar with a border, for FR-HMI-04's progress indication. */
void kiln_fb_progress(kiln_fb_t *fb, int x, int y, int w, int h, uint8_t percent);

#endif /* KILN_HMI_DRAW_H */

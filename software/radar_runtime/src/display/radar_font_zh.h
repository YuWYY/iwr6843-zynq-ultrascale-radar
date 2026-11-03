/* SPDX-License-Identifier: MIT */
#ifndef RADAR_FONT_ZH_H
#define RADAR_FONT_ZH_H
#include <stdint.h>

/* Local, immutable 4-bit coverage glyphs. No allocation, IO or font loading. */
int radar_font_zh_has_utf8(const char *text);
int radar_font_zh_has_glyph(unsigned font_px, uint32_t codepoint);
int radar_font_zh_measure(unsigned font_px, const char *utf8);
int radar_font_zh_draw(uint32_t *pixels, unsigned stride, unsigned width,
                       unsigned height, int x, int y, unsigned font_px,
                       uint32_t color, const char *utf8, int max_width);
#endif

/*
 * This file is part of the Pico FIDO Touch distribution.
 *
 * Tiny text/primitive renderer on top of the ST7789 driver. Uses the
 * public-domain 8x8 bitmap font (font8x8_basic.h) scaled by an integer
 * factor. Intentionally dependency-free so the firmware builds without a
 * heavyweight GUI toolkit; see docs/TOUCH_UI_PLAN.md for the LVGL path.
 */

#ifndef PICO_FIDO_GFX_H
#define PICO_FIDO_GFX_H

#include <stdint.h>

#define GFX_GLYPH_W 8
#define GFX_GLYPH_H 8
#define GFX_MAX_SCALE 4

/* Width in pixels of a string rendered at the given integer scale. */
int gfx_text_width(const char *s, int scale);

/* Draw a NUL-terminated string with its top-left at (x, y). */
void gfx_draw_text(int x, int y, const char *s, int scale,
                   uint16_t fg, uint16_t bg);

/* Draw a string horizontally centred within [0, LCD_WIDTH). */
void gfx_draw_text_centered(int y, const char *s, int scale,
                            uint16_t fg, uint16_t bg);

#endif /* PICO_FIDO_GFX_H */

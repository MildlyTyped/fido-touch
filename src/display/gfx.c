/*
 * This file is part of the Pico FIDO Touch distribution.
 *
 * Tiny text renderer, see gfx.h.
 */

#include "gfx.h"
#include "st7789.h"
#include "board_config.h"
#include "font8x8_basic.h"

#include <string.h>

int gfx_text_width(const char *s, int scale) {
    return (int)strlen(s) * GFX_GLYPH_W * scale;
}

static void draw_glyph(int x, int y, unsigned char c, int scale,
                       uint16_t fg, uint16_t bg) {
    if (c >= 128) {
        c = '?';
    }
    const char *glyph = font8x8_basic[c];
    /* Render into a scaled cell buffer, then blit in one shot. */
    uint16_t cell[GFX_GLYPH_W * GFX_MAX_SCALE * GFX_GLYPH_H * GFX_MAX_SCALE];
    int cw = GFX_GLYPH_W * scale;
    int ch = GFX_GLYPH_H * scale;

    for (int row = 0; row < GFX_GLYPH_H; row++) {
        uint8_t bits = (uint8_t)glyph[row];
        for (int col = 0; col < GFX_GLYPH_W; col++) {
            uint16_t color = (bits & (1u << col)) ? fg : bg;
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    int px = col * scale + sx;
                    int py = row * scale + sy;
                    cell[py * cw + px] = color;
                }
            }
        }
    }
    st7789_blit(x, y, cw, ch, cell);
}

void gfx_draw_text(int x, int y, const char *s, int scale,
                   uint16_t fg, uint16_t bg) {
    if (scale < 1) {
        scale = 1;
    }
    if (scale > GFX_MAX_SCALE) {
        scale = GFX_MAX_SCALE;
    }
    int cx = x;
    for (const char *p = s; *p; p++) {
        draw_glyph(cx, y, (unsigned char)*p, scale, fg, bg);
        cx += GFX_GLYPH_W * scale;
    }
}

void gfx_draw_text_centered(int y, const char *s, int scale,
                            uint16_t fg, uint16_t bg) {
    int w = gfx_text_width(s, scale);
    int x = (LCD_WIDTH - w) / 2;
    if (x < 0) {
        x = 0;
    }
    gfx_draw_text(x, y, s, scale, fg, bg);
}

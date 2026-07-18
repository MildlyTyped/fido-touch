/*
 * This file is part of the Pico FIDO Touch distribution.
 *
 * Minimal ST7789V2 SPI display driver for the Waveshare
 * RP2040-Touch-LCD-1.69 (240x280, portrait).
 */

#ifndef PICO_FIDO_ST7789_H
#define PICO_FIDO_ST7789_H

#include <stdint.h>
#include <stdbool.h>

/* RGB565 colour helpers. */
#define ST7789_RGB(r, g, b) \
    ((uint16_t)(((((uint16_t)(r)) & 0xF8) << 8) | \
                ((((uint16_t)(g)) & 0xFC) << 3) | \
                (((uint16_t)(b)) >> 3)))

#define ST7789_BLACK   ST7789_RGB(0x00, 0x00, 0x00)
#define ST7789_WHITE   ST7789_RGB(0xFF, 0xFF, 0xFF)
#define ST7789_RED     ST7789_RGB(0xE0, 0x1B, 0x24)
#define ST7789_GREEN   ST7789_RGB(0x1F, 0xA8, 0x55)
#define ST7789_BLUE    ST7789_RGB(0x1E, 0x6F, 0xE0)
#define ST7789_YELLOW  ST7789_RGB(0xF2, 0xC0, 0x1E)
#define ST7789_GREY    ST7789_RGB(0x60, 0x66, 0x70)

/* Initialise the SPI bus, control pins and the panel registers. */
void st7789_init(void);

/* Backlight on/off (full-on; PWM dimming is left as a follow-up). */
void st7789_set_backlight(bool on);

/* Fill the whole screen with a single colour. */
void st7789_clear(uint16_t color);

/* Fill an axis-aligned rectangle. Coordinates are clipped to the panel. */
void st7789_fill_rect(int x, int y, int w, int h, uint16_t color);

/* Stream a caller-provided RGB565 buffer into a rectangle (row-major). */
void st7789_blit(int x, int y, int w, int h, const uint16_t *pixels);

#endif /* PICO_FIDO_ST7789_H */

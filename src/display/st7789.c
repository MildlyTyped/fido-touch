/*
 * This file is part of the Pico FIDO Touch distribution.
 *
 * Minimal ST7789V2 SPI display driver for the Waveshare
 * RP2040-Touch-LCD-1.69. The register init sequence mirrors the Waveshare
 * vendor demo (lib/LCD/LCD_1in69.c).
 */

#include "st7789.h"
#include "board_config.h"

#if !defined(ENABLE_EMULATION)
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

static inline void cs_low(void)  { gpio_put(LCD_PIN_CS, 0); }
static inline void cs_high(void) { gpio_put(LCD_PIN_CS, 1); }

static void write_cmd(uint8_t cmd) {
    gpio_put(LCD_PIN_DC, 0);
    cs_low();
    spi_write_blocking(LCD_SPI_PORT, &cmd, 1);
    cs_high();
}

static void write_data(const uint8_t *data, size_t len) {
    gpio_put(LCD_PIN_DC, 1);
    cs_low();
    spi_write_blocking(LCD_SPI_PORT, data, len);
    cs_high();
}

static void write_data8(uint8_t v) {
    write_data(&v, 1);
}

static void reset_panel(void) {
    gpio_put(LCD_PIN_RST, 1);
    sleep_ms(100);
    gpio_put(LCD_PIN_RST, 0);
    sleep_ms(100);
    gpio_put(LCD_PIN_RST, 1);
    sleep_ms(100);
}

static void init_registers(void) {
    write_cmd(0x36); write_data8(0x00);          /* MADCTL: portrait */
    write_cmd(0x3A); write_data8(0x05);          /* COLMOD: 16 bit/px */

    write_cmd(0xB2);
    { const uint8_t d[] = {0x0B, 0x0B, 0x00, 0x33, 0x35}; write_data(d, sizeof(d)); }
    write_cmd(0xB7); write_data8(0x11);
    write_cmd(0xBB); write_data8(0x35);
    write_cmd(0xC0); write_data8(0x2C);
    write_cmd(0xC2); write_data8(0x01);
    write_cmd(0xC3); write_data8(0x0D);
    write_cmd(0xC4); write_data8(0x20);
    write_cmd(0xC6); write_data8(0x13);
    write_cmd(0xD0);
    { const uint8_t d[] = {0xA4, 0xA1}; write_data(d, sizeof(d)); }
    write_cmd(0xD6); write_data8(0xA1);

    write_cmd(0xE0);
    { const uint8_t d[] = {0xF0, 0x06, 0x0B, 0x0A, 0x09, 0x26, 0x29, 0x33,
                           0x41, 0x18, 0x16, 0x15, 0x29, 0x2D}; write_data(d, sizeof(d)); }
    write_cmd(0xE1);
    { const uint8_t d[] = {0xF0, 0x04, 0x08, 0x08, 0x07, 0x03, 0x28, 0x32,
                           0x40, 0x3B, 0x19, 0x18, 0x2A, 0x2E}; write_data(d, sizeof(d)); }
    write_cmd(0xE4);
    { const uint8_t d[] = {0x25, 0x00, 0x00}; write_data(d, sizeof(d)); }

    write_cmd(0x21);                              /* display inversion on */
    write_cmd(0x11);                              /* sleep out */
    sleep_ms(120);
    write_cmd(0x29);                              /* display on */
}

static void set_window(int x0, int y0, int x1, int y1) {
    uint16_t xs = (uint16_t)(x0 + LCD_COL_OFFSET);
    uint16_t xe = (uint16_t)(x1 + LCD_COL_OFFSET);
    uint16_t ys = (uint16_t)(y0 + LCD_ROW_OFFSET);
    uint16_t ye = (uint16_t)(y1 + LCD_ROW_OFFSET);

    write_cmd(0x2A);
    { const uint8_t d[] = {xs >> 8, xs & 0xFF, xe >> 8, xe & 0xFF}; write_data(d, sizeof(d)); }
    write_cmd(0x2B);
    { const uint8_t d[] = {ys >> 8, ys & 0xFF, ye >> 8, ye & 0xFF}; write_data(d, sizeof(d)); }
    write_cmd(0x2C);                              /* memory write */
}

static bool clip_rect(int *x, int *y, int *w, int *h) {
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x >= LCD_WIDTH || *y >= LCD_HEIGHT) { return false; }
    if (*x + *w > LCD_WIDTH)  { *w = LCD_WIDTH - *x; }
    if (*y + *h > LCD_HEIGHT) { *h = LCD_HEIGHT - *y; }
    return (*w > 0 && *h > 0);
}

void st7789_init(void) {
    spi_init(LCD_SPI_PORT, LCD_SPI_BAUD);
    gpio_set_function(LCD_PIN_CLK, GPIO_FUNC_SPI);
    gpio_set_function(LCD_PIN_MOSI, GPIO_FUNC_SPI);

    gpio_init(LCD_PIN_CS);  gpio_set_dir(LCD_PIN_CS, GPIO_OUT);  gpio_put(LCD_PIN_CS, 1);
    gpio_init(LCD_PIN_DC);  gpio_set_dir(LCD_PIN_DC, GPIO_OUT);  gpio_put(LCD_PIN_DC, 0);
    gpio_init(LCD_PIN_RST); gpio_set_dir(LCD_PIN_RST, GPIO_OUT); gpio_put(LCD_PIN_RST, 1);
    gpio_init(LCD_PIN_BL);  gpio_set_dir(LCD_PIN_BL, GPIO_OUT);  gpio_put(LCD_PIN_BL, 0);

    reset_panel();
    init_registers();
    st7789_clear(ST7789_BLACK);
    st7789_set_backlight(true);
}

void st7789_set_backlight(bool on) {
    gpio_put(LCD_PIN_BL, on ? 1 : 0);
}

void st7789_fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (!clip_rect(&x, &y, &w, &h)) {
        return;
    }
    set_window(x, y, x + w - 1, y + h - 1);

    uint8_t line[LCD_WIDTH * 2];
    for (int i = 0; i < w; i++) {
        line[i * 2] = color >> 8;
        line[i * 2 + 1] = color & 0xFF;
    }
    gpio_put(LCD_PIN_DC, 1);
    cs_low();
    for (int row = 0; row < h; row++) {
        spi_write_blocking(LCD_SPI_PORT, line, (size_t)w * 2);
    }
    cs_high();
}

void st7789_blit(int x, int y, int w, int h, const uint16_t *pixels) {
    if (x < 0 || y < 0 || x + w > LCD_WIDTH || y + h > LCD_HEIGHT) {
        return;
    }
    set_window(x, y, x + w - 1, y + h - 1);

    uint8_t line[LCD_WIDTH * 2];
    gpio_put(LCD_PIN_DC, 1);
    cs_low();
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            uint16_t c = pixels[row * w + col];
            line[col * 2] = c >> 8;
            line[col * 2 + 1] = c & 0xFF;
        }
        spi_write_blocking(LCD_SPI_PORT, line, (size_t)w * 2);
    }
    cs_high();
}

void st7789_clear(uint16_t color) {
    st7789_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, color);
}

#else /* ENABLE_EMULATION: no hardware, provide no-op stubs. */

void st7789_init(void) {}
void st7789_set_backlight(bool on) { (void)on; }
void st7789_clear(uint16_t color) { (void)color; }
void st7789_fill_rect(int x, int y, int w, int h, uint16_t color) {
    (void)x; (void)y; (void)w; (void)h; (void)color;
}
void st7789_blit(int x, int y, int w, int h, const uint16_t *pixels) {
    (void)x; (void)y; (void)w; (void)h; (void)pixels;
}

#endif

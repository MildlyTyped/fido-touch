/*
 * This file is part of the Pico FIDO Touch distribution.
 *
 * Board pin mapping for the Waveshare RP2040-Touch-LCD-1.69.
 *   https://www.waveshare.com/wiki/RP2040-Touch-LCD-1.69
 *
 * Values are taken from the vendor C demo (lib/Config/DEV_Config.h and
 * the lib/LCD sources). They MUST be verified against the board schematic
 * before flashing on real hardware.
 */

#ifndef PICO_FIDO_BOARD_CONFIG_H
#define PICO_FIDO_BOARD_CONFIG_H

/* ----- Display: ST7789V2, SPI (spi1) ------------------------------------ */
#define LCD_SPI_PORT     spi1
#define LCD_SPI_BAUD     (40u * 1000u * 1000u) /* 40 MHz */
#define LCD_PIN_DC       8
#define LCD_PIN_CS       9
#define LCD_PIN_CLK      10
#define LCD_PIN_MOSI     11
#define LCD_PIN_MISO     12 /* unused by the panel, reserved */
#define LCD_PIN_RST      13
#define LCD_PIN_BL       25 /* backlight, driven by PWM in the vendor demo */

/* Native panel resolution (portrait). The ST7789 GRAM is 240x320 and the
 * 280-tall visible window is centred, hence the 20px row offset. */
#define LCD_WIDTH        240
#define LCD_HEIGHT       280
#define LCD_ROW_OFFSET   20
#define LCD_COL_OFFSET   0

/* ----- Touch + IMU shared I2C bus (i2c1) -------------------------------- */
#define DEV_I2C_PORT     i2c1
#define DEV_I2C_BAUD     (400u * 1000u) /* 400 kHz */
#define DEV_PIN_SDA      6
#define DEV_PIN_SCL      7
#define TOUCH_PIN_INT    21
#define TOUCH_PIN_RST    22

/* CST816T capacitive touch controller (I2C 7-bit address). */
#define CST816_I2C_ADDR  0x15

/* QMI8658 6-axis IMU (optional, shares the I2C bus). SA0 selects address. */
#define QMI8658_I2C_ADDR 0x6B

/* ----- Battery sense ---------------------------------------------------- */
/* The board exposes a Li-ion charger; battery voltage is read on an ADC pin
 * through a divider. Pin/divider must be confirmed against the schematic. */
#define BATTERY_ADC_PIN     29
#define BATTERY_ADC_CHANNEL 3
#define BATTERY_DIVIDER     3.0f
#define BATTERY_ADC_VREF    3.3f

#endif /* PICO_FIDO_BOARD_CONFIG_H */

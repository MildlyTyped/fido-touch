/*
 * Pico SDK board definition for the Waveshare RP2040-Touch-LCD-1.69.
 *
 * Use with:
 *   cmake .. -DPICO_BOARD=waveshare_rp2040_touch_lcd_1_69 \
 *            -DPICO_BOARD_HEADER_DIRS=${CMAKE_SOURCE_DIR}/src/boards \
 *            -DENABLE_DISPLAY_UI=1
 *
 * It is a plain RP2040 with 16MB of QSPI flash plus an on-board ST7789V2
 * LCD, CST816T touch, QMI8658 IMU and a Li-ion charger. Peripheral pins live
 * in src/display/board_config.h.
 */

#ifndef _BOARDS_WAVESHARE_RP2040_TOUCH_LCD_1_69_H
#define _BOARDS_WAVESHARE_RP2040_TOUCH_LCD_1_69_H

// For board detection
#define WAVESHARE_RP2040_TOUCH_LCD_1_69

// The board has no user-controllable single-colour LED broken out; status is
// shown on the LCD. Leave PICO_DEFAULT_LED_PIN undefined.

#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1
#endif

#ifndef PICO_DEFAULT_I2C
#define PICO_DEFAULT_I2C 1
#endif
#ifndef PICO_DEFAULT_I2C_SDA_PIN
#define PICO_DEFAULT_I2C_SDA_PIN 6
#endif
#ifndef PICO_DEFAULT_I2C_SCL_PIN
#define PICO_DEFAULT_I2C_SCL_PIN 7
#endif

#ifndef PICO_DEFAULT_SPI
#define PICO_DEFAULT_SPI 1
#endif
#ifndef PICO_DEFAULT_SPI_SCK_PIN
#define PICO_DEFAULT_SPI_SCK_PIN 10
#endif
#ifndef PICO_DEFAULT_SPI_TX_PIN
#define PICO_DEFAULT_SPI_TX_PIN 11
#endif
#ifndef PICO_DEFAULT_SPI_RX_PIN
#define PICO_DEFAULT_SPI_RX_PIN 12
#endif

// --- Flash: 16MB W25Q128 ---
#ifndef PICO_BOOT_STAGE2_CHOOSE_W25Q080
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1
#endif
#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

#ifndef PICO_RP2040
#define PICO_RP2040 1
#endif

#endif

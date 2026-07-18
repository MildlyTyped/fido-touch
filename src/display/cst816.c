/*
 * This file is part of the Pico FIDO Touch distribution.
 *
 * Minimal CST816T capacitive-touch driver, see cst816.h.
 */

#include "cst816.h"
#include "board_config.h"

#if !defined(ENABLE_EMULATION)
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"

/* CST816T register addresses (subset). */
#define REG_GESTURE_ID 0x01
#define REG_FINGER_NUM 0x02
#define REG_XPOS_H     0x03

static bool i2c_bus_ready = false;

static int read_regs(uint8_t reg, uint8_t *buf, size_t len) {
    int ret = i2c_write_blocking(DEV_I2C_PORT, CST816_I2C_ADDR, &reg, 1, true);
    if (ret < 0) {
        return ret;
    }
    return i2c_read_blocking(DEV_I2C_PORT, CST816_I2C_ADDR, buf, len, false);
}

void cst816_init(void) {
    /* The display driver may already own the I2C pins; configuring them here
       is idempotent and keeps the touch driver self-contained. */
    i2c_init(DEV_I2C_PORT, DEV_I2C_BAUD);
    gpio_set_function(DEV_PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(DEV_PIN_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(DEV_PIN_SDA);
    gpio_pull_up(DEV_PIN_SCL);

    gpio_init(TOUCH_PIN_INT);
    gpio_set_dir(TOUCH_PIN_INT, GPIO_IN);
    gpio_pull_up(TOUCH_PIN_INT);

    gpio_init(TOUCH_PIN_RST);
    gpio_set_dir(TOUCH_PIN_RST, GPIO_OUT);
    gpio_put(TOUCH_PIN_RST, 0);
    sleep_ms(10);
    gpio_put(TOUCH_PIN_RST, 1);
    sleep_ms(50);

    i2c_bus_ready = true;
}

bool cst816_read(cst816_touch_t *out) {
    if (out == NULL) {
        return false;
    }
    out->pressed = false;
    out->x = out->y = 0;
    out->gesture = CST816_GESTURE_NONE;

    if (!i2c_bus_ready) {
        return false;
    }

    uint8_t buf[6] = {0};
    /* buf[0]=gesture, buf[1]=finger count, buf[2]=XH, buf[3]=XL,
       buf[4]=YH, buf[5]=YL. */
    if (read_regs(REG_GESTURE_ID, buf, sizeof(buf)) < 0) {
        return false;
    }

    uint8_t fingers = buf[1] & 0x0F;
    if (fingers == 0) {
        return false;
    }

    out->pressed = true;
    out->gesture = (cst816_gesture_t)buf[0];
    out->x = (uint16_t)(((buf[2] & 0x0F) << 8) | buf[3]);
    out->y = (uint16_t)(((buf[4] & 0x0F) << 8) | buf[5]);
    return true;
}

#else /* ENABLE_EMULATION */

void cst816_init(void) {}
bool cst816_read(cst816_touch_t *out) {
    if (out != NULL) {
        out->pressed = false;
        out->x = out->y = 0;
        out->gesture = CST816_GESTURE_NONE;
    }
    return false;
}

#endif

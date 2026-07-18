/*
 * This file is part of the Pico FIDO Touch distribution.
 *
 * Minimal CST816T capacitive-touch driver (I2C) for the Waveshare
 * RP2040-Touch-LCD-1.69. Register map from the vendor demo
 * (lib/LCD/Touch_1in69.h).
 */

#ifndef PICO_FIDO_CST816_H
#define PICO_FIDO_CST816_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    CST816_GESTURE_NONE        = 0x00,
    CST816_GESTURE_SLIDE_DOWN  = 0x01,
    CST816_GESTURE_SLIDE_UP    = 0x02,
    CST816_GESTURE_SLIDE_LEFT  = 0x03,
    CST816_GESTURE_SLIDE_RIGHT = 0x04,
    CST816_GESTURE_CLICK       = 0x05,
    CST816_GESTURE_DOUBLE_CLICK = 0x0B,
    CST816_GESTURE_LONG_PRESS  = 0x0C,
} cst816_gesture_t;

typedef struct {
    bool pressed;
    uint16_t x;
    uint16_t y;
    cst816_gesture_t gesture;
} cst816_touch_t;

/* Reset the controller and configure the shared I2C bus + INT/RST pins. */
void cst816_init(void);

/* Poll the current touch state. Returns true when a finger is present. */
bool cst816_read(cst816_touch_t *out);

#endif /* PICO_FIDO_CST816_H */

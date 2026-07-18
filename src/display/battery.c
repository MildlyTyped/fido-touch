/*
 * This file is part of the Pico FIDO Touch distribution.
 *
 * Battery voltage sensing, see battery.h.
 */

#include "battery.h"
#include "board_config.h"

#if !defined(ENABLE_EMULATION)
#include "pico/stdlib.h"
#include "hardware/adc.h"

static bool adc_ready = false;

void battery_init(void) {
    adc_init();
    adc_gpio_init(BATTERY_ADC_PIN);
    adc_ready = true;
}

float battery_read_voltage(void) {
    if (!adc_ready) {
        return 0.0f;
    }
    adc_select_input(BATTERY_ADC_CHANNEL);
    uint16_t raw = adc_read();
    float v = ((float)raw * BATTERY_ADC_VREF / 4095.0f) * BATTERY_DIVIDER;
    return v;
}

int battery_read_percent(void) {
    float v = battery_read_voltage();
    if (v <= 0.0f) {
        return -1;
    }
    /* Simple linear approximation over the usable Li-ion range. */
    const float vmin = 3.30f;
    const float vmax = 4.20f;
    if (v <= vmin) {
        return 0;
    }
    if (v >= vmax) {
        return 100;
    }
    return (int)((v - vmin) / (vmax - vmin) * 100.0f + 0.5f);
}

#else /* ENABLE_EMULATION */

void battery_init(void) {}
float battery_read_voltage(void) { return 0.0f; }
int battery_read_percent(void) { return -1; }

#endif

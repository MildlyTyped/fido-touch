/*
 * This file is part of the Pico FIDO Touch distribution.
 *
 * Battery voltage sensing for the Waveshare RP2040-Touch-LCD-1.69 status
 * screen. The ADC pin and divider ratio in board_config.h are best-effort
 * and must be confirmed against the schematic.
 */

#ifndef PICO_FIDO_BATTERY_H
#define PICO_FIDO_BATTERY_H

#include <stdbool.h>

void battery_init(void);

/* Battery voltage in volts (0 when unavailable). */
float battery_read_voltage(void);

/* Rough state-of-charge estimate in percent (0-100) from a Li-ion curve. */
int battery_read_percent(void);

#endif /* PICO_FIDO_BATTERY_H */

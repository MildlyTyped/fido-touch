/*
 * This file is part of the Pico FIDO Touch distribution.
 *
 * Minimal LVGL configuration for the Waveshare RP2040-Touch-LCD-1.69.
 * Only the values that differ from LVGL's built-in defaults are set here;
 * lv_conf_internal.h fills in the rest. See lv_conf_template.h in the LVGL
 * submodule for the full list of options.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* RGB565 to match the ST7789V2 panel. */
#define LV_COLOR_DEPTH 16

/* Use LVGL's built-in allocator with a small fixed pool (RP2040 has 264 KB
 * of SRAM shared with the FIDO firmware). */
#define LV_USE_STDLIB_MALLOC  LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING  LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_BUILTIN
#define LV_MEM_SIZE           (32U * 1024U)

/* Bare-metal, single core-0 caller; the tick is supplied at runtime via
 * lv_tick_set_cb(). */
#define LV_USE_OS       LV_OS_NONE
#define LV_DEF_REFR_PERIOD  20

/* Trim things we don't need to save flash/RAM. */
#define LV_USE_LOG      0
#define LV_USE_ASSERT_NULL      0
#define LV_USE_ASSERT_MALLOC    0

/* Panel is ~200 DPI-ish; affects default widget sizing. */
#define LV_DPI_DEF      130

/* Fonts used by the UI. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_DEFAULT &lv_font_montserrat_16

#endif /* LV_CONF_H */

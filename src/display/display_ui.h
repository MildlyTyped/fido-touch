/*
 * This file is part of the Pico FIDO Touch distribution.
 *
 * Touchscreen UI layer for the ESP32-S3 + Waveshare 2" Capacitive Touch LCD
 * (ST7789T3 over SPI + CST816 over I2C).
 *
 * Provides platform_ui_init()/platform_ui_task() (the hooks the pico-keys
 * SDK calls when ENABLE_DISPLAY_UI is defined) and drives three screens:
 *   A) an Approve/Deny prompt for FIDO user-presence confirmation,
 *   B) an idle status screen, and
 *   C) a management menu (credentials / OATH / info).
 *
 * See docs/TOUCH_UI_PLAN.md for the full design.
 */

#ifndef PICO_FIDO_DISPLAY_UI_H
#define PICO_FIDO_DISPLAY_UI_H

/* Called once from picokey_init(); sets up display, touch and signals. */
void platform_ui_init(void);

/* Called every core0 loop iteration from the SDK's execute_tasks(). */
void platform_ui_task(void);

/* Optional: let the FIDO app describe the operation awaiting confirmation
 * (e.g. relying-party id and user). Safe to call with NULL. The strings are
 * copied. If never called, the confirm screen shows a generic prompt. */
void display_ui_set_context(const char *rp_id, const char *user_name);

#endif /* PICO_FIDO_DISPLAY_UI_H */

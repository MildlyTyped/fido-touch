/*
 * This file is part of the Pico FIDO Touch distribution.
 *
 * Touchscreen UI layer, see display_ui.h.
 */

#include "display_ui.h"
#include "board_config.h"
#include "st7789.h"
#include "gfx.h"
#include "cst816.h"
#include "battery.h"

#include <string.h>
#include <stdio.h>

#include "picokeys.h"
#include "signal.h"
#include "button.h"
#include "pico_time.h"

/* Flags wired to the SDK user-presence loop (pico-keys-sdk/src/button.c). */
extern bool touch_accept_button;
extern volatile bool cancel_button;

/* ----- UI state --------------------------------------------------------- */
typedef enum {
    SCREEN_STATUS = 0,
    SCREEN_CONFIRM,
    SCREEN_MENU,
    SCREEN_CREDENTIALS,
    SCREEN_OATH,
    SCREEN_INFO,
} ui_screen_t;

typedef struct {
    int x, y, w, h;
} ui_rect_t;

static ui_screen_t g_screen = SCREEN_STATUS;
static bool g_dirty = true;
static bool g_mounted = false;

/* Confirm-screen context. */
static uint32_t g_confirm_start_ms = 0;
static uint32_t g_confirm_timeout_s = 0;
static int g_confirm_last_remaining = -1;
static char g_ctx_rp[40] = {0};
static char g_ctx_user[40] = {0};

/* Touch edge detection + polling cadence. */
static bool g_prev_pressed = false;
static uint32_t g_next_poll_ms = 0;
static uint32_t g_next_status_ms = 0;

/* Layout: confirm-screen buttons. */
static const ui_rect_t BTN_APPROVE = { 15, 170, 210, 45 };
static const ui_rect_t BTN_DENY    = { 15, 225, 210, 45 };

/* Layout: menu rows. */
#define MENU_ITEM_COUNT 4
static const char *const MENU_ITEMS[MENU_ITEM_COUNT] = {
    "Credentials", "OATH codes", "Device info", "Back"
};
static const ui_rect_t MENU_ROWS[MENU_ITEM_COUNT] = {
    { 10, 55, 220, 42 },
    { 10, 105, 220, 42 },
    { 10, 155, 220, 42 },
    { 10, 205, 220, 42 },
};

static bool rect_hit(const ui_rect_t *r, int x, int y) {
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

/* ----- Drawing ---------------------------------------------------------- */
static void draw_button(const ui_rect_t *r, const char *label, uint16_t bg,
                        uint16_t fg) {
    st7789_fill_rect(r->x, r->y, r->w, r->h, bg);
    int scale = 2;
    int tw = gfx_text_width(label, scale);
    int tx = r->x + (r->w - tw) / 2;
    int ty = r->y + (r->h - GFX_GLYPH_H * scale) / 2;
    gfx_draw_text(tx, ty, label, scale, fg, bg);
}

static void draw_status_screen(void) {
    st7789_clear(ST7789_BLACK);
    gfx_draw_text_centered(24, "PICO FIDO", 3, ST7789_WHITE, ST7789_BLACK);
    gfx_draw_text_centered(60, "touch key", 1, ST7789_GREY, ST7789_BLACK);

    gfx_draw_text_centered(110, g_mounted ? "Connected" : "Ready",
                           2, g_mounted ? ST7789_GREEN : ST7789_YELLOW,
                           ST7789_BLACK);

    char batt[24];
    int pct = battery_read_percent();
    if (pct >= 0) {
        snprintf(batt, sizeof(batt), "Battery %d%%", pct);
    } else {
        snprintf(batt, sizeof(batt), "Battery --");
    }
    gfx_draw_text_centered(160, batt, 2, ST7789_WHITE, ST7789_BLACK);

    gfx_draw_text_centered(250, "tap for menu", 1, ST7789_GREY, ST7789_BLACK);
}

static void draw_confirm_static(void) {
    st7789_clear(ST7789_BLACK);
    gfx_draw_text_centered(16, "CONFIRM", 3, ST7789_YELLOW, ST7789_BLACK);

    if (g_ctx_rp[0]) {
        gfx_draw_text_centered(64, g_ctx_rp, 2, ST7789_WHITE, ST7789_BLACK);
    } else {
        gfx_draw_text_centered(64, "User presence", 2, ST7789_WHITE,
                               ST7789_BLACK);
    }
    if (g_ctx_user[0]) {
        gfx_draw_text_centered(96, g_ctx_user, 1, ST7789_GREY, ST7789_BLACK);
    }

    draw_button(&BTN_APPROVE, "APPROVE", ST7789_GREEN, ST7789_BLACK);
    draw_button(&BTN_DENY, "DENY", ST7789_RED, ST7789_WHITE);
}

static void draw_confirm_countdown(int remaining) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%ds", remaining < 0 ? 0 : remaining);
    /* Clear the small countdown band then redraw. */
    st7789_fill_rect(0, 128, LCD_WIDTH, 28, ST7789_BLACK);
    gfx_draw_text_centered(130, buf, 2, ST7789_GREY, ST7789_BLACK);
}

static void draw_list_screen(const char *title, const char *const *items,
                             const ui_rect_t *rows, int count) {
    st7789_clear(ST7789_BLACK);
    gfx_draw_text_centered(14, title, 2, ST7789_WHITE, ST7789_BLACK);
    for (int i = 0; i < count; i++) {
        bool is_back = (strcmp(items[i], "Back") == 0);
        draw_button(&rows[i], items[i],
                    is_back ? ST7789_GREY : ST7789_BLUE, ST7789_WHITE);
    }
}

static void draw_info_screen(const char *title, const char *line1,
                             const char *line2) {
    st7789_clear(ST7789_BLACK);
    gfx_draw_text_centered(14, title, 2, ST7789_WHITE, ST7789_BLACK);
    if (line1) {
        gfx_draw_text_centered(80, line1, 1, ST7789_GREY, ST7789_BLACK);
    }
    if (line2) {
        gfx_draw_text_centered(110, line2, 1, ST7789_GREY, ST7789_BLACK);
    }
    draw_button(&MENU_ROWS[3], "Back", ST7789_GREY, ST7789_WHITE);
}

static void render_current_screen(void) {
    switch (g_screen) {
    case SCREEN_STATUS:
        draw_status_screen();
        break;
    case SCREEN_CONFIRM:
        draw_confirm_static();
        g_confirm_last_remaining = -1;
        break;
    case SCREEN_MENU:
        draw_list_screen("Menu", MENU_ITEMS, MENU_ROWS, MENU_ITEM_COUNT);
        break;
    case SCREEN_CREDENTIALS:
        draw_info_screen("Credentials", "Enumeration is a", "planned feature");
        break;
    case SCREEN_OATH:
        draw_info_screen("OATH codes", "TOTP display is a", "planned feature");
        break;
    case SCREEN_INFO:
        draw_info_screen("Device info", "Pico FIDO Touch", "RP2040 1.69in");
        break;
    }
}

static void goto_screen(ui_screen_t s) {
    g_screen = s;
    g_dirty = true;
}

/* ----- Input handling --------------------------------------------------- */
static void handle_tap(int x, int y) {
    switch (g_screen) {
    case SCREEN_STATUS:
        goto_screen(SCREEN_MENU);
        break;
    case SCREEN_CONFIRM:
        if (rect_hit(&BTN_APPROVE, x, y)) {
            touch_accept_button = true;
        } else if (rect_hit(&BTN_DENY, x, y)) {
            cancel_button = true;
        }
        break;
    case SCREEN_MENU:
        if (rect_hit(&MENU_ROWS[0], x, y)) {
            goto_screen(SCREEN_CREDENTIALS);
        } else if (rect_hit(&MENU_ROWS[1], x, y)) {
            goto_screen(SCREEN_OATH);
        } else if (rect_hit(&MENU_ROWS[2], x, y)) {
            goto_screen(SCREEN_INFO);
        } else if (rect_hit(&MENU_ROWS[3], x, y)) {
            goto_screen(SCREEN_STATUS);
        }
        break;
    case SCREEN_CREDENTIALS:
    case SCREEN_OATH:
    case SCREEN_INFO:
        goto_screen(SCREEN_MENU);
        break;
    }
}

static void poll_touch(void) {
    cst816_touch_t t;
    bool pressed = cst816_read(&t);
    if (pressed && !g_prev_pressed) {
        handle_tap(t.x, t.y);
    }
    g_prev_pressed = pressed;
}

/* ----- Signal handlers (run on core0 from button_wait / usb_task) ------- */
static int on_presence_request(signal_code_t code, void *data) {
    (void)code;
    signal_user_presence_request_data_t *d =
        (signal_user_presence_request_data_t *)data;
    g_confirm_timeout_s = d ? d->timeout : 30;
    g_confirm_start_ms = board_millis();
    goto_screen(SCREEN_CONFIRM);
    return 0;
}

static int on_presence_end(signal_code_t code, void *data) {
    (void)code;
    (void)data;
    g_ctx_rp[0] = g_ctx_user[0] = '\0';
    goto_screen(SCREEN_STATUS);
    return 0;
}

static int on_usb_mounted(signal_code_t code, void *data) {
    (void)code;
    (void)data;
    g_mounted = true;
    if (g_screen == SCREEN_STATUS) {
        g_dirty = true;
    }
    return 0;
}

/* ----- Public API + SDK hooks ------------------------------------------- */
void display_ui_set_context(const char *rp_id, const char *user_name) {
    if (rp_id) {
        strncpy(g_ctx_rp, rp_id, sizeof(g_ctx_rp) - 1);
        g_ctx_rp[sizeof(g_ctx_rp) - 1] = '\0';
    }
    if (user_name) {
        strncpy(g_ctx_user, user_name, sizeof(g_ctx_user) - 1);
        g_ctx_user[sizeof(g_ctx_user) - 1] = '\0';
    }
}

void platform_ui_init(void) {
    st7789_init();
    cst816_init();
    battery_init();

    signal_add(SIGNAL_USB_MOUNTED, SIGNAL_FLAG_NONE, on_usb_mounted);
    signal_add(SIGNAL_USER_PRESENCE_REQUEST, SIGNAL_FLAG_NONE,
               on_presence_request);
    signal_add(SIGNAL_USER_PRESENCE_COMPLETED, SIGNAL_FLAG_NONE,
               on_presence_end);
    signal_add(SIGNAL_USER_PRESENCE_CANCELLED, SIGNAL_FLAG_NONE,
               on_presence_end);
    signal_add(SIGNAL_USER_PRESENCE_TIMEOUT, SIGNAL_FLAG_NONE,
               on_presence_end);

    g_screen = SCREEN_STATUS;
    g_dirty = true;
}

void platform_ui_task(void) {
    uint32_t now = board_millis();

    if (now >= g_next_poll_ms) {
        poll_touch();
        g_next_poll_ms = now + 20;
    }

    if (g_dirty) {
        render_current_screen();
        g_dirty = false;
        g_next_status_ms = now + 1000;
    }

    if (g_screen == SCREEN_CONFIRM) {
        uint32_t elapsed = (now - g_confirm_start_ms) / 1000;
        int remaining = (int)g_confirm_timeout_s - (int)elapsed;
        if (remaining != g_confirm_last_remaining) {
            draw_confirm_countdown(remaining);
            g_confirm_last_remaining = remaining;
        }
    } else if (g_screen == SCREEN_STATUS && now >= g_next_status_ms) {
        /* Refresh the battery reading periodically. */
        g_dirty = true;
    }
}

/* Override the SDK's WEAK picokey_init() to bring up the touch UI at boot. */
int picokey_init(void) {
    platform_ui_init();
    return 0;
}

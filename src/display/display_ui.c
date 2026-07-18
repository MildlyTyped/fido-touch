/*
 * This file is part of the Pico FIDO Touch distribution.
 *
 * LVGL-based touchscreen UI layer, see display_ui.h.
 *
 * Screens:
 *   A) SCREEN_CONFIRM  - Approve/Deny prompt for FIDO user presence.
 *   B) SCREEN_STATUS   - idle status + battery (default screen).
 *   C) SCREEN_MENU + sub-screens - management UI: resident-credential and
 *      OATH-account lists (read-only) plus device info.
 *
 * LVGL runs single-threaded on core 0: platform_ui_task() (called from the
 * SDK's execute_tasks()) drives lv_timer_handler(), and the presence/USB
 * signal handlers - also dispatched on core 0 - just switch the active
 * screen, so there is no cross-core access to LVGL.
 */

#include "display_ui.h"
#include "board_config.h"
#include "st7789.h"
#include "cst816.h"
#include "battery.h"

#include <string.h>
#include <stdio.h>

#include "lvgl.h"

#include "picokeys.h"
#include "signal.h"
#include "button.h"
#include "pico_time.h"
#include "fido.h"

/* Flags wired to the SDK user-presence loop (pico-keys-sdk/src/button.c). */
extern bool touch_accept_button;
extern volatile bool cancel_button;
/* True while a USB transaction is in flight; gates the read-only store access
 * done for the management screens so it never races an active FIDO command. */
extern bool is_busy(void);

/* ----- UI state --------------------------------------------------------- */
typedef enum {
    SCREEN_STATUS = 0,
    SCREEN_CONFIRM,
    SCREEN_MENU,
    SCREEN_CREDENTIALS,
    SCREEN_OATH,
    SCREEN_INFO,
    SCREEN_COUNT,
} ui_screen_t;

static ui_screen_t g_screen = SCREEN_STATUS;
static bool g_mounted = false;

/* Confirm-screen context. */
static uint32_t g_confirm_start_ms = 0;
static uint32_t g_confirm_timeout_s = 0;
static int g_confirm_last_remaining = -1;
static char g_ctx_rp[40] = {0};
static char g_ctx_user[40] = {0};

static uint32_t g_next_status_ms = 0;

/* ----- LVGL objects ----------------------------------------------------- */
static lv_display_t *g_disp = NULL;
static lv_indev_t *g_indev = NULL;

static lv_obj_t *g_screens[SCREEN_COUNT] = {0};
static lv_obj_t *g_creds_list = NULL; /* management: resident credentials */
static lv_obj_t *g_oath_list = NULL;  /* management: OATH accounts         */
#define UI_MAX_LIST 24
static lv_obj_t *lbl_state = NULL;   /* status: Ready / Connected */
static lv_obj_t *lbl_batt = NULL;    /* status: battery %          */
static lv_obj_t *lbl_rp = NULL;      /* confirm: relying party     */
static lv_obj_t *lbl_user = NULL;    /* confirm: user name         */
static lv_obj_t *lbl_count = NULL;   /* confirm: countdown         */

/* Draw buffer: partial rendering, LCD_WIDTH * N lines of RGB565. */
#define DRAW_BUF_LINES 40
static uint8_t g_draw_buf[LCD_WIDTH * DRAW_BUF_LINES * 2]
    __attribute__((aligned(4)));

/* ----- Colours ---------------------------------------------------------- */
static inline lv_color_t col_white(void) { return lv_color_white(); }
static inline lv_color_t col_grey(void)  { return lv_color_make(0x9a, 0xa0, 0xac); }
static inline lv_color_t col_green(void) { return lv_color_make(0x1f, 0xa8, 0x55); }
static inline lv_color_t col_red(void)   { return lv_color_make(0xe0, 0x1b, 0x24); }
static inline lv_color_t col_yellow(void){ return lv_color_make(0xf2, 0xc0, 0x1e); }

/* ----- LVGL hardware callbacks ------------------------------------------ */
static uint32_t tick_cb(void) {
    return board_millis();
}

static void disp_flush(lv_display_t *disp, const lv_area_t *area,
                       uint8_t *px_map) {
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    st7789_blit(area->x1, area->y1, w, h, (const uint16_t *)px_map);
    lv_display_flush_ready(disp);
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    static int32_t last_x = 0, last_y = 0;
    cst816_touch_t t;
    if (cst816_read(&t)) {
        last_x = t.x;
        last_y = t.y;
        if (last_x >= LCD_WIDTH)  { last_x = LCD_WIDTH - 1; }
        if (last_y >= LCD_HEIGHT) { last_y = LCD_HEIGHT - 1; }
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    data->point.x = last_x;
    data->point.y = last_y;
}

/* ----- Navigation ------------------------------------------------------- */
static void refresh_list(ui_screen_t which);

static void show_screen(ui_screen_t s) {
    if (s >= SCREEN_COUNT || g_screens[s] == NULL) {
        return;
    }
    if (s == SCREEN_CREDENTIALS || s == SCREEN_OATH) {
        refresh_list(s);
    }
    g_screen = s;
    lv_screen_load(g_screens[s]);
}

static void nav_cb(lv_event_t *e) {
    ui_screen_t tgt = (ui_screen_t)(intptr_t)lv_event_get_user_data(e);
    show_screen(tgt);
}

static void approve_cb(lv_event_t *e) {
    (void)e;
    touch_accept_button = true;
}

static void deny_cb(lv_event_t *e) {
    (void)e;
    cancel_button = true;
}

/* ----- Screen builders -------------------------------------------------- */
static lv_obj_t *make_screen(void) {
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, lv_color_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

static void build_status_screen(void) {
    lv_obj_t *s = make_screen();
    g_screens[SCREEN_STATUS] = s;

    lv_obj_t *title = make_label(s, "PICO FIDO", &lv_font_montserrat_28,
                                 col_white());
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);

    lv_obj_t *sub = make_label(s, "touch key", &lv_font_montserrat_14,
                               col_grey());
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 56);

    lbl_state = make_label(s, "Ready", &lv_font_montserrat_20, col_yellow());
    lv_obj_align(lbl_state, LV_ALIGN_CENTER, 0, -10);

    lbl_batt = make_label(s, "Battery --", &lv_font_montserrat_20, col_white());
    lv_obj_align(lbl_batt, LV_ALIGN_CENTER, 0, 40);

    lv_obj_t *hint = make_label(s, "tap for menu", &lv_font_montserrat_14,
                                col_grey());
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -14);

    lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s, nav_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)SCREEN_MENU);
}

static void build_confirm_screen(void) {
    lv_obj_t *s = make_screen();
    g_screens[SCREEN_CONFIRM] = s;

    lv_obj_t *title = make_label(s, "CONFIRM", &lv_font_montserrat_28,
                                 col_yellow());
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    lbl_rp = make_label(s, "User presence", &lv_font_montserrat_20,
                        col_white());
    lv_obj_set_width(lbl_rp, LCD_WIDTH - 20);
    lv_obj_set_style_text_align(lbl_rp, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lbl_rp, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl_rp, LV_ALIGN_TOP_MID, 0, 58);

    lbl_user = make_label(s, "", &lv_font_montserrat_14, col_grey());
    lv_obj_set_width(lbl_user, LCD_WIDTH - 20);
    lv_obj_set_style_text_align(lbl_user, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lbl_user, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl_user, LV_ALIGN_TOP_MID, 0, 88);

    lbl_count = make_label(s, "", &lv_font_montserrat_20, col_grey());
    lv_obj_align(lbl_count, LV_ALIGN_TOP_MID, 0, 116);

    lv_obj_t *btn_ok = lv_button_create(s);
    lv_obj_set_size(btn_ok, LCD_WIDTH - 30, 46);
    lv_obj_align(btn_ok, LV_ALIGN_BOTTOM_MID, 0, -58);
    lv_obj_set_style_bg_color(btn_ok, col_green(), 0);
    lv_obj_t *l_ok = make_label(btn_ok, "APPROVE", &lv_font_montserrat_20,
                                col_white());
    lv_obj_center(l_ok);
    lv_obj_add_event_cb(btn_ok, approve_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_no = lv_button_create(s);
    lv_obj_set_size(btn_no, LCD_WIDTH - 30, 46);
    lv_obj_align(btn_no, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_color(btn_no, col_red(), 0);
    lv_obj_t *l_no = make_label(btn_no, "DENY", &lv_font_montserrat_20,
                                col_white());
    lv_obj_center(l_no);
    lv_obj_add_event_cb(btn_no, deny_cb, LV_EVENT_CLICKED, NULL);
}

static lv_obj_t *add_menu_item(lv_obj_t *list, const char *text,
                               ui_screen_t target) {
    lv_obj_t *b = lv_list_add_button(list, NULL, text);
    lv_obj_add_event_cb(b, nav_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)target);
    return b;
}

static void build_menu_screen(void) {
    lv_obj_t *s = make_screen();
    g_screens[SCREEN_MENU] = s;

    lv_obj_t *title = make_label(s, "Menu", &lv_font_montserrat_20,
                                 col_white());
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *list = lv_list_create(s);
    lv_obj_set_size(list, LCD_WIDTH, LCD_HEIGHT - 44);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);

    add_menu_item(list, "Credentials", SCREEN_CREDENTIALS);
    add_menu_item(list, "OATH codes", SCREEN_OATH);
    add_menu_item(list, "Device info", SCREEN_INFO);
    add_menu_item(list, "Back", SCREEN_STATUS);
}

static void build_info_screen(ui_screen_t which, const char *title,
                              const char *line1, const char *line2) {
    lv_obj_t *s = make_screen();
    g_screens[which] = s;

    lv_obj_t *t = make_label(s, title, &lv_font_montserrat_20, col_white());
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 14);

    if (line1) {
        lv_obj_t *l1 = make_label(s, line1, &lv_font_montserrat_16,
                                  col_grey());
        lv_obj_align(l1, LV_ALIGN_CENTER, 0, -12);
    }
    if (line2) {
        lv_obj_t *l2 = make_label(s, line2, &lv_font_montserrat_16,
                                  col_grey());
        lv_obj_align(l2, LV_ALIGN_CENTER, 0, 14);
    }

    lv_obj_t *back = lv_button_create(s);
    lv_obj_set_size(back, LCD_WIDTH - 30, 44);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_t *lb = make_label(back, "Back", &lv_font_montserrat_20,
                              col_white());
    lv_obj_center(lb);
    lv_obj_add_event_cb(back, nav_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)SCREEN_MENU);
}

static lv_obj_t *build_list_screen(ui_screen_t which, const char *title,
                                   const char *caption) {
    lv_obj_t *s = make_screen();
    g_screens[which] = s;

    lv_obj_t *t = make_label(s, title, &lv_font_montserrat_20, col_white());
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 6);

    int list_top = 34;
    if (caption) {
        lv_obj_t *c = make_label(s, caption, &lv_font_montserrat_14,
                                 col_grey());
        lv_obj_set_width(c, LCD_WIDTH - 12);
        lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 30);
        list_top = 50;
    }

    lv_obj_t *list = lv_list_create(s);
    lv_obj_set_size(list, LCD_WIDTH, LCD_HEIGHT - list_top - 50);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, list_top);

    lv_obj_t *back = lv_button_create(s);
    lv_obj_set_size(back, LCD_WIDTH - 30, 40);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_t *lb = make_label(back, "Back", &lv_font_montserrat_20,
                              col_white());
    lv_obj_center(lb);
    lv_obj_add_event_cb(back, nav_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)SCREEN_MENU);
    return list;
}

static void build_ui(void) {
    build_status_screen();
    build_confirm_screen();
    build_menu_screen();
    g_creds_list = build_list_screen(SCREEN_CREDENTIALS, "Credentials", NULL);
    g_oath_list = build_list_screen(SCREEN_OATH, "OATH accounts",
                                    "Codes need host time");
    build_info_screen(SCREEN_INFO, "Device info",
                      "Pico FIDO Touch", "RP2040 1.69\" LCD");
}

/* Populate a management list from the read-only FIDO/OATH accessors. Runs on
 * core 0 and is skipped while a transaction is active (is_busy()). */
static void refresh_list(ui_screen_t which) {
    lv_obj_t *list = (which == SCREEN_CREDENTIALS) ? g_creds_list : g_oath_list;
    if (list == NULL) {
        return;
    }
    lv_obj_clean(list);
    if (is_busy()) {
        lv_list_add_text(list, "Device busy...");
        return;
    }
    static ui_list_entry_t entries[UI_MAX_LIST];
    int n = (which == SCREEN_CREDENTIALS)
            ? fido_ui_list_credentials(entries, UI_MAX_LIST)
            : fido_ui_list_oath(entries, UI_MAX_LIST);
    if (n <= 0) {
        lv_list_add_text(list, which == SCREEN_CREDENTIALS
                         ? "No credentials" : "No accounts");
        return;
    }
    for (int i = 0; i < n; i++) {
        char buf[UI_LIST_TEXT_LEN * 2 + 4];
        if (entries[i].line2[0]) {
            snprintf(buf, sizeof(buf), "%s\n%s", entries[i].line1,
                     entries[i].line2);
        } else {
            snprintf(buf, sizeof(buf), "%s", entries[i].line1);
        }
        lv_list_add_button(list, NULL, buf);
    }
}

/* ----- Dynamic updates -------------------------------------------------- */
static void update_status(void) {
    if (lbl_state) {
        lv_label_set_text(lbl_state, g_mounted ? "Connected" : "Ready");
        lv_obj_set_style_text_color(lbl_state,
                                    g_mounted ? col_green() : col_yellow(), 0);
    }
    if (lbl_batt) {
        int pct = battery_read_percent();
        if (pct >= 0) {
            lv_label_set_text_fmt(lbl_batt, "Battery %d%%", pct);
        } else {
            lv_label_set_text(lbl_batt, "Battery --");
        }
    }
}

static void apply_context(void) {
    if (lbl_rp) {
        lv_label_set_text(lbl_rp, g_ctx_rp[0] ? g_ctx_rp : "User presence");
    }
    if (lbl_user) {
        lv_label_set_text(lbl_user, g_ctx_user);
    }
}

/* ----- Signal handlers (run on core0 from button_wait / usb_task) ------- */
static int on_presence_request(signal_code_t code, void *data) {
    (void)code;
    signal_user_presence_request_data_t *d =
        (signal_user_presence_request_data_t *)data;
    g_confirm_timeout_s = d ? d->timeout : 30;
    g_confirm_start_ms = board_millis();
    g_confirm_last_remaining = -1;
    apply_context();
    show_screen(SCREEN_CONFIRM);
    return 0;
}

static int on_presence_end(signal_code_t code, void *data) {
    (void)code;
    (void)data;
    g_ctx_rp[0] = g_ctx_user[0] = '\0';
    show_screen(SCREEN_STATUS);
    return 0;
}

static int on_usb_mounted(signal_code_t code, void *data) {
    (void)code;
    (void)data;
    g_mounted = true;
    update_status();
    return 0;
}

/* ----- Public API + SDK hooks ------------------------------------------- */
/* Called from the FIDO command handlers, which run on core 1. Only copy the
 * strings here - the labels are pushed to LVGL from on_presence_request(),
 * which runs on core 0 after the presence request crosses the core queue, so
 * LVGL is never touched from core 1. Both fields are always set (cleared when
 * NULL) so the prompt reflects only the current operation. */
void display_ui_set_context(const char *rp_id, const char *user_name) {
    if (rp_id) {
        strncpy(g_ctx_rp, rp_id, sizeof(g_ctx_rp) - 1);
        g_ctx_rp[sizeof(g_ctx_rp) - 1] = '\0';
    } else {
        g_ctx_rp[0] = '\0';
    }
    if (user_name) {
        strncpy(g_ctx_user, user_name, sizeof(g_ctx_user) - 1);
        g_ctx_user[sizeof(g_ctx_user) - 1] = '\0';
    } else {
        g_ctx_user[0] = '\0';
    }
}

void platform_ui_init(void) {
    st7789_init();
    cst816_init();
    battery_init();

    lv_init();
    lv_tick_set_cb(tick_cb);

    g_disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    lv_display_set_flush_cb(g_disp, disp_flush);
    lv_display_set_buffers(g_disp, g_draw_buf, NULL, sizeof(g_draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    g_indev = lv_indev_create();
    lv_indev_set_type(g_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(g_indev, touch_read);

    build_ui();
    update_status();
    show_screen(SCREEN_STATUS);

    signal_add(SIGNAL_USB_MOUNTED, SIGNAL_FLAG_NONE, on_usb_mounted);
    signal_add(SIGNAL_USER_PRESENCE_REQUEST, SIGNAL_FLAG_NONE,
               on_presence_request);
    signal_add(SIGNAL_USER_PRESENCE_COMPLETED, SIGNAL_FLAG_NONE,
               on_presence_end);
    signal_add(SIGNAL_USER_PRESENCE_CANCELLED, SIGNAL_FLAG_NONE,
               on_presence_end);
    signal_add(SIGNAL_USER_PRESENCE_TIMEOUT, SIGNAL_FLAG_NONE,
               on_presence_end);
}

void platform_ui_task(void) {
    lv_timer_handler();

    uint32_t now = board_millis();

    if (g_screen == SCREEN_CONFIRM) {
        uint32_t elapsed = (now - g_confirm_start_ms) / 1000;
        int remaining = (int)g_confirm_timeout_s - (int)elapsed;
        if (remaining < 0) {
            remaining = 0;
        }
        if (remaining != g_confirm_last_remaining) {
            g_confirm_last_remaining = remaining;
            if (lbl_count) {
                lv_label_set_text_fmt(lbl_count, "%ds", remaining);
            }
        }
    } else if (g_screen == SCREEN_STATUS && now >= g_next_status_ms) {
        update_status();
        g_next_status_ms = now + 1000;
    }
}

/* Override the SDK's WEAK picokey_init() to bring up the touch UI at boot. */
int picokey_init(void) {
    platform_ui_init();
    return 0;
}

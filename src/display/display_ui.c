/*
 * This file is part of the Pico FIDO Touch distribution.
 *
 * ESP32-S3 LVGL touchscreen UI for the Waveshare 2" Capacitive Touch LCD
 * (ST7789T3 over SPI + CST816 over I2C). See display_ui.h.
 *
 * Screens:
 *   A) SCREEN_CONFIRM  - Approve/Deny prompt for FIDO user presence.
 *   B) SCREEN_STATUS   - idle status (default screen).
 *   C) SCREEN_MENU + sub-screens - management UI: resident-credential and
 *      OATH-account lists (read-only) plus device info.
 *
 * Concurrency: esp_lvgl_port runs LVGL in its own FreeRTOS task and owns a
 * recursive mutex. LVGL objects are only ever touched (a) inside LVGL event
 * callbacks - which the port already runs under that mutex - or (b) from the
 * SDK's signal handlers and platform_ui_task(), which run on the FIDO / core0
 * tasks and therefore take the mutex explicitly via ui_lock()/ui_unlock().
 */

#include "display_ui.h"

#include <string.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

#include "lvgl.h"

#include "picokeys.h"
#include "signal.h"
#include "button.h"
#include "pico_time.h"
#include "fido.h"

/* ----- Waveshare 2" Capacitive Touch LCD wiring (ESP32-S3 reference) ------ */
#define LCD_HOST            SPI2_HOST
#define PIN_LCD_MOSI        2
#define PIN_LCD_SCLK        1
#define PIN_LCD_MISO        (-1)
#define PIN_LCD_CS          39
#define PIN_LCD_DC          41
#define PIN_LCD_RST         40
#define PIN_LCD_BL          6

#define TOUCH_I2C_PORT      I2C_NUM_0
#define PIN_TP_SDA          15
#define PIN_TP_SCL          7
#define PIN_TP_INT          17
#define PIN_TP_RST          16

#define LCD_WIDTH           240
#define LCD_HEIGHT          320
#define LCD_PIXEL_CLOCK_HZ  (40 * 1000 * 1000)
#define LCD_CMD_BITS        8
#define LCD_PARAM_BITS      8
#define LCD_BUF_LINES       80

static const char *TAG = "display_ui";

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

/* ----- LVGL / esp_lcd handles ------------------------------------------- */
static lv_display_t *g_disp = NULL;
static esp_lcd_panel_handle_t g_panel = NULL;
static esp_lcd_panel_io_handle_t g_panel_io = NULL;
static esp_lcd_touch_handle_t g_touch = NULL;

static lv_obj_t *g_screens[SCREEN_COUNT] = {0};
static lv_obj_t *g_creds_list = NULL; /* management: resident credentials */
static lv_obj_t *g_oath_list = NULL;  /* management: OATH accounts         */
#define UI_MAX_LIST 24
static lv_obj_t *lbl_state = NULL;   /* status: Ready / Connected */
static lv_obj_t *lbl_rp = NULL;      /* confirm: relying party     */
static lv_obj_t *lbl_user = NULL;    /* confirm: user name         */
static lv_obj_t *lbl_count = NULL;   /* confirm: countdown         */

/* ----- LVGL locking ----------------------------------------------------- */
/* Serialises LVGL access from tasks other than the esp_lvgl_port task. The
 * port mutex is recursive, so nesting (e.g. from an event callback) is safe. */
static inline bool ui_lock(void) { return lvgl_port_lock(0); }
static inline void ui_unlock(void) { lvgl_port_unlock(); }

/* ----- Colours ---------------------------------------------------------- */
static inline lv_color_t col_white(void) { return lv_color_white(); }
static inline lv_color_t col_grey(void)  { return lv_color_make(0x9a, 0xa0, 0xac); }
static inline lv_color_t col_green(void) { return lv_color_make(0x1f, 0xa8, 0x55); }
static inline lv_color_t col_red(void)   { return lv_color_make(0xe0, 0x1b, 0x24); }
static inline lv_color_t col_yellow(void){ return lv_color_make(0xf2, 0xc0, 0x1e); }

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
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *sub = make_label(s, "touch key", &lv_font_montserrat_14,
                               col_grey());
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 62);

    lbl_state = make_label(s, "Ready", &lv_font_montserrat_28, col_yellow());
    lv_obj_align(lbl_state, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *hint = make_label(s, "tap for menu", &lv_font_montserrat_14,
                                col_grey());
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -18);

    lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s, nav_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)SCREEN_MENU);
}

static void build_confirm_screen(void) {
    lv_obj_t *s = make_screen();
    g_screens[SCREEN_CONFIRM] = s;

    lv_obj_t *title = make_label(s, "CONFIRM", &lv_font_montserrat_28,
                                 col_yellow());
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    lbl_rp = make_label(s, "User presence", &lv_font_montserrat_20,
                        col_white());
    lv_obj_set_width(lbl_rp, LCD_WIDTH - 20);
    lv_obj_set_style_text_align(lbl_rp, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lbl_rp, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl_rp, LV_ALIGN_TOP_MID, 0, 70);

    lbl_user = make_label(s, "", &lv_font_montserrat_16, col_grey());
    lv_obj_set_width(lbl_user, LCD_WIDTH - 20);
    lv_obj_set_style_text_align(lbl_user, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lbl_user, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl_user, LV_ALIGN_TOP_MID, 0, 104);

    lbl_count = make_label(s, "", &lv_font_montserrat_20, col_grey());
    lv_obj_align(lbl_count, LV_ALIGN_TOP_MID, 0, 138);

    lv_obj_t *btn_ok = lv_button_create(s);
    lv_obj_set_size(btn_ok, LCD_WIDTH - 40, 58);
    lv_obj_align(btn_ok, LV_ALIGN_BOTTOM_MID, 0, -74);
    lv_obj_set_style_bg_color(btn_ok, col_green(), 0);
    lv_obj_t *l_ok = make_label(btn_ok, "APPROVE", &lv_font_montserrat_20,
                                col_white());
    lv_obj_center(l_ok);
    lv_obj_add_event_cb(btn_ok, approve_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_no = lv_button_create(s);
    lv_obj_set_size(btn_no, LCD_WIDTH - 40, 58);
    lv_obj_align(btn_no, LV_ALIGN_BOTTOM_MID, 0, -8);
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
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *list = lv_list_create(s);
    lv_obj_set_size(list, LCD_WIDTH, LCD_HEIGHT - 48);
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
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 18);

    if (line1) {
        lv_obj_t *l1 = make_label(s, line1, &lv_font_montserrat_16,
                                  col_grey());
        lv_obj_align(l1, LV_ALIGN_CENTER, 0, -14);
    }
    if (line2) {
        lv_obj_t *l2 = make_label(s, line2, &lv_font_montserrat_16,
                                  col_grey());
        lv_obj_align(l2, LV_ALIGN_CENTER, 0, 16);
    }

    lv_obj_t *back = lv_button_create(s);
    lv_obj_set_size(back, LCD_WIDTH - 40, 52);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -10);
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
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 8);

    int list_top = 40;
    if (caption) {
        lv_obj_t *c = make_label(s, caption, &lv_font_montserrat_14,
                                 col_grey());
        lv_obj_set_width(c, LCD_WIDTH - 12);
        lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 36);
        list_top = 58;
    }

    lv_obj_t *list = lv_list_create(s);
    lv_obj_set_size(list, LCD_WIDTH, LCD_HEIGHT - list_top - 56);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, list_top);

    lv_obj_t *back = lv_button_create(s);
    lv_obj_set_size(back, LCD_WIDTH - 40, 46);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -8);
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
                      "Pico FIDO Touch", "ESP32-S3 2\" LCD");
}

/* Populate a management list from the read-only FIDO/OATH accessors. The
 * caller holds the LVGL lock; skipped while a transaction is active. */
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
}

static void apply_context(void) {
    if (lbl_rp) {
        lv_label_set_text(lbl_rp, g_ctx_rp[0] ? g_ctx_rp : "User presence");
    }
    if (lbl_user) {
        lv_label_set_text(lbl_user, g_ctx_user);
    }
}

/* ----- Signal handlers (run on the FIDO / core0 tasks) ------------------ */
static int on_presence_request(signal_code_t code, void *data) {
    (void)code;
    signal_user_presence_request_data_t *d =
        (signal_user_presence_request_data_t *)data;
    if (!ui_lock()) {
        return 0;
    }
    g_confirm_timeout_s = d ? d->timeout : 30;
    g_confirm_start_ms = board_millis();
    g_confirm_last_remaining = -1;
    apply_context();
    show_screen(SCREEN_CONFIRM);
    ui_unlock();
    return 0;
}

static int on_presence_end(signal_code_t code, void *data) {
    (void)code;
    (void)data;
    g_ctx_rp[0] = g_ctx_user[0] = '\0';
    if (!ui_lock()) {
        return 0;
    }
    show_screen(SCREEN_STATUS);
    ui_unlock();
    return 0;
}

static int on_usb_mounted(signal_code_t code, void *data) {
    (void)code;
    (void)data;
    g_mounted = true;
    if (!ui_lock()) {
        return 0;
    }
    update_status();
    ui_unlock();
    return 0;
}

/* ----- Public API + SDK hooks ------------------------------------------- */
/* Called from the FIDO command handlers before user presence. Only copies the
 * strings; the labels are pushed to LVGL from on_presence_request(), which
 * takes the LVGL lock. Both fields are always set (cleared when NULL) so the
 * prompt reflects only the current operation. */
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

/* ----- Hardware bring-up ------------------------------------------------ */
static void backlight_on(void) {
    gpio_config_t bk = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_LCD_BL,
    };
    gpio_config(&bk);
    gpio_set_level(PIN_LCD_BL, 1);
}

static void panel_init(void) {
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_BUF_LINES * sizeof(uint16_t) + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &g_panel_io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(g_panel_io, &panel_cfg, &g_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(g_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(g_panel));
    /* ST7789 panels render inverted by default. */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(g_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(g_panel, true));
}

static void touch_init(void) {
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = PIN_TP_SDA,
        .scl_io_num = PIN_TP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_cfg, &tp_io));

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_WIDTH,
        .y_max = LCD_HEIGHT,
        .rst_gpio_num = PIN_TP_RST,
        .int_gpio_num = PIN_TP_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &g_touch));
}

void platform_ui_init(void) {
    backlight_on();
    panel_init();
    touch_init();

    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = g_panel_io,
        .panel_handle = g_panel,
        .buffer_size = LCD_WIDTH * LCD_BUF_LINES,
        .double_buffer = true,
        .hres = LCD_WIDTH,
        .vres = LCD_HEIGHT,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,
        },
    };
    g_disp = lvgl_port_add_disp(&disp_cfg);

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = g_disp,
        .handle = g_touch,
    };
    lvgl_port_add_touch(&touch_cfg);

    if (ui_lock()) {
        build_ui();
        update_status();
        show_screen(SCREEN_STATUS);
        ui_unlock();
    }

    signal_add(SIGNAL_USB_MOUNTED, SIGNAL_FLAG_NONE, on_usb_mounted);
    signal_add(SIGNAL_USER_PRESENCE_REQUEST, SIGNAL_FLAG_NONE,
               on_presence_request);
    signal_add(SIGNAL_USER_PRESENCE_COMPLETED, SIGNAL_FLAG_NONE,
               on_presence_end);
    signal_add(SIGNAL_USER_PRESENCE_CANCELLED, SIGNAL_FLAG_NONE,
               on_presence_end);
    signal_add(SIGNAL_USER_PRESENCE_TIMEOUT, SIGNAL_FLAG_NONE,
               on_presence_end);

    ESP_LOGI(TAG, "touch UI ready (%dx%d)", LCD_WIDTH, LCD_HEIGHT);
}

/* Called each iteration from the SDK core0 loop. esp_lvgl_port drives
 * lv_timer_handler() itself, so here we only refresh the periodic labels
 * (countdown / status) under the LVGL lock. */
void platform_ui_task(void) {
    uint32_t now = board_millis();

    if (g_screen == SCREEN_CONFIRM) {
        uint32_t elapsed = (now - g_confirm_start_ms) / 1000;
        int remaining = (int)g_confirm_timeout_s - (int)elapsed;
        if (remaining < 0) {
            remaining = 0;
        }
        if (remaining != g_confirm_last_remaining) {
            g_confirm_last_remaining = remaining;
            if (ui_lock()) {
                if (lbl_count) {
                    lv_label_set_text_fmt(lbl_count, "%ds", remaining);
                }
                ui_unlock();
            }
        }
    } else if (g_screen == SCREEN_STATUS && now >= g_next_status_ms) {
        if (ui_lock()) {
            update_status();
            ui_unlock();
        }
        g_next_status_ms = now + 1000;
    }
}

/* Override the SDK's WEAK picokey_init() to bring up the touch UI at boot. */
int picokey_init(void) {
    platform_ui_init();
    return 0;
}

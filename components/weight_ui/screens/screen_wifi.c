/**
 * @file screen_wifi.c
 * @brief WiFi setup screen - SSID scan list (left) + QWERTY keyboard for
 *        password entry (right). Tap SSID -> type password -> Connect.
 *
 * Layout (1280 x 800):
 *  +-------------------------------------------------------------+
 *  | ← back   WiFi setup                          [rescan]       |
 *  +--------------------------+----------------------------------+
 *  | SSID list (scrollable):  | Password for "<selected_ssid>":  |
 *  |  > MyNetwork  -42dBm     |  [____________________]          |
 *  |  > OfficeWiFi -58dBm     |                                  |
 *  |  > GuestNet   -67dBm     |  +----- QWERTY KEYBOARD -----+   |
 *  |  > Factory_2G -71dBm     |  |  1 2 3 4 5 6 7 8 9 0       | |
 *  |                          |  |  q w e r t y u i o p       |  |
 *  |                          |  |  a s d f g h j k l         |  |
 *  |                          |  |  ⇧ z x c v b n m  ⌫        |  |
 *  |                          |  |  [123]   space    [connect] | |
 *  +--------------------------+----------------------------------+
 *
 * LVGL's built-in lv_keyboard widget handles the QWERTY layout & input.
 *
 * IMPORTANT - blocking scan:
 *   weight_wifi_scan() can take 2-3 seconds. We release the LVGL lock before
 *   calling it and re-acquire it afterwards so the LVGL task keeps ticking
 *   and the display doesn't freeze. The "Scanning..." placeholder gives the
 *   user visual feedback while the scan runs.
 */

#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "esp_log.h"
#include "esp_event.h"
#include "ui_theme.h"
#include "weight_ui.h"
#include "weight_wifi.h"
#include "weight_config.h"
#include "weight_i18n.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_x.h"

static const char *TAG = "scr_wifi";

#define WIFI_MAX_AP 12

static lv_obj_t *s_ssid_list;
static lv_obj_t *s_pwd_ta;
static lv_obj_t *s_pwd_label;
static lv_obj_t *s_keyboard;
static char s_selected_ssid[33];

/* ---------------------------------------------------------------------------
 * Event handlers
 * --------------------------------------------------------------------------- */
static void on_back(lv_event_t *e)
{
    weight_ui_show(UI_SCREEN_SETTINGS);
}

static void on_connect(lv_event_t *e)
{
    if (s_selected_ssid[0] == '\0')
    {
        ESP_LOGW(TAG, "no SSID selected");
        return;
    }
    const char *pwd = lv_textarea_get_text(s_pwd_ta);
    ESP_LOGI(TAG, "connecting to '%s'...", s_selected_ssid);
    weight_wifi_connect(s_selected_ssid, pwd);
    weight_ui_show(UI_SCREEN_SETTINGS);
}

static void on_keyboard_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY)
        on_connect(e);
    else if (code == LV_EVENT_CANCEL)
        on_back(e);
}

static void on_ssid_picked(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    const char *ssid = (const char *)lv_obj_get_user_data(btn);
    if (!ssid)
        return;

    strncpy(s_selected_ssid, ssid, sizeof(s_selected_ssid) - 1);
    s_selected_ssid[sizeof(s_selected_ssid) - 1] = '\0';

    char buf[80];
    snprintf(buf, sizeof(buf), "%s \"%s\":", TR(wifi_password_for), s_selected_ssid);
    lv_label_set_text(s_pwd_label, buf);
    lv_textarea_set_text(s_pwd_ta, "");
    lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_keyboard, s_pwd_ta);
}

/* ---------------------------------------------------------------------------
 * Scan task
 * --------------------------------------------------------------------------- */
static void scan_task(void *arg)
{
    ESP_LOGI(TAG, "scan_task started");
    wifi_scan_entry_t aps[WIFI_MAX_AP];
    size_t n = weight_wifi_scan(aps, WIFI_MAX_AP);
    ESP_LOGI(TAG, "scan found %d APs", n);

    static char ssid_storage[WIFI_MAX_AP][33];

    bsp_display_lock(portMAX_DELAY);
    lv_obj_clean(s_ssid_list);

    if (n == 0)
    {
        lv_obj_t *empty = lv_label_create(s_ssid_list);
        lv_obj_set_style_text_color(empty, UI_COLOR_TEXT_MUTED, 0);
        lv_obj_set_style_text_font(empty, ui_font_small(), 0);
        lv_label_set_text(empty, TR(wifi_no_networks));
        lv_obj_set_style_pad_all(empty, 20, 0);
        bsp_display_unlock();
        vTaskDelete(NULL);
        return;
    }

    for (size_t i = 0; i < n; i++)
    {
        lv_obj_t *row = lv_btn_create(s_ssid_list);
        lv_obj_set_size(row, LV_PCT(100), 56);
        lv_obj_set_style_bg_color(row, UI_COLOR_PANEL, 0);
        lv_obj_set_style_radius(row, UI_RADIUS_S, 0);
        lv_obj_set_style_pad_hor(row, 14, 0);

        strncpy(ssid_storage[i], aps[i].ssid, sizeof(ssid_storage[i]) - 1);
        ssid_storage[i][sizeof(ssid_storage[i]) - 1] = '\0';
        lv_obj_set_user_data(row, ssid_storage[i]);
        lv_obj_add_event_cb(row, on_ssid_picked, LV_EVENT_CLICKED, NULL);

        lv_obj_t *name = lv_label_create(row);
        lv_obj_set_style_text_color(name, UI_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(name, ui_font_normal(), 0);
        lv_label_set_text(name, aps[i].ssid);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

        char rssi_buf[32];
        snprintf(rssi_buf, sizeof(rssi_buf), "%s %ddBm",
                 aps[i].secure ? LV_SYMBOL_WIFI : "  ", aps[i].rssi);
        lv_obj_t *rssi = lv_label_create(row);
        lv_obj_set_style_text_color(rssi, UI_COLOR_TEXT_MUTED, 0);
        lv_obj_set_style_text_font(rssi, &lv_font_montserrat_14, 0); /* symbol safe */
        lv_label_set_text(rssi, rssi_buf);
        lv_obj_align(rssi, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    bsp_display_unlock();
    vTaskDelete(NULL);
}

static void populate_ssid_list(void)
{
    lv_obj_clean(s_ssid_list);

    lv_obj_t *placeholder = lv_label_create(s_ssid_list);
    lv_obj_set_style_text_color(placeholder, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(placeholder, ui_font_small(), 0);
    lv_label_set_text(placeholder, TR(wifi_scanning));
    lv_obj_set_style_pad_all(placeholder, 20, 0);

    xTaskCreate(scan_task, "wifi_scan", 4096, NULL, 5, NULL);
}

static void on_rescan(lv_event_t *e) { populate_ssid_list(); }

static void on_screen_load(lv_event_t *e)
{
    s_selected_ssid[0] = '\0';
    lv_label_set_text(s_pwd_label, TR(wifi_select_network));
    lv_textarea_set_text(s_pwd_ta, "");
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    populate_ssid_list();
}

/* ---------------------------------------------------------------------------
 * Build screen
 * --------------------------------------------------------------------------- */
lv_obj_t *screen_wifi_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_COLOR_BG, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_screen_load, LV_EVENT_SCREEN_LOADED, NULL);

    /* Top bar */
    lv_obj_t *topbar = lv_obj_create(scr);
    lv_obj_set_size(topbar, UI_W, 60);
    lv_obj_align(topbar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(topbar, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_width(topbar, 0, 0);
    lv_obj_set_style_radius(topbar, 0, 0);
    lv_obj_set_style_pad_hor(topbar, UI_PAD, 0);
    lv_obj_clear_flag(topbar, LV_OBJ_FLAG_SCROLLABLE);

    /* Back button */
    lv_obj_t *back = lv_btn_create(topbar);
    lv_obj_set_size(back, 120, 40);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(back, UI_COLOR_PANEL_ALT, 0);
    lv_obj_set_style_radius(back, UI_RADIUS_S, 0);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_sym = lv_label_create(back);
    lv_obj_set_style_text_font(back_sym, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(back_sym, UI_COLOR_TEXT, 0);
    lv_label_set_text(back_sym, LV_SYMBOL_LEFT);
    lv_obj_align(back_sym, LV_ALIGN_LEFT_MID, 6, 0);

    lv_obj_t *back_txt = lv_label_create(back);
    lv_obj_set_style_text_font(back_txt, ui_font_small(), 0);
    lv_obj_set_style_text_color(back_txt, UI_COLOR_TEXT, 0);
    lv_label_set_text(back_txt, TR(settings_back));
    lv_obj_align_to(back_txt, back_sym, LV_ALIGN_OUT_RIGHT_MID, 4, 0);

    /* Title */
    lv_obj_t *title = lv_label_create(topbar);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(title, ui_font_large(), 0);
    lv_label_set_text(title, TR(wifi_title));
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    /* Rescan button */
    lv_obj_t *rescan = lv_btn_create(topbar);
    lv_obj_set_size(rescan, 140, 40);
    lv_obj_align(rescan, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(rescan, UI_COLOR_PANEL_ALT, 0);
    lv_obj_set_style_radius(rescan, UI_RADIUS_S, 0);
    lv_obj_add_event_cb(rescan, on_rescan, LV_EVENT_CLICKED, NULL);

    lv_obj_t *rescan_sym = lv_label_create(rescan);
    lv_obj_set_style_text_font(rescan_sym, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(rescan_sym, UI_COLOR_TEXT, 0);
    lv_label_set_text(rescan_sym, LV_SYMBOL_REFRESH);
    lv_obj_align(rescan_sym, LV_ALIGN_LEFT_MID, 6, 0);

    lv_obj_t *rescan_txt = lv_label_create(rescan);
    lv_obj_set_style_text_font(rescan_txt, ui_font_small(), 0);
    lv_obj_set_style_text_color(rescan_txt, UI_COLOR_TEXT, 0);
    lv_label_set_text(rescan_txt, TR(wifi_scan));
    lv_obj_align_to(rescan_txt, rescan_sym, LV_ALIGN_OUT_RIGHT_MID, 4, 0);

    /* Left: SSID list */
    const int LEFT_W = 480;
    s_ssid_list = lv_obj_create(scr);
    lv_obj_set_size(s_ssid_list, LEFT_W, UI_H - 60);
    lv_obj_align(s_ssid_list, LV_ALIGN_TOP_LEFT, 0, 60);
    lv_obj_set_style_bg_color(s_ssid_list, UI_COLOR_BG, 0);
    lv_obj_set_style_border_width(s_ssid_list, 0, 0);
    lv_obj_set_style_pad_all(s_ssid_list, UI_PAD, 0);
    lv_obj_set_style_pad_row(s_ssid_list, UI_PAD_S, 0);
    lv_obj_set_flex_flow(s_ssid_list, LV_FLEX_FLOW_COLUMN);

    /* Right: password panel */
    lv_obj_t *right = lv_obj_create(scr);
    lv_obj_set_size(right, UI_W - LEFT_W, UI_H - 60);
    lv_obj_align(right, LV_ALIGN_TOP_RIGHT, 0, 60);
    lv_obj_set_style_bg_color(right, UI_COLOR_BG, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, UI_PAD, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    s_pwd_label = lv_label_create(right);
    lv_obj_set_style_text_color(s_pwd_label, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_pwd_label, ui_font_normal(), 0);
    lv_label_set_text(s_pwd_label, TR(wifi_select_network));
    lv_obj_align(s_pwd_label, LV_ALIGN_TOP_LEFT, 0, 0);

    s_pwd_ta = lv_textarea_create(right);
    lv_obj_set_size(s_pwd_ta, LV_PCT(100), 50);
    lv_obj_align(s_pwd_ta, LV_ALIGN_TOP_LEFT, 0, 40);
    lv_obj_set_style_bg_color(s_pwd_ta, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_color(s_pwd_ta, UI_COLOR_BORDER, 0);
    lv_obj_set_style_text_color(s_pwd_ta, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_pwd_ta, ui_font_small(), 0);
    lv_textarea_set_one_line(s_pwd_ta, true);
    lv_textarea_set_placeholder_text(s_pwd_ta, TR(wifi_no_password));

    /* Connect button */
    lv_obj_t *connect_btn = lv_btn_create(right);
    lv_obj_set_size(connect_btn, 160, 50);
    lv_obj_align(connect_btn, LV_ALIGN_TOP_RIGHT, 0, 100);
    lv_obj_set_style_bg_color(connect_btn, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(connect_btn, UI_RADIUS_S, 0);
    lv_obj_add_event_cb(connect_btn, on_connect, LV_EVENT_CLICKED, NULL);

    lv_obj_t *connect_sym = lv_label_create(connect_btn);
    lv_obj_set_style_text_font(connect_sym, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(connect_sym, UI_COLOR_TEXT, 0);
    lv_label_set_text(connect_sym, LV_SYMBOL_OK);
    lv_obj_align(connect_sym, LV_ALIGN_LEFT_MID, 8, 0);

    lv_obj_t *connect_txt = lv_label_create(connect_btn);
    lv_obj_set_style_text_font(connect_txt, ui_font_normal(), 0);
    lv_obj_set_style_text_color(connect_txt, UI_COLOR_TEXT, 0);
    lv_label_set_text(connect_txt, TR(wifi_connect));
    lv_obj_align_to(connect_txt, connect_sym, LV_ALIGN_OUT_RIGHT_MID, 6, 0);

    /* Keyboard */
    s_keyboard = lv_keyboard_create(right);
    lv_obj_set_size(s_keyboard, LV_PCT(100), UI_H - 60 - 40 - 50 - 30 - 60);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_style_bg_color(s_keyboard, UI_COLOR_PANEL, 0);
    lv_obj_add_event_cb(s_keyboard, on_keyboard_event,
                        LV_EVENT_READY | LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);

    return scr;
}
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
#define WIFI_LEFT_PANEL_W 480
#define WIFI_CUR_CARD_H 100

static volatile bool s_scan_in_progress = false;

static lv_obj_t *s_net_status_lbl;

static lv_obj_t *s_ip_popup = NULL;
static lv_obj_t *s_ip_dhcp_btn = NULL;
static lv_obj_t *s_ip_static_btn = NULL;
static lv_obj_t *s_ip_fields_box = NULL;
static lv_obj_t *s_ip_ta = NULL;
static lv_obj_t *s_gw_ta = NULL;
static lv_obj_t *s_mask_ta = NULL;
static lv_obj_t *s_dns_ta = NULL;
static lv_obj_t *s_ip_kbd = NULL;
static bool s_ip_mode_static = false;
static lv_obj_t *s_cur_ssid_lbl;
static lv_obj_t *s_cur_detail_lbl;
static lv_obj_t *s_cur_btn;
static lv_obj_t *s_cur_btn_lbl;

static lv_obj_t *s_ssid_list;
static lv_obj_t *s_pwd_ta;
static lv_obj_t *s_pwd_label;
static lv_obj_t *s_keyboard;
static lv_obj_t *s_conn_status_lbl;
static lv_timer_t *s_wifi_refresh_timer = NULL;
static char s_selected_ssid[33];

/* forward decls - defined further down, used in on_screen_load() */
static void refresh_net_status(void);
static void refresh_conn_status(void);
static void on_ip_settings_open(lv_event_t *e);

/* ---------------------------------------------------------------------------
 * Event handlers
 * --------------------------------------------------------------------------- */

static void reset_password_panel(void)
{
    s_selected_ssid[0] = '\0';
    lv_textarea_set_text(s_pwd_ta, "");
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_pwd_label, TR(wifi_select_network));
    lv_obj_set_style_text_color(s_pwd_label, UI_COLOR_TEXT, 0);
}

static void on_back(lv_event_t *e)
{
    weight_ui_show(UI_SCREEN_SETTINGS); /* scan no longer drops the link - nothing to restore */
}

static void on_connect(lv_event_t *e)
{
    if (s_selected_ssid[0] == '\0')
        return;
    weight_wifi_connect(s_selected_ssid, lv_textarea_get_text(s_pwd_ta));
    reset_password_panel();
    refresh_conn_status();
}

static void on_keyboard_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY)
        on_connect(e);
    else if (code == LV_EVENT_CANCEL)
        reset_password_panel();
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
    lv_obj_set_style_text_color(s_pwd_label, UI_COLOR_TEXT, 0);
}

static void on_disc_cancel(lv_event_t *e)
{
    lv_msgbox_close_async((lv_obj_t *)lv_event_get_user_data(e));
}

static void on_disc_confirm(lv_event_t *e)
{
    weight_wifi_disconnect();
    lv_msgbox_close_async((lv_obj_t *)lv_event_get_user_data(e));
}

static void on_cur_btn(lv_event_t *e)
{
    weight_wifi_info_t wi;
    weight_wifi_get_info(&wi);

    if (wi.state == WEIGHT_WIFI_DISCONNECTED)
    {
        weight_wifi_reconnect();
        return;
    }

    lv_obj_t *mbox = lv_msgbox_create(NULL); /* modal on top layer */
    lv_obj_set_style_text_font(mbox, ui_font_normal(), 0);
    lv_msgbox_add_text(mbox, TR(wifi_disconnect_confirm));
    lv_obj_t *cancel = lv_msgbox_add_footer_button(mbox, TR(edit_cancel));
    lv_obj_add_event_cb(cancel, on_disc_cancel, LV_EVENT_CLICKED, mbox);
    lv_obj_t *ok = lv_msgbox_add_footer_button(mbox, TR(wifi_disconnect));
    lv_obj_set_style_bg_color(ok, UI_COLOR_HIGH_DARK, 0);
    lv_obj_add_event_cb(ok, on_disc_confirm, LV_EVENT_CLICKED, mbox);
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
        s_scan_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    const weight_config_t *cfg = weight_config_get();

    for (size_t i = 0; i < n; i++)
    {
        lv_obj_t *row = lv_btn_create(s_ssid_list);
        if (!row) /* defensive: lv_btn_create's return was never checked before */
            continue;

        lv_obj_set_size(row, LV_PCT(100), 56);
        lv_obj_set_style_bg_color(row, UI_COLOR_PANEL, 0);
        lv_obj_set_style_radius(row, UI_RADIUS_S, 0);
        lv_obj_set_style_pad_hor(row, 14, 0);

        strncpy(ssid_storage[i], aps[i].ssid, sizeof(ssid_storage[i]) - 1);
        ssid_storage[i][sizeof(ssid_storage[i]) - 1] = '\0';
        lv_obj_set_user_data(row, ssid_storage[i]);
        lv_obj_add_event_cb(row, on_ssid_picked, LV_EVENT_CLICKED, NULL);

        bool is_current = (cfg->wifi_ssid[0] != '\0') &&
                          strcmp(aps[i].ssid, cfg->wifi_ssid) == 0;
        if (is_current)
            lv_obj_set_style_bg_color(row, UI_COLOR_ACCENT, 0);

        lv_obj_t *name = lv_label_create(row);
        lv_obj_set_style_text_color(name, UI_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(name, ui_font_normal(), 0);
        lv_label_set_text(name, aps[i].ssid);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

        char rssi_buf[40];
        snprintf(rssi_buf, sizeof(rssi_buf), "%s %ddBm%s",
                 aps[i].secure ? LV_SYMBOL_WIFI : "  ", aps[i].rssi,
                 is_current ? " " LV_SYMBOL_OK : "");
        lv_obj_t *rssi = lv_label_create(row);
        lv_obj_set_style_text_color(rssi, UI_COLOR_TEXT_MUTED, 0);
        lv_obj_set_style_text_font(rssi, &lv_font_montserrat_14, 0);
        lv_label_set_text(rssi, rssi_buf);
        lv_obj_align(rssi, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    bsp_display_unlock();
    s_scan_in_progress = false;
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

    s_scan_in_progress = true;
    xTaskCreate(scan_task, "wifi_scan", 4096, NULL, 5, NULL);
}

static void refresh_conn_status(void)
{
    weight_wifi_info_t wi;
    weight_wifi_get_info(&wi);

    char detail[96];
    const char *ssid_txt = wi.ssid[0] ? wi.ssid : TR(wifi_not_connected);
    const char *short_txt;
    lv_color_t detail_col = UI_COLOR_TEXT_MUTED;
    bool show_btn = true;
    bool reconnect_btn = false;

    switch (wi.state)
    {
    case WEIGHT_WIFI_CONNECTED:
        snprintf(detail, sizeof(detail), LV_SYMBOL_WIFI " %d dBm    %s    %s",
                 wi.rssi, wi.ip, wi.static_ip ? "Static" : "DHCP");
        detail_col = UI_COLOR_ACCENT;
        short_txt = LV_SYMBOL_OK " Connected";
        break;
    case WEIGHT_WIFI_CONNECTING:
        snprintf(detail, sizeof(detail), "%s    %s", TR(wifi_connecting),
                 wi.static_ip ? "Static" : "DHCP");
        short_txt = "Connecting...";
        break;
    case WEIGHT_WIFI_DISCONNECTED:
        snprintf(detail, sizeof(detail), "%s    %s", TR(wifi_disconnected_manual),
                 wi.static_ip ? "Static" : "DHCP");
        short_txt = "Disconnected";
        reconnect_btn = true;
        break;
    default:
        ssid_txt = TR(wifi_not_connected);
        snprintf(detail, sizeof(detail), "%s", TR(wifi_select_network));
        short_txt = "Disconnected";
        show_btn = false;
        break;
    }

    lv_label_set_text(s_cur_ssid_lbl, ssid_txt);
    lv_label_set_text(s_cur_detail_lbl, detail);
    lv_obj_set_style_text_color(s_cur_detail_lbl, detail_col, 0);

    if (show_btn)
    {
        lv_obj_clear_flag(s_cur_btn, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_cur_btn_lbl, reconnect_btn ? TR(wifi_reconnect) : TR(wifi_disconnect));
        lv_obj_set_style_bg_color(s_cur_btn, reconnect_btn ? UI_COLOR_ACCENT : UI_COLOR_HIGH_DARK, 0);
    }
    else
    {
        lv_obj_add_flag(s_cur_btn, LV_OBJ_FLAG_HIDDEN);
    }

    lv_label_set_text(s_conn_status_lbl, short_txt);
    lv_obj_set_style_text_color(s_conn_status_lbl,
                                wi.state == WEIGHT_WIFI_CONNECTED ? UI_COLOR_ACCENT : UI_COLOR_TEXT_MUTED, 0);

    /* New network rejected -> explain in the password panel (only while the
     * user isn't typing a password for another network) */
    if (wi.new_creds_failed && s_selected_ssid[0] == '\0')
    {
        const char *why = wi.fail == WEIGHT_WIFI_FAIL_AUTH        ? TR(wifi_wrong_password)
                          : wi.fail == WEIGHT_WIFI_FAIL_NOT_FOUND ? TR(wifi_ap_not_found)
                                                                  : TR(wifi_join_error);
        char msg[128];
        snprintf(msg, sizeof(msg), "%s \"%s\": %s", TR(wifi_join_failed), wi.failed_ssid, why);
        lv_label_set_text(s_pwd_label, msg);
        lv_obj_set_style_text_color(s_pwd_label, lv_palette_lighten(LV_PALETTE_RED, 1), 0);
    }
}

static void wifi_refresh_timer_cb(lv_timer_t *t)
{
    refresh_conn_status();
}

static void on_rescan(lv_event_t *e)
{
    if (s_scan_in_progress)
    {
        ESP_LOGI(TAG, "rescan: scan already in progress, ignoring");
        return;
    }
    populate_ssid_list();
}

static void on_screen_load(lv_event_t *e)
{
    s_selected_ssid[0] = '\0';
    lv_label_set_text(s_pwd_label, TR(wifi_select_network));
    lv_textarea_set_text(s_pwd_ta, "");
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    refresh_net_status();
    refresh_conn_status();
    if (!s_wifi_refresh_timer)
        s_wifi_refresh_timer = lv_timer_create(wifi_refresh_timer_cb, 1000, NULL);
    populate_ssid_list();
}

static void on_screen_unload(lv_event_t *e)
{
    if (s_wifi_refresh_timer)
    {
        lv_timer_del(s_wifi_refresh_timer);
        s_wifi_refresh_timer = NULL;
    }
}

void screen_wifi_cleanup(void)
{
    if (s_wifi_refresh_timer)
    {
        lv_timer_del(s_wifi_refresh_timer);
        s_wifi_refresh_timer = NULL;
    }
}

/* ---------------------------------------------------------------------------
 * IP settings (DHCP / Static) popup
 * --------------------------------------------------------------------------- */
static void close_ip_popup(void)
{
    if (s_ip_popup)
    {
        lv_obj_del(s_ip_popup);
        s_ip_popup = NULL;
        s_ip_dhcp_btn = NULL;
        s_ip_static_btn = NULL;
        s_ip_fields_box = NULL;
        s_ip_ta = NULL;
        s_gw_ta = NULL;
        s_mask_ta = NULL;
        s_dns_ta = NULL;
        s_ip_kbd = NULL;
    }
}

static void refresh_net_status(void)
{
    const weight_config_t *cfg = weight_config_get();
    char buf[64];
    if (cfg->wifi_use_static && cfg->wifi_ip[0])
        snprintf(buf, sizeof(buf), "Static: %s", cfg->wifi_ip);
    else
        snprintf(buf, sizeof(buf), "DHCP");
    lv_label_set_text(s_net_status_lbl, buf);
}

static void apply_ip_mode_visuals(void)
{
    lv_obj_set_style_bg_color(s_ip_dhcp_btn,
                              !s_ip_mode_static ? UI_COLOR_ACCENT : UI_COLOR_PANEL_ALT, 0);
    lv_obj_set_style_bg_color(s_ip_static_btn,
                              s_ip_mode_static ? UI_COLOR_ACCENT : UI_COLOR_PANEL_ALT, 0);

    if (s_ip_mode_static)
    {
        lv_obj_clear_flag(s_ip_fields_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_ip_kbd, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(s_ip_kbd, s_ip_ta); /* re-target to a visible field */
    }
    else
    {
        lv_obj_add_flag(s_ip_fields_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ip_kbd, LV_OBJ_FLAG_HIDDEN); /* nothing to type into in DHCP mode */
    }
}

static void on_ip_mode_dhcp(lv_event_t *e)
{
    s_ip_mode_static = false;
    apply_ip_mode_visuals();
}

static void on_ip_mode_static(lv_event_t *e)
{
    s_ip_mode_static = true;
    apply_ip_mode_visuals();
}

static void on_ip_field_focused(lv_event_t *e)
{
    if (!s_ip_kbd)
        return;
    lv_keyboard_set_textarea(s_ip_kbd, lv_event_get_target(e));
    lv_keyboard_set_mode(s_ip_kbd, LV_KEYBOARD_MODE_NUMBER);
}

static void on_ip_cancel(lv_event_t *e) { close_ip_popup(); }

static void on_ip_save(lv_event_t *e)
{
    if (s_ip_mode_static)
    {
        const char *ip = lv_textarea_get_text(s_ip_ta);
        const char *gw = lv_textarea_get_text(s_gw_ta);
        const char *mask = lv_textarea_get_text(s_mask_ta);
        const char *dns = lv_textarea_get_text(s_dns_ta);

        if (!ip[0] || !gw[0] || !mask[0])
        {
            ESP_LOGW(TAG, "static ip: ip/gateway/netmask required");
            return; /* leave popup open so the user can fill it in */
        }
        weight_wifi_set_static_ip(ip, gw, mask, dns);
    }
    else
    {
        weight_wifi_set_static_ip(NULL, NULL, NULL, NULL); /* revert to DHCP */
    }

    close_ip_popup();
    refresh_net_status();
    refresh_conn_status();
}

static lv_obj_t *make_ip_field_row(lv_obj_t *parent, const char *label,
                                   const char *placeholder, const char *prefill,
                                   int32_t y)
{
    if (!parent)
        return NULL;

    lv_obj_t *lbl = lv_label_create(parent);
    if (!lbl)
        return NULL;
    lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(lbl, ui_font_small(), 0);
    lv_label_set_text(lbl, label);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, y);

    lv_obj_t *ta = lv_textarea_create(parent);
    if (!ta)
        return NULL;

    lv_obj_set_size(ta, 280, 44);
    lv_obj_align(ta, LV_ALIGN_TOP_LEFT, 130, y - 6);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, 15);
    lv_textarea_set_placeholder_text(ta, placeholder);
    if (prefill)
        lv_textarea_set_text(ta, prefill);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_16, 0);
    lv_obj_set_style_bg_color(ta, UI_COLOR_PANEL_ALT, 0);
    lv_obj_set_style_border_color(ta, UI_COLOR_ACCENT, 0);
    lv_obj_add_event_cb(ta, on_ip_field_focused, LV_EVENT_CLICKED, NULL);
    return ta;
}

static void on_ip_settings_open(lv_event_t *e)
{
    if (s_ip_popup)
        return;

    ESP_LOGI(TAG, "opening ip settings, free heap: %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));

    const weight_config_t *cfg = weight_config_get();
    s_ip_mode_static = cfg->wifi_use_static;

    lv_obj_t *scr = lv_screen_active();

    s_ip_popup = lv_obj_create(scr);
    if (!s_ip_popup)
    {
        ESP_LOGE(TAG, "ip settings popup: out of memory, aborting");
        return;
    }

    lv_obj_set_size(s_ip_popup, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_ip_popup, 0, 0);
    lv_obj_set_style_bg_opa(s_ip_popup, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(s_ip_popup, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_ip_popup, 0, 0);
    lv_obj_set_style_radius(s_ip_popup, 0, 0);
    lv_obj_clear_flag(s_ip_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_ip_popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_ip_popup);

    lv_obj_t *modal = lv_obj_create(s_ip_popup);
    lv_obj_set_size(modal, 780, 380);
    lv_obj_align(modal, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_bg_color(modal, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, UI_RADIUS_S, 0);
    lv_obj_set_style_pad_all(modal, UI_PAD, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(modal);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(title, ui_font_normal(), 0);
    lv_label_set_text(title, "IP Settings"); /* number/network UI, no TR() */
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    /* DHCP / Static toggle row - same visual pattern as language toggle */
    lv_obj_t *mode_row = lv_obj_create(modal);
    lv_obj_set_size(mode_row, 400, 48);
    lv_obj_align(mode_row, LV_ALIGN_TOP_LEFT, 0, 34);
    lv_obj_set_style_bg_opa(mode_row, 0, 0);
    lv_obj_set_style_border_width(mode_row, 0, 0);
    lv_obj_set_style_pad_all(mode_row, 0, 0);
    lv_obj_set_style_pad_column(mode_row, UI_PAD_S, 0);
    lv_obj_set_flex_flow(mode_row, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(mode_row, LV_OBJ_FLAG_SCROLLABLE);

    s_ip_dhcp_btn = lv_btn_create(mode_row);
    lv_obj_set_size(s_ip_dhcp_btn, 190, 48);
    lv_obj_set_style_radius(s_ip_dhcp_btn, UI_RADIUS_S, 0);
    lv_obj_set_style_border_width(s_ip_dhcp_btn, 0, 0);
    lv_obj_add_event_cb(s_ip_dhcp_btn, on_ip_mode_dhcp, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dhcp_lbl = lv_label_create(s_ip_dhcp_btn);
    lv_label_set_text(dhcp_lbl, "DHCP");
    lv_obj_set_style_text_color(dhcp_lbl, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(dhcp_lbl, ui_font_small(), 0);
    lv_obj_center(dhcp_lbl);

    s_ip_static_btn = lv_btn_create(mode_row);
    lv_obj_set_size(s_ip_static_btn, 190, 48);
    lv_obj_set_style_radius(s_ip_static_btn, UI_RADIUS_S, 0);
    lv_obj_set_style_border_width(s_ip_static_btn, 0, 0);
    lv_obj_add_event_cb(s_ip_static_btn, on_ip_mode_static, LV_EVENT_CLICKED, NULL);
    lv_obj_t *static_lbl = lv_label_create(s_ip_static_btn);
    lv_label_set_text(static_lbl, "Static");
    lv_obj_set_style_text_color(static_lbl, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(static_lbl, ui_font_small(), 0);
    lv_obj_center(static_lbl);

    /* Static fields box - hidden unless Static selected */
    s_ip_fields_box = lv_obj_create(modal);
    if (!s_ip_fields_box)
    {
        ESP_LOGE(TAG, "ip settings fields box: out of memory, aborting");
        close_ip_popup();
        return;
    }
    lv_obj_set_size(s_ip_fields_box, 740, 190);
    lv_obj_align(s_ip_fields_box, LV_ALIGN_TOP_LEFT, 0, 92);
    lv_obj_set_style_bg_opa(s_ip_fields_box, 0, 0);
    lv_obj_set_style_border_width(s_ip_fields_box, 0, 0);
    lv_obj_set_style_pad_all(s_ip_fields_box, 0, 0);
    lv_obj_clear_flag(s_ip_fields_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_ip_fields_box, LV_OBJ_FLAG_CLICKABLE);

    s_ip_ta = make_ip_field_row(s_ip_fields_box, "IP Address", "",
                                cfg->wifi_ip[0] ? cfg->wifi_ip : NULL, 10);
    s_gw_ta = make_ip_field_row(s_ip_fields_box, "Gateway", "",
                                cfg->wifi_gw[0] ? cfg->wifi_gw : NULL, 60);
    s_mask_ta = make_ip_field_row(s_ip_fields_box, "Netmask", "",
                                  cfg->wifi_netmask[0] ? cfg->wifi_netmask : NULL, 110);
    s_dns_ta = make_ip_field_row(s_ip_fields_box, "DNS (optional)", "",
                                 cfg->wifi_dns[0] ? cfg->wifi_dns : NULL, 160);

    lv_obj_t *cancel_btn = lv_btn_create(modal);
    lv_obj_set_size(cancel_btn, 160, 48);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(cancel_btn, UI_COLOR_PANEL_ALT, 0);
    lv_obj_set_style_radius(cancel_btn, UI_RADIUS_S, 0);
    lv_obj_set_style_border_width(cancel_btn, 0, 0);
    lv_obj_add_event_cb(cancel_btn, on_ip_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cancel_btn);
    lv_label_set_text(cl, TR(edit_cancel));
    lv_obj_set_style_text_color(cl, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(cl, ui_font_small(), 0);
    lv_obj_center(cl);

    lv_obj_t *save_btn = lv_btn_create(modal);
    lv_obj_set_size(save_btn, 160, 48);
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(save_btn, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(save_btn, UI_RADIUS_S, 0);
    lv_obj_set_style_border_width(save_btn, 0, 0);
    lv_obj_add_event_cb(save_btn, on_ip_save, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(save_btn);
    lv_label_set_text(sl, TR(edit_save));
    lv_obj_set_style_text_color(sl, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(sl, ui_font_small(), 0);
    lv_obj_center(sl);

    s_ip_kbd = lv_keyboard_create(s_ip_popup);
    lv_obj_set_size(s_ip_kbd, LV_PCT(100), 320);
    lv_obj_align(s_ip_kbd, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(s_ip_kbd, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(s_ip_kbd, s_ip_ta);
    lv_obj_add_event_cb(s_ip_kbd, on_ip_cancel, LV_EVENT_CANCEL, NULL);

    apply_ip_mode_visuals();
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
    lv_obj_add_event_cb(scr, on_screen_unload, LV_EVENT_SCREEN_UNLOADED, NULL);

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

    /* IP settings button - left of rescan (topbar exists here, fine) */
    /* Left top: current network card */
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, WIFI_LEFT_PANEL_W - 2 * UI_PAD, WIFI_CUR_CARD_H);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, UI_PAD, 60 + UI_PAD);
    lv_obj_set_style_bg_color(card, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, UI_RADIUS_S, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cur_title = lv_label_create(card);
    lv_obj_set_style_text_color(cur_title, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(cur_title, ui_font_small(), 0);
    lv_label_set_text(cur_title, TR(wifi_current));
    lv_obj_align(cur_title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_cur_ssid_lbl = lv_label_create(card);
    lv_obj_set_width(s_cur_ssid_lbl, WIFI_LEFT_PANEL_W - 2 * UI_PAD - 24 - 150);
    lv_label_set_long_mode(s_cur_ssid_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_cur_ssid_lbl, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_cur_ssid_lbl, ui_font_normal(), 0);
    lv_obj_align(s_cur_ssid_lbl, LV_ALIGN_TOP_LEFT, 0, 22);

    lv_obj_t *ip_btn2 = lv_btn_create(card);
    lv_obj_set_size(ip_btn2, 100, 52);
    lv_obj_align(ip_btn2, LV_ALIGN_RIGHT_MID, -150, 0);
    lv_obj_set_style_bg_color(ip_btn2, UI_COLOR_PANEL_ALT, 0);
    lv_obj_set_style_radius(ip_btn2, UI_RADIUS_S, 0);
    lv_obj_add_event_cb(ip_btn2, on_ip_settings_open, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ip_lbl2 = lv_label_create(ip_btn2);
    lv_obj_set_style_text_font(ip_lbl2, ui_font_small(), 0);
    lv_obj_set_style_text_color(ip_lbl2, UI_COLOR_TEXT, 0);
    lv_label_set_text(ip_lbl2, "IP");
    lv_obj_center(ip_lbl2);

    s_cur_detail_lbl = lv_label_create(card);
    lv_obj_set_style_text_font(s_cur_detail_lbl, ui_font_small(), 0);
    lv_obj_align(s_cur_detail_lbl, LV_ALIGN_TOP_LEFT, 0, 52);

    s_cur_btn = lv_btn_create(card);
    lv_obj_set_size(s_cur_btn, 140, 52);
    lv_obj_align(s_cur_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(s_cur_btn, UI_RADIUS_S, 0);
    lv_obj_add_event_cb(s_cur_btn, on_cur_btn, LV_EVENT_CLICKED, NULL);
    s_cur_btn_lbl = lv_label_create(s_cur_btn);
    lv_obj_set_style_text_font(s_cur_btn_lbl, ui_font_normal(), 0);
    lv_obj_set_style_text_color(s_cur_btn_lbl, UI_COLOR_TEXT, 0);
    lv_obj_center(s_cur_btn_lbl);

    /* Left: SSID list */
    s_ssid_list = lv_obj_create(scr);
    lv_obj_set_size(s_ssid_list, WIFI_LEFT_PANEL_W, UI_H - 60);
    lv_obj_align(s_ssid_list, LV_ALIGN_TOP_LEFT, 0, 60);
    lv_obj_set_style_bg_color(s_ssid_list, UI_COLOR_BG, 0);
    lv_obj_set_style_border_width(s_ssid_list, 0, 0);
    lv_obj_set_style_pad_all(s_ssid_list, UI_PAD, 0);
    lv_obj_set_style_pad_row(s_ssid_list, UI_PAD_S, 0);
    lv_obj_set_flex_flow(s_ssid_list, LV_FLEX_FLOW_COLUMN);

    /* Right: password panel */
    lv_obj_t *right = lv_obj_create(scr);
    lv_obj_set_size(right, UI_W - WIFI_LEFT_PANEL_W, UI_H - 60);
    lv_obj_align(right, LV_ALIGN_TOP_RIGHT, 0, 60);
    lv_obj_set_style_bg_color(right, UI_COLOR_BG, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, UI_PAD, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    /* status row - flex row guarantees side-by-side layout, avoids
     * align_to ordering issues between two freshly-created auto-sized labels */
    lv_obj_t *status_row = lv_obj_create(right);
    lv_obj_set_size(status_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(status_row, LV_ALIGN_TOP_RIGHT, 0, 4);
    lv_obj_set_style_bg_opa(status_row, 0, 0);
    lv_obj_set_style_border_width(status_row, 0, 0);
    lv_obj_set_style_pad_all(status_row, 0, 0);
    lv_obj_set_style_pad_column(status_row, 10, 0);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(status_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(status_row, LV_OBJ_FLAG_CLICKABLE);

    s_conn_status_lbl = lv_label_create(status_row);
    lv_obj_set_style_text_font(s_conn_status_lbl, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_conn_status_lbl, "Disconnected");

    s_net_status_lbl = lv_label_create(status_row);
    lv_obj_set_style_text_color(s_net_status_lbl, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_net_status_lbl, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_net_status_lbl, "DHCP");

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
/**
 * @file screen_mqtt.c
 * @brief MQTT broker setup - host, port, user, password, TLS toggle, keyboard.
 *
 * Layout (1280 x 800):
 *  +-------------------------------------------------------------+
 *  | ← back   MQTT broker                          [Connect]    |
 *  +------------------------------+------------------------------+
 *  |  Broker host                 |                              |
 *  |  [emqx.example.com_______]   |                              |
 *  |  Port           ☑ TLS         |       QWERTY keyboard        |
 *  |  [8883____]                  |       (binds to last-tapped  |
 *  |  Username                    |        textarea)             |
 *  |  [_______________________]   |                              |
 *  |  Password                    |                              |
 *  |  [•••••••••••____________]   |                              |
 *  +------------------------------+------------------------------+
 *
 * Flipping the TLS switch sets a sensible default port (8883 on, 1883 off)
 * unless the user has manually edited it. Connect saves the config and
 * (re)starts MQTT, then returns to Settings.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "lvgl.h"
#include "esp_log.h"
#include "ui_theme.h"
#include "ui_loading.h"
#include "weight_ui.h"
#include "weight_mqtt.h"
#include "weight_config.h"
#include "weight_i18n.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_x.h"

static const char *TAG = "scr_mqtt";

static lv_obj_t *s_host_ta;
static lv_obj_t *s_port_ta;
static lv_obj_t *s_user_ta;
static lv_obj_t *s_pass_ta;
static lv_obj_t *s_tls_sw;
static lv_obj_t *s_keyboard;
static bool s_port_user_edited;

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------- */
static void bind_keyboard_to(lv_obj_t *ta, lv_keyboard_mode_t mode)
{
    lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_keyboard, ta);
    lv_keyboard_set_mode(s_keyboard, mode);
}

static void on_field_focused(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    bind_keyboard_to(ta, (ta == s_port_ta)
                             ? LV_KEYBOARD_MODE_NUMBER
                             : LV_KEYBOARD_MODE_TEXT_LOWER);
}

static void on_port_value_changed(lv_event_t *e) { s_port_user_edited = true; }

static void on_tls_changed(lv_event_t *e)
{
    if (s_port_user_edited)
        return;
    bool tls_on = lv_obj_has_state(s_tls_sw, LV_STATE_CHECKED);
    lv_textarea_set_text(s_port_ta, tls_on ? "8883" : "1883");
}

/* ---------------------------------------------------------------------------
 * Navigation / connect
 * --------------------------------------------------------------------------- */
static void on_back(lv_event_t *e)
{
    weight_ui_show(UI_SCREEN_SETTINGS);
}

static void mqtt_connect_task(void *arg)
{
    esp_err_t err = weight_mqtt_restart();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "weight_mqtt_restart failed: %s", esp_err_to_name(err));
        ui_loading_set_text(TR(mqtt_connect_failed));
        vTaskDelay(pdMS_TO_TICKS(1500));
        ui_loading_hide();
        vTaskDelete(NULL);
        return;
    }
    for (int i = 0; i < 80; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (weight_mqtt_is_connected())
            break;
    }
    ui_loading_hide();
    vTaskDelete(NULL);
}

static void on_connect(lv_event_t *e)
{
    const char *host = lv_textarea_get_text(s_host_ta);
    const char *port_str = lv_textarea_get_text(s_port_ta);
    const char *user = lv_textarea_get_text(s_user_ta);
    const char *pass = lv_textarea_get_text(s_pass_ta);
    bool tls = lv_obj_has_state(s_tls_sw, LV_STATE_CHECKED);

    if (!host || host[0] == '\0')
    {
        lv_textarea_set_placeholder_text(s_host_ta, TR(mqtt_host_required));
        return;
    }
    long port = strtol(port_str, NULL, 10);
    if (port < 1 || port > 65535)
    {
        lv_textarea_set_placeholder_text(s_port_ta, TR(mqtt_port_invalid));
        return;
    }

    ESP_LOGI(TAG, "saving MQTT: %s:%ld tls=%d user='%s'", host, port, tls, user);
    weight_config_set_mqtt(host, (uint16_t)port, user, pass, tls);
    weight_config_save();
    weight_mqtt_restart();

    ui_loading_show(TR(mqtt_connecting));
    xTaskCreate(mqtt_connect_task, "mqtt_connect", 4096, NULL, 5, NULL);
    weight_ui_show(UI_SCREEN_SETTINGS);
}

static void on_keyboard_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY)
        on_connect(e);
    else if (code == LV_EVENT_CANCEL)
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void on_screen_load(lv_event_t *e)
{
    const weight_config_t *cfg = weight_config_get();
    lv_textarea_set_text(s_host_ta, cfg->mqtt_host);

    char port_buf[8];
    snprintf(port_buf, sizeof(port_buf), "%u",
             cfg->mqtt_port ? (unsigned)cfg->mqtt_port : 8883u);
    lv_textarea_set_text(s_port_ta, port_buf);
    lv_textarea_set_text(s_user_ta, cfg->mqtt_user);
    lv_textarea_set_text(s_pass_ta, cfg->mqtt_pass);

    if (cfg->mqtt_tls)
        lv_obj_add_state(s_tls_sw, LV_STATE_CHECKED);
    else
        lv_obj_clear_state(s_tls_sw, LV_STATE_CHECKED);

    s_port_user_edited = false;
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

/* ---------------------------------------------------------------------------
 * Field helper
 * --------------------------------------------------------------------------- */
static lv_obj_t *make_field(lv_obj_t *parent, const char *title,
                            const char *placeholder, bool password, int width)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(label, ui_font_small(), 0);
    lv_label_set_text(label, title);

    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, width, 50);
    lv_obj_set_style_bg_color(ta, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_color(ta, UI_COLOR_BORDER, 0);
    lv_obj_set_style_text_color(ta, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_18, 0);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    if (password)
        lv_textarea_set_password_mode(ta, true);

    lv_obj_add_event_cb(ta, on_field_focused, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta, on_field_focused, LV_EVENT_CLICKED, NULL);
    return ta;
}

/* ---------------------------------------------------------------------------
 * Build screen
 * --------------------------------------------------------------------------- */
lv_obj_t *screen_mqtt_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_COLOR_BG, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_screen_load, LV_EVENT_SCREEN_LOADED, NULL);

    /* ---- Top bar ---- */
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
    lv_label_set_text(title, TR(mqtt_title));
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    /* Connect button */
    lv_obj_t *connect = lv_btn_create(topbar);
    lv_obj_set_size(connect, 160, 40);
    lv_obj_align(connect, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(connect, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(connect, UI_RADIUS_S, 0);
    lv_obj_add_event_cb(connect, on_connect, LV_EVENT_CLICKED, NULL);

    lv_obj_t *connect_sym = lv_label_create(connect);
    lv_obj_set_style_text_font(connect_sym, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(connect_sym, UI_COLOR_TEXT, 0);
    lv_label_set_text(connect_sym, LV_SYMBOL_OK);
    lv_obj_align(connect_sym, LV_ALIGN_LEFT_MID, 8, 0);

    lv_obj_t *connect_txt = lv_label_create(connect);
    lv_obj_set_style_text_font(connect_txt, ui_font_small(), 0);
    lv_obj_set_style_text_color(connect_txt, UI_COLOR_TEXT, 0);
    lv_label_set_text(connect_txt, TR(mqtt_connect));
    lv_obj_align_to(connect_txt, connect_sym, LV_ALIGN_OUT_RIGHT_MID, 6, 0);

    /* ---- Left: form fields ---- */
    const int LEFT_W = 560;
    lv_obj_t *left = lv_obj_create(scr);
    lv_obj_set_size(left, LEFT_W, UI_H - 60);
    lv_obj_align(left, LV_ALIGN_TOP_LEFT, 0, 60);
    lv_obj_set_style_bg_color(left, UI_COLOR_BG, 0);
    lv_obj_set_style_border_width(left, 0, 0);
    lv_obj_set_style_pad_all(left, UI_PAD, 0);
    lv_obj_set_style_pad_row(left, 10, 0);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    const int FIELD_W = LEFT_W - UI_PAD * 2;

    s_host_ta = make_field(left, TR(mqtt_host),
                           TR(mqtt_host_placeholder), false, FIELD_W);

    /* Port + TLS row */
    lv_obj_t *port_label = lv_label_create(left);
    lv_obj_set_style_text_color(port_label, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(port_label, ui_font_small(), 0);
    lv_label_set_text(port_label, TR(mqtt_port_tls));

    lv_obj_t *port_row = lv_obj_create(left);
    lv_obj_set_size(port_row, FIELD_W, 60);
    lv_obj_set_style_bg_color(port_row, UI_COLOR_BG, 0);
    lv_obj_set_style_border_width(port_row, 0, 0);
    lv_obj_set_style_pad_all(port_row, 0, 0);
    lv_obj_clear_flag(port_row, LV_OBJ_FLAG_SCROLLABLE);

    s_port_ta = lv_textarea_create(port_row);
    lv_obj_set_size(s_port_ta, 160, 50);
    lv_obj_align(s_port_ta, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_port_ta, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_color(s_port_ta, UI_COLOR_BORDER, 0);
    lv_obj_set_style_text_color(s_port_ta, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_port_ta, &lv_font_montserrat_18, 0);
    lv_textarea_set_one_line(s_port_ta, true);
    lv_textarea_set_max_length(s_port_ta, 5);
    lv_textarea_set_accepted_chars(s_port_ta, "0123456789");
    lv_textarea_set_placeholder_text(s_port_ta, "8883");
    lv_obj_add_event_cb(s_port_ta, on_field_focused, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_port_ta, on_field_focused, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_port_ta, on_port_value_changed, LV_EVENT_VALUE_CHANGED, NULL);

    s_tls_sw = lv_switch_create(port_row);
    lv_obj_set_size(s_tls_sw, 70, 36);
    lv_obj_align(s_tls_sw, LV_ALIGN_LEFT_MID, 200, 0);
    lv_obj_add_event_cb(s_tls_sw, on_tls_changed, LV_EVENT_VALUE_CHANGED, NULL);

    s_user_ta = make_field(left, TR(mqtt_user),
                           TR(mqtt_user_placeholder), false, FIELD_W);

    s_pass_ta = make_field(left, TR(mqtt_pass),
                           TR(mqtt_pass_placeholder), true, FIELD_W);

    /* ---- Right: keyboard ---- */
    s_keyboard = lv_keyboard_create(scr);
    lv_obj_set_size(s_keyboard, UI_W - LEFT_W - UI_PAD, UI_H - 60 - UI_PAD * 2);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_RIGHT, -UI_PAD, -UI_PAD);
    lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_style_bg_color(s_keyboard, UI_COLOR_PANEL, 0);
    lv_obj_add_event_cb(s_keyboard, on_keyboard_event,
                        LV_EVENT_READY | LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);

    return scr;
}
#pragma once

#include "lvgl.h"

LV_FONT_DECLARE(lv_font_nato_sans_jp_bold_14);
LV_FONT_DECLARE(lv_font_nato_sans_jp_bold_16);
LV_FONT_DECLARE(lv_font_nato_sans_jp_bold_24);

typedef struct
{
    // --- Common ---
    const char *lang_name;

    // --- screen_main ---
    const char *main_status_idle;
    const char *main_status_idle_model;
    const char *main_status_run;
    const char *main_status_weighing;
    const char *main_status_pass;
    const char *main_status_high;
    const char *main_status_low;
    const char *main_btn_ready;
    const char *main_btn_stop;
    const char *main_btn_models;
    const char *main_btn_setup;
    const char *main_btn_dismiss;
    const char *main_select_model;
    const char *main_model_none;
    const char *main_model_empty;
    const char *main_field_model;
    const char *main_field_upper;
    const char *main_field_std;
    const char *main_field_lower;
    const char *main_alert_high;
    const char *main_alert_low;

    // --- screen_models ---
    const char *models_title_fmt; /* "Models (%u)" format */
    const char *models_col_name;
    const char *models_col_lower;
    const char *models_col_standard;
    const char *models_col_upper;
    const char *models_empty;
    const char *models_new;

    // --- screen_model_edit ---
    const char *edit_title_new;
    const char *edit_title_edit;
    const char *edit_field_name;
    const char *edit_field_lower;
    const char *edit_field_std;
    const char *edit_field_upper;
    const char *edit_delete_btn;
    const char *edit_status_name_empty;
    const char *edit_status_lower;
    const char *edit_status_upper;
    const char *edit_status_all_zero;
    const char *edit_status_ready;
    const char *edit_name_title;
    const char *edit_name_placeholder;
    const char *edit_action_cancel;
    const char *edit_action_clr;
    const char *edit_action_save;
    const char *edit_cancel;
    const char *edit_save;

    /* settings screen */
    const char *settings_title;
    const char *settings_back;
    const char *settings_wifi;
    const char *settings_mqtt;
    const char *settings_unit;
    const char *settings_decimal;
    const char *settings_scale;
    const char *settings_stability;
    const char *settings_audio;
    const char *settings_device;
    const char *settings_tenant;
    const char *settings_about;
    const char *settings_language;
    const char *settings_lang_en;
    const char *settings_lang_ja;
    const char *settings_dec_0;
    const char *settings_dec_1;
    const char *settings_dec_2;
    const char *settings_dec_3;
    const char *settings_stab_3;
    const char *settings_stab_5;
    const char *settings_stab_7;
    const char *settings_stab_10;
    const char *settings_mute;
    const char *settings_unmute;
    const char *settings_tenant_short;
    const char *settings_tenant_uuid;
    const char *status_connected;
    const char *status_disconnected;
    const char *status_not_configured;
    const char *status_not_set;
    const char *status_muted;
    const char *status_unmuted;

    // --- screen_wifi ---
    const char *wifi_title;
    const char *wifi_scan;
    const char *wifi_scanning;
    const char *wifi_no_networks;
    const char *wifi_select_network;
    const char *wifi_password_for;
    const char *wifi_connect;
    const char *wifi_no_password;

    // --- screen_mqtt ---
    const char *mqtt_title;
    const char *mqtt_host;
    const char *mqtt_port;
    const char *mqtt_port_tls;
    const char *mqtt_user;
    const char *mqtt_pass;
    const char *mqtt_connect;
    const char *mqtt_host_placeholder;
    const char *mqtt_user_placeholder;
    const char *mqtt_pass_placeholder;
    const char *mqtt_host_required;
    const char *mqtt_port_invalid;
    const char *mqtt_connecting;
    const char *mqtt_connect_failed;
} weight_lang_t;

// Active language accessor — use this everywhere in screens
const weight_lang_t *weight_i18n_get(void);

// Switch language (call from settings screen)
void weight_i18n_set_lang(int lang_index); // 0 = EN, 1 = JA
void weight_i18n_init(int index);
int weight_i18n_get_lang_index(void);

// Returns the correct font for current language
static inline const lv_font_t *ui_font_small(void)
{
    return (weight_i18n_get_lang_index() == 1)
               ? &lv_font_nato_sans_jp_bold_14
               : &lv_font_montserrat_14;
}

static inline const lv_font_t *ui_font_normal(void)
{
    return (weight_i18n_get_lang_index() == 1)
               ? &lv_font_nato_sans_jp_bold_16
               : &lv_font_montserrat_18;
}

static inline const lv_font_t *ui_font_large(void)
{
    return (weight_i18n_get_lang_index() == 1)
               ? &lv_font_nato_sans_jp_bold_24
               : &lv_font_montserrat_28;
}

// Shorthand macro — just like t() in React
#define TR(key) (weight_i18n_get()->key)

/**
 * Command to check the Kanji char needed
     $text = Get-Content "C:\Users\ITDepartment\esp\esp-weight_hmi_p4\components\weight_ui\i18n\lang_ja.h" -Raw -Encoding UTF8
     $chars = $text.ToCharArray() | Where-Object { [int]$_ -gt 127 } | Sort-Object | Get-Unique
     $symbols = -join $chars
     Write-Output $symbols
 */
/**
 * @file screen_settings.c
 * @brief Settings - 2x4 grid of configuration cards.
 *
 * Changes from v1:
 *  - Display unit card  → dropdown (kg / g / lb / oz)
 *  - Stability card     → two dropdowns: samples (3/5/7/10) + tolerance
 *  - Device card        → tappable; opens an LVGL keyboard popup to edit device ID
 */

#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "ui_theme.h"
#include "ui_loading.h"
#include "weight_ui.h"
#include "weight_config.h"
#include "weight_state.h"
#include "weight_audio.h"
#include "weight_mqtt.h"
#include "weight_state.h"

static const char *TAG = "screen_settings";

/* ---------------------------------------------------------------------------
 * Value label cache (refreshed on screen load + after every change)
 * --------------------------------------------------------------------------- */
typedef struct
{
    lv_obj_t *wifi;
    lv_obj_t *mqtt;
    lv_obj_t *unit;    /* unit dropdown card */
    lv_obj_t *decimal; /* decimal places dropdown card */
    lv_obj_t *scale;
    lv_obj_t *stability;
    lv_obj_t *audio;
    lv_obj_t *device;
    lv_obj_t *about;
    lv_obj_t *tenant;
} settings_labels_t;

static settings_labels_t s_lbls;

/* ---------------------------------------------------------------------------
 * Dropdown popup state (shared; only one open at a time)
 * --------------------------------------------------------------------------- */
typedef enum
{
    DD_NONE = 0,
    DD_UNIT,
    DD_DECIMAL,
    DD_STAB_WINDOW,
} dd_context_t;

/* ---------------------------------------------------------------------------
 * Keyboard popup state (Device ID editor)
 * --------------------------------------------------------------------------- */
static lv_obj_t *s_kbd_popup = NULL; /* modal container */
static lv_obj_t *s_kbd_ta = NULL;    /* text area inside popup */
static lv_obj_t *s_kbd = NULL;       /* LVGL keyboard */

/* Audio popup state */
static lv_obj_t *s_audio_popup = NULL;
static lv_obj_t *s_audio_slider = NULL;
static lv_obj_t *s_audio_val_lbl = NULL;
static lv_obj_t *s_audio_mute_btn = NULL;
static lv_obj_t *s_audio_mute_lbl = NULL;

/* Tenant popup state  */
static lv_obj_t *s_tenant_popup = NULL;
static lv_obj_t *s_tenant_id_ta = NULL;
static lv_obj_t *s_tenant_short_ta = NULL;
static lv_obj_t *s_tenant_kbd = NULL;

/* Dropdown popup state */
static lv_obj_t *s_dd_popup = NULL;
static dd_context_t s_dd_ctx = DD_NONE;

/* ============================================================================
 * Refresh all value labels
 * ========================================================================== */
static void refresh_values(void)
{
    const weight_config_t *cfg = weight_config_get();
    weight_state_snapshot_t snap;
    weight_state_get_snapshot(&snap);

    char buf[128];

    /* WiFi */
    if (cfg->wifi_ssid[0])
        snprintf(buf, sizeof(buf), "%s | %s", cfg->wifi_ssid,
                 snap.wifi_connected ? "connected" : "disconnected");
    else
        snprintf(buf, sizeof(buf), "not configured");
    lv_label_set_text(s_lbls.wifi, buf);

    /* Tenant */
    snprintf(buf, sizeof(buf), "%s | %s", cfg->tenant_short, cfg->tenant_id);
    lv_label_set_text(s_lbls.tenant, buf);

    /* MQTT */
    if (cfg->mqtt_host[0])
        snprintf(buf, sizeof(buf), "%s:%u | %s",
                 cfg->mqtt_host, (unsigned)cfg->mqtt_port,
                 snap.mqtt_connected ? "connected" : "disconnected");
    else
        snprintf(buf, sizeof(buf), "not configured");
    lv_label_set_text(s_lbls.mqtt, buf);

    /* Unit */
    lv_label_set_text(s_lbls.unit, weight_config_unit_str(cfg->unit));

    /* Decimal */
    snprintf(buf, sizeof(buf), "%u decimal%s",
             (unsigned)cfg->decimal_places,
             cfg->decimal_places == 1 ? "" : "s");
    lv_label_set_text(s_lbls.decimal, buf);

    /* Scale */
    snprintf(buf, sizeof(buf), "baud %lu | zero %.*f",
             (unsigned long)cfg->uart_baud,
             cfg->decimal_places, cfg->zero_threshold);
    lv_label_set_text(s_lbls.scale, buf);

    /* Stability */
    snprintf(buf, sizeof(buf), "%u samples | +-%.*f%s",
             (unsigned)cfg->stability_window,
             cfg->decimal_places, cfg->stability_tolerance,
             weight_config_unit_str(cfg->unit));
    lv_label_set_text(s_lbls.stability, buf);

    /* Audio */
    snprintf(buf, sizeof(buf), "vol %u | %s",
             (unsigned)cfg->audio_volume,
             cfg->audio_muted ? "muted" : "unmuted");
    lv_label_set_text(s_lbls.audio, buf);

    /* Device — read from config, not state snapshot */
    lv_label_set_text(s_lbls.device, cfg->device_id[0] ? cfg->device_id : "(not set)");

    /* About */
    uint64_t up_us = esp_timer_get_time();
    unsigned up_sec = (unsigned)(up_us / 1000000ULL);
    unsigned hours = up_sec / 3600;
    unsigned mins = (up_sec % 3600) / 60;
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    snprintf(buf, sizeof(buf), "heap %ukB | up %02u:%02u",
             (unsigned)(free_heap / 1024), hours, mins);
    lv_label_set_text(s_lbls.about, buf);
}

static void on_screen_load(lv_event_t *e)
{
    refresh_values();
}

/* ============================================================================
 * Generic popup close helper
 * ========================================================================== */
static void close_tenant_popup(void)
{
    if (s_tenant_popup)
    {
        lv_obj_del(s_tenant_popup);
        s_tenant_popup = NULL;
        s_tenant_id_ta = NULL;
        s_tenant_short_ta = NULL;
        s_tenant_kbd = NULL;
    }
}

/* keyboard focus helper for tenant fields */
static void on_field_focused_tenant(lv_event_t *e)
{
    if (!s_tenant_kbd)
        return;
    lv_keyboard_set_textarea(s_tenant_kbd, lv_event_get_target(e));
}

static void close_dd_popup(void)
{
    if (s_dd_popup)
    {
        lv_obj_del(s_dd_popup);
        s_dd_popup = NULL;
        s_dd_ctx = DD_NONE;
    }
}

static void close_kbd_popup(void)
{
    if (s_kbd_popup)
    {
        lv_obj_del(s_kbd_popup);
        s_kbd_popup = NULL;
        s_kbd_ta = NULL;
        s_kbd = NULL;
    }
}

static void close_audio_popup(void)
{
    if (s_audio_popup)
    {
        lv_obj_del(s_audio_popup);
        s_audio_popup = NULL;
        s_audio_slider = NULL;
        s_audio_val_lbl = NULL;
        s_audio_mute_btn = NULL;
        s_audio_mute_lbl = NULL;
    }
}

/* ============================================================================
 * Navigation
 * ========================================================================== */
static void on_back(lv_event_t *e)
{
    close_dd_popup();
    close_kbd_popup();
    close_audio_popup();
    close_tenant_popup();
    weight_ui_show(UI_SCREEN_MAIN);
}

static void on_wifi_card(lv_event_t *e)
{
    weight_ui_show(UI_SCREEN_WIFI);
    refresh_values();
}

static void on_mqtt_card(lv_event_t *e)
{
    weight_ui_show(UI_SCREEN_MQTT);
    refresh_values();
}

/* Slider value changed — apply volume live so operator hears it */
static void on_audio_slider(lv_event_t *e)
{
    if (!s_audio_slider)
        return;
    int32_t v = lv_slider_get_value(s_audio_slider);

    char buf[16];
    snprintf(buf, sizeof(buf), "%ld%%", (long)v);
    lv_label_set_text(s_audio_val_lbl, buf);

    /* Save and apply via weight_audio helper */
    weight_audio_set_volume((uint8_t)v);
}

/* Play a test sound when slider is released so user hears new level */
static void on_audio_slider_released(lv_event_t *e)
{
    weight_audio_play(WEIGHT_AUDIO_CLICK);
}

/* Toggle mute button */
static void on_audio_mute_toggle(lv_event_t *e)
{
    const weight_config_t *cfg = weight_config_get();
    bool new_muted = !cfg->audio_muted;
    weight_audio_set_muted(new_muted);

    lv_label_set_text(s_audio_mute_lbl, new_muted ? "Unmute" : "Mute");
    lv_obj_set_style_bg_color(s_audio_mute_btn,
                              new_muted ? UI_COLOR_HIGH_DARK : UI_COLOR_PANEL_ALT, 0);
}

/* Done button */
static void on_audio_done(lv_event_t *e)
{
    close_audio_popup();
    refresh_values();
}

/* Overlay click → close */
static void on_audio_overlay_click(lv_event_t *e)
{
    close_audio_popup();
    refresh_values();
}

static void on_audio_card(lv_event_t *e)
{
    close_dd_popup();
    close_kbd_popup();
    if (s_audio_popup)
        return;

    lv_obj_t *scr = lv_screen_active();
    const weight_config_t *cfg = weight_config_get();

    /* Overlay */
    s_audio_popup = lv_obj_create(scr);
    lv_obj_set_size(s_audio_popup, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_audio_popup, 0, 0);
    lv_obj_set_style_bg_opa(s_audio_popup, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(s_audio_popup, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_audio_popup, 0, 0);
    lv_obj_set_style_radius(s_audio_popup, 0, 0);
    lv_obj_clear_flag(s_audio_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_audio_popup, on_audio_overlay_click, LV_EVENT_CLICKED, NULL);
    lv_obj_move_foreground(s_audio_popup);

    /* Modal card */
    lv_obj_t *modal = lv_obj_create(s_audio_popup);
    lv_obj_set_size(modal, 600, 280);
    lv_obj_center(modal);
    lv_obj_set_style_bg_color(modal, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, UI_RADIUS_S, 0);
    lv_obj_set_style_pad_all(modal, UI_PAD, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_CLICKABLE); /* don't swallow overlay click */

    /* Title */
    lv_obj_t *title = lv_label_create(modal);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_label_set_text(title, "Audio Volume");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    /* Volume value label (right-aligned, big) */
    s_audio_val_lbl = lv_label_create(modal);
    lv_obj_set_style_text_color(s_audio_val_lbl, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(s_audio_val_lbl, &lv_font_montserrat_28, 0);
    char vbuf[16];
    snprintf(vbuf, sizeof(vbuf), "%u%%", (unsigned)cfg->audio_volume);
    lv_label_set_text(s_audio_val_lbl, vbuf);
    lv_obj_align(s_audio_val_lbl, LV_ALIGN_TOP_RIGHT, 0, 0);

    /* Slider */
    s_audio_slider = lv_slider_create(modal);
    lv_obj_set_size(s_audio_slider, 560, 24);
    lv_obj_align(s_audio_slider, LV_ALIGN_TOP_LEFT, 0, 70);
    lv_slider_set_range(s_audio_slider, 0, 100);
    lv_slider_set_value(s_audio_slider, cfg->audio_volume, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_audio_slider, UI_COLOR_PANEL_ALT, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_audio_slider, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_audio_slider, UI_COLOR_ACCENT, LV_PART_KNOB);
    lv_obj_add_event_cb(s_audio_slider, on_audio_slider, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_audio_slider, on_audio_slider_released, LV_EVENT_RELEASED, NULL);

    /* Tick marks below slider */
    lv_obj_t *tick_row = lv_obj_create(modal);
    lv_obj_set_size(tick_row, 560, 20);
    lv_obj_align(tick_row, LV_ALIGN_TOP_LEFT, 0, 100);
    lv_obj_set_style_bg_opa(tick_row, 0, 0);
    lv_obj_set_style_border_width(tick_row, 0, 0);
    lv_obj_set_style_pad_all(tick_row, 0, 0);
    lv_obj_clear_flag(tick_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tick_row, LV_OBJ_FLAG_CLICKABLE);

    const char *tick_labels[] = {"0", "25", "50", "75", "100"};
    for (int i = 0; i < 5; i++)
    {
        lv_obj_t *t = lv_label_create(tick_row);
        lv_obj_set_style_text_color(t, UI_COLOR_TEXT_MUTED, 0);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
        lv_label_set_text(t, tick_labels[i]);
        lv_obj_set_pos(t, (560 - 30) * i / 4, 0);
    }

    /* Mute toggle button */
    s_audio_mute_btn = lv_btn_create(modal);
    lv_obj_set_size(s_audio_mute_btn, 200, 56);
    lv_obj_align(s_audio_mute_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_audio_mute_btn,
                              cfg->audio_muted ? UI_COLOR_HIGH_DARK : UI_COLOR_PANEL_ALT, 0);
    lv_obj_set_style_radius(s_audio_mute_btn, UI_RADIUS_S, 0);
    lv_obj_set_style_border_width(s_audio_mute_btn, 0, 0);
    lv_obj_add_event_cb(s_audio_mute_btn, on_audio_mute_toggle, LV_EVENT_CLICKED, NULL);
    s_audio_mute_lbl = lv_label_create(s_audio_mute_btn);
    lv_label_set_text(s_audio_mute_lbl, cfg->audio_muted ? "Unmute" : "Mute");
    lv_obj_set_style_text_color(s_audio_mute_lbl, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_audio_mute_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(s_audio_mute_lbl);

    /* Done button */
    lv_obj_t *done_btn = lv_btn_create(modal);
    lv_obj_set_size(done_btn, 200, 56);
    lv_obj_align(done_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(done_btn, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(done_btn, UI_RADIUS_S, 0);
    lv_obj_set_style_border_width(done_btn, 0, 0);
    lv_obj_add_event_cb(done_btn, on_audio_done, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dl = lv_label_create(done_btn);
    lv_label_set_text(dl, LV_SYMBOL_OK "  Done");
    lv_obj_set_style_text_color(dl, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(dl, &lv_font_montserrat_20, 0);
    lv_obj_center(dl);
}

static void on_tenant_save(lv_event_t *e)
{
    if (!s_tenant_id_ta || !s_tenant_short_ta)
        return;
    const char *tid = lv_textarea_get_text(s_tenant_id_ta);
    const char *tshort = lv_textarea_get_text(s_tenant_short_ta);
    if (tid && tid[0] && tshort && tshort[0])
    {
        weight_config_set_tenant(tid, tshort);
        weight_config_save();
        /* update state so topics rebuild on next mqtt restart */
        weight_state_set_tenant(tid, tshort);
        weight_mqtt_restart();
    }
    close_tenant_popup();
    refresh_values();
}

static void on_tenant_cancel(lv_event_t *e) { close_tenant_popup(); }

static void on_tenant_card(lv_event_t *e)
{
    close_dd_popup();
    close_kbd_popup();
    if (s_tenant_popup)
        return;

    lv_obj_t *scr = lv_screen_active();
    const weight_config_t *cfg = weight_config_get();

    /* Overlay */
    s_tenant_popup = lv_obj_create(scr);
    lv_obj_set_size(s_tenant_popup, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_tenant_popup, 0, 0);
    lv_obj_set_style_bg_opa(s_tenant_popup, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(s_tenant_popup, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_tenant_popup, 0, 0);
    lv_obj_set_style_radius(s_tenant_popup, 0, 0);
    lv_obj_clear_flag(s_tenant_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_tenant_popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_tenant_popup);

    /* Modal */
    lv_obj_t *modal = lv_obj_create(s_tenant_popup);
    lv_obj_set_size(modal, 700, 220);
    lv_obj_align(modal, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(modal, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, UI_RADIUS_S, 0);
    lv_obj_set_style_pad_all(modal, UI_PAD, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lv_obj_t *title = lv_label_create(modal);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_label_set_text(title, "Edit Tenant");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    /* Tenant Short field */
    lv_obj_t *short_lbl = lv_label_create(modal);
    lv_obj_set_style_text_color(short_lbl, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(short_lbl, &lv_font_montserrat_14, 0);
    lv_label_set_text(short_lbl, "Short slug (e.g. gim)");
    lv_obj_align(short_lbl, LV_ALIGN_TOP_LEFT, 0, 34);

    s_tenant_short_ta = lv_textarea_create(modal);
    lv_obj_set_size(s_tenant_short_ta, 300, 48);
    lv_obj_align(s_tenant_short_ta, LV_ALIGN_TOP_LEFT, 0, 54);
    lv_textarea_set_one_line(s_tenant_short_ta, true);
    lv_textarea_set_max_length(s_tenant_short_ta, 15);
    lv_textarea_set_text(s_tenant_short_ta, cfg->tenant_short);
    lv_obj_set_style_text_font(s_tenant_short_ta, &lv_font_montserrat_18, 0);
    lv_obj_set_style_bg_color(s_tenant_short_ta, UI_COLOR_PANEL_ALT, 0);
    lv_obj_set_style_border_color(s_tenant_short_ta, UI_COLOR_ACCENT, 0);
    lv_obj_add_event_cb(s_tenant_short_ta, on_field_focused_tenant, LV_EVENT_CLICKED, NULL);

    /* Tenant ID field */
    lv_obj_t *id_lbl = lv_label_create(modal);
    lv_obj_set_style_text_color(id_lbl, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(id_lbl, &lv_font_montserrat_14, 0);
    lv_label_set_text(id_lbl, "Tenant UUID");
    lv_obj_align(id_lbl, LV_ALIGN_TOP_LEFT, 320, 34);

    s_tenant_id_ta = lv_textarea_create(modal);
    lv_obj_set_size(s_tenant_id_ta, 340, 48);
    lv_obj_align(s_tenant_id_ta, LV_ALIGN_TOP_LEFT, 320, 54);
    lv_textarea_set_one_line(s_tenant_id_ta, true);
    lv_textarea_set_max_length(s_tenant_id_ta, 63);
    lv_textarea_set_text(s_tenant_id_ta, cfg->tenant_id);
    lv_obj_set_style_text_font(s_tenant_id_ta, &lv_font_montserrat_18, 0);
    lv_obj_set_style_bg_color(s_tenant_id_ta, UI_COLOR_PANEL_ALT, 0);
    lv_obj_set_style_border_color(s_tenant_id_ta, UI_COLOR_ACCENT, 0);
    lv_obj_add_event_cb(s_tenant_id_ta, on_field_focused_tenant, LV_EVENT_CLICKED, NULL);

    /* Cancel button */
    lv_obj_t *cancel_btn = lv_btn_create(modal);
    lv_obj_set_size(cancel_btn, 140, 44);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(cancel_btn, UI_COLOR_PANEL_ALT, 0);
    lv_obj_set_style_radius(cancel_btn, UI_RADIUS_S, 0);
    lv_obj_set_style_border_width(cancel_btn, 0, 0);
    lv_obj_add_event_cb(cancel_btn, on_tenant_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cancel_btn);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, UI_COLOR_TEXT, 0);
    lv_obj_center(cl);

    /* Save button */
    lv_obj_t *save_btn = lv_btn_create(modal);
    lv_obj_set_size(save_btn, 140, 44);
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(save_btn, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(save_btn, UI_RADIUS_S, 0);
    lv_obj_set_style_border_width(save_btn, 0, 0);
    lv_obj_add_event_cb(save_btn, on_tenant_save, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(save_btn);
    lv_label_set_text(sl, LV_SYMBOL_OK "  Save");
    lv_obj_set_style_text_color(sl, UI_COLOR_TEXT, 0);
    lv_obj_center(sl);

    /* Keyboard — shared, binds to whichever field was tapped */
    s_tenant_kbd = lv_keyboard_create(s_tenant_popup);
    lv_obj_set_size(s_tenant_kbd, LV_PCT(100), 330);
    lv_obj_align(s_tenant_kbd, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_tenant_kbd, s_tenant_short_ta); /* default focus */
    lv_keyboard_set_mode(s_tenant_kbd, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_event_cb(s_tenant_kbd, on_tenant_save, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_tenant_kbd, on_tenant_cancel, LV_EVENT_CANCEL, NULL);
}

/* ============================================================================
 * DROPDOWN POPUP — shared for Unit, Decimal places, Stability window
 * ========================================================================== */

/* Overlay click → close */
static void on_dd_overlay_click(lv_event_t *e)
{
    close_dd_popup();
}

/* One option button was tapped */
static void on_dd_option(lv_event_t *e)
{
    uint32_t idx = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    const weight_config_t *cfg = weight_config_get();

    switch (s_dd_ctx)
    {
    case DD_UNIT:
        /* idx matches wcfg_unit_t: 0=g 1=kg 2=lb 3=oz */
        weight_config_set_unit((wcfg_unit_t)idx);
        weight_config_save();
        break;

    case DD_DECIMAL:
        /* idx: 0..3 decimal places */
        weight_config_set_decimal((uint8_t)idx);
        weight_config_save();
        break;

    case DD_STAB_WINDOW:
    {
        /* idx: 0=3 1=5 2=7 3=10 samples */
        static const uint8_t windows[] = {3, 5, 7, 10};
        uint8_t w = (idx < 4) ? windows[idx] : 3;
        weight_config_set_stability(w, cfg->stability_tolerance);
        weight_config_save();
        break;
    }

    default:
        break;
    }

    close_dd_popup();
    refresh_values();
}

/**
 * open_dropdown  — builds a small modal list anchored just below `anchor_obj`.
 *
 * @param anchor_obj   The card or button that was tapped (used to position the list)
 * @param ctx          Which setting this list controls
 * @param options      NULL-terminated array of option strings
 * @param n            Number of options
 * @param current_idx  Index of the currently-active option (shown highlighted)
 */
static void open_dropdown(lv_obj_t *anchor_obj, dd_context_t ctx,
                          const char *const *options, int n, int current_idx)
{
    /* Close any existing popup first */
    close_dd_popup();
    s_dd_ctx = ctx;

    lv_obj_t *scr = lv_screen_active();

    /* Full-screen transparent overlay to catch outside-tap */
    s_dd_popup = lv_obj_create(scr);
    lv_obj_set_size(s_dd_popup, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_dd_popup, 0, 0);
    lv_obj_set_style_bg_opa(s_dd_popup, LV_OPA_40, 0);
    lv_obj_set_style_bg_color(s_dd_popup, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_dd_popup, 0, 0);
    lv_obj_set_style_radius(s_dd_popup, 0, 0);
    lv_obj_clear_flag(s_dd_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_dd_popup, on_dd_overlay_click, LV_EVENT_CLICKED, NULL);
    lv_obj_move_foreground(s_dd_popup);

    /* Get anchor position to place the list below it */
    lv_area_t area;
    lv_obj_get_coords(anchor_obj, &area);
    int32_t list_x = area.x1;
    int32_t list_y = area.y2 + 4;
    int32_t list_w = lv_obj_get_width(anchor_obj);
    int32_t item_h = 52;
    int32_t list_h = n * item_h + 8;

    /* Clamp so it doesn't go off screen bottom */
    int32_t scr_h = lv_display_get_vertical_resolution(lv_display_get_default());
    if (list_y + list_h > scr_h - 8)
        list_y = area.y1 - list_h - 4;

    /* List panel */
    lv_obj_t *panel = lv_obj_create(s_dd_popup);
    lv_obj_set_size(panel, list_w, list_h);
    lv_obj_set_pos(panel, list_x, list_y);
    lv_obj_set_style_bg_color(panel, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, UI_RADIUS_S, 0);
    lv_obj_set_style_pad_all(panel, 4, 0);
    lv_obj_set_style_pad_row(panel, 2, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    /* Stop overlay-click from firing when user clicks inside the panel */
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_CLICKABLE);

    /* Option buttons */
    for (int i = 0; i < n; i++)
    {
        lv_obj_t *btn = lv_btn_create(panel);
        lv_obj_set_size(btn, list_w - 8, item_h - 4);
        lv_obj_set_pos(btn, 0, 4 + i * item_h);

        bool is_active = (i == current_idx);
        lv_obj_set_style_bg_color(btn,
                                  is_active ? UI_COLOR_ACCENT : UI_COLOR_PANEL_ALT, 0);
        lv_obj_set_style_radius(btn, UI_RADIUS_S, 0);
        lv_obj_set_style_border_width(btn, 0, 0);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, options[i]);
        lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
        lv_obj_center(lbl);

        lv_obj_add_event_cb(btn, on_dd_option, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
    }
}

/* ============================================================================
 * Display Unit card → opens unit dropdown
 * ========================================================================== */
static void on_unit_card(lv_event_t *e)
{
    const weight_config_t *cfg = weight_config_get();
    /* options match wcfg_unit_t: 0=g 1=kg 2=lb 3=oz */
    static const char *unit_opts[] = {"g", "kg", "lb", "oz"};
    open_dropdown(lv_event_get_current_target(e),
                  DD_UNIT, unit_opts, 4, (int)cfg->unit);
}

/* ============================================================================
 * Decimal places card → opens decimal dropdown
 * ========================================================================== */
static void on_decimal_card(lv_event_t *e)
{
    const weight_config_t *cfg = weight_config_get();
    static const char *dec_opts[] = {"0 decimals", "1 decimal", "2 decimals", "3 decimals"};
    open_dropdown(lv_event_get_current_target(e),
                  DD_DECIMAL, dec_opts, 4, (int)cfg->decimal_places);
}

/* ============================================================================
 * Stability card → opens dropdown for sample window
 * ========================================================================== */
static void on_stability_card(lv_event_t *e)
{
    const weight_config_t *cfg = weight_config_get();

    static const char *stab_opts[] = {"3 samples", "5 samples", "7 samples", "10 samples"};
    int current = 0;
    switch (cfg->stability_window)
    {
    case 5:
        current = 1;
        break;
    case 7:
        current = 2;
        break;
    case 10:
        current = 3;
        break;
    default:
        current = 0;
        break;
    }
    open_dropdown(lv_event_get_current_target(e),
                  DD_STAB_WINDOW, stab_opts, 4, current);
}

/* ============================================================================
 * Device ID keyboard popup
 * ========================================================================== */
static void on_kbd_ready(lv_event_t *e)
{
    if (!s_kbd_ta)
        return;
    const char *txt = lv_textarea_get_text(s_kbd_ta);
    if (txt && txt[0])
    {
        weight_config_set_device_id(txt);
        weight_config_save();
    }
    close_kbd_popup();
    refresh_values();
}

static void on_kbd_cancel(lv_event_t *e)
{
    close_kbd_popup();
}

static void on_device_card(lv_event_t *e)
{
    close_dd_popup(); /* safety */
    if (s_kbd_popup)
        return; /* already open */

    lv_obj_t *scr = lv_screen_active();
    const weight_config_t *cfg = weight_config_get();

    /* Semi-transparent overlay — NOT clickable itself so it doesn't
     * swallow taps meant for the modal's Save/Cancel buttons */
    s_kbd_popup = lv_obj_create(scr);
    lv_obj_set_size(s_kbd_popup, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_kbd_popup, 0, 0);
    lv_obj_set_style_bg_opa(s_kbd_popup, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(s_kbd_popup, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_kbd_popup, 0, 0);
    lv_obj_set_style_radius(s_kbd_popup, 0, 0);
    lv_obj_clear_flag(s_kbd_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_kbd_popup, LV_OBJ_FLAG_CLICKABLE); /* don't swallow taps */
    lv_obj_move_foreground(s_kbd_popup);

    /* Modal card — upper portion of screen */
    lv_obj_t *modal = lv_obj_create(s_kbd_popup);
    lv_obj_set_size(modal, 700, 160);
    lv_obj_align(modal, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(modal, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, UI_RADIUS_S, 0);
    lv_obj_set_style_pad_all(modal, UI_PAD, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lv_obj_t *title = lv_label_create(modal);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_label_set_text(title, "Edit Device ID");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    /* Text area — pre-fill from config (not state) */
    s_kbd_ta = lv_textarea_create(modal);
    lv_obj_set_size(s_kbd_ta, 660, 52);
    lv_obj_align(s_kbd_ta, LV_ALIGN_TOP_LEFT, 0, 34);
    lv_textarea_set_one_line(s_kbd_ta, true);
    lv_textarea_set_max_length(s_kbd_ta, 31);
    lv_textarea_set_placeholder_text(s_kbd_ta, "device-id");
    lv_textarea_set_text(s_kbd_ta, cfg->device_id); /* read from config */
    lv_obj_set_style_text_font(s_kbd_ta, &lv_font_montserrat_18, 0);
    lv_obj_set_style_bg_color(s_kbd_ta, UI_COLOR_PANEL_ALT, 0);
    lv_obj_set_style_border_width(s_kbd_ta, 1, 0);
    lv_obj_set_style_border_color(s_kbd_ta, UI_COLOR_ACCENT, 0);

    /* Cancel button */
    lv_obj_t *cancel_btn = lv_btn_create(modal);
    lv_obj_set_size(cancel_btn, 140, 44);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(cancel_btn, UI_COLOR_PANEL_ALT, 0);
    lv_obj_set_style_radius(cancel_btn, UI_RADIUS_S, 0);
    lv_obj_set_style_border_width(cancel_btn, 0, 0);
    lv_obj_add_event_cb(cancel_btn, on_kbd_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cancel_btn);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, UI_COLOR_TEXT, 0);
    lv_obj_center(cl);

    /* Save button */
    lv_obj_t *save_btn = lv_btn_create(modal);
    lv_obj_set_size(save_btn, 140, 44);
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(save_btn, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(save_btn, UI_RADIUS_S, 0);
    lv_obj_set_style_border_width(save_btn, 0, 0);
    lv_obj_add_event_cb(save_btn, on_kbd_ready, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(save_btn);
    lv_label_set_text(sl, LV_SYMBOL_OK "  Save");
    lv_obj_set_style_text_color(sl, UI_COLOR_TEXT, 0);
    lv_obj_center(sl);

    /* LVGL keyboard — fills lower portion */
    s_kbd = lv_keyboard_create(s_kbd_popup);
    lv_obj_set_size(s_kbd, LV_PCT(100), 330);
    lv_obj_align(s_kbd, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_kbd, s_kbd_ta);
    lv_keyboard_set_mode(s_kbd, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_event_cb(s_kbd, on_kbd_ready, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_kbd, on_kbd_cancel, LV_EVENT_CANCEL, NULL);
}

/* ============================================================================
 * Helper: build one labelled card.
 * Returns the value label pointer so the caller can cache it.
 * ========================================================================== */
static lv_obj_t *make_setting_card(lv_obj_t *parent, const char *title,
                                   lv_event_cb_t on_click)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_style_bg_color(card, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, UI_RADIUS_S, 0);
    lv_obj_set_style_pad_hor(card, 16, 0);
    lv_obj_set_style_pad_ver(card, 10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    if (on_click)
    {
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, on_click, LV_EVENT_CLICKED, NULL);

        /* Subtle pressed state */
        lv_obj_set_style_bg_color(card, UI_COLOR_PANEL_ALT,
                                  LV_STATE_PRESSED);
    }

    /* "tap to change" hint for interactive cards */
    if (on_click)
    {
        lv_obj_t *hint = lv_label_create(card);
        lv_obj_set_style_text_color(hint, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
        lv_label_set_text(hint, LV_SYMBOL_RIGHT);
        lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_add_flag(hint, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *t = lv_label_create(card);
    lv_obj_set_style_text_color(t, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
    lv_label_set_text(t, title);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(t, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *v = lv_label_create(card);
    lv_obj_set_style_text_color(v, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_18, 0);
    lv_label_set_text(v, "…");
    lv_obj_align(v, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_flag(v, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(v, LV_OBJ_FLAG_CLICKABLE);

    return v; /* caller caches this */
}

/* ============================================================================
 * Build the settings screen
 * ========================================================================== */
lv_obj_t *screen_settings_create(void)
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

    lv_obj_t *back = lv_btn_create(topbar);
    lv_obj_set_size(back, 100, 40);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(back, UI_COLOR_PANEL_ALT, 0);
    lv_obj_set_style_radius(back, UI_RADIUS_S, 0);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT "  back");
    lv_obj_center(bl);

    lv_obj_t *title = lv_label_create(topbar);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_label_set_text(title, "Settings");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *ver = lv_label_create(topbar);
    lv_obj_set_style_text_color(ver, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_14, 0);
    lv_label_set_text(ver, "v1.0.0");
    lv_obj_align(ver, LV_ALIGN_RIGHT_MID, 0, 0);

    /* ---- 2-column grid ---- */
    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_set_size(grid, UI_W, UI_H - 60);
    lv_obj_align(grid, LV_ALIGN_TOP_LEFT, 0, 60);
    lv_obj_set_style_bg_color(grid, UI_COLOR_BG, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_radius(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, UI_PAD, 0);
    lv_obj_set_style_pad_row(grid, UI_PAD_S, 0);
    lv_obj_set_style_pad_column(grid, UI_PAD_S, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    static int32_t col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static int32_t row_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(grid, col_dsc, row_dsc);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);

    /* Row 0 — WiFi + MQTT */
    s_lbls.wifi = make_setting_card(grid, "WiFi", on_wifi_card);
    lv_obj_set_grid_cell(lv_obj_get_parent(s_lbls.wifi),
                         LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 0, 1);

    s_lbls.mqtt = make_setting_card(grid, "MQTT broker", on_mqtt_card);
    lv_obj_set_grid_cell(lv_obj_get_parent(s_lbls.mqtt),
                         LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 0, 1);

    /* Row 1 — Unit + Decimal (both dropdowns) */
    s_lbls.unit = make_setting_card(grid, "Unit", on_unit_card);
    lv_obj_set_grid_cell(lv_obj_get_parent(s_lbls.unit),
                         LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 1, 1);

    s_lbls.decimal = make_setting_card(grid, "Decimal places", on_decimal_card);
    lv_obj_set_grid_cell(lv_obj_get_parent(s_lbls.decimal),
                         LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 1, 1);

    /* Row 2 — Scale (read-only) + Stability dropdown */
    s_lbls.scale = make_setting_card(grid, "Scale", NULL);
    lv_obj_set_grid_cell(lv_obj_get_parent(s_lbls.scale),
                         LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 2, 1);

    s_lbls.stability = make_setting_card(grid, "Stability", on_stability_card);
    lv_obj_set_grid_cell(lv_obj_get_parent(s_lbls.stability),
                         LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 2, 1);

    /* Row 3 — Audio toggle + Device ID editor */
    s_lbls.audio = make_setting_card(grid, "Audio", on_audio_card);
    lv_obj_set_grid_cell(lv_obj_get_parent(s_lbls.audio),
                         LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 3, 1);

    s_lbls.device = make_setting_card(grid, "Device ID  " LV_SYMBOL_EDIT, on_device_card);
    lv_obj_set_grid_cell(lv_obj_get_parent(s_lbls.device),
                         LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 3, 1);

    /* Row 4 col 0 — Tenant */
    s_lbls.tenant = make_setting_card(grid, "Tenant  " LV_SYMBOL_EDIT, on_tenant_card);
    lv_obj_set_grid_cell(lv_obj_get_parent(s_lbls.tenant),
                         LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 4, 1);

    /* Row 4 col 1 — About */
    s_lbls.about = make_setting_card(grid, "About", NULL);
    lv_obj_set_grid_cell(lv_obj_get_parent(s_lbls.about),
                         LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 4, 1);

    refresh_values();
    return scr;
}
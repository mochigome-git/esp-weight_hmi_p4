/**
 * @file screen_main.c
 * @brief Main weight display screen.
 *
 * Fixes applied:
 *  - Device ID now reads cfg->device_id (not snap.device_id)
 *  - MQTT "icon" is now a text label "MQTT" (not LV_SYMBOL_UPLOAD)
 *  - Clock shows "--:--" until NTP has synced (year > 2020 check)
 *  - Top bar uses a proper flex row so labels never overlap
 */

#include "lvgl.h"
#include "esp_log.h"
#include <time.h>
#include "ui_theme.h"
#include "weight_i18n.h"
#include "weight_ui.h"
#include "weight_state.h"
#include "weight_config.h"
#include "weight_model_store.h"
#include "esp_timer.h"
#include "weight_mqtt.h"

static const char *TAG = "scr_main";

/* ---------------------------------------------------------------------------
 * State
 * --------------------------------------------------------------------------- */
static bool s_store_ready = false;
static lv_timer_t *s_refresh_timer = NULL;

/* Cached widget pointers */
static lv_obj_t *s_weight_lbl;
static lv_obj_t *s_unit_lbl;
static lv_obj_t *s_status_pill_lbl;
static lv_obj_t *s_pass_cnt_lbl; /* "PASS 123" counter, top-right of weight card */
static lv_obj_t *s_weight_card;
static lv_obj_t *s_model_lbl;
static lv_obj_t *s_upper_lbl;
static lv_obj_t *s_std_lbl;
static lv_obj_t *s_lower_lbl;
static lv_obj_t *s_wifi_icon;
static lv_obj_t *s_mqtt_lbl; /* text "MQTT", not an icon */
static lv_obj_t *s_clock_lbl;
static lv_obj_t *s_device_lbl;
static lv_obj_t *s_runstop_btn;
static lv_obj_t *s_runstop_lbl;
static lv_obj_t *s_alert_banner;
static lv_obj_t *s_alert_text;
static lv_obj_t *s_model_dropdown;
static lv_obj_t *s_newlot_btn;
static lv_obj_t *s_lot_val_lbl;   /* "#3" / "no active lot" / "--" */
static lv_obj_t *s_lot_since_lbl; /* "since 09:36" / "requesting..." */
static uint32_t s_seen_lot_seq;   /* last NEW LOT result already toasted */
static lv_obj_t *s_lot_popup = NULL;
static int64_t s_last_lot_us = 0;
static size_t s_last_model_count = (size_t)-1;

static lv_timer_t *s_resume_delay_timer = NULL;

static weight_status_t s_last_card_status = (weight_status_t)-1;

#define LOT_COOLDOWN_US (3 * 1000000LL)
#define LOT_ACK_TIMEOUT_US (10 * 1000000LL) /* give up waiting for lot/ack */

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------- */

static void set_text_if_changed(lv_obj_t *lbl, const char *txt)
{
    const char *cur = lv_label_get_text(lbl);
    if (cur && strcmp(cur, txt) == 0)
        return; /* identical — skip realloc + invalidate + redraw */
    lv_label_set_text(lbl, txt);
}

static void format_weight(char *out, size_t out_len, float v, uint8_t dec)
{
    snprintf(out, out_len, "%.*f", dec, v);
}

static void set_card_color(weight_status_t s)
{
    lv_color_t bg;

    if (s == s_last_card_status)
        return;

    switch (s)
    {
    case WEIGHT_STATUS_PASS:
        bg = UI_COLOR_PASS;
        break;
    case WEIGHT_STATUS_HIGH:
        bg = UI_COLOR_HIGH;
        break;
    case WEIGHT_STATUS_LOW:
        bg = UI_COLOR_LOW;
        break;
    default:
        bg = UI_COLOR_PANEL;
        break;
    }
    lv_obj_set_style_bg_color(s_weight_card, bg, 0);
}

static void rebuild_model_dropdown(void)
{
    if (!s_store_ready)
        return;

    char buf[2048] = {0};
    size_t off = 0;
    size_t n = weight_model_store_count();
    for (size_t i = 0; i < n; i++)
    {
        weight_model_t m;
        if (!weight_model_store_get(i, &m))
            continue;
        if (m.deleted)
            continue;
        int written = snprintf(buf + off, sizeof(buf) - off,
                               "%s%s", off > 0 ? "\n" : "", m.name);
        if (written < 0)
            break;
        off += (size_t)written;
        if (off >= sizeof(buf) - 32)
            break;
    }
    if (off == 0)
        snprintf(buf, sizeof(buf), "%s", TR(main_model_none));

    lv_dropdown_set_options(s_model_dropdown, buf);
}

static void toast_timer_cb(lv_timer_t *t)
{
    lv_obj_t *toast = (lv_obj_t *)lv_timer_get_user_data(t);
    if (toast)
        lv_obj_delete(toast);
}

static void show_toast(const char *msg, lv_color_t bg)
{
    lv_obj_t *toast = lv_obj_create(lv_layer_top());
    lv_obj_set_size(toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(toast, bg, 0);
    lv_obj_set_style_border_width(toast, 0, 0);
    lv_obj_set_style_radius(toast, UI_RADIUS, 0);
    lv_obj_set_style_pad_hor(toast, 28, 0);
    lv_obj_set_style_pad_ver(toast, 16, 0);
    lv_obj_clear_flag(toast, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(toast, LV_ALIGN_TOP_MID, 0, 60);

    lv_obj_t *l = lv_label_create(toast);
    lv_obj_set_style_text_color(l, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(l, ui_font_normal(), 0);
    lv_label_set_text(l, msg);

    lv_timer_t *t = lv_timer_create(toast_timer_cb, 2500, toast);
    lv_timer_set_repeat_count(t, 1);
}

static void close_lot_popup(void)
{
    if (s_lot_popup)
    {
        lv_obj_delete_async(s_lot_popup); /* called from a child's event - don't delete synchronously */
        s_lot_popup = NULL;
    }
}

/* ---------------------------------------------------------------------------
 * Event handlers
 * --------------------------------------------------------------------------- */
static void resume_refresh_timer_cb(void *unused)
{
    if (s_refresh_timer)
        lv_timer_resume(s_refresh_timer);
}

static void apply_fonts(void)
{
    lv_obj_set_style_text_font(s_device_lbl, ui_font_small(), 0);
    lv_obj_set_style_text_font(s_clock_lbl, ui_font_small(), 0);
    lv_obj_set_style_text_font(s_mqtt_lbl, &lv_font_montserrat_16, 0); /* stays Montserrat by design */
    lv_obj_set_style_text_font(s_status_pill_lbl, ui_font_small(), 0);
    lv_obj_set_style_text_font(s_model_dropdown, ui_font_small(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_runstop_lbl, ui_font_large(), 0);
    lv_obj_set_style_text_font(s_lot_val_lbl, ui_font_large(), 0);
    lv_obj_set_style_text_font(s_lot_since_lbl, ui_font_small(), 0);
    /* + the make_field() labels if you keep pointers to their label/value objs */
}

static void resume_delay_timer_cb(lv_timer_t *t)
{
    if (s_refresh_timer)
        lv_timer_resume(s_refresh_timer);
    s_resume_delay_timer = NULL; /* LVGL auto-deletes (repeat_count=1) — do NOT lv_timer_del here */
}

static void on_screen_load(lv_event_t *e)
{
    s_store_ready = true;
    apply_fonts();
    rebuild_model_dropdown();

    /* Delay resuming the 10Hz refresh timer by one display-refresh cycle
     * so the giant weight label's first redraw doesn't land in the same
     * frame as this screen's own transition redraw. lv_async_call alone
     * isn't enough — a paused timer fires immediately on resume since it
     * doesn't track elapsed time while paused, so it lands in the same
     * lv_timer_handler() pass as the transition. A real delay avoids that. */
    if (s_resume_delay_timer)
    {
        lv_timer_del(s_resume_delay_timer);
        s_resume_delay_timer = NULL;
    }
    s_resume_delay_timer = lv_timer_create(resume_delay_timer_cb, 80, NULL);
    lv_timer_set_repeat_count(s_resume_delay_timer, 1);
}

static void pause_refresh_timer_cb(void *unused)
{
    if (s_refresh_timer)
        lv_timer_pause(s_refresh_timer);
}

static void on_models_btn(lv_event_t *e)
{
    rebuild_model_dropdown();
    if (s_refresh_timer)
        lv_timer_pause(s_refresh_timer); /* synchronous, no async needed */
    weight_ui_show(UI_SCREEN_MODELS);
}

static void on_settings_btn(lv_event_t *e)
{
    if (s_refresh_timer)
        lv_timer_pause(s_refresh_timer);
    weight_ui_show(UI_SCREEN_SETTINGS);
}
static void on_runstop_btn(lv_event_t *e)
{
    weight_mode_t cur = weight_state_get_mode();
    if (cur == WEIGHT_MODE_RUN)
    {
        weight_state_set_mode(WEIGHT_MODE_IDLE);
    }
    else
    {
        if (!weight_state_has_active_model())
        {
            ESP_LOGW(TAG, "cannot enter RUN mode - no model selected");
            return;
        }
        weight_state_set_mode(WEIGHT_MODE_RUN);
    }
}

static void on_alert_dismiss(lv_event_t *e)
{
    weight_state_dismiss_alert();
}

static void on_model_picked(lv_event_t *e)
{
    if (!s_store_ready)
        return;

    uint16_t sel = lv_dropdown_get_selected(s_model_dropdown);
    size_t idx_alive = 0;
    size_t n = weight_model_store_count();
    for (size_t i = 0; i < n; i++)
    {
        weight_model_t m;
        if (!weight_model_store_get(i, &m))
            continue;
        if (m.deleted)
            continue;
        if (idx_alive == sel)
        {
            weight_active_model_t am = {0};
            strncpy(am.id, m.id, sizeof(am.id) - 1);
            strncpy(am.name, m.name, sizeof(am.name) - 1);
            strncpy(am.unit, m.unit[0] ? m.unit : "g", sizeof(am.unit) - 1);
            am.lower_limit = m.lower_limit;
            am.standard = m.standard;
            am.upper_limit = m.upper_limit;
            am.valid = true;
            weight_state_set_active_model(&am);
            return;
        }
        idx_alive++;
    }
}

static void on_lot_cancel(lv_event_t *e) { close_lot_popup(); }

static void on_lot_confirm(lv_event_t *e)
{
    close_lot_popup();
    esp_err_t err = weight_mqtt_publish_new_lot();
    if (err == ESP_OK)
    {
        weight_state_reset_pass_count(); /* new lot -> count from zero */
        s_last_lot_us = esp_timer_get_time();
        show_toast(TR(main_lot_sent), UI_COLOR_PASS_DARK);
    }
    else if (err == ESP_ERR_NOT_FOUND)
        show_toast(TR(main_lot_no_machine), UI_COLOR_HIGH_DARK);
    else
        show_toast(TR(main_lot_offline), UI_COLOR_HIGH_DARK);
}

static void on_newlot_btn(lv_event_t *e)
{
    if (s_lot_popup)
        return;
    if (s_last_lot_us && esp_timer_get_time() - s_last_lot_us < LOT_COOLDOWN_US)
        return; /* swallow accidental double taps */

    s_lot_popup = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_lot_popup, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_lot_popup, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_lot_popup, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_lot_popup, 0, 0);
    lv_obj_set_style_radius(s_lot_popup, 0, 0);
    lv_obj_clear_flag(s_lot_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_lot_popup, LV_OBJ_FLAG_CLICKABLE); /* block touches behind */

    lv_obj_t *modal = lv_obj_create(s_lot_popup);
    lv_obj_set_size(modal, 520, 220);
    lv_obj_center(modal);
    lv_obj_set_style_bg_color(modal, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, UI_RADIUS_S, 0);
    lv_obj_set_style_pad_all(modal, UI_PAD * 2, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *msg = lv_label_create(modal);
    lv_obj_set_style_text_color(msg, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(msg, ui_font_large(), 0);
    lv_label_set_text(msg, TR(main_lot_confirm));
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *cancel = lv_btn_create(modal);
    lv_obj_set_size(cancel, 200, 64);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(cancel, UI_COLOR_PANEL_ALT, 0);
    lv_obj_set_style_radius(cancel, UI_RADIUS_S, 0);
    lv_obj_add_event_cb(cancel, on_lot_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, TR(edit_cancel));
    lv_obj_set_style_text_font(cl, ui_font_normal(), 0);
    lv_obj_center(cl);

    lv_obj_t *ok = lv_btn_create(modal);
    lv_obj_set_size(ok, 200, 64);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(ok, UI_COLOR_LOW_DARK, 0);
    lv_obj_set_style_radius(ok, UI_RADIUS_S, 0);
    lv_obj_add_event_cb(ok, on_lot_confirm, LV_EVENT_CLICKED, NULL);
    lv_obj_t *okl = lv_label_create(ok);
    lv_label_set_text(okl, TR(main_lot_confirm_btn));
    lv_obj_set_style_text_font(okl, ui_font_normal(), 0);
    lv_obj_center(okl);
}

/* ---------------------------------------------------------------------------
 * Refresh timer - 10 Hz
 * --------------------------------------------------------------------------- */
static void refresh_cb(lv_timer_t *t)
{
    /* Rebuild dropdown if model count changed (covers add/delete from edit screen) */
    if (s_store_ready)
    {
        size_t n = weight_model_store_count();
        if (n != s_last_model_count)
        {
            s_last_model_count = n;
            rebuild_model_dropdown();
        }
    }

    weight_state_snapshot_t snap;
    weight_state_get_snapshot(&snap);
    const weight_config_t *cfg = weight_config_get();

    char buf[64];

    /* Weight value */
    format_weight(buf, sizeof(buf), snap.last_reading, cfg->decimal_places);
    set_text_if_changed(s_weight_lbl, buf);

    /* Unit */
    set_text_if_changed(s_unit_lbl, weight_config_unit_str(cfg->unit));

    /* Status pill */
    const char *status_txt = TR(main_status_idle);
    if (snap.mode == WEIGHT_MODE_RUN)
    {
        if (!snap.last_stable)
        {
            status_txt = TR(main_status_weighing);
        }
        else
        {
            switch (snap.last_status)
            {
            case WEIGHT_STATUS_PASS:
                status_txt = TR(main_status_pass);
                break;
            case WEIGHT_STATUS_HIGH:
                status_txt = TR(main_status_high);
                break;
            case WEIGHT_STATUS_LOW:
                status_txt = TR(main_status_low);
                break;
            default:
                status_txt = TR(main_status_run);
                break;
            }
        }
    }
    else if (snap.model.valid)
    {
        status_txt = TR(main_status_idle_model);
    }

    set_text_if_changed(s_status_pill_lbl, status_txt);

    /* PASS counter */
    snprintf(buf, sizeof(buf), "%s  %lu", TR(main_pass_count), (unsigned long)snap.pass_count);
    set_text_if_changed(s_pass_cnt_lbl, buf);

    /* Card color — only show PASS/HIGH/LOW after reading is stable.
     * Otherwise stay neutral (panel color) while operator is still placing item. */
    weight_status_t display_status = WEIGHT_STATUS_NONE;
    if (snap.mode == WEIGHT_MODE_RUN && snap.last_stable)
    {
        display_status = snap.last_status;
    }
    set_card_color(display_status);

    /* Right panel: active model info */
    if (snap.model.valid)
    {
        set_text_if_changed(s_model_lbl, snap.model.name);
        snprintf(buf, sizeof(buf), "%.*f", cfg->decimal_places, snap.model.upper_limit);
        lv_label_set_text(s_upper_lbl, buf);
        snprintf(buf, sizeof(buf), "%.*f", cfg->decimal_places, snap.model.standard);
        lv_label_set_text(s_std_lbl, buf);
        snprintf(buf, sizeof(buf), "%.*f", cfg->decimal_places, snap.model.lower_limit);
        lv_label_set_text(s_lower_lbl, buf);
    }
    else
    {
        lv_label_set_text(s_model_lbl, TR(main_model_empty));
        lv_label_set_text(s_upper_lbl, "");
        lv_label_set_text(s_std_lbl, "");
        lv_label_set_text(s_lower_lbl, "");
    }

    /* Current lot card */
    if (snap.lot_req_pending &&
        esp_timer_get_time() - snap.lot_req_since_us > LOT_ACK_TIMEOUT_US)
    {
        weight_state_lot_request_finish(WEIGHT_LOT_RESULT_TIMEOUT);
        weight_state_get_snapshot(&snap); /* pick up the finished request */
    }

    if (snap.lot_known && snap.lot_active)
    {
        snprintf(buf, sizeof(buf), "#%ld", (long)snap.lot_number);
        set_text_if_changed(s_lot_val_lbl, buf);
        lv_obj_set_style_text_color(s_lot_val_lbl, UI_COLOR_TEXT, 0);
    }
    else
    {
        set_text_if_changed(s_lot_val_lbl, snap.lot_known ? TR(main_lot_none) : "--");
        lv_obj_set_style_text_color(s_lot_val_lbl, UI_COLOR_TEXT_DIM, 0);
    }

    if (snap.lot_req_pending)
    {
        set_text_if_changed(s_lot_since_lbl, TR(main_lot_pending));
        lv_obj_set_style_text_color(s_lot_since_lbl, UI_COLOR_ACCENT, 0);
    }
    else
    {
        char hhmm[8] = "";
        if (snap.lot_known && snap.lot_active && snap.lot_started_ts > 0)
        {
            struct tm lt;
            localtime_r(&snap.lot_started_ts, &lt);
            snprintf(hhmm, sizeof(hhmm), "%02d:%02d", lt.tm_hour, lt.tm_min);
            snprintf(buf, sizeof(buf), TR(main_lot_since_fmt), hhmm);
        }
        else
            buf[0] = '\0';
        set_text_if_changed(s_lot_since_lbl, buf);
        lv_obj_set_style_text_color(s_lot_since_lbl, UI_COLOR_TEXT_MUTED, 0);
    }

    /* NEW LOT outcome -> one toast per finished request */
    if (snap.lot_result_seq != s_seen_lot_seq)
    {
        s_seen_lot_seq = snap.lot_result_seq;
        switch (snap.lot_result)
        {
        case WEIGHT_LOT_RESULT_OK:
            snprintf(buf, sizeof(buf), TR(main_lot_ok_fmt), (long)snap.lot_number);
            show_toast(buf, UI_COLOR_PASS_DARK);
            break;
        case WEIGHT_LOT_RESULT_ERROR:
            show_toast(TR(main_lot_failed), UI_COLOR_HIGH_DARK);
            break;
        case WEIGHT_LOT_RESULT_TIMEOUT:
            show_toast(TR(main_lot_timeout), UI_COLOR_HIGH_DARK);
            break;
        default:
            break;
        }
    }

    /* RUN/STOP button */
    if (snap.mode == WEIGHT_MODE_RUN)
    {
        set_text_if_changed(s_runstop_lbl, TR(main_btn_stop));
        lv_obj_set_style_bg_color(s_runstop_btn, UI_COLOR_HIGH_DARK, 0);
    }
    else
    {
        set_text_if_changed(s_runstop_lbl, TR(main_btn_ready));
        lv_obj_set_style_bg_color(s_runstop_btn, UI_COLOR_PASS_DARK, 0);
    }

    /* WiFi icon */
    lv_obj_set_style_text_color(s_wifi_icon,
                                snap.wifi_connected ? UI_COLOR_PASS : UI_COLOR_TEXT_DIM, 0);

    /* MQTT text label — green when connected, dim when not */
    lv_obj_set_style_text_color(s_mqtt_lbl,
                                snap.mqtt_connected ? UI_COLOR_PASS : UI_COLOR_TEXT_DIM, 0);

    /* Clock — show "--:--" until NTP has set a real time (year > 2020) */
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    if (tm_info.tm_year > (2020 - 1900))
    {
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                 tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
    }
    else
    {
        snprintf(buf, sizeof(buf), "--:--:--");
    }
    set_text_if_changed(s_clock_lbl, buf);

    /* Device name — human-readable label for display. Falls back to
     * device_id if no name has been set yet (e.g. freshly flashed unit). */
    set_text_if_changed(s_device_lbl,
                        cfg->device_name[0] ? cfg->device_name
                                            : (cfg->device_id[0] ? cfg->device_id : snap.device_id));

    /* Sticky alert banner */
    if (snap.alert_active)
    {
        lv_obj_clear_flag(s_alert_banner, LV_OBJ_FLAG_HIDDEN);
        const char *prefix = (snap.alert_status == WEIGHT_STATUS_HIGH)
                                 ? TR(main_alert_high)
                                 : TR(main_alert_low);
        float limit_val = (snap.alert_status == WEIGHT_STATUS_HIGH)
                              ? snap.model.upper_limit
                              : snap.model.lower_limit;
        snprintf(buf, sizeof(buf), "%s (%.*f)", prefix, cfg->decimal_places, limit_val);
        lv_label_set_text(s_alert_text, buf);
    }
    else
    {
        lv_obj_add_flag(s_alert_banner, LV_OBJ_FLAG_HIDDEN);
    }
}

void screen_main_cleanup(void)
{
    if (s_refresh_timer)
    {
        lv_timer_pause(s_refresh_timer);
        lv_timer_del(s_refresh_timer);
        s_refresh_timer = NULL;
    }
    if (s_resume_delay_timer)
    {
        lv_timer_del(s_resume_delay_timer);
        s_resume_delay_timer = NULL;
    }
    s_store_ready = false;
    s_last_model_count = (size_t)-1;
    s_last_card_status = (weight_status_t)-1;
}

/* ---------------------------------------------------------------------------
 * Field card helper
 * --------------------------------------------------------------------------- */
static lv_obj_t *make_field(lv_obj_t *parent, const char *label, lv_obj_t **value_out)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, LV_PCT(100), 56);
    lv_obj_set_style_bg_color(card, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, UI_RADIUS_S, 0);
    lv_obj_set_style_pad_hor(card, 10, 0);
    lv_obj_set_style_pad_ver(card, 6, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = lv_label_create(card);
    lv_obj_set_style_text_color(l, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(l, ui_font_small(), 0);
    lv_label_set_text(l, label);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 0);

    *value_out = lv_label_create(card);
    lv_obj_set_style_text_color(*value_out, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(*value_out, ui_font_normal(), 0);
    lv_label_set_text(*value_out, "-");
    lv_obj_align(*value_out, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    return card;
}

/* ---------------------------------------------------------------------------
 * Build screen
 * --------------------------------------------------------------------------- */
lv_obj_t *screen_main_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_COLOR_BG, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_screen_load, LV_EVENT_SCREEN_LOADED, NULL);

    /* ----- Top bar -----
     * Uses a flex row so items never overlap regardless of text length.
     * Left:   device_id
     * Center: [WiFi icon]  [MQTT text]
     * Right:  clock
     */
    lv_obj_t *topbar = lv_obj_create(scr);
    lv_obj_set_size(topbar, UI_W, 44);
    lv_obj_align(topbar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(topbar, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_width(topbar, 0, 0);
    lv_obj_set_style_radius(topbar, 0, 0);
    lv_obj_set_style_pad_hor(topbar, UI_PAD, 0);
    lv_obj_set_style_pad_ver(topbar, 0, 0);
    lv_obj_clear_flag(topbar, LV_OBJ_FLAG_SCROLLABLE);

    /* Device ID — left-anchored, fixed width so it doesn't push icons */
    s_device_lbl = lv_label_create(topbar);
    lv_obj_set_style_text_color(s_device_lbl, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_device_lbl, ui_font_small(), 0);
    lv_label_set_long_mode(s_device_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_device_lbl, 320);
    lv_label_set_text(s_device_lbl, "p4-...");
    lv_obj_align(s_device_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    /* Clock — right-anchored, fixed width */
    s_clock_lbl = lv_label_create(topbar);
    lv_obj_set_style_text_color(s_clock_lbl, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_clock_lbl, ui_font_small(), 0);
    lv_obj_set_width(s_clock_lbl, 110);
    lv_obj_set_style_text_align(s_clock_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_clock_lbl, "--:--:--");
    lv_obj_align(s_clock_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    /* Center group — WiFi icon + MQTT label in a flex row */
    lv_obj_t *center_group = lv_obj_create(topbar);
    lv_obj_set_size(center_group, LV_SIZE_CONTENT, 44);
    lv_obj_align_to(center_group, s_clock_lbl, LV_ALIGN_OUT_LEFT_MID, -12, 0);
    lv_obj_set_style_bg_opa(center_group, 0, 0);
    lv_obj_set_style_border_width(center_group, 0, 0);
    lv_obj_set_style_pad_all(center_group, 0, 0);
    lv_obj_set_style_pad_column(center_group, 8, 0);
    lv_obj_set_flex_flow(center_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(center_group, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(center_group, LV_OBJ_FLAG_SCROLLABLE);

    /* WiFi icon — always Montserrat, LV_SYMBOL is LVGL-specific */
    s_wifi_icon = lv_label_create(center_group);
    lv_obj_set_style_text_font(s_wifi_icon, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_wifi_icon, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);

    /* MQTT label */
    s_mqtt_lbl = lv_label_create(center_group);
    lv_obj_set_style_text_font(s_mqtt_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_mqtt_lbl, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(s_mqtt_lbl, "MQTT");

    /* ----- Right panel (model info + actions) ----- */
    const int RIGHT_W = 280;
    lv_obj_t *right = lv_obj_create(scr);
    lv_obj_set_size(right, RIGHT_W, UI_H - 44);
    lv_obj_align(right, LV_ALIGN_TOP_RIGHT, 0, 44);
    lv_obj_set_style_bg_color(right, UI_COLOR_BG, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_radius(right, 0, 0);
    lv_obj_set_style_pad_all(right, UI_PAD, 0);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(right, UI_PAD_S, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *picker_lbl = lv_label_create(right);
    lv_obj_set_style_text_color(picker_lbl, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(picker_lbl, ui_font_small(), 0);
    lv_label_set_text(picker_lbl, TR(main_select_model));

    s_model_dropdown = lv_dropdown_create(right);
    lv_obj_set_width(s_model_dropdown, LV_PCT(100));
    lv_dropdown_set_options(s_model_dropdown, "(loading...)");
    lv_obj_set_style_bg_color(s_model_dropdown, UI_COLOR_PANEL, 0);
    lv_obj_set_style_text_color(s_model_dropdown, UI_COLOR_TEXT, 0);
    lv_obj_set_style_border_color(s_model_dropdown, UI_COLOR_BORDER, 0);

    // Main part: JP font for the selected text
    lv_obj_set_style_text_font(s_model_dropdown, ui_font_small(), LV_PART_MAIN);

    // Indicator (arrow symbol): keep Montserrat so the arrow renders correctly
    lv_obj_set_style_text_font(s_model_dropdown, &lv_font_montserrat_16, LV_PART_INDICATOR);

    // List popup: JP font
    lv_obj_t *list = lv_dropdown_get_list(s_model_dropdown);
    lv_obj_set_style_text_font(list, ui_font_small(), 0);

    lv_obj_add_event_cb(s_model_dropdown, on_model_picked, LV_EVENT_VALUE_CHANGED, NULL);

    make_field(right, TR(main_field_model), &s_model_lbl);
    make_field(right, TR(main_field_upper), &s_upper_lbl);
    make_field(right, TR(main_field_std), &s_std_lbl);
    make_field(right, TR(main_field_lower), &s_lower_lbl);

    lv_obj_t *spacer = lv_obj_create(right);
    lv_obj_set_size(spacer, LV_PCT(100), 4);
    lv_obj_set_style_bg_opa(spacer, 0, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *brow1 = lv_obj_create(right);
    lv_obj_set_size(brow1, LV_PCT(100), 48);
    lv_obj_set_style_bg_opa(brow1, 0, 0);
    lv_obj_set_style_border_width(brow1, 0, 0);
    lv_obj_set_style_pad_all(brow1, 0, 0);
    lv_obj_set_flex_flow(brow1, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(brow1, UI_PAD_S, 0);
    lv_obj_clear_flag(brow1, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *models_btn = lv_btn_create(brow1);
    lv_obj_set_flex_grow(models_btn, 1);
    lv_obj_set_height(models_btn, 48);
    lv_obj_set_style_bg_color(models_btn, UI_COLOR_PANEL_ALT, 0);
    lv_obj_set_style_radius(models_btn, UI_RADIUS_S, 0);
    lv_obj_add_event_cb(models_btn, on_models_btn, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ml = lv_label_create(models_btn);
    lv_label_set_text(ml, TR(main_btn_models));
    lv_obj_set_style_text_font(ml, ui_font_small(), 0);
    lv_obj_center(ml);

    lv_obj_t *setup_btn = lv_btn_create(brow1);
    lv_obj_set_flex_grow(setup_btn, 1);
    lv_obj_set_height(setup_btn, 48);
    lv_obj_set_style_bg_color(setup_btn, UI_COLOR_PANEL_ALT, 0);
    lv_obj_set_style_radius(setup_btn, UI_RADIUS_S, 0);
    lv_obj_add_event_cb(setup_btn, on_settings_btn, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(setup_btn);
    lv_label_set_text(sl, TR(main_btn_setup));
    lv_obj_set_style_text_font(sl, ui_font_small(), 0);
    lv_obj_center(sl);

    s_runstop_btn = lv_btn_create(right);
    lv_obj_set_size(s_runstop_btn, LV_PCT(100), 64);
    lv_obj_set_style_bg_color(s_runstop_btn, UI_COLOR_PASS_DARK, 0);
    lv_obj_set_style_radius(s_runstop_btn, UI_RADIUS, 0);
    lv_obj_add_event_cb(s_runstop_btn, on_runstop_btn, LV_EVENT_CLICKED, NULL);
    s_runstop_lbl = lv_label_create(s_runstop_btn);
    lv_label_set_text(s_runstop_lbl, TR(main_btn_ready));
    lv_obj_set_style_text_font(s_runstop_lbl, ui_font_large(), 0);
    lv_obj_set_style_text_color(s_runstop_lbl, UI_COLOR_TEXT, 0);
    lv_obj_center(s_runstop_lbl);

    /* Flex filler: eats all remaining height so NEW LOT sits at the bottom,
     * well away from READY/STOP. */
    lv_obj_t *fill = lv_obj_create(right);
    lv_obj_remove_style_all(fill);
    lv_obj_set_width(fill, LV_PCT(100));
    lv_obj_set_flex_grow(fill, 1);
    lv_obj_clear_flag(fill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* Current lot card - sits right above NEW LOT so the operator sees
     * which lot they are in before (and after) pressing it. */
    lv_obj_t *lot_card = lv_obj_create(right);
    lv_obj_set_size(lot_card, LV_PCT(100), 76);
    lv_obj_set_style_bg_color(lot_card, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_side(lot_card, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(lot_card, 4, 0);
    lv_obj_set_style_border_color(lot_card, UI_COLOR_LOW, 0); /* same amber as NEW LOT */
    lv_obj_set_style_radius(lot_card, UI_RADIUS_S, 0);
    lv_obj_set_style_pad_hor(lot_card, 10, 0);
    lv_obj_set_style_pad_ver(lot_card, 6, 0);
    lv_obj_clear_flag(lot_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lot_title = lv_label_create(lot_card);
    lv_obj_set_style_text_color(lot_title, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(lot_title, ui_font_small(), 0);
    lv_label_set_text(lot_title, TR(main_lot_current));
    lv_obj_align(lot_title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_lot_since_lbl = lv_label_create(lot_card);
    lv_obj_set_style_text_color(s_lot_since_lbl, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_lot_since_lbl, ui_font_small(), 0);
    lv_label_set_text(s_lot_since_lbl, "");
    lv_obj_align(s_lot_since_lbl, LV_ALIGN_TOP_RIGHT, 0, 0);

    s_lot_val_lbl = lv_label_create(lot_card);
    lv_obj_set_style_text_color(s_lot_val_lbl, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_lot_val_lbl, ui_font_large(), 0);
    lv_label_set_text(s_lot_val_lbl, "--");
    lv_obj_align(s_lot_val_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    {
        /* Don't toast an old result when the screen is (re)built */
        weight_state_snapshot_t snap0;
        weight_state_get_snapshot(&snap0);
        s_seen_lot_seq = snap0.lot_result_seq;
    }

    s_newlot_btn = lv_btn_create(right);
    lv_obj_set_size(s_newlot_btn, LV_PCT(100), 64);
    lv_obj_set_style_bg_color(s_newlot_btn, UI_COLOR_LOW_DARK, 0); /* amber, not green */
    lv_obj_set_style_radius(s_newlot_btn, UI_RADIUS, 0);
    lv_obj_add_event_cb(s_newlot_btn, on_newlot_btn, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = lv_label_create(s_newlot_btn);
    lv_label_set_text(nl, TR(main_btn_new_lot));
    lv_obj_set_style_text_font(nl, ui_font_large(), 0);
    lv_obj_set_style_text_color(nl, UI_COLOR_TEXT, 0);
    lv_obj_center(nl);

    /* ----- Weight card ----- */
    s_weight_card = lv_obj_create(scr);
    lv_obj_set_size(s_weight_card, UI_W - RIGHT_W - 2 * UI_PAD, UI_H - 44 - 2 * UI_PAD);
    lv_obj_align(s_weight_card, LV_ALIGN_TOP_LEFT, UI_PAD, 44 + UI_PAD);
    lv_obj_set_style_bg_color(s_weight_card, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_width(s_weight_card, 0, 0);
    lv_obj_set_style_radius(s_weight_card, UI_RADIUS, 0);
    lv_obj_clear_flag(s_weight_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *s_status_pill = lv_obj_create(s_weight_card);
    lv_obj_set_size(s_status_pill, 240, 32);
    lv_obj_align(s_status_pill, LV_ALIGN_TOP_LEFT, 14, 14);
    lv_obj_set_style_bg_color(s_status_pill, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_status_pill, LV_OPA_40, 0);
    lv_obj_set_style_border_width(s_status_pill, 0, 0);
    lv_obj_set_style_radius(s_status_pill, 16, 0);
    lv_obj_clear_flag(s_status_pill, LV_OBJ_FLAG_SCROLLABLE);

    s_status_pill_lbl = lv_label_create(s_status_pill);
    lv_obj_set_style_text_color(s_status_pill_lbl, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_status_pill_lbl, ui_font_small(), 0);
    lv_label_set_text(s_status_pill_lbl, "IDLE");
    lv_obj_center(s_status_pill_lbl);

    /* PASS counter pill - mirrors the status pill on the right */
    lv_obj_t *pass_pill = lv_obj_create(s_weight_card);
    lv_obj_set_size(pass_pill, LV_SIZE_CONTENT, 40);
    lv_obj_align(pass_pill, LV_ALIGN_TOP_RIGHT, -14, 10);
    lv_obj_set_style_bg_color(pass_pill, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(pass_pill, LV_OPA_40, 0);
    lv_obj_set_style_border_side(pass_pill, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(pass_pill, 4, 0);
    lv_obj_set_style_border_color(pass_pill, UI_COLOR_PASS, 0);
    lv_obj_set_style_radius(pass_pill, UI_RADIUS_S, 0);
    lv_obj_set_style_pad_hor(pass_pill, 14, 0);
    lv_obj_set_style_pad_ver(pass_pill, 0, 0);
    lv_obj_clear_flag(pass_pill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_pass_cnt_lbl = lv_label_create(pass_pill);
    lv_obj_set_style_text_color(s_pass_cnt_lbl, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_pass_cnt_lbl, ui_font_normal(), 0);
    lv_label_set_text(s_pass_cnt_lbl, "");
    lv_obj_center(s_pass_cnt_lbl);

    LV_FONT_DECLARE(lv_font_montserrat_bold_178);
    s_weight_lbl = lv_label_create(s_weight_card);
    lv_obj_set_style_text_color(s_weight_lbl, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_weight_lbl, &lv_font_montserrat_bold_178, 0);
    lv_label_set_text(s_weight_lbl, "0.0");
    lv_obj_center(s_weight_lbl);

    s_unit_lbl = lv_label_create(s_weight_card);
    lv_obj_set_style_text_color(s_unit_lbl, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_unit_lbl, ui_font_large(), 0);
    lv_label_set_text(s_unit_lbl, "g");
    lv_obj_align_to(s_unit_lbl, s_weight_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, 40);

    s_alert_banner = lv_obj_create(s_weight_card);
    lv_obj_set_size(s_alert_banner, LV_PCT(94), 50);
    lv_obj_align(s_alert_banner, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(s_alert_banner, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_alert_banner, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_alert_banner, 0, 0);
    lv_obj_set_style_radius(s_alert_banner, UI_RADIUS_S, 0);
    lv_obj_set_style_pad_hor(s_alert_banner, 14, 0);
    lv_obj_set_style_pad_ver(s_alert_banner, 8, 0);
    lv_obj_clear_flag(s_alert_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_alert_banner, LV_OBJ_FLAG_HIDDEN);

    s_alert_text = lv_label_create(s_alert_banner);
    lv_obj_set_style_text_color(s_alert_text, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_alert_text, ui_font_normal(), 0);
    lv_label_set_text(s_alert_text, "");
    lv_obj_align(s_alert_text, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *dismiss = lv_btn_create(s_alert_banner);
    lv_obj_set_size(dismiss, 100, 32);
    lv_obj_align(dismiss, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(dismiss, lv_color_hex(0x555555), 0);
    lv_obj_set_style_radius(dismiss, UI_RADIUS_S, 0);
    lv_obj_add_event_cb(dismiss, on_alert_dismiss, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dl = lv_label_create(dismiss);
    lv_label_set_text(dl, TR(main_btn_dismiss));
    lv_obj_center(dl);

    s_refresh_timer = lv_timer_create(refresh_cb, 100, NULL);
    lv_timer_pause(s_refresh_timer);

    return scr;
}
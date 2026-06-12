#include "ui_loading.h"
#include "ui_theme.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_x.h"
#include "esp_log.h"

static lv_obj_t *s_overlay;
static lv_obj_t *s_label;

void ui_loading_show(const char *message)
{
    bsp_display_lock(portMAX_DELAY);

    if (!s_overlay)
    {
        /* Full-screen semi-transparent dimmer */
        s_overlay = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_overlay);
        lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_overlay, LV_OPA_70, 0);
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
        /* Block all touches behind the overlay */
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);

        /* Centered card */
        lv_obj_t *card = lv_obj_create(s_overlay);
        lv_obj_set_size(card, 420, 200);
        lv_obj_center(card);
        lv_obj_set_style_bg_color(card, UI_COLOR_PANEL, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_radius(card, UI_RADIUS_S, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *spinner = lv_spinner_create(card);
        lv_obj_set_size(spinner, 60, 60);
        lv_obj_align(spinner, LV_ALIGN_TOP_MID, 0, 20);

        s_label = lv_label_create(card);
        lv_obj_set_style_text_color(s_label, UI_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(s_label, &lv_font_montserrat_18, 0);
        lv_label_set_long_mode(s_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_label, 380);
        lv_obj_set_style_text_align(s_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_label, LV_ALIGN_BOTTOM_MID, 0, -20);
    }

    lv_label_set_text(s_label, message ? message : "Loading...");
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

    bsp_display_unlock();
}

void ui_loading_set_text(const char *message)
{
    if (!s_label || !message)
        return;
    bsp_display_lock(portMAX_DELAY);
    lv_label_set_text(s_label, message);
    bsp_display_unlock();
}

void ui_loading_hide(void)
{
    if (!s_overlay)
        return;
    bsp_display_lock(portMAX_DELAY);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    bsp_display_unlock();
}
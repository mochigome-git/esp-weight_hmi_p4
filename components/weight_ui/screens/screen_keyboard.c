/**
 * @file screen_keyboard.c
 * @brief Placeholder - we use LVGL's built-in lv_keyboard widget inline
 *        in screen_wifi.c (and elsewhere as needed) rather than a dedicated
 *        keyboard "screen". This file exists so screen_keyboard_create() is
 *        a defined symbol (declared in weight_ui.h); weight_ui_init() calls
 *        it once and discards the result.
 *
 * If we ever need a full-screen text editor (e.g. for MQTT host entry),
 * replace this body with lv_keyboard + lv_textarea, similar to screen_wifi.
 */

#include "lvgl.h"
#include "ui_theme.h"
#include "weight_ui.h"

static void on_back(lv_event_t *e)
{
    weight_ui_show(UI_SCREEN_SETTINGS);
}

lv_obj_t *screen_keyboard_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_COLOR_BG, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_btn_create(scr);
    lv_obj_set_size(back, 100, 40);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, UI_PAD, UI_PAD);
    lv_obj_set_style_bg_color(back, UI_COLOR_PANEL_ALT, 0);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT "  back");
    lv_obj_center(bl);

    lv_obj_t *info = lv_label_create(scr);
    lv_obj_set_style_text_color(info, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_18, 0);
    lv_label_set_text(info, "Keyboard screen reserved for future use");
    lv_obj_center(info);

    return scr;
}
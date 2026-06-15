/**
 * @file screen_models.c
 * @brief Models list - tap row to edit, + to create new.
 *
 * Layout (1280 x 800):
 *  +------------------------------------------------------------+
 *  | ← back   Models (N)                          [+ New model] |
 *  +------------------------------------------------------------+
 *  | name              | lower | standard | upper |             |
 *  +------------------------------------------------------------+
 *  | 200g sachet       | 1150  | 1200     | 1250  | [edit] [x] |
 *  | 500g pouch        | 2900  | 3000     | 3100  | [edit] [x] |
 *  | ...                                                        |
 *  +------------------------------------------------------------+
 *
 * Tapping a row navigates to model_edit with that model preloaded.
 * Tapping + New navigates to model_edit blank.
 *
 * rebuild_list() is called from on_screen_load (SCREEN_LOADED event) rather
 * than from screen_models_create() so that weight_model_store is guaranteed
 * to be fully initialised (mutex created) before we query it.
 */

#include "lvgl.h"
#include "esp_log.h"
#include "ui_theme.h"
#include "weight_ui.h"
#include "weight_config.h"
#include "weight_model_store.h"
#include "weight_i18n.h"

static int s_edit_index = -1;
int screen_model_edit_get_index(void) { return s_edit_index; }
void screen_model_edit_set_index(int idx) { s_edit_index = idx; }

static lv_obj_t *s_list_container;
static lv_obj_t *s_title_lbl;

/* ---------------------------------------------------------------------------
 * Event handlers
 * --------------------------------------------------------------------------- */
static void on_back(lv_event_t *e)
{
    weight_ui_show(UI_SCREEN_MAIN);
}

static void on_new(lv_event_t *e)
{
    screen_model_edit_set_index(-1);
    weight_ui_show(UI_SCREEN_MODEL_EDIT);
}

static void on_row_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    screen_model_edit_set_index(idx);
    weight_ui_show(UI_SCREEN_MODEL_EDIT);
}

/* ---------------------------------------------------------------------------
 * List builder
 * --------------------------------------------------------------------------- */
static void rebuild_list(void)
{
    lv_obj_clean(s_list_container);

    const weight_config_t *cfg = weight_config_get();
    size_t n = weight_model_store_count();
    size_t alive = 0;
    char buf[32];

    /* Column header */
    lv_obj_t *hdr = lv_obj_create(s_list_container);
    lv_obj_set_size(hdr, LV_PCT(100), 28);
    lv_obj_set_style_bg_opa(hdr, 0, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_hor(hdr, 14, 0);
    lv_obj_set_style_pad_ver(hdr, 4, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    const char *cols[] = {
        TR(models_col_name),
        TR(models_col_lower),
        TR(models_col_standard),
        TR(models_col_upper),
        ""};
    static const int col_x[] = {0, 500, 700, 900, 1100};

    for (int i = 0; i < 5; i++)
    {
        lv_obj_t *l = lv_label_create(hdr);
        lv_obj_set_style_text_color(l, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_font(l, ui_font_small(), 0);
        lv_label_set_text(l, cols[i]);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, col_x[i], 0);
    }

    /* Data rows */
    for (size_t i = 0; i < n; i++)
    {
        weight_model_t m;
        if (!weight_model_store_get(i, &m))
            continue;
        if (m.deleted)
            continue;
        alive++;

        lv_obj_t *row = lv_obj_create(s_list_container);
        lv_obj_set_size(row, LV_PCT(100), 56);
        lv_obj_set_style_bg_color(row, UI_COLOR_PANEL, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, UI_RADIUS_S, 0);
        lv_obj_set_style_pad_hor(row, 14, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, on_row_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        /* Model name — user-entered, keep ui_font_normal for readability */
        lv_obj_t *name = lv_label_create(row);
        lv_obj_set_style_text_color(name, UI_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(name, ui_font_normal(), 0);
        lv_label_set_text(name, m.name);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, col_x[0], 0);

        snprintf(buf, sizeof(buf), "%.*f", cfg->decimal_places, m.lower_limit);
        lv_obj_t *lo = lv_label_create(row);
        lv_obj_set_style_text_color(lo, UI_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(lo, ui_font_normal(), 0);
        lv_label_set_text(lo, buf);
        lv_obj_align(lo, LV_ALIGN_LEFT_MID, col_x[1], 0);

        snprintf(buf, sizeof(buf), "%.*f", cfg->decimal_places, m.standard);
        lv_obj_t *st = lv_label_create(row);
        lv_obj_set_style_text_color(st, UI_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(st, ui_font_normal(), 0);
        lv_label_set_text(st, buf);
        lv_obj_align(st, LV_ALIGN_LEFT_MID, col_x[2], 0);

        snprintf(buf, sizeof(buf), "%.*f", cfg->decimal_places, m.upper_limit);
        lv_obj_t *up = lv_label_create(row);
        lv_obj_set_style_text_color(up, UI_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(up, ui_font_normal(), 0);
        lv_label_set_text(up, buf);
        lv_obj_align(up, LV_ALIGN_LEFT_MID, col_x[3], 0);
    }

    /* Title with count */
    char title[64];
    snprintf(title, sizeof(title), TR(models_title_fmt), (unsigned)alive);
    lv_label_set_text(s_title_lbl, title);

    /* Empty state */
    if (alive == 0)
    {
        lv_obj_t *empty = lv_label_create(s_list_container);
        lv_obj_set_style_text_color(empty, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_font(empty, ui_font_normal(), 0);
        lv_label_set_text(empty, TR(models_empty));
        lv_obj_set_style_pad_all(empty, 40, 0);
    }
}

static void on_screen_load(lv_event_t *e) { rebuild_list(); }

/* ---------------------------------------------------------------------------
 * Build screen
 * --------------------------------------------------------------------------- */
lv_obj_t *screen_models_create(void)
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
    s_title_lbl = lv_label_create(topbar);
    lv_obj_set_style_text_color(s_title_lbl, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_title_lbl, ui_font_large(), 0);
    lv_label_set_text(s_title_lbl, TR(models_title_fmt));
    lv_obj_align(s_title_lbl, LV_ALIGN_CENTER, 0, 0);

    /* New model button */
    lv_obj_t *new_btn = lv_btn_create(topbar);
    lv_obj_set_size(new_btn, 180, 40);
    lv_obj_align(new_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(new_btn, UI_COLOR_PASS_DARK, 0);
    lv_obj_set_style_radius(new_btn, UI_RADIUS_S, 0);
    lv_obj_add_event_cb(new_btn, on_new, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = lv_label_create(new_btn);
    lv_label_set_text(nl, TR(models_new));
    lv_obj_set_style_text_font(nl, ui_font_small(), 0);
    lv_obj_set_style_text_color(nl, UI_COLOR_TEXT, 0);
    lv_obj_center(nl);

    /* Scrollable list container */
    s_list_container = lv_obj_create(scr);
    lv_obj_set_size(s_list_container, UI_W, UI_H - 60);
    lv_obj_align(s_list_container, LV_ALIGN_TOP_LEFT, 0, 60);
    lv_obj_set_style_bg_color(s_list_container, UI_COLOR_BG, 0);
    lv_obj_set_style_border_width(s_list_container, 0, 0);
    lv_obj_set_style_radius(s_list_container, 0, 0);
    lv_obj_set_style_pad_all(s_list_container, UI_PAD, 0);
    lv_obj_set_flex_flow(s_list_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list_container, UI_PAD_S, 0);

    return scr;
}
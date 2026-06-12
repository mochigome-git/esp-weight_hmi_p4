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

/* Index of model the user picked (-1 = new model). Passed to edit screen. */
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
 * List builder - only called from on_screen_load or after edits, never
 * during screen construction, so the model store mutex always exists.
 * --------------------------------------------------------------------------- */
static void rebuild_list(void)
{
    lv_obj_clean(s_list_container);

    const weight_config_t *cfg = weight_config_get();
    size_t n = weight_model_store_count();
    size_t alive = 0;
    char buf[32];

    /* Column header row */
    lv_obj_t *hdr = lv_obj_create(s_list_container);
    lv_obj_set_size(hdr, LV_PCT(100), 28);
    lv_obj_set_style_bg_opa(hdr, 0, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_hor(hdr, 14, 0);
    lv_obj_set_style_pad_ver(hdr, 4, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    static const char *cols[] = {"name", "lower", "standard", "upper", ""};
    static const int col_x[] = {0, 500, 700, 900, 1100};
    for (int i = 0; i < 5; i++)
    {
        lv_obj_t *l = lv_label_create(hdr);
        lv_obj_set_style_text_color(l, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
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

        lv_obj_t *name = lv_label_create(row);
        lv_obj_set_style_text_color(name, UI_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_20, 0);
        lv_label_set_text(name, m.name);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, col_x[0], 0);

        snprintf(buf, sizeof(buf), "%.*f", cfg->decimal_places, m.lower_limit);
        lv_obj_t *lo = lv_label_create(row);
        lv_obj_set_style_text_color(lo, UI_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(lo, &lv_font_montserrat_18, 0);
        lv_label_set_text(lo, buf);
        lv_obj_align(lo, LV_ALIGN_LEFT_MID, col_x[1], 0);

        snprintf(buf, sizeof(buf), "%.*f", cfg->decimal_places, m.standard);
        lv_obj_t *st = lv_label_create(row);
        lv_obj_set_style_text_color(st, UI_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(st, &lv_font_montserrat_18, 0);
        lv_label_set_text(st, buf);
        lv_obj_align(st, LV_ALIGN_LEFT_MID, col_x[2], 0);

        snprintf(buf, sizeof(buf), "%.*f", cfg->decimal_places, m.upper_limit);
        lv_obj_t *up = lv_label_create(row);
        lv_obj_set_style_text_color(up, UI_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(up, &lv_font_montserrat_18, 0);
        lv_label_set_text(up, buf);
        lv_obj_align(up, LV_ALIGN_LEFT_MID, col_x[3], 0);
    }

    /* Update title with live count */
    char title[64];
    snprintf(title, sizeof(title), "Models (%u)", (unsigned)alive);
    lv_label_set_text(s_title_lbl, title);

    if (alive == 0)
    {
        lv_obj_t *empty = lv_label_create(s_list_container);
        lv_obj_set_style_text_color(empty, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_18, 0);
        lv_label_set_text(empty, "no models yet - tap + New model");
        lv_obj_set_style_pad_all(empty, 40, 0);
    }
}

/* Re-render when becoming visible so MQTT-synced changes show up.
 * This is the ONLY place rebuild_list() is called at construction time;
 * the store mutex is guaranteed to exist by the time this fires. */
static void on_screen_load(lv_event_t *e)
{
    rebuild_list();
}

/* ---------------------------------------------------------------------------
 * Build screen
 * --------------------------------------------------------------------------- */
lv_obj_t *screen_models_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_COLOR_BG, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Register load handler FIRST - rebuild_list fires lazily on display,
     * never during construction, so the model store is always ready. */
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

    lv_obj_t *back = lv_btn_create(topbar);
    lv_obj_set_size(back, 100, 40);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(back, UI_COLOR_PANEL_ALT, 0);
    lv_obj_set_style_radius(back, UI_RADIUS_S, 0);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT "  back");
    lv_obj_center(bl);

    s_title_lbl = lv_label_create(topbar);
    lv_obj_set_style_text_color(s_title_lbl, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_24, 0);
    lv_label_set_text(s_title_lbl, "Models");
    lv_obj_align(s_title_lbl, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *new_btn = lv_btn_create(topbar);
    lv_obj_set_size(new_btn, 160, 40);
    lv_obj_align(new_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(new_btn, UI_COLOR_PASS_DARK, 0);
    lv_obj_set_style_radius(new_btn, UI_RADIUS_S, 0);
    lv_obj_add_event_cb(new_btn, on_new, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = lv_label_create(new_btn);
    lv_label_set_text(nl, "+ New model");
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
    /* Scrollable - leave LV_OBJ_FLAG_SCROLLABLE set (default) */

    /* NOTE: do NOT call rebuild_list() here. It will be called by
     * on_screen_load when this screen first becomes active, by which point
     * weight_model_store_init() has already run in app_main. */

    return scr;
}
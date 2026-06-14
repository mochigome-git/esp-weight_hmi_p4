/**
 * @file screen_model_edit.c
 * @brief Model editor with on-screen numpad.
 *
 * Layout (1280 x 800):
 *  +------------------------------------------------------------+
 *  | ← back   New model / Edit model                            |
 *  +------------------------------+-----------------------------+
 *  | left panel - 4 fields:       | numpad:                     |
 *  | - name (text - uses kbd)     |  7  8  9                    |
 *  | - lower limit                |  4  5  6                    |
 *  | - standard (active)          |  1  2  3                    |
 *  | - upper limit                |  .  0  ⌫                   |
 *  |                              | cancel  clr  save           |
 *  | [delete model] (edit only)   |                             |
 *  +------------------------------+-----------------------------+
 */

#include "lvgl.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_random.h"
#include "ui_theme.h"
#include "weight_ui.h"
#include "weight_config.h"
#include "weight_model_store.h"
#include "weight_mqtt.h"

static const char *TAG = "scr_edit";

/* Index passed in from screen_models (-1 = new) */
int screen_model_edit_get_index(void);
void screen_model_edit_set_index(int idx);
static void do_show_models(void *unused);
static int validate_and_highlight(void);

/* Current edit buffer */
static weight_model_t s_buf;
static int s_active_field = 1; /* 0=name, 1=lower, 2=std, 3=upper */
/* NOTE: s_text_buf removed - was unused (numpad reads field values via
 * format_field_value() directly rather than a separate text buffer). */
static lv_obj_t *s_field_cards[4];
static lv_obj_t *s_field_value_lbls[4];
static lv_obj_t *s_title_lbl;
static lv_obj_t *s_delete_btn;
static lv_obj_t *s_status_lbl;

/* Raw text buffers being edited - one per numeric field (1=lower, 2=std, 3=upper) */
static char s_edit_text[4][32];

static void seed_edit_text_from_buf(void)
{
    /* When loading a model for edit, seed the strings from the float values */
    const weight_config_t *cfg = weight_config_get();
    s_edit_text[0][0] = '\0'; /* name handled separately */
    snprintf(s_edit_text[1], sizeof(s_edit_text[1]), "%.*f", cfg->decimal_places, s_buf.lower_limit);
    snprintf(s_edit_text[2], sizeof(s_edit_text[2]), "%.*f", cfg->decimal_places, s_buf.standard);
    snprintf(s_edit_text[3], sizeof(s_edit_text[3]), "%.*f", cfg->decimal_places, s_buf.upper_limit);
    /* If the seeded value is just "0" or "0.0" treat as empty so first digit replaces it */
    for (int i = 1; i < 4; i++)
    {
        float v = strtof(s_edit_text[i], NULL);
        if (v == 0.0f)
            s_edit_text[i][0] = '\0';
    }
}

static void format_field_value(int field, char *out, size_t out_len)
{
    if (field == 0)
    {
        snprintf(out, out_len, "%s", s_buf.name);
        return;
    }
    if (field >= 1 && field <= 3)
    {
        /* Show raw edit buffer if non-empty, else "0" */
        snprintf(out, out_len, "%s", s_edit_text[field][0] ? s_edit_text[field] : "0");
        return;
    }
    out[0] = '\0';
}

static void refresh_fields(void)
{
    for (int i = 0; i < 4; i++)
    {
        char tmp[32];
        format_field_value(i, tmp, sizeof(tmp));
        lv_label_set_text(s_field_value_lbls[i], tmp[0] ? tmp : "—");
    }
    /* Reset status color to warning, validate_and_highlight will override on success */
    lv_obj_set_style_text_color(s_status_lbl, UI_COLOR_HIGH, 0);
    validate_and_highlight();
}

/* Returns 0 if valid, otherwise sets s_status_lbl text and highlights bad fields. */
static int validate_and_highlight(void)
{
    float lo = strtof(s_edit_text[1], NULL);
    float std = strtof(s_edit_text[2], NULL);
    float up = strtof(s_edit_text[3], NULL);

    /* Reset to non-active default; refresh_fields will re-color the active one */
    lv_color_t normal = UI_COLOR_PANEL;
    lv_color_t active = UI_COLOR_PASS_DARK;
    lv_color_t bad = UI_COLOR_HIGH_DARK;

    for (int i = 0; i < 4; i++)
    {
        lv_obj_set_style_bg_color(s_field_cards[i],
                                  (i == s_active_field) ? active : normal, 0);
    }

    if (s_buf.name[0] == '\0')
    {
        lv_label_set_text(s_status_lbl, "name is empty");
        lv_obj_set_style_bg_color(s_field_cards[0], bad, 0);
        return -1;
    }
    if (lo > std)
    {
        lv_label_set_text(s_status_lbl, "lower must be <= standard");
        lv_obj_set_style_bg_color(s_field_cards[1], bad, 0);
        lv_obj_set_style_bg_color(s_field_cards[2], bad, 0);
        return -1;
    }
    if (std > up)
    {
        lv_label_set_text(s_status_lbl, "standard must be <= upper");
        lv_obj_set_style_bg_color(s_field_cards[2], bad, 0);
        lv_obj_set_style_bg_color(s_field_cards[3], bad, 0);
        return -1;
    }
    if (lo == 0 && std == 0 && up == 0)
    {
        lv_label_set_text(s_status_lbl, "set lower / standard / upper");
        return -1;
    }
    lv_label_set_text(s_status_lbl, LV_SYMBOL_OK "  ready to save");
    lv_obj_set_style_text_color(s_status_lbl, UI_COLOR_PASS, 0);
    return 0;
}

static void generate_uuid(char *out, size_t len)
{
    uint8_t r[16];
    esp_fill_random(r, sizeof(r));
    r[6] = (r[6] & 0x0f) | 0x40; // version 4
    r[8] = (r[8] & 0x3f) | 0x80; // variant
    snprintf(out, len,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
             r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
}

static void load_for_edit(void)
{
    memset(&s_buf, 0, sizeof(s_buf));
    int idx = screen_model_edit_get_index();
    if (idx >= 0)
    {
        weight_model_store_get((size_t)idx, &s_buf);
        lv_label_set_text(s_title_lbl, "Edit model");
        lv_obj_clear_flag(s_delete_btn, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        /* New model - generate a UUID-ish id from time and a counter */
        generate_uuid(s_buf.id, sizeof(s_buf.id));
        strncpy(s_buf.unit, "g", sizeof(s_buf.unit) - 1);
        lv_label_set_text(s_title_lbl, "New model");
        lv_obj_add_flag(s_delete_btn, LV_OBJ_FLAG_HIDDEN);
    }
    s_active_field = 0; // start on name field
    seed_edit_text_from_buf();
    refresh_fields();
}

/* ---------------------------------------------------------------------------
 * Numpad input
 * --------------------------------------------------------------------------- */
static void apply_numpad_digit(char c)
{
    if (s_active_field < 1 || s_active_field > 3)
        return;
    char *cur = s_edit_text[s_active_field];
    size_t l = strlen(cur);

    if (c == '.' && strchr(cur, '.'))
        return;             /* no double decimal */
    if (c == '.' && l == 0) /* leading "." becomes "0." */
    {
        cur[0] = '0';
        cur[1] = '.';
        cur[2] = '\0';
        refresh_fields();
        return;
    }
    if (l >= sizeof(s_edit_text[0]) - 1)
        return;

    cur[l] = c;
    cur[l + 1] = '\0';
    refresh_fields();
}

static void apply_backspace(void)
{
    if (s_active_field == 0)
    {
        size_t l = strlen(s_buf.name);
        if (l > 0)
            s_buf.name[l - 1] = '\0';
    }
    else if (s_active_field >= 1 && s_active_field <= 3)
    {
        size_t l = strlen(s_edit_text[s_active_field]);
        if (l > 0)
            s_edit_text[s_active_field][l - 1] = '\0';
    }
    refresh_fields();
}

static void apply_clear(void)
{
    if (s_active_field == 0)
        s_buf.name[0] = '\0';
    else if (s_active_field >= 1 && s_active_field <= 3)
        s_edit_text[s_active_field][0] = '\0';
    refresh_fields();
}

static void on_numpad_btn(lv_event_t *e)
{
    const char *txt = (const char *)lv_event_get_user_data(e);
    if (!txt)
        return;
    char c = txt[0];

    if (c == 'C')
    {
        apply_clear();
        return;
    }
    if (c == 'B')
    {
        apply_backspace();
        return;
    }
    if (c == 'X')
    {
        lv_async_call(do_show_models, NULL);
        return;
    }
    if (c == 'S')
    {
        s_buf.lower_limit = strtof(s_edit_text[1], NULL);
        s_buf.standard = strtof(s_edit_text[2], NULL);
        s_buf.upper_limit = strtof(s_edit_text[3], NULL);

        if (validate_and_highlight() != 0)
        {
            ESP_LOGW(TAG, "save rejected (see status)");
            return;
        }
        weight_model_store_upsert_local(&s_buf);
        ESP_LOGI(TAG, "save: post-upsert");

        // Push to Supabase via MQTT bridge
        weight_mqtt_publish_model_push(&s_buf);

        lv_async_call(do_show_models, NULL);
        return;
    }
    if (s_active_field == 0)
        return; /* name field needs text keyboard, not numpad */
    apply_numpad_digit(c);
}

static lv_obj_t *s_kbd_popup = NULL;
static lv_obj_t *s_kbd_ta = NULL;
static lv_obj_t *s_kbd = NULL;

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

static void on_kbd_ready(lv_event_t *e)
{
    if (!s_kbd_ta)
        return;
    const char *txt = lv_textarea_get_text(s_kbd_ta);
    if (txt)
        strlcpy(s_buf.name, txt, sizeof(s_buf.name));
    close_kbd_popup();
    s_active_field = 1; // auto-advance to lower limit
    refresh_fields();
}

static void on_kbd_cancel(lv_event_t *e)
{
    close_kbd_popup();
}

static void open_name_keyboard(void)
{
    if (s_kbd_popup)
        return;
    lv_obj_t *scr = lv_screen_active();

    s_kbd_popup = lv_obj_create(scr);
    lv_obj_set_size(s_kbd_popup, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_kbd_popup, 0, 0);
    lv_obj_set_style_bg_opa(s_kbd_popup, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(s_kbd_popup, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_kbd_popup, 0, 0);
    lv_obj_set_style_radius(s_kbd_popup, 0, 0);
    lv_obj_clear_flag(s_kbd_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_kbd_popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_kbd_popup);

    lv_obj_t *modal = lv_obj_create(s_kbd_popup);
    lv_obj_set_size(modal, 700, 160);
    lv_obj_align(modal, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(modal, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, UI_RADIUS_S, 0);
    lv_obj_set_style_pad_all(modal, UI_PAD, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(modal);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_label_set_text(title, "Model name");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_kbd_ta = lv_textarea_create(modal);
    lv_obj_set_size(s_kbd_ta, 660, 52);
    lv_obj_align(s_kbd_ta, LV_ALIGN_TOP_LEFT, 0, 34);
    lv_textarea_set_one_line(s_kbd_ta, true);
    lv_textarea_set_max_length(s_kbd_ta, sizeof(s_buf.name) - 1);
    lv_textarea_set_placeholder_text(s_kbd_ta, "e.g. 200g sachet");
    lv_textarea_set_text(s_kbd_ta, s_buf.name);
    lv_obj_set_style_text_font(s_kbd_ta, &lv_font_montserrat_18, 0);
    lv_obj_set_style_bg_color(s_kbd_ta, UI_COLOR_PANEL_ALT, 0);
    lv_obj_set_style_border_width(s_kbd_ta, 1, 0);
    lv_obj_set_style_border_color(s_kbd_ta, UI_COLOR_ACCENT, 0);

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

    lv_obj_t *save_btn = lv_btn_create(modal);
    lv_obj_set_size(save_btn, 140, 44);
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(save_btn, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(save_btn, UI_RADIUS_S, 0);
    lv_obj_set_style_border_width(save_btn, 0, 0);
    lv_obj_add_event_cb(save_btn, on_kbd_ready, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(save_btn);
    lv_label_set_text(sl, LV_SYMBOL_OK "  OK");
    lv_obj_set_style_text_color(sl, UI_COLOR_TEXT, 0);
    lv_obj_center(sl);

    s_kbd = lv_keyboard_create(s_kbd_popup);
    lv_obj_set_size(s_kbd, LV_PCT(100), 330);
    lv_obj_align(s_kbd, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_kbd, s_kbd_ta);
    lv_keyboard_set_mode(s_kbd, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_event_cb(s_kbd, on_kbd_ready, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_kbd, on_kbd_cancel, LV_EVENT_CANCEL, NULL);
}

static void do_show_models(void *unused)
{
    (void)unused;
    weight_ui_show(UI_SCREEN_MODELS);
}

static void on_delete(lv_event_t *e)
{
    ESP_LOGI(TAG, "delete clicked, id=%s", s_buf.id);
    if (s_buf.id[0] == '\0')
        return;
    weight_mqtt_publish_model_delete(s_buf.id);
    weight_model_store_delete_local(s_buf.id);
    memset(&s_buf, 0, sizeof(s_buf));
    screen_model_edit_set_index(-1);
    weight_ui_show(UI_SCREEN_MODELS);
}

static void on_screen_load(lv_event_t *e)
{
    printf("EDIT SCREEN LOADED, delete_btn=%p\n", s_delete_btn);
    fflush(stdout);
    load_for_edit();
}

static void on_field_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "field click idx=%d", idx);
    s_active_field = idx;
    refresh_fields();
    if (s_active_field == 0)
    {
        open_name_keyboard();
    }
}

/* ---------------------------------------------------------------------------
 * Build screen
 * --------------------------------------------------------------------------- */
static lv_obj_t *make_edit_field(lv_obj_t *parent, int idx, const char *label)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, LV_PCT(100), 70);
    lv_obj_set_style_bg_color(card, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, UI_RADIUS_S, 0);
    lv_obj_set_style_pad_hor(card, 14, 0);
    lv_obj_set_style_pad_ver(card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, on_field_click, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    lv_obj_t *l = lv_label_create(card);
    lv_obj_set_style_text_color(l, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_label_set_text(l, label);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *v = lv_label_create(card);
    lv_obj_set_style_text_color(v, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_32, 0);
    lv_label_set_text(v, "—");
    lv_obj_align(v, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    s_field_cards[idx] = card;
    s_field_value_lbls[idx] = v;
    return card;
}

/* NOTE: make_key() has been removed entirely. It was defined but never called
 * (the numpad is built with inline loops below). Keeping dead code with
 * -Werror=unused-function would fail the build. */

lv_obj_t *screen_model_edit_create(void)
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

    s_title_lbl = lv_label_create(topbar);
    lv_obj_set_style_text_color(s_title_lbl, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_24, 0);
    lv_label_set_text(s_title_lbl, "New model");
    lv_obj_align(s_title_lbl, LV_ALIGN_CENTER, 0, 0);

    /* Left column - fields */
    lv_obj_t *left = lv_obj_create(scr);
    lv_obj_set_size(left, 560, UI_H - 60);
    lv_obj_align(left, LV_ALIGN_TOP_LEFT, 0, 60);
    lv_obj_set_style_bg_color(left, UI_COLOR_BG, 0);
    lv_obj_set_style_border_width(left, 0, 0);
    lv_obj_set_style_pad_all(left, UI_PAD, 0);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, UI_PAD_S, 0);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    make_edit_field(left, 0, "model name");
    make_edit_field(left, 1, "lower limit");
    make_edit_field(left, 2, "standard");
    make_edit_field(left, 3, "upper limit");

    /* Delete button - placed as direct child of screen with absolute position
     * so flex layout can't crush or push it off-screen. */
    s_delete_btn = lv_btn_create(scr);
    lv_obj_set_size(s_delete_btn, 520, 60);
    lv_obj_set_pos(s_delete_btn, UI_PAD, UI_H - 60 - UI_PAD); // bottom-left of screen
    lv_obj_set_style_bg_color(s_delete_btn, UI_COLOR_HIGH_DARK, 0);
    lv_obj_set_style_radius(s_delete_btn, UI_RADIUS_S, 0);
    lv_obj_set_style_border_width(s_delete_btn, 0, 0);
    lv_obj_add_event_cb(s_delete_btn, on_delete, LV_EVENT_CLICKED, NULL);
    lv_obj_move_foreground(s_delete_btn); // ensure it's on top of everything
    lv_obj_t *dl = lv_label_create(s_delete_btn);
    lv_obj_set_style_text_color(dl, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(dl, &lv_font_montserrat_20, 0);
    lv_label_set_text(dl, LV_SYMBOL_TRASH "  delete model");
    lv_obj_center(dl);
    lv_obj_add_flag(s_delete_btn, LV_OBJ_FLAG_HIDDEN);

    /* Status / hint line at bottom of left column */
    s_status_lbl = lv_label_create(left);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_status_lbl, UI_COLOR_HIGH, 0);
    lv_label_set_text(s_status_lbl, "");

    /* Right column - numpad */
    lv_obj_t *right = lv_obj_create(scr);
    lv_obj_set_size(right, UI_W - 560, UI_H - 60);
    lv_obj_align(right, LV_ALIGN_TOP_RIGHT, 0, 60);
    lv_obj_set_style_bg_color(right, UI_COLOR_BG, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, UI_PAD, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    /* 3-column x 5-row grid (4 numpad rows + 1 action row) */
    static int32_t col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static int32_t row_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(right, col_dsc, row_dsc);
    lv_obj_set_layout(right, LV_LAYOUT_GRID);
    lv_obj_set_style_pad_gap(right, 6, 0);

    static const char *labels[] = {
        "7",
        "8",
        "9",
        "4",
        "5",
        "6",
        "1",
        "2",
        "3",
        ".",
        "0",
        LV_SYMBOL_BACKSPACE,
    };
    static const char *send_codes[] = {
        "7",
        "8",
        "9",
        "4",
        "5",
        "6",
        "1",
        "2",
        "3",
        ".",
        "0",
        "B",
    };

    /* Numpad digit/symbol keys - rows 0-3 */
    int idx = 0;
    for (int row = 0; row < 4; row++)
    {
        for (int col = 0; col < 3; col++)
        {
            lv_obj_t *btn = lv_btn_create(right);
            lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_STRETCH, col, 1,
                                 LV_GRID_ALIGN_STRETCH, row, 1);
            lv_obj_set_style_bg_color(btn, UI_COLOR_PANEL_ALT, 0);
            lv_obj_set_style_radius(btn, UI_RADIUS_S, 0);
            lv_obj_add_event_cb(btn, on_numpad_btn, LV_EVENT_CLICKED,
                                (void *)send_codes[idx]);

            lv_obj_t *l = lv_label_create(btn);
            lv_obj_set_style_text_color(l, UI_COLOR_TEXT, 0);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_32, 0);
            lv_label_set_text(l, labels[idx]);
            lv_obj_center(l);
            idx++;
        }
    }

    /* Bottom action row (row 4): cancel, clr, save */
    typedef struct
    {
        const char *txt;
        const char *send;
        lv_color_t bg;
    } action_t;
    action_t actions[3] = {
        {"cancel", "X", UI_COLOR_HIGH_DARK},
        {"clr", "C", UI_COLOR_PANEL_ALT},
        {"save", "S", UI_COLOR_PASS_DARK},
    };

    for (int col = 0; col < 3; col++)
    {
        lv_obj_t *btn = lv_btn_create(right);
        lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_STRETCH, col, 1,
                             LV_GRID_ALIGN_STRETCH, 4, 1);
        lv_obj_set_style_bg_color(btn, actions[col].bg, 0);
        lv_obj_set_style_radius(btn, UI_RADIUS_S, 0);
        lv_obj_add_event_cb(btn, on_numpad_btn, LV_EVENT_CLICKED,
                            (void *)actions[col].send);
        lv_obj_t *l = lv_label_create(btn);
        lv_obj_set_style_text_color(l, UI_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
        lv_label_set_text(l, actions[col].txt);
        lv_obj_center(l);
    }

    printf("EDIT SCREEN CREATED, delete_btn=%p\n", s_delete_btn);
    fflush(stdout);

    return scr;
}
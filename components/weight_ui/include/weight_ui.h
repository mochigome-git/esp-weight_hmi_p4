/**
 * @file weight_ui.h
 * @brief LVGL UI subsystem.
 *
 * - Initialises MIPI-DSI panel + GT911 touch
 * - Starts LVGL task
 * - Registers all screens (main, models list, model edit, settings, wifi setup)
 * - Provides a small show_screen() API
 */

#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        UI_SCREEN_MAIN = 0,
        UI_SCREEN_MODELS,
        UI_SCREEN_MODEL_EDIT,
        UI_SCREEN_SETTINGS,
        UI_SCREEN_WIFI,
        UI_SCREEN_MQTT,
        UI_SCREEN_COUNT /* sentinel — always last, used for array sizing */
    } ui_screen_id_t;

    /* Initialise display hardware + LVGL + all screens */
    esp_err_t weight_ui_init(void);

    /* Switch to the given screen (safe to call from any task) */
    void weight_ui_show(ui_screen_id_t id);

    void weight_ui_destroy_screen(ui_screen_id_t id);

    void screen_main_cleanup(void);

    /* ---------------------------------------------------------------------------
     * Screen builder forward declarations
     * Each returns the screen root lv_obj_t * (a screen created with
     * lv_obj_create(NULL)).  Called once from weight_ui_init() under the
     * LVGL lock; after that the pointers are cached in s_screens[].
     * --------------------------------------------------------------------------- */
    lv_obj_t *screen_main_create(void);
    lv_obj_t *screen_models_create(void);
    lv_obj_t *screen_model_edit_create(void);
    lv_obj_t *screen_settings_create(void);
    lv_obj_t *screen_wifi_create(void);
    lv_obj_t *screen_keyboard_create(void); /* placeholder, inline kbd used elsewhere */
    lv_obj_t *screen_mqtt_create(void);

#ifdef __cplusplus
}
#endif
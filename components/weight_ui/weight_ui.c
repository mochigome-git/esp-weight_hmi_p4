/**
 * @file weight_ui.c
 *
 * UI subsystem - initialises the Waveshare BSP (display, touch, LVGL adapter)
 * and registers our screens.
 *
 * The BSP does the heavy lifting:
 *   - bsp_display_start() configures MIPI-DSI bus, JD9365 panel, GT911 touch,
 *     and starts the LVGL task with proper tearing-avoid mode.
 *   - bsp_display_lock() / unlock() are the mutex pair for all LVGL calls
 *     from outside the LVGL task itself.
 *
 * IMPORTANT: do NOT call lv_init() ourselves - the BSP's
 * esp_lv_adapter_init() does that. Calling lv_init twice can cause subtle
 * memory corruption.
 */

#include <stdio.h>
#include "weight_ui.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_x.h"
#include "bsp/display.h"
#include "lvgl.h"

static const char *TAG = "ui";

/* One slot per screen id - sized by the sentinel enum value */
static lv_obj_t *s_screens[UI_SCREEN_COUNT];

esp_err_t weight_ui_init(void)
{
    ESP_LOGI(TAG, "starting BSP display...");

    /* Use bsp_display_start_with_config() so we can set rotation 90°.
     * The 10.1" panel is natively portrait (800x1280); rotating 90° gives
     * us landscape (1280x800) which matches our screen designs. */
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_270,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags = {
            .swap_xy = 1,
            .mirror_x = 0,
            .mirror_y = 1,
        },
    };
    lv_display_t *disp = bsp_display_start_with_config(&cfg);
    if (!disp)
    {
        ESP_LOGE(TAG, "bsp_display_start_with_config failed");
        return ESP_FAIL;
    }

    /* Give the LVGL task time to fully start before we try the lock */
    vTaskDelay(pdMS_TO_TICKS(100));

    bsp_display_brightness_set(80);

    /* Build ONLY the main screen up-front. The other four (models, edit,
     * settings, wifi) are built lazily the first time the user navigates
     * to them - see weight_ui_show() below.
     *
     * Why: building all 5 screens at boot consumes enough internal SRAM
     * that WiFi can't allocate its rx/tx buffers and crashes. Lazy
     * creation defers ~80% of the UI memory until after WiFi + MQTT have
     * grabbed what they need. */
    if (bsp_display_lock(pdMS_TO_TICKS(1000)) != ESP_OK)
    {
        ESP_LOGE(TAG, "bsp_display_lock failed");
        return ESP_FAIL;
    }

    s_screens[UI_SCREEN_MAIN] = screen_main_create();
    if (s_screens[UI_SCREEN_MAIN])
    {
        lv_screen_load(s_screens[UI_SCREEN_MAIN]);
    }

    bsp_display_unlock();

    ESP_LOGI(TAG, "UI init: main screen ready (others built on demand)");
    return ESP_OK;
}

void weight_ui_show(ui_screen_id_t id)
{
    if (id >= UI_SCREEN_COUNT)
        return;

    if (bsp_display_lock(pdMS_TO_TICKS(1000)) != ESP_OK)
    {
        ESP_LOGE(TAG, "weight_ui_show: lock failed");
        return;
    }

    /* Lazy-create the requested screen if we haven't built it yet */
    if (!s_screens[id])
    {
        ESP_LOGI(TAG, "lazy-building screen %d", id);
        switch (id)
        {
        case UI_SCREEN_MAIN:
            s_screens[id] = screen_main_create();
            break;
        case UI_SCREEN_MODELS:
            s_screens[id] = screen_models_create();
            break;
        case UI_SCREEN_MODEL_EDIT:
            s_screens[id] = screen_model_edit_create();
            break;
        case UI_SCREEN_SETTINGS:
            s_screens[id] = screen_settings_create();
            break;
        case UI_SCREEN_WIFI:
            s_screens[id] = screen_wifi_create();
            break;
        case UI_SCREEN_MQTT:
            s_screens[id] = screen_mqtt_create();
            break;
        default:
            break;
        }
    }

    if (s_screens[id])
    {
        lv_screen_load(s_screens[id]);
    }

    bsp_display_unlock();
}

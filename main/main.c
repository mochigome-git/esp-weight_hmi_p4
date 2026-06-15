/**
 * @file main.c
 * @brief ESP32-P4 Weighing Scale HMI - entry point
 *
 * Hardware: Waveshare ESP32-P4-WIFI6-Touch-LCD-10.1
 * - 1280 x 800 MIPI-DSI touch panel
 * - ES8311 audio codec + onboard speaker
 * - ESP32-C6 WiFi co-processor (via SDIO)
 * - 32MB flash, 32MB PSRAM
 *
 * Boot sequence:
 *   1. NVS init (config + models)
 *   2. SPIFFS mount (audio WAVs)
 *   3. weight_state init (shared struct + mutex)
 *   4. weight_config load
 *   5. weight_audio init (ES8311, preload WAVs to PSRAM)
 *   6. weight_ui start (LVGL, MIPI-DSI, touch)
 *   7. weight_wifi connect (saved creds or stays in setup mode)
 *   8. weight_mqtt connect (after WiFi up)
 *   9. weight_model_store load from NVS + request sync from broker
 *  10. weight_uart task start (RS232 reader)
 *  11. weight_evaluator task start (stability + pass/high/low)
 *
 * Always boots into IDLE - operator must press 'Ready' to start publishing.
 */

#include <stdio.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include <dirent.h>

#include "weight_time.h"
#include "weight_state.h"
#include "weight_config.h"
#include "weight_audio.h"
#include "weight_ui.h"
#include "weight_wifi.h"
#include "weight_mqtt.h"
#include "weight_uart.h"
#include "weight_evaluator.h"
#include "weight_model_store.h"
#include "weight_i18n.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_x.h"

/* =============================================================================
 * Hardcoded tenant identifiers (per agreement - keep at top for easy edits)
 *
 * TENANT_ID    : tenant UUID from Supabase, used in JSON payloads + DB foreign keys
 * TENANT_SHORT : short slug for MQTT topics, e.g. "gim", "gcl", "acme"
 *                Topics become {TENANT_SHORT}/{device_id}/... to keep them short
 *                and human-readable on the broker.
 * Replace both before flashing.
 * ============================================================================= */

static const char *TAG = "main";

void app_main(void)
{
    /* 1. NVS */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* 2. SPIFFS */
    ESP_ERROR_CHECK(bsp_spiffs_mount());
    ESP_LOGI("debug", "listing /spiffs contents:");
    DIR *d = opendir("/spiffs");
    if (d)
    {
        struct dirent *e;
        while ((e = readdir(d)) != NULL)
        {
            ESP_LOGI("debug", "  found: %s", e->d_name);
        }
        closedir(d);
    }
    else
    {
        ESP_LOGE("debug", "cannot open /spiffs - mount failed or wrong path");
    }

    /* 3. Shared state */
    ESP_ERROR_CHECK(weight_state_init("", ""));

    /* 4. Config */
    ESP_ERROR_CHECK(weight_config_init());
    weight_i18n_init(weight_config_get_lang());

    const weight_config_t *cfg = weight_config_get();
    weight_state_set_tenant(cfg->tenant_id, cfg->tenant_short);

    /* 5. Audio */
    ESP_ERROR_CHECK(weight_audio_init());

    /* 6. UI + BSP (this internally inits esp-hosted via SDIO to C6) */
    ESP_ERROR_CHECK(weight_ui_init());

    /* 7. Wait for esp-hosted bridge to fully settle after BSP init */
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* 8. WiFi - NOW after BSP/esp-hosted is ready */
    ESP_ERROR_CHECK(weight_wifi_init());

    /* 8b. SNTP — safe to start here, will retry until WiFi gets IP */
    weight_time_init();

    /* 9. MQTT */
    ESP_ERROR_CHECK(weight_mqtt_init());

    /* 10. Model store */
    ESP_ERROR_CHECK(weight_model_store_init());

    /* 11. UART task */
    ESP_ERROR_CHECK(weight_uart_start());

    /* 12. Evaluator task */
    ESP_ERROR_CHECK(weight_evaluator_start());
}
/**
 * @file weight_ui_watchdog.c
 *
 * UI freeze guard + heap logger.
 *
 * Why this exists
 *   The system task watchdog does NOT watch the LVGL task, and
 *   CONFIG_ESP_TASK_WDT_PANIC was off, so when the UI stalled (LVGL assert
 *   loop on a failed malloc, a stuck flush, a blocked lock...) the panel just
 *   stayed frozen forever while MQTT/UART kept running in the background.
 *
 * How it works
 *   - An lv_timer inside the LVGL task stamps s_alive_us every second.
 *     If the LVGL task is alive, the stamp is fresh.
 *   - An esp_timer (independent of LVGL) checks the stamp every 5 s.
 *     If it is older than UI_STALL_LIMIT_US, we log the heap state and
 *     esp_restart(). The device boots back into IDLE like a normal power-on.
 *   - Every 60 s it logs internal / PSRAM heap so a slow leak or
 *     fragmentation is visible on the serial console.
 */

#include "weight_ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_x.h"
#include "lvgl.h"

static const char *TAG = "ui_wdt";

#define ALIVE_PERIOD_MS 1000
#define CHECK_PERIOD_US (5ULL * 1000000ULL)
#define UI_STALL_LIMIT_US (20ULL * 1000000ULL) /* 20 s without an LVGL tick = frozen */
#define HEAP_LOG_EVERY_CHECKS 12               /* 12 x 5 s = 60 s */

static volatile int64_t s_alive_us;
static esp_timer_handle_t s_check_timer;
static uint32_t s_check_count;

void weight_ui_log_heap(const char *why)
{
    ESP_LOGI(TAG, "[heap %s] int free=%u min=%u largest=%u | psram free=%u largest=%u",
             why,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

/* Runs inside the LVGL task */
static void alive_cb(lv_timer_t *t)
{
    (void)t;
    s_alive_us = esp_timer_get_time();
}

/* Runs in the esp_timer task - must not block */
static void check_cb(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();
    int64_t age = now - s_alive_us;

    if (++s_check_count % HEAP_LOG_EVERY_CHECKS == 0)
        weight_ui_log_heap("periodic");

    if (age > (int64_t)UI_STALL_LIMIT_US)
    {
        ESP_LOGE(TAG, "LVGL task stalled for %lld ms - restarting", (long long)(age / 1000));
        weight_ui_log_heap("stall");
        esp_restart();
    }
}

esp_err_t weight_ui_watchdog_start(void)
{
    if (s_check_timer)
        return ESP_OK;

    s_alive_us = esp_timer_get_time();

    if (bsp_display_lock(1000) != ESP_OK)
    {
        ESP_LOGE(TAG, "lock failed - watchdog not started");
        return ESP_ERR_TIMEOUT;
    }
    lv_timer_t *t = lv_timer_create(alive_cb, ALIVE_PERIOD_MS, NULL);
    bsp_display_unlock();
    if (!t)
        return ESP_ERR_NO_MEM;

    const esp_timer_create_args_t args = {
        .callback = check_cb,
        .name = "ui_wdt",
    };
    esp_err_t err = esp_timer_create(&args, &s_check_timer);
    if (err != ESP_OK)
        return err;
    err = esp_timer_start_periodic(s_check_timer, CHECK_PERIOD_US);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "UI watchdog armed (stall limit %llu s)",
                 (unsigned long long)(UI_STALL_LIMIT_US / 1000000ULL));
        weight_ui_log_heap("boot");
    }
    return err;
}

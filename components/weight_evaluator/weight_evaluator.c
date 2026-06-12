/**
 * @file weight_evaluator.c
 */

#include <math.h>
#include <string.h>
#include "weight_evaluator.h"
#include "weight_uart.h"
#include "weight_state.h"
#include "weight_config.h"
#include "weight_audio.h"
#include "weight_mqtt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

static const char *TAG = "eval";

#define WINDOW_MAX 32

static float win[WINDOW_MAX];
static uint8_t win_n = 0;
static uint8_t win_idx = 0;

static float s_last_window_mean = 0.0f;
static bool s_have_last_mean = false;
static bool published_for_current_placement = false;
static weight_status_t s_last_published_status = WEIGHT_STATUS_NONE;

static void win_push(float v, uint8_t window_size)
{
    if (window_size > WINDOW_MAX)
        window_size = WINDOW_MAX;
    win[win_idx] = v;
    win_idx = (win_idx + 1) % window_size;
    if (win_n < window_size)
        win_n++;
}

static void win_reset(void)
{
    win_n = 0;
    win_idx = 0;
    s_have_last_mean = false;
}

static bool window_is_stable(uint8_t window_size, float tolerance)
{
    if (win_n < window_size)
        return false;

    float sum = 0.0f;
    for (uint8_t i = 0; i < win_n; i++)
        sum += win[i];
    float mean = sum / win_n;

    for (uint8_t i = 0; i < win_n; i++)
    {
        if (fabsf(win[i] - mean) > tolerance)
            return false;
    }

    if (s_have_last_mean && fabsf(mean - s_last_window_mean) > tolerance)
    {
        s_last_window_mean = mean;
        return false;
    }

    s_last_window_mean = mean;
    s_have_last_mean = true;
    return true;
}

static weight_status_t evaluate_limits(float v, const weight_active_model_t *m)
{
    if (!m->valid)
        return WEIGHT_STATUS_NONE;
    if (v > m->upper_limit)
        return WEIGHT_STATUS_HIGH;
    if (v < m->lower_limit)
        return WEIGHT_STATUS_LOW;
    return WEIGHT_STATUS_PASS;
}

static void eval_task(void *arg)
{
    ESP_LOGI(TAG, "evaluator started");

    /* Rate limiter: process at most 4 samples/sec (one per 250 ms) */
    static int64_t last_accepted_us = 0;
    const int64_t INTERVAL_US = 250000; /* 250 ms = 4 Hz */

    while (1)
    {
        weight_uart_sample_t s;
        if (!weight_uart_take(&s, portMAX_DELAY))
            continue;

        /* --- rate gate --- */
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_accepted_us < INTERVAL_US)
            continue; /* too soon — drop this sample */
        last_accepted_us = now_us;
        /* --- end rate gate --- */

        const weight_config_t *cfg = weight_config_get();
        weight_state_snapshot_t snap;
        weight_state_get_snapshot(&snap);

        /* Zero — full reset */
        if (s.value <= cfg->zero_threshold)
        {
            win_reset();
            published_for_current_placement = false;
            s_last_published_status = WEIGHT_STATUS_NONE;
            weight_state_update_reading(s.value, false, WEIGHT_STATUS_NONE, s.ts);
            continue;
        }

        /* Value is above zero — check if it moved away from current window */
        if (win_n > 0)
        {
            float sum = 0.0f;
            for (uint8_t i = 0; i < win_n; i++)
                sum += win[i];
            float cur_mean = sum / win_n;

            if (fabsf(s.value - cur_mean) > cfg->stability_tolerance)
            {
                /* Value is changing — reset window but DO NOT rearm gate.
                 * If we already published, keep the gate locked until zero. */
                win_n = 0;
                win_idx = 0;
                win_reset();
            }
        }

        win_push(s.value, cfg->stability_window);
        bool stable = window_is_stable(cfg->stability_window, cfg->stability_tolerance);

        weight_status_t status = WEIGHT_STATUS_NONE;
        if (snap.model.valid)
            status = evaluate_limits(s.value, &snap.model);

        /* Display status — retain last published status until zero */
        weight_status_t display_status = published_for_current_placement
                                             ? s_last_published_status
                                             : status;

        weight_state_update_reading(s.value, stable, display_status, s.ts);

        bool fresh_stable_event = stable && !published_for_current_placement;

        if (fresh_stable_event)
        {
            ESP_LOGI(TAG, "stable: %.3f %s  status=%d",
                     s.value, weight_config_unit_str(cfg->unit), status);

            if (snap.mode == WEIGHT_MODE_RUN && status != WEIGHT_STATUS_NONE)
            {
                switch (status)
                {
                case WEIGHT_STATUS_PASS:
                    weight_audio_play(WEIGHT_AUDIO_PASS);
                    break;
                case WEIGHT_STATUS_LOW:
                    weight_audio_play(WEIGHT_AUDIO_LOW);
                    break;
                case WEIGHT_STATUS_HIGH:
                    weight_audio_play(WEIGHT_AUDIO_HIGH);
                    break;
                default:
                    break;
                }
            }

            if (snap.mode == WEIGHT_MODE_RUN && snap.model.valid)
                weight_mqtt_publish_reading(s.value, status, s.ts);

            s_last_published_status = status; /* remember what we showed */
            published_for_current_placement = true;
        }
    }
}

esp_err_t weight_evaluator_start(void)
{
    win_reset();
    BaseType_t r = xTaskCreatePinnedToCore(eval_task, "eval", 4096, NULL, 9, NULL, 0);
    return (r == pdPASS) ? ESP_OK : ESP_FAIL;
}
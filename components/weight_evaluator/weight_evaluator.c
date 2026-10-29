/**
 * @file weight_evaluator.c
 *
 * Pipeline per sample:
 *   UART sample -> 250 ms bucket average -> zero check -> "moved" check ->
 *   sliding window -> stability -> limits -> state/audio/alert/MQTT
 *
 * Placement gate ("one publish per placement"):
 *   - Armed while the pan is empty (<= max(zero_threshold, noise floor)).
 *   - The first stable reading in RUN mode publishes, beeps, and (for
 *     HIGH/LOW) raises the sticky alert ONCE. The gate then locks until the
 *     pan returns to zero.
 *   - Readings taken in IDLE never lock the gate, so an item already on the
 *     pan when READY is pressed is still published once it is stable.
 *   - If the weight moves (> tolerance) away from the published value while
 *     the gate is locked, the display follows the live value/status instead
 *     of keeping the stale published result. Set EVAL_REARM_ON_CHANGE=1 to
 *     also publish again once the new weight stabilises.
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

#ifndef EVAL_REARM_ON_CHANGE
#define EVAL_REARM_ON_CHANGE 0
#endif

/* Empty-pan noise floor, in grams. The scale (used in kg) drifts around
 * zero by about 0.01 kg (10 g); with zero_threshold at its 0.0 default
 * those drift readings were stable and > 0, so they were published as real
 * placements. Anything at or below max(zero_threshold, this floor) is
 * treated as an empty pan: never published, and it re-arms the gate.
 * 100 g = 0.1 kg: 10x the drift, far below any real item (~7-10 kg). */
#ifndef EVAL_NOISE_FLOOR_G
#define EVAL_NOISE_FLOOR_G 100.0f
#endif

#define WINDOW_MAX 32
#define BUCKET_US 250000 /* 4 Hz evaluation rate */

static float win[WINDOW_MAX];
static uint8_t win_n = 0;
static uint8_t win_idx = 0;
static uint8_t win_size_used = 0;

static float s_last_window_mean = 0.0f;
static bool s_have_last_mean = false;

static bool s_gate_locked = false; /* published for the current placement */
static float s_published_value = 0.0f;
static weight_status_t s_published_status = WEIGHT_STATUS_NONE;

static void win_reset(void)
{
    win_n = 0;
    win_idx = 0;
    s_have_last_mean = false;
}

static void win_push(float v, uint8_t window_size)
{
    if (window_size > WINDOW_MAX)
        window_size = WINDOW_MAX;
    if (window_size != win_size_used)
    {
        /* Stability window changed in settings - old ring layout is invalid */
        win_size_used = window_size;
        win_reset();
    }
    win[win_idx] = v;
    win_idx = (uint8_t)((win_idx + 1) % window_size);
    if (win_n < window_size)
        win_n++;
}

static float win_mean(void)
{
    float sum = 0.0f;
    for (uint8_t i = 0; i < win_n; i++)
        sum += win[i];
    return win_n ? sum / win_n : 0.0f;
}

static bool window_is_stable(uint8_t window_size, float tolerance)
{
    if (window_size > WINDOW_MAX)
        window_size = WINDOW_MAX;
    if (win_n < window_size)
        return false;

    float mean = win_mean();
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

static void play_for_status(weight_status_t st)
{
    switch (st)
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

/* EVAL_NOISE_FLOOR_G expressed in the configured display unit */
static float noise_floor_in_unit(wcfg_unit_t unit)
{
    switch (unit)
    {
    case WCFG_UNIT_KG:
        return EVAL_NOISE_FLOOR_G / 1000.0f;
    case WCFG_UNIT_LB:
        return EVAL_NOISE_FLOOR_G / 453.59237f;
    case WCFG_UNIT_OZ:
        return EVAL_NOISE_FLOOR_G / 28.349523f;
    case WCFG_UNIT_G:
    default:
        return EVAL_NOISE_FLOOR_G;
    }
}

/* Evaluate one (bucket-averaged) value */
static void process_value(float value, time_t ts)
{
    const weight_config_t *cfg = weight_config_get();
    weight_state_snapshot_t snap;
    weight_state_get_snapshot(&snap);

    /* Zero / empty-pan noise - full reset, re-arm the placement gate */
    float zero_level = noise_floor_in_unit(cfg->unit);
    if (cfg->zero_threshold > zero_level)
        zero_level = cfg->zero_threshold;
    if (value <= zero_level)
    {
        win_reset();
        s_gate_locked = false;
        s_published_status = WEIGHT_STATUS_NONE;
        weight_state_update_reading(value, false, WEIGHT_STATUS_NONE, ts);
        return;
    }

    /* Value moved away from the current window -> start a new window */
    if (win_n > 0 && fabsf(value - win_mean()) > cfg->stability_tolerance)
        win_reset();

    win_push(value, cfg->stability_window);
    bool stable = window_is_stable(cfg->stability_window, cfg->stability_tolerance);

    weight_status_t status = WEIGHT_STATUS_NONE;
    if (snap.model.valid)
        status = evaluate_limits(value, &snap.model);

    bool moved_since_publish = s_gate_locked &&
                               fabsf(value - s_published_value) > cfg->stability_tolerance;

#if EVAL_REARM_ON_CHANGE
    if (moved_since_publish && stable)
    {
        /* Weight changed and settled again: treat as a new placement */
        s_gate_locked = false;
        moved_since_publish = false;
    }
#endif

    /* Display: keep the published verdict while the item is untouched,
     * follow the live value once it has moved. */
    weight_status_t display_status =
        (s_gate_locked && !moved_since_publish) ? s_published_status : status;
    weight_state_update_reading(value, stable, display_status, ts);

    bool in_run = (snap.mode == WEIGHT_MODE_RUN) && snap.model.valid;
    if (!stable || s_gate_locked || !in_run)
        return; /* gate only locks on a real RUN-mode publish */

    ESP_LOGI(TAG, "stable: %.3f %s  status=%d",
             value, weight_config_unit_str(cfg->unit), status);

    play_for_status(status);
    if (status == WEIGHT_STATUS_HIGH || status == WEIGHT_STATUS_LOW)
        weight_state_raise_alert(status, value, ts);

    /* Non-blocking: queued to the MQTT outbox, or to the offline buffer */
    weight_mqtt_publish_reading(value, status, ts);
    if (status == WEIGHT_STATUS_PASS)
        weight_state_count_pass();

    s_published_value = value;
    s_published_status = status;
    s_gate_locked = true;
}

static void eval_task(void *arg)
{
    ESP_LOGI(TAG, "evaluator started (bucket %d ms, rearm_on_change=%d)",
             BUCKET_US / 1000, EVAL_REARM_ON_CHANGE);

    /* Average every sample that arrives inside a 250 ms bucket instead of
     * dropping them: same 4 Hz evaluation rate, but less noise and no
     * wasted scale output. */
    double b_sum = 0.0;
    uint32_t b_cnt = 0;
    time_t b_ts = 0;
    int64_t b_start_us = 0;

    while (1)
    {
        weight_uart_sample_t s;
        bool got = weight_uart_take(&s, pdMS_TO_TICKS(300));
        int64_t now_us = esp_timer_get_time();

        if (got)
        {
            if (b_cnt == 0)
                b_start_us = now_us;
            b_sum += s.value;
            b_cnt++;
            b_ts = s.ts;
        }

        /* Close the bucket when it is old enough, or when the scale went
         * quiet (take() timed out) and we still hold data. */
        if (b_cnt > 0 && (now_us - b_start_us >= BUCKET_US || !got))
        {
            float mean = (float)(b_sum / b_cnt);
            b_sum = 0.0;
            b_cnt = 0;
            process_value(mean, b_ts);
        }
    }
}

esp_err_t weight_evaluator_start(void)
{
    win_reset();
    BaseType_t r = xTaskCreatePinnedToCore(eval_task, "eval", 4096, NULL, 9, NULL, 0);
    return (r == pdPASS) ? ESP_OK : ESP_FAIL;
}

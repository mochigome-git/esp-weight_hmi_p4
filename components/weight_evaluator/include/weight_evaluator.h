/**
 * @file weight_evaluator.h
 * @brief Stability + pass/high/low evaluator.
 *
 * Pulls samples from weight_uart, averages them into 250 ms buckets,
 * maintains a sliding window for stability detection (every sample within
 * tolerance of the mean), checks against active model limits, updates
 * weight_state, fires audio + alert, triggers MQTT publish.
 *
 * Publishing rules:
 *   - Only in WEIGHT_MODE_RUN with a model selected
 *   - Only when reading is stable
 *   - Only when reading > zero_threshold (ignore zero/negative)
 *   - One publish (and one beep / one alert) per placement; the gate re-arms
 *     when the pan returns to zero
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    esp_err_t weight_evaluator_start(void);

#ifdef __cplusplus
}
#endif

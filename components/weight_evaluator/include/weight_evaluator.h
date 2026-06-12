/**
 * @file weight_evaluator.h
 * @brief Stability + pass/high/low evaluator.
 *
 * Pulls samples from weight_uart, maintains a sliding window for stability
 * detection (max-min within window <= tolerance), checks against active model
 * limits, updates weight_state, fires audio, triggers MQTT publish.
 *
 * Publishing rules:
 *   - Only in WEIGHT_MODE_RUN
 *   - Only when reading is stable
 *   - Only when reading > zero_threshold (ignore zero/negative)
 *   - One publish per stable-reading transition (no spam on long-held stable)
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t weight_evaluator_start(void);

#ifdef __cplusplus
}
#endif

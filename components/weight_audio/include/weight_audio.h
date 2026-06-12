/**
 * @file weight_audio.h
 * @brief Alert sound playback via ES8311 codec.
 *
 * WAV files are expected at /spiffs/ with these names:
 *   alert_pass.wav    - short confirmation tone
 *   alert_low.wav     - warning tone for low alert
 *   alert_high.wav    - urgent tone for high alert
 *   click.wav         - optional UI tap feedback
 *
 * Format: 16-bit PCM, 16kHz, mono. Keep under ~32KB each (preloaded to PSRAM).
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WEIGHT_AUDIO_PASS = 0,
    WEIGHT_AUDIO_LOW,
    WEIGHT_AUDIO_HIGH,
    WEIGHT_AUDIO_CLICK,
    WEIGHT_AUDIO_MAX,
} weight_audio_clip_t;

esp_err_t weight_audio_init(void);

/* Non-blocking - drops the request if already playing. */
esp_err_t weight_audio_play(weight_audio_clip_t clip);

/* Volume / mute - persists via weight_config */
void      weight_audio_set_volume(uint8_t vol_0_100);
void      weight_audio_set_muted(bool muted);

#ifdef __cplusplus
}
#endif

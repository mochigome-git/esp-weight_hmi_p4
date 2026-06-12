/**
 * @file weight_audio.c
 *
 * Architecture:
 *   - At init: open ES8311 via esp_codec_dev, mount I2S as TX
 *   - Read each WAV from /spiffs/ into PSRAM (PCM portion only, skip 44-byte hdr)
 *   - On play(): queue request to audio task which writes PCM via codec dev write API
 *
 * WAV header format (PCM 16-bit):
 *   "RIFF" + 4 byte size + "WAVEfmt " + ... + "data" + 4 byte size + PCM bytes
 * We do a minimal parse: find "data" chunk and treat what follows as PCM.
 */

#include <string.h>
#include <stdio.h>
#include "weight_audio.h"
#include "weight_config.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

/* NOTE: actual ES8311 / codec_dev wiring is board-specific. The Waveshare BSP
 * for the 10.1" provides bsp_audio_codec_speaker_init() which returns a
 * codec_dev_handle. We use that here. */
#include "esp_codec_dev.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_x.h"

static const char *TAG = "audio";

/* Software gain applied once at WAV load time.
 * 2 = +6dB, 3 = +9.5dB, 4 = +12dB. Clamps to prevent clipping.
 * Increase if WAVs sound too quiet even at max codec volume. */
#define AUDIO_SW_GAIN 3

typedef struct
{
    uint8_t *pcm; /* PCM samples in PSRAM */
    size_t bytes; /* number of PCM bytes */
    uint32_t sample_rate;
    uint16_t bits;
    uint16_t channels;
} clip_t;

static clip_t s_clips[WEIGHT_AUDIO_MAX];
static QueueHandle_t s_play_q;
static esp_codec_dev_handle_t s_codec;

static const char *clip_path(weight_audio_clip_t c)
{
    switch (c)
    {
    case WEIGHT_AUDIO_PASS:
        return "/spiffs/alert_pass.wav";
    case WEIGHT_AUDIO_LOW:
        return "/spiffs/alert_low.wav";
    case WEIGHT_AUDIO_HIGH:
        return "/spiffs/alert_high.wav";
    case WEIGHT_AUDIO_CLICK:
        return "/spiffs/click.wav";
    default:
        return NULL;
    }
}

/* Minimal WAV reader: returns malloc'd PCM in PSRAM, fills meta fields.
 * On failure returns NULL and clip stays empty (audio silently skipped). */
static esp_err_t load_wav(const char *path, clip_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        ESP_LOGW(TAG, "missing: %s (skipped)", path);
        return ESP_ERR_NOT_FOUND;
    }

    /* Read into a temp buffer first */
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    rewind(f);

    if (flen < 44)
    {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buf = malloc(flen);
    if (!buf)
    {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    fread(buf, 1, flen, f);
    fclose(f);

    /* Parse - simple linear search for 'fmt ' and 'data' chunks */
    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0)
    {
        free(buf);
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t sr = 16000;
    uint16_t bits = 16, ch = 1;
    uint8_t *pcm = NULL;
    size_t pcm_len = 0;

    size_t off = 12;
    while (off + 8 < (size_t)flen)
    {
        const char *cid = (const char *)(buf + off);
        uint32_t csz = (uint32_t)buf[off + 4] |
                       ((uint32_t)buf[off + 5] << 8) |
                       ((uint32_t)buf[off + 6] << 16) |
                       ((uint32_t)buf[off + 7] << 24);

        if (memcmp(cid, "fmt ", 4) == 0)
        {
            ch = (uint16_t)buf[off + 10] | ((uint16_t)buf[off + 11] << 8);
            sr = (uint32_t)buf[off + 12] | ((uint32_t)buf[off + 13] << 8) |
                 ((uint32_t)buf[off + 14] << 16) | ((uint32_t)buf[off + 15] << 24);
            bits = (uint16_t)buf[off + 22] | ((uint16_t)buf[off + 23] << 8);
        }
        else if (memcmp(cid, "data", 4) == 0)
        {
            pcm_len = csz;
            pcm = heap_caps_malloc(pcm_len, MALLOC_CAP_SPIRAM);
            if (!pcm)
            {
                free(buf);
                return ESP_ERR_NO_MEM;
            }
            memcpy(pcm, buf + off + 8, pcm_len);

            /* Apply software gain — boost amplitude with clipping protection */
            if (bits == 16 && AUDIO_SW_GAIN > 1)
            {
                int16_t *samples = (int16_t *)pcm;
                size_t n = pcm_len / 2;
                for (size_t i = 0; i < n; i++)
                {
                    int32_t s = (int32_t)samples[i] * AUDIO_SW_GAIN;
                    if (s > 32767)
                        s = 32767;
                    if (s < -32768)
                        s = -32768;
                    samples[i] = (int16_t)s;
                }
                ESP_LOGI(TAG, "applied %dx software gain", AUDIO_SW_GAIN);
            }
            break;
        }
        off += 8 + csz;
    }
    free(buf);

    if (!pcm)
        return ESP_ERR_INVALID_ARG;

    out->pcm = pcm;
    out->bytes = pcm_len;
    out->sample_rate = sr;
    out->bits = bits;
    out->channels = ch;
    ESP_LOGI(TAG, "loaded %s (%u bytes, %luHz %dch %dbit)",
             path, (unsigned)pcm_len, (unsigned long)sr, ch, bits);
    return ESP_OK;
}

static void play_task(void *arg)
{
    weight_audio_clip_t clip;
    while (1)
    {
        if (xQueueReceive(s_play_q, &clip, portMAX_DELAY) != pdTRUE)
            continue;
        if (clip >= WEIGHT_AUDIO_MAX)
            continue;

        const weight_config_t *cfg = weight_config_get();
        if (cfg->audio_muted)
            continue;

        clip_t *c = &s_clips[clip];
        if (!c->pcm || !c->bytes)
            continue;

        /* Set sample rate per clip in case different */
        esp_codec_dev_sample_info_t fs = {
            .sample_rate = c->sample_rate,
            .channel = c->channels,
            .bits_per_sample = c->bits,
        };
        esp_codec_dev_open(s_codec, &fs);
        esp_codec_dev_set_out_vol(s_codec, cfg->audio_volume);
        esp_codec_dev_write(s_codec, c->pcm, c->bytes);
        esp_codec_dev_close(s_codec);
    }
}

esp_err_t weight_audio_init(void)
{
    /* TODO: replace this stub with the actual Waveshare BSP call once we
     * vendor the BSP. Likely:
     *   s_codec = bsp_audio_codec_speaker_init();
     *
     * For now we provide a __weak local stub that returns NULL, so the
     * project links cleanly and audio is gracefully disabled at runtime.
     * When the BSP is added, its strong definition will override this stub. */
    s_codec = bsp_audio_codec_speaker_init();
    if (!s_codec)
    {
        ESP_LOGW(TAG, "codec init failed - alerts will be silent");
    }

    for (int i = 0; i < WEIGHT_AUDIO_MAX; i++)
    {
        const char *p = clip_path((weight_audio_clip_t)i);
        if (p)
            load_wav(p, &s_clips[i]);
    }

    s_play_q = xQueueCreate(4, sizeof(weight_audio_clip_t));
    if (!s_play_q)
        return ESP_ERR_NO_MEM;

    BaseType_t r = xTaskCreatePinnedToCore(play_task, "audio", 4096, NULL, 6, NULL, 1);
    return (r == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t weight_audio_play(weight_audio_clip_t clip)
{
    if (clip >= WEIGHT_AUDIO_MAX)
        return ESP_ERR_INVALID_ARG;
    /* Non-blocking - drop if already busy */
    return xQueueSend(s_play_q, &clip, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void weight_audio_set_volume(uint8_t v)
{
    weight_config_set_audio(v, weight_config_get()->audio_muted);
    weight_config_save();
}

void weight_audio_set_muted(bool m)
{
    weight_config_set_audio(weight_config_get()->audio_volume, m);
    weight_config_save();
}

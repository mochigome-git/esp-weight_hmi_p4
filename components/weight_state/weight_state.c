/**
 * @file weight_state.c
 * @brief Thread-safe shared state.
 */

#include <string.h>
#include "weight_state.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "state";

static weight_state_snapshot_t s_state;
static SemaphoreHandle_t s_mutex;

#define LOCK() xSemaphoreTake(s_mutex, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mutex)

/* Derive a short device_id from the WiFi MAC, e.g. "p4-7c4f1a2b3c4d" */
static void derive_device_id(char *out, size_t out_len)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, out_len, "p4-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

esp_err_t weight_state_init(const char *tenant_id, const char *tenant_short)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex)
        return ESP_ERR_NO_MEM;

    memset(&s_state, 0, sizeof(s_state));
    derive_device_id(s_state.device_id, sizeof(s_state.device_id));
    strncpy(s_state.tenant_id, tenant_id, sizeof(s_state.tenant_id) - 1);
    strncpy(s_state.tenant_short, tenant_short, sizeof(s_state.tenant_short) - 1);
    s_state.mode = WEIGHT_MODE_IDLE;

    ESP_LOGI(TAG, "init: device_id=%s tenant_id=%s short=%s",
             s_state.device_id, s_state.tenant_id, s_state.tenant_short);
    return ESP_OK;
}

void weight_state_get_snapshot(weight_state_snapshot_t *out)
{
    LOCK();
    memcpy(out, &s_state, sizeof(*out));
    UNLOCK();
}

void weight_state_set_mode(weight_mode_t mode)
{
    LOCK();
    s_state.mode = mode;
    if (mode == WEIGHT_MODE_IDLE)
    {
        /* Going to IDLE clears latest reading - keep alert sticky though */
        s_state.last_stable = false;
        s_state.last_status = WEIGHT_STATUS_NONE;
    }
    UNLOCK();
    ESP_LOGI(TAG, "mode -> %s", mode == WEIGHT_MODE_RUN ? "RUN" : "IDLE");
}

void weight_state_set_active_model(const weight_active_model_t *m)
{
    LOCK();
    memcpy(&s_state.model, m, sizeof(*m));
    s_state.model.valid = true;
    UNLOCK();
    ESP_LOGI(TAG, "active model: %s [%.3f .. %.3f .. %.3f] %s",
             m->name, m->lower_limit, m->standard, m->upper_limit, m->unit);
}

void weight_state_clear_active_model(void)
{
    LOCK();
    memset(&s_state.model, 0, sizeof(s_state.model));
    UNLOCK();
}

void weight_state_update_reading(float v, bool stable,
                                 weight_status_t status, time_t ts)
{
    LOCK();
    s_state.last_reading = v;
    s_state.last_stable = stable;
    s_state.last_status = status;
    s_state.last_reading_ts = ts;

    /* Sticky alert: only trigger on stable HIGH/LOW with WEIGHT_STATUS_NONE
     * guard — evaluator passes NONE after first publish to prevent retrigger
     * during item removal */
    if (stable &&
        status != WEIGHT_STATUS_NONE &&
        (status == WEIGHT_STATUS_HIGH || status == WEIGHT_STATUS_LOW))
    {
        s_state.alert_active = true;
        s_state.alert_status = status;
        s_state.alert_reading = v;
        s_state.alert_ts = ts;
    }
    UNLOCK();
}

void weight_state_dismiss_alert(void)
{
    LOCK();
    s_state.alert_active = false;
    UNLOCK();
    ESP_LOGI(TAG, "alert dismissed");
}

void weight_state_set_wifi(bool c)
{
    LOCK();
    s_state.wifi_connected = c;
    UNLOCK();
}
void weight_state_set_mqtt(bool c)
{
    LOCK();
    s_state.mqtt_connected = c;
    UNLOCK();
}

weight_mode_t weight_state_get_mode(void)
{
    LOCK();
    weight_mode_t m = s_state.mode;
    UNLOCK();
    return m;
}

bool weight_state_has_active_model(void)
{
    LOCK();
    bool v = s_state.model.valid;
    UNLOCK();
    return v;
}

void weight_state_set_tenant(const char *tenant_id, const char *tenant_short)
{
    LOCK();
    if (tenant_id)
        strncpy(s_state.tenant_id, tenant_id, sizeof(s_state.tenant_id) - 1);
    if (tenant_short)
        strncpy(s_state.tenant_short, tenant_short, sizeof(s_state.tenant_short) - 1);
    UNLOCK();
    ESP_LOGI(TAG, "tenant updated: %s / %s", s_state.tenant_id, s_state.tenant_short);
}

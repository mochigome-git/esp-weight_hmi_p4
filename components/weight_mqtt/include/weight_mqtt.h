/**
 * @file weight_mqtt.h
 * @brief EMQX MQTT client with TLS + basic auth.
 *
 * Topics use the short tenant slug (weight_config tenant_short) to keep them
 * compact on the broker, while the JSON payload still carries the full
 * tenant_id UUID for database joins. device_id is the configured device id,
 * falling back to the MAC-derived one.
 *
 *   PUB  {tenant_short}/devices/payload          reading JSON (QoS1) + heartbeat (30 s)
 *   PUB  {tenant_short}/{device_id}/status       LWT + retained status
 *   PUB  {tenant_short}/{device_id}/sync/req     request full model resync
 *   PUB  {tenant_short}/{device_id}/lot/new      new-lot request
 *   PUB  {tenant_short}/models/push              HMI model edits / deletes
 *   SUB  {tenant_short}/models/sync/#            upsert/delete events from Go svc
 *
 * Readings share {tenant_short}/devices/payload with the heartbeat because
 * that is the only topic the EMQX rule bridges to Kafka (gcl_devices_payload)
 * for gokafka-raw. device_id travels in the JSON body.
 *
 * Reading JSON shape:
 *   {
 *     "device_id": "...", "tenant_id": "...",
 *     "timestamp": "2026-06-05T14:32:18Z",
 *     "ts_valid": false, "uptime_ms": 12345,      <- only before SNTP sync
 *     "readings": {"value": 1234.5, "unit": "g", "status": "pass",
 *                  "model_id": "...", "model_name": "200g sachet"},
 *     "limits": {"lower": 1150.0, "standard": 1200.0, "upper": 1250.0}
 *   }
 *
 * All publish functions are non-blocking and safe to call from any task
 * (LVGL, evaluator, esp_timer). Readings that cannot be sent right now are
 * buffered in PSRAM and flushed on the next connect.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"
#include "weight_state.h"
#include "weight_model_store.h"

#ifdef __cplusplus
extern "C"
{
#endif

    esp_err_t weight_mqtt_init(void);

    /* Build and start the MQTT client - only succeeds when a broker host is
     * configured. Safe to call repeatedly (no-op once started). */
    esp_err_t weight_mqtt_start_if_ready(void);

    /* Queue a stable weight reading (buffered offline if not connected).
     * Returns ESP_OK if queued for sending now, ESP_ERR_INVALID_STATE if it
     * was buffered for later. */
    esp_err_t weight_mqtt_publish_reading(float value, weight_status_t status, time_t ts);

    /* Push current model + run-mode status as retained message. Called
     * automatically on connect and whenever mode / active model changes. */
    esp_err_t weight_mqtt_publish_status(void);

    /* Request full re-sync of models (also sent automatically on connect). */
    esp_err_t weight_mqtt_request_sync(void);

    /* Tear down and rebuild the client (new device id / broker settings).
     * Blocks up to ~10 s - call from a worker task, not the LVGL task. */
    esp_err_t weight_mqtt_restart(void);

    bool weight_mqtt_is_connected(void);

    esp_err_t weight_mqtt_publish_heartbeat(void);

    esp_err_t weight_mqtt_publish_model_push(const weight_model_t *m);

    esp_err_t weight_mqtt_publish_model_delete(const char *id);

    /* Returns ESP_ERR_NOT_FOUND if machine_id unset, ESP_ERR_INVALID_STATE if offline. */
    esp_err_t weight_mqtt_publish_new_lot(void);

#ifdef __cplusplus
}
#endif

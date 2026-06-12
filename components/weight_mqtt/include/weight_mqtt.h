/**
 * @file weight_mqtt.h
 * @brief EMQX MQTT client with TLS + basic auth.
 *
 * Topics use a short tenant slug (TENANT_SHORT from main.c) to keep them
 * compact on the broker, while the JSON payload still carries the full
 * tenant_id UUID for database joins.
 *
 *   PUB  {tenant_short}/{device_id}/weight       reading JSON
 *   PUB  {tenant_short}/{device_id}/status       LWT + retained status
 *   PUB  {tenant_short}/{device_id}/sync/req     request full model resync
 *   SUB  {tenant_short}/models/sync/+            upsert/delete events from Go svc
 *
 * Reading JSON shape (matches the user's spec):
 *   {
 *     "device_id": "...", "tenant_id": "...",
 *     "timestamp": "2026-06-05T14:32:18.234Z",
 *     "model_id": "...", "model_name": "200g sachet",
 *     "reading": 1234.5, "unit": "g",
 *     "status": "pass",
 *     "limits": {"lower": 1150.0, "standard": 1200.0, "upper": 1250.0}
 *   }
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"
#include "weight_state.h"

#ifdef __cplusplus
extern "C"
{
#endif

    esp_err_t weight_mqtt_init(void);

    /* Build and start the MQTT client - only succeeds when (a) broker host is
     * configured in weight_config and (b) WiFi has an IP. Safe to call repeatedly.
     * Called automatically by the WiFi GOT_IP handler, and by the settings UI
     * after the user enters broker details. */
    esp_err_t weight_mqtt_start_if_ready(void);

    /* Publish a stable weight reading. Called by evaluator. */
    esp_err_t weight_mqtt_publish_reading(float value, weight_status_t status, time_t ts);

    /* Push current model + run-mode status as retained message. */
    esp_err_t weight_mqtt_publish_status(void);

    /* Request full re-sync of models from broker (called by model store on boot
     * after MQTT comes up). */
    esp_err_t weight_mqtt_request_sync(void);

    esp_err_t weight_mqtt_restart(void);

    bool weight_mqtt_is_connected(void);

    esp_err_t weight_mqtt_publish_heartbeat(void);

#ifdef __cplusplus
}
#endif

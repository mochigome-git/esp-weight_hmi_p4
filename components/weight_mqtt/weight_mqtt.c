/**
 * @file weight_mqtt.c
 *
 * esp-mqtt based client. Settings come from weight_config (host, port, user,
 * pass, tls). CA cert is loaded from NVS via weight_config_get_ca_cert.
 *
 * Reconnects automatically. On every (re)connect: subscribe model sync topic
 * and request a full resync.
 */

#include <string.h>
#include <stdio.h>
#include <sys/time.h>
#include "weight_mqtt.h"
#include "weight_state.h"
#include "weight_config.h"
#include "weight_model_store.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_timer.h"

#define FW_VERSION "1.0.0"

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t s_client;
static bool s_connected = false;
static time_t s_machine_start = 0;
static time_t s_machine_stop = 0;
static esp_timer_handle_t s_heartbeat_timer = NULL;

/* Heap-allocated buffers for topics, computed once after init.
 * Max topic length: "{16 short}/{37 device_id}/sync/req" ~= 64, plus headroom. */
static char s_topic_reading[96];
static char s_topic_status[96];
static char s_topic_sync_req[96];
static char s_topic_sync_sub[96];
static char s_topic_payload[96];
static char s_topic_model_push[96];

/* Heap-allocated CA cert (PEM), loaded from NVS at init */
static char *s_ca_pem = NULL;

/* CA cert embedded by CMake EMBED_TXTFILES (certs/emqxsl-ca.crt) */
extern const uint8_t emqx_ca_pem_start[] asm("_binary_emqxsl_ca_crt_start");
extern const uint8_t emqx_ca_pem_end[] asm("_binary_emqxsl_ca_crt_end");

static const char *status_str(weight_status_t s)
{
    switch (s)
    {
    case WEIGHT_STATUS_PASS:
        return "pass";
    case WEIGHT_STATUS_HIGH:
        return "high";
    case WEIGHT_STATUS_LOW:
        return "low";
    default:
        return "none";
    }
}

/* ISO-8601 UTC string from time_t. ts in unix seconds; we don't have ms here
 * because UART parses on second granularity for now. */
static void iso8601(time_t t, char *out, size_t len)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static void build_topics(const weight_state_snapshot_t *s)
{
    /* Topic scheme: {tenant_short}/{device_id}/...
     * keeps broker output short and human-readable. */
    snprintf(s_topic_reading, sizeof(s_topic_reading), "%s/%s/weight", s->tenant_short, s->device_id);
    snprintf(s_topic_status, sizeof(s_topic_status), "%s/%s/status", s->tenant_short, s->device_id);
    snprintf(s_topic_sync_req, sizeof(s_topic_sync_req), "%s/%s/sync/req", s->tenant_short, s->device_id);
    snprintf(s_topic_sync_sub, sizeof(s_topic_sync_sub), "%s/models/sync/#", s->tenant_short);
    snprintf(s_topic_payload, sizeof(s_topic_payload), "%s/devices/payload", s->tenant_short);
    snprintf(s_topic_model_push, sizeof(s_topic_model_push), "%s/models/push", s->tenant_short); // no device_id
}

/* Uptime in seconds since boot */
static uint32_t get_uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

static void heartbeat_timer_cb(void *arg)
{
    weight_mqtt_publish_heartbeat();
}

esp_err_t weight_mqtt_publish_heartbeat(void)
{
    if (!s_connected)
        return ESP_ERR_INVALID_STATE;

    const weight_config_t *cfg = weight_config_get();
    weight_state_snapshot_t snap;
    weight_state_get_snapshot(&snap);

    const char *device_id = cfg->device_id[0] ? cfg->device_id : snap.device_id;

    char ts_str[32];
    iso8601(time(NULL), ts_str, sizeof(ts_str));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddStringToObject(root, "tenant_id", snap.tenant_id);

    cJSON *status_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(status_obj, "fw", FW_VERSION);
    cJSON_AddStringToObject(status_obj, "ts", ts_str);
    cJSON_AddStringToObject(status_obj, "kind", "heartbeat");
    cJSON_AddNumberToObject(status_obj, "uptime_s", get_uptime_s());
    cJSON_AddItemToObject(root, "status", status_obj);

    char *payload = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, "publishing heartbeat to: '%s'", s_topic_payload);
    ESP_LOGI(TAG, "payload: %s", payload);

    int msg_id = esp_mqtt_client_publish(s_client, s_topic_payload, /* ← changed */
                                         payload, 0, 1, 0);         /* not retained */
    free(payload);
    cJSON_Delete(root);
    return msg_id >= 0 ? ESP_OK : ESP_FAIL;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        weight_state_set_mqtt(true);
        ESP_LOGI(TAG, "connected - subscribing to: '%s'", s_topic_sync_sub);
        esp_mqtt_client_subscribe(s_client, s_topic_sync_sub, 1);
        weight_mqtt_publish_status();
        weight_mqtt_publish_heartbeat(); /* immediate heartbeat on connect */
        weight_mqtt_request_sync();

        /* Start 3-minute heartbeat timer */
        if (!s_heartbeat_timer)
        {
            esp_timer_create_args_t args = {
                .callback = heartbeat_timer_cb,
                .name = "mqtt_hb",
            };
            esp_timer_create(&args, &s_heartbeat_timer);
        }
        esp_timer_start_periodic(s_heartbeat_timer, 30ULL * 1000000ULL); /* 30 sec */
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        weight_state_set_mqtt(false);
        ESP_LOGW(TAG, "disconnected");
        if (s_heartbeat_timer)
            esp_timer_stop(s_heartbeat_timer);
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "incoming data on topic: '%.*s'",
                 event->topic_len, event->topic);
        ESP_LOGI(TAG, "payload: '%.*s'",
                 event->data_len, event->data);
        {
            char *payload = strndup(event->data, event->data_len);
            if (payload)
            {
                // Bridge sends {"timestamp":"...","models":[{...},{...}]}
                // Iterate the array and apply each model individually
                cJSON *root = cJSON_Parse(payload);
                if (root)
                {
                    cJSON *models_arr = cJSON_GetObjectItem(root, "models");
                    if (cJSON_IsArray(models_arr))
                    {
                        cJSON *item;
                        cJSON_ArrayForEach(item, models_arr)
                        {
                            char *model_str = cJSON_PrintUnformatted(item);
                            if (model_str)
                            {
                                esp_err_t err = weight_model_store_apply_remote_json(model_str);
                                if (err != ESP_OK)
                                    ESP_LOGW(TAG, "sync apply failed: %s", esp_err_to_name(err));
                                free(model_str);
                            }
                        }
                    }
                    else
                    {
                        // Fallback: single model object
                        esp_err_t err = weight_model_store_apply_remote_json(payload);
                        if (err != ESP_OK)
                            ESP_LOGW(TAG, "sync apply failed: %s", esp_err_to_name(err));
                    }
                    cJSON_Delete(root);
                }
                free(payload);
            }
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error type=%d",
                 event->error_handle->error_type);
        break;

    default:
        break;
    }
}

esp_err_t weight_mqtt_publish_model_delete(const char *id)
{
    if (!s_connected)
        return ESP_ERR_INVALID_STATE;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", id);

    char *payload = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, "publishing model delete to: '%s'", s_topic_model_push);
    ESP_LOGI(TAG, "payload: %s", payload);

    // Reuse models/push topic with a "deleted" flag
    cJSON_AddBoolToObject(root, "deleted", true);
    free(payload);
    payload = cJSON_PrintUnformatted(root);

    int msg_id = esp_mqtt_client_publish(s_client, s_topic_model_push,
                                         payload, 0, 1, 0);
    free(payload);
    cJSON_Delete(root);
    return msg_id >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t weight_mqtt_publish_model_push(const weight_model_t *m)
{
    if (!s_connected)
        return ESP_ERR_INVALID_STATE;

    char s_lower[16], s_standard[16], s_upper[16];
    snprintf(s_lower, sizeof(s_lower), "%.3f", m->lower_limit);
    snprintf(s_standard, sizeof(s_standard), "%.3f", m->standard);
    snprintf(s_upper, sizeof(s_upper), "%.3f", m->upper_limit);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", m->id);
    cJSON_AddStringToObject(root, "name", m->name);
    cJSON_AddRawToObject(root, "lower_limit", s_lower);
    cJSON_AddRawToObject(root, "standard", s_standard);
    cJSON_AddRawToObject(root, "upper_limit", s_upper);
    const weight_config_t *cfg = weight_config_get();
    cJSON_AddStringToObject(root, "unit", weight_config_unit_str(cfg->unit));

    char *payload = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, "publishing model push to: '%s'", s_topic_model_push);
    ESP_LOGI(TAG, "payload: %s", payload);

    int msg_id = esp_mqtt_client_publish(s_client, s_topic_model_push,
                                         payload, 0, 1, 0);
    free(payload);
    cJSON_Delete(root);
    return msg_id >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t weight_mqtt_init(void)
{
    /* Phase 1: set up internal state only. Do NOT create the MQTT client yet.
     * MQTT needs (a) a valid broker host and (b) WiFi connectivity. Both come
     * later - the WiFi handler calls weight_mqtt_start_if_ready() on GOT_IP,
     * and the settings UI calls it after a broker host is saved. */
    weight_state_snapshot_t snap;
    weight_state_get_snapshot(&snap);
    build_topics(&snap);

    ESP_LOGI(TAG, "init (deferred start - waits for WiFi + broker config)");
    return ESP_OK;
}

/* Build the actual client + start it. Safe to call repeatedly - no-op if
 * already started, no-op if broker host empty. */
esp_err_t weight_mqtt_start_if_ready(void)
{
    if (s_client)
    {
        /* Already initialised. esp-mqtt handles its own reconnection. */
        return ESP_OK;
    }

    const weight_config_t *cfg = weight_config_get();

    /* Refuse to start with an invalid broker host - this would build a URI
     * like "mqtts://:8883" which esp-mqtt rejects with "Error parse uri". */
    if (cfg->mqtt_host[0] == '\0')
    {
        ESP_LOGI(TAG, "broker host not configured - skipping MQTT start");
        return ESP_ERR_INVALID_STATE;
    }

    weight_state_snapshot_t snap;
    weight_state_get_snapshot(&snap);

    /* TLS cert selection:
     *   1. If a custom CA cert is stored in NVS (self-hosted EMQX with
     *      self-signed cert), use that.
     *   2. Otherwise fall back to the embedded bundle (covers EMQX Cloud
     *      and most public CA-signed brokers).
     */
    const char *ca_cert = NULL;
    if (cfg->mqtt_tls)
    {
        if (!s_ca_pem)
        {
            s_ca_pem = malloc(4096);
            if (s_ca_pem)
            {
                size_t actual = 0;
                if (weight_config_get_ca_cert(s_ca_pem, 4096, &actual) != ESP_OK)
                {
                    /* No custom cert in NVS - use the embedded bundle */
                    free(s_ca_pem);
                    s_ca_pem = NULL;
                }
            }
        }

        if (s_ca_pem)
        {
            ESP_LOGI(TAG, "TLS: using custom CA cert from NVS");
            ca_cert = s_ca_pem;
        }
        else
        {
            ESP_LOGI(TAG, "TLS: using embedded EMQX/public CA bundle");
            ca_cert = (const char *)emqx_ca_pem_start;
        }
    }

    /* Build LWT - retained, topic /status, payload says offline */
    char lwt[256];
    snprintf(lwt, sizeof(lwt),
             "{\"device_id\":\"%s\",\"online\":false}", snap.device_id);

    char uri[128];
    snprintf(uri, sizeof(uri), "%s://%s:%hu",
             cfg->mqtt_tls ? "mqtts" : "mqtt",
             cfg->mqtt_host, (uint16_t)cfg->mqtt_port);

    esp_mqtt_client_config_t mc = {
        .broker.address.uri = uri,
        .credentials.username = cfg->mqtt_user,
        .credentials.authentication.password = cfg->mqtt_pass,
        .credentials.client_id = cfg->device_id[0] ? cfg->device_id : snap.device_id,
        .session.last_will = {
            .topic = s_topic_status,
            .msg = lwt,
            .msg_len = strlen(lwt),
            .qos = 1,
            .retain = 1,
        },
        .session.keepalive = 30,
    };

    if (cfg->mqtt_tls && ca_cert)
    {
        mc.broker.verification.certificate = ca_cert;
    }

    s_client = esp_mqtt_client_init(&mc);
    if (!s_client)
        return ESP_FAIL;

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);

    ESP_LOGI(TAG, "starting client: %s (tls=%d, custom_ca=%d)",
             uri, cfg->mqtt_tls, s_ca_pem != NULL);

    return esp_mqtt_client_start(s_client);
}

esp_err_t weight_mqtt_restart(void)
{
    ESP_LOGI(TAG, "restart called - rebuilding topics and client");

    if (s_client)
    {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        s_connected = false;
        weight_state_set_mqtt(false);
    }

    /* Build topics using config device_id directly, not snapshot */
    const weight_config_t *cfg = weight_config_get();

    weight_state_snapshot_t snap;
    weight_state_get_snapshot(&snap);

    /* Patch snap with the correct device_id from config */
    const char *dev_id = cfg->device_id[0] ? cfg->device_id : snap.device_id;
    snprintf(s_topic_reading, sizeof(s_topic_reading), "%s/%s/weight", snap.tenant_short, dev_id);
    snprintf(s_topic_status, sizeof(s_topic_status), "%s/%s/status", snap.tenant_short, dev_id);
    snprintf(s_topic_sync_req, sizeof(s_topic_sync_req), "%s/%s/sync/req", snap.tenant_short, dev_id);
    snprintf(s_topic_sync_sub, sizeof(s_topic_sync_sub), "%s/models/sync/#", snap.tenant_short);
    snprintf(s_topic_payload, sizeof(s_topic_payload), "%s/devices/payload", snap.tenant_short);

    ESP_LOGI(TAG, "topics rebuilt:");
    ESP_LOGI(TAG, "  reading : %s", s_topic_reading);
    ESP_LOGI(TAG, "  status  : %s", s_topic_status);
    ESP_LOGI(TAG, "  sync_sub: %s", s_topic_sync_sub);

    return weight_mqtt_start_if_ready();
}

esp_err_t weight_mqtt_publish_reading(float value, weight_status_t status, time_t ts)
{
    if (!s_connected)
    {
        ESP_LOGW(TAG, "publish_reading skipped - not connected");
        return ESP_ERR_INVALID_STATE;
    }

    const weight_config_t *cfg = weight_config_get();
    weight_state_snapshot_t snap;
    weight_state_get_snapshot(&snap);

    const char *device_id = cfg->device_id[0] ? cfg->device_id : snap.device_id;

    char ts_str[32];
    iso8601(ts, ts_str, sizeof(ts_str));

    /* Round all floats to 3 decimal places */
    char s_reading[16], s_lower[16], s_standard[16], s_upper[16];
    snprintf(s_reading, sizeof(s_reading), "%.3f", value);
    snprintf(s_lower, sizeof(s_lower), "%.3f", snap.model.lower_limit);
    snprintf(s_standard, sizeof(s_standard), "%.3f", snap.model.standard);
    snprintf(s_upper, sizeof(s_upper), "%.3f", snap.model.upper_limit);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddStringToObject(root, "tenant_id", snap.tenant_id);
    cJSON_AddStringToObject(root, "timestamp", ts_str);
    /* removed: model_id, model_name, status from root */

    cJSON *reading_obj = cJSON_CreateObject();
    cJSON_AddRawToObject(reading_obj, "value", s_reading);
    cJSON_AddStringToObject(reading_obj, "unit", weight_config_unit_str(cfg->unit));
    cJSON_AddStringToObject(reading_obj, "status", status_str(status)); /* moved here */
    cJSON_AddStringToObject(reading_obj, "model_id", snap.model.id);
    cJSON_AddStringToObject(reading_obj, "model_name", snap.model.name);
    cJSON_AddItemToObject(root, "readings", reading_obj);

    cJSON *lim = cJSON_CreateObject();
    cJSON_AddRawToObject(lim, "lower", s_lower);
    cJSON_AddRawToObject(lim, "standard", s_standard);
    cJSON_AddRawToObject(lim, "upper", s_upper);
    cJSON_AddItemToObject(root, "limits", lim);

    char *payload = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, "publishing to topic: '%s'", s_topic_reading);
    ESP_LOGI(TAG, "payload: %s", payload);

    int msg_id = esp_mqtt_client_publish(s_client, s_topic_reading,
                                         payload, 0, 1, 0);
    ESP_LOGI(TAG, "publish result msg_id=%d (%s)",
             msg_id, msg_id >= 0 ? "OK" : "FAILED");

    free(payload);
    cJSON_Delete(root);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t weight_mqtt_publish_status(void)
{
    if (!s_connected)
        return ESP_ERR_INVALID_STATE;

    const weight_config_t *cfg = weight_config_get();
    weight_state_snapshot_t snap;
    weight_state_get_snapshot(&snap);

    bool is_running = (snap.mode == WEIGHT_MODE_RUN);

    /* Track machine start/stop times */
    time_t now = time(NULL);
    if (is_running && s_machine_start == 0)
        s_machine_start = now;
    if (!is_running && s_machine_start != 0)
        s_machine_stop = now;

    char start_str[32] = "--";
    char stop_str[32] = "--";
    if (s_machine_start > 0)
        iso8601(s_machine_start, start_str, sizeof(start_str));
    if (s_machine_stop > 0)
        iso8601(s_machine_stop, stop_str, sizeof(stop_str));

    const char *device_id = cfg->device_id[0] ? cfg->device_id : snap.device_id;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "online", true);
    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddStringToObject(root, "tenant_id", snap.tenant_id);
    cJSON_AddStringToObject(root, "machine_state", is_running ? "running" : "idle");
    cJSON_AddStringToObject(root, "machine_start", start_str);
    cJSON_AddStringToObject(root, "machine_stop", stop_str);
    cJSON_AddStringToObject(root, "current_model_id", snap.model.id);

    char *payload = cJSON_PrintUnformatted(root);
    int msg_id = esp_mqtt_client_publish(s_client, s_topic_status,
                                         payload, 0, 1, 1); /* retained */
    free(payload);
    cJSON_Delete(root);
    return msg_id >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t weight_mqtt_request_sync(void)
{
    if (!s_connected)
        return ESP_ERR_INVALID_STATE;
    const char *msg = "{\"action\":\"resync\"}";
    int id = esp_mqtt_client_publish(s_client, s_topic_sync_req, msg, 0, 1, 0);
    return id >= 0 ? ESP_OK : ESP_FAIL;
}

bool weight_mqtt_is_connected(void)
{
    weight_state_snapshot_t snap;
    weight_state_get_snapshot(&snap);
    return snap.mqtt_connected;
}

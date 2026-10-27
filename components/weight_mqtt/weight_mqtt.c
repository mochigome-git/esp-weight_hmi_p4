/**
 * @file weight_mqtt.c
 *
 * esp-mqtt based client. Settings come from weight_config (host, port, user,
 * pass, tls). CA cert is loaded from NVS via weight_config_get_ca_cert.
 *
 * Reconnects automatically. On every (re)connect: subscribe model sync + lot
 * ack topics, publish status + heartbeat, request a full resync, ask for the
 * machine's current lot, flush offline readings.
 *
 * Concurrency rules
 *   - s_client_mtx guards the LIFETIME of s_client. Every public function
 *     takes it (short timeout) before touching s_client, so a restart can
 *     never free the client under a publisher.
 *   - The esp-mqtt event handler runs in the MQTT task and must NEVER take
 *     s_client_mtx: restart holds it while esp_mqtt_client_stop() waits for
 *     that very task. Handler code uses the *_c() helpers with event->client.
 *   - Publishing uses esp_mqtt_client_enqueue(): non-blocking, the MQTT task
 *     does the socket I/O. Nothing here blocks the evaluator, the LVGL task
 *     or the esp_timer task on the network.
 *
 * Offline buffering
 *   - Readings that can't be queued (no client / not connected / outbox
 *     refused) go into a PSRAM ring (READING_RING_LEN, oldest dropped) and
 *     are flushed on the next MQTT_EVENT_CONNECTED.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include "weight_mqtt.h"
#include "weight_state.h"
#include "weight_config.h"
#include "weight_model_store.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_random.h"

#define FW_VERSION "1.0.1"

#define HEARTBEAT_PERIOD_US (30ULL * 1000000ULL) /* 30 s */
#define LOCK_WAIT_PUBLISH pdMS_TO_TICKS(50)
#define LOCK_WAIT_RESTART pdMS_TO_TICKS(10000)
#define READING_RING_LEN 500     /* ~350 B each, in PSRAM */
#define SYNC_MSG_MAX (64 * 1024) /* largest sync message we reassemble */

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected = false;
static esp_timer_handle_t s_heartbeat_timer = NULL;
static SemaphoreHandle_t s_client_mtx = NULL;

/* Topics. Rebuilt only under s_client_mtx while the client is stopped,
 * so the MQTT task never sees them change mid-use. */
static char s_topic_reading[96];
static char s_topic_status[96];
static char s_topic_sync_req[96];
static char s_topic_sync_sub[96];
static char s_topic_payload[96];
static char s_topic_model_push[96];
static char s_topic_lot_new[96];
static char s_topic_lot_get[96];
static char s_topic_lot_ack[96];
static char s_dev_id[WCFG_MAX_DEVICE_ID_LEN]; /* id used in topics / client id / LWT */

/* Custom CA cert (PEM) from NVS; must outlive the client (not copied by esp-mqtt) */
static char *s_ca_pem = NULL;

/* CA cert embedded by CMake EMBED_TXTFILES (certs/emqxsl-ca.crt) */
extern const uint8_t emqx_ca_pem_start[] asm("_binary_emqxsl_ca_crt_start");
extern const uint8_t emqx_ca_pem_end[] asm("_binary_emqxsl_ca_crt_end");

/* ---- offline reading ring ---- */
static char *s_ring[READING_RING_LEN];
static size_t s_ring_head, s_ring_count;
static SemaphoreHandle_t s_ring_mtx;
static uint32_t s_ring_dropped;

/* ---- sync message reassembly (MQTT task only) ---- */
static char *s_rx_buf;
static int s_rx_total;
static bool s_rx_skip;
static bool s_rx_is_lot; /* current message arrived on s_topic_lot_ack */

/* request_id of the NEW LOT request we are waiting for ("" = none).
 * Written by the LVGL task, read by the MQTT task. */
static char s_lot_req_id[40];
static portMUX_TYPE s_lot_mux = portMUX_INITIALIZER_UNLOCKED;

/* ============================================================================
 * Small helpers
 * ========================================================================== */
static bool client_lock(TickType_t wait)
{
    return s_client_mtx && xSemaphoreTake(s_client_mtx, wait) == pdTRUE;
}

static void client_unlock(void)
{
    xSemaphoreGive(s_client_mtx);
}

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

/* ISO-8601 UTC string from time_t (second resolution) */
static void iso8601(time_t t, char *out, size_t len)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static uint32_t get_uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

static char *psram_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p)
        p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

/* Device id for payloads: configured id, else the MAC-derived one */
static void current_device_id(char *out, size_t len)
{
    weight_config_lock();
    const weight_config_t *cfg = weight_config_get();
    bool have = cfg->device_id[0] != '\0';
    if (have)
        strlcpy(out, cfg->device_id, len);
    weight_config_unlock();
    if (!have)
    {
        weight_state_snapshot_t snap;
        weight_state_get_snapshot(&snap);
        strlcpy(out, snap.device_id, len);
    }
}

static void build_topics_locked(void)
{
    weight_state_snapshot_t snap;
    weight_state_get_snapshot(&snap);
    current_device_id(s_dev_id, sizeof(s_dev_id));

    const char *t = snap.tenant_short;
    /* Readings go on the shared ingest topic, NOT {t}/{dev}/weight: the EMQX
     * rule only bridges {t}/devices/payload -> Kafka (gcl_devices_payload),
     * so anything on /weight never reached gokafka-raw. device_id is already
     * in the JSON body, so it doesn't need to be in the topic. */
    snprintf(s_topic_reading, sizeof(s_topic_reading), "%s/devices/payload", t);
    snprintf(s_topic_status, sizeof(s_topic_status), "%s/%s/status", t, s_dev_id);
    snprintf(s_topic_sync_req, sizeof(s_topic_sync_req), "%s/%s/sync/req", t, s_dev_id);
    snprintf(s_topic_sync_sub, sizeof(s_topic_sync_sub), "%s/models/sync/#", t);
    snprintf(s_topic_payload, sizeof(s_topic_payload), "%s/devices/payload", t);
    snprintf(s_topic_model_push, sizeof(s_topic_model_push), "%s/models/push", t); /* no device_id */
    snprintf(s_topic_lot_new, sizeof(s_topic_lot_new), "%s/%s/lot/new", t, s_dev_id);
    snprintf(s_topic_lot_get, sizeof(s_topic_lot_get), "%s/%s/lot/get", t, s_dev_id);
    snprintf(s_topic_lot_ack, sizeof(s_topic_lot_ack), "%s/%s/lot/ack", t, s_dev_id);

    ESP_LOGI(TAG, "topics: reading=%s status=%s sync_sub=%s",
             s_topic_reading, s_topic_status, s_topic_sync_sub);
}

static int enqueue_c(esp_mqtt_client_handle_t c, const char *topic,
                     const char *payload, int qos, int retain)
{
    if (!c || !payload)
        return -1;
    return esp_mqtt_client_enqueue(c, topic, payload, 0, qos, retain, true);
}

/* ============================================================================
 * Offline ring
 * ========================================================================== */
static void ring_push(char *payload_owned)
{
    if (!payload_owned)
        return;
    if (!s_ring_mtx)
    {
        free(payload_owned);
        return;
    }
    xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
    if (s_ring_count == READING_RING_LEN)
    {
        /* drop oldest */
        free(s_ring[s_ring_head]);
        s_ring[s_ring_head] = NULL;
        s_ring_head = (s_ring_head + 1) % READING_RING_LEN;
        s_ring_count--;
        s_ring_dropped++;
    }
    size_t tail = (s_ring_head + s_ring_count) % READING_RING_LEN;
    s_ring[tail] = payload_owned;
    s_ring_count++;
    size_t n = s_ring_count;
    xSemaphoreGive(s_ring_mtx);
    ESP_LOGW(TAG, "offline: reading buffered (%u pending, %lu dropped total)",
             (unsigned)n, (unsigned long)s_ring_dropped);
}

/* MQTT task only (on CONNECTED) */
static void ring_flush_c(esp_mqtt_client_handle_t c)
{
    if (!s_ring_mtx)
        return;
    int sent = 0;
    while (1)
    {
        xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
        if (s_ring_count == 0)
        {
            xSemaphoreGive(s_ring_mtx);
            break;
        }
        char *p = s_ring[s_ring_head];
        xSemaphoreGive(s_ring_mtx);

        if (enqueue_c(c, s_topic_reading, p, 1, 0) < 0)
            break; /* outbox full - keep the rest for the next connect */

        xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
        s_ring[s_ring_head] = NULL;
        s_ring_head = (s_ring_head + 1) % READING_RING_LEN;
        s_ring_count--;
        xSemaphoreGive(s_ring_mtx);
        free(p);
        sent++;
    }
    if (sent)
        ESP_LOGI(TAG, "flushed %d buffered readings", sent);
}

/* ============================================================================
 * Payload builders + *_c senders (no locking; caller guarantees client)
 * ========================================================================== */
static char *build_status_json(void)
{
    weight_state_snapshot_t snap;
    weight_state_get_snapshot(&snap);
    char dev[WCFG_MAX_DEVICE_ID_LEN];
    current_device_id(dev, sizeof(dev));

    bool is_running = (snap.mode == WEIGHT_MODE_RUN);
    char start_str[32] = "--";
    char stop_str[32] = "--";
    if (snap.run_started_ts > 0)
        iso8601(snap.run_started_ts, start_str, sizeof(start_str));
    if (snap.run_stopped_ts > 0)
        iso8601(snap.run_stopped_ts, stop_str, sizeof(stop_str));

    cJSON *root = cJSON_CreateObject();
    if (!root)
        return NULL;
    cJSON_AddBoolToObject(root, "online", true);
    cJSON_AddStringToObject(root, "device_id", dev);
    cJSON_AddStringToObject(root, "tenant_id", snap.tenant_id);
    cJSON_AddStringToObject(root, "machine_state", is_running ? "running" : "idle");
    cJSON_AddStringToObject(root, "machine_start", start_str);
    cJSON_AddStringToObject(root, "machine_stop", stop_str);
    cJSON_AddStringToObject(root, "current_model_id", snap.model.id);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

static int status_c(esp_mqtt_client_handle_t c)
{
    char *payload = build_status_json();
    if (!payload)
        return -1;
    int id = enqueue_c(c, s_topic_status, payload, 1, 1); /* retained */
    ESP_LOGD(TAG, "status -> %s: %s", s_topic_status, payload);
    free(payload);
    return id;
}

static int heartbeat_c(esp_mqtt_client_handle_t c)
{
    weight_state_snapshot_t snap;
    weight_state_get_snapshot(&snap);
    char dev[WCFG_MAX_DEVICE_ID_LEN];
    current_device_id(dev, sizeof(dev));

    char ts_str[32];
    iso8601(time(NULL), ts_str, sizeof(ts_str));

    cJSON *root = cJSON_CreateObject();
    if (!root)
        return -1;
    cJSON_AddStringToObject(root, "device_id", dev);
    cJSON_AddStringToObject(root, "tenant_id", snap.tenant_id);

    cJSON *status_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(status_obj, "fw", FW_VERSION);
    cJSON_AddStringToObject(status_obj, "ts", ts_str);
    cJSON_AddStringToObject(status_obj, "kind", "heartbeat");
    cJSON_AddNumberToObject(status_obj, "uptime_s", get_uptime_s());
    cJSON_AddNumberToObject(status_obj, "pending_readings", (double)s_ring_count);
    cJSON_AddItemToObject(root, "status", status_obj);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload)
        return -1;
    int id = enqueue_c(c, s_topic_payload, payload, 1, 0); /* not retained */
    ESP_LOGD(TAG, "heartbeat -> %s: %s", s_topic_payload, payload);
    free(payload);
    return id;
}

static int request_sync_c(esp_mqtt_client_handle_t c)
{
    return enqueue_c(c, s_topic_sync_req, "{\"action\":\"resync\"}", 1, 0);
}

/* Ask goweight-bridge for this machine's current active lot. The answer
 * comes back on s_topic_lot_ack with "query":true. */
static int lot_get_c(esp_mqtt_client_handle_t c)
{
    char machine_id[WCFG_MAX_MACHINE_ID_LEN];
    weight_config_lock();
    strlcpy(machine_id, weight_config_get()->machine_id, sizeof(machine_id));
    weight_config_unlock();
    if (machine_id[0] == '\0')
    {
        weight_state_clear_lot(); /* no machine -> nothing to show */
        return -1;
    }

    char req_id[37];
    weight_model_store_new_id(req_id, sizeof(req_id));

    cJSON *root = cJSON_CreateObject();
    if (!root)
        return -1;
    cJSON_AddStringToObject(root, "request_id", req_id);
    cJSON_AddStringToObject(root, "machine_id", machine_id);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload)
        return -1;
    int id = enqueue_c(c, s_topic_lot_get, payload, 1, 0);
    ESP_LOGI(TAG, "lot/get -> %s (msg %d)", s_topic_lot_get, id);
    free(payload);
    return id;
}

/* ---- lot/ack ---- */

/* Parse "2026-10-27T00:36:45.583293+00:00" (Postgres timestamptz in JSON) to
 * epoch seconds. Fraction ignored; numeric offset honoured. 0 on failure. */
static time_t parse_iso8601(const char *s)
{
    int y, mo, d, h, mi, sec;
    if (!s || sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &sec) != 6)
        return 0;

    /* days from civil (Howard Hinnant), no timegm() dependency */
    int yy = y - (mo <= 2);
    int era = (yy >= 0 ? yy : yy - 399) / 400;
    unsigned yoe = (unsigned)(yy - era * 400);
    unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    int64_t t = days * 86400 + h * 3600 + mi * 60 + sec;

    /* offset: find +HH:MM / -HH:MM after the seconds, or Z */
    const char *p = s + 19;
    while (*p == '.' || (*p >= '0' && *p <= '9'))
        p++;
    if (*p == '+' || *p == '-')
    {
        int oh = 0, om = 0;
        sscanf(p + 1, "%2d:%2d", &oh, &om);
        int off = oh * 3600 + om * 60;
        t += (*p == '+') ? -off : off;
    }
    return (time_t)t;
}

static void handle_lot_ack_json(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root)
    {
        ESP_LOGW(TAG, "lot/ack: invalid JSON");
        return;
    }

    const cJSON *req = cJSON_GetObjectItem(root, "request_id");
    bool ok = cJSON_IsTrue(cJSON_GetObjectItem(root, "ok"));
    bool query = cJSON_IsTrue(cJSON_GetObjectItem(root, "query"));
    const cJSON *lot = cJSON_GetObjectItem(root, "lot");
    const cJSON *err = cJSON_GetObjectItem(root, "error");

    /* Is this the answer to the NEW LOT request we're waiting for? */
    bool mine = false;
    if (!query && cJSON_IsString(req))
    {
        portENTER_CRITICAL(&s_lot_mux);
        mine = s_lot_req_id[0] && strcmp(s_lot_req_id, req->valuestring) == 0;
        if (mine)
            s_lot_req_id[0] = '\0';
        portEXIT_CRITICAL(&s_lot_mux);
    }

    if (ok && cJSON_IsObject(lot))
    {
        const cJSON *num = cJSON_GetObjectItem(lot, "lot_number");
        const cJSON *st = cJSON_GetObjectItem(lot, "status");
        const cJSON *ca = cJSON_GetObjectItem(lot, "created_at");
        bool active = !cJSON_IsString(st) || strcmp(st->valuestring, "active") == 0;
        if (cJSON_IsNumber(num))
            weight_state_set_lot(active, (int64_t)num->valuedouble,
                                 cJSON_IsString(ca) ? parse_iso8601(ca->valuestring) : 0);
    }
    else if (ok && query)
    {
        weight_state_set_lot(false, 0, 0); /* machine has no active lot */
    }

    if (mine)
        weight_state_lot_request_finish(ok ? WEIGHT_LOT_RESULT_OK : WEIGHT_LOT_RESULT_ERROR);

    if (!ok)
        ESP_LOGW(TAG, "lot/ack error: %s", cJSON_IsString(err) ? err->valuestring : "?");

    cJSON_Delete(root);
}

/* ============================================================================
 * Sync ingestion (MQTT task)
 * ========================================================================== */
static void handle_sync_json(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root)
    {
        ESP_LOGW(TAG, "sync: invalid JSON (%u bytes)", (unsigned)strlen(json));
        return;
    }

    /* Bridge sends {"timestamp":"...","models":[{...},{...}]} or a single
     * model object. Batch => one NVS commit for the whole message. */
    weight_model_store_begin_batch();
    int applied = 0, failed = 0;
    cJSON *models_arr = cJSON_GetObjectItem(root, "models");
    if (cJSON_IsArray(models_arr))
    {
        cJSON *item;
        cJSON_ArrayForEach(item, models_arr)
        {
            if (weight_model_store_apply_remote_item(item) == ESP_OK)
                applied++;
            else
                failed++;
        }
    }
    else
    {
        if (weight_model_store_apply_remote_item(root) == ESP_OK)
            applied++;
        else
            failed++;
    }
    esp_err_t err = weight_model_store_end_batch();
    cJSON_Delete(root);

    ESP_LOGI(TAG, "sync: %d applied, %d failed, commit=%s",
             applied, failed, esp_err_to_name(err));
}

static void on_data(esp_mqtt_event_handle_t ev)
{
    /* esp-mqtt splits messages larger than its buffer into several
     * MQTT_EVENT_DATA events; topic is only set on the first one. */
    if (ev->current_data_offset == 0)
    {
        free(s_rx_buf);
        s_rx_buf = NULL;
        s_rx_skip = false;
        s_rx_total = ev->total_data_len;

        ESP_LOGD(TAG, "data on '%.*s' (%d bytes)", ev->topic_len, ev->topic, ev->total_data_len);
        s_rx_is_lot = (size_t)ev->topic_len == strlen(s_topic_lot_ack) &&
                      memcmp(ev->topic, s_topic_lot_ack, ev->topic_len) == 0;

        if (s_rx_total <= 0 || s_rx_total > SYNC_MSG_MAX)
        {
            ESP_LOGW(TAG, "sync message too large (%d), ignored", s_rx_total);
            s_rx_skip = true;
            return;
        }
        s_rx_buf = heap_caps_malloc(s_rx_total + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_rx_buf)
            s_rx_buf = malloc(s_rx_total + 1);
        if (!s_rx_buf)
        {
            ESP_LOGE(TAG, "no memory for %d-byte sync message", s_rx_total);
            s_rx_skip = true;
            return;
        }
    }

    if (s_rx_skip || !s_rx_buf)
        return;
    if (ev->current_data_offset + ev->data_len > s_rx_total)
    {
        ESP_LOGW(TAG, "fragment overflow, dropping message");
        s_rx_skip = true;
        free(s_rx_buf);
        s_rx_buf = NULL;
        return;
    }

    memcpy(s_rx_buf + ev->current_data_offset, ev->data, ev->data_len);
    if (ev->current_data_offset + ev->data_len < s_rx_total)
        return; /* more fragments coming */

    s_rx_buf[s_rx_total] = '\0';
    if (s_rx_is_lot)
        handle_lot_ack_json(s_rx_buf);
    else
        handle_sync_json(s_rx_buf);
    free(s_rx_buf);
    s_rx_buf = NULL;
}

/* ============================================================================
 * Event handler (runs in the MQTT task - never take s_client_mtx here)
 * ========================================================================== */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t c = event->client;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        weight_state_set_mqtt(true);
        ESP_LOGI(TAG, "connected - subscribing to '%s'", s_topic_sync_sub);
        esp_mqtt_client_subscribe(c, s_topic_sync_sub, 1);
        esp_mqtt_client_subscribe(c, s_topic_lot_ack, 1);
        status_c(c);
        heartbeat_c(c);
        request_sync_c(c);
        lot_get_c(c); /* subscribe is queued first, so the answer isn't missed */
        ring_flush_c(c);
        if (s_heartbeat_timer)
        {
            esp_timer_stop(s_heartbeat_timer); /* ignore "not running" */
            esp_timer_start_periodic(s_heartbeat_timer, HEARTBEAT_PERIOD_US);
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        weight_state_set_mqtt(false);
        ESP_LOGW(TAG, "disconnected");
        if (s_heartbeat_timer)
            esp_timer_stop(s_heartbeat_timer);
        break;

    case MQTT_EVENT_DATA:
        on_data(event);
        break;

    case MQTT_EVENT_ERROR:
        if (event->error_handle)
            ESP_LOGE(TAG, "MQTT error type=%d", event->error_handle->error_type);
        break;

    default:
        break;
    }
}

/* esp_timer task: must not block. enqueue is non-blocking; lock is try-only. */
static void heartbeat_timer_cb(void *arg)
{
    if (!client_lock(0))
        return; /* restart in progress - skip this beat */
    if (s_client && s_connected)
        heartbeat_c(s_client);
    client_unlock();
}

/* weight_state change callback: mode or active model changed */
static void on_state_change(void)
{
    weight_mqtt_publish_status();
}

/* ============================================================================
 * Client lifecycle
 * ========================================================================== */
static esp_err_t start_locked(void)
{
    if (s_client)
        return ESP_OK; /* esp-mqtt handles its own reconnection */

    weight_config_t cfg;
    weight_config_copy(&cfg);

    /* Refuse to start with an empty broker host - esp-mqtt would reject
     * "mqtts://:8883" with "Error parse uri". */
    if (cfg.mqtt_host[0] == '\0')
    {
        ESP_LOGI(TAG, "broker host not configured - skipping MQTT start");
        return ESP_ERR_INVALID_STATE;
    }

    /* TLS cert selection:
     *   1. custom CA cert stored in NVS (self-hosted EMQX, self-signed)
     *   2. otherwise the embedded bundle (EMQX Cloud / public CAs) */
    const char *ca_cert = NULL;
    if (cfg.mqtt_tls)
    {
        if (!s_ca_pem)
        {
            s_ca_pem = malloc(WCFG_MAX_CA_CERT_LEN);
            if (s_ca_pem)
            {
                size_t actual = 0;
                if (weight_config_get_ca_cert(s_ca_pem, WCFG_MAX_CA_CERT_LEN, &actual) != ESP_OK)
                {
                    free(s_ca_pem);
                    s_ca_pem = NULL;
                }
            }
        }
        ca_cert = s_ca_pem ? s_ca_pem : (const char *)emqx_ca_pem_start;
        ESP_LOGI(TAG, "TLS: using %s CA", s_ca_pem ? "custom NVS" : "embedded");
    }

    /* LWT - retained on /status. Same device id as the topics. */
    char lwt[160];
    snprintf(lwt, sizeof(lwt), "{\"device_id\":\"%s\",\"online\":false}", s_dev_id);

    char uri[128];
    snprintf(uri, sizeof(uri), "%s://%s:%u",
             cfg.mqtt_tls ? "mqtts" : "mqtt", cfg.mqtt_host, (unsigned)cfg.mqtt_port);

    /* esp-mqtt copies uri / credentials / client id / LWT during init,
     * so stack buffers are fine here. The CA pointer is NOT copied. */
    esp_mqtt_client_config_t mc = {
        .broker.address.uri = uri,
        .credentials.username = cfg.mqtt_user,
        .credentials.authentication.password = cfg.mqtt_pass,
        .credentials.client_id = s_dev_id,
        .session.last_will = {
            .topic = s_topic_status,
            .msg = lwt,
            .msg_len = (int)strlen(lwt),
            .qos = 1,
            .retain = 1,
        },
        .session.keepalive = 30,
    };
    if (ca_cert)
        mc.broker.verification.certificate = ca_cert;

    s_client = esp_mqtt_client_init(&mc);
    if (!s_client)
        return ESP_FAIL;

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    ESP_LOGI(TAG, "starting client: %s (tls=%d)", uri, cfg.mqtt_tls);

    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK)
    {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    return err;
}

esp_err_t weight_mqtt_init(void)
{
    /* Phase 1: internal state only. The client is created later, when WiFi
     * has an IP (weight_wifi) or the broker is configured (settings UI). */
    s_client_mtx = xSemaphoreCreateMutex();
    s_ring_mtx = xSemaphoreCreateMutex();
    if (!s_client_mtx || !s_ring_mtx)
    {
        ESP_LOGE(TAG, "failed to create mqtt mutexes");
        return ESP_ERR_NO_MEM;
    }

    esp_timer_create_args_t args = {
        .callback = heartbeat_timer_cb,
        .name = "mqtt_hb",
    };
    esp_err_t err = esp_timer_create(&args, &s_heartbeat_timer);
    if (err != ESP_OK)
        return err;

    client_lock(portMAX_DELAY);
    build_topics_locked();
    client_unlock();

    weight_state_set_change_cb(on_state_change);

    ESP_LOGI(TAG, "init (deferred start - waits for WiFi + broker config)");
    return ESP_OK;
}

esp_err_t weight_mqtt_start_if_ready(void)
{
    if (!client_lock(LOCK_WAIT_RESTART))
        return ESP_ERR_TIMEOUT;
    esp_err_t err = start_locked();
    client_unlock();
    return err;
}

esp_err_t weight_mqtt_restart(void)
{
    if (!s_client_mtx)
    {
        ESP_LOGE(TAG, "restart called before weight_mqtt_init()");
        return ESP_ERR_INVALID_STATE;
    }
    if (!client_lock(LOCK_WAIT_RESTART))
    {
        ESP_LOGW(TAG, "restart: could not acquire mqtt lock, skipping "
                      "(another restart is likely already in progress)");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "restart - rebuilding topics and client");
    if (s_heartbeat_timer)
        esp_timer_stop(s_heartbeat_timer);

    if (s_client)
    {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    s_connected = false;
    weight_state_set_mqtt(false);

    build_topics_locked();
    esp_err_t err = start_locked();
    client_unlock();
    return err;
}

bool weight_mqtt_is_connected(void)
{
    return s_connected;
}

/* ============================================================================
 * Public publishers (any task, non-blocking)
 * ========================================================================== */
esp_err_t weight_mqtt_publish_reading(float value, weight_status_t status, time_t ts)
{
    weight_state_snapshot_t snap;
    weight_state_get_snapshot(&snap);
    char dev[WCFG_MAX_DEVICE_ID_LEN];
    current_device_id(dev, sizeof(dev));
    wcfg_unit_t unit = weight_config_get()->unit;

    bool ts_valid = ts >= WEIGHT_TIME_VALID_EPOCH;
    char ts_str[32];
    iso8601(ts, ts_str, sizeof(ts_str));

    /* Round all floats to 3 decimal places */
    char s_reading[16], s_lower[16], s_standard[16], s_upper[16];
    snprintf(s_reading, sizeof(s_reading), "%.3f", value);
    snprintf(s_lower, sizeof(s_lower), "%.3f", snap.model.lower_limit);
    snprintf(s_standard, sizeof(s_standard), "%.3f", snap.model.standard);
    snprintf(s_upper, sizeof(s_upper), "%.3f", snap.model.upper_limit);

    cJSON *root = cJSON_CreateObject();
    if (!root)
        return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "device_id", dev);
    cJSON_AddStringToObject(root, "tenant_id", snap.tenant_id);
    cJSON_AddStringToObject(root, "timestamp", ts_str);
    if (!ts_valid)
    {
        /* Clock not synced yet: tell the bridge not to trust "timestamp" */
        cJSON_AddBoolToObject(root, "ts_valid", false);
        cJSON_AddNumberToObject(root, "uptime_ms", (double)(esp_timer_get_time() / 1000));
    }

    cJSON *reading_obj = cJSON_CreateObject();
    cJSON_AddRawToObject(reading_obj, "value", s_reading);
    cJSON_AddStringToObject(reading_obj, "unit", weight_config_unit_str(unit));
    cJSON_AddStringToObject(reading_obj, "status", status_str(status));
    cJSON_AddStringToObject(reading_obj, "model_id", snap.model.id);
    cJSON_AddStringToObject(reading_obj, "model_name", snap.model.name);
    cJSON_AddItemToObject(root, "readings", reading_obj);

    cJSON *lim = cJSON_CreateObject();
    cJSON_AddRawToObject(lim, "lower", s_lower);
    cJSON_AddRawToObject(lim, "standard", s_standard);
    cJSON_AddRawToObject(lim, "upper", s_upper);
    cJSON_AddItemToObject(root, "limits", lim);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload)
        return ESP_ERR_NO_MEM;

    int msg_id = -1;
    if (client_lock(LOCK_WAIT_PUBLISH))
    {
        if (s_client && s_connected)
            msg_id = enqueue_c(s_client, s_topic_reading, payload, 1, 0);
        client_unlock();
    }

    if (msg_id >= 0)
    {
        ESP_LOGI(TAG, "reading queued %s %s (msg %d)", s_reading, status_str(status), msg_id);
        ESP_LOGD(TAG, "payload: %s", payload);
        free(payload);
        return ESP_OK;
    }

    /* Not connected / restarting / outbox refused: keep it for later */
    ring_push(psram_strdup(payload));
    free(payload);
    return ESP_ERR_INVALID_STATE;
}

esp_err_t weight_mqtt_publish_status(void)
{
    if (!client_lock(LOCK_WAIT_PUBLISH))
        return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_client && s_connected)
        err = status_c(s_client) >= 0 ? ESP_OK : ESP_FAIL;
    client_unlock();
    return err;
}

esp_err_t weight_mqtt_publish_heartbeat(void)
{
    if (!client_lock(LOCK_WAIT_PUBLISH))
        return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_client && s_connected)
        err = heartbeat_c(s_client) >= 0 ? ESP_OK : ESP_FAIL;
    client_unlock();
    return err;
}

esp_err_t weight_mqtt_request_sync(void)
{
    if (!client_lock(LOCK_WAIT_PUBLISH))
        return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_client && s_connected)
        err = request_sync_c(s_client) >= 0 ? ESP_OK : ESP_FAIL;
    client_unlock();
    return err;
}

static esp_err_t publish_model_json(cJSON *root)
{
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload)
        return ESP_ERR_NO_MEM;

    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (client_lock(LOCK_WAIT_PUBLISH))
    {
        if (s_client && s_connected)
            err = enqueue_c(s_client, s_topic_model_push, payload, 1, 0) >= 0 ? ESP_OK : ESP_FAIL;
        client_unlock();
    }
    else
        err = ESP_ERR_TIMEOUT;

    ESP_LOGD(TAG, "model push -> %s: %s (%s)", s_topic_model_push, payload, esp_err_to_name(err));
    free(payload);
    return err;
}

esp_err_t weight_mqtt_publish_model_push(const weight_model_t *m)
{
    if (!m)
        return ESP_ERR_INVALID_ARG;

    char s_lower[16], s_standard[16], s_upper[16];
    snprintf(s_lower, sizeof(s_lower), "%.3f", m->lower_limit);
    snprintf(s_standard, sizeof(s_standard), "%.3f", m->standard);
    snprintf(s_upper, sizeof(s_upper), "%.3f", m->upper_limit);

    cJSON *root = cJSON_CreateObject();
    if (!root)
        return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "id", m->id);
    cJSON_AddStringToObject(root, "name", m->name);
    cJSON_AddRawToObject(root, "lower_limit", s_lower);
    cJSON_AddRawToObject(root, "standard", s_standard);
    cJSON_AddRawToObject(root, "upper_limit", s_upper);
    cJSON_AddStringToObject(root, "unit", weight_config_unit_str(weight_config_get()->unit));
    if (m->updated_at)
        cJSON_AddNumberToObject(root, "updated_at", (double)m->updated_at);
    return publish_model_json(root);
}

esp_err_t weight_mqtt_publish_model_delete(const char *id)
{
    if (!id || !id[0])
        return ESP_ERR_INVALID_ARG;
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return ESP_ERR_NO_MEM;
    /* Reuse models/push topic with a "deleted" flag */
    cJSON_AddStringToObject(root, "id", id);
    cJSON_AddBoolToObject(root, "deleted", true);
    return publish_model_json(root);
}

static void make_uuid(char *out, size_t len)
{
    weight_model_store_new_id(out, len);
}

esp_err_t weight_mqtt_publish_new_lot(void)
{
    char machine_id[WCFG_MAX_MACHINE_ID_LEN];
    weight_config_lock();
    strlcpy(machine_id, weight_config_get()->machine_id, sizeof(machine_id));
    weight_config_unlock();
    if (machine_id[0] == '\0')
        return ESP_ERR_NOT_FOUND;
    if (!s_connected)
        return ESP_ERR_INVALID_STATE;

    weight_state_snapshot_t snap;
    weight_state_get_snapshot(&snap);
    char dev[WCFG_MAX_DEVICE_ID_LEN];
    current_device_id(dev, sizeof(dev));

    char req_id[37];
    make_uuid(req_id, sizeof(req_id));
    char ts_str[32];
    iso8601(time(NULL), ts_str, sizeof(ts_str));

    cJSON *root = cJSON_CreateObject();
    if (!root)
        return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "request_id", req_id); /* for dedupe on QoS1 redelivery */
    cJSON_AddStringToObject(root, "machine_id", machine_id);
    cJSON_AddStringToObject(root, "device_id", dev);
    cJSON_AddStringToObject(root, "tenant_id", snap.tenant_id);
    cJSON_AddStringToObject(root, "requested_at", ts_str);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload)
        return ESP_ERR_NO_MEM;

    /* Remember the id before sending so a fast ack can't beat us to it */
    portENTER_CRITICAL(&s_lot_mux);
    strlcpy(s_lot_req_id, req_id, sizeof(s_lot_req_id));
    portEXIT_CRITICAL(&s_lot_mux);

    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (client_lock(LOCK_WAIT_PUBLISH))
    {
        if (s_client && s_connected)
            err = enqueue_c(s_client, s_topic_lot_new, payload, 1, 0) >= 0 ? ESP_OK : ESP_FAIL;
        client_unlock();
    }
    else
        err = ESP_ERR_TIMEOUT;

    if (err == ESP_OK)
        weight_state_lot_request_begin();
    else
    {
        portENTER_CRITICAL(&s_lot_mux);
        s_lot_req_id[0] = '\0';
        portEXIT_CRITICAL(&s_lot_mux);
    }

    ESP_LOGI(TAG, "new lot -> '%s': %s", s_topic_lot_new, esp_err_to_name(err));
    free(payload);
    return err;
}

esp_err_t weight_mqtt_request_lot(void)
{
    if (!client_lock(LOCK_WAIT_PUBLISH))
        return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_client && s_connected)
        err = lot_get_c(s_client) >= 0 ? ESP_OK : ESP_FAIL;
    client_unlock();
    return err;
}

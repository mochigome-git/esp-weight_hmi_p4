/**
 * @file weight_wifi.c
 * @brief WiFi station for ESP32-P4 + ESP32-C6 (Waveshare 10.1" board).
 *
 * The radio lives on the C6; esp_wifi_remote forwards esp_wifi_* calls over
 * SDIO (esp_hosted 1.4.* / esp_wifi_remote 0.14.* - must match the C6 slave
 * firmware). lwIP / esp_netif run on the P4.
 *
 * Architecture:
 *   All esp_wifi_* calls happen in ONE task ("wifi_mgr"). The esp_event
 *   handler and timers only post messages to its queue, and the public API
 *   posts commands. That removes every connect/disconnect/scan race and
 *   keeps slow SDIO RPCs out of the event loop and the LVGL task.
 *
 * Behaviour:
 *  - Scan does NOT drop a working connection. Only an in-flight connect
 *    attempt is paused (the radio can't scan while associating) and is
 *    resumed right after.
 *  - New credentials are tried first and saved to NVS only after GOT_IP.
 *    After NEW_CREDS_MAX_FAILS failures we revert to the saved network.
 *  - Reconnect uses exponential backoff 1 s -> 30 s, reset on success.
 *  - User Disconnect turns auto-reconnect off until Reconnect/Connect/reboot.
 *  - Static IP is restored from config at boot and re-applied on every
 *    WIFI_EVENT_STA_CONNECTED (the default netif glue restarts DHCP there).
 */

#include <string.h>
#include <stdlib.h>
#include "weight_wifi.h"
#include "weight_state.h"
#include "weight_config.h"
#include "weight_mqtt.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"

static const char *TAG = "wifi";

#define RECONNECT_MIN_MS 1000
#define RECONNECT_MAX_MS 30000
#define NEW_CREDS_MAX_FAILS 3
#define RSSI_POLL_MS 10000
#define SCAN_FETCH_MAX 32
#define SCAN_WAIT_MS 15000
#define CMD_QUEUE_LEN 16
#define WORKER_STACK 4096
#define WORKER_PRIO 5

/* ---------------------------------------------------------------------------
 * Messages to the worker
 * --------------------------------------------------------------------------- */
typedef enum
{
    EV_STA_START,
    EV_STA_CONNECTED,
    EV_DISCONNECTED,
    EV_GOT_IP,
    EV_LOST_IP,
    CMD_RETRY,
    CMD_POLL,
    CMD_CONNECT,
    CMD_DISCONNECT,
    CMD_RECONNECT,
    CMD_SCAN,
} wifi_msg_type_t;

typedef struct
{
    wifi_msg_type_t type;
    union
    {
        uint16_t reason;
        uint32_t ip;
        struct
        {
            char ssid[33];
            char pass[65];
        } creds;
    } u;
} wifi_msg_t;

/* ---------------------------------------------------------------------------
 * State
 * --------------------------------------------------------------------------- */
static QueueHandle_t s_q;
static esp_netif_t *s_netif_sta;
static esp_timer_handle_t s_retry_timer;
static esp_timer_handle_t s_poll_timer;

static SemaphoreHandle_t s_scan_mutex; /* serialises weight_wifi_scan() callers */
static SemaphoreHandle_t s_scan_done;  /* worker -> caller */
static wifi_scan_entry_t s_scan_buf[WIFI_MAX_SCAN_RESULTS];
static size_t s_scan_count;

/* Worker-owned: only touched from the wifi_mgr task (and init before it starts) */
static bool s_has_creds;
static bool s_user_disconnected;
static bool s_trying_new;
static bool s_ignore_leave; /* swallow the ASSOC_LEAVE caused by our own disconnect */
static int s_new_fail_count;
static uint32_t s_backoff_ms = RECONNECT_MIN_MS;
static char s_new_ssid[33];
static char s_new_pass[65];
static char s_mqtt_ssid[33]; /* network MQTT was last fully (re)started on */
static volatile bool s_mqtt_restart_pending;

/* Shared with UI - guarded by s_info_mux */
static portMUX_TYPE s_info_mux = portMUX_INITIALIZER_UNLOCKED;
static weight_wifi_info_t s_info;

typedef struct
{
    bool enabled;
    char ip[16];
    char gw[16];
    char netmask[16];
    char dns[16];
} static_ip_cfg_t;

static static_ip_cfg_t s_static_cfg = {0};

/* ---------------------------------------------------------------------------
 * Small helpers
 * --------------------------------------------------------------------------- */
static void post(const wifi_msg_t *m)
{
    if (!s_q || xQueueSend(s_q, m, 0) != pdTRUE)
        ESP_LOGW(TAG, "queue full - dropped msg %d", (int)m->type);
}

static esp_err_t post_from_api(const wifi_msg_t *m)
{
    if (!s_q)
        return ESP_ERR_INVALID_STATE;
    return xQueueSend(s_q, m, pdMS_TO_TICKS(100)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static const char *target_ssid(void)
{
    return s_trying_new ? s_new_ssid : weight_config_get()->wifi_ssid;
}

static weight_wifi_state_t get_state(void)
{
    taskENTER_CRITICAL(&s_info_mux);
    weight_wifi_state_t st = s_info.state;
    taskEXIT_CRITICAL(&s_info_mux);
    return st;
}

static void set_state(weight_wifi_state_t st)
{
    char ssid[33] = "";
    if (st != WEIGHT_WIFI_NO_CREDS)
        strlcpy(ssid, target_ssid(), sizeof(ssid));

    taskENTER_CRITICAL(&s_info_mux);
    s_info.state = st;
    strlcpy(s_info.ssid, ssid, sizeof(s_info.ssid));
    if (st != WEIGHT_WIFI_CONNECTED)
    {
        s_info.ip[0] = '\0';
        s_info.rssi = 0;
    }
    taskEXIT_CRITICAL(&s_info_mux);
}

static weight_wifi_fail_t classify_reason(uint16_t r)
{
    switch (r)
    {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return WEIGHT_WIFI_FAIL_AUTH;
    case WIFI_REASON_NO_AP_FOUND:
        return WEIGHT_WIFI_FAIL_NOT_FOUND;
    default:
        return WEIGHT_WIFI_FAIL_OTHER;
    }
}

static esp_err_t apply_sta_config(const char *ssid, const char *pass)
{
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass ? pass : "", sizeof(wc.sta.password));
    /* Threshold = minimum accepted security. Open network only when no
     * password; otherwise WPA2 or better (WPA3 still accepted). */
    wc.sta.threshold.authmode = (pass && pass[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    /* Several APs with the same SSID (factory roaming setups): scan all
     * channels and join the strongest instead of the first one found. */
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "set_config('%s') failed: %s", ssid, esp_err_to_name(err));
    return err;
}

/* ---------------------------------------------------------------------------
 * Static IP (called from the event handler on STA_CONNECTED, and from
 * weight_wifi_set_static_ip) - esp_netif only, no radio RPC.
 * --------------------------------------------------------------------------- */
static void apply_static_ip_if_configured(void)
{
    if (!s_static_cfg.enabled || !s_netif_sta)
        return;

    esp_err_t err = esp_netif_dhcpc_stop(s_netif_sta);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
    {
        ESP_LOGE(TAG, "dhcpc_stop failed: %s", esp_err_to_name(err));
        return;
    }

    esp_netif_ip_info_t ip_info = {0};
    ip_info.ip.addr = ipaddr_addr(s_static_cfg.ip);
    ip_info.gw.addr = ipaddr_addr(s_static_cfg.gw);
    ip_info.netmask.addr = ipaddr_addr(s_static_cfg.netmask);

    err = esp_netif_set_ip_info(s_netif_sta, &ip_info);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "set_ip_info failed: %s", esp_err_to_name(err));
        return;
    }

    if (s_static_cfg.dns[0] != '\0')
    {
        esp_netif_dns_info_t dns_info = {0};
        dns_info.ip.type = ESP_IPADDR_TYPE_V4;
        dns_info.ip.u_addr.ip4.addr = ipaddr_addr(s_static_cfg.dns);
        esp_netif_set_dns_info(s_netif_sta, ESP_NETIF_DNS_MAIN, &dns_info);
    }

    ESP_LOGI(TAG, "static ip applied: %s gw %s mask %s", s_static_cfg.ip,
             s_static_cfg.gw, s_static_cfg.netmask);
}

/* ---------------------------------------------------------------------------
 * MQTT kick
 * --------------------------------------------------------------------------- */
static void mqtt_restart_task(void *arg)
{
    weight_mqtt_restart();
    s_mqtt_restart_pending = false;
    vTaskDelete(NULL);
}

static void kick_mqtt(const char *ssid)
{
    /* Full restart (also rebuilds topics from the configured device_id) only
     * on the first connect or when the network changed. Reconnects to the
     * same network are handled by esp-mqtt's own retry - no TLS churn. */
    if (s_mqtt_ssid[0] && strcmp(s_mqtt_ssid, ssid) == 0)
        return;
    if (s_mqtt_restart_pending)
        return;

    strlcpy(s_mqtt_ssid, ssid, sizeof(s_mqtt_ssid));
    s_mqtt_restart_pending = true;
    if (xTaskCreate(mqtt_restart_task, "mqtt_start", 4096, NULL, 5, NULL) != pdPASS)
    {
        s_mqtt_restart_pending = false;
        s_mqtt_ssid[0] = '\0';
        ESP_LOGE(TAG, "could not start mqtt_restart_task");
    }
}

/* ---------------------------------------------------------------------------
 * Worker actions
 * --------------------------------------------------------------------------- */
static void schedule_retry(void)
{
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, (uint64_t)s_backoff_ms * 1000ULL);
    ESP_LOGI(TAG, "retry in %lu ms", (unsigned long)s_backoff_ms);
    s_backoff_ms *= 2;
    if (s_backoff_ms > RECONNECT_MAX_MS)
        s_backoff_ms = RECONNECT_MAX_MS;
}

static void start_connect(void)
{
    esp_timer_stop(s_retry_timer);
    set_state(WEIGHT_WIFI_CONNECTING);
    ESP_LOGI(TAG, "connecting to '%s'", target_ssid());
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
        schedule_retry();
    }
}

/* Drop the current link/attempt without triggering the retry logic. */
static void quiet_disconnect(void)
{
    weight_wifi_state_t st = get_state();
    if (st == WEIGHT_WIFI_CONNECTED || st == WEIGHT_WIFI_CONNECTING)
    {
        s_ignore_leave = true;
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(150)); /* let the driver settle */
    }
}

static void revert_to_saved(uint16_t reason)
{
    ESP_LOGW(TAG, "new network '%s' failed (reason %u) - reverting",
             s_new_ssid, (unsigned)reason);

    taskENTER_CRITICAL(&s_info_mux);
    s_info.new_creds_failed = true;
    s_info.fail = classify_reason(reason);
    strlcpy(s_info.failed_ssid, s_new_ssid, sizeof(s_info.failed_ssid));
    taskEXIT_CRITICAL(&s_info_mux);

    s_trying_new = false;
    memset(s_new_pass, 0, sizeof(s_new_pass));
    s_backoff_ms = RECONNECT_MIN_MS;

    const weight_config_t *cfg = weight_config_get();
    if (cfg->wifi_ssid[0] && apply_sta_config(cfg->wifi_ssid, cfg->wifi_pass) == ESP_OK)
    {
        s_has_creds = true;
        start_connect();
    }
    else
    {
        s_has_creds = false;
        set_state(WEIGHT_WIFI_NO_CREDS);
    }
}

static void on_got_ip(uint32_t ip)
{
    esp_timer_stop(s_retry_timer);
    s_backoff_ms = RECONNECT_MIN_MS;
    s_ignore_leave = false;

    if (s_trying_new)
    {
        weight_config_set_wifi(s_new_ssid, s_new_pass);
        weight_config_save();
        ESP_LOGI(TAG, "new network '%s' verified - saved", s_new_ssid);
        s_trying_new = false;
        memset(s_new_pass, 0, sizeof(s_new_pass));
    }

    esp_ip4_addr_t a = {.addr = ip};
    char ipbuf[16];
    snprintf(ipbuf, sizeof(ipbuf), IPSTR, IP2STR(&a));

    wifi_ap_record_t ap;
    int8_t rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        rssi = ap.rssi;

    set_state(WEIGHT_WIFI_CONNECTED);
    taskENTER_CRITICAL(&s_info_mux);
    strlcpy(s_info.ip, ipbuf, sizeof(s_info.ip));
    s_info.rssi = rssi;
    s_info.static_ip = s_static_cfg.enabled;
    s_info.last_reason = 0;
    s_info.new_creds_failed = false;
    s_info.fail = WEIGHT_WIFI_FAIL_NONE;
    taskEXIT_CRITICAL(&s_info_mux);

    weight_state_set_wifi(true);
    ESP_LOGI(TAG, "connected to '%s', ip %s, rssi %d", weight_config_get()->wifi_ssid,
             ipbuf, rssi);

    kick_mqtt(weight_config_get()->wifi_ssid);
}

static void on_disconnected(uint16_t reason)
{
    weight_state_set_wifi(false);

    if (reason == WIFI_REASON_ASSOC_LEAVE && s_ignore_leave)
    {
        s_ignore_leave = false;
        ESP_LOGI(TAG, "disconnected (own request)");
        return;
    }

    ESP_LOGW(TAG, "disconnected, reason %u", (unsigned)reason);
    taskENTER_CRITICAL(&s_info_mux);
    s_info.last_reason = reason;
    taskEXIT_CRITICAL(&s_info_mux);

    if (!s_has_creds)
    {
        set_state(WEIGHT_WIFI_NO_CREDS);
        return;
    }
    if (s_user_disconnected)
    {
        set_state(WEIGHT_WIFI_DISCONNECTED);
        return;
    }
    if (s_trying_new && ++s_new_fail_count >= NEW_CREDS_MAX_FAILS)
    {
        revert_to_saved(reason);
        return;
    }

    set_state(WEIGHT_WIFI_CONNECTING);
    schedule_retry();
}

static void do_connect_new(const char *ssid, const char *pass)
{
    ESP_LOGI(TAG, "trying new network '%s'", ssid);
    esp_timer_stop(s_retry_timer);
    quiet_disconnect();

    strlcpy(s_new_ssid, ssid, sizeof(s_new_ssid));
    strlcpy(s_new_pass, pass, sizeof(s_new_pass));
    s_trying_new = true;
    s_new_fail_count = 0;
    s_user_disconnected = false;
    s_backoff_ms = RECONNECT_MIN_MS;

    taskENTER_CRITICAL(&s_info_mux);
    s_info.new_creds_failed = false;
    s_info.fail = WEIGHT_WIFI_FAIL_NONE;
    taskEXIT_CRITICAL(&s_info_mux);

    if (apply_sta_config(ssid, pass) != ESP_OK)
    {
        revert_to_saved(0);
        return;
    }
    s_has_creds = true;
    start_connect();
}

static void do_user_disconnect(void)
{
    ESP_LOGI(TAG, "user disconnect - auto-reconnect off");
    esp_timer_stop(s_retry_timer);
    quiet_disconnect();
    s_user_disconnected = true;
    if (s_trying_new)
    {
        s_trying_new = false;
        memset(s_new_pass, 0, sizeof(s_new_pass));
    }
    weight_state_set_wifi(false);
    set_state(s_has_creds ? WEIGHT_WIFI_DISCONNECTED : WEIGHT_WIFI_NO_CREDS);
}

static void do_reconnect(void)
{
    const weight_config_t *cfg = weight_config_get();
    s_user_disconnected = false;
    s_backoff_ms = RECONNECT_MIN_MS;
    if (!cfg->wifi_ssid[0] || apply_sta_config(cfg->wifi_ssid, cfg->wifi_pass) != ESP_OK)
    {
        s_has_creds = false;
        set_state(WEIGHT_WIFI_NO_CREDS);
        return;
    }
    s_has_creds = true;
    start_connect();
}

static void collect_scan_results(void)
{
    uint16_t n = SCAN_FETCH_MAX;
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    if (!recs)
    {
        ESP_LOGE(TAG, "scan: out of memory");
        return;
    }
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK)
    {
        free(recs);
        return;
    }

    for (uint16_t i = 0; i < n; i++)
    {
        const char *ssid = (const char *)recs[i].ssid;
        if (ssid[0] == '\0')
            continue; /* hidden network */

        size_t j;
        for (j = 0; j < s_scan_count; j++)
            if (strcmp(s_scan_buf[j].ssid, ssid) == 0)
                break;

        if (j < s_scan_count)
        {
            /* same SSID from another AP - keep the strongest */
            if (recs[i].rssi > s_scan_buf[j].rssi)
                s_scan_buf[j].rssi = recs[i].rssi;
            continue;
        }
        if (s_scan_count >= WIFI_MAX_SCAN_RESULTS)
            continue;

        wifi_scan_entry_t *e = &s_scan_buf[s_scan_count++];
        strlcpy(e->ssid, ssid, sizeof(e->ssid));
        e->rssi = recs[i].rssi;
        e->secure = recs[i].authmode != WIFI_AUTH_OPEN;
    }
    free(recs);

    /* strongest first */
    for (size_t i = 1; i < s_scan_count; i++)
    {
        wifi_scan_entry_t tmp = s_scan_buf[i];
        size_t j = i;
        while (j > 0 && s_scan_buf[j - 1].rssi < tmp.rssi)
        {
            s_scan_buf[j] = s_scan_buf[j - 1];
            j--;
        }
        s_scan_buf[j] = tmp;
    }
}

static void do_scan(void)
{
    s_scan_count = 0;
    bool resume = false;

    if (get_state() == WEIGHT_WIFI_CONNECTING)
    {
        /* The radio can't scan during an association attempt - pause it and
         * resume right after. A CONNECTED link is left alone: the driver
         * scans off-channel and returns to the AP between channels. */
        esp_timer_stop(s_retry_timer);
        quiet_disconnect();
        resume = true;
    }

    wifi_scan_config_t sc = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {.min = 100, .max = 300},
    };

    esp_err_t err = esp_wifi_scan_start(&sc, true); /* blocks this task only */
    if (err == ESP_OK)
        collect_scan_results();
    else
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));

    ESP_LOGI(TAG, "scan: %u networks", (unsigned)s_scan_count);

    if (resume && s_has_creds && !s_user_disconnected)
        start_connect();

    xSemaphoreGive(s_scan_done);
}

static void do_poll(void)
{
    if (get_state() != WEIGHT_WIFI_CONNECTED)
        return;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
    {
        taskENTER_CRITICAL(&s_info_mux);
        s_info.rssi = ap.rssi;
        taskEXIT_CRITICAL(&s_info_mux);
    }
}

static void worker_task(void *arg)
{
    wifi_msg_t m;
    for (;;)
    {
        if (xQueueReceive(s_q, &m, portMAX_DELAY) != pdTRUE)
            continue;

        switch (m.type)
        {
        case EV_STA_START:
            ESP_LOGI(TAG, "STA started");
            if (s_has_creds && !s_user_disconnected)
                start_connect();
            break;

        case EV_STA_CONNECTED:
            ESP_LOGI(TAG, "associated, waiting for IP...");
            break;

        case EV_GOT_IP:
            on_got_ip(m.u.ip);
            break;

        case EV_LOST_IP:
            ESP_LOGW(TAG, "lost IP");
            weight_state_set_wifi(false);
            if (get_state() == WEIGHT_WIFI_CONNECTED)
                esp_wifi_disconnect(); /* -> DISCONNECTED -> normal retry path */
            break;

        case EV_DISCONNECTED:
            on_disconnected(m.u.reason);
            break;

        case CMD_RETRY:
            if (s_has_creds && !s_user_disconnected &&
                get_state() != WEIGHT_WIFI_CONNECTED)
                start_connect();
            break;

        case CMD_POLL:
            do_poll();
            break;

        case CMD_CONNECT:
            do_connect_new(m.u.creds.ssid, m.u.creds.pass);
            memset(&m, 0, sizeof(m)); /* don't leave the password on the stack */
            break;

        case CMD_DISCONNECT:
            do_user_disconnect();
            break;

        case CMD_RECONNECT:
            do_reconnect();
            break;

        case CMD_SCAN:
            do_scan();
            break;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Event handler / timers - post only, never block, never call esp_wifi_*
 * --------------------------------------------------------------------------- */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    wifi_msg_t m = {0};

    if (base == WIFI_EVENT)
    {
        switch (id)
        {
        case WIFI_EVENT_STA_START:
            m.type = EV_STA_START;
            break;
        case WIFI_EVENT_STA_CONNECTED:
            /* Must run immediately on every connect - the default netif glue
             * (re)starts DHCP on this event and would clobber static IP. */
            apply_static_ip_if_configured();
            m.type = EV_STA_CONNECTED;
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            m.type = EV_DISCONNECTED;
            m.u.reason = data ? ((wifi_event_sta_disconnected_t *)data)->reason : 0;
            break;
        default:
            return;
        }
    }
    else if (base == IP_EVENT)
    {
        if (id == IP_EVENT_STA_GOT_IP)
        {
            m.type = EV_GOT_IP;
            m.u.ip = ((ip_event_got_ip_t *)data)->ip_info.ip.addr;
        }
        else if (id == IP_EVENT_STA_LOST_IP)
        {
            m.type = EV_LOST_IP;
        }
        else
        {
            return;
        }
    }
    else
    {
        return;
    }

    post(&m);
}

static void retry_timer_cb(void *arg)
{
    wifi_msg_t m = {.type = CMD_RETRY};
    post(&m);
}

static void poll_timer_cb(void *arg)
{
    wifi_msg_t m = {.type = CMD_POLL};
    post(&m);
}

/* ---------------------------------------------------------------------------
 * Init
 * --------------------------------------------------------------------------- */
esp_err_t weight_wifi_init(void)
{
    s_q = xQueueCreate(CMD_QUEUE_LEN, sizeof(wifi_msg_t));
    s_scan_mutex = xSemaphoreCreateMutex();
    s_scan_done = xSemaphoreCreateBinary();
    if (!s_q || !s_scan_mutex || !s_scan_done)
        return ESP_ERR_NO_MEM;

    esp_timer_create_args_t rargs = {.callback = retry_timer_cb, .name = "wifi_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&rargs, &s_retry_timer));
    esp_timer_create_args_t pargs = {.callback = poll_timer_cb, .name = "wifi_poll"};
    ESP_ERROR_CHECK(esp_timer_create(&pargs, &s_poll_timer));

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t ev = esp_event_loop_create_default();
    if (ev != ESP_OK && ev != ESP_ERR_INVALID_STATE)
        ESP_ERROR_CHECK(ev);

    s_netif_sta = esp_netif_create_default_wifi_sta();

    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&icfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_LOST_IP, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    const weight_config_t *cfg = weight_config_get();

    /* Restore static IP from config (was RAM-only before - lost on reboot) */
    if (cfg->wifi_use_static && cfg->wifi_ip[0])
    {
        strlcpy(s_static_cfg.ip, cfg->wifi_ip, sizeof(s_static_cfg.ip));
        strlcpy(s_static_cfg.gw, cfg->wifi_gw, sizeof(s_static_cfg.gw));
        strlcpy(s_static_cfg.netmask, cfg->wifi_netmask, sizeof(s_static_cfg.netmask));
        strlcpy(s_static_cfg.dns, cfg->wifi_dns, sizeof(s_static_cfg.dns));
        s_static_cfg.enabled = true;
        ESP_LOGI(TAG, "static ip restored: %s", s_static_cfg.ip);
    }
    s_info.static_ip = s_static_cfg.enabled;

    if (cfg->wifi_ssid[0] && apply_sta_config(cfg->wifi_ssid, cfg->wifi_pass) == ESP_OK)
    {
        s_has_creds = true;
        ESP_LOGI(TAG, "auto-connect to '%s'", cfg->wifi_ssid);
    }
    else
    {
        ESP_LOGW(TAG, "no saved credentials - awaiting on-HMI setup");
    }
    set_state(s_has_creds ? WEIGHT_WIFI_CONNECTING : WEIGHT_WIFI_NO_CREDS);

    /* Worker must exist before esp_wifi_start() so STA_START is handled */
    if (xTaskCreate(worker_task, "wifi_mgr", WORKER_STACK, NULL, WORKER_PRIO, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_wifi_start());
    esp_timer_start_periodic(s_poll_timer, (uint64_t)RSSI_POLL_MS * 1000ULL);

    ESP_LOGI(TAG, "wifi station init complete");
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */
size_t weight_wifi_scan(wifi_scan_entry_t *out, size_t max)
{
    if (!out || max == 0 || !s_q)
        return 0;
    if (xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(SCAN_WAIT_MS)) != pdTRUE)
        return 0;

    xSemaphoreTake(s_scan_done, 0); /* drop a stale completion from a timed-out scan */

    size_t n = 0;
    wifi_msg_t m = {.type = CMD_SCAN};
    if (post_from_api(&m) == ESP_OK &&
        xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(SCAN_WAIT_MS)) == pdTRUE)
    {
        n = s_scan_count < max ? s_scan_count : max;
        memcpy(out, s_scan_buf, n * sizeof(*out));
    }
    else
    {
        ESP_LOGW(TAG, "scan timed out");
    }

    xSemaphoreGive(s_scan_mutex);
    return n;
}

esp_err_t weight_wifi_connect(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0')
        return ESP_ERR_INVALID_ARG;

    wifi_msg_t m = {.type = CMD_CONNECT};
    strlcpy(m.u.creds.ssid, ssid, sizeof(m.u.creds.ssid));
    strlcpy(m.u.creds.pass, pass ? pass : "", sizeof(m.u.creds.pass));
    esp_err_t err = post_from_api(&m);
    memset(&m, 0, sizeof(m));
    return err;
}

esp_err_t weight_wifi_disconnect(void)
{
    wifi_msg_t m = {.type = CMD_DISCONNECT};
    return post_from_api(&m);
}

esp_err_t weight_wifi_reconnect(void)
{
    wifi_msg_t m = {.type = CMD_RECONNECT};
    return post_from_api(&m);
}

bool weight_wifi_is_connected(void)
{
    return get_state() == WEIGHT_WIFI_CONNECTED;
}

void weight_wifi_get_info(weight_wifi_info_t *out)
{
    if (!out)
        return;
    taskENTER_CRITICAL(&s_info_mux);
    *out = s_info;
    taskEXIT_CRITICAL(&s_info_mux);
}

esp_err_t weight_wifi_set_static_ip(const char *ip, const char *gateway,
                                    const char *netmask, const char *dns)
{
    if (!ip || ip[0] == '\0')
    {
        memset(&s_static_cfg, 0, sizeof(s_static_cfg));
        if (s_netif_sta)
        {
            esp_err_t err = esp_netif_dhcpc_start(s_netif_sta);
            if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
                ESP_LOGW(TAG, "dhcpc_start failed: %s", esp_err_to_name(err));
        }
        weight_config_set_static_ip(false, "", "", "", "");
        weight_config_save();
        taskENTER_CRITICAL(&s_info_mux);
        s_info.static_ip = false;
        taskEXIT_CRITICAL(&s_info_mux);
        ESP_LOGI(TAG, "static ip cleared, reverted to DHCP (saved)");
        return ESP_OK;
    }

    if (!gateway || !netmask)
        return ESP_ERR_INVALID_ARG;

    strlcpy(s_static_cfg.ip, ip, sizeof(s_static_cfg.ip));
    strlcpy(s_static_cfg.gw, gateway, sizeof(s_static_cfg.gw));
    strlcpy(s_static_cfg.netmask, netmask, sizeof(s_static_cfg.netmask));
    strlcpy(s_static_cfg.dns, dns ? dns : "", sizeof(s_static_cfg.dns));
    s_static_cfg.enabled = true;

    weight_config_set_static_ip(true, ip, gateway, netmask, dns ? dns : "");
    weight_config_save();
    taskENTER_CRITICAL(&s_info_mux);
    s_info.static_ip = true;
    taskEXIT_CRITICAL(&s_info_mux);
    ESP_LOGI(TAG, "static ip saved: %s gw %s mask %s", ip, gateway, netmask);

    if (weight_wifi_is_connected())
        apply_static_ip_if_configured();

    return ESP_OK;
}

bool weight_wifi_is_static_ip(void)
{
    return s_static_cfg.enabled;
}
/**
 * @file weight_wifi.c
 * @brief WiFi station for ESP32-P4 + ESP32-C6 (Waveshare 10.1" board).
 *
 * On this board the radio is on a separate ESP32-C6 co-processor. The
 * esp_wifi_remote managed component routes esp_wifi_* calls over SDIO to
 * the C6, where esp-hosted slave firmware actually drives the radio.
 *
 * Critical: the host's esp_hosted version must match the slave firmware
 * Waveshare flashed onto the C6. This project pins esp_hosted to "1.4.*"
 * and esp_wifi_remote to "0.14.*" - the same versions as Waveshare's
 * confirmed-working 04_wifistation example. Newer versions fail with
 * "OS adapter function version error".
 *
 * Reconnect strategy:
 *  - On disconnect we DO NOT block in the event handler (that's a hard rule
 *    - blocking in esp_event handlers freezes the WiFi stack and trips the
 *    interrupt watchdog). Instead we post to a esp_timer-driven retry that
 *    runs on its own thread.
 */

#include <string.h>
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
#include "freertos/event_groups.h"

static const char *TAG = "wifi";

#define WIFI_RECONNECT_DELAY_MS 3000

#define BIT_CONNECTED BIT0
#define BIT_HAS_CREDS BIT1

static EventGroupHandle_t s_evt;
static esp_netif_t *s_netif_sta;
static esp_timer_handle_t s_reconnect_timer;

/* ---------------------------------------------------------------------------
 * Reconnect (called from esp_timer thread, NOT from event handler)
 * --------------------------------------------------------------------------- */
static void reconnect_timer_cb(void *arg)
{
    if (xEventGroupGetBits(s_evt) & BIT_HAS_CREDS)
    {
        ESP_LOGI(TAG, "reconnect attempt");
        esp_wifi_connect();
    }
}

static void schedule_reconnect(void)
{
    /* esp_timer_start_once is safe to call from any context, never blocks */
    esp_timer_stop(s_reconnect_timer);
    esp_timer_start_once(s_reconnect_timer,
                         (uint64_t)WIFI_RECONNECT_DELAY_MS * 1000ULL);
}

/* ---------------------------------------------------------------------------
 * Event handler - keep it tight, NEVER block
 * --------------------------------------------------------------------------- */
static void mqtt_restart_task(void *arg)
{
    weight_mqtt_restart();
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT)
    {
        switch (id)
        {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA started");
            if (xEventGroupGetBits(s_evt) & BIT_HAS_CREDS)
            {
                esp_wifi_connect();
            }
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
        {
            wifi_event_sta_disconnected_t *e =
                (wifi_event_sta_disconnected_t *)data;
            ESP_LOGW(TAG, "disconnected (reason=%d), retry in %dms",
                     e ? e->reason : -1, WIFI_RECONNECT_DELAY_MS);
            weight_state_set_wifi(false);
            xEventGroupClearBits(s_evt, BIT_CONNECTED);
            /* schedule retry via timer - DO NOT delay in this handler */
            schedule_reconnect();
            break;
        }

        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "associated, waiting for IP...");
            break;
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&e->ip_info.ip));
        weight_state_set_wifi(true);
        xEventGroupSetBits(s_evt, BIT_CONNECTED);

        /* Offload MQTT restart to a worker task — do NOT call directly
         * from the event handler. esp_mqtt_client_* functions are heavy
         * and will overflow the sys_evt task stack. */
        xTaskCreate(mqtt_restart_task, "mqtt_start", 4096, NULL, 5, NULL);
    }
}

/* ---------------------------------------------------------------------------
 * Init
 * --------------------------------------------------------------------------- */
esp_err_t weight_wifi_init(void)
{
    s_evt = xEventGroupCreate();
    if (!s_evt)
        return ESP_ERR_NO_MEM;

    esp_timer_create_args_t targs = {
        .callback = reconnect_timer_cb,
        .name = "wifi_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_reconnect_timer));

    ESP_ERROR_CHECK(esp_netif_init());

    esp_err_t ev = esp_event_loop_create_default();
    if (ev != ESP_OK && ev != ESP_ERR_INVALID_STATE)
        ESP_ERROR_CHECK(ev);

    s_netif_sta = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    const weight_config_t *cfg_ptr = weight_config_get();
    if (cfg_ptr->wifi_ssid[0] != '\0')
    {
        wifi_config_t wc = {0};
        snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", cfg_ptr->wifi_ssid);
        snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", cfg_ptr->wifi_pass);
        wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
        xEventGroupSetBits(s_evt, BIT_HAS_CREDS);
        ESP_LOGI(TAG, "auto-connect to '%s'", cfg_ptr->wifi_ssid);
    }
    else
    {
        ESP_LOGW(TAG, "no saved credentials - awaiting on-HMI setup");
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    // /* ---- diagnostic block: poll MAC for up to 3 seconds ---- */
    // uint8_t mac[6];
    // for (int i = 0; i < 30; i++)
    // {
    //     esp_err_t r = esp_wifi_get_mac(WIFI_IF_STA, mac);
    //     ESP_LOGI(TAG, "mac poll %d: err=0x%x  %02x:%02x:%02x:%02x:%02x:%02x",
    //              i, r, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    //     if (r == ESP_OK && mac[0] != 0)
    //         break;
    //     vTaskDelay(pdMS_TO_TICKS(100));
    // }
    // /* ---- end diagnostic ---- */

    ESP_LOGI(TAG, "wifi station init complete");
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * On-HMI scan
 * --------------------------------------------------------------------------- */
size_t weight_wifi_scan(wifi_scan_entry_t *out, size_t max)
{
    if (!out || max == 0)
        return 0;

    /* Stop any in-progress connect — scan + connect don't coexist on
       esp_wifi_remote. Harmless if not connected. */
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Make sure any previous scan results are cleared */
    esp_wifi_scan_stop();

    wifi_scan_config_t cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0, /* all channels */
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = {.min = 100, .max = 300}, /* ms per channel */
        },
    };

    ESP_LOGI(TAG, "starting scan...");
    esp_err_t err = esp_wifi_scan_start(&cfg, true); /* blocking */
    ESP_LOGI(TAG, "scan_start returned: %s", esp_err_to_name(err));
    if (err != ESP_OK)
        return 0;

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    ESP_LOGI(TAG, "ap_count = %d", ap_count);
    if (ap_count == 0)
        return 0;
    if (ap_count > max)
        ap_count = max;

    wifi_ap_record_t *recs = calloc(ap_count, sizeof(*recs));
    if (!recs)
        return 0;
    esp_wifi_scan_get_ap_records(&ap_count, recs);

    for (size_t i = 0; i < ap_count; i++)
    {
        snprintf(out[i].ssid, sizeof(out[i].ssid), "%s", (char *)recs[i].ssid);
        out[i].rssi = recs[i].rssi;
        out[i].secure = recs[i].authmode != WIFI_AUTH_OPEN;
    }
    free(recs);
    return ap_count;
}

/* ---------------------------------------------------------------------------
 * On-HMI connect - called when user picks a network + types password
 * --------------------------------------------------------------------------- */
esp_err_t weight_wifi_connect(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0')
        return ESP_ERR_INVALID_ARG;

    weight_config_set_wifi(ssid, pass ? pass : "");
    weight_config_save();

    wifi_config_t wc = {0};
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", ssid);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", pass ? pass : "");
    wc.sta.threshold.authmode = (pass && pass[0]) ? WIFI_AUTH_WPA2_PSK
                                                  : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    xEventGroupSetBits(s_evt, BIT_HAS_CREDS);

    esp_wifi_disconnect();
    return esp_wifi_connect();
}

bool weight_wifi_is_connected(void)
{
    return s_evt && (xEventGroupGetBits(s_evt) & BIT_CONNECTED) != 0;
}
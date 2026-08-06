/**
 * @file weight_wifi.c
 * @brief WiFi station for ESP32-P4 + ESP32-C6 (Waveshare 10.1" board).
 *
 * On this board the radio is on a separate ESP32-C6 co-processor. The
 * esp_wifi_remote managed component routes esp_wifi_* calls over SDIO to
 * the C6, where esp-hosted slave firmware actually drives the radio.
 * The TCP/IP stack (lwIP) and esp_netif still run on the P4 host, so
 * static IP / DHCP handling below is unaffected by the remote radio.
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
 *
 * Static IP:
 *  - ESP-IDF's default netif glue (from esp_netif_create_default_wifi_sta())
 *    restarts the DHCP client on every WIFI_EVENT_STA_CONNECTED, including
 *    reconnects triggered by reconnect_timer_cb(). So static IP is (re)applied
 *    inside the WIFI_EVENT_STA_CONNECTED handler on every connect, not just
 *    once at init - otherwise the first reconnect after a drop silently
 *    reverts to DHCP.
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
#include "lwip/inet.h"

static const char *TAG = "wifi";

#define WIFI_RECONNECT_DELAY_MS 3000

#define BIT_CONNECTED BIT0
#define BIT_HAS_CREDS BIT1

static EventGroupHandle_t s_evt;
static esp_netif_t *s_netif_sta;
static esp_timer_handle_t s_reconnect_timer;
static volatile bool s_scanning = false; /* true while weight_wifi_scan()'s
                                          * own esp_wifi_disconnect() is in
                                          * flight - suppresses auto-reconnect
                                          * for that disconnect only */

/* Static IP config, held in RAM. NOTE: this is not yet persisted to NVS -
 * see weight_wifi_set_static_ip() below for what to wire into
 * weight_config.c if you want it to survive reboot. */
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
 * Static IP application - called from WIFI_EVENT_STA_CONNECTED, see note
 * in file header on why it must run on every connect, not just once.
 * --------------------------------------------------------------------------- */
static void apply_static_ip_if_configured(void)
{
    if (!s_static_cfg.enabled)
        return; /* leave DHCP client running as normal */

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

    ESP_LOGI(TAG, "static ip applied: %s gw %s mask %s%s%s",
             s_static_cfg.ip, s_static_cfg.gw, s_static_cfg.netmask,
             s_static_cfg.dns[0] ? " dns " : "", s_static_cfg.dns);
}

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
    volatile bool *pending = (volatile bool *)arg;
    weight_mqtt_restart();
    if (pending)
        *pending = false;
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
            weight_state_set_wifi(false);
            xEventGroupClearBits(s_evt, BIT_CONNECTED);

            if (s_scanning)
            {
                ESP_LOGI(TAG, "disconnected (reason=%d) - scan in progress, "
                              "no auto-reconnect scheduled",
                         e ? e->reason : -1);
            }
            else
            {
                ESP_LOGW(TAG, "disconnected (reason=%d), retry in %dms",
                         e ? e->reason : -1, WIFI_RECONNECT_DELAY_MS);
                /* schedule retry via timer - DO NOT delay in this handler */
                schedule_reconnect();
            }
            break;
        }
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "associated, waiting for IP...");
            /* Must run here, on every connect - see file header note.
             * esp_netif's default glue will otherwise (re)start DHCP
             * on this same event and clobber a static config. */
            apply_static_ip_if_configured();
            break;
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&e->ip_info.ip));
        weight_state_set_wifi(true);
        xEventGroupSetBits(s_evt, BIT_CONNECTED);

        /* Debounce: if a restart task is already pending/running, don't
         * pile up another one. Rapid reconnects (e.g. while testing static
         * IP) can otherwise queue several restarts back to back. */
        static volatile bool s_mqtt_restart_pending = false;
        if (!s_mqtt_restart_pending)
        {
            s_mqtt_restart_pending = true;
            xTaskCreate(mqtt_restart_task, "mqtt_start", 4096, &s_mqtt_restart_pending, 5, NULL);
        }
        else
        {
            ESP_LOGI(TAG, "mqtt restart already pending, skipping duplicate trigger");
        }
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

    size_t result = 0;

    s_scanning = true;

    /* Stop any in-progress connect — scan + connect don't coexist on
       esp_wifi_remote. Harmless if not connected. This disconnect is
       intentional; s_scanning tells the event handler to skip the
       auto-reconnect it would normally schedule. */
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
        goto done;

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    ESP_LOGI(TAG, "ap_count = %d", ap_count);
    if (ap_count == 0)
        goto done;
    if (ap_count > max)
        ap_count = max;

    wifi_ap_record_t *recs = calloc(ap_count, sizeof(*recs));
    if (!recs)
        goto done;
    esp_wifi_scan_get_ap_records(&ap_count, recs);

    for (size_t i = 0; i < ap_count; i++)
    {
        snprintf(out[i].ssid, sizeof(out[i].ssid), "%s", (char *)recs[i].ssid);
        out[i].rssi = recs[i].rssi;
        out[i].secure = recs[i].authmode != WIFI_AUTH_OPEN;
    }
    free(recs);
    result = ap_count;

done:
    s_scanning = false;
    /* If nothing else reconnects us (no auto-reconnect was scheduled for
     * the scan disconnect above), and the user backs out of the WiFi
     * screen without picking a new network, we'll sit disconnected until
     * weight_wifi_connect() is called or a real link-drop reconnect kicks
     * in later. If screen_wifi.c should restore the previous connection
     * on "back" with nothing selected, call esp_wifi_connect() there. */
    return result;
}

/* ---------------------------------------------------------------------------
 * On-HMI connect - called when user picks a network + types password
 * --------------------------------------------------------------------------- */
esp_err_t weight_wifi_connect(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0')
        return ESP_ERR_INVALID_ARG;

    esp_timer_stop(s_reconnect_timer); /* cancel any pending auto-reconnect */

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

/* ---------------------------------------------------------------------------
 * Static IP
 * --------------------------------------------------------------------------- */
esp_err_t weight_wifi_set_static_ip(const char *ip, const char *gateway,
                                    const char *netmask, const char *dns)
{
    if (!ip || ip[0] == '\0')
    {
        /* revert to DHCP */
        memset(&s_static_cfg, 0, sizeof(s_static_cfg));
        if (s_netif_sta)
        {
            esp_err_t err = esp_netif_dhcpc_start(s_netif_sta);
            if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
                ESP_LOGW(TAG, "dhcpc_start failed: %s", esp_err_to_name(err));
        }
        weight_config_set_static_ip(false, "", "", "", "");
        weight_config_save();
        ESP_LOGI(TAG, "static ip cleared, reverted to DHCP (saved)");
        return ESP_OK;
    }

    if (!gateway || !netmask)
        return ESP_ERR_INVALID_ARG;

    snprintf(s_static_cfg.ip, sizeof(s_static_cfg.ip), "%s", ip);
    snprintf(s_static_cfg.gw, sizeof(s_static_cfg.gw), "%s", gateway);
    snprintf(s_static_cfg.netmask, sizeof(s_static_cfg.netmask), "%s", netmask);
    snprintf(s_static_cfg.dns, sizeof(s_static_cfg.dns), "%s", dns ? dns : "");
    s_static_cfg.enabled = true;

    weight_config_set_static_ip(true, ip, gateway, netmask, dns ? dns : "");
    weight_config_save();
    ESP_LOGI(TAG, "static ip saved: %s gw %s mask %s%s%s (applies on next connect)",
             ip, gateway, netmask, dns && dns[0] ? " dns " : "", dns ? dns : "");

    /* If already connected, apply immediately rather than waiting for
     * the next reconnect cycle. */
    if (weight_wifi_is_connected())
        apply_static_ip_if_configured();

    return ESP_OK;
}

bool weight_wifi_is_static_ip(void)
{
    return s_static_cfg.enabled;
}
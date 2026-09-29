#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define WIFI_MAX_SCAN_RESULTS 20

    typedef struct
    {
        char ssid[33];
        int8_t rssi;
        bool secure;
    } wifi_scan_entry_t;

    typedef enum
    {
        WEIGHT_WIFI_NO_CREDS = 0, /* nothing saved, waiting for on-HMI setup */
        WEIGHT_WIFI_DISCONNECTED, /* user pressed Disconnect - no auto-reconnect */
        WEIGHT_WIFI_CONNECTING,   /* attempting / waiting to retry */
        WEIGHT_WIFI_CONNECTED,    /* associated and has an IP */
    } weight_wifi_state_t;

    typedef enum
    {
        WEIGHT_WIFI_FAIL_NONE = 0,
        WEIGHT_WIFI_FAIL_AUTH,      /* wrong password / handshake failed */
        WEIGHT_WIFI_FAIL_NOT_FOUND, /* SSID not in range */
        WEIGHT_WIFI_FAIL_OTHER,
    } weight_wifi_fail_t;

    typedef struct
    {
        weight_wifi_state_t state;
        char ssid[33]; /* network in use / being tried */
        char ip[16];   /* valid when CONNECTED */
        int8_t rssi;   /* valid when CONNECTED, refreshed every 10 s */
        bool static_ip;
        uint16_t last_reason; /* last esp-wifi disconnect reason */

        /* Set when a newly entered network was rejected and we reverted to
         * the previously saved one. Cleared on the next successful connect. */
        bool new_creds_failed;
        weight_wifi_fail_t fail;
        char failed_ssid[33];
    } weight_wifi_info_t;

    esp_err_t weight_wifi_init(void);

    /* Blocking scan (up to ~15 s). Does NOT drop an active connection.
     * Call from a worker task, never from the LVGL task. */
    size_t weight_wifi_scan(wifi_scan_entry_t *out, size_t max);

    /* Try a new network. Non-blocking. Credentials are only saved to NVS
     * after the connection gets an IP; on repeated failure the previously
     * saved network is restored and info.new_creds_failed is set. */
    esp_err_t weight_wifi_connect(const char *ssid, const char *pass);

    /* User-initiated disconnect. Auto-reconnect stays off until
     * weight_wifi_reconnect(), weight_wifi_connect() or a reboot. */
    esp_err_t weight_wifi_disconnect(void);

    /* Reconnect to the saved network after a user disconnect. */
    esp_err_t weight_wifi_reconnect(void);

    bool weight_wifi_is_connected(void);

    /* Snapshot of current state for the UI. Cheap, never blocks on the radio. */
    void weight_wifi_get_info(weight_wifi_info_t *out);

    /* --- Static IP ---
     * Takes effect immediately if connected, otherwise on next connect.
     * Pass ip == NULL or "" to revert to DHCP. dns may be NULL/"".
     */
    esp_err_t weight_wifi_set_static_ip(const char *ip, const char *gateway,
                                        const char *netmask, const char *dns);

    bool weight_wifi_is_static_ip(void);

#ifdef __cplusplus
}
#endif
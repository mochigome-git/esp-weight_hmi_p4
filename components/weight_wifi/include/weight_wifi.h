#pragma once

#include <stdint.h>
#include <stdbool.h>
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

    esp_err_t weight_wifi_init(void);

    /* Blocking scan - up to 5 seconds. Returns number filled in out. */
    size_t weight_wifi_scan(wifi_scan_entry_t *out, size_t max);

    /* Apply new credentials, save to config, attempt connect. */
    esp_err_t weight_wifi_connect(const char *ssid, const char *pass);

    bool weight_wifi_is_connected(void);

    /* --- Static IP ---
     * Call before or after weight_wifi_connect(); takes effect on the next
     * (re)connect. Pass ip == NULL or "" to revert to DHCP.
     * dns may be NULL/"" to skip setting a DNS server.
     */
    esp_err_t weight_wifi_set_static_ip(const char *ip, const char *gateway,
                                        const char *netmask, const char *dns);

    bool weight_wifi_is_static_ip(void);

#ifdef __cplusplus
}
#endif
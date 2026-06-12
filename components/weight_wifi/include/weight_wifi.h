/**
 * @file weight_wifi.h
 * @brief WiFi station - scan and connect.
 *
 * Operator picks SSID from on-HMI list, types password via lv_keyboard,
 * we save to NVS and connect. On boot, if creds exist, auto-connect.
 *
 * Internally this uses esp_wifi_remote because on the Waveshare ESP32-P4
 * boards the WiFi co-processor is an ESP32-C6 connected via SDIO. The
 * managed component esp_wifi_remote redirects the standard esp_wifi_ APIs
 * to the C6, so user code looks the same as on a regular ESP32.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_MAX_SCAN_RESULTS 20

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} wifi_scan_entry_t;

esp_err_t weight_wifi_init(void);

/* Blocking scan - up to 5 seconds. Returns number filled in out. */
size_t weight_wifi_scan(wifi_scan_entry_t *out, size_t max);

/* Apply new credentials, save to config, attempt connect. */
esp_err_t weight_wifi_connect(const char *ssid, const char *pass);

bool      weight_wifi_is_connected(void);

#ifdef __cplusplus
}
#endif

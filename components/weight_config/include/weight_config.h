/**
 * @file weight_config.h
 * @brief Persistent device configuration (decimal precision, unit, MQTT, audio...)
 *
 * Stored in NVS namespace "weight_cfg".
 * All getters are O(1) reads from cached struct - call weight_config_save() to persist edits.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define WCFG_MAX_HOST_LEN 64
#define WCFG_MAX_USER_LEN 64
#define WCFG_MAX_PASS_LEN 128
#define WCFG_MAX_CA_CERT_LEN 4096 /* fits typical Let's Encrypt + chain */
#define WCFG_MAX_WIFI_SSID_LEN 33
#define WCFG_MAX_WIFI_PASS_LEN 65
#define WCFG_MAX_IP_STR_LEN 16 /* fits "255.255.255.255" + NUL */
#define WCFG_MAX_DEVICE_NAME_LEN 32
#define WCFG_MAX_DEVICE_ID_LEN 40
#define WCFG_MAX_MACHINE_ID_LEN 40

    typedef enum
    {
        WCFG_UNIT_G = 0,
        WCFG_UNIT_KG,
        WCFG_UNIT_LB,
        WCFG_UNIT_OZ,
    } wcfg_unit_t;

    typedef struct
    {

        /* Device */
        char device_id[WCFG_MAX_DEVICE_ID_LEN];     /* was char device_id[32] */
        char device_name[WCFG_MAX_DEVICE_NAME_LEN]; /* short, human-readable
        label for display only */
        char machine_id[WCFG_MAX_MACHINE_ID_LEN];
        char tenant_id[64];
        char tenant_short[16];

        /* Display */
        uint8_t lang;           /* 0 = EN, 1 = JA */
        uint8_t decimal_places; /* 0..3 */
        wcfg_unit_t unit;

        /* Scale / RS232 */
        uint32_t uart_baud;   /* 4800, 9600, 19200, 38400 */
        float zero_threshold; /* readings below this are ignored, in current unit */

        /* Stability detection */
        uint8_t stability_window;  /* 3..10 samples */
        float stability_tolerance; /* in current unit (e.g. 0.5g) */

        /* Audio */
        uint8_t audio_volume; /* 0..100 */
        bool audio_muted;

        /* WiFi */
        char wifi_ssid[WCFG_MAX_WIFI_SSID_LEN];
        char wifi_pass[WCFG_MAX_WIFI_PASS_LEN];

        /* WiFi static IP - if wifi_use_static is false, DHCP is used and
         * the ip/gw/netmask/dns fields below are ignored. */
        bool wifi_use_static;
        char wifi_ip[WCFG_MAX_IP_STR_LEN];
        char wifi_gw[WCFG_MAX_IP_STR_LEN];
        char wifi_netmask[WCFG_MAX_IP_STR_LEN];
        char wifi_dns[WCFG_MAX_IP_STR_LEN];

        /* MQTT (EMQX with TLS + basic auth) */
        char mqtt_host[WCFG_MAX_HOST_LEN];
        uint16_t mqtt_port; /* typically 8883 */
        char mqtt_user[WCFG_MAX_USER_LEN];
        char mqtt_pass[WCFG_MAX_PASS_LEN];
        bool mqtt_tls;
        /* CA cert PEM stored separately - see weight_config_get_ca_cert / set */
    } weight_config_t;

    esp_err_t weight_config_init(void);

    /* Thread safety: setters and weight_config_save() take an internal
     * recursive mutex. Tasks other than the UI that read STRING fields
     * (device_id, mqtt_host, ...) should wrap the read in
     * weight_config_lock()/unlock() or take a full copy. */
    void weight_config_lock(void);
    void weight_config_unlock(void);
    void weight_config_copy(weight_config_t *out);

    /* Returns the cached in-memory config. The pointer is stable for app lifetime
     * (singleton). Treat it as read-only; modify via setters + save. */
    const weight_config_t *weight_config_get(void);

    /* Mutators - just modify the cached struct; commit with _save. */
    int weight_config_get_lang(void);
    void weight_config_set_lang(int lang);
    void weight_config_set_device_id(const char *id);
    void weight_config_set_device_name(const char *name);
    void weight_config_set_machine_id(const char *id);
    void weight_config_set_tenant(const char *tenant_id, const char *tenant_short);
    void weight_config_set_decimal(uint8_t d);
    void weight_config_set_unit(wcfg_unit_t u);
    void weight_config_set_uart_baud(uint32_t baud);
    void weight_config_set_zero_threshold(float t);
    void weight_config_set_stability(uint8_t window, float tolerance);
    void weight_config_set_audio(uint8_t volume, bool muted);
    void weight_config_set_wifi(const char *ssid, const char *pass);
    /* Static IP. Pass use_static=false to revert to DHCP (ip/gw/netmask/dns
     * may be NULL/"" in that case). gw/netmask are required whenever
     * use_static is true; dns may be NULL/"" to leave DNS unset. */
    void weight_config_set_static_ip(bool use_static, const char *ip,
                                     const char *gw, const char *netmask,
                                     const char *dns);
    void weight_config_set_mqtt(const char *host, uint16_t port,
                                const char *user, const char *pass, bool tls);

    /* CA cert is handled separately because it's large (up to 4KB) and rarely changed. */
    esp_err_t weight_config_get_ca_cert(char *out, size_t out_len, size_t *actual_len);
    esp_err_t weight_config_set_ca_cert(const char *pem);

    /* Persist all current settings to NVS. */
    esp_err_t weight_config_save(void);

    /* Unit conversion helper - returns unit string like "g", "kg" */
    const char *weight_config_unit_str(wcfg_unit_t u);

#ifdef __cplusplus
}
#endif

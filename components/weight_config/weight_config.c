/**
 * @file weight_config.c
 */

#include <string.h>
#include "weight_config.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "config";
static const char *NS = "weight_cfg";

static weight_config_t s_cfg;

static void apply_defaults(weight_config_t *c)
{
    memset(c, 0, sizeof(*c));
    strlcpy(c->tenant_id, "f00af313-35eb-4af0-9ce6-555eb48a6537", sizeof(c->tenant_id));
    strlcpy(c->tenant_short, "gcl", sizeof(c->tenant_short));
#ifdef DEFAULT_DEVICE_ID
    strlcpy(c->device_id, DEFAULT_DEVICE_ID, sizeof(c->device_id));
#endif
#ifdef DEFAULT_MACHINE_ID
    strlcpy(c->machine_id, DEFAULT_MACHINE_ID, sizeof(c->machine_id));
#endif

    c->decimal_places = 1;
    c->unit = WCFG_UNIT_G;
    c->uart_baud = 9600;
    c->zero_threshold = 0.0f;
    c->stability_window = 5;
    c->stability_tolerance = 0.5f;
    c->audio_volume = 70;
    c->audio_muted = false;
    c->mqtt_port = 8883;
    c->mqtt_tls = true;
    c->lang = 0;
}
#define LOAD_U8(handle, key, dst)                  \
    do                                             \
    {                                              \
        uint8_t v;                                 \
        if (nvs_get_u8(handle, key, &v) == ESP_OK) \
            (dst) = v;                             \
    } while (0)
#define LOAD_U16(handle, key, dst)                  \
    do                                              \
    {                                               \
        uint16_t v;                                 \
        if (nvs_get_u16(handle, key, &v) == ESP_OK) \
            (dst) = v;                              \
    } while (0)
#define LOAD_U32(handle, key, dst)                  \
    do                                              \
    {                                               \
        uint32_t v;                                 \
        if (nvs_get_u32(handle, key, &v) == ESP_OK) \
            (dst) = v;                              \
    } while (0)
#define LOAD_F32(handle, key, dst)                  \
    do                                              \
    {                                               \
        uint32_t v;                                 \
        if (nvs_get_u32(handle, key, &v) == ESP_OK) \
            memcpy(&(dst), &v, sizeof(float));      \
    } while (0)
#define LOAD_STR(handle, key, dst)           \
    do                                       \
    {                                        \
        size_t l = sizeof(dst);              \
        nvs_get_str(handle, key, (dst), &l); \
    } while (0)
#define LOAD_BOOL(handle, key, dst)                \
    do                                             \
    {                                              \
        uint8_t v;                                 \
        if (nvs_get_u8(handle, key, &v) == ESP_OK) \
            (dst) = v != 0;                        \
    } while (0)

/* Build-provisioned IDs (DEVICE_ID / MACHINE_ID files at project root).
 * NVS remembers the last build value applied ("*_fw" marker keys):
 *  - build value changed since last flash -> overwrite the saved ID once
 *  - build value unchanged                -> keep NVS (HMI edits win)
 *  - saved ID empty                       -> fill from build value      */
static bool provision_id(nvs_handle_t h, const char *val_key, const char *marker_key,
                         const char *build_val, char *dst, size_t dst_len)
{
    char last[64] = "";
    size_t len = sizeof(last);
    if (nvs_get_str(h, marker_key, last, &len) != ESP_OK)
        last[0] = '\0';

    if (strcmp(last, build_val) == 0)
    {
        if (dst[0] != '\0')
            return false; /* same build as before - keep saved/HMI value */
        ESP_LOGW(TAG, "%s empty - filling from build: %s", val_key, build_val);
    }
    else
    {
        ESP_LOGW(TAG, "%s: new build value %s (saved was '%s') - overwriting",
                 val_key, build_val, dst[0] ? dst : "");
        nvs_set_str(h, marker_key, build_val);
    }

    strlcpy(dst, build_val, dst_len);
    nvs_set_str(h, val_key, dst);
    return true;
}

static void apply_build_ids(void)
{
#if defined(DEFAULT_DEVICE_ID) || defined(DEFAULT_MACHINE_ID)
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK)
    {
        /* NVS unusable - at least don't run with empty IDs */
#ifdef DEFAULT_DEVICE_ID
        if (!s_cfg.device_id[0])
            strlcpy(s_cfg.device_id, DEFAULT_DEVICE_ID, sizeof(s_cfg.device_id));
#endif
#ifdef DEFAULT_MACHINE_ID
        if (!s_cfg.machine_id[0])
            strlcpy(s_cfg.machine_id, DEFAULT_MACHINE_ID, sizeof(s_cfg.machine_id));
#endif
        return;
    }

    bool dirty = false;
#ifdef DEFAULT_DEVICE_ID
    dirty |= provision_id(h, "devid", "devid_fw", DEFAULT_DEVICE_ID,
                          s_cfg.device_id, sizeof(s_cfg.device_id));
#endif
#ifdef DEFAULT_MACHINE_ID
    dirty |= provision_id(h, "machid", "machid_fw", DEFAULT_MACHINE_ID,
                          s_cfg.machine_id, sizeof(s_cfg.machine_id));
#endif
    if (dirty)
        nvs_commit(h);
    nvs_close(h);
#endif
}

esp_err_t weight_config_init(void)
{
    apply_defaults(&s_cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGI(TAG, "no saved config, using defaults");
        goto done;
    }
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        goto done; /* still proceed with defaults */
    }

    LOAD_STR(h, "tenant_id", s_cfg.tenant_id);
    LOAD_STR(h, "tenant_srt", s_cfg.tenant_short);
    LOAD_U8(h, "decimal", s_cfg.decimal_places);
    LOAD_U8(h, "unit", s_cfg.unit);
    LOAD_U32(h, "baud", s_cfg.uart_baud);
    LOAD_F32(h, "zthr", s_cfg.zero_threshold);
    LOAD_U8(h, "stb_win", s_cfg.stability_window);
    LOAD_F32(h, "stb_tol", s_cfg.stability_tolerance);
    LOAD_U8(h, "vol", s_cfg.audio_volume);
    LOAD_BOOL(h, "mute", s_cfg.audio_muted);
    LOAD_STR(h, "wssid", s_cfg.wifi_ssid);
    LOAD_STR(h, "wpass", s_cfg.wifi_pass);
    LOAD_BOOL(h, "wstatic", s_cfg.wifi_use_static);
    LOAD_STR(h, "wip", s_cfg.wifi_ip);
    LOAD_STR(h, "wgw", s_cfg.wifi_gw);
    LOAD_STR(h, "wmask", s_cfg.wifi_netmask);
    LOAD_STR(h, "wdns", s_cfg.wifi_dns);
    LOAD_STR(h, "devid", s_cfg.device_id);
    LOAD_STR(h, "machid", s_cfg.machine_id);
    LOAD_STR(h, "devname", s_cfg.device_name);
    LOAD_STR(h, "mhost", s_cfg.mqtt_host);
    LOAD_U16(h, "mport", s_cfg.mqtt_port);
    LOAD_STR(h, "muser", s_cfg.mqtt_user);
    LOAD_STR(h, "mpass", s_cfg.mqtt_pass);
    LOAD_BOOL(h, "mtls", s_cfg.mqtt_tls);
    LOAD_U8(h, "lang", s_cfg.lang);
    nvs_close(h);

done:
    apply_build_ids();

    ESP_LOGI(TAG, "device_id=%s machine_id=%s",
             s_cfg.device_id[0] ? s_cfg.device_id : "(unset)",
             s_cfg.machine_id[0] ? s_cfg.machine_id : "(unset)");
    ESP_LOGI(TAG, "config loaded (dec=%d unit=%s baud=%lu vol=%d mqtt=%s:%d tls=%d)",
             s_cfg.decimal_places, weight_config_unit_str(s_cfg.unit),
             (unsigned long)s_cfg.uart_baud, s_cfg.audio_volume,
             s_cfg.mqtt_host, s_cfg.mqtt_port, s_cfg.mqtt_tls);
    return ESP_OK;
}

const weight_config_t *weight_config_get(void) { return &s_cfg; }

void weight_config_set_device_id(const char *id)
{
    strlcpy(s_cfg.device_id, id, sizeof(s_cfg.device_id));
}
void weight_config_set_device_name(const char *name)
{
    strlcpy(s_cfg.device_name, name, sizeof(s_cfg.device_name));
}
void weight_config_set_decimal(uint8_t d) { s_cfg.decimal_places = (d > 3) ? 3 : d; }
void weight_config_set_unit(wcfg_unit_t u) { s_cfg.unit = u; }
void weight_config_set_uart_baud(uint32_t b) { s_cfg.uart_baud = b; }
void weight_config_set_zero_threshold(float t) { s_cfg.zero_threshold = t; }
void weight_config_set_stability(uint8_t w, float tol)
{
    s_cfg.stability_window = (w < 3) ? 3 : (w > 10 ? 10 : w);
    s_cfg.stability_tolerance = tol;
}
void weight_config_set_audio(uint8_t v, bool m)
{
    s_cfg.audio_volume = (v > 100) ? 100 : v;
    s_cfg.audio_muted = m;
}
void weight_config_set_wifi(const char *ssid, const char *pass)
{
    strncpy(s_cfg.wifi_ssid, ssid, sizeof(s_cfg.wifi_ssid) - 1);
    strncpy(s_cfg.wifi_pass, pass, sizeof(s_cfg.wifi_pass) - 1);
}
void weight_config_set_static_ip(bool use_static, const char *ip,
                                 const char *gw, const char *netmask,
                                 const char *dns)
{
    s_cfg.wifi_use_static = use_static;
    strncpy(s_cfg.wifi_ip, ip ? ip : "", sizeof(s_cfg.wifi_ip) - 1);
    strncpy(s_cfg.wifi_gw, gw ? gw : "", sizeof(s_cfg.wifi_gw) - 1);
    strncpy(s_cfg.wifi_netmask, netmask ? netmask : "", sizeof(s_cfg.wifi_netmask) - 1);
    strncpy(s_cfg.wifi_dns, dns ? dns : "", sizeof(s_cfg.wifi_dns) - 1);
}
void weight_config_set_mqtt(const char *host, uint16_t port,
                            const char *user, const char *pass, bool tls)
{
    strncpy(s_cfg.mqtt_host, host, sizeof(s_cfg.mqtt_host) - 1);
    s_cfg.mqtt_port = port;
    strncpy(s_cfg.mqtt_user, user, sizeof(s_cfg.mqtt_user) - 1);
    strncpy(s_cfg.mqtt_pass, pass, sizeof(s_cfg.mqtt_pass) - 1);
    s_cfg.mqtt_tls = tls;
}

esp_err_t weight_config_get_ca_cert(char *out, size_t out_len, size_t *actual_len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK)
        return err;
    size_t l = out_len;
    err = nvs_get_str(h, "ca_cert", out, &l);
    nvs_close(h);
    if (err == ESP_OK && actual_len)
        *actual_len = l;
    return err;
}

esp_err_t weight_config_set_ca_cert(const char *pem)
{
    if (!pem)
        return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    err = nvs_set_str(h, "ca_cert", pem);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t weight_config_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;

    uint32_t f;
    nvs_set_str(h, "tenant_id", s_cfg.tenant_id);
    nvs_set_str(h, "tenant_srt", s_cfg.tenant_short);
    nvs_set_u8(h, "decimal", s_cfg.decimal_places);
    nvs_set_u8(h, "unit", (uint8_t)s_cfg.unit);
    nvs_set_u32(h, "baud", s_cfg.uart_baud);
    memcpy(&f, &s_cfg.zero_threshold, 4);
    nvs_set_u32(h, "zthr", f);
    nvs_set_u8(h, "stb_win", s_cfg.stability_window);
    memcpy(&f, &s_cfg.stability_tolerance, 4);
    nvs_set_u32(h, "stb_tol", f);
    nvs_set_u8(h, "vol", s_cfg.audio_volume);
    nvs_set_u8(h, "mute", s_cfg.audio_muted ? 1 : 0);
    nvs_set_str(h, "wssid", s_cfg.wifi_ssid);
    nvs_set_str(h, "wpass", s_cfg.wifi_pass);
    nvs_set_u8(h, "wstatic", s_cfg.wifi_use_static ? 1 : 0);
    nvs_set_str(h, "wip", s_cfg.wifi_ip);
    nvs_set_str(h, "wgw", s_cfg.wifi_gw);
    nvs_set_str(h, "wmask", s_cfg.wifi_netmask);
    nvs_set_str(h, "wdns", s_cfg.wifi_dns);
    nvs_set_str(h, "devid", s_cfg.device_id);
    nvs_set_str(h, "machid", s_cfg.machine_id);
    nvs_set_str(h, "devname", s_cfg.device_name);
    nvs_set_str(h, "mhost", s_cfg.mqtt_host);
    nvs_set_u16(h, "mport", s_cfg.mqtt_port);
    nvs_set_str(h, "muser", s_cfg.mqtt_user);
    nvs_set_str(h, "mpass", s_cfg.mqtt_pass);
    nvs_set_u8(h, "mtls", s_cfg.mqtt_tls ? 1 : 0);
    nvs_set_u8(h, "lang", s_cfg.lang);

    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "config saved");
    return err;
}

const char *weight_config_unit_str(wcfg_unit_t u)
{
    switch (u)
    {
    case WCFG_UNIT_G:
        return "g";
    case WCFG_UNIT_KG:
        return "kg";
    case WCFG_UNIT_LB:
        return "lb";
    case WCFG_UNIT_OZ:
        return "oz";
    default:
        return "g";
    }
}

void weight_config_set_tenant(const char *tenant_id, const char *tenant_short)
{
    if (tenant_id)
        strlcpy(s_cfg.tenant_id, tenant_id, sizeof(s_cfg.tenant_id));
    if (tenant_short)
        strlcpy(s_cfg.tenant_short, tenant_short, sizeof(s_cfg.tenant_short));
}

int weight_config_get_lang(void) { return (int)s_cfg.lang; }

void weight_config_set_lang(int l) { s_cfg.lang = (uint8_t)(l & 0x01); }

void weight_config_set_machine_id(const char *id)
{
    strlcpy(s_cfg.machine_id, id ? id : "", sizeof(s_cfg.machine_id));
}
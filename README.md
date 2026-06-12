# Weighing Scale HMI - ESP32-P4

Industrial weighing-scale HMI for Malaysian SME factories. Reads ASCII weight
data from RS232, displays on a 10.1" touch panel, compares against per-model
high/low limits, alerts via on-board speaker, and publishes results to MQTT
for the GIM platform.

## Hardware

- **Board:** Waveshare ESP32-P4-WIFI6-Touch-LCD-10.1
  - 1280 x 800 IPS touch panel (MIPI-DSI)
  - ES8311 audio codec + built-in speaker
  - 32MB NOR flash, 32MB PSRAM
  - WiFi 6 via on-board ESP32-C6 (SDIO)
- **RS232 converter:** external MAX3232 module on the 40-pin header
  - TX = GPIO 37, RX = GPIO 38
- **Scale:** any ASCII-streaming weighing indicator (configurable baud, default 9600)

## Repository layout

```
weight-hmi-p4/
├── CMakeLists.txt           - ESP-IDF project root
├── partitions.csv           - 4M app, 2M spiffs (audio), nvs, phy
├── sdkconfig.defaults       - target=esp32p4, psram, flashsize, freertos tuning
├── main/
│   ├── main.c               - boot sequence + tenant_id macro
│   └── idf_component.yml    - registry deps (lvgl, codec_dev, lcd drivers...)
├── components/
│   ├── weight_state         - shared state struct + mutex (single source of truth)
│   ├── weight_config        - NVS device config (decimal, unit, baud, audio, mqtt creds...)
│   ├── weight_uart          - RS232 reader + ASCII parser (mirrors your Arduino logic)
│   ├── weight_evaluator     - stability detection + pass/high/low + audio + mqtt triggers
│   ├── weight_model_store   - 100-slot model cache in NVS, MQTT sync ingest
│   ├── weight_mqtt          - EMQX client with TLS + basic auth, reading + status publish
│   ├── weight_audio         - ES8311 init, WAV preload to PSRAM, playback
│   ├── weight_wifi          - station mode, scan + connect from HMI
│   └── weight_ui            - LVGL screens (main, models, edit, settings, wifi)
├── spiffs/                  - alert WAV files (drop here, get auto-flashed)
└── supabase/
    └── 0001_weight_scale_hmi.sql
```

## Architecture - data flow

```
RS232 line --> weight_uart -> sample queue --> weight_evaluator -+--> weight_state (snapshot for UI)
                                                                  +--> weight_audio (pass/low/high beep)
                                                                  +--> weight_mqtt (publish on stable)

Supabase weight_models -> Go service -> MQTT sync topic -> weight_mqtt -> weight_model_store -> NVS
                                                                                              -> UI dropdown
```

## Build

```bash
# 1. Install ESP-IDF 5.3 or newer
# 2. export IDF_PATH and source export.sh

# 3. Set tenant_id in main/main.c (TENANT_ID macro at top of file)

# 4. Configure
go to project root

idf.py set-target esp32p4
idf.py menuconfig    # optional, defaults are sane

# 5. Build + flash + monitor
idf.py -p /dev/ttyUSB0 flash monitor
```

First boot: device has no WiFi creds, settings screen lets operator pick SSID
and type password. MQTT creds and CA cert can also be entered via settings, or
preconfigured by editing the NVS image before flashing.

## Settings entered via HMI

- WiFi: SSID + password (scan picker + on-screen keyboard)
- MQTT: host, port (default 8883), username, password, TLS toggle, CA cert paste
- Display: decimal places (0-3), unit (g / kg / lb / oz)
- Stability: window (3-10 samples), tolerance (e.g. 0.5)
- Scale: baud (4800 / 9600 / 19200 / 38400), zero threshold (below = ignored)
- Audio: volume (0-100), mute toggle, test buttons

## MQTT topics

Topics use the short tenant slug (`TENANT_SHORT` macro in `main.c`, e.g. `gim`)
to keep them compact on the broker. The JSON payload still carries the full
`tenant_id` UUID for database joins.

- PUB `{tenant_short}/{device_id}/weight` - reading JSON, QoS 1
- PUB `{tenant_short}/{device_id}/status` - retained + LWT, QoS 1
- PUB `{tenant_short}/{device_id}/sync/req` - resync request on boot
- SUB `{tenant_short}/models/sync/+` - upsert/delete events

Example with `TENANT_SHORT="gim"` and device `p4-7c4f1a2b3c4d`:

- `gim/p4-7c4f1a2b3c4d/weight`
- `gim/p4-7c4f1a2b3c4d/status`
- `gim/models/sync/abc-uuid`

Reading payload:

```json
{
  "device_id": "p4-7c4f1a2b3c4d",
  "tenant_id": "00000000-0000-0000-0000-000000000000",
  "timestamp": "2026-06-05T14:32:18Z",
  "model_id": "uuid",
  "model_name": "200g sachet",
  "reading": 1234.5,
  "unit": "g",
  "status": "pass",
  "limits": { "lower": 1150.0, "standard": 1200.0, "upper": 1250.0 }
}
```

## Status / current scope

This scaffold compiles and links once the Waveshare BSP for the 10.1" panel
is vendored under `components/` (or pulled via idf_component.yml). Component
implementations are functional except for:

- `weight_audio.c::weight_audio_init()` - calls `bsp_audio_codec_speaker_init()`
  which must come from the BSP
- `weight_ui.c::hw_init_stub()` - replace with `bsp_display_start()` from BSP
- All screens in `weight_ui/screens/` are placeholders - UX is sketched in the
  earlier design conversation; LVGL implementation comes next

## Importnant steps

```
\managed_components\espressif_esp_wifi_remote\kconfig

change all $ESP_IDF_VERSION to hardcoded version "5.5"
```

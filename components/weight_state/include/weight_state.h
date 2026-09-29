/**
 * @file weight_state.h
 * @brief Single source of truth for live system state.
 *
 * All tasks read/write through this API which handles the mutex internally.
 * Never touch the static state struct from outside this component.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define WEIGHT_MAX_MODEL_NAME 32
#define WEIGHT_MAX_MODEL_ID_LEN 37 /* uuid string + nul */
#define WEIGHT_MAX_UNIT_LEN 8      /* "g", "kg", "lb", "oz" */
#define WEIGHT_MAX_DEVICE_ID_LEN 37
#define WEIGHT_MAX_TENANT_ID_LEN 37
#define WEIGHT_MAX_TENANT_SHORT_LEN 16 /* short slug like "gim" used in MQTT topics */

/* Any time_t below this means "clock not set yet" (SNTP not synced). 2023-11-14. */
#define WEIGHT_TIME_VALID_EPOCH ((time_t)1700000000)

    /* Pass/fail status of latest stable reading */
    typedef enum
    {
        WEIGHT_STATUS_NONE = 0, /* no reading yet / not in run mode */
        WEIGHT_STATUS_PASS,     /* lower <= reading <= upper */
        WEIGHT_STATUS_HIGH,     /* reading > upper */
        WEIGHT_STATUS_LOW,      /* reading < lower */
    } weight_status_t;

    /* Operating mode */
    typedef enum
    {
        WEIGHT_MODE_IDLE = 0, /* device powered, model selected or not, NOT publishing */
        WEIGHT_MODE_RUN,      /* model selected, READY pressed, publishing on stable readings */
    } weight_mode_t;

    /* Active model parameters (denormalised from weight_model_store for fast access) */
    typedef struct
    {
        char id[WEIGHT_MAX_MODEL_ID_LEN];
        char name[WEIGHT_MAX_MODEL_NAME];
        float lower_limit;
        float standard;
        float upper_limit;
        char unit[WEIGHT_MAX_UNIT_LEN];
        bool valid; /* false until a model is selected */
    } weight_active_model_t;

    /* Live state snapshot. Returned by weight_state_get_snapshot for read-only consumers. */
    typedef struct
    {
        /* Device identity */
        char device_id[WEIGHT_MAX_DEVICE_ID_LEN]; /* derived from MAC, set once at boot */
        char tenant_id[WEIGHT_MAX_TENANT_ID_LEN];
        char tenant_short[WEIGHT_MAX_TENANT_SHORT_LEN]; /* MQTT topic prefix */

        /* Operating mode */
        weight_mode_t mode;
        time_t run_started_ts; /* last IDLE->RUN transition, 0 = never */
        time_t run_stopped_ts; /* last RUN->IDLE transition, 0 = running / never stopped */

        /* Currently selected model (only one at a time) */
        weight_active_model_t model;

        /* Latest reading - updated by evaluator */
        float last_reading;
        bool last_stable;
        weight_status_t last_status;
        time_t last_reading_ts; /* unix epoch seconds */

        /* Sticky alert state - raised by the evaluator once per placement,
         * cleared only by user dismiss action */
        bool alert_active;
        weight_status_t alert_status;
        float alert_reading;
        time_t alert_ts;

        /* Connectivity */
        bool wifi_connected;
        bool mqtt_connected;
    } weight_state_snapshot_t;

    /* Called (outside the state lock) whenever the mode or the active model
     * changes. weight_mqtt uses it to push the retained status message.
     * Must not block. */
    typedef void (*weight_state_change_cb_t)(void);

    /* ---------- Lifecycle ---------- */
    esp_err_t weight_state_init(const char *tenant_id, const char *tenant_short);
    void weight_state_set_change_cb(weight_state_change_cb_t cb);

    /* ---------- Read ---------- */
    /* Atomically copy the entire state into the caller's buffer. */
    void weight_state_get_snapshot(weight_state_snapshot_t *out);
    void weight_state_set_tenant(const char *tenant_id, const char *tenant_short);

    /* ---------- Write (these acquire mutex internally) ---------- */
    void weight_state_set_mode(weight_mode_t mode);
    void weight_state_set_active_model(const weight_active_model_t *model);
    void weight_state_clear_active_model(void);

    /* Model store hooks - keep the denormalised active copy in sync.
     * refresh: if the active model has the same id, copy name/limits/unit.
     * drop:    if the active model has this id, clear it and drop to IDLE. */
    bool weight_state_refresh_active_model(const weight_active_model_t *model);
    bool weight_state_drop_active_model_if(const char *id);

    /* Called by evaluator on every evaluated sample. Does NOT raise alerts. */
    void weight_state_update_reading(float value, bool stable,
                                     weight_status_t status, time_t ts);

    /* Called by evaluator once per placement, on a fresh stable HIGH/LOW in RUN. */
    void weight_state_raise_alert(weight_status_t status, float value, time_t ts);

    /* Called by UI when alert dismiss tapped. */
    void weight_state_dismiss_alert(void);

    /* Connectivity flags */
    void weight_state_set_wifi(bool connected);
    void weight_state_set_mqtt(bool connected);

    /* ---------- Convenience getters (read with mutex) ---------- */
    weight_mode_t weight_state_get_mode(void);
    bool weight_state_has_active_model(void);

#ifdef __cplusplus
}
#endif

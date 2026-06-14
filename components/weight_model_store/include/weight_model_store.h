/**
 * @file weight_model_store.h
 * @brief Local model cache (NVS) + MQTT sync ingestion.
 *
 * Models come from Supabase via the Go service which publishes upserts to
 *   {tenant_short}/models/sync/{model_id}
 * Each device subscribes, applies, and persists locally for offline use.
 *
 * The local list is the cache; Supabase is the source of truth.
 * Operator-side edits on the HMI (register / edit limits) are persisted locally
 * AND published to a request topic for the Go service to upsert into Supabase.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "weight_state.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define WEIGHT_MAX_MODELS 100

    typedef struct
    {
        char id[WEIGHT_MAX_MODEL_ID_LEN];
        char name[WEIGHT_MAX_MODEL_NAME];
        float lower_limit;
        float standard;
        float upper_limit;
        char unit[WEIGHT_MAX_UNIT_LEN];
        int64_t updated_at; /* unix ms, for last-write-wins */
        bool deleted;
    } weight_model_t;

    esp_err_t weight_model_store_init(void);

    /* Read API */
    size_t weight_model_store_count(void);
    bool weight_model_store_get(size_t index, weight_model_t *out);
    bool weight_model_store_find_by_id(const char *id, weight_model_t *out);

    /* Mutation API - persists to NVS and (when called by UI) publishes to broker
     * via the request topic. Sync ingestions call _apply_remote which doesn't
     * re-publish to avoid loops. */
    esp_err_t weight_model_store_upsert_local(const weight_model_t *m);
    esp_err_t weight_model_store_delete_local(const char *id);

    /* Called by weight_mqtt when an upstream sync message arrives. JSON is the
     * raw payload from the sync topic. */
    esp_err_t weight_model_store_apply_remote_json(const char *json);

    /* Helpers */
    const weight_model_t *weight_model_store_array(void); /* read-only pointer */

#ifdef __cplusplus
}
#endif

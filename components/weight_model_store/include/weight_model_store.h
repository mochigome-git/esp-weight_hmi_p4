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
 *
 * Conflict rule: last-write-wins on updated_at (unix ms). Remote records that
 * are older than the local copy are ignored; identical records are no-ops
 * (no flash write).
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

    struct cJSON; /* forward decl - avoids pulling cJSON into every includer */

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

    /* Incremented on every effective change (add/edit/delete). The UI polls
     * this to know when to rebuild lists - catches renames and limit edits,
     * not just count changes. */
    uint32_t weight_model_store_revision(void);

    /* Mutation API.
     * upsert_local / delete_local persist to NVS immediately (unless inside a
     * batch). They do not publish anything. */
    esp_err_t weight_model_store_upsert_local(const weight_model_t *m);
    esp_err_t weight_model_store_delete_local(const char *id);

    /* For operator edits from the HMI: fills in a UUIDv4 id if empty and
     * stamps updated_at (so a stale echo from the server can't overwrite
     * the edit), writes the result back into *m, then upserts. Publish *m
     * afterwards so the server sees the same id. */
    esp_err_t weight_model_store_save_from_ui(weight_model_t *m);

    /* Generate a UUIDv4 string (buffer >= 37 bytes). */
    void weight_model_store_new_id(char *out, size_t len);

    /* Batch: defer the NVS commit until the matching end_batch(). Nestable.
     * Used by the MQTT sync handler so a 100-model resync is one flash write. */
    void weight_model_store_begin_batch(void);
    esp_err_t weight_model_store_end_batch(void);

    /* Called by weight_mqtt when an upstream sync message arrives. */
    esp_err_t weight_model_store_apply_remote_json(const char *json);
    esp_err_t weight_model_store_apply_remote_item(const struct cJSON *item);

    /* Helpers - NOT thread-safe, prefer _get() */
    const weight_model_t *weight_model_store_array(void);

#ifdef __cplusplus
}
#endif

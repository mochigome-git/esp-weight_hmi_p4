/**
 * @file weight_model_store.c
 *
 * Storage layout (NVS namespace "models"):
 *   "count"          uint16     - number of models stored
 *   "m_NNN"          blob       - serialised weight_model_t for index NNN
 *
 * On boot, loads all into RAM. All reads from RAM. Writes go to RAM + NVS.
 */

#include <string.h>
#include <stdio.h>
#include "weight_model_store.h"
#include "esp_log.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "models";
static const char *NS = "models";

static weight_model_t s_models[WEIGHT_MAX_MODELS];
static size_t s_count;
static SemaphoreHandle_t s_mtx;

#define LOCK() xSemaphoreTake(s_mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mtx)

static int find_index_locked(const char *id)
{
    for (size_t i = 0; i < s_count; i++)
    {
        if (strcmp(s_models[i].id, id) == 0)
            return (int)i;
    }
    return -1;
}

static esp_err_t persist_all_locked(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;

    nvs_set_u16(h, "count", (uint16_t)s_count);
    for (size_t i = 0; i < s_count; i++)
    {
        /* NVS keys max 15 chars. %hu + uint16_t cast caps at 5 digits → fits comfortably,
         * and lets gcc see the value is bounded so -Wformat-truncation is happy. */
        char key[12];
        snprintf(key, sizeof(key), "m_%03hu", (uint16_t)i);
        nvs_set_blob(h, key, &s_models[i], sizeof(weight_model_t));
    }
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t weight_model_store_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx)
        return ESP_ERR_NO_MEM;
    s_count = 0;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK)
    {
        ESP_LOGI(TAG, "no cached models, starting empty");
        return ESP_OK;
    }

    uint16_t count = 0;
    nvs_get_u16(h, "count", &count);
    if (count > WEIGHT_MAX_MODELS)
        count = WEIGHT_MAX_MODELS;

    for (size_t i = 0; i < count; i++)
    {
        char key[12];
        snprintf(key, sizeof(key), "m_%03hu", (uint16_t)i);
        size_t sz = sizeof(weight_model_t);
        if (nvs_get_blob(h, key, &s_models[s_count], &sz) == ESP_OK)
        {
            s_count++;
        }
    }
    nvs_close(h);
    ESP_LOGI(TAG, "loaded %u models from NVS", (unsigned)s_count);
    return ESP_OK;
}

size_t weight_model_store_count(void)
{
    if (!s_mtx)
        return 0; // ← init hasn't run yet, safe fallback
    LOCK();
    size_t n = s_count;
    UNLOCK();
    return n;
}

bool weight_model_store_get(size_t i, weight_model_t *out)
{
    if (!s_mtx)
        return false; // ← safe fallback
    bool ok = false;
    LOCK();
    if (i < s_count)
    {
        memcpy(out, &s_models[i], sizeof(*out));
        ok = true;
    }
    UNLOCK();
    return ok;
}

bool weight_model_store_find_by_id(const char *id, weight_model_t *out)
{
    if (!s_mtx)
        return false;

    bool ok = false;
    LOCK();
    int idx = find_index_locked(id);
    if (idx >= 0)
    {
        memcpy(out, &s_models[idx], sizeof(*out));
        ok = true;
    }
    UNLOCK();
    return ok;
}

esp_err_t weight_model_store_upsert_local(const weight_model_t *m)
{
    if (!m || m->id[0] == '\0')
        return ESP_ERR_INVALID_ARG;
    LOCK();
    int idx = find_index_locked(m->id);
    if (idx >= 0)
    {
        memcpy(&s_models[idx], m, sizeof(*m));
    }
    else
    {
        if (s_count >= WEIGHT_MAX_MODELS)
        {
            UNLOCK();
            return ESP_ERR_NO_MEM;
        }
        memcpy(&s_models[s_count++], m, sizeof(*m));
    }
    esp_err_t err = persist_all_locked();
    UNLOCK();
    ESP_LOGI(TAG, "upsert %s '%s'", m->id, m->name);
    return err;
}

esp_err_t weight_model_store_delete_local(const char *id)
{
    LOCK();
    int idx = find_index_locked(id);
    if (idx < 0)
    {
        UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    /* Shift down */
    for (size_t i = idx; i < s_count - 1; i++)
    {
        s_models[i] = s_models[i + 1];
    }
    s_count--;
    esp_err_t err = persist_all_locked();
    UNLOCK();
    ESP_LOGI(TAG, "deleted %s", id);
    return err;
}

esp_err_t weight_model_store_apply_remote_json(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root)
        return ESP_ERR_INVALID_ARG;

    weight_model_t m;
    memset(&m, 0, sizeof(m));

    cJSON *j;
    if ((j = cJSON_GetObjectItem(root, "id")) && cJSON_IsString(j))
        strncpy(m.id, j->valuestring, sizeof(m.id) - 1);
    if ((j = cJSON_GetObjectItem(root, "name")) && cJSON_IsString(j))
        strncpy(m.name, j->valuestring, sizeof(m.name) - 1);
    if ((j = cJSON_GetObjectItem(root, "lower_limit")) && cJSON_IsNumber(j))
        m.lower_limit = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItem(root, "standard")) && cJSON_IsNumber(j))
        m.standard = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItem(root, "upper_limit")) && cJSON_IsNumber(j))
        m.upper_limit = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItem(root, "unit")) && cJSON_IsString(j))
        strncpy(m.unit, j->valuestring, sizeof(m.unit) - 1);
    if ((j = cJSON_GetObjectItem(root, "updated_at")) && cJSON_IsNumber(j))
        m.updated_at = (int64_t)j->valuedouble;
    if ((j = cJSON_GetObjectItem(root, "deleted")) && cJSON_IsBool(j))
        m.deleted = cJSON_IsTrue(j);

    cJSON_Delete(root);

    if (m.id[0] == '\0')
        return ESP_ERR_INVALID_ARG;

    if (m.deleted)
    {
        return weight_model_store_delete_local(m.id);
    }
    return weight_model_store_upsert_local(&m);
}

const weight_model_t *weight_model_store_array(void)
{
    return s_models;
}

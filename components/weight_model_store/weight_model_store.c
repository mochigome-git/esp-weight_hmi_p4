/**
 * @file weight_model_store.c
 *
 * Storage layout (NVS namespace "models") - unchanged from v1 so existing
 * devices keep their cache:
 *   "count"          uint16     - number of models stored
 *   "rec_sz"         uint16     - sizeof(weight_model_t) the blobs were written with
 *   "m_NNN"          blob       - serialised weight_model_t for index NNN
 *
 * On boot, loads all into RAM. All reads from RAM.
 *
 * Writes are diff-based: a shadow copy of what is in flash is kept, and only
 * records whose bytes changed are rewritten. A resync of 100 unchanged models
 * therefore writes nothing, and inside a batch everything is committed once.
 */

#include <string.h>
#include <stdio.h>
#include <sys/time.h>
#include "weight_model_store.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "models";
static const char *NS = "models";

static weight_model_t s_models[WEIGHT_MAX_MODELS];
static size_t s_count;

/* What is currently in NVS (for diff-based writes) */
static weight_model_t s_flash[WEIGHT_MAX_MODELS];
static size_t s_flash_count;

static SemaphoreHandle_t s_mtx; /* recursive: batch + nested calls */
static int s_batch_depth;
static bool s_dirty;
static volatile uint32_t s_revision;

#define LOCK() xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGiveRecursive(s_mtx)

static void make_key(char *key, size_t len, size_t i)
{
    /* NVS keys max 15 chars. %hu + uint16_t cast keeps gcc's
     * -Wformat-truncation happy. */
    snprintf(key, len, "m_%03hu", (uint16_t)i);
}

static int find_index_locked(const char *id)
{
    for (size_t i = 0; i < s_count; i++)
    {
        if (strcmp(s_models[i].id, id) == 0)
            return (int)i;
    }
    return -1;
}

static int64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static bool clock_valid(void)
{
    return time(NULL) >= WEIGHT_TIME_VALID_EPOCH;
}

/* Write only what differs from the shadow copy. */
static esp_err_t flush_locked(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }

    int writes = 0;
    esp_err_t first_err = ESP_OK;

    for (size_t i = 0; i < s_count; i++)
    {
        if (i < s_flash_count && memcmp(&s_models[i], &s_flash[i], sizeof(weight_model_t)) == 0)
            continue;
        char key[12];
        make_key(key, sizeof(key), i);
        esp_err_t e = nvs_set_blob(h, key, &s_models[i], sizeof(weight_model_t));
        if (e == ESP_OK)
        {
            s_flash[i] = s_models[i];
            writes++;
        }
        else if (first_err == ESP_OK)
            first_err = e;
    }

    /* Remove records past the new end */
    for (size_t i = s_count; i < s_flash_count; i++)
    {
        char key[12];
        make_key(key, sizeof(key), i);
        esp_err_t e = nvs_erase_key(h, key);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND && first_err == ESP_OK)
            first_err = e;
        writes++;
    }

    if (s_count != s_flash_count)
    {
        nvs_set_u16(h, "count", (uint16_t)s_count);
        writes++;
    }
    nvs_set_u16(h, "rec_sz", (uint16_t)sizeof(weight_model_t)); /* no-op if unchanged */

    err = nvs_commit(h);
    nvs_close(h);

    if (first_err == ESP_OK && err == ESP_OK)
    {
        s_flash_count = s_count;
        s_dirty = false;
    }
    else
    {
        /* keep s_dirty so the next change retries */
        ESP_LOGE(TAG, "persist failed: %s", esp_err_to_name(first_err != ESP_OK ? first_err : err));
    }
    if (writes)
        ESP_LOGD(TAG, "persisted %d record changes (%u models)", writes, (unsigned)s_count);
    return first_err != ESP_OK ? first_err : err;
}

static esp_err_t persist_locked(void)
{
    s_dirty = true;
    if (s_batch_depth > 0)
        return ESP_OK; /* committed by end_batch */
    return flush_locked();
}

static void to_active(const weight_model_t *m, weight_active_model_t *am)
{
    memset(am, 0, sizeof(*am));
    strlcpy(am->id, m->id, sizeof(am->id));
    strlcpy(am->name, m->name, sizeof(am->name));
    strlcpy(am->unit, m->unit, sizeof(am->unit));
    am->lower_limit = m->lower_limit;
    am->standard = m->standard;
    am->upper_limit = m->upper_limit;
    am->valid = true;
}

esp_err_t weight_model_store_init(void)
{
    s_mtx = xSemaphoreCreateRecursiveMutex();
    if (!s_mtx)
        return ESP_ERR_NO_MEM;
    s_count = 0;
    s_flash_count = 0;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK)
    {
        ESP_LOGI(TAG, "no cached models, starting empty");
        return ESP_OK;
    }

    uint16_t rec_sz = 0;
    if (nvs_get_u16(h, "rec_sz", &rec_sz) == ESP_OK && rec_sz != sizeof(weight_model_t))
    {
        /* weight_model_t layout changed between firmware versions - the
         * blobs can't be trusted. Start empty; the broker resync refills. */
        ESP_LOGW(TAG, "model record size changed (%u -> %u), discarding cache",
                 (unsigned)rec_sz, (unsigned)sizeof(weight_model_t));
        nvs_close(h);
        return ESP_OK;
    }

    uint16_t count = 0;
    nvs_get_u16(h, "count", &count);
    if (count > WEIGHT_MAX_MODELS)
        count = WEIGHT_MAX_MODELS;

    for (size_t i = 0; i < count; i++)
    {
        char key[12];
        make_key(key, sizeof(key), i);
        size_t sz = sizeof(weight_model_t);
        if (nvs_get_blob(h, key, &s_models[s_count], &sz) == ESP_OK &&
            sz == sizeof(weight_model_t))
        {
            s_models[s_count].id[sizeof(s_models[0].id) - 1] = '\0';
            s_models[s_count].name[sizeof(s_models[0].name) - 1] = '\0';
            s_models[s_count].unit[sizeof(s_models[0].unit) - 1] = '\0';
            s_count++;
        }
    }
    nvs_close(h);

    /* Shadow = what we just read. If some slots failed to load, the
     * indexes shifted; mark everything dirty so the next write repairs it. */
    memcpy(s_flash, s_models, s_count * sizeof(weight_model_t));
    s_flash_count = (s_count == count) ? s_count : 0;

    s_revision = 1;
    ESP_LOGI(TAG, "loaded %u models from NVS", (unsigned)s_count);
    return ESP_OK;
}

size_t weight_model_store_count(void)
{
    if (!s_mtx)
        return 0; /* init hasn't run yet */
    LOCK();
    size_t n = s_count;
    UNLOCK();
    return n;
}

uint32_t weight_model_store_revision(void)
{
    return s_revision;
}

bool weight_model_store_get(size_t i, weight_model_t *out)
{
    if (!s_mtx)
        return false;
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
    if (!s_mtx || !id)
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

void weight_model_store_new_id(char *out, size_t len)
{
    uint8_t r[16];
    esp_fill_random(r, sizeof(r));
    r[6] = (r[6] & 0x0f) | 0x40; /* version 4 */
    r[8] = (r[8] & 0x3f) | 0x80; /* variant 10 */
    snprintf(out, len,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
             r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
}

void weight_model_store_begin_batch(void)
{
    if (!s_mtx)
        return;
    LOCK();
    s_batch_depth++;
    UNLOCK();
}

esp_err_t weight_model_store_end_batch(void)
{
    if (!s_mtx)
        return ESP_ERR_INVALID_STATE;
    esp_err_t err = ESP_OK;
    LOCK();
    if (s_batch_depth > 0)
        s_batch_depth--;
    if (s_batch_depth == 0 && s_dirty)
        err = flush_locked();
    UNLOCK();
    return err;
}

/* from_remote: apply last-write-wins against the existing record. */
static esp_err_t upsert_internal(const weight_model_t *m, bool from_remote)
{
    if (!s_mtx)
        return ESP_ERR_INVALID_STATE;
    if (!m || m->name[0] == '\0')
        return ESP_ERR_INVALID_ARG;

    weight_model_t tmp;
    memcpy(&tmp, m, sizeof(tmp));
    tmp.id[sizeof(tmp.id) - 1] = '\0';
    tmp.name[sizeof(tmp.name) - 1] = '\0';
    tmp.unit[sizeof(tmp.unit) - 1] = '\0';
    tmp.deleted = false;

    if (tmp.id[0] == '\0')
        weight_model_store_new_id(tmp.id, sizeof(tmp.id));

    esp_err_t err = ESP_OK;
    bool changed = false;

    LOCK();
    int idx = find_index_locked(tmp.id);
    if (idx >= 0)
    {
        const weight_model_t *cur = &s_models[idx];
        if (from_remote && tmp.updated_at != 0 && cur->updated_at != 0 &&
            tmp.updated_at < cur->updated_at)
        {
            UNLOCK();
            ESP_LOGD(TAG, "ignore stale remote %s (%lld < %lld)", tmp.id,
                     (long long)tmp.updated_at, (long long)cur->updated_at);
            return ESP_OK;
        }
        if (memcmp(cur, &tmp, sizeof(tmp)) != 0)
        {
            s_models[idx] = tmp;
            changed = true;
        }
    }
    else
    {
        if (s_count >= WEIGHT_MAX_MODELS)
        {
            UNLOCK();
            ESP_LOGW(TAG, "store full (%d), dropping %s", WEIGHT_MAX_MODELS, tmp.id);
            return ESP_ERR_NO_MEM;
        }
        s_models[s_count++] = tmp;
        changed = true;
    }

    if (changed)
    {
        err = persist_locked();
        s_revision++;
    }
    UNLOCK();

    if (changed)
    {
        ESP_LOGI(TAG, "upsert %s '%s'", tmp.id, tmp.name);
        weight_active_model_t am;
        to_active(&tmp, &am);
        weight_state_refresh_active_model(&am);
    }
    return err;
}

esp_err_t weight_model_store_upsert_local(const weight_model_t *m)
{
    return upsert_internal(m, false);
}

esp_err_t weight_model_store_save_from_ui(weight_model_t *m)
{
    if (!m)
        return ESP_ERR_INVALID_ARG;

    if (m->id[0] == '\0')
        weight_model_store_new_id(m->id, sizeof(m->id));

    /* Newer than any copy we hold, so a stale server echo can't undo it.
     * Without a valid clock, just bump past the old value. */
    weight_model_t cur;
    int64_t prev = weight_model_store_find_by_id(m->id, &cur) ? cur.updated_at : 0;
    int64_t t = clock_valid() ? now_ms() : 0;
    m->updated_at = (t > prev) ? t : prev + 1;

    return upsert_internal(m, false);
}

esp_err_t weight_model_store_delete_local(const char *id)
{
    if (!s_mtx || !id)
        return ESP_ERR_INVALID_STATE;

    LOCK();
    int idx = find_index_locked(id);
    if (idx < 0)
    {
        UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    /* Shift down */
    for (size_t i = (size_t)idx; i + 1 < s_count; i++)
        s_models[i] = s_models[i + 1];
    s_count--;
    esp_err_t err = persist_locked();
    s_revision++;
    UNLOCK();

    ESP_LOGI(TAG, "deleted %s", id);
    weight_state_drop_active_model_if(id);
    return err;
}

esp_err_t weight_model_store_apply_remote_item(const cJSON *root)
{
    if (!root || !cJSON_IsObject(root))
        return ESP_ERR_INVALID_ARG;

    weight_model_t m;
    memset(&m, 0, sizeof(m));

    const cJSON *j;
    if ((j = cJSON_GetObjectItem(root, "id")) && cJSON_IsString(j))
        strlcpy(m.id, j->valuestring, sizeof(m.id));
    if ((j = cJSON_GetObjectItem(root, "name")) && cJSON_IsString(j))
        strlcpy(m.name, j->valuestring, sizeof(m.name));
    if ((j = cJSON_GetObjectItem(root, "lower_limit")) && cJSON_IsNumber(j))
        m.lower_limit = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItem(root, "standard")) && cJSON_IsNumber(j))
        m.standard = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItem(root, "upper_limit")) && cJSON_IsNumber(j))
        m.upper_limit = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItem(root, "unit")) && cJSON_IsString(j))
        strlcpy(m.unit, j->valuestring, sizeof(m.unit));
    if ((j = cJSON_GetObjectItem(root, "updated_at")) && cJSON_IsNumber(j))
        m.updated_at = (int64_t)j->valuedouble;
    if ((j = cJSON_GetObjectItem(root, "deleted")) && cJSON_IsBool(j))
        m.deleted = cJSON_IsTrue(j);

    if (m.id[0] == '\0')
        return ESP_ERR_INVALID_ARG;

    if (m.deleted)
    {
        esp_err_t err = weight_model_store_delete_local(m.id);
        return (err == ESP_ERR_NOT_FOUND) ? ESP_OK : err; /* tombstone for unknown id: fine */
    }
    return upsert_internal(&m, true);
}

esp_err_t weight_model_store_apply_remote_json(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root)
        return ESP_ERR_INVALID_ARG;
    esp_err_t err = weight_model_store_apply_remote_item(root);
    cJSON_Delete(root);
    return err;
}

const weight_model_t *weight_model_store_array(void)
{
    return s_models;
}

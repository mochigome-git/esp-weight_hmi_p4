#include "weight_time.h"
#include <stdbool.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_sntp.h"

static const char *TAG = "time";
static bool s_started = false;
static bool s_synced = false;

static void on_time_sync(struct timeval *tv)
{
    s_synced = true;
    ESP_LOGI(TAG, "NTP synced: epoch=%lld", (long long)tv->tv_sec);
}

void weight_time_init(void)
{
    if (s_started)
        return;
    s_started = true;

    /* Malaysia: MYT = UTC+8, no DST.
     * POSIX TZ sign is inverted: "MYT-8" means UTC+8. */
    setenv("TZ", "MYT-8", 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    sntp_set_time_sync_notification_cb(on_time_sync);
    esp_sntp_init();

    ESP_LOGI(TAG, "SNTP started, waiting for sync...");
}

bool weight_time_is_synced(void)
{
    return s_synced;
}
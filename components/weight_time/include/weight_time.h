#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * Initialize SNTP and set timezone.
     * Safe to call before WiFi is up — SNTP will retry once DNS resolves.
     * Idempotent: subsequent calls are no-ops.
     */
    void weight_time_init(void);

    /**
     * @return true once NTP has synced at least once.
     */
    bool weight_time_is_synced(void);

#ifdef __cplusplus
}
#endif
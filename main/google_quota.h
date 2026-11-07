#pragma once

#include "config.h"
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*google_quota_cb_t)(const google_quota_data_t *data);

esp_err_t google_quota_init(google_quota_cb_t callback);
void google_quota_set_active(bool active);
bool google_quota_is_active(void);
void google_quota_request_refresh(void);
const google_quota_data_t* google_quota_get_latest(void);

// Shared mutex to serialize HTTPS/mbedTLS sessions across tasks and prevent heap exhaustion
void google_quota_tls_lock(void);
void google_quota_tls_unlock(void);

#ifdef __cplusplus
}
#endif

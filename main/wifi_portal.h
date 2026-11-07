#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wifi_state_cb_t)(bool connected, const char *ip_str);

esp_err_t wifi_portal_init(wifi_state_cb_t state_cb);
void wifi_portal_start_ap(void);
void wifi_portal_stop_ap(void);
bool wifi_portal_is_ap_active(void);
int wifi_portal_get_remaining_sec(void);
bool wifi_portal_is_connected(void);
const char *wifi_portal_get_ip(void);

#ifdef __cplusplus
}
#endif

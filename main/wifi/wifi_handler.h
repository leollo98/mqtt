#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_handler_init(void);

esp_err_t wifi_handler_connect(void);

esp_err_t wifi_handler_load_credentials(void);

esp_err_t wifi_handler_reset_retry(void);

esp_err_t wifi_handler_set_credentials(
	const char* ssid,
	const char* password);

bool wifi_handler_is_connected(void);

typedef enum {
	WIFI_HANDLER_STATE_NOT_CONFIGURED,
	WIFI_HANDLER_STATE_CONNECTING,
	WIFI_HANDLER_STATE_CONNECTED,
	WIFI_HANDLER_STATE_FAILED
} wifi_handler_state_t;

wifi_handler_state_t wifi_handler_get_state(void);

#ifdef __cplusplus
}

#endif
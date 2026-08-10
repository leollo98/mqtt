#pragma once

#include "esp_err.h"
#include "modbus/pzem.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mqtt_app_start(void);

esp_err_t mqtt_publish_telemetry(const pzem_data_t* data);

#ifdef __cplusplus
}
#endif
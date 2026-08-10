#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
	float voltage;
	float current;
	float power;
	float energy;
	float frequency;
	float power_factor;
} pzem_data_t;

esp_err_t pzem_init(void);

esp_err_t pzem_read_all(pzem_data_t* data);

esp_err_t pzem_set_read_interval(uint32_t interval_ms);

uint32_t pzem_get_read_interval(void);
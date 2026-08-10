#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t button_init(void);

bool button_get_state(void);

bool button_get_event(bool* state);

#ifdef __cplusplus
}
#endif
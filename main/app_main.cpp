#include <inttypes.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "button/button.h"
#include "modbus/pzem.h"
#include "wifi/mqtt.h"
#include "wifi/wifi_handler.h"

static const char* TAG = "APP";

static void pzem_task(void* pvParameters) {
	pzem_data_t data;

	while (true) {
		esp_err_t err = pzem_read_all(&data);

		if (err == ESP_OK) {
			ESP_LOGI(TAG,
					 "PZEM: V=%.1f V | I=%.3f A | P=%.1f W | "
					 "E=%.0f Wh | F=%.1f Hz | FP=%.2f",
					 data.voltage, data.current, data.power, data.energy,
					 data.frequency, data.power_factor);

			esp_err_t mqtt_err = mqtt_publish_telemetry(&data);

			if (mqtt_err != ESP_OK) {
				ESP_LOGW(
					TAG,
					"Falha ao enfileirar telemetria MQTT: %s",
					esp_err_to_name(mqtt_err));
			}

		} else {
			ESP_LOGE(TAG, "Erro ao ler PZEM: %s", esp_err_to_name(err));
		}

		vTaskDelay(pdMS_TO_TICKS(pzem_get_read_interval()));
	}
}

extern "C" void app_main(void) {
	ESP_LOGI(TAG, "[APP] Startup..");
	ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes",
			 esp_get_free_heap_size());
	ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

	esp_log_level_set("*", ESP_LOG_INFO);
	esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
	esp_log_level_set("mqtt_client", ESP_LOG_VERBOSE);
	esp_log_level_set("APP", ESP_LOG_VERBOSE);
	esp_log_level_set("transport_base", ESP_LOG_VERBOSE);
	esp_log_level_set("transport", ESP_LOG_VERBOSE);
	esp_log_level_set("outbox", ESP_LOG_VERBOSE);

	// esp_log_level_set("*", ESP_LOG_INFO); // Para deploy

	ESP_ERROR_CHECK(nvs_flash_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	ESP_ERROR_CHECK(wifi_handler_init());
	ESP_ERROR_CHECK(pzem_init());
	ESP_ERROR_CHECK(mqtt_app_start());
	ESP_ERROR_CHECK(button_init());
	BaseType_t result = xTaskCreate(
		pzem_task,
		"pzem_task",
		4096,
		nullptr,
		5,
		nullptr);

	if (result != pdPASS) {
		ESP_LOGE(TAG, "Falha ao criar PZEM task");
	}
}

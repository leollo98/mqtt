#include "wifi_handler.h"

#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi_provisioning.h"

namespace {

	static const char* TAG = "WIFI";

	static constexpr char NVS_NAMESPACE[] = "wifi";
	static constexpr char NVS_KEY_SSID[] = "ssid";
	static constexpr char NVS_KEY_PASSWORD[] = "password";

	static constexpr size_t WIFI_SSID_MAX_LEN = 32;
	static constexpr size_t WIFI_PASSWORD_MAX_LEN = 64;

	static bool wifi_initialized = false;

	static wifi_handler_state_t wifi_state = WIFI_HANDLER_STATE_NOT_CONFIGURED;

	static uint8_t wifi_retry_count = 0;
	static bool provisioning_start_requested = false;

	static char wifi_ssid[WIFI_SSID_MAX_LEN + 1] = {};
	static char wifi_password[WIFI_PASSWORD_MAX_LEN + 1] = {};

	static esp_err_t enter_provisioning_mode(void) {
		ESP_LOGW(
			TAG,
			"Falha ao conectar ao Wi-Fi após %d tentativas",
			CONFIG_APP_WIFI_MAX_RETRY);

		wifi_state = WIFI_HANDLER_STATE_FAILED;
		wifi_retry_count = 0;

		esp_err_t err = esp_wifi_stop();

		if (err != ESP_OK &&
			err != ESP_ERR_WIFI_NOT_INIT &&
			err != ESP_ERR_INVALID_STATE) {
			ESP_LOGE(
				TAG,
				"Falha ao parar Wi-Fi STA: %s",
				esp_err_to_name(err));

			return err;
		}

		return wifi_provisioning_start();
	}

	static void start_provisioning_task(void* pvParameters) {
		vTaskDelay(pdMS_TO_TICKS(100));

		esp_err_t err = enter_provisioning_mode();

		if (err != ESP_OK) {
			ESP_LOGE(
				TAG,
				"Falha ao iniciar provisioning: %s",
				esp_err_to_name(err));
		}

		provisioning_start_requested = false;

		vTaskDelete(nullptr);
	}

	static void wifi_event_handler(
		void* arg,
		esp_event_base_t event_base,
		int32_t event_id,
		void* event_data) {
		if (event_base == WIFI_EVENT) {
			switch (event_id) {
				case WIFI_EVENT_STA_START:
					ESP_LOGI(
						TAG,
						"Wi-Fi STA iniciado");

					wifi_retry_count = 0;
					wifi_state = WIFI_HANDLER_STATE_CONNECTING;

					break;

				case WIFI_EVENT_STA_DISCONNECTED:
					wifi_state = WIFI_HANDLER_STATE_CONNECTING;

					if (wifi_retry_count < CONFIG_APP_WIFI_MAX_RETRY) {
						wifi_retry_count++;

						ESP_LOGW(
							TAG,
							"Wi-Fi desconectado. "
							"Tentativa %u/%u",
							wifi_retry_count,
							CONFIG_APP_WIFI_MAX_RETRY);

						esp_err_t err = esp_wifi_connect();

						if (err != ESP_OK) {
							ESP_LOGW(
								TAG,
								"Falha ao solicitar reconexão: %s",
								esp_err_to_name(err));
						}

					} else {
						wifi_state = WIFI_HANDLER_STATE_FAILED;

						ESP_LOGE(
							TAG,
							"Limite de tentativas Wi-Fi atingido");

						if (CONFIG_APP_WIFI_PROVISIONING) {
							if (!provisioning_start_requested &&
								!wifi_provisioning_is_active()) {
								provisioning_start_requested = true;

								BaseType_t result = xTaskCreate(
									start_provisioning_task,
									"wifi_provisioning",
									4096,
									nullptr,
									5,
									nullptr);

								if (result != pdPASS) {
									provisioning_start_requested = false;

									ESP_LOGE(
										TAG,
										"Falha ao criar task de provisioning");
								}
							}

						} else {
							ESP_LOGW(
								TAG,
								"Provisionamento Wi-Fi desabilitado");
						}
					}

					break;

				default:
					break;
			}

			return;
		}

		if (event_base == IP_EVENT &&
			event_id == IP_EVENT_STA_GOT_IP) {
			const auto* event = static_cast<const ip_event_got_ip_t*>(event_data);

			wifi_retry_count = 0;

			wifi_state = WIFI_HANDLER_STATE_CONNECTED;

			ESP_LOGI(
				TAG,
				"Wi-Fi conectado. IP: " IPSTR,
				IP2STR(&event->ip_info.ip));
		}
	}

}  // namespace

esp_err_t wifi_handler_set_credentials(
	const char* ssid,
	const char* password) {
	if (ssid == nullptr || password == nullptr) {
		return ESP_ERR_INVALID_ARG;
	}

	if (strlen(ssid) == 0 ||
		strlen(ssid) > WIFI_SSID_MAX_LEN) {
		return ESP_ERR_INVALID_ARG;
	}

	if (strlen(password) > WIFI_PASSWORD_MAX_LEN) {
		return ESP_ERR_INVALID_ARG;
	}

	nvs_handle_t nvs_handle;

	esp_err_t err = nvs_open(
		NVS_NAMESPACE,
		NVS_READWRITE,
		&nvs_handle);

	if (err != ESP_OK) {
		ESP_LOGE(
			TAG,
			"Falha ao abrir NVS: %s",
			esp_err_to_name(err));

		return err;
	}

	err = nvs_set_str(
		nvs_handle,
		NVS_KEY_SSID,
		ssid);

	if (err != ESP_OK) {
		nvs_close(nvs_handle);
		return err;
	}

	err = nvs_set_str(
		nvs_handle,
		NVS_KEY_PASSWORD,
		password);

	if (err != ESP_OK) {
		nvs_close(nvs_handle);
		return err;
	}

	err = nvs_commit(nvs_handle);

	nvs_close(nvs_handle);

	if (err != ESP_OK) {
		ESP_LOGE(
			TAG,
			"Falha ao salvar credenciais: %s",
			esp_err_to_name(err));

		return err;
	}

	strncpy(
		wifi_ssid,
		ssid,
		sizeof(wifi_ssid) - 1);

	strncpy(
		wifi_password,
		password,
		sizeof(wifi_password) - 1);

	ESP_LOGI(TAG, "Credenciais Wi-Fi salvas");

	return ESP_OK;
}

esp_err_t wifi_handler_load_credentials(void) {
	nvs_handle_t nvs_handle;

	esp_err_t err = nvs_open(
		NVS_NAMESPACE,
		NVS_READONLY,
		&nvs_handle);

	if (err == ESP_ERR_NVS_NOT_FOUND) {
		ESP_LOGI(TAG, "Nenhuma configuração Wi-Fi encontrada");
		return ESP_ERR_NOT_FOUND;
	}

	if (err != ESP_OK) {
		return err;
	}

	size_t ssid_len = sizeof(wifi_ssid);
	size_t password_len = sizeof(wifi_password);

	err = nvs_get_str(
		nvs_handle,
		NVS_KEY_SSID,
		wifi_ssid,
		&ssid_len);

	if (err != ESP_OK) {
		nvs_close(nvs_handle);
		return err;
	}

	err = nvs_get_str(
		nvs_handle,
		NVS_KEY_PASSWORD,
		wifi_password,
		&password_len);

	nvs_close(nvs_handle);

	if (err != ESP_OK) {
		return err;
	}

	ESP_LOGI(
		TAG,
		"Credenciais Wi-Fi carregadas para SSID: %s",
		wifi_ssid);

	return ESP_OK;
}

esp_err_t wifi_handler_init(void) {
	if (wifi_initialized) {
		return ESP_OK;
	}

	esp_err_t err = esp_netif_init();

	if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
		ESP_LOGE(
			TAG,
			"Falha ao inicializar esp_netif: %s",
			esp_err_to_name(err));

		return err;
	}

	err = esp_event_handler_register(
		WIFI_EVENT,
		ESP_EVENT_ANY_ID,
		&wifi_event_handler,
		nullptr);

	if (err != ESP_OK) {
		return err;
	}

	err = esp_event_handler_register(
		IP_EVENT,
		IP_EVENT_STA_GOT_IP,
		&wifi_event_handler,
		nullptr);

	if (err != ESP_OK) {
		return err;
	}

	esp_netif_create_default_wifi_sta();

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

	err = esp_wifi_init(&cfg);

	if (err != ESP_OK) {
		ESP_LOGE(
			TAG,
			"Falha ao inicializar Wi-Fi: %s",
			esp_err_to_name(err));

		return err;
	}

	wifi_initialized = true;

	ESP_LOGI(TAG, "Wi-Fi inicializado");

	err = wifi_handler_load_credentials();

	if (err == ESP_OK) {
		ESP_LOGI(
			TAG,
			"Credenciais encontradas");

		return wifi_handler_connect();
	}

	if (err == ESP_ERR_NOT_FOUND) {
		wifi_state = WIFI_HANDLER_STATE_NOT_CONFIGURED;

		ESP_LOGW(
			TAG,
			"Wi-Fi não configurado");

		if (CONFIG_APP_WIFI_PROVISIONING) {
			return wifi_provisioning_start();
		}

		ESP_LOGW(
			TAG,
			"Provisionamento Wi-Fi desabilitado");

		return ESP_OK;
	}

	return err;
}

esp_err_t wifi_handler_connect(void) {
	if (!wifi_initialized) {
		return ESP_ERR_INVALID_STATE;
	}

	if (wifi_ssid[0] == '\0') {
		ESP_LOGE(TAG, "SSID não configurado");
		return ESP_ERR_INVALID_STATE;
	}

	wifi_config_t wifi_config = {};

	strncpy(
		reinterpret_cast<char*>(wifi_config.sta.ssid),
		wifi_ssid,
		sizeof(wifi_config.sta.ssid) - 1);

	strncpy(
		reinterpret_cast<char*>(wifi_config.sta.password),
		wifi_password,
		sizeof(wifi_config.sta.password) - 1);

	esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);

	if (err != ESP_OK) {
		ESP_LOGE(
			TAG,
			"Falha ao configurar modo STA: %s",
			esp_err_to_name(err));

		return err;
	}

	err = esp_wifi_set_config(
		WIFI_IF_STA,
		&wifi_config);

	if (err != ESP_OK) {
		return err;
	}

	err = esp_wifi_start();

	if (err != ESP_OK &&
		err != ESP_ERR_INVALID_STATE) {
		wifi_state = WIFI_HANDLER_STATE_FAILED;

		return err;
	}

	wifi_retry_count = 0;

	wifi_state = WIFI_HANDLER_STATE_CONNECTING;

	err = esp_wifi_connect();

	if (err != ESP_OK &&
		err != ESP_ERR_INVALID_STATE) {
		wifi_state = WIFI_HANDLER_STATE_FAILED;

		return err;
	}

	ESP_LOGI(
		TAG,
		"Conexão Wi-Fi solicitada");

	return ESP_OK;
}

bool wifi_handler_is_connected(void) {
	return wifi_state == WIFI_HANDLER_STATE_CONNECTED;
}

wifi_handler_state_t wifi_handler_get_state(void) {
	return wifi_state;
}

esp_err_t wifi_handler_reset_retry(void) {
	wifi_retry_count = 0;
	return ESP_OK;
}
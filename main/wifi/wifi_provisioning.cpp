#include "wifi_provisioning.h"

#include <cstring>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi_handler.h"

namespace {

	static const char* TAG = "WIFI_PROV";

	static constexpr char AP_SSID[] = "PZEM-Setup";
	static constexpr char AP_PASSWORD[] = "12345678";

	static constexpr uint8_t AP_CHANNEL = 1;
	static constexpr uint8_t AP_MAX_CONNECTIONS = 4;

	static constexpr uint32_t SWITCH_TO_STA_DELAY_MS = 500;

	static httpd_handle_t http_server = nullptr;
	static esp_netif_t* ap_netif = nullptr;
	static bool provisioning_active = false;
	static bool provisioning_transitioning = false;

	static void switch_to_station_task(void* pvParameters) {
		// Dá tempo para o navegador receber a resposta HTTP.
		vTaskDelay(pdMS_TO_TICKS(SWITCH_TO_STA_DELAY_MS));

		ESP_LOGI(
			TAG,
			"Encerrando provisioning e iniciando conexão Wi-Fi");

		esp_err_t err = wifi_provisioning_stop();

		if (err != ESP_OK) {
			ESP_LOGE(
				TAG,
				"Falha ao encerrar provisioning: %s",
				esp_err_to_name(err));

			vTaskDelete(nullptr);
			return;
		}

		err = wifi_handler_connect();

		if (err != ESP_OK) {
			ESP_LOGE(
				TAG,
				"Falha ao conectar ao Wi-Fi: %s",
				esp_err_to_name(err));

			vTaskDelete(nullptr);
			return;
		}

		ESP_LOGI(
			TAG,
			"Conexão Wi-Fi iniciada após provisioning");

		vTaskDelete(nullptr);
	}

	static esp_err_t index_handler(
		httpd_req_t* req) {
		static constexpr char html[] =
			"<!DOCTYPE html>"
			"<html>"
			"<head>"
			"<meta charset=\"UTF-8\">"
			"<meta name=\"viewport\" "
			"content=\"width=device-width,initial-scale=1\">"
			"<title>PZEM Setup</title>"
			"</head>"
			"<body>"
			"<h1>Configuração Wi-Fi</h1>"
			"<form method=\"POST\" action=\"/configure\">"
			"<label>SSID</label><br>"
			"<input name=\"ssid\" "
			"type=\"text\" "
			"maxlength=\"32\" "
			"required><br><br>"
			"<label>Senha</label><br>"
			"<input name=\"password\" "
			"type=\"password\" "
			"maxlength=\"64\"><br><br>"
			"<button type=\"submit\">Conectar</button>"
			"</form>"
			"</body>"
			"</html>";

		httpd_resp_set_type(
			req,
			"text/html; charset=UTF-8");

		return httpd_resp_send(
			req,
			html,
			HTTPD_RESP_USE_STRLEN);
	}

	static bool url_decode(
		const char* src,
		char* dst,
		size_t dst_size) {
		if (src == nullptr ||
			dst == nullptr ||
			dst_size == 0) {
			return false;
		}

		size_t dst_index = 0;

		while (*src != '\0') {
			if (dst_index >= dst_size - 1) {
				return false;
			}

			if (*src == '+') {
				dst[dst_index++] = ' ';
				src++;
				continue;
			}

			if (*src == '%') {
				if (src[1] == '\0' ||
					src[2] == '\0') {
					return false;
				}

				auto hex_to_value = [](char c) -> int {
					if (c >= '0' && c <= '9') {
						return c - '0';
					}

					if (c >= 'a' && c <= 'f') {
						return c - 'a' + 10;
					}

					if (c >= 'A' && c <= 'F') {
						return c - 'A' + 10;
					}

					return -1;
				};

				int high = hex_to_value(src[1]);
				int low = hex_to_value(src[2]);

				if (high < 0 || low < 0) {
					return false;
				}

				dst[dst_index++] = static_cast<char>((high << 4) | low);

				src += 3;
				continue;
			}

			dst[dst_index++] = *src;
			src++;
		}

		dst[dst_index] = '\0';

		return true;
	}

	static bool get_form_value(
		const char* body,
		const char* key,
		char* output,
		size_t output_size) {
		if (body == nullptr ||
			key == nullptr ||
			output == nullptr ||
			output_size == 0) {
			return false;
		}

		const size_t key_len = std::strlen(key);

		const char* current = body;

		while (*current != '\0') {
			if ((current == body ||
				 *(current - 1) == '&') &&
				std::strncmp(current, key, key_len) == 0 &&
				current[key_len] == '=') {
				const char* value_start = current + key_len + 1;

				const char* value_end = std::strchr(value_start, '&');

				size_t value_length;

				if (value_end != nullptr) {
					value_length = static_cast<size_t>(
						value_end - value_start);
				} else {
					value_length = std::strlen(value_start);
				}

				if (value_length >= output_size) {
					return false;
				}

				char encoded[128];

				if (value_length >= sizeof(encoded)) {
					return false;
				}

				std::memcpy(
					encoded,
					value_start,
					value_length);

				encoded[value_length] = '\0';

				return url_decode(
					encoded,
					output,
					output_size);
			}

			const char* next = std::strchr(current, '&');

			if (next == nullptr) {
				break;
			}

			current = next + 1;
		}

		return false;
	}

	static esp_err_t configure_handler(
		httpd_req_t* req) {
		if (provisioning_transitioning) {
			httpd_resp_send_err(
				req,
				HTTPD_500_INTERNAL_SERVER_ERROR,
				"Configuracao ja esta sendo processada");

			return ESP_OK;
		}

		ESP_LOGI(
			TAG,
			"Recebendo configuração Wi-Fi");

		if (req->content_len <= 0) {
			httpd_resp_send_err(
				req,
				HTTPD_400_BAD_REQUEST,
				"Dados de configuracao ausentes");

			return ESP_OK;
		}

		if (req->content_len >= 512) {
			httpd_resp_send_err(
				req,
				HTTPD_400_BAD_REQUEST,
				"Dados de configuracao muito grandes");

			return ESP_OK;
		}

		char body[512] = {};

		size_t received = 0;

		while (received < req->content_len) {
			int ret = httpd_req_recv(
				req,
				body + received,
				req->content_len - received);

			if (ret <= 0) {
				if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
					continue;
				}

				ESP_LOGE(
					TAG,
					"Falha ao receber dados HTTP");

				httpd_resp_send_err(
					req,
					HTTPD_500_INTERNAL_SERVER_ERROR,
					"Falha ao receber configuracao");

				return ESP_OK;
			}

			received += static_cast<size_t>(ret);
		}

		body[received] = '\0';

		char ssid[33] = {};
		char password[65] = {};

		if (!get_form_value(
				body,
				"ssid",
				ssid,
				sizeof(ssid))) {
			httpd_resp_send_err(
				req,
				HTTPD_400_BAD_REQUEST,
				"SSID invalido");

			return ESP_OK;
		}

		if (!get_form_value(
				body,
				"password",
				password,
				sizeof(password))) {
			httpd_resp_send_err(
				req,
				HTTPD_400_BAD_REQUEST,
				"Senha invalida");

			return ESP_OK;
		}

		ESP_LOGI(
			TAG,
			"SSID recebido: %s",
			ssid);

		esp_err_t err = wifi_handler_set_credentials(
			ssid,
			password);

		if (err != ESP_OK) {
			ESP_LOGE(
				TAG,
				"Falha ao salvar credenciais: %s",
				esp_err_to_name(err));

			httpd_resp_send_err(
				req,
				HTTPD_500_INTERNAL_SERVER_ERROR,
				"Falha ao salvar configuracao");

			return ESP_OK;
		}

		provisioning_transitioning = true;

		static constexpr char response[] =
			"<!DOCTYPE html>"
			"<html>"
			"<head>"
			"<meta charset=\"UTF-8\">"
			"<meta name=\"viewport\" "
			"content=\"width=device-width,initial-scale=1\">"
			"<title>Wi-Fi configurado</title>"
			"</head>"
			"<body>"
			"<h1>Wi-Fi configurado!</h1>"
			"<p>O dispositivo esta tentando conectar.</p>"
			"</body>"
			"</html>";

		httpd_resp_set_type(
			req,
			"text/html; charset=UTF-8");

		esp_err_t response_err = httpd_resp_send(
			req,
			response,
			HTTPD_RESP_USE_STRLEN);

		if (response_err != ESP_OK) {
			ESP_LOGW(
				TAG,
				"Falha ao enviar resposta HTTP: %s",
				esp_err_to_name(response_err));

			return response_err;
		}

		BaseType_t task_result = xTaskCreate(
			switch_to_station_task,
			"wifi_to_sta",
			4096,
			nullptr,
			5,
			nullptr);

		if (task_result != pdPASS) {
			ESP_LOGE(
				TAG,
				"Falha ao criar task de transicao Wi-Fi");

			provisioning_transitioning = false;

			return ESP_FAIL;
		}

		return ESP_OK;
	}

	static esp_err_t start_http_server(void) {
		httpd_config_t config = HTTPD_DEFAULT_CONFIG();

		config.server_port = 80;

		esp_err_t err = httpd_start(
			&http_server,
			&config);

		if (err != ESP_OK) {
			ESP_LOGE(
				TAG,
				"Falha ao iniciar HTTP server: %s",
				esp_err_to_name(err));

			return err;
		}

		httpd_uri_t index_uri = {
			.uri = "/",
			.method = HTTP_GET,
			.handler = index_handler,
			.user_ctx = nullptr,
		};

		err = httpd_register_uri_handler(
			http_server,
			&index_uri);

		if (err != ESP_OK) {
			httpd_stop(http_server);
			http_server = nullptr;

			return err;
		}

		httpd_uri_t configure_uri = {
			.uri = "/configure",
			.method = HTTP_POST,
			.handler = configure_handler,
			.user_ctx = nullptr,
		};

		err = httpd_register_uri_handler(
			http_server,
			&configure_uri);

		if (err != ESP_OK) {
			httpd_stop(http_server);
			http_server = nullptr;

			return err;
		}

		ESP_LOGI(
			TAG,
			"HTTP server iniciado");

		return ESP_OK;
	}

}  // namespace

esp_err_t wifi_provisioning_start(void) {
	if (provisioning_active) {
		return ESP_OK;
	}

	ESP_LOGI(
		TAG,
		"Iniciando modo de provisionamento");

	if (ap_netif == nullptr) {
		ap_netif = esp_netif_create_default_wifi_ap();
	}

	if (ap_netif == nullptr) {
		ESP_LOGE(
			TAG,
			"Falha ao criar interface AP");

		return ESP_FAIL;
	}

	wifi_config_t ap_config = {};

	std::strncpy(
		reinterpret_cast<char*>(ap_config.ap.ssid),
		AP_SSID,
		sizeof(ap_config.ap.ssid) - 1);

	std::strncpy(
		reinterpret_cast<char*>(ap_config.ap.password),
		AP_PASSWORD,
		sizeof(ap_config.ap.password) - 1);

	ap_config.ap.ssid_len = static_cast<uint8_t>(std::strlen(AP_SSID));

	ap_config.ap.channel = AP_CHANNEL;

	ap_config.ap.max_connection = AP_MAX_CONNECTIONS;

	ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

	esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);

	if (err != ESP_OK) {
		esp_netif_destroy_default_wifi(ap_netif);
		ap_netif = nullptr;
		return err;
	}

	err = esp_wifi_set_config(
		WIFI_IF_AP,
		&ap_config);

	if (err != ESP_OK) {
		esp_netif_destroy_default_wifi(ap_netif);
		ap_netif = nullptr;
		return err;
	}

	err = esp_wifi_start();

	if (err != ESP_OK &&
		err != ESP_ERR_INVALID_STATE) {
		ESP_LOGE(
			TAG,
			"Falha ao iniciar AP: %s",
			esp_err_to_name(err));

		esp_netif_destroy_default_wifi(ap_netif);
		ap_netif = nullptr;

		return err;
	}

	err = start_http_server();

	if (err != ESP_OK) {
		esp_wifi_stop();

		esp_netif_destroy_default_wifi(ap_netif);
		ap_netif = nullptr;

		return err;
	}

	provisioning_active = true;

	ESP_LOGI(
		TAG,
		"AP iniciado: %s",
		AP_SSID);

	ESP_LOGI(
		TAG,
		"Acesse http://192.168.4.1");

	return ESP_OK;
}

esp_err_t wifi_provisioning_stop(void) {
	if (!provisioning_active) {
		return ESP_OK;
	}

	ESP_LOGI(
		TAG,
		"Encerrando modo de provisionamento");

	if (http_server != nullptr) {
		esp_err_t err = httpd_stop(http_server);

		http_server = nullptr;

		if (err != ESP_OK) {
			ESP_LOGE(
				TAG,
				"Falha ao parar HTTP server: %s",
				esp_err_to_name(err));

			return err;
		}
	}

	esp_err_t err = esp_wifi_stop();

	if (err != ESP_OK &&
		err != ESP_ERR_WIFI_NOT_INIT &&
		err != ESP_ERR_INVALID_STATE) {
		ESP_LOGE(
			TAG,
			"Falha ao parar Wi-Fi: %s",
			esp_err_to_name(err));

		return err;
	}

	if (ap_netif != nullptr) {
		esp_netif_destroy_default_wifi(ap_netif);
		ap_netif = nullptr;
	}

	provisioning_active = false;
	provisioning_transitioning = false;

	ESP_LOGI(
		TAG,
		"Modo de provisionamento encerrado");

	return ESP_OK;
}

bool wifi_provisioning_is_active(void) {
	return provisioning_active;
}
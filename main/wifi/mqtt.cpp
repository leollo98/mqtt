#include "mqtt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "button/button.h"

#if CONFIG_CERT_VALIDATE_OVERRIDE
static const char cert_override_pem[] =
	"-----BEGIN CERTIFICATE-----\n" CONFIG_BROKER_CERTIFICATE_OVERRIDE
	"\n"
	"-----END CERTIFICATE-----";
#endif

#if CONFIG_CERT_VALIDATE_CA
extern const uint8_t broker_emqx_io_ca_crt_start[] asm("_binary_broker_emqx_io_ca_crt_start");
extern const uint8_t broker_emqx_io_ca_crt_end[] asm("_binary_broker_emqx_io_ca_crt_end");
#endif

namespace {

	static const char* TAG = "MQTT";

	static volatile bool mqtt_connected = false;

	constexpr size_t MQTT_QUEUE_LENGTH = CONFIG_APP_MQTT_QUEUE_SIZE;

	static TickType_t mqtt_next_retry_tick = 0;

	constexpr TickType_t MQTT_RETRY_DELAY = pdMS_TO_TICKS(1000);

	static esp_mqtt_client_handle_t mqtt_client = nullptr;

	static TaskHandle_t mqtt_task_handle = nullptr;

	static QueueHandle_t telemetry_queue = nullptr;

	static bool pending_button_valid = false;
	static bool pending_button_state = false;

	static TickType_t mqtt_next_health_tick = 0;

	constexpr TickType_t MQTT_HEALTH_INTERVAL =
		pdMS_TO_TICKS(CONFIG_APP_MQTT_HEALTH_INTERVAL * 1000);

	enum class mqtt_delivery_state_t {
		IDLE,
		PUBLISHING
	};

	static mqtt_delivery_state_t mqtt_delivery_state = mqtt_delivery_state_t::IDLE;

	static bool pending_telemetry_valid = false;

	static int pending_msg_id = -1;

	struct mqtt_message_t {
		pzem_data_t pzem;
		bool button_state;
	};

	static mqtt_message_t pending_telemetry{};

	static void mqtt_process_button_events(void) {
		bool state;

		while (button_get_event(&state)) {
			pending_button_state = state;
			pending_button_valid = true;
		}
	}
	static bool mqtt_json_get_uint32(
		const char* json,
		size_t json_len,
		const char* key,
		uint32_t* value) {
		if (json == nullptr ||
			key == nullptr ||
			value == nullptr) {
			return false;
		}

		char pattern[64];

		int pattern_len = snprintf(
			pattern,
			sizeof(pattern),
			"\"%s\"",
			key);

		if (pattern_len <= 0 ||
			static_cast<size_t>(pattern_len) >= sizeof(pattern)) {
			return false;
		}

		const char* key_start = nullptr;

		for (size_t i = 0;
			 i + static_cast<size_t>(pattern_len) <= json_len;
			 ++i) {
			if (memcmp(
					&json[i],
					pattern,
					pattern_len) == 0) {
				key_start = &json[i];
				break;
			}
		}

		if (key_start == nullptr) {
			return false;
		}

		const char* value_start =
			key_start + pattern_len;

		const char* json_end =
			json + json_len;

		while (value_start < json_end &&
			   (*value_start == ' ' ||
				*value_start == '\t' ||
				*value_start == '\r' ||
				*value_start == '\n' ||
				*value_start == ':')) {
			++value_start;
		}

		if (value_start >= json_end) {
			return false;
		}

		uint64_t number = 0;
		bool has_digit = false;

		while (value_start < json_end &&
			   *value_start >= '0' &&
			   *value_start <= '9') {
			has_digit = true;

			number =
				number * 10 +
				static_cast<uint64_t>(
					*value_start - '0');

			if (number > UINT32_MAX) {
				return false;
			}

			++value_start;
		}

		if (!has_digit) {
			return false;
		}

		*value = static_cast<uint32_t>(number);

		return true;
	}
	static bool mqtt_json_get_string(
		const char* json,
		size_t json_len,
		const char* key,
		char* output,
		size_t output_size) {
		if (json == nullptr ||
			key == nullptr ||
			output == nullptr ||
			output_size == 0) {
			return false;
		}

		char pattern[64];

		int pattern_len = snprintf(
			pattern,
			sizeof(pattern),
			"\"%s\"",
			key);

		if (pattern_len <= 0 ||
			static_cast<size_t>(pattern_len) >= sizeof(pattern)) {
			return false;
		}

		const char* key_start = nullptr;

		for (size_t i = 0;
			 i + static_cast<size_t>(pattern_len) <= json_len;
			 ++i) {
			if (memcmp(
					&json[i],
					pattern,
					pattern_len) == 0) {
				key_start = &json[i];
				break;
			}
		}

		if (key_start == nullptr) {
			return false;
		}

		const char* value_start =
			key_start + pattern_len;

		const char* json_end =
			json + json_len;

		while (value_start < json_end &&
			   (*value_start == ' ' ||
				*value_start == '\t' ||
				*value_start == '\r' ||
				*value_start == '\n' ||
				*value_start == ':')) {
			++value_start;
		}

		if (value_start >= json_end ||
			*value_start != '"') {
			return false;
		}

		++value_start;

		const char* value_end = value_start;

		while (value_end < json_end &&
			   *value_end != '"') {
			++value_end;
		}

		if (value_end >= json_end) {
			return false;
		}

		size_t value_len =
			static_cast<size_t>(
				value_end - value_start);

		if (value_len >= output_size) {
			return false;
		}

		memcpy(
			output,
			value_start,
			value_len);

		output[value_len] = '\0';

		return true;
	}

	static void mqtt_process_command(
		const char* data,
		size_t data_len) {
		if (data == nullptr ||
			data_len == 0) {
			return;
		}

		char command[32];
		uint32_t interval_s = 0;

		if (!mqtt_json_get_string(
				data,
				data_len,
				"command",
				command,
				sizeof(command))) {
			ESP_LOGW(
				TAG,
				"Comando MQTT sem campo 'command' valido");

			return;
		}

		if (!mqtt_json_get_uint32(
				data,
				data_len,
				"interval_s",
				&interval_s)) {
			ESP_LOGW(
				TAG,
				"Comando MQTT sem campo 'interval_s' valido");

			return;
		}

		if (strcmp(
				command,
				"set_interval") != 0) {
			ESP_LOGW(
				TAG,
				"Comando MQTT desconhecido: %s",
				command);

			return;
		}

		if (interval_s == 0 ||
			interval_s > UINT32_MAX / 1000U) {
			ESP_LOGW(
				TAG,
				"Intervalo invalido: %lu s",
				static_cast<unsigned long>(
					interval_s));

			return;
		}

		const uint32_t interval_ms =
			interval_s * 1000U;

		esp_err_t err =
			pzem_set_read_interval(interval_ms);

		if (err == ESP_OK) {
			ESP_LOGI(
				TAG,
				"Comando executado: intervalo=%lu s",
				static_cast<unsigned long>(
					interval_s));
		} else {
			ESP_LOGW(
				TAG,
				"Falha ao alterar intervalo: %s",
				esp_err_to_name(err));
		}
	}

	static esp_err_t mqtt_publish_health(void) {
		if (mqtt_client == nullptr) {
			return ESP_ERR_INVALID_STATE;
		}

		if (!mqtt_connected) {
			return ESP_ERR_INVALID_STATE;
		}

		const uint32_t uptime_s =
			static_cast<uint32_t>(esp_timer_get_time() / 1000000ULL);

		const size_t free_heap =
			esp_get_free_heap_size();

		const size_t min_free_heap =
			esp_get_minimum_free_heap_size();

		const UBaseType_t telemetry_pending =
			uxQueueMessagesWaiting(telemetry_queue);

		const bool button_state =
			button_get_state();

		char payload[256];

		snprintf(
			payload,
			sizeof(payload),
			"{"
			"\"type\":\"health\","
			"\"uptime_s\":%lu,"
			"\"free_heap\":%u,"
			"\"min_free_heap\":%u,"
			"\"mqtt_connected\":%s,"
			"\"telemetry_pending\":%u,"
			"\"button_state\":%s"
			"}",
			static_cast<unsigned long>(uptime_s),
			static_cast<unsigned>(free_heap),
			static_cast<unsigned>(min_free_heap),
			mqtt_connected ? "true" : "false",
			static_cast<unsigned>(telemetry_pending),
			button_state ? "true" : "false");

		const int msg_id =
			esp_mqtt_client_publish(
				mqtt_client,
				CONFIG_MQTT_HEALTH_ENDPOINT,
				payload,
				0,
				1,
				0);

		if (msg_id < 0) {
			ESP_LOGW(
				TAG,
				"Falha ao publicar health");

			return ESP_FAIL;
		}

		ESP_LOGI(
			TAG,
			"Health enviado: msg_id=%d payload=%s",
			msg_id,
			payload);

		return ESP_OK;
	}

	static bool mqtt_load_next_telemetry(void) {
		if (mqtt_delivery_state != mqtt_delivery_state_t::IDLE) {
			return false;
		}

		if (pending_telemetry_valid) {
			ESP_LOGD(
				TAG,
				"Telemetria pendente encontrada. "
				"Preparando reenvio.");

			return true;
		}

		if (xQueueReceive(
				telemetry_queue,
				&pending_telemetry,
				0) != pdTRUE) {
			return false;
		}

		pending_telemetry_valid = true;

		ESP_LOGD(
			TAG,
			"Nova telemetria carregada da fila. "
			"Mensagem marcada como pendente.");

		return true;
	}

	static esp_err_t mqtt_publish(
		const pzem_data_t* pzem,
		bool button_state,
		int* msg_id_out) {
		if (pzem == nullptr) {
			return ESP_ERR_INVALID_ARG;
		}

		if (mqtt_client == nullptr) {
			return ESP_ERR_INVALID_STATE;
		}

		if (!mqtt_connected) {
			return ESP_ERR_INVALID_STATE;
		}

		char payload[256];

		snprintf(
			payload,
			sizeof(payload),
			"{"
			"\"type\":\"telemetry\","
			"\"voltage\":%.1f,"
			"\"current\":%.3f,"
			"\"power\":%.1f,"
			"\"energy\":%.0f,"
			"\"frequency\":%.1f,"
			"\"power_factor\":%.2f,"
			"\"button_state\":%s"
			"}",
			pzem->voltage,
			pzem->current,
			pzem->power,
			pzem->energy,
			pzem->frequency,
			pzem->power_factor,
			button_state ? "true" : "false");

		int msg_id = esp_mqtt_client_publish(
			mqtt_client,
			CONFIG_MQTT_TELEMETRY_ENDPOINT,
			payload,
			0,
			1,
			0);

		if (msg_id < 0) {
			ESP_LOGW(
				TAG,
				"Falha ao publicar telemetria MQTT");

			return ESP_FAIL;
		}

		if (msg_id_out != nullptr) {
			*msg_id_out = msg_id;
		}

		ESP_LOGI(
			TAG,
			"Telemetria publicada: msg_id=%d",
			msg_id);

		return ESP_OK;
	}

	static void mqtt_process_delivery(void) {
		if (!mqtt_connected) {
			return;
		}

		TickType_t now = xTaskGetTickCount();

		if (now < mqtt_next_retry_tick) {
			return;
		}

		switch (mqtt_delivery_state) {
			case mqtt_delivery_state_t::IDLE: {
				if (!mqtt_load_next_telemetry()) {
					return;
				}

				int msg_id = -1;

				esp_err_t err = mqtt_publish(
					&pending_telemetry.pzem,
					pending_telemetry.button_state,
					&msg_id);

				if (err != ESP_OK) {
					mqtt_next_retry_tick =
						xTaskGetTickCount() + MQTT_RETRY_DELAY;

					ESP_LOGW(
						TAG,
						"Falha ao publicar telemetria. "
						"Mensagem permanece pendente.");

					return;
				}

				pending_msg_id = msg_id;

				mqtt_delivery_state = mqtt_delivery_state_t::PUBLISHING;

				ESP_LOGD(
					TAG,
					"Estado MQTT: IDLE -> PUBLISHING "
					"(msg_id=%d)",
					pending_msg_id);

				break;
			}

			case mqtt_delivery_state_t::PUBLISHING:

				break;
		}
	}

	static esp_err_t mqtt_publish_button(bool state) {
		if (mqtt_client == nullptr) {
			return ESP_ERR_INVALID_STATE;
		}

		if (!mqtt_connected) {
			return ESP_ERR_INVALID_STATE;
		}

		char payload[128];

		snprintf(
			payload,
			sizeof(payload),
			"{"
			"\"type\":\"button\","
			"\"button_state\":%s"
			"}",
			state ? "true" : "false");

		int msg_id = esp_mqtt_client_publish(
			mqtt_client,
			CONFIG_MQTT_BUTTON_ENDPOINT,
			payload,
			0,
			1,
			0);

		if (msg_id < 0) {
			ESP_LOGW(
				TAG,
				"Falha ao publicar estado do botao");

			return ESP_FAIL;
		}

		ESP_LOGI(
			TAG,
			"Estado do botao enviado: state=%d msg_id=%d",
			state,
			msg_id);

		return ESP_OK;
	}

	static void mqtt_task(void* pvParameters) {
		while (true) {
			mqtt_process_button_events();

			if (!mqtt_connected) {
				vTaskDelay(pdMS_TO_TICKS(100));
				continue;
			}

			if (pending_button_valid) {
				if (mqtt_publish_button(
						pending_button_state) == ESP_OK) {
					pending_button_valid = false;
				}

				continue;
			}

			const TickType_t now = xTaskGetTickCount();

			if (now >= mqtt_next_health_tick) {
				mqtt_publish_health();

				mqtt_next_health_tick = now + MQTT_HEALTH_INTERVAL;
			}

			mqtt_process_delivery();

			vTaskDelay(pdMS_TO_TICKS(10));
		}
	}

	static void mqtt_event_handler(
		void* handler_args,
		esp_event_base_t base,
		int32_t event_id,
		void* event_data) {
		esp_mqtt_event_handle_t event = static_cast<esp_mqtt_event_handle_t>(event_data);

		switch (static_cast<esp_mqtt_event_id_t>(event_id)) {
			case MQTT_EVENT_CONNECTED: {
				mqtt_connected = true;

				ESP_LOGI(
					TAG,
					"MQTT conectado. Mensagens pendentes: %u",
					static_cast<unsigned>(
						uxQueueMessagesWaiting(telemetry_queue)));

				int msg_id =
					esp_mqtt_client_subscribe(
						mqtt_client,
						CONFIG_MQTT_COMMAND_ENDPOINT,
						1);

				if (msg_id < 0) {
					ESP_LOGE(
						TAG,
						"Falha ao assinar comando MQTT");
				} else {
					ESP_LOGI(
						TAG,
						"Comando MQTT assinado: msg_id=%d",
						msg_id);
				}

				break;
			}

			case MQTT_EVENT_DISCONNECTED:

				mqtt_connected = false;

				pending_msg_id = -1;

				mqtt_delivery_state = mqtt_delivery_state_t::IDLE;

				ESP_LOGW(
					TAG,
					"MQTT desconectado. "
					"Mensagens pendentes permanecem em RAM.");

				break;

			case MQTT_EVENT_PUBLISHED:

				ESP_LOGD(
					TAG,
					"Mensagem publicada, msg_id=%d",
					event->msg_id);

				if (mqtt_delivery_state == mqtt_delivery_state_t::PUBLISHING &&
					event->msg_id == pending_msg_id) {
					pending_msg_id = -1;

					pending_telemetry_valid = false;

					mqtt_delivery_state = mqtt_delivery_state_t::IDLE;

					ESP_LOGD(
						TAG,
						"Entrega confirmada. "
						"Estado MQTT: PUBLISHING -> IDLE");
				}

				break;

			case MQTT_EVENT_ERROR:

				ESP_LOGE(TAG, "Erro MQTT");

				break;

			case MQTT_EVENT_DATA:

				if (event->topic != nullptr &&
					event->topic_len > 0) {
					const size_t topic_len = static_cast<size_t>(event->topic_len);

					const char* command_topic = CONFIG_MQTT_COMMAND_ENDPOINT;

					const size_t command_topic_len = strlen(command_topic);

					if (topic_len == command_topic_len &&
						strncmp(
							event->topic,
							command_topic,
							topic_len) == 0) {
						mqtt_process_command(
							event->data,
							event->data_len);
					}
				}

				break;

			default:
				break;
		}
	}

}  // namespace

esp_err_t mqtt_app_start(void) {
	telemetry_queue = xQueueCreate(
		MQTT_QUEUE_LENGTH,
		sizeof(mqtt_message_t));

	if (telemetry_queue == nullptr) {
		ESP_LOGE(TAG, "Falha ao criar fila MQTT");
		return ESP_ERR_NO_MEM;
	}

	esp_mqtt_client_config_t mqtt_cfg = {};

	mqtt_cfg.broker.address.uri = CONFIG_MQTT_BROKER_URI;

	mqtt_cfg.network.disable_auto_reconnect = false;
	mqtt_cfg.network.reconnect_timeout_ms = 5000;

#if CONFIG_CERT_VALIDATE_OVERRIDE

	mqtt_cfg.broker.verification.certificate = cert_override_pem;

#elif CONFIG_CERT_VALIDATE_CA

	mqtt_cfg.broker.verification.certificate = reinterpret_cast<const char*>(broker_emqx_io_ca_crt_start);

#else

	mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;

#endif

	mqtt_client = esp_mqtt_client_init(&mqtt_cfg);

	if (mqtt_client == nullptr) {
		ESP_LOGE(TAG, "Falha ao inicializar cliente MQTT");
		return ESP_FAIL;
	}

	ESP_ERROR_CHECK(
		esp_mqtt_client_register_event(
			mqtt_client,
			static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID),
			mqtt_event_handler,
			nullptr));

	BaseType_t result = xTaskCreate(
		mqtt_task,
		"mqtt_task",
		4096,
		nullptr,
		5,
		&mqtt_task_handle);

	if (result != pdPASS) {
		ESP_LOGE(TAG, "Falha ao criar MQTT task");
		return ESP_FAIL;
	}

	ESP_ERROR_CHECK(
		esp_mqtt_client_start(mqtt_client));

	ESP_LOGI(TAG, "MQTT iniciado");

	return ESP_OK;
}

esp_err_t mqtt_publish_telemetry(
	const pzem_data_t* data) {
	if (data == nullptr) {
		return ESP_ERR_INVALID_ARG;
	}

	if (telemetry_queue == nullptr) {
		return ESP_ERR_INVALID_STATE;
	}

	mqtt_message_t message;

	message.pzem = *data;
	message.button_state = button_get_state();

	if (xQueueSend(
			telemetry_queue,
			&message,
			0) != pdTRUE) {
		ESP_LOGW(
			TAG,
			"Fila MQTT cheia: telemetria mais antiga será descartada");

		mqtt_message_t discarded;

		if (xQueueReceive(
				telemetry_queue,
				&discarded,
				0) == pdTRUE) {
			if (xQueueSend(
					telemetry_queue,
					&message,
					0) != pdTRUE) {
				ESP_LOGE(
					TAG,
					"Falha ao inserir nova telemetria na fila MQTT");

				return ESP_ERR_NO_MEM;
			}
		} else {
			ESP_LOGE(
				TAG,
				"Fila MQTT cheia e não foi possível liberar espaço");

			return ESP_ERR_TIMEOUT;
		}
	}

	return ESP_OK;
}

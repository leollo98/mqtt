#include "button.h"

#include "driver/gpio.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

	static const char* TAG = "BUTTON";

	static constexpr uint32_t BUTTON_EVENT_QUEUE_LENGTH = 10;

	struct button_event_t {
		bool state;
	};

	static bool button_initialized = false;

	static bool button_state = false;

	static QueueHandle_t button_event_queue = nullptr;

	static TaskHandle_t button_debounce_task_handle = nullptr;

	static portMUX_TYPE button_spinlock = portMUX_INITIALIZER_UNLOCKED;

	static bool read_button_state(void) {
		const int level = gpio_get_level(
			static_cast<gpio_num_t>(
				CONFIG_APP_BUTTON_GPIO));

#if CONFIG_APP_BUTTON_ACTIVE_LOW
		return level == 0;
#else
		return level != 0;
#endif
	}

	static void IRAM_ATTR button_isr_handler(
		void* arg) {
		BaseType_t higher_priority_task_woken = pdFALSE;

		if (button_debounce_task_handle != nullptr) {
			vTaskNotifyGiveFromISR(
				button_debounce_task_handle,
				&higher_priority_task_woken);
		}

		if (higher_priority_task_woken == pdTRUE) {
			portYIELD_FROM_ISR();
		}
	}

	static void button_debounce_task(void* pvParameters) {
		while (true) {
			ulTaskNotifyTake(
				pdTRUE,
				portMAX_DELAY);

#if CONFIG_APP_BUTTON_DEBOUNCE

			vTaskDelay(
				pdMS_TO_TICKS(
					CONFIG_APP_BUTTON_DEBOUNCE_MS));

			while (
				ulTaskNotifyTake(
					pdTRUE,
					0) > 0) {
			}

#endif

			const bool new_state = read_button_state();

			bool state_changed = false;

			portENTER_CRITICAL(
				&button_spinlock);

			if (new_state != button_state) {
				button_state = new_state;
				state_changed = true;
			}

			portEXIT_CRITICAL(
				&button_spinlock);

			if (state_changed) {
				button_event_t event = {
					.state = new_state};

				if (xQueueSend(
						button_event_queue,
						&event,
						0) != pdTRUE) {
					ESP_LOGW(
						TAG,
						"Fila de eventos do botao cheia");
				} else {
					ESP_LOGI(
						TAG,
						"Estado do botao alterado: %d",
						new_state);
				}
			}
		}
	}

}  // namespace

esp_err_t button_init(void) {
	if (button_initialized) {
		return ESP_OK;
	}

	button_event_queue = xQueueCreate(
		BUTTON_EVENT_QUEUE_LENGTH,
		sizeof(button_event_t));

	if (button_event_queue == nullptr) {
		ESP_LOGE(
			TAG,
			"Falha ao criar fila de eventos do botao");

		return ESP_ERR_NO_MEM;
	}

	gpio_config_t config = {};

	config.pin_bit_mask = (1ULL << CONFIG_APP_BUTTON_GPIO);

	config.mode = GPIO_MODE_INPUT;

#if CONFIG_APP_BUTTON_ACTIVE_LOW

	config.pull_up_en = GPIO_PULLUP_ENABLE;

	config.pull_down_en = GPIO_PULLDOWN_DISABLE;

#else

	config.pull_up_en = GPIO_PULLUP_DISABLE;

	config.pull_down_en = GPIO_PULLDOWN_ENABLE;

#endif

	config.intr_type = GPIO_INTR_ANYEDGE;

	esp_err_t err = gpio_config(&config);

	if (err != ESP_OK) {
		ESP_LOGE(
			TAG,
			"Falha ao configurar GPIO do botao: %s",
			esp_err_to_name(err));

		vQueueDelete(button_event_queue);
		button_event_queue = nullptr;

		return err;
	}

	button_state = read_button_state();

	err = gpio_install_isr_service(0);

	if (err != ESP_OK &&
		err != ESP_ERR_INVALID_STATE) {
		ESP_LOGE(
			TAG,
			"Falha ao instalar ISR GPIO: %s",
			esp_err_to_name(err));

		vQueueDelete(button_event_queue);
		button_event_queue = nullptr;

		return err;
	}

	err = xTaskCreate(
			  button_debounce_task,
			  "button_debounce",
			  2048,
			  nullptr,
			  6,
			  &button_debounce_task_handle) == pdPASS
			  ? ESP_OK
			  : ESP_ERR_NO_MEM;

	if (err != ESP_OK) {
		ESP_LOGE(
			TAG,
			"Falha ao criar task do botao");

		button_debounce_task_handle = nullptr;

		vQueueDelete(button_event_queue);
		button_event_queue = nullptr;

		return err;
	}

	err = gpio_isr_handler_add(
		static_cast<gpio_num_t>(
			CONFIG_APP_BUTTON_GPIO),
		button_isr_handler,
		nullptr);

	if (err != ESP_OK) {
		ESP_LOGE(
			TAG,
			"Falha ao registrar ISR do botao: %s",
			esp_err_to_name(err));

		vTaskDelete(
			button_debounce_task_handle);

		button_debounce_task_handle = nullptr;

		vQueueDelete(button_event_queue);
		button_event_queue = nullptr;

		return err;
	}

	button_initialized = true;

	ESP_LOGI(
		TAG,
		"Botao inicializado no GPIO %d. Estado inicial=%d",
		CONFIG_APP_BUTTON_GPIO,
		button_state);

#if CONFIG_APP_BUTTON_DEBOUNCE

	ESP_LOGI(
		TAG,
		"Debounce habilitado: %d ms",
		CONFIG_APP_BUTTON_DEBOUNCE_MS);

#else

	ESP_LOGI(
		TAG,
		"Debounce desabilitado");

#endif

	return ESP_OK;
}

bool button_get_state(void) {
	bool state;

	portENTER_CRITICAL(
		&button_spinlock);

	state = button_state;

	portEXIT_CRITICAL(
		&button_spinlock);

	return state;
}

bool button_get_event(bool* state) {
	if (state == nullptr ||
		button_event_queue == nullptr) {
		return false;
	}

	button_event_t event;

	if (xQueueReceive(
			button_event_queue,
			&event,
			0) != pdTRUE) {
		return false;
	}

	*state = event.state;

	return true;
}
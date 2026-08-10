#include "pzem.h"

#include <math.h>
#include <cstring>

#include "driver/gpio.h"
#include "driver/uart.h"

#include "esp_log.h"
#include "sdkconfig.h"

namespace {
#if CONFIG_PZEM_MODE_HARDWARE
	namespace config {

		constexpr uart_port_t pzem_uart = static_cast<uart_port_t>(CONFIG_APP_PZEM_UART_PORT);
		constexpr gpio_num_t tx_gpio = static_cast<gpio_num_t>(CONFIG_APP_PZEM_TX_GPIO);
		constexpr gpio_num_t rx_gpio = static_cast<gpio_num_t>(CONFIG_APP_PZEM_RX_GPIO);

		constexpr uint32_t baud_rate = 9600;

		constexpr size_t rx_buffer_size = 256;
		constexpr size_t tx_buffer_size = 256;

		constexpr uint8_t default_address = 0x01;
		constexpr uint8_t function_read_input = 0x04;

		constexpr uint16_t max_registers = 10;
		constexpr uint16_t modbus_max_registers = 125;

		constexpr uint8_t crc_size = 2;

		constexpr size_t max_response_size = (max_registers * 2) + crc_size;
		constexpr size_t max_frame_size = 3 + max_response_size;

		constexpr TickType_t timeout = pdMS_TO_TICKS(1000);
	}

	static_assert(
		config::max_registers > 0 &&
			config::max_registers <= config::modbus_max_registers,
		"Quantidade de registradores Modbus invalida");

	static_assert(
		config::tx_gpio != config::rx_gpio,
		"TX e RX nao podem usar o mesmo GPIO");

	static_assert(
		config::max_response_size <= 255,
		"Buffer de resposta excede o tamanho permitido");

	static_assert(
		config::baud_rate > 0,
		"Baud rate deve ser maior que zero");

	static_assert(
		config::default_address >= 1 &&
			config::default_address <= 247,
		"Endereco Modbus invalido");
#endif

	static const char* TAG = "PZEM";

	static uint32_t pzem_read_interval_ms = CONFIG_APP_PZEM_READ_INTERVAL * 1000;

#if CONFIG_PZEM_MODE_SIMULATION

	static float simulated_energy = 0.0f;

#endif

}  // namespace

esp_err_t pzem_set_read_interval(
	uint32_t interval_ms) {
	const uint32_t min_interval =
		CONFIG_APP_PZEM_MIN_READ_INTERVAL * 1000;

	const uint32_t max_interval =
		CONFIG_APP_PZEM_MAX_READ_INTERVAL * 1000;

	if (interval_ms < min_interval ||
		interval_ms > max_interval) {
		ESP_LOGW(
			TAG,
			"Intervalo invalido: %lu ms. "
			"Permitido: %lu-%lu ms",
			static_cast<unsigned long>(interval_ms),
			static_cast<unsigned long>(min_interval),
			static_cast<unsigned long>(max_interval));

		return ESP_ERR_INVALID_ARG;
	}

	pzem_read_interval_ms =
		interval_ms;

	ESP_LOGI(
		TAG,
		"Intervalo de leitura alterado para %lu ms",
		static_cast<unsigned long>(
			pzem_read_interval_ms));

	return ESP_OK;
}

uint32_t pzem_get_read_interval(void) {
	return pzem_read_interval_ms;
}

esp_err_t pzem_init(void) {
#if CONFIG_PZEM_MODE_SIMULATION

	simulated_energy = 0.0f;

	ESP_LOGW(
		TAG,
		"PZEM em modo SIMULACAO");

	return ESP_OK;

#else
	uart_config_t uart_config = {};

	uart_config.baud_rate = config::baud_rate;
	uart_config.data_bits = UART_DATA_8_BITS;
	uart_config.parity = UART_PARITY_DISABLE;
	uart_config.stop_bits = UART_STOP_BITS_1;
	uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
	uart_config.rx_flow_ctrl_thresh = 0;
	uart_config.source_clk = UART_SCLK_DEFAULT;

	esp_err_t err = uart_driver_install(config::uart, config::rx_buffer_size,
										config::tx_buffer_size, 0, nullptr, 0);

	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Falha ao instalar driver UART: %s", esp_err_to_name(err));
		return err;
	}

	err = uart_param_config(config::uart, &uart_config);

	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Falha ao configurar UART: %s", esp_err_to_name(err));
		uart_driver_delete(config::uart);
		return err;
	}

	err = uart_set_pin(config::uart, config::tx_gpio, config::rx_gpio,
					   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Falha ao configurar GPIOs da UART: %s",
				 esp_err_to_name(err));
		uart_driver_delete(config::uart);
		return err;
	}

	ESP_LOGI(TAG, "UART do PZEM inicializada: UART=%d TX=%d RX=%d Baud=%lu",
			 config::uart, config::tx_gpio, config::rx_gpio,
			 static_cast<unsigned long>(config::baud_rate));

	return ESP_OK;

#endif
}

#if CONFIG_PZEM_MODE_HARDWARE

static uint16_t pzem_crc16(const uint8_t* data, size_t length) {
	uint16_t crc = 0xFFFF;

	for (size_t i = 0; i < length; i++) {
		crc ^= data[i];

		for (int j = 0; j < 8; j++) {
			if (crc & 0x0001) {
				crc >>= 1;
				crc ^= 0xA001;
			} else {
				crc >>= 1;
			}
		}
	}

	return crc;
}

static void pzem_flush_input(void) {
	uart_flush_input(config::uart);
}

static esp_err_t pzem_send_request(uint8_t slave, uint8_t function, uint16_t address, uint16_t quantity) {
	uint8_t frame[8];

	frame[0] = slave;
	frame[1] = function;

	frame[2] = (address >> 8) & 0xFF;
	frame[3] = address & 0xFF;

	frame[4] = (quantity >> 8) & 0xFF;
	frame[5] = quantity & 0xFF;

	uint16_t crc = pzem_crc16(frame, 6);

	frame[6] = crc & 0xFF;
	frame[7] = (crc >> 8) & 0xFF;

	pzem_flush_input();

	int written = uart_write_bytes(config::uart, frame, sizeof(frame));

	if (written != sizeof(frame)) {
		ESP_LOGE(TAG, "Falha ao enviar requisicao Modbus");
		return ESP_FAIL;
	}

	return ESP_OK;
}

static esp_err_t pzem_receive_header(uint8_t* header) {
	int len = uart_read_bytes(config::uart, header, 3, config::timeout);

	if (len != 3) {
		ESP_LOGE(TAG, "Timeout lendo cabecalho Modbus");
		return ESP_ERR_TIMEOUT;
	}

	return ESP_OK;
}

static esp_err_t pzem_validate_header(const uint8_t* header, uint8_t slave, uint16_t quantity) {
	uint8_t response_slave = header[0];
	uint8_t response_function = header[1];
	uint8_t byte_count = header[2];

	if (response_slave != slave) {
		ESP_LOGE(TAG, "Slave incorreto: esperado 0x%02X, recebido 0x%02X", slave,
				 response_slave);

		return ESP_ERR_INVALID_RESPONSE;
	}

	if (response_function == (config::function_read_input | 0x80)) {
		ESP_LOGE(TAG, "PZEM retornou excecao Modbus");
		return ESP_ERR_INVALID_RESPONSE;
	}

	if (response_function != config::function_read_input) {
		ESP_LOGE(TAG, "Funcao Modbus inesperada: 0x%02X", response_function);

		return ESP_ERR_INVALID_RESPONSE;
	}

	if (byte_count != quantity * 2) {
		ESP_LOGE(TAG, "Quantidade de bytes incorreta: esperado %d, recebido %d",
				 quantity * 2, byte_count);

		return ESP_ERR_INVALID_RESPONSE;
	}

	return ESP_OK;
}

static esp_err_t pzem_receive_data(uint8_t* response, uint8_t byte_count) {
	if (byte_count > config::max_registers * 2) {
		return ESP_ERR_INVALID_SIZE;
	}
	int expected_length = byte_count + 2;

	int len = uart_read_bytes(config::uart, response, expected_length, config::timeout);

	if (len != expected_length) {
		ESP_LOGE(TAG, "Resposta incompleta: esperado %d bytes, recebido %d",
				 expected_length, len);

		return ESP_ERR_TIMEOUT;
	}

	return ESP_OK;
}

static esp_err_t pzem_validate_crc(const uint8_t* header,
								   const uint8_t* response,
								   uint8_t byte_count) {
	uint8_t frame[config::max_frame_size];

	frame[0] = header[0];
	frame[1] = header[1];
	frame[2] = header[2];

	std::memcpy(&frame[3], response, byte_count + 2);

	size_t frame_length = 3 + byte_count + 2;

	uint16_t received_crc = (uint16_t)response[byte_count] |
							((uint16_t)response[byte_count + 1] << 8);

	uint16_t calculated_crc = pzem_crc16(frame, frame_length - 2);

	if (received_crc != calculated_crc) {
		ESP_LOGE(TAG, "CRC invalido: recebido=0x%04X calculado=0x%04X",
				 received_crc, calculated_crc);

		return ESP_ERR_INVALID_CRC;
	}

	return ESP_OK;
}

static void pzem_parse_registers(
	const uint8_t* response,
	uint16_t quantity,
	uint16_t* registers) {
	for (uint16_t i = 0; i < quantity; i++) {
		registers[i] = ((uint16_t)response[i * 2] << 8) |
					   response[i * 2 + 1];
	}
}

static esp_err_t pzem_read_registers(uint8_t slave, uint16_t start_register, uint16_t quantity, uint16_t* registers) {
	if (registers == nullptr) {
		return ESP_ERR_INVALID_ARG;
	}

	if (quantity == 0 || quantity > config::max_registers) {
		return ESP_ERR_INVALID_ARG;
	}

	esp_err_t err = pzem_send_request(slave, config::function_read_input, start_register, quantity);
	if (err != ESP_OK) {
		return err;
	}

	uint8_t header[3];
	err = pzem_receive_header(header);
	if (err != ESP_OK) {
		return err;
	}

	err = pzem_validate_header(header, slave, quantity);
	if (err != ESP_OK) {
		return err;
	}
	uint8_t byte_count = header[2];

	uint8_t response[config::max_response_size];
	err = pzem_receive_data(response, byte_count);
	if (err != ESP_OK) {
		return err;
	}

	err = pzem_validate_crc(header, response, byte_count);
	if (err != ESP_OK) {
		return err;
	}

	pzem_parse_registers(response, quantity, registers);

	return ESP_OK;
}
#endif

esp_err_t pzem_read_all(pzem_data_t* data) {
	if (data == nullptr) {
		return ESP_ERR_INVALID_ARG;
	}

#if CONFIG_PZEM_MODE_SIMULATION

	data->voltage = static_cast<float>(CONFIG_PZEM_SIM_VOLTAGE);

	data->current = static_cast<float>(CONFIG_PZEM_SIM_CURRENT);

	data->frequency = static_cast<float>(CONFIG_PZEM_SIM_FREQUENCY);

	data->power_factor = static_cast<float>(CONFIG_PZEM_SIM_POWER_FACTOR) / 100.0f;

	data->power = data->voltage * data->current * data->power_factor;

	data->energy += data->power / 3600000.0f;

	return ESP_OK;

#else

	uint16_t registers[10];

	esp_err_t err = pzem_read_registers(config::default_address, 0x0000, 10, registers);

	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Falha ao ler registradores do PZEM: %s",
				 esp_err_to_name(err));

		return err;
	}

	uint32_t current_raw = ((uint32_t)registers[2] << 16) | registers[1];

	uint32_t power_raw = ((uint32_t)registers[4] << 16) | registers[3];

	uint32_t energy_raw = ((uint32_t)registers[6] << 16) | registers[5];

	data->voltage = registers[0] / 10.0f;

	data->current = current_raw / 1000.0f;

	data->power = power_raw / 10.0f;

	data->energy = (float)energy_raw;

	data->frequency = registers[7] / 10.0f;

	data->power_factor = registers[8] / 100.0f;

	return ESP_OK;

#endif
}
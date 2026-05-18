#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/pulse_cnt.h"

#include "esp_wifi.h"
#include "mqtt_client.h"

#include "dht22.h"

#define RETURN_ON_ERROR_LOCAL(x, log_tag, format, ...) do {                                  \
		esp_err_t err_rc_ = (x);                                                              \
		if (err_rc_ != ESP_OK) {                                                              \
			ESP_LOGE(log_tag, "%s(%d): " format, __FUNCTION__, __LINE__, ##__VA_ARGS__);    \
			return err_rc_;                                                                   \
		}                                                                                     \
	} while (0)

#ifndef ESP_RETURN_ON_ERROR
#define ESP_RETURN_ON_ERROR(x, log_tag, format, ...) RETURN_ON_ERROR_LOCAL(x, log_tag, format, ##__VA_ARGS__)
#endif

// ======================= Configuracion general =======================
#define WIFI_SSID                  "CHRISTIAN_8689"
#define WIFI_PASS                  "redes2026"

#define MQTT_BROKER_URI            "mqtt://192.168.137.143:1883"
#define MQTT_BROKER_HOST           "192.168.137.143"
#define MQTT_BROKER_PORT           1883

// Encoder
#define ENCODER_PULSES_PER_REV     20
#define ENCODER_GPIO               15

// PID inicial
#define PID_KP_DEFAULT             2.5f
#define PID_KI_DEFAULT             0.5f
#define PID_KD_DEFAULT             0.1f

// Control motor
#define PWM_FREQ_HZ                5000
#define PWM_RES_BITS               8
#define PWM_GPIO                   20
#define PID_LOOP_MS                100
#define RPM_PUBLISH_INTERVAL_MS    1000

// Limites de RPM
#define MAX_RPM                    300.0f
#define MIN_RPM                    0.0f

// DHT22
#define DHT22_GPIO                 10

// Rele
#define RELAY_GPIO                 2

// MFRC522 (SPI)
#define RFID_SPI_HOST              SPI2_HOST
#define RFID_PIN_MISO              19
#define RFID_PIN_MOSI              23
#define RFID_PIN_SCLK              18
#define RFID_PIN_CS                5
#define RFID_PIN_RST               21



// Tareas
#define TASK_STACK_SIZE            4096

// MQTT QoS configurables
#define MQTT_QOS_SENSORS           0
#define MQTT_QOS_ACTUATORS         1
#define MQTT_QOS_TELEMETRY         0

// Topicos
#define TOPIC_TEMP                 "sensores/temperatura"
#define TOPIC_HUM                  "sensores/humedad"
#define TOPIC_RFID                 "seguridad/rfid"
#define TOPIC_MOTOR_SETPOINT       "actuadores/motor/setpoint"
#define TOPIC_MOTOR_STATE          "actuadores/motor/estado"
#define TOPIC_RELAY_CONTROL        "actuadores/rele/control"
#define TOPIC_RELAY_STATE          "actuadores/rele/estado"
#define TOPIC_REQUEST_STATE        "solicitud/estado"

static const char *TAG = "IOT_MQTT_PID";

// ======================= Estado PID =======================
typedef struct {
	float kp;
	float ki;
	float kd;
	float integral;
	float prev_error;
	float out_min;
	float out_max;
	float integral_min;
	float integral_max;
	float last_p;
	float last_i;
	float last_d;
} pid_state_t;

typedef struct {
	float rpm_actual;
	float rpm_deseado;
	uint8_t duty_pwm;
	float pid_term_p;
	float pid_term_i;
	float pid_term_d;
} motor_telemetry_t;

// ======================= Variables globales =======================
static EventGroupHandle_t s_wifi_event_group;
static QueueHandle_t motor_setpoint_queue;

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static volatile bool s_mqtt_connected = false;

static pid_state_t s_pid;
static motor_telemetry_t s_motor_telemetry;
static portMUX_TYPE s_motor_mux = portMUX_INITIALIZER_UNLOCKED;

static pcnt_unit_handle_t s_pcnt_unit = NULL;
static pcnt_channel_handle_t s_pcnt_chan = NULL;

static spi_device_handle_t s_rfid_spi = NULL;

static const int WIFI_CONNECTED_BIT = BIT0;

// ======================= Driver MFRC522 minimo =======================
#define MFRC522_REG_COMMAND        0x01
#define MFRC522_REG_COM_I_EN       0x02
#define MFRC522_REG_DIV_I_EN       0x03
#define MFRC522_REG_COM_IRQ        0x04
#define MFRC522_REG_DIV_IRQ        0x05
#define MFRC522_REG_ERROR          0x06
#define MFRC522_REG_STATUS2        0x08
#define MFRC522_REG_FIFO_DATA      0x09
#define MFRC522_REG_FIFO_LEVEL     0x0A
#define MFRC522_REG_CONTROL        0x0C
#define MFRC522_REG_BIT_FRAMING    0x0D
#define MFRC522_REG_MODE           0x11
#define MFRC522_REG_TX_CONTROL     0x14
#define MFRC522_REG_TX_AUTO        0x15
#define MFRC522_REG_T_MODE         0x2A
#define MFRC522_REG_T_PRESCALER    0x2B
#define MFRC522_REG_T_RELOAD_H     0x2C
#define MFRC522_REG_T_RELOAD_L     0x2D

#define PCD_IDLE                   0x00
#define PCD_TRANSCEIVE             0x0C
#define PCD_SOFTRESET              0x0F

#define PICC_CMD_REQA              0x26
#define PICC_CMD_ANTICOLL_CL1      0x93

static inline uint8_t clamp_qos(int qos)
{
	if (qos < 0) {
		return 0;
	}
	if (qos > 2) {
		return 2;
	}
	return (uint8_t)qos;
}

static void mqtt_publish_if_connected(const char *topic, const char *payload, int qos, int retain)
{
	if (!s_mqtt_connected || s_mqtt_client == NULL) {
		return;
	}
	esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, clamp_qos(qos), retain);
}

static esp_err_t mfrc522_write_reg(uint8_t reg, uint8_t val)
{
	uint8_t tx[2] = {(uint8_t)((reg << 1U) & 0x7EU), val};
	spi_transaction_t t = {
		.length = 16,
		.tx_buffer = tx,
	};
	return spi_device_polling_transmit(s_rfid_spi, &t);
}

static esp_err_t mfrc522_read_reg(uint8_t reg, uint8_t *val)
{
	uint8_t tx[2] = {(uint8_t)(((reg << 1U) & 0x7EU) | 0x80U), 0x00};
	uint8_t rx[2] = {0};
	spi_transaction_t t = {
		.length = 16,
		.tx_buffer = tx,
		.rx_buffer = rx,
	};
	esp_err_t err = spi_device_polling_transmit(s_rfid_spi, &t);
	if (err == ESP_OK) {
		*val = rx[1];
	}
	return err;
}

static esp_err_t mfrc522_set_bitmask(uint8_t reg, uint8_t mask)
{
	uint8_t val = 0;
	ESP_RETURN_ON_ERROR(mfrc522_read_reg(reg, &val), TAG, "RFID read reg fallo");
	return mfrc522_write_reg(reg, val | mask);
}

static esp_err_t mfrc522_clear_bitmask(uint8_t reg, uint8_t mask)
{
	uint8_t val = 0;
	ESP_RETURN_ON_ERROR(mfrc522_read_reg(reg, &val), TAG, "RFID read reg fallo");
	return mfrc522_write_reg(reg, val & (uint8_t)(~mask));
}

static esp_err_t mfrc522_reset(void)
{
	ESP_RETURN_ON_ERROR(mfrc522_write_reg(MFRC522_REG_COMMAND, PCD_SOFTRESET), TAG, "RFID reset cmd fallo");
	vTaskDelay(pdMS_TO_TICKS(50));
	return ESP_OK;
}

static esp_err_t mfrc522_init(void)
{
	gpio_config_t rst_cfg = {
		.pin_bit_mask = 1ULL << RFID_PIN_RST,
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	ESP_RETURN_ON_ERROR(gpio_config(&rst_cfg), TAG, "RFID RST GPIO fallo");
	gpio_set_level(RFID_PIN_RST, 1);

	spi_bus_config_t buscfg = {
		.mosi_io_num = RFID_PIN_MOSI,
		.miso_io_num = RFID_PIN_MISO,
		.sclk_io_num = RFID_PIN_SCLK,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = 32,
	};

	esp_err_t err = spi_bus_initialize(RFID_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
	if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
		ESP_LOGE(TAG, "spi_bus_initialize RFID fallo: %s", esp_err_to_name(err));
		return err;
	}

	spi_device_interface_config_t devcfg = {
		.clock_speed_hz = 1000000,
		.mode = 0,
		.spics_io_num = RFID_PIN_CS,
		.queue_size = 1,
		.flags = 0,
	};

	ESP_RETURN_ON_ERROR(spi_bus_add_device(RFID_SPI_HOST, &devcfg, &s_rfid_spi), TAG, "RFID add device fallo");
	ESP_RETURN_ON_ERROR(mfrc522_reset(), TAG, "RFID reset fallo");

	ESP_RETURN_ON_ERROR(mfrc522_write_reg(MFRC522_REG_T_MODE, 0x8D), TAG, "RFID T_MODE fallo");
	ESP_RETURN_ON_ERROR(mfrc522_write_reg(MFRC522_REG_T_PRESCALER, 0x3E), TAG, "RFID T_PRESCALER fallo");
	ESP_RETURN_ON_ERROR(mfrc522_write_reg(MFRC522_REG_T_RELOAD_L, 30), TAG, "RFID T_RELOAD_L fallo");
	ESP_RETURN_ON_ERROR(mfrc522_write_reg(MFRC522_REG_T_RELOAD_H, 0), TAG, "RFID T_RELOAD_H fallo");
	ESP_RETURN_ON_ERROR(mfrc522_write_reg(MFRC522_REG_TX_AUTO, 0x40), TAG, "RFID TX_AUTO fallo");
	ESP_RETURN_ON_ERROR(mfrc522_write_reg(MFRC522_REG_MODE, 0x3D), TAG, "RFID MODE fallo");

	// Activa la antena del lector
	ESP_RETURN_ON_ERROR(mfrc522_set_bitmask(MFRC522_REG_TX_CONTROL, 0x03), TAG, "RFID antena fallo");
	ESP_LOGI(TAG, "MFRC522 inicializado");
	return ESP_OK;
}

static esp_err_t mfrc522_transceive(const uint8_t *send, uint8_t send_len, uint8_t *back, uint8_t *back_len, uint8_t valid_bits)
{
	uint8_t irq_en = 0x77;
	uint8_t wait_irq = 0x30;
	uint8_t n = 0;
	uint8_t last_bits = 0;

	ESP_RETURN_ON_ERROR(mfrc522_write_reg(MFRC522_REG_COM_I_EN, irq_en | 0x80), TAG, "RFID IRQ EN fallo");
	ESP_RETURN_ON_ERROR(mfrc522_clear_bitmask(MFRC522_REG_COM_IRQ, 0x80), TAG, "RFID clear IRQ fallo");
	ESP_RETURN_ON_ERROR(mfrc522_set_bitmask(MFRC522_REG_FIFO_LEVEL, 0x80), TAG, "RFID flush FIFO fallo");
	ESP_RETURN_ON_ERROR(mfrc522_write_reg(MFRC522_REG_COMMAND, PCD_IDLE), TAG, "RFID CMD idle fallo");

	for (uint8_t i = 0; i < send_len; ++i) {
		ESP_RETURN_ON_ERROR(mfrc522_write_reg(MFRC522_REG_FIFO_DATA, send[i]), TAG, "RFID FIFO write fallo");
	}

	ESP_RETURN_ON_ERROR(mfrc522_write_reg(MFRC522_REG_BIT_FRAMING, valid_bits), TAG, "RFID framing fallo");
	ESP_RETURN_ON_ERROR(mfrc522_write_reg(MFRC522_REG_COMMAND, PCD_TRANSCEIVE), TAG, "RFID CMD transceive fallo");
	ESP_RETURN_ON_ERROR(mfrc522_set_bitmask(MFRC522_REG_BIT_FRAMING, 0x80), TAG, "RFID start send fallo");

	int i = 2000;
	do {
		if (mfrc522_read_reg(MFRC522_REG_COM_IRQ, &n) != ESP_OK) {
			return ESP_FAIL;
		}
		i--;
	} while (i != 0 && (n & 0x01) == 0 && (n & wait_irq) == 0);

	ESP_RETURN_ON_ERROR(mfrc522_clear_bitmask(MFRC522_REG_BIT_FRAMING, 0x80), TAG, "RFID clear start send fallo");

	if (i == 0) {
		return ESP_ERR_TIMEOUT;
	}

	uint8_t err = 0;
	ESP_RETURN_ON_ERROR(mfrc522_read_reg(MFRC522_REG_ERROR, &err), TAG, "RFID read error reg fallo");
	if (err & 0x1B) {
		return ESP_FAIL;
	}

	if ((n & irq_en & 0x01) != 0) {
		return ESP_ERR_TIMEOUT;
	}

	if (back != NULL && back_len != NULL) {
		uint8_t fifo_level = 0;
		ESP_RETURN_ON_ERROR(mfrc522_read_reg(MFRC522_REG_FIFO_LEVEL, &fifo_level), TAG, "RFID read fifo level fallo");
		ESP_RETURN_ON_ERROR(mfrc522_read_reg(MFRC522_REG_CONTROL, &last_bits), TAG, "RFID read control fallo");
		last_bits &= 0x07;

		uint8_t length = (last_bits != 0) ? (uint8_t)((fifo_level - 1) * 8 + last_bits) : (uint8_t)(fifo_level * 8);
		uint8_t bytes = (uint8_t)((length + 7) / 8);
		if (bytes > *back_len) {
			return ESP_ERR_INVALID_SIZE;
		}

		for (uint8_t j = 0; j < bytes; ++j) {
			ESP_RETURN_ON_ERROR(mfrc522_read_reg(MFRC522_REG_FIFO_DATA, &back[j]), TAG, "RFID read fifo fallo");
		}
		*back_len = bytes;
	}

	return ESP_OK;
}

static esp_err_t mfrc522_read_uid(char *uid_buf, size_t uid_buf_len)
{
	uint8_t atqa[2] = {0};
	uint8_t atqa_len = sizeof(atqa);
	uint8_t reqa = PICC_CMD_REQA;
	esp_err_t err = mfrc522_transceive(&reqa, 1, atqa, &atqa_len, 0x07);
	if (err != ESP_OK) {
		return err;
	}

	uint8_t anticoll_cmd[2] = {PICC_CMD_ANTICOLL_CL1, 0x20};
	uint8_t resp[5] = {0};
	uint8_t resp_len = sizeof(resp);
	err = mfrc522_transceive(anticoll_cmd, 2, resp, &resp_len, 0x00);
	if (err != ESP_OK || resp_len < 5) {
		return ESP_FAIL;
	}

	uint8_t bcc = (uint8_t)(resp[0] ^ resp[1] ^ resp[2] ^ resp[3]);
	if (bcc != resp[4]) {
		return ESP_ERR_INVALID_CRC;
	}

	snprintf(uid_buf, uid_buf_len, "%02X:%02X:%02X:%02X", resp[0], resp[1], resp[2], resp[3]);
	return ESP_OK;
}

// ======================= PID =======================
static void pid_init(pid_state_t *pid, float kp, float ki, float kd)
{
	pid->kp = kp;
	pid->ki = ki;
	pid->kd = kd;
	pid->integral = 0.0f;
	pid->prev_error = 0.0f;
	pid->out_min = 0.0f;
	pid->out_max = (float)((1U << PWM_RES_BITS) - 1U);
	pid->integral_min = -200.0f;
	pid->integral_max = 200.0f;
	pid->last_p = 0.0f;
	pid->last_i = 0.0f;
	pid->last_d = 0.0f;
}

static float pid_compute(pid_state_t *pid, float setpoint, float actual, float dt)
{
	if (dt <= 0.0f) {
		return 0.0f;
	}

	float error = setpoint - actual;

	// Anti-windup: limita integral y evita crecer si salida esta saturada
	float p_term = pid->kp * error;
	float derivative = (error - pid->prev_error) / dt;
	float d_term = pid->kd * derivative;

	float tentative_integral = pid->integral + (error * dt);
	if (tentative_integral > pid->integral_max) {
		tentative_integral = pid->integral_max;
	} else if (tentative_integral < pid->integral_min) {
		tentative_integral = pid->integral_min;
	}
	float i_term_candidate = pid->ki * tentative_integral;

	float output_candidate = p_term + i_term_candidate + d_term;
	bool saturated_high = output_candidate > pid->out_max;
	bool saturated_low = output_candidate < pid->out_min;
	bool helps_desaturate = (saturated_high && error < 0.0f) || (saturated_low && error > 0.0f);

	if (!saturated_high && !saturated_low) {
		pid->integral = tentative_integral;
	} else if (helps_desaturate) {
		pid->integral = tentative_integral;
	}

	pid->last_p = p_term;
	pid->last_i = pid->ki * pid->integral;
	pid->last_d = d_term;

	float output = pid->last_p + pid->last_i + pid->last_d;
	if (output > pid->out_max) {
		output = pid->out_max;
	} else if (output < pid->out_min) {
		output = pid->out_min;
	}

	pid->prev_error = error;
	return output;
}

// ======================= Motor/encoder =======================
static float read_rpm_from_encoder(int32_t pulses, float sample_time_s)
{
	if (sample_time_s <= 0.0f || ENCODER_PULSES_PER_REV <= 0) {
		return 0.0f;
	}
	float revs = (float)pulses / (float)ENCODER_PULSES_PER_REV;
	float rps = revs / sample_time_s;
	return rps * 60.0f;
}

static esp_err_t relay_init(void)
{
	gpio_config_t io_conf = {
		.pin_bit_mask = 1ULL << RELAY_GPIO,
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "Error init rele");
	gpio_set_level(RELAY_GPIO, 0);
	return ESP_OK;
}

static esp_err_t pwm_init(void)
{
	ledc_timer_config_t ledc_timer = {
		.speed_mode = LEDC_LOW_SPEED_MODE,
		.duty_resolution = PWM_RES_BITS,
		.timer_num = LEDC_TIMER_0,
		.freq_hz = PWM_FREQ_HZ,
		.clk_cfg = LEDC_AUTO_CLK,
	};
	ESP_RETURN_ON_ERROR(ledc_timer_config(&ledc_timer), TAG, "Error init timer LEDC");

	ledc_channel_config_t ledc_channel = {
		.gpio_num = PWM_GPIO,
		.speed_mode = LEDC_LOW_SPEED_MODE,
		.channel = LEDC_CHANNEL_0,
		.intr_type = LEDC_INTR_DISABLE,
		.timer_sel = LEDC_TIMER_0,
		.duty = 0,
		.hpoint = 0,
	};
	ESP_RETURN_ON_ERROR(ledc_channel_config(&ledc_channel), TAG, "Error init canal LEDC");
	return ESP_OK;
}

static esp_err_t pwm_set_duty_raw(uint32_t duty)
{
	uint32_t max_duty = (1U << PWM_RES_BITS) - 1U;
	if (duty > max_duty) {
		duty = max_duty;
	}
	ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty), TAG, "Error set duty LEDC");
	ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0), TAG, "Error update duty LEDC");
	return ESP_OK;
}

static esp_err_t encoder_pcnt_init(void)
{
	pcnt_unit_config_t unit_config = {
		.high_limit = 32767,
		.low_limit = -32768,
	};
	ESP_RETURN_ON_ERROR(pcnt_new_unit(&unit_config, &s_pcnt_unit), TAG, "Error crear PCNT unit");

	pcnt_chan_config_t chan_config = {
		.edge_gpio_num = ENCODER_GPIO,
		.level_gpio_num = -1,
	};
	ESP_RETURN_ON_ERROR(pcnt_new_channel(s_pcnt_unit, &chan_config, &s_pcnt_chan), TAG, "Error crear PCNT channel");
	ESP_RETURN_ON_ERROR(
		pcnt_channel_set_edge_action(s_pcnt_chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD),
		TAG,
		"Error edge action PCNT"
	);
	ESP_RETURN_ON_ERROR(
		pcnt_channel_set_level_action(s_pcnt_chan, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_KEEP),
		TAG,
		"Error level action PCNT"
	);

	pcnt_glitch_filter_config_t filter_cfg = {
		.max_glitch_ns = 1000,
	};
	ESP_RETURN_ON_ERROR(pcnt_unit_set_glitch_filter(s_pcnt_unit, &filter_cfg), TAG, "Error filtro PCNT");
	ESP_RETURN_ON_ERROR(pcnt_unit_enable(s_pcnt_unit), TAG, "Error habilitar PCNT");
	ESP_RETURN_ON_ERROR(pcnt_unit_clear_count(s_pcnt_unit), TAG, "Error limpiar contador PCNT");
	ESP_RETURN_ON_ERROR(pcnt_unit_start(s_pcnt_unit), TAG, "Error iniciar PCNT");
	return ESP_OK;
}

// ======================= WiFi / MQTT =======================
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
	(void)handler_args;
	(void)base;
	esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

	switch (event_id) {
		case MQTT_EVENT_CONNECTED:
			s_mqtt_connected = true;
			ESP_LOGI(TAG, "MQTT conectado");
			esp_mqtt_client_subscribe(s_mqtt_client, TOPIC_MOTOR_SETPOINT, MQTT_QOS_ACTUATORS);
			esp_mqtt_client_subscribe(s_mqtt_client, TOPIC_RELAY_CONTROL, MQTT_QOS_ACTUATORS);
			esp_mqtt_client_subscribe(s_mqtt_client, TOPIC_REQUEST_STATE, MQTT_QOS_ACTUATORS);
			break;

		case MQTT_EVENT_DISCONNECTED:
			s_mqtt_connected = false;
			ESP_LOGW(TAG, "MQTT desconectado");
			break;

		case MQTT_EVENT_DATA: {
			char topic[128] = {0};
			char data[128] = {0};
			int tlen = (event->topic_len < (int)sizeof(topic) - 1) ? event->topic_len : (int)sizeof(topic) - 1;
			int dlen = (event->data_len < (int)sizeof(data) - 1) ? event->data_len : (int)sizeof(data) - 1;
			memcpy(topic, event->topic, tlen);
			memcpy(data, event->data, dlen);

			if (strcmp(topic, TOPIC_MOTOR_SETPOINT) == 0) {
				float new_setpoint = 0.0f;
				if (strcasecmp(data, "OFF") == 0) {
					new_setpoint = 0.0f;
				} else {
					char *endptr = NULL;
					float value = strtof(data, &endptr);
					if (endptr != data) {
						if (value > MAX_RPM) {
							value = MAX_RPM;
						}
						if (value < MIN_RPM) {
							value = MIN_RPM;
						}
						new_setpoint = value;
					} else {
						ESP_LOGW(TAG, "Setpoint invalido: %s", data);
						break;
					}
				}

				if (xQueueOverwrite(motor_setpoint_queue, &new_setpoint) != pdPASS) {
					ESP_LOGW(TAG, "No se pudo actualizar setpoint en cola");
				}
			} else if (strcmp(topic, TOPIC_RELAY_CONTROL) == 0) {
				if (strcasecmp(data, "ON") == 0) {
					gpio_set_level(RELAY_GPIO, 1);
					mqtt_publish_if_connected(TOPIC_RELAY_STATE, "ON", MQTT_QOS_ACTUATORS, 0);
				} else if (strcasecmp(data, "OFF") == 0) {
					gpio_set_level(RELAY_GPIO, 0);
					mqtt_publish_if_connected(TOPIC_RELAY_STATE, "OFF", MQTT_QOS_ACTUATORS, 0);
				}
			} else if (strcmp(topic, TOPIC_REQUEST_STATE) == 0) {
				motor_telemetry_t snapshot = {0};
				taskENTER_CRITICAL(&s_motor_mux);
				snapshot = s_motor_telemetry;
				taskEXIT_CRITICAL(&s_motor_mux);

				char json[256];
				snprintf(
					json,
					sizeof(json),
					"{\"rpm_actual\":%.2f,\"rpm_deseado\":%.2f,\"duty_pwm\":%u,\"pid_term_p\":%.2f,\"pid_term_i\":%.2f,\"pid_term_d\":%.2f}",
					snapshot.rpm_actual,
					snapshot.rpm_deseado,
					snapshot.duty_pwm,
					snapshot.pid_term_p,
					snapshot.pid_term_i,
					snapshot.pid_term_d
				);

				const char *reply_topic = (strlen(data) > 0) ? data : TOPIC_MOTOR_STATE;
				mqtt_publish_if_connected(reply_topic, json, MQTT_QOS_ACTUATORS, 0);
			}
			break;
		}

		default:
			break;
	}
}

static void start_mqtt_client(void)
{
	if (s_mqtt_client != NULL) {
		return;
	}

	esp_mqtt_client_config_t mqtt_cfg = {
		.broker.address.uri = MQTT_BROKER_URI,
		.broker.address.hostname = MQTT_BROKER_HOST,
		.broker.address.port = MQTT_BROKER_PORT,
		.credentials.client_id = "esp32c6_iot_node",
		.session.keepalive = 30,
		.network.reconnect_timeout_ms = 5000,
	};

	s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
	if (s_mqtt_client == NULL) {
		ESP_LOGE(TAG, "No se pudo crear cliente MQTT");
		return;
	}

	esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
	ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt_client));
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
	(void)arg;
	(void)event_data;
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
		esp_wifi_connect();
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
		ESP_LOGW(TAG, "WiFi desconectado, reintentando...");
		esp_wifi_connect();
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
		ESP_LOGI(TAG, "WiFi conectado con IP");
		start_mqtt_client();
	}
}

static esp_err_t wifi_init_sta(void)
{
	s_wifi_event_group = xEventGroupCreate();
	if (s_wifi_event_group == NULL) {
		return ESP_ERR_NO_MEM;
	}

	ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init fallo");
	ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop fallo");
	esp_netif_create_default_wifi_sta();

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init fallo");

	ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL), TAG, "WiFi handler fallo");
	ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL), TAG, "IP handler fallo");

	wifi_config_t wifi_config = {
		.sta = {
			.threshold.authmode = WIFI_AUTH_WPA2_PSK,
			.sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
		},
	};
	strlcpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
	strlcpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));

	ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi_set_mode fallo");
	ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "wifi_set_config fallo");
	ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi_start fallo");
	return ESP_OK;
}

// ======================= Tareas =======================
static void task_sensor_reader(void *arg)
{
	(void)arg;
	dht22_data_t dht_data = {0};

	for (;;) {
		if (dht22_read(DHT22_GPIO, &dht_data) == 0) {
			char msg_temp[32];
			char msg_hum[32];
			snprintf(msg_temp, sizeof(msg_temp), "%.2f", dht_data.temperature);
			snprintf(msg_hum, sizeof(msg_hum), "%.2f", dht_data.humidity);
			mqtt_publish_if_connected(TOPIC_TEMP, msg_temp, MQTT_QOS_SENSORS, 0);
			mqtt_publish_if_connected(TOPIC_HUM, msg_hum, MQTT_QOS_SENSORS, 0);
		} else {
			ESP_LOGW(TAG, "Error de lectura DHT22");
		}

		vTaskDelay(pdMS_TO_TICKS(10000));
	}
}

static void task_rfid_scanner(void *arg)
{
	(void)arg;
	char uid[32] = {0};

	for (;;) {
		esp_err_t ret = mfrc522_read_uid(uid, sizeof(uid));
		if (ret == ESP_OK) {
			mqtt_publish_if_connected(TOPIC_RFID, uid, MQTT_QOS_ACTUATORS, 0);
			ESP_LOGI(TAG, "RFID leido: %s", uid);
		} else {
			ESP_LOGW(TAG, "RFID lectura fallo: %s", esp_err_to_name(ret));
		}
		vTaskDelay(pdMS_TO_TICKS(500));
	}
}

static void task_motor_control(void *arg)
{
	(void)arg;
	TickType_t last_wake = xTaskGetTickCount();
	int64_t prev_us = esp_timer_get_time();

	float rpm_setpoint = 0.0f;
	float received_setpoint = 0.0f;

	for (;;) {
		vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PID_LOOP_MS));

		if (xQueueReceive(motor_setpoint_queue, &received_setpoint, 0) == pdPASS) {
			rpm_setpoint = received_setpoint;
			if (rpm_setpoint <= 0.0f) {
				// Reinicia integral al apagar motor para evitar acumulacion
				s_pid.integral = 0.0f;
				s_pid.prev_error = 0.0f;
			}
		}

		int64_t now_us = esp_timer_get_time();
		float dt = (float)(now_us - prev_us) / 1000000.0f;
		prev_us = now_us;

		int count = 0;
		if (pcnt_unit_get_count(s_pcnt_unit, &count) != ESP_OK) {
			ESP_LOGW(TAG, "No se pudo leer PCNT");
			continue;
		}
		pcnt_unit_clear_count(s_pcnt_unit);

		float rpm_actual = read_rpm_from_encoder(count, dt);
		float pid_out = 0.0f;

		if (rpm_setpoint > 0.0f) {
			if (rpm_actual <= 0.01f && count == 0) {
				// Si no hay pulsos, evita incrementar integral indefinidamente
				float backup_ki = s_pid.ki;
				s_pid.ki = 0.0f;
				pid_out = pid_compute(&s_pid, rpm_setpoint, rpm_actual, dt);
				s_pid.ki = backup_ki;
			} else {
				pid_out = pid_compute(&s_pid, rpm_setpoint, rpm_actual, dt);
			}
		} else {
			pid_out = 0.0f;
		}

		if (pid_out < 0.0f) {
			pid_out = 0.0f;
		}
		uint32_t duty = (uint32_t)pid_out;
		if (pwm_set_duty_raw(duty) != ESP_OK) {
			ESP_LOGW(TAG, "Fallo al actualizar PWM");
		}

		taskENTER_CRITICAL(&s_motor_mux);
		s_motor_telemetry.rpm_actual = rpm_actual;
		s_motor_telemetry.rpm_deseado = rpm_setpoint;
		s_motor_telemetry.duty_pwm = (uint8_t)duty;
		s_motor_telemetry.pid_term_p = s_pid.last_p;
		s_motor_telemetry.pid_term_i = s_pid.last_i;
		s_motor_telemetry.pid_term_d = s_pid.last_d;
		taskEXIT_CRITICAL(&s_motor_mux);
	}
}

static void task_mqtt_publisher(void *arg)
{
	(void)arg;
	motor_telemetry_t snapshot;

	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(RPM_PUBLISH_INTERVAL_MS));

		taskENTER_CRITICAL(&s_motor_mux);
		snapshot = s_motor_telemetry;
		taskEXIT_CRITICAL(&s_motor_mux);

		char json[256];
		snprintf(
			json,
			sizeof(json),
			"{\"rpm_actual\":%.2f,\"rpm_deseado\":%.2f,\"duty_pwm\":%u,\"pid_term_p\":%.2f,\"pid_term_i\":%.2f,\"pid_term_d\":%.2f}",
			snapshot.rpm_actual,
			snapshot.rpm_deseado,
			snapshot.duty_pwm,
			snapshot.pid_term_p,
			snapshot.pid_term_i,
			snapshot.pid_term_d
		);

		mqtt_publish_if_connected(TOPIC_MOTOR_STATE, json, MQTT_QOS_TELEMETRY, 0);
	}
}

void app_main(void)
{
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	pid_init(&s_pid, PID_KP_DEFAULT, PID_KI_DEFAULT, PID_KD_DEFAULT);

	motor_setpoint_queue = xQueueCreate(1, sizeof(float));
	if (motor_setpoint_queue == NULL) {
		ESP_LOGE(TAG, "No se pudo crear cola de setpoint");
		return;
	}

	ESP_ERROR_CHECK(relay_init());
	ESP_ERROR_CHECK(pwm_init());
	if (encoder_pcnt_init() != ESP_OK) {
		ESP_LOGE(TAG, "No se pudo inicializar PCNT del encoder");
		return;
	}

	if (dht22_init(DHT22_GPIO) != 0) {
		ESP_LOGW(TAG, "DHT22 init fallo, la tarea seguira intentando lecturas");
	}

	if (mfrc522_init() != ESP_OK) {
		ESP_LOGW(TAG, "MFRC522 init fallo, tarea RFID seguira activa");
	}

	ESP_ERROR_CHECK(wifi_init_sta());

	// Espera conexion WiFi inicial; MQTT se inicia al obtener IP
	xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

	xTaskCreate(task_sensor_reader, "task_sensor_reader", TASK_STACK_SIZE, NULL, 5, NULL);
	xTaskCreate(task_rfid_scanner, "task_rfid_scanner", TASK_STACK_SIZE, NULL, 5, NULL);
	xTaskCreate(task_motor_control, "task_motor_control", TASK_STACK_SIZE, NULL, 8, NULL);
	xTaskCreate(task_mqtt_publisher, "task_mqtt_publisher", TASK_STACK_SIZE, NULL, 5, NULL);
}

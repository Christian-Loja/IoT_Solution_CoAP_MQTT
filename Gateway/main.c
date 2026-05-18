#include <stdbool.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coap3/coap.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "gateway_mqtt_bridge.h"

#define COAP_SERVER_IP   "192.168.137.57"
#define COAP_SERVER_PORT 5683
#define LED_RED_GPIO     2
#define LED_BLUE_GPIO    4

/* Adjust the GPIOs above if your board uses different indicator pins. */

#define HTTP_TAG "wroom32_bridge"
#define COAP_REQUEST_TIMEOUT_MS 3000
#define COAP_MAX_RETRIES 2
#define COAP_RESPONSE_BUFFER_SIZE 512
#define COAP_REQUEST_BUFFER_SIZE 256
#define RFID_EXPECTED_UID "39:36:18:03"
#define RFID_QUEUE_LENGTH 8

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

typedef struct {
	bool completed;
	bool success;
	int http_status;
	coap_pdu_code_t coap_code;
	char error[96];
	char response_payload[COAP_RESPONSE_BUFFER_SIZE];
	char request_payload[COAP_REQUEST_BUFFER_SIZE];
} coap_result_t;

static EventGroupHandle_t s_wifi_event_group;
static int s_wifi_retry_num;
static httpd_handle_t s_http_server;
static SemaphoreHandle_t s_coap_mutex;
static TimerHandle_t s_led_timer;
static bool s_led_blink_active;
static bool s_led_blink_phase_on;
static bool s_led_red_forced;
static bool s_led_blue_forced;
static coap_result_t s_last_result;
static bool s_coap_started;
static QueueHandle_t s_rfid_queue;
static TaskHandle_t s_rfid_task;

static const char *TAG = HTTP_TAG;

static void leds_apply_state(void);
static esp_err_t send_json_error(httpd_req_t *req, int status, const char *message);
static esp_err_t send_json_ok_raw(httpd_req_t *req, int status, const char *data_json);
static void coap_result_reset(const char *request_payload);
static void coap_result_set_error(int http_status, const char *message);
static coap_response_t coap_message_handler(coap_session_t *session,
											const coap_pdu_t *sent,
											const coap_pdu_t *received,
											const coap_mid_t mid);
static esp_err_t coap_request_common(const char *resource,
									 coap_pdu_type_t pdu_type,
									 coap_pdu_code_t method,
									 const char *payload_json,
									 bool expect_response_json,
									 bool is_post);
static esp_err_t wifi_init_sta(void);
static esp_err_t temp_get_handler(httpd_req_t *req);
static esp_err_t dist_get_handler(httpd_req_t *req);
static esp_err_t servo_post_handler(httpd_req_t *req);
static esp_err_t buzzer_post_handler(httpd_req_t *req);
static esp_err_t mqtt_post_handler(httpd_req_t *req);
static void coap_send_get(const char *resource);
static void coap_send_post(const char *resource, const char *payload_json);
static void mqtt_rfid_message_handler(const char *topic, const char *payload, void *user_ctx);
static void rfid_action_task(void *arg);
static bool extract_rfid_uid(const char *payload, char *uid, size_t uid_len);
static bool rfid_uid_matches_expected(const char *received_uid);
static void handle_rfid_uid(const char *received_uid);

static const char *skip_ws(const char *s)
{
	while (s && *s && isspace((unsigned char)*s)) {
		s++;
	}
	return s;
}

static const char *json_find_value(const char *json, const char *field)
{
	char key[64];
	snprintf(key, sizeof(key), "\"%s\"", field);
	const char *p = strstr(json, key);
	if (!p) {
		return NULL;
	}
	p = strchr(p, ':');
	if (!p) {
		return NULL;
	}
	return skip_ws(p + 1);
}

static bool json_get_int(const char *json, const char *field, int *out)
{
	const char *p = json_find_value(json, field);
	if (!p) {
		return false;
	}

	char *end = NULL;
	long v = strtol(p, &end, 10);
	if (end == p) {
		return false;
	}
	*out = (int)v;
	return true;
}

static bool json_get_float(const char *json, const char *field, float *out)
{
	const char *p = json_find_value(json, field);
	if (!p) {
		return false;
	}

	char *end = NULL;
	float v = strtof(p, &end);
	if (end == p) {
		return false;
	}
	*out = v;
	return true;
}

static bool json_get_string(const char *json, const char *field, char *out, size_t out_len)
{
	const char *p = json_find_value(json, field);
	if (!p || *p != '"') {
		return false;
	}

	p++;
	const char *end = strchr(p, '"');
	if (!end) {
		return false;
	}

	size_t n = (size_t)(end - p);
	if (n + 1 > out_len) {
		return false;
	}
	memcpy(out, p, n);
	out[n] = '\0';
	return true;
}

static void trim_copy(const char *input, char *output, size_t output_len)
{
	size_t start = 0;
	size_t end = strlen(input);

	while (start < end && isspace((unsigned char)input[start])) {
		start++;
	}
	while (end > start && isspace((unsigned char)input[end - 1])) {
		end--;
	}

	if (end - start >= output_len) {
		end = start + output_len - 1;
	}
	memcpy(output, input + start, end - start);
	output[end - start] = '\0';

	if (output[0] == '"') {
		size_t len = strlen(output);
		if (len >= 2 && output[len - 1] == '"') {
			memmove(output, output + 1, len - 2);
			output[len - 2] = '\0';
		}
	}
}

static void normalize_uid(const char *input, char *output, size_t output_len)
{
	size_t out_index = 0;
	for (size_t i = 0; input[i] != '\0' && out_index + 1 < output_len; ++i) {
		unsigned char ch = (unsigned char)input[i];
		if (isxdigit(ch)) {
			output[out_index++] = (char)toupper(ch);
		}
	}
	output[out_index] = '\0';
}

static bool extract_rfid_uid(const char *payload, char *uid, size_t uid_len)
{
	if (!payload || payload[0] == '\0') {
		return false;
	}

	if (json_get_string(payload, "uid", uid, uid_len)) {
		trim_copy(uid, uid, uid_len);
		return uid[0] != '\0';
	}

	if (json_get_string(payload, "rfid_uid", uid, uid_len)) {
		trim_copy(uid, uid, uid_len);
		return uid[0] != '\0';
	}

	trim_copy(payload, uid, uid_len);
	return uid[0] != '\0';
}

static bool rfid_uid_matches_expected(const char *received_uid)
{
	char received_norm[32];
	char expected_norm[32];
	normalize_uid(received_uid, received_norm, sizeof(received_norm));
	normalize_uid(RFID_EXPECTED_UID, expected_norm, sizeof(expected_norm));
	return received_norm[0] != '\0' && strcmp(received_norm, expected_norm) == 0;
}

static void handle_rfid_uid(const char *received_uid)
{
	if (!received_uid || received_uid[0] == '\0') {
		ESP_LOGW(TAG, "RFID payload vacío o inválido");
		return;
	}

	ESP_LOGI(TAG, "RFID recibido=%s esperado=%s", received_uid, RFID_EXPECTED_UID);

	if (rfid_uid_matches_expected(received_uid)) {
		ESP_LOGI(TAG, "RFID autorizado, moviendo servo a 0 grados");
		coap_send_post("/servo", "{\"angle\":0}");
		if (s_last_result.success && (s_last_result.http_status == 200 || s_last_result.http_status == 201)) {
			vTaskDelay(pdMS_TO_TICKS(3000));
			ESP_LOGI(TAG, "Retornando servo a 180 grados");
			coap_send_post("/servo", "{\"angle\":180}");
		} else {
			ESP_LOGW(TAG, "No se pudo mover el servo a 0 grados: %s",
					 s_last_result.error[0] ? s_last_result.error : "Bad response from CoAP server");
		}
	} else {
		ESP_LOGW(TAG, "RFID denegado, activando buzzer intermitente");
		coap_send_post("/buzzer", "{\"duration\":1000,\"mode\":1,\"frequency\":1000}");
	}
}

static void rfid_action_task(void *arg)
{
	(void)arg;

	char uid[64] = {0};
	for (;;) {
		if (xQueueReceive(s_rfid_queue, uid, portMAX_DELAY) == pdTRUE) {
			handle_rfid_uid(uid);
		}
	}
}

static void mqtt_rfid_message_handler(const char *topic, const char *payload, void *user_ctx)
{
	(void)user_ctx;
	if (!topic || !payload || !s_rfid_queue) {
		return;
	}

	if (strcmp(topic, "seguridad/rfid") != 0) {
		return;
	}

	char uid[64] = {0};
	if (!extract_rfid_uid(payload, uid, sizeof(uid))) {
		ESP_LOGW(TAG, "No se pudo extraer UID desde payload RFID: %s", payload);
		return;
	}

	if (xQueueSend(s_rfid_queue, uid, 0) != pdTRUE) {
		ESP_LOGW(TAG, "Cola RFID llena, descartando UID=%s", uid);
	}
}

static bool validate_get_response(const char *resource, const char *payload)
{
	if (!payload || payload[0] == '\0') {
		return false;
	}

	if (strcmp(resource, "/temp") == 0) {
		float temp = 0.0f;
		float humidity = 0.0f;
		return json_get_float(payload, "temp", &temp) && json_get_float(payload, "humidity", &humidity);
	}

	if (strcmp(resource, "/dist") == 0) {
		float distance = 0.0f;
		return json_get_float(payload, "distance", &distance);
	}

	return false;
}

static void led_timer_callback(TimerHandle_t timer)
{
	(void)timer;
	if (!s_led_blink_active) {
		return;
	}
	s_led_blink_phase_on = !s_led_blink_phase_on;
	leds_apply_state();
}

static void leds_init(void)
{
	gpio_config_t io_conf = {
		.pin_bit_mask = (1ULL << LED_RED_GPIO) | (1ULL << LED_BLUE_GPIO),
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	ESP_ERROR_CHECK(gpio_config(&io_conf));
	gpio_set_level(LED_RED_GPIO, 0);
	gpio_set_level(LED_BLUE_GPIO, 0);
	s_led_timer = xTimerCreate("led_blink", pdMS_TO_TICKS(100), pdTRUE, NULL, led_timer_callback);
}

static void leds_apply_state(void)
{
	int red_level = 0;
	int blue_level = 0;

	if (s_led_blink_active) {
		red_level = s_led_blink_phase_on ? 1 : 0;
		blue_level = s_led_blink_phase_on ? 1 : 0;
	} else {
		red_level = s_led_red_forced ? 1 : 0;
		blue_level = s_led_blue_forced ? 1 : 0;
	}

	gpio_set_level(LED_RED_GPIO, red_level);
	gpio_set_level(LED_BLUE_GPIO, blue_level);
}

void led_red_on(void)
{
	s_led_red_forced = true;
	s_led_blue_forced = false;
	s_led_blink_active = false;
	if (s_led_timer) {
		xTimerStop(s_led_timer, 0);
	}
	leds_apply_state();
}

void led_blue_on(void)
{
	s_led_red_forced = false;
	s_led_blue_forced = true;
	s_led_blink_active = false;
	if (s_led_timer) {
		xTimerStop(s_led_timer, 0);
	}
	leds_apply_state();
}

void led_both_blink_start(void)
{
	s_led_blink_active = true;
	s_led_blink_phase_on = true;
	leds_apply_state();
	if (s_led_timer) {
		xTimerStop(s_led_timer, 0);
		xTimerStart(s_led_timer, 0);
	}
}

void led_both_blink_stop(void)
{
	s_led_blink_active = false;
	if (s_led_timer) {
		xTimerStop(s_led_timer, 0);
	}
	leds_apply_state();
}

void leds_off(void)
{
	s_led_red_forced = false;
	s_led_blue_forced = false;
	s_led_blink_active = false;
	s_led_blink_phase_on = false;
	if (s_led_timer) {
		xTimerStop(s_led_timer, 0);
	}
	gpio_set_level(LED_RED_GPIO, 0);
	gpio_set_level(LED_BLUE_GPIO, 0);
}

static void coap_result_reset(const char *request_payload)
{
	memset(&s_last_result, 0, sizeof(s_last_result));
	s_last_result.http_status = 502;
	if (request_payload) {
		strlcpy(s_last_result.request_payload, request_payload, sizeof(s_last_result.request_payload));
	}
}

static void coap_result_set_error(int http_status, const char *message)
{
	s_last_result.completed = true;
	s_last_result.success = false;
	s_last_result.http_status = http_status;
	strlcpy(s_last_result.error, message, sizeof(s_last_result.error));
}

static coap_response_t coap_message_handler(coap_session_t *session,
											const coap_pdu_t *sent,
											const coap_pdu_t *received,
											const coap_mid_t mid)
{
	(void)session;
	(void)sent;
	(void)mid;

	if (!received) {
		coap_result_set_error(502, "Bad response from CoAP server");
		return COAP_RESPONSE_OK;
	}

	s_last_result.coap_code = coap_pdu_get_code(received);

	if (COAP_RESPONSE_CLASS(s_last_result.coap_code) != 2) {
		coap_result_set_error(502, "Bad response from CoAP server");
		return COAP_RESPONSE_OK;
	}

	const uint8_t *payload = NULL;
	size_t payload_len = 0;

	if (coap_get_data(received, &payload_len, &payload) && payload && payload_len > 0) {
		size_t copy_len = payload_len;
		if (copy_len >= sizeof(s_last_result.response_payload)) {
			copy_len = sizeof(s_last_result.response_payload) - 1;
		}
		memcpy(s_last_result.response_payload, payload, copy_len);
		s_last_result.response_payload[copy_len] = '\0';
	} else {
		s_last_result.response_payload[0] = '\0';
	}

	s_last_result.completed = true;
	s_last_result.success = true;
	return COAP_RESPONSE_OK;
}

static esp_err_t send_json_error(httpd_req_t *req, int status, const char *message)
{
	char json[192];
	snprintf(json, sizeof(json), "{\"error\":\"%s\"}", message);

	httpd_resp_set_type(req, "application/json");
	if (status == 201) {
		httpd_resp_set_status(req, "201 Created");
	} else if (status == 400) {
		httpd_resp_set_status(req, "400 Bad Request");
	} else if (status == 503) {
		httpd_resp_set_status(req, "503 Service Unavailable");
	} else if (status == 502) {
		httpd_resp_set_status(req, "502 Bad Gateway");
	} else if (status == 504) {
		httpd_resp_set_status(req, "504 Gateway Timeout");
	} else {
		httpd_resp_set_status(req, "500 Internal Server Error");
	}

	return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_json_ok_raw(httpd_req_t *req, int status, const char *data_json)
{
	char json[COAP_RESPONSE_BUFFER_SIZE + 160];
	if (!data_json || data_json[0] == '\0') {
		return ESP_FAIL;
	}

	snprintf(json, sizeof(json), "{\"status\":\"ok\",\"data\":%s}", data_json);

	httpd_resp_set_type(req, "application/json");
	if (status == 201) {
		httpd_resp_set_status(req, "201 Created");
	} else {
		httpd_resp_set_status(req, "200 OK");
	}

	return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t coap_request_common(const char *resource,
									 coap_pdu_type_t pdu_type,
									 coap_pdu_code_t method,
									 const char *payload_json,
									 bool expect_response_json,
									 bool is_post)
{
	esp_err_t final_err = ESP_FAIL;
	(void)expect_response_json;

	if (!s_coap_mutex) {
		s_coap_mutex = xSemaphoreCreateMutex();
	}
	if (!s_coap_mutex) {
		coap_result_set_error(500, "Internal error");
		return ESP_FAIL;
	}

	if (xSemaphoreTake(s_coap_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
		coap_result_set_error(500, "Internal error");
		return ESP_FAIL;
	}

	coap_result_reset(payload_json);

	for (int attempt = 0; attempt <= COAP_MAX_RETRIES; ++attempt) {
		coap_context_t *ctx = NULL;
		coap_session_t *session = NULL;
		coap_address_t dst_addr;
		coap_pdu_t *request = NULL;
		bool timed_out = false;
		bool resolved = false;

		if (!s_coap_started) {
			coap_startup();
			s_coap_started = true;
		}

		ctx = coap_new_context(NULL);
		if (!ctx) {
			coap_result_set_error(502, "Bad response from CoAP server");
			break;
		}

		coap_context_set_block_mode(ctx, COAP_BLOCK_USE_LIBCOAP | COAP_BLOCK_SINGLE_BODY);
		coap_register_response_handler(ctx, coap_message_handler);

		coap_address_init(&dst_addr);
		dst_addr.size = sizeof(dst_addr.addr.sin);
		dst_addr.addr.sin.sin_family = AF_INET;
		dst_addr.addr.sin.sin_port = htons(COAP_SERVER_PORT);
		if (inet_pton(AF_INET, COAP_SERVER_IP, &dst_addr.addr.sin.sin_addr) != 1) {
			coap_result_set_error(502, "Bad response from CoAP server");
			coap_free_context(ctx);
			break;
		}

		session = coap_new_client_session(ctx, NULL, &dst_addr, COAP_PROTO_UDP);
		if (!session) {
			coap_result_set_error(502, "Bad response from CoAP server");
			coap_free_context(ctx);
			break;
		}

		request = coap_new_pdu(pdu_type, method, session);
		if (!request) {
			coap_result_set_error(502, "Bad response from CoAP server");
			coap_session_release(session);
			coap_free_context(ctx);
			break;
		}

		unsigned char token[8];
		size_t token_length = sizeof(token);
		coap_session_new_token(session, &token_length, token);
		coap_add_token(request, token_length, token);

		const char *path = resource[0] == '/' ? resource + 1 : resource;
		coap_add_option(request, COAP_OPTION_URI_PATH, strlen(path), (const uint8_t *)path);

		if (is_post) {
			unsigned char buf[4];
			coap_add_option(request,
							COAP_OPTION_CONTENT_FORMAT,
							coap_encode_var_safe(buf, sizeof(buf), COAP_MEDIATYPE_APPLICATION_JSON),
							buf);
			if (payload_json && payload_json[0] != '\0') {
				coap_add_data_large_request(session,
											request,
											strlen(payload_json),
											(const uint8_t *)payload_json,
											NULL,
											NULL);
			}
		}

		coap_send(session, request);
		led_both_blink_start();

		int remaining_ms = COAP_REQUEST_TIMEOUT_MS;
		while (!s_last_result.completed && remaining_ms > 0) {
			int step_ms = remaining_ms > 1000 ? 1000 : remaining_ms;
			int rc = coap_io_process(ctx, step_ms);
			if (rc < 0) {
				break;
			}
			if (s_last_result.completed) {
				break;
			}
			if (rc == 0) {
				remaining_ms -= step_ms;
			} else if (rc < step_ms) {
				remaining_ms -= rc;
			} else {
				remaining_ms = 0;
			}
		}

		led_both_blink_stop();
		leds_off();

		if (s_last_result.completed && s_last_result.success) {
			resolved = true;
		} else if (!s_last_result.completed) {
			timed_out = true;
		}

		coap_session_release(session);
		coap_free_context(ctx);

		if (resolved) {
			if (!is_post) {
				if (!validate_get_response(resource, s_last_result.response_payload)) {
					coap_result_set_error(502, "Bad response from CoAP server");
					break;
				}
				s_last_result.http_status = 200;
				final_err = ESP_OK;
				break;
			}
			s_last_result.http_status = (s_last_result.coap_code == COAP_RESPONSE_CODE_CREATED) ? 201 : 200;
			final_err = ESP_OK;
			break;
		}

		if (!timed_out) {
			break;
		}

		if (attempt == COAP_MAX_RETRIES) {
			coap_result_set_error(504, "CoAP timeout");
			break;
		}
	}

	xSemaphoreGive(s_coap_mutex);
	return final_err;
}

static void coap_send_get(const char *resource)
{
	(void)coap_request_common(resource, COAP_MESSAGE_NON, COAP_REQUEST_GET, NULL, true, false);
}

static void coap_send_post(const char *resource, const char *payload_json)
{
	(void)coap_request_common(resource, COAP_MESSAGE_CON, COAP_REQUEST_POST, payload_json, true, true);
}

static char *read_request_body(httpd_req_t *req)
{
	const size_t total_len = req->content_len;
	if (total_len == 0 || total_len > 1024) {
		return NULL;
	}

	char *buffer = calloc(1, total_len + 1);
	if (!buffer) {
		return NULL;
	}

	size_t received = 0;
	while (received < total_len) {
		int r = httpd_req_recv(req, buffer + received, total_len - received);
		if (r <= 0) {
			free(buffer);
			return NULL;
		}
		received += (size_t)r;
	}

	buffer[total_len] = '\0';
	return buffer;
}

static char *build_servo_payload(const char *body)
{
	int angle = 0;
	if (!json_get_int(body, "angle", &angle) || angle < 0 || angle > 180) {
		return NULL;
	}

	char *json = malloc(32);
	if (!json) {
		return NULL;
	}
	snprintf(json, 32, "{\"angle\":%d}", angle);
	return json;
}

static char *build_buzzer_payload(const char *body)
{
	int duration = 0;
	int mode = 0;
	int frequency = 1000;

	/* Accept either "duration_ms" (older name) or "duration" (used by tests) */
	if (!json_get_int(body, "duration_ms", &duration)) {
		if (!json_get_int(body, "duration", &duration)) {
			return NULL;
		}
	}
	if (duration <= 0) {
		return NULL;
	}

	char mode_str[24];
	if (json_get_string(body, "mode", mode_str, sizeof(mode_str))) {
		if (strcmp(mode_str, "continuous") == 0) {
			mode = 0;
		} else if (strcmp(mode_str, "intermittent") == 0) {
			mode = 1;
		} else if (strcmp(mode_str, "0") == 0) {
			mode = 0;
		} else if (strcmp(mode_str, "1") == 0) {
			mode = 1;
		} else {
			/* unknown string -> default to continuous */
			mode = 0;
		}
	} else {
		/* If numeric mode provided, accept 0 or 1. If missing or invalid, default to 0 */
		int parsed_mode = 0;
		if (json_get_int(body, "mode", &parsed_mode)) {
			if (parsed_mode == 0 || parsed_mode == 1) {
				mode = parsed_mode;
			} else {
				mode = (parsed_mode != 0) ? 1 : 0;
			}
		} else {
			mode = 0; /* default */
		}
	}

	int parsed_freq = 0;
	if (!json_get_int(body, "frequency", &parsed_freq)) {
		/* accept alternate key */
		json_get_int(body, "frequency_hz", &parsed_freq);
	}
	if (parsed_freq > 0) {
		frequency = parsed_freq;
	}

	char *json = malloc(96);
	if (!json) {
		return NULL;
	}
	snprintf(json, 96, "{\"duration\":%d,\"mode\":%d,\"frequency\":%d}", duration, mode, frequency);
	return json;
}

static esp_err_t temp_get_handler(httpd_req_t *req)
{
	led_red_on();
	coap_send_get("/temp");

	if (s_last_result.success && s_last_result.http_status == 200) {
		if (!validate_get_response("/temp", s_last_result.response_payload)) {
			return send_json_error(req, 502, "Bad response from CoAP server");
		}
		return send_json_ok_raw(req, 200, s_last_result.response_payload);
	}

	return send_json_error(req, s_last_result.http_status, s_last_result.error[0] ? s_last_result.error : "Bad response from CoAP server");
}

static esp_err_t dist_get_handler(httpd_req_t *req)
{
	led_red_on();
	coap_send_get("/dist");

	if (s_last_result.success && s_last_result.http_status == 200) {
		if (!validate_get_response("/dist", s_last_result.response_payload)) {
			return send_json_error(req, 502, "Bad response from CoAP server");
		}
		return send_json_ok_raw(req, 200, s_last_result.response_payload);
	}

	return send_json_error(req, s_last_result.http_status, s_last_result.error[0] ? s_last_result.error : "Bad response from CoAP server");
}

static esp_err_t servo_post_handler(httpd_req_t *req)
{
	char *body = read_request_body(req);
	if (!body) {
		return send_json_error(req, 400, "Invalid JSON body");
	}

	char *payload = build_servo_payload(body);
	free(body);
	if (!payload) {
		return send_json_error(req, 400, "Invalid servo payload");
	}

	led_blue_on();
	coap_send_post("/servo", payload);

	esp_err_t result;
	if (s_last_result.success && (s_last_result.http_status == 200 || s_last_result.http_status == 201)) {
		char data_json[COAP_RESPONSE_BUFFER_SIZE + 220];
		if (s_last_result.response_payload[0] != '\0' && s_last_result.response_payload[0] == '{') {
			snprintf(data_json,
					 sizeof(data_json),
					 "{\"resource\":\"/servo\",\"coap_code\":%d,\"request\":%s,\"response\":%s}",
					 (int)s_last_result.coap_code,
					 payload,
					 s_last_result.response_payload);
		} else {
			snprintf(data_json,
					 sizeof(data_json),
					 "{\"resource\":\"/servo\",\"coap_code\":%d,\"request\":%s}",
					 (int)s_last_result.coap_code,
					 payload);
		}
		result = send_json_ok_raw(req, s_last_result.http_status, data_json);
	} else {
		result = send_json_error(req, s_last_result.http_status, s_last_result.error[0] ? s_last_result.error : "Bad response from CoAP server");
	}

	free(payload);
	return result;
}

static esp_err_t buzzer_post_handler(httpd_req_t *req)
{
	char *body = read_request_body(req);
	if (!body) {
		return send_json_error(req, 400, "Invalid JSON body");
	}

	char *payload = build_buzzer_payload(body);
	free(body);
	if (!payload) {
		return send_json_error(req, 400, "Invalid buzzer payload");
	}

	led_blue_on();
	coap_send_post("/buzzer", payload);

	esp_err_t result;
	if (s_last_result.success && (s_last_result.http_status == 200 || s_last_result.http_status == 201)) {
		char data_json[COAP_RESPONSE_BUFFER_SIZE + 220];
		if (s_last_result.response_payload[0] != '\0' && s_last_result.response_payload[0] == '{') {
			snprintf(data_json,
					 sizeof(data_json),
					 "{\"resource\":\"/buzzer\",\"coap_code\":%d,\"request\":%s,\"response\":%s}",
					 (int)s_last_result.coap_code,
					 payload,
					 s_last_result.response_payload);
		} else {
			snprintf(data_json,
					 sizeof(data_json),
					 "{\"resource\":\"/buzzer\",\"coap_code\":%d,\"request\":%s}",
					 (int)s_last_result.coap_code,
					 payload);
		}
		result = send_json_ok_raw(req, s_last_result.http_status, data_json);
	} else {
		result = send_json_error(req, s_last_result.http_status, s_last_result.error[0] ? s_last_result.error : "Bad response from CoAP server");
	}

	free(payload);
	return result;
}

static bool build_mqtt_publish_request(const char *body, char *topic, size_t topic_len, char *payload, size_t payload_len)
{
	if (!body || body[0] == '\0') {
		return false;
	}

	if (json_get_string(body, "payload", payload, payload_len)) {
		if (!json_get_string(body, "topic", topic, topic_len)) {
			strlcpy(topic, "gateway/comandos", topic_len);
		}
		return true;
	}

	strlcpy(topic, "gateway/comandos", topic_len);
	strlcpy(payload, body, payload_len);
	return true;
}

static esp_err_t mqtt_post_handler(httpd_req_t *req)
{
	char *body = read_request_body(req);
	if (!body) {
		return send_json_error(req, 400, "Invalid JSON body");
	}

	char topic[128] = {0};
	char payload[256] = {0};
	bool publish_request_ok = build_mqtt_publish_request(body, topic, sizeof(topic), payload, sizeof(payload));
	free(body);
	if (!publish_request_ok) {
		return send_json_error(req, 400, "Invalid MQTT payload");
	}

	if (!gateway_mqtt_bridge_publish(topic, payload, 1, false, pdMS_TO_TICKS(20))) {
		return send_json_error(req, 503, "MQTT bridge unavailable");
	}

	char data_json[192];
	snprintf(data_json, sizeof(data_json), "{\"route\":\"mqtt\",\"topic\":\"%s\",\"queued\":true}", topic);
	return send_json_ok_raw(req, 200, data_json);
}

void start_http_server(void)
{
	if (s_http_server) {
		return;
	}

	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.stack_size = 8192;
	config.uri_match_fn = httpd_uri_match_wildcard;

	ESP_ERROR_CHECK(httpd_start(&s_http_server, &config));

	httpd_uri_t temp_uri = {
		.uri = "/temp",
		.method = HTTP_GET,
		.handler = temp_get_handler,
		.user_ctx = NULL,
	};
	httpd_uri_t dist_uri = {
		.uri = "/dist",
		.method = HTTP_GET,
		.handler = dist_get_handler,
		.user_ctx = NULL,
	};
	httpd_uri_t servo_uri = {
		.uri = "/servo",
		.method = HTTP_POST,
		.handler = servo_post_handler,
		.user_ctx = NULL,
	};
	httpd_uri_t buzzer_uri = {
		.uri = "/buzzer",
		.method = HTTP_POST,
		.handler = buzzer_post_handler,
		.user_ctx = NULL,
	};
	httpd_uri_t mqtt_uri = {
		.uri = "/mqtt",
		.method = HTTP_POST,
		.handler = mqtt_post_handler,
		.user_ctx = NULL,
	};

	ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &temp_uri));
	ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &dist_uri));
	ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &servo_uri));
	ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &buzzer_uri));
	ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &mqtt_uri));
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
	(void)arg;
	(void)event_data;

	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
		ESP_ERROR_CHECK(esp_wifi_connect());
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		if (s_wifi_retry_num < 10) {
			ESP_ERROR_CHECK(esp_wifi_connect());
			s_wifi_retry_num++;
		} else if (s_wifi_event_group) {
			xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
		}
		ESP_LOGW(TAG, "Retrying Wi-Fi connection");
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
		ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
		s_wifi_retry_num = 0;
		if (s_wifi_event_group) {
			xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
		}
	}
}

static esp_err_t wifi_init_sta(void)
{

	s_wifi_event_group = xEventGroupCreate();
	if (!s_wifi_event_group) {
		return ESP_FAIL;
	}

	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_create_default_wifi_sta();

	#ifdef __INTELLISENSE__
	wifi_init_config_t cfg = { 0 };
	cfg.magic = WIFI_INIT_CONFIG_MAGIC;
	#else
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	#endif
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
														ESP_EVENT_ANY_ID,
														&wifi_event_handler,
														NULL,
														NULL));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
														IP_EVENT_STA_GOT_IP,
														&wifi_event_handler,
														NULL,
														NULL));

	wifi_config_t wifi_config = { 0 };
	strlcpy((char *)wifi_config.sta.ssid, "CHRISTIAN_8689", sizeof(wifi_config.sta.ssid));
	strlcpy((char *)wifi_config.sta.password, "redes2026", sizeof(wifi_config.sta.password));
	wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
	wifi_config.sta.pmf_cfg.capable = true;
	wifi_config.sta.pmf_cfg.required = false;

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_start());

	EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
										   WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
										   pdFALSE,
										   pdFALSE,
										   portMAX_DELAY);
	if (bits & WIFI_CONNECTED_BIT) {
		ESP_LOGI(TAG, "Wi-Fi connected");
		return ESP_OK;
	}

	ESP_LOGE(TAG, "Wi-Fi connection failed");
	return ESP_FAIL;
}

void app_main(void)
{
	esp_err_t mqtt_err;
	gateway_mqtt_bridge_config_t mqtt_cfg = GATEWAY_MQTT_BRIDGE_DEFAULT_CONFIG();
	mqtt_cfg.client_id = "esp32wroom32_gateway";

	ESP_ERROR_CHECK(nvs_flash_init());
	leds_init();
	ESP_ERROR_CHECK(wifi_init_sta());
	s_rfid_queue = xQueueCreate(RFID_QUEUE_LENGTH, 64);
	if (!s_rfid_queue) {
		ESP_LOGE(TAG, "No se pudo crear la cola RFID");
	} else if (xTaskCreate(rfid_action_task, "rfid_action_task", 4096, NULL, 5, &s_rfid_task) != pdPASS) {
		ESP_LOGE(TAG, "No se pudo crear la tarea RFID");
	}
	gateway_mqtt_bridge_register_message_callback(mqtt_rfid_message_handler, NULL);
	mqtt_err = gateway_mqtt_bridge_init(&mqtt_cfg);
	if (mqtt_err != ESP_OK) {
		ESP_LOGE(TAG, "MQTT bridge init failed: %s", esp_err_to_name(mqtt_err));
	}
	start_http_server();
	ESP_LOGI(TAG, "HTTP bridge ready. Use GET /temp, GET /dist, POST /servo, POST /buzzer, POST /mqtt");
}

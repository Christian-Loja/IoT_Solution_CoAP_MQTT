/* ============================================================
 *  main.c — ESP32-C6 Servidor CoAP Multiperféricos
 *  Práctica 4 — Redes de Telecomunicaciones (2026)
 *  Framework: ESP-IDF v6.0   Protocolo: CoAP/UDP RFC 7252
 * ============================================================
 *  Sensores/Actuadores:
 *    DHT22       → Temperatura/Humedad        (GPIO5)
 *    HC-SR04     → Distancia ultrasónica      (GPIO6 Trigger, GPIO7 Echo)
 *    Buzzer      → Sonido (buzzer pasivo)     (GPIO8 PWM)
 *    Servo SG90  → Posicionamiento angular    (GPIO9 PWM)
 *
 *  Recursos CoAP expuestos (servidor UDP:5683):
 *    GET  /temp   → NoN (No confirmable)
 *    GET  /dist   → NoN (No confirmable)
 *    POST /buzzer → CoN (Confirmable)
 *    POST /servo  → CoN (Confirmable)
 * ============================================================ */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

/* libcoap includes */
#include "coap3/coap.h"

/* Drivers locales */
#include "config.h"
#include "dht22.h"
#include "hc_sr04.h"
#include "actuators.h"

static const char *TAG = "CoAP_Server";

/* ─── Wi-Fi ──────────────────────────────────────────────── */
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_num = 0;

/* ─── Datos de sensores (acceso desde múltiples tareas) ──── */
static volatile float  g_temperatura = 0.0f;
static volatile float  g_humedad = 0.0f;
static volatile float  g_distancia = 0.0f;
static volatile uint8_t g_sensor_status = 0;  /* Bit 0: DHT22 OK, Bit 1: HC-SR04 OK */

/* Mutex para acceso seguro a datos */
#include "freertos/semphr.h"
static SemaphoreHandle_t sensor_data_mutex;

/* ════════════════════════════════════════════════════════════
 *  SECCIÓN: Wi-Fi
 * ════════════════════════════════════════════════════════════ */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *event_data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "WiFi iniciando...");

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        gpio_set_level(PIN_LED_STATUS, 0);
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "Reconectando WiFi (%d/%d)...", s_retry_num, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "WiFi conexión fallida - reintentos máximos alcanzados");
        }

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "✓ WiFi OK — IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry_num = 0;
        gpio_set_level(PIN_LED_STATUS, 1);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t h_wifi, h_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &h_wifi));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, &h_ip));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi conectado exitosamente");
    } else {
        ESP_LOGE(TAG, "Error de WiFi");
    }
}

/* ════════════════════════════════════════════════════════════
 *  SECCIÓN: Lectura de Sensores (Tarea periódica)
 * ════════════════════════════════════════════════════════════ */

static void sensor_read_task(void *arg)
{
    dht22_data_t dht22_data;
    float distance;
    int err;
    
    /* Dar tiempo a WiFi para conectar */
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    
    while (1) {
        /* Leer DHT22 */
        err = dht22_read(PIN_DHT22, &dht22_data);
        if (err == 0) {
            xSemaphoreTake(sensor_data_mutex, portMAX_DELAY);
            g_temperatura = dht22_data.temperature;
            g_humedad = dht22_data.humidity;
            g_sensor_status |= 0x01;  /* Bit 0: DHT22 OK */
            xSemaphoreGive(sensor_data_mutex);
            ESP_LOGI(TAG, "DHT22 ✓ — T:%.1f°C H:%.1f%%", g_temperatura, g_humedad);
        } else {
            xSemaphoreTake(sensor_data_mutex, portMAX_DELAY);
            g_sensor_status &= ~0x01;  /* Limpiar bit DHT22 */
            xSemaphoreGive(sensor_data_mutex);
            ESP_LOGW(TAG, "DHT22 ✗ — Error lectura");
        }
        
        vTaskDelay(500 / portTICK_PERIOD_MS);
        
        /* Leer HC-SR04 */
        err = hc_sr04_read(PIN_HC_TRIGGER, PIN_HC_ECHO, &distance);
        if (err == 0) {
            xSemaphoreTake(sensor_data_mutex, portMAX_DELAY);
            g_distancia = distance;
            g_sensor_status |= 0x02;  /* Bit 1: HC-SR04 OK */
            xSemaphoreGive(sensor_data_mutex);
            ESP_LOGI(TAG, "HC-SR04 ✓ — D:%.2f cm", g_distancia);
        } else {
            xSemaphoreTake(sensor_data_mutex, portMAX_DELAY);
            g_sensor_status &= ~0x02;  /* Limpiar bit HC-SR04 */
            xSemaphoreGive(sensor_data_mutex);
            ESP_LOGW(TAG, "HC-SR04 ✗ — Error lectura");
        }
        
        vTaskDelay(DHT22_READ_INTERVAL_MS / portTICK_PERIOD_MS);
    }
}

/* ════════════════════════════════════════════════════════════
 *  SECCIÓN: Manejadores de Recursos CoAP
 * ════════════════════════════════════════════════════════════ */

/**
 * @brief Maneja recurso GET /temp (NoN - No confirmable)
 * Devuelve: {"temp":XX.X, "humidity":XX.X}
 */
static void coap_resource_temp(coap_resource_t *resource,
                                coap_session_t *session,
                                const coap_pdu_t *request,
                                const coap_string_t *query,
                                coap_pdu_t *response)
{
    unsigned char buf[128];
    int json_len;
    
    xSemaphoreTake(sensor_data_mutex, portMAX_DELAY);
    uint8_t status = g_sensor_status;
    float temp = g_temperatura;
    float humid = g_humedad;
    xSemaphoreGive(sensor_data_mutex);
    
    if (!(status & 0x01)) {
        /* DHT22 sin datos */
        const char *error = "{\"error\":\"DHT22 sensor not ready\"}";
        size_t error_len = strlen(error);
        coap_pdu_set_type(response, COAP_MESSAGE_NON);
        coap_pdu_set_code(response, COAP_RESPONSE_CODE(500));
        coap_add_data(response, error_len, (const uint8_t *)error);
        return;
    }
    
    /* Generar JSON response */
    json_len = snprintf((char*)buf, sizeof(buf), 
                        "{\"temp\":%.1f,\"humidity\":%.1f}", temp, humid);
    
    if (json_len < 0 || (size_t)json_len >= sizeof(buf)) {
        coap_pdu_set_type(response, COAP_MESSAGE_NON);
        coap_pdu_set_code(response, COAP_RESPONSE_CODE(500));
        return;
    }
    
    coap_add_data(response, (size_t)json_len, (const uint8_t *)buf);
    coap_pdu_set_type(response, COAP_MESSAGE_NON);
    coap_pdu_set_code(response, COAP_RESPONSE_CODE(205));  /* 2.05 Content */
    
    ESP_LOGI(TAG, "CoAP GET /temp — Enviado NoN");
}

/**
 * @brief Maneja recurso GET /dist (NoN - No confirmable)
 * Devuelve: {"distance":XX.XX}
 */
static void coap_resource_dist(coap_resource_t *resource,
                                coap_session_t *session,
                                const coap_pdu_t *request,
                                const coap_string_t *query,
                                coap_pdu_t *response)
{
    unsigned char buf[128];
    int json_len;
    
    xSemaphoreTake(sensor_data_mutex, portMAX_DELAY);
    uint8_t status = g_sensor_status;
    float dist = g_distancia;
    xSemaphoreGive(sensor_data_mutex);
    
    if (!(status & 0x02)) {
        /* HC-SR04 sin datos */
        const char *error = "{\"error\":\"HC-SR04 sensor not ready\"}";
        size_t error_len = strlen(error);
        coap_pdu_set_type(response, COAP_MESSAGE_NON);
        coap_pdu_set_code(response, COAP_RESPONSE_CODE(500));
        coap_add_data(response, error_len, (const uint8_t *)error);
        return;
    }
    
    /* Generar JSON response */
    json_len = snprintf((char*)buf, sizeof(buf), 
                        "{\"distance\":%.2f}", dist);
    
    if (json_len < 0 || (size_t)json_len >= sizeof(buf)) {
        coap_pdu_set_type(response, COAP_MESSAGE_NON);
        coap_pdu_set_code(response, COAP_RESPONSE_CODE(500));
        return;
    }
    
    coap_add_data(response, (size_t)json_len, (const uint8_t *)buf);
    coap_pdu_set_type(response, COAP_MESSAGE_NON);
    coap_pdu_set_code(response, COAP_RESPONSE_CODE(205));  /* 2.05 Content */
    
    ESP_LOGI(TAG, "CoAP GET /dist — Enviado NoN");
}

/**
 * @brief Maneja recurso POST /servo (CoN - Confirmable)
 * Body esperado: {"angle":XXX} donde XXX es 0-180
 * Respuesta: {"status":"ok"} o {"error":"..."}
 */
static void coap_resource_servo(coap_resource_t *resource,
                                 coap_session_t *session,
                                 const coap_pdu_t *request,
                                 const coap_string_t *query,
                                 coap_pdu_t *response)
{
    unsigned char buf[256];
    size_t len;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    
    /* Obtener payload */
    if (!coap_get_data(request, &payload_len, &payload) || payload_len == 0 || payload == NULL) {
        const char *error = "{\"error\":\"No payload\"}";
        len = strlen(error);
        coap_pdu_set_code(response, COAP_RESPONSE_CODE(400));  /* Bad Request */
        coap_add_data(response, len, (const uint8_t *)error);
        goto response_done;
    }
    
    char payload_copy[257];
    size_t copy_len = payload_len < sizeof(payload_copy) - 1 ? payload_len : sizeof(payload_copy) - 1;
    memcpy(payload_copy, payload, copy_len);
    payload_copy[copy_len] = '\0';
    
    /* Extraer ángulo de JSON simple {"angle":XXX} */
    int angle = -1;
    if (sscanf(payload_copy, "{\"angle\":%d}", &angle) != 1) {
        const char *error = "{\"error\":\"Invalid JSON format\"}";
        len = strlen(error);
        coap_pdu_set_code(response, COAP_RESPONSE_CODE(400));
        coap_add_data(response, len, (const uint8_t *)error);
        goto response_done;
    }
    
    /* Validar rango */
    if (angle < 0 || angle > 180) {
        len = snprintf((char*)buf, sizeof(buf),
                       "{\"error\":\"Angle out of range (0-180), got %d\"}", angle);
        coap_pdu_set_code(response, COAP_RESPONSE_CODE(400));
        coap_add_data(response, (size_t)len, (const uint8_t *)buf);
        goto response_done;
    }
    
    /* Ejecutar movimiento servo */
    if (servo_move(angle) == 0) {
        len = snprintf((char*)buf, sizeof(buf),
                       "{\"status\":\"ok\",\"angle\":%d}", angle);
        coap_pdu_set_code(response, COAP_RESPONSE_CODE(201));  /* 2.01 Created */
        ESP_LOGI(TAG, "CoAP POST /servo ✓ — Ángulo: %d°", angle);
    } else {
        const char *error = "{\"error\":\"Servo control failed\"}";
        len = strlen(error);
        coap_pdu_set_code(response, COAP_RESPONSE_CODE(500));
        ESP_LOGE(TAG, "CoAP POST /servo ✗ — Error en servo");
    }
    
    coap_add_data(response, len, (const uint8_t *)buf);

response_done:
    ESP_LOGI(TAG, "CoAP POST /servo — Enviado CoN");
}

/**
 * @brief Maneja recurso POST /buzzer (CoN - Confirmable)
 * Body esperado: {"duration":XXX, "mode":Y, "frequency":ZZZZ}
 * mode: 0 = continuo, 1 = intermitente
 * Respuesta: {"status":"ok"} o {"error":"..."}
 */
static void coap_resource_buzzer(coap_resource_t *resource,
                                  coap_session_t *session,
                                  const coap_pdu_t *request,
                                  const coap_string_t *query,
                                  coap_pdu_t *response)
{
    unsigned char buf[256];
    size_t len;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    
    int duration = 0, mode = 0, frequency = 1000;
    
    /* Obtener payload */
    if (!coap_get_data(request, &payload_len, &payload) || payload_len == 0 || payload == NULL) {
        const char *error = "{\"error\":\"No payload\"}";
        len = strlen(error);
        coap_pdu_set_code(response, COAP_RESPONSE_CODE(400));
        coap_add_data(response, len, (const uint8_t *)error);
        goto response_done;
    }
    
    char payload_copy[257];
    size_t copy_len = payload_len < sizeof(payload_copy) - 1 ? payload_len : sizeof(payload_copy) - 1;
    memcpy(payload_copy, payload, copy_len);
    payload_copy[copy_len] = '\0';
    
    /* Extraer parámetros de JSON (permitir espacios alrededor de :) */
    int parse_ok = 0;
    
    /* Intentar con espacios primero (json.dumps() de Python añade espacios) */
    if (sscanf(payload_copy, "{\"duration\": %d, \"mode\": %d, \"frequency\": %d}",
               &duration, &mode, &frequency) == 3) {
        parse_ok = 1;
    }
    /* Si no funciona, intentar sin espacios */
    else if (sscanf(payload_copy, "{\"duration\":%d,\"mode\":%d,\"frequency\":%d}",
                    &duration, &mode, &frequency) == 3) {
        parse_ok = 1;
    }
    
    if (!parse_ok) {
        const char *error = "{\"error\":\"Invalid JSON format\"}";
        len = strlen(error);
        coap_pdu_set_code(response, COAP_RESPONSE_CODE(400));
        coap_add_data(response, len, (const uint8_t *)error);
        goto response_done;
    }
    
    /* Si no se especifica frequency, usar default */
    if (frequency == 0) frequency = BUZZER_DEFAULT_FREQ;
    
    /* Ejecutar buzzer */
    if (buzzer_play(duration, mode, frequency) == 0) {
        len = snprintf((char*)buf, sizeof(buf),
                       "{\"status\":\"ok\",\"duration\":%d,\"mode\":%d}", 
                       duration, mode);
        coap_pdu_set_code(response, COAP_RESPONSE_CODE(201));
        ESP_LOGI(TAG, "CoAP POST /buzzer ✓ — Dur: %dms, Modo: %s, Freq: %dHz",
                 duration, mode == 0 ? "cont" : "int", frequency);
    } else {
        const char *error = "{\"error\":\"Buzzer control failed\"}";
        len = strlen(error);
        coap_pdu_set_code(response, COAP_RESPONSE_CODE(500));
        ESP_LOGE(TAG, "CoAP POST /buzzer ✗ — Error");
    }
    
    coap_add_data(response, len, (const uint8_t *)buf);

response_done:
    ESP_LOGI(TAG, "CoAP POST /buzzer — Enviado CoN");
}

/* ════════════════════════════════════════════════════════════
 *  SECCIÓN: Servidor CoAP
 * ════════════════════════════════════════════════════════════ */

static coap_context_t *coap_ctx;

static void coap_server_task(void *arg)
{
    coap_address_t server_addr;
    coap_resource_t *resource;
    
    ESP_LOGI(TAG, "Iniciando servidor CoAP en puerto %d...", COAP_SERVER_PORT);
    
    /* Esperar conexión WiFi */
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, 
                        pdFALSE, pdFALSE, portMAX_DELAY);
    
    /* Inicializar contexto CoAP */
    coap_address_init(&server_addr);
    server_addr.addr.sin.sin_family = AF_INET;
    server_addr.addr.sin.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.addr.sin.sin_port = htons(COAP_SERVER_PORT);
    
    coap_ctx = coap_new_context(&server_addr);
    if (!coap_ctx) {
        ESP_LOGE(TAG, "Error creando contexto CoAP");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "✓ Contexto CoAP creado");
    
    /* Registrar recurso /temp (GET, NoN) */
    resource = coap_resource_init(coap_new_str_const((const uint8_t *)"temp", 4),
                                  COAP_RESOURCE_FLAGS_NOTIFY_CON);
    if (!resource) {
        ESP_LOGE(TAG, "Error creando recurso /temp");
        goto error;
    }
    coap_register_request_handler(resource, COAP_REQUEST_GET, 
                                  coap_resource_temp);
    coap_add_resource(coap_ctx, resource);
    ESP_LOGI(TAG, "✓ Recurso /temp registrado");
    
    /* Registrar recurso /dist (GET, NoN) */
    resource = coap_resource_init(coap_new_str_const((const uint8_t *)"dist", 4),
                                  COAP_RESOURCE_FLAGS_NOTIFY_CON);
    if (!resource) {
        ESP_LOGE(TAG, "Error creando recurso /dist");
        goto error;
    }
    coap_register_request_handler(resource, COAP_REQUEST_GET,
                                  coap_resource_dist);
    coap_add_resource(coap_ctx, resource);
    ESP_LOGI(TAG, "✓ Recurso /dist registrado");
    
    /* Registrar recurso /servo (POST, CoN) */
    resource = coap_resource_init(coap_new_str_const((const uint8_t *)"servo", 5),
                                  COAP_RESOURCE_FLAGS_NOTIFY_CON);
    if (!resource) {
        ESP_LOGE(TAG, "Error creando recurso /servo");
        goto error;
    }
    coap_register_request_handler(resource, COAP_REQUEST_POST,
                                  coap_resource_servo);
    coap_add_resource(coap_ctx, resource);
    ESP_LOGI(TAG, "✓ Recurso /servo registrado");
    
    /* Registrar recurso /buzzer (POST, CoN) */
    resource = coap_resource_init(coap_new_str_const((const uint8_t *)"buzzer", 6),
                                  COAP_RESOURCE_FLAGS_NOTIFY_CON);
    if (!resource) {
        ESP_LOGE(TAG, "Error creando recurso /buzzer");
        goto error;
    }
    coap_register_request_handler(resource, COAP_REQUEST_POST,
                                  coap_resource_buzzer);
    coap_add_resource(coap_ctx, resource);
    ESP_LOGI(TAG, "✓ Recurso /buzzer registrado");
    
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "  ✓ SERVIDOR CoAP ACTIVO EN PUERTO %d", COAP_SERVER_PORT);
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "  GET  /temp   → Temperatura y humedad (NoN)");
    ESP_LOGI(TAG, "  GET  /dist   → Distancia ultrasónica (NoN)");
    ESP_LOGI(TAG, "  POST /servo  → Control servo 0-180° (CoN)");
    ESP_LOGI(TAG, "  POST /buzzer → Control buzzer (CoN)");
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
    
    /* Loop principal del servidor */
    while (1) {
        int wait_ms = coap_io_process(coap_ctx, 1000);
        if (wait_ms < 0) {
            ESP_LOGE(TAG, "Error en coap_io_process");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms > 0 ? wait_ms : 10));
    }

error:
    if (coap_ctx) {
        coap_free_context(coap_ctx);
    }
    vTaskDelete(NULL);
}

/* ════════════════════════════════════════════════════════════
 *  SECCIÓN: Inicialización de GPIO y Periféricos
 * ════════════════════════════════════════════════════════════ */

static void peripherals_init(void)
{
    /* Inicializar LED de estado */
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << PIN_LED_STATUS,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    gpio_set_level(PIN_LED_STATUS, 0);
    
    /* Inicializar DHT22 */
    if (dht22_init(PIN_DHT22) != 0) {
        ESP_LOGE(TAG, "Error inicializando DHT22");
    } else {
        ESP_LOGI(TAG, "✓ DHT22 inicializado");
    }
    
    /* Inicializar HC-SR04 */
    if (hc_sr04_init(PIN_HC_TRIGGER, PIN_HC_ECHO) != 0) {
        ESP_LOGE(TAG, "Error inicializando HC-SR04");
    } else {
        ESP_LOGI(TAG, "✓ HC-SR04 inicializado");
    }
    
    /* Inicializar Buzzer */
    if (buzzer_init() != 0) {
        ESP_LOGE(TAG, "Error inicializando Buzzer");
    } else {
        ESP_LOGI(TAG, "✓ Buzzer inicializado");
    }
    
    /* Inicializar Servo */
    if (servo_init() != 0) {
        ESP_LOGE(TAG, "Error inicializando Servo");
    } else {
        ESP_LOGI(TAG, "✓ Servo inicializado");
    }
}

/* ════════════════════════════════════════════════════════════
 *  MAIN
 * ════════════════════════════════════════════════════════════ */

void app_main(void)
{
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "  ESP32-C6 CoAP Server — Multiperféricos");
    ESP_LOGI(TAG, "  ESP-IDF v5.x/v6.x — 2026");
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
    
    /* Inicializar NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    /* Crear mutex para datos de sensores */
    sensor_data_mutex = xSemaphoreCreateMutex();
    if (!sensor_data_mutex) {
        ESP_LOGE(TAG, "Error creando mutex de sensores");
        return;
    }
    
    /* Inicializar periféricos */
    peripherals_init();
    
    /* Inicializar Wi-Fi */
    wifi_init_sta();
    
    /* Crear tarea de lectura de sensores */
    if (xTaskCreate(sensor_read_task, "sensor_read", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Error creando tarea de sensores");
        return;
    }
    ESP_LOGI(TAG, "✓ Tarea de sensores creada");
    
    /* Crear tarea del servidor CoAP */
    if (xTaskCreate(coap_server_task, "coap_server", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Error creando servidor CoAP");
        return;
    }
    ESP_LOGI(TAG, "✓ Tarea CoAP creada");
}

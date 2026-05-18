#include "gateway_mqtt_bridge.h"

#include <string.h>

#include "esp_log.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "gateway_mqtt_bridge";

static esp_mqtt_client_handle_t s_client = NULL;
static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_task = NULL;
static volatile bool s_connected = false;
static gateway_mqtt_bridge_config_t s_cfg;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    (void)event_data;

    if (event_id == MQTT_EVENT_CONNECTED) {
        s_connected = true;
        ESP_LOGI(TAG, "Conectado al broker MQTT");
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        s_connected = false;
        ESP_LOGW(TAG, "Desconectado del broker MQTT");
    } else if (event_id == MQTT_EVENT_ERROR) {
        ESP_LOGE(TAG, "Error de MQTT");
    }
}

static void mqtt_bridge_task(void *arg)
{
    (void)arg;

    gateway_mqtt_bridge_message_t msg;

    for (;;) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) == pdTRUE) {
            while (!s_connected) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }

            if (s_client == NULL) {
                ESP_LOGW(TAG, "Cliente MQTT no inicializado");
                continue;
            }

            int msg_id = esp_mqtt_client_publish(
                s_client,
                msg.topic,
                msg.payload,
                0,
                msg.qos,
                msg.retain
            );

            if (msg_id < 0) {
                ESP_LOGW(TAG, "Fallo publicando en topic=%s", msg.topic);
            } else {
                ESP_LOGI(TAG, "Publicado topic=%s msg_id=%d", msg.topic, msg_id);
            }
        }
    }
}

esp_err_t gateway_mqtt_bridge_init(const gateway_mqtt_bridge_config_t *config)
{
    if (config == NULL || config->broker_uri == NULL || config->client_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_client != NULL) {
        return ESP_OK;
    }

    s_cfg = *config;

    if (s_cfg.queue_length == 0) {
        s_cfg.queue_length = 8;
    }
    if (s_cfg.task_stack_size == 0) {
        s_cfg.task_stack_size = 4096;
    }
    if (s_cfg.task_priority == 0) {
        s_cfg.task_priority = 5;
    }

    s_queue = xQueueCreate(s_cfg.queue_length, sizeof(gateway_mqtt_bridge_message_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_cfg.broker_uri,
        .credentials.client_id = s_cfg.client_id,
        .credentials.username = s_cfg.username,
        .credentials.authentication.password = s_cfg.password,
        .session.keepalive = 30,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == NULL) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        vQueueDelete(s_queue);
        s_queue = NULL;
        return err;
    }

    BaseType_t task_ok = xTaskCreate(
        mqtt_bridge_task,
        "mqtt_bridge_task",
        s_cfg.task_stack_size,
        NULL,
        s_cfg.task_priority,
        &s_task
    );
    if (task_ok != pdPASS) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        vQueueDelete(s_queue);
        s_queue = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Inicializado con broker %s", s_cfg.broker_uri);
    return ESP_OK;
}

void gateway_mqtt_bridge_deinit(void)
{
    if (s_task != NULL) {
        vTaskDelete(s_task);
        s_task = NULL;
    }

    if (s_client != NULL) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }

    if (s_queue != NULL) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }

    s_connected = false;
}

bool gateway_mqtt_bridge_is_connected(void)
{
    return s_connected;
}

bool gateway_mqtt_bridge_publish(const char *topic, const char *payload, int qos, bool retain, TickType_t timeout_ticks)
{
    if (topic == NULL || payload == NULL || s_queue == NULL) {
        return false;
    }

    gateway_mqtt_bridge_message_t msg = {0};
    strlcpy(msg.topic, topic, sizeof(msg.topic));
    strlcpy(msg.payload, payload, sizeof(msg.payload));
    msg.qos = qos;
    msg.retain = retain;

    return xQueueSend(s_queue, &msg, timeout_ticks) == pdTRUE;
}

const gateway_mqtt_bridge_config_t *gateway_mqtt_bridge_get_config(void)
{
    return &s_cfg;
}

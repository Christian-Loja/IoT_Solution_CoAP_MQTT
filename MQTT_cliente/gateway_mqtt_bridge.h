#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *broker_uri;
    const char *client_id;
    const char *username;
    const char *password;
    size_t queue_length;
    UBaseType_t task_priority;
    uint32_t task_stack_size;
} gateway_mqtt_bridge_config_t;

#define GATEWAY_MQTT_BRIDGE_DEFAULT_CONFIG() \
    {                                       \
        .broker_uri = "mqtt://192.168.137.143:1883", \
        .client_id = "esp32wroom32_gateway", \
        .username = NULL,                   \
        .password = NULL,                   \
        .queue_length = 8,                  \
        .task_priority = 5,                 \
        .task_stack_size = 4096,            \
    }

typedef struct {
    char topic[128];
    char payload[256];
    int qos;
    bool retain;
} gateway_mqtt_bridge_message_t;

esp_err_t gateway_mqtt_bridge_init(const gateway_mqtt_bridge_config_t *config);
void gateway_mqtt_bridge_deinit(void);
bool gateway_mqtt_bridge_is_connected(void);
bool gateway_mqtt_bridge_publish(const char *topic, const char *payload, int qos, bool retain, TickType_t timeout_ticks);
const gateway_mqtt_bridge_config_t *gateway_mqtt_bridge_get_config(void);

#ifdef __cplusplus
}
#endif

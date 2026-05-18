/*
 * Fragmento de integración para pegar en el gateway ESP32-WROOM-32.
 * No forma parte del build; sirve como guía de uso.
 */

#include "gateway_mqtt_bridge.h"

void gateway_start_services_after_wifi(void)
{
    gateway_mqtt_bridge_config_t cfg = GATEWAY_MQTT_BRIDGE_DEFAULT_CONFIG();
    cfg.client_id = "gateway_wroom32";
    ESP_ERROR_CHECK(gateway_mqtt_bridge_init(&cfg));
}

esp_err_t gateway_handle_http_to_mqtt(const char *json_body)
{
    if (!gateway_mqtt_bridge_publish("gateway/comandos", json_body, 1, false, pdMS_TO_TICKS(10))) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

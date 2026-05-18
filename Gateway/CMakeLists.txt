idf_component_register(SRCS "main.c" "gateway_mqtt_bridge.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_http_server coap nvs_flash esp_wifi esp_event esp_netif esp_driver_gpio)

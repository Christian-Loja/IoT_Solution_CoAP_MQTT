idf_component_register(SRCS "main.c" "dht22.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi esp_event esp_netif nvs_flash mqtt esp_timer
                             esp_driver_gpio esp_driver_ledc esp_driver_spi esp_driver_pcnt)

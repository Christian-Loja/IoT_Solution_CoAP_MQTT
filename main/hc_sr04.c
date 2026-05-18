/* ============================================================
 *  hc_sr04.c — Driver HC-SR04 Implementación
 *  Fórmula distancia: cm = (tiempo_us / 2) / 29.1
 * ============================================================ */

#include "hc_sr04.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "HC_SR04";

int hc_sr04_init(int trigger_pin, int echo_pin)
{
    /* Configurar TRIGGER como salida */
    gpio_config_t trigger_conf = {
        .pin_bit_mask = 1ULL << trigger_pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    
    if (gpio_config(&trigger_conf) != ESP_OK) {
        ESP_LOGE(TAG, "Error configurando TRIGGER pin %d", trigger_pin);
        return -1;
    }
    
    /* Configurar ECHO como entrada (sin pull-up/pull-down, el sensor genera pulso limpio) */
    gpio_config_t echo_conf = {
        .pin_bit_mask = 1ULL << echo_pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    
    if (gpio_config(&echo_conf) != ESP_OK) {
        ESP_LOGE(TAG, "Error configurando ECHO pin %d", echo_pin);
        return -1;
    }

    gpio_set_pull_mode(echo_pin, GPIO_FLOATING);
    
    gpio_set_level(trigger_pin, 0);
    ESP_LOGI(TAG, "HC-SR04 inicializado (TRIGGER=%d, ECHO=%d)", 
             trigger_pin, echo_pin);
    
    return 0;
}

int hc_sr04_read(int trigger_pin, int echo_pin, float *distance_cm)
{
    if (!distance_cm) return -1;
    
    /* Enviar pulso TRIGGER de 10us (típico) */
    gpio_set_level(trigger_pin, 0);
    esp_rom_delay_us(2);
    
    gpio_set_level(trigger_pin, 1);
    esp_rom_delay_us(10);  /* Pulso de 10us */
    gpio_set_level(trigger_pin, 0);
    
    /* Esperar que ECHO suba a 1 */
    int64_t start_time = esp_timer_get_time();
    int timeout_us = HC_SR04_TIMEOUT_US;
    
    while (gpio_get_level(echo_pin) == 0) {
        if ((esp_timer_get_time() - start_time) > timeout_us) {
            ESP_LOGW(TAG, "HC-SR04 timeout esperando ECHO alto");
            return -1;
        }
    }
    
    int64_t echo_start = esp_timer_get_time();
    
    /* Esperar que ECHO baje a 0 (pulso terminó) */
    while (gpio_get_level(echo_pin) == 1) {
        if ((esp_timer_get_time() - echo_start) > timeout_us) {
            ESP_LOGW(TAG, "HC-SR04 timeout leyendo duración ECHO");
            return -1;
        }
    }
    
    int64_t echo_end = esp_timer_get_time();
    int64_t echo_duration_us = echo_end - echo_start;
    
    /* Fórmula: distancia (cm) = (tiempo_us / 2) / 29.1
     * Simplificado: distancia (cm) = tiempo_us / 58.2
     */
    *distance_cm = (echo_duration_us / 58.2f);
    
    /* Validar rango razonable */
    if (*distance_cm < DISTANCE_MIN_CM || *distance_cm > DISTANCE_MAX_CM) {
        ESP_LOGW(TAG, "Distancia fuera de rango: %.2f cm (echo=%lld us)", 
                 *distance_cm, echo_duration_us);
        return -3;  /* ERR_RANGE_ERROR */
    }
    
    ESP_LOGI(TAG, "HC-SR04 OK — Distancia: %.2f cm (echo=%lld us)", 
             *distance_cm, echo_duration_us);
    
    return 0;
}

/* ============================================================
 *  dht22.c — Driver DHT22 Implementación
 *  Protocolo: DHT22 1-wire (timing crítico)
 * ============================================================ */

#include "dht22.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "DHT22";

#define DHT22_TIMEOUT_US       100000  /* Timeout total de lectura     */
#define DHT22_START_LOW_US     18000   /* Mantener bajo 18ms           */
#define DHT22_START_HIGH_US    40      /* Mantener alto 40us           */
#define DHT22_BIT_TIMEOUT_US   100     /* Timeout lectura de cada bit  */

/**
 * @brief Espera a un nivel lógico con timeout
 */
static int _wait_level(int pin, int level, int timeout_us)
{
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(pin) != level) {
        if ((esp_timer_get_time() - start) > timeout_us) {
            return -1;  /* Timeout */
        }
        asm("nop");  /* Reduce busy-waiting */
    }
    return 0;
}

int dht22_init(int pin)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT_OD,  /* Open-drain (1-wire)  */
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    
    if (gpio_config(&io_conf) != ESP_OK) {
        ESP_LOGE(TAG, "Error configurando GPIO %d", pin);
        return -1;
    }
    
    gpio_set_level(pin, 1);  /* Reposo alto */
    return 0;
}

int dht22_read(int pin, dht22_data_t *data)
{
    if (!data) return -1;
    
    uint8_t bits[5] = {0};
    int byte_idx = 0, bit_idx = 0;
    
    /* Paso 1: Enviar pulso de inicio (bajo 18ms) */
    gpio_set_level(pin, 0);
    esp_rom_delay_us(DHT22_START_LOW_US);
    
    /* Paso 2: Soltar pin (alto 40us aprox) */
    gpio_set_level(pin, 1);
    esp_rom_delay_us(DHT22_START_HIGH_US);
    
    /* Paso 3: Cambiar a entrada para lectura */
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    
    /* Paso 4: Esperar respuesta DHT (bajo ~80us) */
    if (_wait_level(pin, 0, 200) < 0) {
        ESP_LOGW(TAG, "DHT22 no respondió (esperando LOW)");
        goto cleanup;
    }
    
    /* Paso 5: Esperar transición a alto (~80us) */
    if (_wait_level(pin, 1, 200) < 0) {
        ESP_LOGW(TAG, "DHT22 timeout (esperando HIGH)");
        goto cleanup;
    }
    
    /* Paso 6: Leer 40 bits (5 bytes) */
    for (int i = 0; i < 40; i++) {
        /* Esperar transición a bajo */
        if (_wait_level(pin, 0, 100) < 0) {
            ESP_LOGW(TAG, "DHT22 timeout en bit %d (LOW)", i);
            goto cleanup;
        }
        
        /* Esperar transición a alto y medir cuánto dura el nivel alto */
        if (_wait_level(pin, 1, DHT22_BIT_TIMEOUT_US) < 0) {
            ESP_LOGW(TAG, "DHT22 timeout en bit %d (HIGH)", i);
            goto cleanup;
        }
        int64_t start = esp_timer_get_time();
        
        /* Esperar bajada (duración indica bit 0 o 1) */
        if (_wait_level(pin, 0, DHT22_BIT_TIMEOUT_US) < 0) {
            ESP_LOGW(TAG, "DHT22 timeout detectando bajada bit %d", i);
            goto cleanup;
        }
        int64_t pulse_us = esp_timer_get_time() - start;
        
        /* Duración >50us = bit 1, <50us = bit 0 */
        if (pulse_us > 50) {
            bits[byte_idx] |= (1 << (7 - bit_idx));
        }
        
        bit_idx++;
        if (bit_idx == 8) {
            bit_idx = 0;
            byte_idx++;
        }
    }
    
cleanup:
    /* Restaurar configuración GPIO */
    gpio_set_direction(pin, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(pin, 1);
    
    if (byte_idx < 5) {
        return -1;
    }
    
    /* Verificar checksum */
    uint8_t checksum = bits[0] + bits[1] + bits[2] + bits[3];
    if (checksum != bits[4]) {
        ESP_LOGW(TAG, "Checksum DHT22 incorrecto (calc=%d, recv=%d)", 
                 checksum, bits[4]);
        return -1;
    }
    
    /* Decodificar datos */
    /* bits[0] = Humedad MSB, bits[1] = Humedad LSB */
    /* bits[2] = Temp MSB, bits[3] = Temp LSB */
    uint16_t humidity_raw = (bits[0] << 8) | bits[1];
    uint16_t temp_raw = (bits[2] << 8) | bits[3];
    
    /* Temperatura negativa? */
    int sign = 1;
    if (temp_raw & 0x8000) {
        sign = -1;
        temp_raw &= 0x7FFF;
    }
    
    data->humidity = humidity_raw / 10.0f;
    data->temperature = (temp_raw / 10.0f) * sign;
    
    ESP_LOGI(TAG, "DHT22 OK — Temp: %.1f°C, Humedad: %.1f%%", 
             data->temperature, data->humidity);
    
    return 0;
}

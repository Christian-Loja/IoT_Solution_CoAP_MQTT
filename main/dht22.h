/* ============================================================
 *  dht22.h — Driver DHT22 (Temperature/Humidity Sensor)
 *  Protocolo: 1-wire (GPIO directo)
 * ============================================================ */

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Estructura con datos del sensor */
typedef struct {
    float temperature;  /* Temperatura en °C */
    float humidity;     /* Humedad en % */
} dht22_data_t;

/**
 * @brief Inicializa el pin GPIO para DHT22
 * @param pin GPIO del sensor
 * @return ESP_OK si éxito, ESP_FAIL si error
 */
int dht22_init(int pin);

/**
 * @brief Lee temperatura y humedad del DHT22
 * @param pin GPIO del sensor
 * @param data Puntero a estructura para guardar resultados
 * @return 0 si éxito, ERR_SENSOR_TIMEOUT si falla
 */
int dht22_read(int pin, dht22_data_t *data);

#ifdef __cplusplus
}
#endif

/* ============================================================
 *  hc_sr04.h — Driver HC-SR04 (Ultrasonic Distance Sensor)
 *  Protocolo: Pulsos GPIO (Trigger + Echo capture)
 * ============================================================ */

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa HC-SR04 (configura GPIOs)
 * @param trigger_pin GPIO para enviar pulso trigger
 * @param echo_pin GPIO para leer echo
 * @return 0 si éxito, -1 si error
 */
int hc_sr04_init(int trigger_pin, int echo_pin);

/**
 * @brief Lee distancia del sensor HC-SR04
 * @param trigger_pin GPIO trigger
 * @param echo_pin GPIO echo
 * @param distance_cm Puntero para guardar distancia en cm
 * @return 0 si éxito, ERR_SENSOR_TIMEOUT si falla
 */
int hc_sr04_read(int trigger_pin, int echo_pin, float *distance_cm);

#ifdef __cplusplus
}
#endif

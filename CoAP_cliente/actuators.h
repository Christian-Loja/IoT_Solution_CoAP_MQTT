/* ============================================================
 *  actuators.h — Drivers para Buzzer y Servo
 *  Ambos usan LEDC (PWM) de ESP32-C6
 * ============================================================ */

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── BUZZER ─────────────────────────────────────────────── */

typedef enum {
    BUZZER_CONTINUOUS = 0,   /* Sonido continuo */
    BUZZER_INTERMITTENT = 1, /* Intermitente (100ms on/off) */
} buzzer_mode_t;

/**
 * @brief Inicializa el buzzer (configura LEDC PWM)
 * @return 0 si éxito, -1 si error
 */
int buzzer_init(void);

/**
 * @brief Activa buzzer por duración especificada
 * @param duration_ms Duración en ms (max BUZZER_MAX_DURATION)
 * @param mode Continuo (0) o intermitente (1)
 * @param frequency_hz Frecuencia en Hz (típico 1000)
 * @return 0 si éxito, -2 si parámetro inválido
 */
int buzzer_play(uint16_t duration_ms, uint8_t mode, uint16_t frequency_hz);

/**
 * @brief Detiene buzzer inmediatamente
 * @return 0 siempre
 */
int buzzer_stop(void);

/* ─── SERVO ──────────────────────────────────────────────── */

/**
 * @brief Inicializa el servo (configura LEDC PWM 50Hz)
 * @return 0 si éxito, -1 si error
 */
int servo_init(void);

/**
 * @brief Mueve servo a ángulo específico
 * @param angle Ángulo en grados (0-180)
 * @return 0 si éxito, -2 si ángulo inválido
 */
int servo_move(int angle);

/**
 * @brief Apaga la salida PWM del servo
 * @return 0 siempre
 */
int servo_stop(void);

#ifdef __cplusplus
}
#endif

#pragma once

#include "driver/gpio.h"
#include "driver/ledc.h"

/* ============================================================
 *  config.h — ESP32-C6 CoAP Servidor Multiperféricos
 *  Práctica 4 — Redes de Telecomunicaciones (2026)
 *  Framework: ESP-IDF v5.x/v6.x   Protocolo: CoAP/UDP RFC 7252
 * ============================================================ */

/* ─── Credenciales Wi-Fi ─────────────────────────────────── */
#define WIFI_SSID         "CHRISTIAN_8689"
#define WIFI_PASSWORD     "redes2026"
#define WIFI_MAX_RETRY    10

/* ─── CoAP ───────────────────────────────────────────────── */
#define COAP_SERVER_PORT  5683   /* UDP estándar CoAP          */

/* ═══════════════════════════════════════════════════════════
 *  ASIGNACIÓN DE PINES GPIO (ESP32-C6)
 * ═══════════════════════════════════════════════════════════ */

/* Sensores de lectura */
#define PIN_DHT22         GPIO_NUM_5    /* Temperatura/Humedad (1-wire) */
#define PIN_HC_TRIGGER    GPIO_NUM_6    /* HC-SR04 Trigger              */
#define PIN_HC_ECHO       GPIO_NUM_7    /* HC-SR04 Echo (input capture) */

/* Actuadores con PWM */
#define PIN_BUZZER        GPIO_NUM_8    /* Buzzer pasivo (LEDC)         */
#define PIN_SERVO         GPIO_NUM_9    /* Servomotor SG90 (LEDC)       */

/* LED de estado */
#define PIN_LED_STATUS    GPIO_NUM_4    /* LED WiFi/CoAP status         */

/* I2C (reservado para expansiones futuras) */
#define PIN_I2C_SDA       GPIO_NUM_2    /* I2C SDA                      */
#define PIN_I2C_SCL       GPIO_NUM_3    /* I2C SCL                      */

/* ─── LEDC (PWM) ─────────────────────────────────────────── */
#define LEDC_TIMER_BIT    LEDC_TIMER_10_BIT    /* 1024 niveles    */
#define LEDC_BASE_FREQ    5000                 /* Hz base         */

/* Buzzer */
#define LEDC_BUZZER_CHANNEL   LEDC_CHANNEL_0
#define LEDC_BUZZER_FREQ      1000  /* Hz frecuencia buzzer   */

/* Servo */
#define LEDC_SERVO_CHANNEL    LEDC_CHANNEL_1
#define LEDC_SERVO_FREQ       50    /* Hz (estándar servos)   */

/* ─── Sensores ─────────────────────────────────────────────*/
#define DHT22_READ_INTERVAL_MS    2000  /* Lecturas cada 2s    */
#define HC_SR04_TIMEOUT_US        15000 /* Timeout ultrasónico (15ms máximo para ~2.6m) */

/* ─── Servo ──────────────────────────────────────────────── */
#define SERVO_MIN_US      1000   /* Pulso mínimo (0°)      */
#define SERVO_MAX_US      2000   /* Pulso máximo (180°)    */
#define SERVO_SPEED_MS    250    /* Tiempo para alcanzar posición antes de apagar PWM */

/* ─── Buzzer ─────────────────────────────────────────────── */
#define BUZZER_DEFAULT_FREQ  1000  /* Hz predeterminado      */
#define BUZZER_MAX_DURATION  5000  /* ms máximo permitido    */

/* ─── Timing ─────────────────────────────────────────────── */
#define MUESTREO_MS       2000   /* ms entre lecturas de sensor */

/* ─── Umbrales y límites ────────────────────────────────── */
#define TEMP_MIN_C        -40.0f
#define TEMP_MAX_C        85.0f
#define HUMIDITY_MIN_PCT  0.0f
#define HUMIDITY_MAX_PCT  100.0f
#define DISTANCE_MIN_CM   2.0f
#define DISTANCE_MAX_CM   400.0f
#define SERVO_MIN_ANGLE   0
#define SERVO_MAX_ANGLE   180

/* ─── Códigos de error ──────────────────────────────────── */
#define ERR_SENSOR_TIMEOUT  -1
#define ERR_INVALID_PARAM   -2
#define ERR_RANGE_ERROR     -3

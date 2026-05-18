/* ============================================================
 *  actuators.c — Driver Buzzer y Servo (LEDC PWM)
 *  ESP32-C6: Usa LEDC Timer 0, Channels 0-1
 * ============================================================ */

#include "actuators.h"
#include "config.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "ACTUATORS";

/* Variables globales para control de buzzer */
static TaskHandle_t buzzer_task_handle = NULL;

typedef struct {
    uint32_t duration_ms;
    uint8_t mode;
} buzzer_task_params_t;

/* ════════════════════════════════════════════════════════════
 *  SECCIÓN: BUZZER
 * ════════════════════════════════════════════════════════════ */

int buzzer_init(void)
{
    /* Configurar timer LEDC (compartido para buzzer y servo) */
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = LEDC_BASE_FREQ,      /* Base freq alta (PWM flexible) */
    };
    
    if (ledc_timer_config(&timer_conf) != ESP_OK) {
        ESP_LOGE(TAG, "Error configurando LEDC Timer");
        return -1;
    }
    
    /* Configurar canal BUZZER (Channel 0) */
    ledc_channel_config_t buzzer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_BUZZER_CHANNEL,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = PIN_BUZZER,
        .duty = 0,  /* Inician apagados */
        .hpoint = 0,
    };
    
    if (ledc_channel_config(&buzzer_conf) != ESP_OK) {
        ESP_LOGE(TAG, "Error configurando canal BUZZER");
        return -1;
    }
    
    ESP_LOGI(TAG, "Buzzer inicializado (GPIO %d)", PIN_BUZZER);
    return 0;
}

/**
 * @brief Tarea que controla duración e intermitencia del buzzer
 */
static void _buzzer_task(void *arg)
{
    buzzer_task_params_t *params = (buzzer_task_params_t *)arg;
    if (params == NULL) {
        buzzer_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    uint32_t duration_ms = params->duration_ms;
    uint8_t mode = params->mode;
    free(params);
    
    if (mode == BUZZER_INTERMITTENT) {
        /* Intermitente: 100ms on, 100ms off */
        uint32_t elapsed = 0;
        while (elapsed < duration_ms) {
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_BUZZER_CHANNEL, 512);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_BUZZER_CHANNEL);
            vTaskDelay(100 / portTICK_PERIOD_MS);
            
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_BUZZER_CHANNEL, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_BUZZER_CHANNEL);
            vTaskDelay(100 / portTICK_PERIOD_MS);
            
            elapsed += 200;
        }
    } else {
        /* Continuo: esperar duración */
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_BUZZER_CHANNEL, 512);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_BUZZER_CHANNEL);
        vTaskDelay(duration_ms / portTICK_PERIOD_MS);
    }
    
    /* Apagar buzzer al finalizar */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_BUZZER_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_BUZZER_CHANNEL);
    
    buzzer_task_handle = NULL;
    vTaskDelete(NULL);
}

int buzzer_play(uint16_t duration_ms, uint8_t mode, uint16_t frequency_hz)
{
    /* Validaciones */
    if (duration_ms == 0 || duration_ms > BUZZER_MAX_DURATION) {
        ESP_LOGW(TAG, "Buzzer duración inválida: %u ms", duration_ms);
        return -2;  /* ERR_INVALID_PARAM */
    }
    
    if (mode > 1) {
        ESP_LOGW(TAG, "Buzzer modo inválido: %u", mode);
        return -2;
    }
    
    if (frequency_hz < 100 || frequency_hz > 10000) {
        ESP_LOGW(TAG, "Buzzer frecuencia inválida: %u Hz", frequency_hz);
        return -2;
    }
    
    /* Detener tarea anterior si existe */
    if (buzzer_task_handle != NULL) {
        vTaskDelete(buzzer_task_handle);
    }
    
    /* Actualizar frecuencia PWM */
    ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, frequency_hz);
    
    buzzer_task_params_t *task_params = malloc(sizeof(buzzer_task_params_t));
    if (task_params == NULL) {
        ESP_LOGE(TAG, "Error reservando memoria para tarea de buzzer");
        return -1;
    }

    task_params->duration_ms = duration_ms;
    task_params->mode = mode;

    /* Crear tarea para controlar duración */
    if (xTaskCreate(_buzzer_task, "buzzer_task", 2048, 
                    task_params, 5,
                    &buzzer_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Error creando tarea de buzzer");
        free(task_params);
        return -1;
    }
    
    ESP_LOGI(TAG, "Buzzer ON — Duración: %u ms, Modo: %s, Freq: %u Hz", 
             duration_ms, mode == 0 ? "continuo" : "intermitente", frequency_hz);
    
    return 0;
}

int buzzer_stop(void)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_BUZZER_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_BUZZER_CHANNEL);
    
    if (buzzer_task_handle != NULL) {
        vTaskDelete(buzzer_task_handle);
        buzzer_task_handle = NULL;
    }
    
    ESP_LOGI(TAG, "Buzzer STOP");
    return 0;
}

/* ════════════════════════════════════════════════════════════
 *  SECCIÓN: SERVO
 * ════════════════════════════════════════════════════════════ */

int servo_init(void)
{
    /* Usar un timer dedicado para que el buzzer no altere la señal del servo */
    
    /* IMPORTANTE: Configurar el timer LEDC_TIMER_1 primero */
    ledc_timer_config_t servo_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = LEDC_SERVO_FREQ,  /* 50Hz para servos */
    };
    
    if (ledc_timer_config(&servo_timer) != ESP_OK) {
        ESP_LOGE(TAG, "Error configurando LEDC Timer 1 para servo");
        return -1;
    }
    
    /* Configurar canal SERVO (Timer independiente para no mezclar con buzzer) */
    ledc_channel_config_t servo_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_SERVO_CHANNEL,
        .timer_sel = LEDC_TIMER_1,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = PIN_SERVO,
        .duty = 0,
        .hpoint = 0,
    };
    
    if (ledc_channel_config(&servo_conf) != ESP_OK) {
        ESP_LOGE(TAG, "Error configurando canal SERVO");
        return -1;
    }
    
    /* Posición inicial: 90° (centro) - sin apagar PWM después */
    int angle = 90;
    uint16_t pulse_us = SERVO_MIN_US + ((angle * (SERVO_MAX_US - SERVO_MIN_US)) / 180);
    uint32_t duty = (pulse_us * 1023) / 20000;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_SERVO_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_SERVO_CHANNEL);
    ESP_LOGI(TAG, "Servo inicializado en 90° (GPIO %d, 50Hz)", PIN_SERVO);
    return 0;
}

int servo_move(int angle)
{
    /* Validar rango */
    if (angle < SERVO_MIN_ANGLE || angle > SERVO_MAX_ANGLE) {
        ESP_LOGW(TAG, "Servo ángulo inválido: %d° (rango 0-180)", angle);
        return -2;  /* ERR_INVALID_PARAM */
    }
    
    /* Mapear ángulo a duración de pulso (us):
     * 0°   = 1000us  (SERVO_MIN_US)
     * 90°  = 1500us  (centro)
     * 180° = 2000us  (SERVO_MAX_US)
     * 
     * Para LEDC con 50Hz = 20ms = 20000us
     * duty = (pulso_us / 20000) * (2^10 - 1) = (pulso_us / 20000) * 1023
     */
    
    uint16_t pulse_us = SERVO_MIN_US + 
                        ((angle * (SERVO_MAX_US - SERVO_MIN_US)) / 180);
    
    uint32_t duty = (pulse_us * 1023) / 20000;
    
    if (ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_SERVO_CHANNEL, duty) != ESP_OK) {
        ESP_LOGE(TAG, "Error configurando duty servo");
        return -1;
    }
    
    if (ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_SERVO_CHANNEL) != ESP_OK) {
        ESP_LOGE(TAG, "Error actualizando duty servo");
        return -1;
    }

    /* IMPORTANTE: No apagar el PWM. El servo necesita mantener el pulso continuamente 
       en 50Hz para conservar su posición. */
    
    ESP_LOGI(TAG, "Servo movido a %u° (duty=%lu, pulso=%u us)", 
             angle, duty, pulse_us);
    
    return 0;
}

int servo_stop(void)
{
    if (ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_SERVO_CHANNEL, 0) != ESP_OK) {
        ESP_LOGE(TAG, "Error apagando duty del servo");
        return -1;
    }

    if (ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_SERVO_CHANNEL) != ESP_OK) {
        ESP_LOGE(TAG, "Error actualizando duty del servo al apagar");
        return -1;
    }
    
    ESP_LOGI(TAG, "Servo desactivado");
    return 0;
}

# Documento Técnico: Justificación de Diseño

## 📌 Asignación de Pines GPIO para ESP32-C6

### Especificaciones de ESP32-C6

El ESP32-C6 es un microcontrolador RISC-V de bajo consumo con **30 pines GPIO** (GPIO0 a GPIO22, con algunos reservados).

#### Capacidades por Tipo de Pin:

| Rango | Características | Uso Recomendado |
|-------|-----------------|-----------------|
| GPIO0-1 | Strapping/Boot | ⚠️ EVITAR para sensores |
| GPIO2-3 | I2C nativo, PWM | I2C, LEDC |
| GPIO4-7 | GPIO estándar, PWM | Sensores, PWM |
| GPIO8-10 | Internos (OPI), PWM | ⚠️ Reducida capacidad, pero soportan LEDC |
| GPIO11-22 | GPIO estándar | Cualquier uso |

---

## 🎯 Estrategia de Selección de Pines

### 1. **DHT22 → GPIO5**

**Características del DHT22:**
- Protocolo: 1-wire (digital, single-pin)
- Tiempo de ciclo: ~2 segundos
- Timing crítico: Requiere respuestas microsegundo-precisas
- Interrupciones: Deben deshabilitarse durante lectura

**Por qué GPIO5:**
- ✅ GPIO estándar de alta confiabilidad
- ✅ Completamente separado de periféricos internos
- ✅ Adyacente a pines SPI si necesitas expandir en el futuro
- ✅ Sin capacitancia parásita (a diferencia de GPIO8-10)
- ✅ Permite usar `taskDISABLE_INTERRUPTS()` sin interferencias

**Alternativas rechazadas:**
- ❌ GPIO0, GPIO1: Conflictos de boot
- ❌ GPIO8-10: Timing más inestable (pines internos OPI)
- ❌ GPIO2-3: Reservados idealmente para I2C futuro

---

### 2. **HC-SR04 Trigger → GPIO6**

**Características HC-SR04:**
- Trigger: Pulso de 10µs a nivel alto
- Echo: Pulso variable (duración = distancia)
- Rango: 2-400cm (eco hasta ~30ms)

**Por qué GPIO6:**
- ✅ Output digital simple (Trigger)
- ✅ Adyacente a GPIO7 (pairing lógico)
- ✅ Timing poco sensible para Trigger
- ✅ Frecuencia típica: 1 lectura/200ms (no-crítica)

---

### 3. **HC-SR04 Echo → GPIO7**

**Características:**
- Debe capturar flancos ascendentes/descendentes
- Requiere lectura de duración en microsegundos
- Timing crítico pero no tan severo como DHT22

**Por qué GPIO7:**
- ✅ Input compatible con captura de tiempo
- ✅ Contiguo a Trigger (GPIO6) → mejor routing PCB
- ✅ GPIO estándar (no limitado por OPI)
- ✅ `esp_timer_get_time()` es preciso para esta escala

**Alternativas rechazadas:**
- ❌ GPIO8-10: Mejor evitar para precisión de timing

---

### 4. **Buzzer (PWM) → GPIO8**

**Características Buzzer:**
- Necesita PWM variable (1-10kHz típico)
- Duty cycle: 50% (square wave simple)
- No requiere timing ultrapreciso
- Múltiples frecuencias simultáneamente: NO necesario

**Por qué GPIO8:**
- ✅ Soporte LEDC nativo (PWM desde ESP32)
- ✅ Canal LEDC independiente (no compartido con servo)
- ✅ Frecuencia de 1-10kHz fácilmente alcanzable
- ✅ Permite cambiar frecuencia dinámicamente

**Configuración LEDC para Buzzer:**
```
Timer: LEDC_TIMER_0 (compartido con servo)
Channel: LEDC_CHANNEL_0
Frecuencia: 1000 Hz (ajustable en runtime)
Resolución: 10-bit (0-1023)
Duty: 50% para sonido claro
```

**Alternativas rechazadas:**
- ❌ GPIO SPI/I2C: Posibles conflictos futuros
- ❌ GPIO2-3: Ya reservados para I2C

---

### 5. **Servo (PWM 50Hz) → GPIO9**

**Características Servo SG90:**
- Estándar: 50Hz (20ms per ciclo)
- Pulso: 1000-2000µs (0-180°)
- Corriente: ~100-500mA (requiere fuente separada)
- Precisión: ±10° típica

**Por qué GPIO9:**
- ✅ Soporte LEDC independiente (LEDC_CHANNEL_1)
- ✅ 50Hz fácilmente configurable (ledc_set_freq)
- ✅ Duty cycle variable: mapeo directo ángulo → duty
- ✅ No comparte canal con buzzer

**Cálculo PWM para Servo:**
```c
// LEDC en 50Hz = período 20ms = 20000µs
// Para ángulo 0-180°:
// duty = (pulso_us / 20000) * 1023
// Ejemplo: 90° → 1500µs → duty = (1500/20000)*1023 = 76.7 ≈ 77
```

**Alternativas rechazadas:**
- ❌ GPIO2-3: Reservados para I2C
- ❌ GPIO6-7: Usados por HC-SR04

---

### 6. **LED Status → GPIO4**

**Propósito:**
- Indicador visual: WiFi conectado + CoAP activo
- Bajo consumo

**Por qué GPIO4:**
- ✅ GPIO simple, sin requisitos especiales
- ✅ Adyacente a DHT22 (GPIO5) → agrupación lógica
- ✅ Completamente libre de conflictos
- ✅ Bajo consumo (LED típico 20mA)

---

## ⚡ Esquema de Conexión Física

```
ESP32-C6 [Pines lado A]
┌─────────────────────────────────┐
│ 3V3 [POWER]                     │
│ GND [POWER]                     │
│                                 │
│ GPIO4 ─→ LED Status (+ resistor)│
│ GPIO5 ─→ DHT22 (DATA pin)       │
│ GPIO6 ─→ HC-SR04 (TRIG, 5V)     │
│ GPIO7 ←─ HC-SR04 (ECHO, 5V)     │
│                                 │
│ GPIO8 ─→ Buzzer (+ GND)         │
│ GPIO9 ─→ Servo (+ 5V separado)  │
│                                 │
│ GPIO2 ─→ I2C SDA (reservado)    │
│ GPIO3 ─→ I2C SCL (reservado)    │
└─────────────────────────────────┘

Consideraciones de Potencia:
- LEDs: ~5mA @ 3V3 (resistor 1kΩ)
- Sensores (DHT22, HC-SR04): ~5mA c/u
- Servo: ~100-500mA (FUENTE SEPARADA recomendada)
- Buzzer: ~20-100mA (según modelo)

Total ESP32: ~200mA (WiFi + CoAP)
Total Periféricos: ~200mA (con servo en servicio)

IMPORTANTE: Servo y ESP32 DEBEN tener fuentes separadas
debido al pico de corriente al cambiar ángulo.
```

---

## 🔌 Esquemático Mínimo Recomendado

### Para Prototipado Seguro:

```
┌─────────────────────────────────┐
│ ESP32-C6 + Placa Desarrollo     │
├─────────────────────────────────┤
│ USB → 5V Regulador → VDD (esp)  │
│                                 │
│ GPIO5 ┬→ [1kΩ] ─→ DHT22 DATA   │
│       └→ DHT22 VDD (3V3)        │
│       └→ DHT22 GND (GND)        │
│                                 │
│ GPIO6 → HC-SR04 TRIG (5V)       │
│ GPIO7 ← HC-SR04 ECHO (5V)       │
│      └→ HC-SR04 VDD (5V)        │
│      └→ HC-SR04 GND (GND)       │
│                                 │
│ GPIO8 → [1kΩ] → Buzzer +       │
│           GND ← Buzzer -        │
│                                 │
│ GPIO9 → Servo SIGNAL           │
│ 5V(sep) → Servo VDD            │
│ GND ← Servo GND                │
│                                 │
│ GPIO4 → LED (+ 1kΩ resistor)    │
│ GND ← LED (-)                   │
└─────────────────────────────────┘
```

---

## 📊 Tabla Comparativa de Alternativas Rechazadas

| Pin Alternativo | Sensor | ¿Por qué NO? | Riesgo |
|---|---|---|---|
| GPIO0 | DHT22 | Strapping pin (BOOT) | Fallo al iniciar |
| GPIO1 | HC-SR04 | Strapping pin | Inestable timing |
| GPIO2-3 | Buzzer | Mejor para I2C futuro | Limita expansión |
| GPIO8-10 | DHT22 | Pines OPI internos | Timing inestable |
| GPIO19-22 | Servo | Demasiado lejos (PCB) | Ruido EMI en PWM |
| ADC pins | Sensores | Limita escalado ADC | Conflictos si añades DAC |

---

## ⚙️ Configuración de Periféricos en Código

### LEDC (PWM)

```c
// Timer compartido para ambos canales
ledc_timer_config_t timer = {
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .timer_num = LEDC_TIMER_0,
    .duty_resolution = LEDC_TIMER_10_BIT,  // 0-1023 duty
    .freq_hz = 5000,                       // Base freq (se ajusta por canal)
};

// Buzzer: LEDC_CHANNEL_0 @ variable freq (100-10000Hz)
ledc_channel_config_t buzzer = {
    .channel = LEDC_CHANNEL_0,
    .gpio_num = GPIO_NUM_8,
    .duty = 512,        // 50% duty
};

// Servo: LEDC_CHANNEL_1 @ 50Hz fijo
ledc_channel_config_t servo = {
    .channel = LEDC_CHANNEL_1,
    .gpio_num = GPIO_NUM_9,
    .duty = 76,         // 1500µs → 90°
};
```

### GPIO Input/Output

```c
// HC-SR04 TRIGGER (salida simple)
gpio_config_t trigger = {
    .pin_bit_mask = 1ULL << GPIO_NUM_6,
    .mode = GPIO_MODE_OUTPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
};

// HC-SR04 ECHO (entrada simple, sin interrupción)
// - Lectura síncrona en polling (loop tight)
// - Captura manual de tiempo con esp_timer_get_time()
gpio_config_t echo = {
    .pin_bit_mask = 1ULL << GPIO_NUM_7,
    .mode = GPIO_MODE_INPUT,
};

// DHT22 (open-drain 1-wire)
gpio_config_t dht = {
    .pin_bit_mask = 1ULL << GPIO_NUM_5,
    .mode = GPIO_MODE_OUTPUT_OD,       // Open-drain
    .pull_up_en = GPIO_PULLUP_ENABLE,  // Pullup interno
};
```

---

## 🧪 Consideraciones para Prototipado

### Ruido y Estabilidad:

1. **Sensores analógicos**: No usados, pero si añades LDR/MQ-135:
   - Usar GPIO34-35 (ADC channel nativo)
   - Añadir capacitor 100nF en Vcc para filtro

2. **PWM**: Buzzer + Servo generan EMI
   - Twisted pair para GPIO9 → Servo
   - Ferrita toroid en supply del servo

3. **Alimentación separada para servo**:
   - Fuente 5V mínimo 1A
   - Capacitor 100µF + 10µF desacoplamiento
   - Diodo de protección inversa recomendado

---

## 📈 Escalabilidad Futura

**Si deseas añadir:**

### 1. **Sensor I2C (BME280, etc)**
```
GPIO2 ← SDA (I2C)
GPIO3 ← SCL (I2C)
Pines ya reservados ✓
```

### 2. **Otro Servo**
```
Usar GPIO11 + LEDC_CHANNEL_2
O GPIO12 + LEDC_CHANNEL_3
(disponibles)
```

### 3. **Entrada Digital (botón)**
```
Usar GPIO11-22 (todos disponibles)
Evitar GPIO0-1 (strapping)
```

### 4. **ADC Adicional**
```
GPIO17 (ADC1_CH6) para LDR
GPIO18 (ADC1_CH7) para MQ-135
No conflictúa con GPIO 5-9
```

---

## ✅ Checklist Final de Validación

- [x] Todos los pines evitan strapping pins (GPIO0-1)
- [x] DHT22 y HC-SR04 en GPIO de timing confiable
- [x] Buzzer y Servo en canales LEDC independientes
- [x] Pines I2C reservados (GPIO2-3) para expansión
- [x] LED status en GPIO no crítico
- [x] Sin conflictos entre periféricos
- [x] Fuentes de poder separadas para servo
- [x] Timing de sensores validado experimentalmente
- [x] Documentación de routing PCB clara

---

## 📚 Referencias Técnicas

- **ESP32-C6 TRM** (Technical Reference Manual):
  - GPIO Matrix, LEDC, ADC specs
  - Strapping pin behavior
  
- **Hojas de Datos**:
  - DHT22: Timing diagrams en protocolo 1-wire
  - HC-SR04: 10µs trigger, echo response time
  - SG90: 50Hz, 1000-2000µs servo pulses

---

**Documento**: Justificación de Asignación de Pines  
**Versión**: 1.0  
**Revisión**: Mayo 2026  
**Estado**: ✅ Validado y documentado

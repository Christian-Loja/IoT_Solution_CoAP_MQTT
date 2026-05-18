# ESP32-C6 CoAP Server — Multiperféricos

**Servidor CoAP embebido en ESP32-C6** que expone recursos para leer sensores y controlar actuadores a través del protocolo CoAP (RFC 7252).

## 📋 Características

### Hardware Soportado
- **Sensor DHT22**: Temperatura (°C) y Humedad relativa (%)
- **Sensor HC-SR04**: Distancia ultrasónica (cm)
- **Buzzer pasivo**: Sonido continuo o intermitente (PWM LEDC)
- **Servomotor SG90**: Posicionamiento 0-180° (PWM LEDC 50Hz)
- **LED de estado**: Indicador WiFi/CoAP activo

### Recursos CoAP Expuestos

#### 📖 Lectura (GET, No-Confirmable)
```
GET coap://<ESP32-IP>:5683/temp
Respuesta: {"temp":25.3, "humidity":65.2}
Tipo: NoN (Non-confirmable)
```

```
GET coap://<ESP32-IP>:5683/dist
Respuesta: {"distance":45.67}
Tipo: NoN (Non-confirmable)
```

#### ✍️ Escritura/Control (POST, Confirmable)
```
POST coap://<ESP32-IP>:5683/servo
Payload: {"angle":90}
Respuesta: {"status":"ok", "angle":90}
Tipo: CoN (Confirmable)
```

```
POST coap://<ESP32-IP>:5683/buzzer
Payload: {"duration":2000, "mode":0, "frequency":1000}
Respuesta: {"status":"ok", "duration":2000, "mode":0}
Tipo: CoN (Confirmable)

mode:     0 = Continuo, 1 = Intermitente (100ms on/off)
duration: 0-5000 ms
frequency: 100-10000 Hz
```

## 🔌 Asignación de Pines GPIO (ESP32-C6)

| Componente | Pin GPIO | Tipo | Justificación |
|-----------|----------|------|---------------|
| DHT22 | GPIO5 | Digital | 1-wire, sin conflictos |
| HC-SR04 Trigger | GPIO6 | Output | Pulso de inicio |
| HC-SR04 Echo | GPIO7 | Input | Captura de flancos |
| Buzzer | GPIO8 | PWM (LEDC CH0) | Frecuencia variable |
| Servo | GPIO9 | PWM (LEDC CH1) | 50Hz estándar |
| LED Status | GPIO4 | Output | Indicador visual |

**Notas importantes:**
- GPIO0, GPIO1: Reservados para boot/strapping
- GPIO8-10: Internos, pero GPIO8-9 soportan LEDC
- GPIO22: No disponible en ESP32-C6

## ⚙️ Compilación

### Requisitos Previos
```bash
# Instalar ESP-IDF v5.x o superior
export IDF_PATH=~/esp/esp-idf
source $IDF_PATH/export.sh

# O usar ESP-IDF venv (recomendado)
. ~/esp-idf/export.sh
```

### Build Steps

1. **Clonar/navegar al proyecto**:
```bash
cd ESP32_C6_CoAP_Server/
```

2. **Configurar target a ESP32-C6**:
```bash
idf.py set-target esp32c6
```

3. **Editar configuración WiFi**:
```bash
idf.py menuconfig
# Navegar a: Component Config → CoAP
# Y a: Component Config → Wi-Fi
# Configurar SSID y PASSWORD
```

4. **Compilar**:
```bash
idf.py build
```

5. **Flashear a ESP32-C6** (por USB):
```bash
idf.py -p /dev/ttyUSB0 flash
# Windows: idf.py -p COM3 flash
```

6. **Ver logs**:
```bash
idf.py -p /dev/ttyUSB0 monitor
```

Combinado: `idf.py -p /dev/ttyUSB0 build flash monitor`

## 🧪 Ejemplos de Uso

### Cliente CoAP (Línea de Comandos)

**Requisito**: Instalar cliente CoAP
```bash
# Linux
sudo apt install libcoap-bin

# macOS
brew install libcoap

# O compilar desde: https://github.com/obgm/libcoap
```

### Ejemplos de Queries

#### 1️⃣ Leer temperatura y humedad (NoN)
```bash
coap-client -m get coap://192.168.1.100/temp
# Respuesta (inmediata, sin confirmación):
# {"temp":25.3,"humidity":65.2}
```

#### 2️⃣ Leer distancia ultrasónica (NoN)
```bash
coap-client -m get coap://192.168.1.100/dist
# Respuesta:
# {"distance":45.67}
```

#### 3️⃣ Mover servo a 90° (CoN)
```bash
coap-client -m post -t json \
  -e '{"angle":90}' \
  coap://192.168.1.100/servo
# Respuesta (confirmada):
# {"status":"ok","angle":90}
```

#### 4️⃣ Activar buzzer 3 segundos continuo (CoN)
```bash
coap-client -m post -t json \
  -e '{"duration":3000,"mode":0,"frequency":1000}' \
  coap://192.168.1.100/buzzer
# Respuesta:
# {"status":"ok","duration":3000,"mode":0}
```

#### 5️⃣ Buzzer intermitente 2 segundos (CoN)
```bash
coap-client -m post -t json \
  -e '{"duration":2000,"mode":1,"frequency":800}' \
  coap://192.168.1.100/buzzer
# mode=1: 100ms on, 100ms off
```

### Cliente Python (Recomendado)

```python
import aiocoap
import asyncio
import json

async def test_coap():
    # Cliente CoAP
    context = await aiocoap.Context.create_client_context()
    
    # GET /temp (NoN)
    request = aiocoap.Message(code=aiocoap.GET, uri="coap://192.168.1.100/temp")
    response = await context.request(request).response
    print(f"Temp: {response.payload.decode()}")
    # {"temp":25.3,"humidity":65.2}
    
    # POST /servo (CoN)
    request = aiocoap.Message(
        code=aiocoap.POST, 
        uri="coap://192.168.1.100/servo",
        payload=b'{"angle":45}'
    )
    response = await context.request(request).response
    print(f"Servo: {response.payload.decode()}")
    # {"status":"ok","angle":45}

asyncio.run(test_coap())
```

**Instalar aiocoap**:
```bash
pip install aiocoap
```

### Wireshark Packet Inspection

Para capturar y analizar mensajes CoAP:
```bash
wireshark &
# Filtro: coap
# Puerto UDP 5683
```

## 📊 Estructura del Proyecto

```
ESP32_C6_CoAP_Server/
├── CMakeLists.txt          # Build root
├── sdkconfig.defaults      # Configuración ESP-IDF
├── main/
│   ├── CMakeLists.txt      # Build component
│   ├── main.c              # Main + servidor CoAP
│   ├── config.h            # Pines, constantes
│   ├── dht22.h             # Header driver DHT22
│   ├── dht22.c             # Implementación DHT22
│   ├── hc_sr04.h           # Header driver HC-SR04
│   ├── hc_sr04.c           # Implementación HC-SR04
│   ├── actuators.h         # Header buzzer + servo
│   └── actuators.c         # Implementación actuadores
└── README.md               # Este archivo

Tamaño código: ~15-20KB (flash)
Memoria RAM: ~8-12KB runtime
```

## 🐛 Troubleshooting

### "Conexión WiFi falla"
- Verificar SSID y contraseña en `sdkconfig`
- Comprobar que el AP está en 2.4GHz (ESP32-C6 no soporta 5GHz)
- Ver logs con `idf.py monitor`

### "Servidor CoAP no responde"
- Verificar IP: `ping 192.168.1.100` (reemplazar IP real)
- Comprobar que los sensores están inicializados: ver log "✓ DHT22 inicializado"
- Revisar conectividad CoAP: `coap-client -m ping coap://192.168.1.100`

### "DHT22 devuelve error"
- Verificar conexión del sensor
- Comprobar que Pin GPIO5 está libre (sin conflictos)
- Timing crítico: asegurar que interrupciones no interfieran

### "HC-SR04 no funciona"
- Distancia fuera de rango (2-400cm)?
- Verificar alimentación (5V típico)
- GPIO7 (Echo) debe ser entrada, GPIO6 (Trigger) salida

### "Servo no se mueve"
- Ángulo fuera de rango 0-180°?
- Verificar alimentación del servo (mínimo 500mA)
- Comprobar que PWM está activo: pin 9 debe mostrar ~50Hz

### "Buzzer sin sonido"
- Frecuencia muy baja (<100Hz) o alta (>10000Hz)?
- Verificar polaridad buzzer (algunos son polarizados)
- Probar con frecuencia default: 1000Hz

## 🔐 Seguridad (Notas)

Por defecto, CoAP sin DTLS. Para producción:
```c
// En config.h descomentar:
#define COAP_WITH_DTLS 1
```

Luego compilar con:
```bash
idf.py menuconfig
# Component Config → libcoap → Enable DTLS
```

## 📚 Referencias

- [RFC 7252 - CoAP](https://tools.ietf.org/html/rfc7252)
- [libcoap Documentation](https://libcoap.net/)
- [ESP-IDF Docs](https://docs.espressif.com/projects/esp-idf/en/latest/)
- [ESP32-C6 Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-c6_datasheet_en.pdf)

## 📝 Cambios Recientes (v1.0)

- ✅ Servidor CoAP completo con 4 recursos
- ✅ Drivers DHT22 y HC-SR04 (timing crítico)
- ✅ Control PWM buzzer e servo (LEDC)
- ✅ Manejo concurrente con FreeRTOS + mutex
- ✅ Respuestas CoN y NoN correctas
- ✅ JSON payload parsing simple

## 📄 Licencia

Código libre para uso educativo y comercial.

---

**Autor**: Equipo Redes Telecomunicaciones 2026  
**Versión**: 1.0  
**Última actualización**: Mayo 2026

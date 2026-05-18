# Guía de Integración y Compilación

## 🚀 Inicio Rápido

### Instalación Previa

Si aún no tienes ESP-IDF instalado:

```bash
# Linux/macOS
mkdir -p ~/esp
cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
git checkout v5.3  # O v6.0 si prefieres versión más nueva
./install.sh
source export.sh

# Windows (PowerShell)
# Descargar ESP-IDF desde: https://dl.espressif.com/esp-idf/
# O usar: pip install esp-idf
```

---

## 📦 Estructura de Carpetas Recomendada

```
~/workspace/
├── esp-idf/              # Instalación ESP-IDF
├── esp32c6_projects/
│   └── esp32c6_coap_server/
│       ├── CMakeLists.txt
│       ├── sdkconfig.defaults
│       ├── main/
│       │   ├── CMakeLists.txt
│       │   ├── main.c
│       │   ├── config.h
│       │   ├── dht22.h
│       │   ├── dht22.c
│       │   ├── hc_sr04.h
│       │   ├── hc_sr04.c
│       │   ├── actuators.h
│       │   └── actuators.c
│       ├── README.md
│       ├── DISEÑO_PINES.md
│       ├── COMPILACION.md
│       └── cliente_coap_test.py
```

---

## 🔧 Pasos de Compilación Detallados

### 1. Clonar/Crear Proyecto

```bash
# Opción A: Si ya tienes los archivos
cd esp32c6_coap_server/

# Opción B: Crear desde plantilla
idf.py create-project esp32c6_coap_server
cd esp32c6_coap_server/
# Luego copiar los archivos main.c, etc.
```

### 2. Seleccionar Target ESP32-C6

```bash
idf.py set-target esp32c6
```

Esto genera/actualiza:
- `sdkconfig` (si no existe)
- `.idf_build/` (directorio de compilación)

### 3. Configurar SDK (Menuconfig)

```bash
idf.py menuconfig
```

**Navegación en menuconfig:**
```
[Opción Clave] → [Subopciones]

Project Configuration
  → (tu_ssid)                    # WIFI_SSID
  → (tu_contraseña)             # WIFI_PASSWORD

Component Config
  → CoAP
    → [x] Enable libcoap
    → [x] Enable DTLS (opcional, NO recomendado para desarrollo)
  → Wi-Fi
    → Default WiFi SSID = "TU_RED_WIFI"
    → Default WiFi Password = "TU_CONTRASEÑA"

Compiler
  → Optimization Level → Release (-Os)
```

**Guardar:** `Ctrl+S` → `q`

### 4. Compilar (Build)

```bash
idf.py build
```

**Salida esperada:**
```
...
[100%] Built target esp32c6_coap_server.elf
...
Size of binary: XXX bytes
...
```

**Si hay errores:**

| Error | Causa | Solución |
|-------|-------|----------|
| `Cannot find IDF_PATH` | ESP-IDF no inicializado | `source ~/esp/esp-idf/export.sh` |
| `Component not found: coap` | libcoap no instalada | Asegurar `CONFIG_COAP_ENABLE_DTLS=n` en menuconfig |
| `undefined reference to '_dht22_init'` | Archivos main.c no compilados | Verificar `main/CMakeLists.txt` lista todos los SRCS |

### 5. Flashear al Dispositivo

#### Identificar Puerto Serial

```bash
# Linux
ls /dev/ttyUSB*
# Típicamente: /dev/ttyUSB0

# macOS
ls /dev/tty.usbserial*
# Típicamente: /dev/tty.usbserial-14

# Windows
# Ver en Administrador de dispositivos → Puertos COM
# Típicamente: COM3, COM4, etc.
```

#### Flashear con IDF

```bash
# Comando completo (build + flash + monitor)
idf.py -p /dev/ttyUSB0 build flash monitor

# O separado:
idf.py -p /dev/ttyUSB0 flash
```

**Parámetros opcionales:**
```bash
idf.py -p /dev/ttyUSB0 -b 460800 flash  # Baudrate más rápido
idf.py -p COM3 flash                     # Windows
```

---

## 📋 Output Esperado en Monitor

Después de `idf.py monitor`:

```
I (0) boot: ESP-IDF v5.3.0 2nd stage bootloader
I (27) boot: chip revision: v0.1
I (152) esp_image: segment 0: paddr=0x00000020 vaddr=0x42000020 size=0x0c934 (   51508) map
...
I (XXX) CoAP_Server: ═══════════════════════════════════════════════════════
I (XXX) CoAP_Server:   ESP32-C6 CoAP Server — Multiperféricos
I (XXX) CoAP_Server:   ESP-IDF v5.x/v6.x — 2026
I (XXX) CoAP_Server: ═══════════════════════════════════════════════════════

I (XXX) wifi: mode : sta
I (XXX) CoAP_Server: WiFi iniciando...

I (XXX) wifi:<ssid>
I (XXX) CoAP_Server: Reconectando WiFi (1/10)...

I (XXX) wifi:state: connected
I (XXX) CoAP_Server: ✓ WiFi OK — IP: 192.168.1.100

I (XXX) CoAP_Server: ✓ DHT22 inicializado
I (XXX) CoAP_Server: ✓ HC-SR04 inicializado
I (XXX) CoAP_Server: ✓ Buzzer inicializado
I (XXX) CoAP_Server: ✓ Servo inicializado
I (XXX) CoAP_Server: ✓ Contexto CoAP creado
I (XXX) CoAP_Server: ✓ Recurso /temp registrado
I (XXX) CoAP_Server: ✓ Recurso /dist registrado
I (XXX) CoAP_Server: ✓ Recurso /servo registrado
I (XXX) CoAP_Server: ✓ Recurso /buzzer registrado
I (XXX) CoAP_Server: ═══════════════════════════════════════════════════════
I (XXX) CoAP_Server:   ✓ SERVIDOR CoAP ACTIVO EN PUERTO 5683
I (XXX) CoAP_Server: ═══════════════════════════════════════════════════════
```

Si ves esto → **¡Éxito!** El servidor está activo.

---

## ✅ Verificación Post-Compilación

### 1. Probar Conectividad WiFi

```bash
# Desde otra máquina en la misma red:
ping 192.168.1.100
# Deberías recibir respuestas ICMP
```

### 2. Probar CoAP con Command-Line Tool

```bash
# Instalar libcoap-bin si no tienes
sudo apt install libcoap-bin

# Probar conectividad CoAP
coap-client -m ping coap://192.168.1.100

# Leer temperatura
coap-client -m get coap://192.168.1.100/temp
# Respuesta esperada: {"temp":25.3,"humidity":65.2}

# Mover servo a 90°
coap-client -m post -t json \
  -e '{"angle":90}' \
  coap://192.168.1.100/servo
# Respuesta esperada: {"status":"ok","angle":90}
```

### 3. Ejecutar Test Completo (Python)

```bash
# Instalar aiocoap
pip install aiocoap

# Ejecutar tests automáticos
python3 cliente_coap_test.py 192.168.1.100

# Salida esperada:
# ✓ TEST 1: GET /temp — éxito
# ✓ TEST 2: GET /dist — éxito
# ✓ TEST 3: POST /servo — éxito
# ...
```

---

## 🔍 Debugging Avanzado

### Ver Logs en Detalle

```bash
idf.py monitor -v 5
# v = verbosity (0-5, donde 5 es máximo detalle)
```

### Capturar Logs a Archivo

```bash
idf.py monitor > logs.txt 2>&1
# Ctrl+C para detener, ver logs.txt después
```

### Usar GDB (Debugger Remoto)

```bash
# Terminal 1: Iniciar servidor de depuración
idf.py gdb

# Terminal 2: Conectar GDB
(gdb) set remote hardware-watchpoint-limit unlimited
(gdb) target remote localhost:3333
(gdb) monitor reset halt
(gdb) c  # continuar ejecución
```

### Analizar Stack Trace

Si hay crash o excepción:
```
Backtrace:
  0x42001234 in function_name at file.c:123
  0x42005678 in main at main.c:456
```

Usar `addr2line` para convertir a línea:
```bash
xtensa-esp32c6-elf-addr2line -pfeia \
  build/esp32c6_coap_server.elf \
  0x42001234
# Mostrará archivo y línea exacta
```

---

## 📊 Análisis de Memoria

### Tamaño de Binario

```bash
idf.py size
# Output:
# Total image size: 1234567 bytes
# IRAM: 456 bytes
# Flash: 1234111 bytes
```

### Análisis Detallado

```bash
idf.py size-components
# Muestra tamaño por componente:
# freertos: 50KB
# esp_wifi: 150KB
# coap: 80KB
# ...
```

### Optimizar Tamaño (Opcional)

En `menuconfig`:
```
Compiler → Optimization Level → Release (-Os)
Component Config → FreeRTOS → Tick rate → 100
```

---

## 🐛 Troubleshooting Compilación

### Error: `Cannot find coap component`

```bash
# Solución: Asegurar que el componente CoAP esté disponible
# En ESP-IDF v5.x+ debería estar incluido

# Si no funciona:
idf.py component-list | grep coap
# Si no aparece, instalar manualmente
idf.py component-manager create-manifest

# O descargar de GitHub:
# https://github.com/obgm/libcoap
```

### Error: `Undefined reference to coap_new_context`

```bash
# Asegurar que main/CMakeLists.txt tiene:
# REQUIRES
#   coap

# Verificar:
cat main/CMakeLists.txt | grep REQUIRES
```

### Error: `DHT22 sensor not ready` (en runtime)

```bash
# Verificar en logs si se inicializó:
# I (XXX) DHT22: DHT22 inicializado

# Si no:
# 1. Revisar pin GPIO5 en config.h
# 2. Validar conexión física del sensor
# 3. Probar con un blink test en GPIO5
```

### Error: `CoAP server startup failed`

```bash
# Causas posibles:
# 1. WiFi no conectado → revisar credentials
# 2. Puerto 5683 en uso → cambiar puerto en config.h
# 3. Memoria insuficiente → revisar logs de heap

# Ver estado WiFi:
idf.py monitor | grep WiFi
```

---

## 📝 Opciones Avanzadas de Build

### Build Limpiar

```bash
# Eliminar todos los objetos compilados
idf.py clean

# Eliminar TODAS las configuraciones (sdkconfig, etc)
idf.py fullclean
```

### Build Paralelo (Más rápido)

```bash
idf.py -j4 build   # Compilar con 4 threads (ajusta según CPU cores)
```

### Build Verbose (Ver comandos GCC)

```bash
idf.py build VERBOSE=1
# Útil para debugging de compiler issues
```

---

## 🎯 Checklist Final Antes de Producción

- [ ] `sdkconfig` configurado con SSID/password correctos
- [ ] Target set a `esp32c6`
- [ ] Compilación sin warnings
- [ ] Binario flasheado sin errores
- [ ] WiFi conectado según logs
- [ ] CoAP responde a ping: `coap-client -m ping ...`
- [ ] GET /temp y GET /dist responden con JSON válido
- [ ] POST /servo mueve servo a ángulos válidos
- [ ] POST /buzzer produce sonido
- [ ] No hay crashes en monitor durante 5+ minutos
- [ ] Consumo de potencia dentro de límites

---

## 📚 Referencias Rápidas

```bash
# Ver todas las opciones idf.py
idf.py --help

# Help para comando específico
idf.py build --help
idf.py flash --help

# Ver versión ESP-IDF
idf.py version

# Configuración actual
cat sdkconfig | grep WIFI_SSID
```

---

**Documento**: Guía de Compilación e Integración  
**Versión**: 1.0  
**Última actualización**: Mayo 2026  
**Estado**: ✅ Validado

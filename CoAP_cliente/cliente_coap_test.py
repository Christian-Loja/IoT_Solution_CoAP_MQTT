#!/usr/bin/env python3
"""
cliente_coap_test.py — Cliente CoAP de Prueba para ESP32-C6
Requiere: pip install aiocoap
"""

import asyncio
import json
import sys
from aiocoap import *

class CoAPClientTester:
    def __init__(self, server_ip="192.168.137.92", port=5683):
        self.server_ip = server_ip
        self.port = port
        self.base_uri = f"coap://{server_ip}:{port}"
        
    async def init(self):
        """Inicializar contexto CoAP"""
        self.context = await Context.create_client_context()
    
    async def cleanup(self):
        """Cerrar contexto"""
        await self.context.shutdown()
    
    async def test_get_temp(self):
        """GET /temp — Lectura DHT22 (NoN)"""
        print("\n" + "="*60)
        print("TEST 1: GET /temp (Temperatura y Humedad)")
        print("="*60)
        
        request = Message(code=GET, uri=f"{self.base_uri}/temp")
        try:
            response = await self.context.request(request).response
            print(f"✓ Respuesta recibida (NoN)")
            print(f"  Código: {response.code}")
            print(f"  Payload: {response.payload.decode()}")
            
            # Parsear JSON
            try:
                data = json.loads(response.payload.decode())
                print(f"  └─ Temperatura: {data.get('temp')}°C")
                print(f"  └─ Humedad: {data.get('humidity')}%")
            except json.JSONDecodeError:
                print(f"  └─ Advertencia: No es JSON válido")
        except Exception as e:
            print(f"✗ Error: {e}")
    
    async def test_get_distance(self):
        """GET /dist — Lectura HC-SR04 (NoN)"""
        print("\n" + "="*60)
        print("TEST 2: GET /dist (Distancia Ultrasónica)")
        print("="*60)
        
        request = Message(code=GET, uri=f"{self.base_uri}/dist")
        try:
            response = await self.context.request(request).response
            print(f"✓ Respuesta recibida (NoN)")
            print(f"  Código: {response.code}")
            print(f"  Payload: {response.payload.decode()}")
            
            try:
                data = json.loads(response.payload.decode())
                print(f"  └─ Distancia: {data.get('distance')} cm")
            except json.JSONDecodeError:
                print(f"  └─ Advertencia: No es JSON válido")
        except Exception as e:
            print(f"✗ Error: {e}")
    
    async def test_servo_move(self, angle=90):
        """POST /servo — Mover servo a ángulo (CoN)"""
        print("\n" + "="*60)
        print(f"TEST 3: POST /servo (Mover a {angle}°)")
        print("="*60)
        
        payload = json.dumps({"angle": angle}).encode()
        request = Message(
            code=POST,
            uri=f"{self.base_uri}/servo",
            payload=payload,
            content_format=ContentFormat.JSON
        )
        
        try:
            response = await self.context.request(request).response
            print(f"✓ Respuesta recibida (CoN confirmado)")
            print(f"  Código: {response.code}")
            print(f"  Payload: {response.payload.decode()}")
            
            try:
                data = json.loads(response.payload.decode())
                if data.get('status') == 'ok':
                    print(f"  └─ ✓ Servo movido a {data.get('angle')}°")
                else:
                    print(f"  └─ ✗ Error: {data.get('error')}")
            except json.JSONDecodeError:
                print(f"  └─ Advertencia: No es JSON válido")
        except Exception as e:
            print(f"✗ Error: {e}")
    
    async def test_buzzer_continuous(self, duration_ms=2000, frequency_hz=1000):
        """POST /buzzer — Buzzer continuo (CoN)"""
        print("\n" + "="*60)
        print(f"TEST 4: POST /buzzer (Continuo {duration_ms}ms @ {frequency_hz}Hz)")
        print("="*60)
        
        payload = json.dumps({
            "duration": duration_ms,
            "mode": 0,  # 0 = continuo
            "frequency": frequency_hz
        }).encode()
        
        request = Message(
            code=POST,
            uri=f"{self.base_uri}/buzzer",
            payload=payload,
            content_format=ContentFormat.JSON
        )
        
        try:
            response = await self.context.request(request).response
            print(f"✓ Respuesta recibida (CoN confirmado)")
            print(f"  Código: {response.code}")
            print(f"  Payload: {response.payload.decode()}")
            
            try:
                data = json.loads(response.payload.decode())
                if data.get('status') == 'ok':
                    print(f"  └─ ✓ Buzzer activo {duration_ms}ms")
                else:
                    print(f"  └─ ✗ Error: {data.get('error')}")
            except json.JSONDecodeError:
                print(f"  └─ Advertencia: No es JSON válido")
        except Exception as e:
            print(f"✗ Error: {e}")
    
    async def test_buzzer_intermittent(self, duration_ms=2000, frequency_hz=800):
        """POST /buzzer — Buzzer intermitente (CoN)"""
        print("\n" + "="*60)
        print(f"TEST 5: POST /buzzer (Intermitente {duration_ms}ms @ {frequency_hz}Hz)")
        print("="*60)
        
        payload = json.dumps({
            "duration": duration_ms,
            "mode": 1,  # 1 = intermitente
            "frequency": frequency_hz
        }).encode()
        
        request = Message(
            code=POST,
            uri=f"{self.base_uri}/buzzer",
            payload=payload,
            content_format=ContentFormat.JSON
        )
        
        try:
            response = await self.context.request(request).response
            print(f"✓ Respuesta recibida (CoN confirmado)")
            print(f"  Código: {response.code}")
            print(f"  Payload: {response.payload.decode()}")
            
            try:
                data = json.loads(response.payload.decode())
                if data.get('status') == 'ok':
                    print(f"  └─ ✓ Buzzer intermitente activo {duration_ms}ms")
                else:
                    print(f"  └─ ✗ Error: {data.get('error')}")
            except json.JSONDecodeError:
                print(f"  └─ Advertencia: No es JSON válido")
        except Exception as e:
            print(f"✗ Error: {e}")
    
    async def test_servo_angles(self, angles=[0, 45, 90, 135, 180]):
        """POST /servo — Probar múltiples ángulos"""
        print("\n" + "="*60)
        print(f"TEST 6: Secuencia de ángulos: {angles}")
        print("="*60)
        
        for angle in angles:
            await self.test_servo_move(angle)
            print(f"  [esperando {angle}° antes siguiente...]")
            await asyncio.sleep(1.5)
    
    async def test_error_handling(self):
        """TEST: Manejo de errores"""
        print("\n" + "="*60)
        print("TEST 7: Manejo de Errores")
        print("="*60)
        
        # Ángulo fuera de rango
        print("\n7a) Servo con ángulo inválido (270°)")
        payload = json.dumps({"angle": 270}).encode()
        request = Message(
            code=POST,
            uri=f"{self.base_uri}/servo",
            payload=payload,
            content_format=ContentFormat.JSON
        )
        try:
            response = await self.context.request(request).response
            print(f"  Código: {response.code} (esperado: 400 Bad Request)")
            print(f"  Respuesta: {response.payload.decode()}")
        except Exception as e:
            print(f"  Error: {e}")
        
        # Buzzer sin payload
        print("\n7b) Buzzer sin payload")
        request = Message(code=POST, uri=f"{self.base_uri}/buzzer")
        try:
            response = await self.context.request(request).response
            print(f"  Código: {response.code} (esperado: 400 Bad Request)")
            print(f"  Respuesta: {response.payload.decode()}")
        except Exception as e:
            print(f"  Error: {e}")

async def main():
    # Cambiar IP si es necesario
    server_ip = "192.168.137.92"
    
    if len(sys.argv) > 1:
        server_ip = sys.argv[1]
    
    print(f"\n╔════════════════════════════════════════════════════════════╗")
    print(f"║  CoAP Client Test — ESP32-C6                               ║")
    print(f"║  Servidor: {server_ip}:5683                             ║")
    print(f"╚════════════════════════════════════════════════════════════╝")
    
    tester = CoAPClientTester(server_ip=server_ip)
    await tester.init()
    
    try:
        # Ejecutar todos los tests
        await tester.test_get_temp()
        await asyncio.sleep(0.5)
        
        await tester.test_get_distance()
        await asyncio.sleep(0.5)
        
        await tester.test_servo_move(angle=0)
        await asyncio.sleep(1)
        
        await tester.test_servo_move(angle=90)
        await asyncio.sleep(1)
        
        await tester.test_servo_move(angle=180)
        await asyncio.sleep(1)
        
        await tester.test_buzzer_continuous(duration_ms=2000, frequency_hz=1000)
        await asyncio.sleep(2.5)
        
        await tester.test_buzzer_intermittent(duration_ms=2000, frequency_hz=800)
        await asyncio.sleep(2.5)
        
        # Tests de error
        await tester.test_error_handling()
        
        # Resumen
        print("\n" + "="*60)
        print("✓ TESTS COMPLETADOS")
        print("="*60)
        
    except KeyboardInterrupt:
        print("\n\n⚠️ Tests interrumpidos por usuario")
    finally:
        await tester.cleanup()

if __name__ == "__main__":
    asyncio.run(main())

# Parqueadero Inteligente RFID — Arduino Mega 2560 + Wemos D1 Mini + FastAPI

```
PIR ─┐
RFID ┤                                     Serial1 (9600)              WiFi
LCD  ├──► ARDUINO MEGA 2560 ──[TX1/RX1]──► WEMOS D1 MINI ──HTTP──► FastAPI ──► SQLite
HC-SR04                                                                │
RGB  ┘   (toda la lógica local)             (solo puente)          Panel web (/)
MAX7219
```

| Archivo | Placa | Qué hace |
|---|---|---|
| `firmware/parqueadero_mega/parqueadero_mega.ino` | Arduino Mega 2560 | Todo el sistema: PIR por interrupción, RFID, LCD, matriz, ultrasónico, RGB, máquina de estados |
| `firmware/wemos_bridge/wemos_bridge.ino` | Wemos D1 Mini | Puente Serial ⇄ HTTP (el Mega no tiene WiFi) |
| `backend/main.py` | PC | API FastAPI + SQLite + panel web |

Son **dos sketches porque son dos microcontroladores**: cada uno se compila y se sube por separado.

---

## 1. Revisión de la distribución de pines

La distribución propuesta **es correcta y no tiene conflictos**. Verificaciones hechas:

| Periférico | Pines | Estado |
|---|---|---|
| PIR | D2 | ✅ `digitalPinToInterrupt(2)` = INT4. Es la **única** interrupción externa realmente libre: D18/D19 los usa Serial1 y D20/D21 el I2C. |
| RC522 (SPI HW) | SS 53, SCK 52, MOSI 51, MISO 50, RST 5 | ✅ Es el SPI de hardware del Mega. D5 solo se usa como salida digital. ⚠️ Ver nota de 3.3 V abajo. |
| LCD I2C | SDA 20, SCL 21 | ✅ I2C del Mega. Dirección típica `0x27` (si no ves nada, prueba `0x3F`). |
| MAX7219 | DIN 40, CS 42, CLK 44 | ✅ SPI por software, independiente del RC522. D44 es PWM del Timer5, pero aquí solo se usa digital. |
| LEDs entrada | rojo 6, verde 7 | ✅ Con resistencia de 220 Ω cada uno. |
| HC-SR04 | Trig 8, Echo 9 | ✅ D9 solo entrada; no interfiere con el PWM de D10. |
| LED RGB puesto | R 11, G 10 | ✅ Ambos PWM (D11=Timer1, D10=Timer2). No chocan con nada del proyecto. |
| Wemos | TX1 18 → RX Wemos, RX1 19 ← TX Wemos | ✅ Lógicamente correcto. ⚠️ Ver nota de niveles abajo. |

### Dos advertencias de hardware que sí importan

1. **RC522 = 3.3 V.** Alimentar VCC del RC522 desde el pin **3V3** del Mega (nunca 5 V). Sus entradas
   (SS, SCK, MOSI, RST) reciben 5 V del Mega: lo correcto es un **divisor 1 kΩ / 2 kΩ** en cada una
   (o un módulo level-shifter). MISO va directo al Mega (3.3 V leído como HIGH). Muchos módulos
   sobreviven sin divisor, pero fallan de forma intermitente y se degradan.
2. **Wemos RX = 3.3 V.** El TX1 (D18) del Mega entrega 5 V: pon divisor **1 kΩ / 2 kΩ** antes del RX
   del Wemos. La dirección Wemos TX (3.3 V) → RX1 del Mega funciona directo.
3. **Masa común obligatoria** entre Mega, Wemos y la fuente. El MAX7219 y los LEDs conviene
   alimentarlos con una fuente externa de **5 V / 2 A** (no por USB) y unir GND.

> El sketch del Wemos trae `USE_HW_SERIAL 0`, que usa **D6 (RX) / D5 (TX)** por SoftwareSerial. Es la
> opción recomendada porque deja libre el USB del Wemos para depurar. Si quieres el cableado del
> enunciado (pines RX/TX rotulados), pon `USE_HW_SERIAL 1` y perderás el Monitor Serie del Wemos.

---

## 2. Librerías a instalar (Arduino IDE → Gestor de librerías)

Para el Mega:
- **MFRC522** (by GithubCommunity)
- **LiquidCrystal I2C** (by Frank de Brabander)
- **LedControl** (by Eberhard Fahle)

Para el Wemos: nada extra, solo el core → *Preferencias → Gestor de URLs adicionales*:
`http://arduino.esp8266.com/stable/package_esp8266com_index.json`
y luego *Gestor de tarjetas → esp8266*.

---

## 3. Puesta en marcha (software y hardware a la vez)

### Paso 1 — Levantar el backend primero

```powershell
cd D:\Dev\embebidos\backend
python -m venv venv
venv\Scripts\Activate.ps1
pip install -r requirements.txt
uvicorn main:app --host 0.0.0.0 --port 8000
```

`--host 0.0.0.0` es imprescindible: sin eso el Wemos no puede alcanzar el servidor.
Abre `http://localhost:8000/` → el panel debe decir **“Hardware desconectado”** (aún es correcto).

Anota la IP del PC:

```powershell
ipconfig | Select-String IPv4
```

Y **abre el puerto en el Firewall de Windows** (una sola vez, PowerShell como administrador):

```powershell
New-NetFirewallRule -DisplayName "Parqueadero 8000" -Direction Inbound -LocalPort 8000 -Protocol TCP -Action Allow
```

### Paso 2 — Programar el Wemos (aún desconectado del Mega)

1. Abre `firmware/wemos_bridge/wemos_bridge.ino`.
2. Edita `WIFI_SSID`, `WIFI_PASSWORD` y `SERVER_BASE` con la IP del paso 1
   (ej. `http://192.168.1.42:8000`). Usa WiFi de **2.4 GHz**; el ESP8266 no ve 5 GHz.
3. Placa: *LOLIN(WeMos) D1 R2 & mini*. Sube el sketch.
4. Monitor Serie a **115200**: debe imprimir `IP del Wemos: 192.168.x.x`.
5. Prueba el camino de red sin el Mega, desde el PC:

   ```powershell
   Invoke-RestMethod -Uri http://localhost:8000/scan -Method Post -ContentType application/json -Body '{"uid":"AB:CD:12:34"}'
   ```

   La 1.ª vez responde `entry`, la 2.ª `exit` con la duración. Ya validaste backend + panel.

### Paso 3 — Programar el Mega (sin nada conectado todavía)

1. Abre `firmware/parqueadero_mega/parqueadero_mega.ino`.
2. Placa: *Arduino Mega or Mega 2560*. Sube el sketch.
3. Monitor Serie a **115200** → debe aparecer `MEGA listo.`

### Paso 4 — Armar el hardware por bloques (no todo de golpe)

Con el Mega **desenergizado** en cada cambio, y comprobando en el Monitor Serie tras cada bloque:

| Orden | Bloque | Cómo se comprueba |
|---|---|---|
| 1 | LEDs rojo (D6) y verde (D7) | Al arrancar, el rojo queda encendido fijo |
| 2 | LCD I2C (20/21) | Muestra `Parqueadero RFID / Iniciando...`. Si no: cambia `LCD_ADDR` a `0x3F` y sube el contraste del potenciómetro del backpack |
| 3 | PIR (D2) | Al pasar la mano, la LCD enciende con `Bienvenido! / Acerque su tag` |
| 4 | RC522 (50–53 + D5, **3.3 V**) | Al pasar una tarjeta, el Monitor Serie imprime `UID leido: XX:XX:XX:XX` |
| 5 | HC-SR04 (8/9) + RGB (10/11) | El RGB está verde; al acercar la mano a <15 cm pasa a rojo e imprime `Puesto 1: OCUPADO` |
| 6 | MAX7219 (40/42/44) | Muestra `1` (un puesto libre) y cambia a `0` al ocupar el puesto |
| 7 | Wemos (18/19 + divisor + GND común) | El panel web pasa a **“Hardware conectado”** en ≤ 5 s |

### Paso 5 — Registrar tus tarjetas reales

Los UIDs de demo (`AB:CD:12:34`, etc.) son ficticios. Con el UID que imprimió el paso 4:

1. **Backend** — agrégalo a la tabla `cards` (es la autoridad):

   ```powershell
   cd D:\Dev\embebidos\backend
   python -c "import sqlite3; c=sqlite3.connect('parking.db'); c.execute('INSERT OR REPLACE INTO cards VALUES (?,?,?)', ('A1:B2:C3:D4','Eliath Velasco','ABC-123')); c.commit()"
   ```

2. **Mega** — añádelo también al arreglo `TARJETAS[]` del sketch. Esa lista es el **respaldo
   offline**: si el WiFi se cae, el Mega decide solo (timeout de 1.2 s) y el sistema sigue abriendo.

### Paso 6 — Prueba integrada de punta a punta

1. Acércate a la entrada → el PIR enciende la LCD (interrupción).
2. Pasa la tarjeta → LCD `Verificando...`; el Mega manda `SCAN|uid` al Wemos.
3. Autorizado → rojos OFF, verdes ON 5 s, LCD con el nombre; el panel web suma un registro.
4. Estaciona (mano a <15 cm del HC-SR04) → RGB rojo, matriz baja a `0`, el panel marca el puesto ocupado.
5. Pasa la misma tarjeta otra vez → salida con la duración en minutos.
6. Tarjeta no registrada → rojos parpadeando 3 s y `Acceso denegado`.

---

## 4. Protocolo del enlace Serial1 (9600 bps)

| Dirección | Mensaje | Significado |
|---|---|---|
| Mega → Wemos | `SCAN\|AB:CD:12:34` | Se leyó una tarjeta; pide veredicto |
| Mega → Wemos | `STATE\|libres\|total\|máscara\|presencia` | Telemetría cada 3 s (`máscara` = un `0`/`1` por puesto) |
| Mega → Wemos | `SLOT\|1\|1` | El puesto 1 cambió a ocupado |
| Mega → Wemos | `PIR\|1` / `BOOT\|mega` | Evento de presencia / arranque |
| Wemos → Mega | `AUTH\|1\|Bienvenido Juan` | Autorizado, con el texto para la LCD |
| Wemos → Mega | `AUTH\|0\|Tarjeta no registrada` | Denegado |
| Wemos → Mega | `NET\|1` / `NET\|0` | Estado del WiFi |

Todo con `\n` al final. Si el `AUTH` no llega en 1.2 s, el Mega resuelve con su lista local.

---

## 5. Endpoints del backend

| Método | Ruta | Descripción |
|---|---|---|
| POST | `/scan` | Entrada/salida según el UID (alterna) |
| POST | `/telemetry` | Estado en vivo del hardware (lo manda el Mega cada 3 s) |
| POST | `/event` | Eventos sueltos: `PIR`, `SLOT`, `BOOT` |
| GET | `/status` | Resumen para el panel: libres, ocupados, puestos, PIR, online |
| GET | `/sessions` | Todos los registros |
| GET | `/inside` | Vehículos dentro ahora |
| GET | `/` | Panel web |

---

## 6. Fallas típicas

| Síntoma | Causa probable |
|---|---|
| El RC522 nunca lee | Alimentado a 5 V, o falta el divisor en SCK/MOSI/SS, o cables largos. Debe ir a 3.3 V con cables cortos |
| LCD encendida pero sin texto | Dirección I2C errada (`0x3F`) o potenciómetro de contraste al extremo |
| El PIR dispara solo | Es normal en los primeros ~60 s (calibración). Baja la sensibilidad del potenciómetro del módulo |
| La matriz muestra basura | DIN/CLK/CS cruzados, o alimentación insuficiente (usar fuente externa) |
| Panel dice “Hardware desconectado” | Falta GND común Mega–Wemos, TX/RX cruzados al revés, o baudios distintos (ambos a 9600) |
| El Wemos no conecta al WiFi | Red de 5 GHz, o contraseña/SSID con acentos |
| El Wemos conecta pero el POST falla | `SERVER_BASE` con IP vieja, uvicorn sin `--host 0.0.0.0`, o Firewall de Windows |
| Ultrasónico salta entre libre/ocupado | Superficie inclinada. Sube `UMBRAL_OCUPADO_CM` o `MUESTRAS_ESTABLES` |

## 7. Ampliar a más puestos

En el sketch del Mega, sube `NUM_PUESTOS` y agrega los pines en las cuatro tablas
(`PIN_TRIG`, `PIN_ECHO`, `PIN_RGB_R`, `PIN_RGB_G`). Todo lo demás —matriz, telemetría, panel web—
se ajusta solo. Usa pines PWM para el RGB (D2–D13, D44–D46) y digitales cualesquiera para el sonar.

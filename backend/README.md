
```powershell
git clone <url-del-repo>
cd embebidos/backend

python -m venv venv
venv\Scripts\Activate.ps1
pip install -r requirements.txt

uvicorn main:app --reload --host 0.0.0.0 --port 8000
```

http://localhost:8000/

 
```
Tarjeta -> RC522 -> ESP8266 --POST /scan {uid}--> FastAPI <-> SQLite
                                                     |
                                               Panel web (/)
```

1. El ESP8266 lee el UID de la tarjeta.
2. Hace un POST a `/scan` con `{"uid": "..."}`.
3. El backend decide:
   - Sin sesión abierta para ese UID → **entrada** (estado `dentro`).
   - Con sesión abierta → **salida** (estado `fuera`, calcula duración).
   - UID no registrado → `denied`.
4. Responde con el estado, y el panel web se actualiza solo.

## Programar el ESP8266

1. Conectar al WiFi (`ESP8266WiFi.h`).
2. Leer el UID de la tarjeta con el RC522 (`MFRC522.h`, por SPI).
3. POST a `http://<IP-del-PC>:8000/scan` con el body `{"uid":"..."}`
   (`ESP8266HTTPClient.h`).
4. Leer la respuesta JSON y mostrarla (LCD, LED, etc.).


## Probar en Postman

- `POST`
- `http://localhost:8000/scan`
- Body: **raw** → tipo **JSON** 
- Contenido:

  ```json
  { "uid": "AB:CD:12:34" }
  ```

Enviar **dos veces** con el mismo UID: la 1ª da `entry`, la 2ª da `exit`
con la duración. Un UID que no exista da `denied`.

## Endpoints

| Método | Ruta        | Descripción                          |
|--------|-------------|--------------------------------------|
| POST   | `/scan`     | Registra entrada/salida según el UID |
| GET    | `/sessions` | Lista todos los registros            |
| GET    | `/inside`   | Vehículos dentro ahora               |
| GET    | `/`         | Panel web                            |

## Tarjetas de demo

Son datos de fake de prueba
Cambiar por UIDs reales.

| Nombre      | UID           |
|-------------|---------------|
| Juan Perez  | `AB:CD:12:34` |
| Maria Lopez | `11:22:33:44` |
| Carlos Ruiz | `DE:AD:BE:EF` |

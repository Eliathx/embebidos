
```powershell
git clone <url-del-repo>
cd embebidos/backend

python -m venv venv
venv\Scripts\Activate.ps1
pip install -r requirements.txt

uvicorn main:app --reload --host 0.0.0.0 --port 8000
```

http://localhost:8000/

> Guía completa de hardware + firmware: ver el [README raíz](../README.md).

```
Tarjeta -> RC522 -> MEGA 2560 --Serial1--> Wemos D1 Mini --POST /scan--> FastAPI <-> SQLite
                                                                            |
                                                                      Panel web (/)
```

1. El Mega lee el UID de la tarjeta y lo manda al Wemos por Serial1.
2. Hace un POST a `/scan` con `{"uid": "..."}`.
3. El backend decide:
   - Sin sesión abierta para ese UID → **entrada** (estado `dentro`).
   - Con sesión abierta → **salida** (estado `fuera`, calcula duración).
   - UID no registrado → `denied`.
4. Responde con el estado, y el panel web se actualiza solo.


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

| Método | Ruta         | Descripción                                   |
|--------|--------------|-----------------------------------------------|
| POST   | `/scan`      | Registra entrada/salida según el UID          |
| POST   | `/telemetry` | Estado en vivo del hardware (Mega, cada 3 s)  |
| POST   | `/event`     | Eventos sueltos: `PIR`, `SLOT`, `BOOT`        |
| GET    | `/status`    | Resumen del panel: libres/ocupados/PIR/online |
| GET    | `/sessions`  | Lista todos los registros                     |
| GET    | `/inside`    | Vehículos dentro ahora                        |
| GET    | `/`          | Panel web                                     |

## Tarjetas de demo

Son datos de fake de prueba
Cambiar por UIDs reales.

| Nombre      | UID           |
|-------------|---------------|
| Juan Perez  | `AB:CD:12:34` |
| Maria Lopez | `11:22:33:44` |
| Carlos Ruiz | `DE:AD:BE:EF` |

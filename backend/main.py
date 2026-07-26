"""
Backend del parqueadero inteligente.

Arduino MEGA 2560 (logica local) --Serial1--> Wemos D1 Mini --HTTP--> aqui

- POST /scan      : UID de la tarjeta. Sin sesion abierta -> ENTRADA
                    ('dentro'); con sesion abierta -> SALIDA ('fuera')
                    y calcula la duracion.
- POST /telemetry : estado en vivo del hardware (puestos libres/ocupados,
                    presencia del PIR). Lo envia el MEGA cada 3 s.
- POST /event     : eventos sueltos (PIR, cambio de puesto, arranque).
- GET  /status    : lo que consume el panel web para las tarjetas en vivo.
"""

import sqlite3
from datetime import datetime, timezone
from pathlib import Path

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse
from pydantic import BaseModel

BASE_DIR = Path(__file__).parent
DB_PATH = BASE_DIR / "parking.db"

# Base de datos SQLite

def get_db():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def init_db():
    conn = get_db()
    conn.executescript(
        """
        CREATE TABLE IF NOT EXISTS cards (
            uid   TEXT PRIMARY KEY,
            name  TEXT NOT NULL,
            plate TEXT
        );
        CREATE TABLE IF NOT EXISTS sessions (
            id           INTEGER PRIMARY KEY AUTOINCREMENT,
            uid          TEXT    NOT NULL,
            name         TEXT,
            entry_time   TEXT    NOT NULL,
            exit_time    TEXT,
            status       TEXT    NOT NULL DEFAULT 'dentro',
            duration_min REAL
        );
        """
    )
    # Tarjetas de demo
    if conn.execute("SELECT COUNT(*) AS n FROM cards").fetchone()["n"] == 0:
        conn.executemany(
            "INSERT INTO cards (uid, name, plate) VALUES (?, ?, ?)",
            [
                ("AB:CD:12:34", "Juan Perez",  "PBX-123"),
                ("11:22:33:44", "Maria Lopez", "GHZ-456"),
                ("DE:AD:BE:EF", "Carlos Ruiz", "TAX-789"),
            ],
        )
    conn.commit()
    conn.close()


def now():
    return datetime.now(timezone.utc)


# Estado en vivo del hardware (volatil: se repuebla cada 3 s con /telemetry)
HW_STATE = {
    "free": None,
    "total": None,
    "slots": "",
    "presence": 0,
    "last_seen": None,
    "events": [],  # ultimos eventos, mas nuevos primero
}


def hw_online() -> bool:
    """El MEGA manda telemetria cada 3 s: 10 s sin datos => offline."""
    if HW_STATE["last_seen"] is None:
        return False
    delta = now() - datetime.fromisoformat(HW_STATE["last_seen"])
    return delta.total_seconds() < 10

app = FastAPI(title="Parqueadero RFID")
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)
init_db()


class ScanIn(BaseModel):
    uid: str


class TelemetryIn(BaseModel):
    free: int
    total: int
    slots: str = ""      # mascara de ocupacion, un caracter por puesto: "010"
    presence: int = 0    # 1 si el PIR detecto un vehiculo hace poco


class EventIn(BaseModel):
    kind: str            # SLOT / PIR / BOOT
    data: str = ""


@app.post("/scan")
def scan(payload: ScanIn):
    """Endpoint que llama el ESP8266 (o el mock) cuando pasan una tarjeta."""
    uid = payload.uid.strip().upper()
    conn = get_db()

    card = conn.execute(
        "SELECT * FROM cards WHERE upper(uid) = ?", (uid,)
    ).fetchone()

    if card is None:
        conn.close()
        return {
            "action": "denied",
            "granted": False,
            "message": "Tarjeta no registrada",
            "rgb": "red",
            "matrix": "cross",
        }

    name = card["name"]
    open_session = conn.execute(
        "SELECT * FROM sessions "
        "WHERE upper(uid) = ? AND exit_time IS NULL "
        "ORDER BY id DESC LIMIT 1",
        (uid,),
    ).fetchone()

    if open_session is None:
        conn.execute(
            "INSERT INTO sessions (uid, name, entry_time, status) "
            "VALUES (?, ?, ?, 'dentro')",
            (uid, name, now().isoformat()),
        )
        conn.commit()
        conn.close()
        return {
            "action": "entry",
            "granted": True,
            "name": name,
            "message": f"Bienvenido {name}",
            "rgb": "green",
            "matrix": "arrow_up",
        }

    entry_t = datetime.fromisoformat(open_session["entry_time"])
    exit_t = now()
    duration_min = round((exit_t - entry_t).total_seconds() / 60, 1)
    conn.execute(
        "UPDATE sessions "
        "SET exit_time = ?, status = 'fuera', duration_min = ? "
        "WHERE id = ?",
        (exit_t.isoformat(), duration_min, open_session["id"]),
    )
    conn.commit()
    conn.close()
    return {
        "action": "exit",
        "granted": True,
        "name": name,
        "message": f"Hasta luego {name} ({duration_min} min)",
        "duration_min": duration_min,
        "rgb": "blue",
        "matrix": "arrow_down",
    }


@app.post("/telemetry")
def telemetry(payload: TelemetryIn):
    """Estado en vivo que envia el MEGA (via Wemos) cada 3 segundos."""
    HW_STATE.update(
        free=payload.free,
        total=payload.total,
        slots=payload.slots,
        presence=payload.presence,
        last_seen=now().isoformat(),
    )
    return {"ok": True}


@app.post("/event")
def event(payload: EventIn):
    """Eventos sueltos del hardware: PIR, cambio de puesto, arranque."""
    HW_STATE["last_seen"] = now().isoformat()
    HW_STATE["events"].insert(
        0, {"kind": payload.kind, "data": payload.data, "at": now().isoformat()}
    )
    del HW_STATE["events"][20:]          # solo guardamos los ultimos 20
    return {"ok": True}


@app.get("/status")
def status():
    """Resumen para el panel web: hardware + ocupacion + vehiculos dentro."""
    conn = get_db()
    dentro = conn.execute(
        "SELECT COUNT(*) AS n FROM sessions WHERE exit_time IS NULL"
    ).fetchone()["n"]
    total_reg = conn.execute("SELECT COUNT(*) AS n FROM sessions").fetchone()["n"]
    conn.close()

    total = HW_STATE["total"]
    free = HW_STATE["free"]
    return {
        "online": hw_online(),
        "last_seen": HW_STATE["last_seen"],
        "free": free,
        "total": total,
        "occupied": (total - free) if (total is not None and free is not None) else None,
        "slots": [c == "1" for c in HW_STATE["slots"]],
        "presence": bool(HW_STATE["presence"]),
        "inside": dentro,
        "records": total_reg,
        "events": HW_STATE["events"][:8],
    }


@app.get("/sessions")
def list_sessions():
    """Todos los registros, mas nuevos primero (lo que consume el panel)."""
    conn = get_db()
    rows = conn.execute("SELECT * FROM sessions ORDER BY id DESC").fetchall()
    conn.close()
    return [dict(r) for r in rows]


@app.get("/inside")
def inside():
    """Vehiculos actualmente dentro."""
    conn = get_db()
    rows = conn.execute(
        "SELECT * FROM sessions WHERE exit_time IS NULL ORDER BY id DESC"
    ).fetchall()
    conn.close()
    return {"count": len(rows), "sessions": [dict(r) for r in rows]}


@app.get("/")
def panel():
    """Panel admin (frontend simple)."""
    return FileResponse(BASE_DIR / "static" / "index.html")

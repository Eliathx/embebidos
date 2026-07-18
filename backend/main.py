"""
POST /scan con el UID de la tarjeta.
- Si la tarjeta NO tiene una sesion abierta -> registra ENTRADA (status 'dentro')
- Si la tarjeta SI tiene una sesion abierta -> registra SALIDA (status 'fuera')
  y calcula la duracion.

El firmware (ESP8266) solo lee el UID, hace el POST y pinta lo que
venga en la respuesta (mensaje LCD, color RGB, icono de la matriz).
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

import sqlite3
import time

SCHEMA = """
CREATE TABLE IF NOT EXISTS devices (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  uid TEXT, name TEXT NOT NULL, address TEXT NOT NULL, token_enc TEXT NOT NULL,
  fw_version TEXT, created INTEGER NOT NULL, last_seen INTEGER, last_seq INTEGER NOT NULL DEFAULT 0,
  online INTEGER NOT NULL DEFAULT 0, status_json TEXT
);
CREATE TABLE IF NOT EXISTS status_samples (
  device_id INTEGER NOT NULL, ts INTEGER NOT NULL, power TEXT, battery_pct REAL,
  bottle_pct REAL, float TEXT, mode TEXT
);
CREATE TABLE IF NOT EXISTS events (
  device_id INTEGER NOT NULL, seq INTEGER NOT NULL, ts INTEGER NOT NULL, type TEXT NOT NULL,
  data_json TEXT NOT NULL, UNIQUE(device_id, seq)
);
CREATE TABLE IF NOT EXISTS alerts (
  id INTEGER PRIMARY KEY AUTOINCREMENT, device_id INTEGER NOT NULL, type TEXT NOT NULL,
  state TEXT NOT NULL, opened_ts INTEGER NOT NULL, closed_ts INTEGER, ack_by TEXT, ack_ts INTEGER
);
CREATE TABLE IF NOT EXISTS audit_log (
  id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER NOT NULL, actor TEXT NOT NULL,
  device_id INTEGER, action TEXT NOT NULL, params_json TEXT, result TEXT
);
"""


def connect(path: str) -> sqlite3.Connection:
    conn = sqlite3.connect(path, timeout=10)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    return conn


def init(path: str) -> None:
    conn = connect(path)
    conn.executescript(SCHEMA)
    conn.commit()
    conn.close()


def now() -> int:
    return int(time.time())

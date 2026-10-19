"""Hub logic: device I/O, event ingest, polling, offline detection, audited commands."""
import asyncio
import hashlib
import hmac
import json
import sqlite3

import httpx

from . import alerts, crypto
from .config import Settings
from .db import connect, now

SIG_HEADER = "X-DrainBrain-Signature"
BACKFILL_LIMIT = 200


class DeviceError(Exception):
    def __init__(self, status: int, code: str, message: str):
        super().__init__(f"{code}: {message}")
        self.status, self.code, self.message = status, code, message


def base_url(address: str) -> str:
    a = address.strip().rstrip("/")
    return (a if "://" in a else "http://" + a) + "/api/v1"


async def call(address: str, token: str | None, method: str, path: str, body=None, timeout: float = 5.0):
    headers = {"Authorization": f"Bearer {token}"} if token else {}
    try:
        async with httpx.AsyncClient(timeout=timeout) as c:
            r = await c.request(method, base_url(address) + path, json=body, headers=headers)
    except httpx.HTTPError as e:
        raise DeviceError(0, "unreachable", str(e) or type(e).__name__) from e
    try:
        data = r.json()
    except ValueError:
        data = {}
    if r.status_code >= 400:
        err = data.get("error", {}) if isinstance(data, dict) else {}
        raise DeviceError(r.status_code, err.get("code", "device_error"), err.get("message", r.text[:200]))
    return data


def sign(token: str, body: bytes) -> str:
    return hmac.new(token.encode(), body, hashlib.sha256).hexdigest()


def verify(token: str, body: bytes, signature: str | None) -> bool:
    return bool(signature) and hmac.compare_digest(sign(token, body), signature.strip().lower())


def dev_token(settings: Settings, dev: sqlite3.Row) -> str:
    return crypto.decrypt(settings.secret_key, dev["token_enc"])


# ---- adding devices -------------------------------------------------------

async def add_device(settings: Settings, name: str | None, address: str, token: str, actor: str = "admin") -> int:
    info = await call(address, None, "GET", "/info")
    token_enc = crypto.encrypt(settings.secret_key, token)
    conn = connect(settings.db_path)
    try:
        cur = conn.execute(
            "INSERT INTO devices(uid,name,address,token_enc,fw_version,created) VALUES(?,?,?,?,?,?)",
            (info.get("id"), name or info.get("name") or address, address, token_enc, info.get("firmware"), now()))
        dev_id = cur.lastrowid
        conn.execute("INSERT INTO audit_log(ts,actor,device_id,action,params_json,result) VALUES(?,?,?,?,?,?)",
                     (now(), actor, dev_id, "device_add", json.dumps({"address": address}), "ok"))
        conn.commit()
    finally:
        conn.close()
    if settings.public_url:  # best effort: register our hook URL on the device
        try:
            url = f"{settings.public_url}/hook/{dev_id}"
            cfg = await call(address, token, "GET", "/config")
            hooks = list(cfg.get("webhooks", []))
            if url not in hooks:
                await call(address, token, "PUT", "/config", {"webhooks": hooks + [url]})
        except DeviceError:
            pass
    return dev_id


# ---- event ingest ---------------------------------------------------------

def ingest_event(conn: sqlite3.Connection, dev_id: int, ev: dict) -> bool:
    """Store an event, deduplicating on (device_id, seq). Returns True if new."""
    seq = int(ev["seq"])
    ts = int(ev.get("time") or now())
    cur = conn.execute("INSERT OR IGNORE INTO events(device_id,seq,ts,type,data_json) VALUES(?,?,?,?,?)",
                       (dev_id, seq, ts, str(ev["type"]), json.dumps(ev.get("data") or {})))
    if cur.rowcount == 0:
        return False
    alerts.apply_event(conn, dev_id, ev["type"], ts)
    conn.execute("UPDATE devices SET last_seq=MAX(last_seq,?) WHERE id=?", (seq, dev_id))
    return True


def touch_seen(conn: sqlite3.Connection, dev_id: int) -> None:
    conn.execute("UPDATE devices SET last_seen=?, online=1 WHERE id=?", (now(), dev_id))
    alerts.close_alert(conn, dev_id, "offline")


async def backfill(settings: Settings, dev: sqlite3.Row) -> int:
    token, since, n = dev_token(settings, dev), dev["last_seq"], 0
    while True:
        data = await call(dev["address"], token, "GET", f"/events?since={since}&limit={BACKFILL_LIMIT}")
        evs = data.get("events", [])
        conn = connect(settings.db_path)
        try:
            for ev in sorted(evs, key=lambda e: e["seq"]):
                n += ingest_event(conn, dev["id"], ev)
                since = max(since, ev["seq"])
            conn.commit()
        finally:
            conn.close()
        if len(evs) < BACKFILL_LIMIT:
            return n


# ---- polling --------------------------------------------------------------

async def poll_device(settings: Settings, dev_id: int) -> bool:
    conn = connect(settings.db_path)
    try:
        dev = conn.execute("SELECT * FROM devices WHERE id=?", (dev_id,)).fetchone()
    finally:
        conn.close()
    if dev is None:
        return False
    try:
        st = await call(dev["address"], dev_token(settings, dev), "GET", "/status")
    except DeviceError:
        return False
    was_offline = not dev["online"]
    if was_offline or st.get("event_seq", 0) > dev["last_seq"]:
        try:
            await backfill(settings, dev)
        except DeviceError:
            pass
    conn = connect(settings.db_path)
    try:
        b = st.get("bottle") or {}
        conn.execute("UPDATE devices SET status_json=? WHERE id=?", (json.dumps(st), dev_id))
        conn.execute("INSERT INTO status_samples VALUES(?,?,?,?,?,?,?)",
                     (dev_id, now(), st.get("power"), st.get("battery_pct"), b.get("pct"), st.get("float"), st.get("mode")))
        touch_seen(conn, dev_id)
        alerts.reconcile_status(conn, dev_id, st)
        conn.commit()
    finally:
        conn.close()
    return True


def check_offline(settings: Settings, at: int | None = None) -> list[int]:
    """Mark devices silent for 3 poll intervals offline and open an alert. Returns newly offline device ids."""
    at = at or now()
    limit = 3 * settings.poll_interval
    conn = connect(settings.db_path)
    out = []
    try:
        for d in conn.execute("SELECT id,last_seen,created FROM devices").fetchall():
            if at - (d["last_seen"] or d["created"]) > limit:
                conn.execute("UPDATE devices SET online=0 WHERE id=?", (d["id"],))
                if alerts.open_alert(conn, d["id"], "offline", at):
                    out.append(d["id"])
        conn.commit()
    finally:
        conn.close()
    return out


async def poll_all(settings: Settings) -> None:
    conn = connect(settings.db_path)
    ids = [r["id"] for r in conn.execute("SELECT id FROM devices")]
    conn.close()
    await asyncio.gather(*(poll_device(settings, i) for i in ids))
    check_offline(settings)


async def poll_loop(settings: Settings) -> None:
    while True:
        try:
            await poll_all(settings)
        except Exception:  # keep the poller alive
            pass
        await asyncio.sleep(settings.poll_interval)


# ---- commands (proxied to the device and audited) -------------------------

async def command(settings: Settings, dev: sqlite3.Row, actor: str, action: str, method: str, path: str, body=None):
    result, err = "ok", None
    try:
        data = await call(dev["address"], dev_token(settings, dev), method, path, body)
    except DeviceError as e:
        result, err, data = f"error {e.status} {e.code}", e, None
    if method != "GET":  # reads are not audited; every write is
        conn = connect(settings.db_path)
        try:
            conn.execute("INSERT INTO audit_log(ts,actor,device_id,action,params_json,result) VALUES(?,?,?,?,?,?)",
                         (now(), actor, dev["id"], action, json.dumps(body), result))
            conn.commit()
        finally:
            conn.close()
    if err:
        raise err
    if method != "GET":
        await poll_device(settings, dev["id"])
    return data

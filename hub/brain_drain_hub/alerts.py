"""Alert state machine. Alerts open/close from pushed events and from polled status; both paths are idempotent."""
import sqlite3

from .db import now

BOTTLE_LOW_PCT = 15
BATTERY_LOW_PCT = 20
BATTERY_CLEAR_PCT = 25  # hysteresis

OPEN_ON = {
    "float_alarm": "float_alarm", "power_lost": "power_lost", "battery_low": "battery_low",
    "battery_critical": "battery_low", "bottle_low": "bottle_low", "bottle_empty": "bottle_empty",
}
CLOSE_ON = {
    "float_cleared": ["float_alarm"],
    "power_restored": ["power_lost", "battery_low"],
    "bottle_inserted": ["bottle_low", "bottle_empty"],
}
TYPES = ["float_alarm", "power_lost", "battery_low", "bottle_low", "bottle_empty", "offline"]


def open_alert(conn: sqlite3.Connection, device_id: int, atype: str, ts: int | None = None) -> bool:
    if conn.execute("SELECT 1 FROM alerts WHERE device_id=? AND type=? AND state='open'", (device_id, atype)).fetchone():
        return False
    conn.execute("INSERT INTO alerts(device_id,type,state,opened_ts) VALUES(?,?,'open',?)", (device_id, atype, ts or now()))
    return True


def close_alert(conn: sqlite3.Connection, device_id: int, atype: str, ts: int | None = None) -> bool:
    cur = conn.execute("UPDATE alerts SET state='closed', closed_ts=? WHERE device_id=? AND type=? AND state='open'",
                       (ts or now(), device_id, atype))
    return cur.rowcount > 0


def apply_event(conn: sqlite3.Connection, device_id: int, etype: str, ts: int | None = None) -> None:
    if etype in OPEN_ON:
        open_alert(conn, device_id, OPEN_ON[etype], ts)
    for atype in CLOSE_ON.get(etype, []):
        close_alert(conn, device_id, atype, ts)


def reconcile_status(conn: sqlite3.Connection, device_id: int, st: dict) -> None:
    """Derive conditions from a polled /status document (safety net for missed pushes)."""
    def set_(atype: str, active: bool) -> None:
        (open_alert if active else close_alert)(conn, device_id, atype)

    set_("float_alarm", st.get("float") == "alarm")
    on_batt = st.get("power") == "battery"
    set_("power_lost", on_batt)
    pct = st.get("battery_pct")
    if pct is not None:
        if on_batt and pct <= BATTERY_LOW_PCT:
            open_alert(conn, device_id, "battery_low")
        elif pct >= BATTERY_CLEAR_PCT or not on_batt:
            close_alert(conn, device_id, "battery_low")
    b = st.get("bottle") or {}
    if b.get("present", True) and b.get("pct") is not None:
        empty = b["pct"] <= 0 or b.get("remaining_ml", 1) <= 0
        low = b["pct"] <= BOTTLE_LOW_PCT
        set_("bottle_empty", empty)
        set_("bottle_low", low and not empty)


def acknowledge(conn: sqlite3.Connection, alert_id: int, by: str) -> bool:
    cur = conn.execute("UPDATE alerts SET ack_by=?, ack_ts=? WHERE id=? AND ack_by IS NULL", (by, now(), alert_id))
    return cur.rowcount > 0

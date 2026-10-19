import asyncio
import html
import json
import sqlite3
import time

from fastapi.testclient import TestClient

from brain_drain_hub import core
from brain_drain_hub.app import create_app
from brain_drain_hub.db import connect
from conftest import TOKEN, authed, free_port, wait_for
import sim_device


def signed(sim_dev, ev: dict):
    body = json.dumps(ev, separators=(",", ":")).encode()
    return body, {"X-DrainBrain-Signature": core.sign(TOKEN, body), "Content-Type": "application/json"}


def event(sim, seq, etype, data=None):
    return {"device_id": sim.sim.device_id, "seq": seq, "time": int(time.time()), "type": etype, "data": data or {}}


def alert_types(client, state="open"):
    return sorted(a["type"] for a in client.get("/api/v1/alerts", params={"state": state}).json())


# ---- security / storage ---------------------------------------------------

def test_auth_required_and_token_encrypted(client, settings, dev_id):
    anon = TestClient(client.app)
    assert anon.get("/").status_code == 401
    assert anon.get("/api/v1/devices").status_code == 401
    assert anon.get("/healthz").status_code == 200
    row = connect(settings.db_path).execute("SELECT token_enc FROM devices").fetchone()
    assert TOKEN not in row["token_enc"]
    assert "token_enc" not in client.get("/api/v1/devices").json()[0]


# ---- webhook: HMAC + dedupe ----------------------------------------------

def test_hook_hmac_and_dedupe(client, sim, dev_id, settings):
    body, hdr = signed(sim, event(sim, 1, "boot"))
    assert client.post(f"/hook/{dev_id}", content=body, headers=hdr).json() == {"ok": True, "duplicate": False}
    assert client.post(f"/hook/{dev_id}", content=body, headers=hdr).json() == {"ok": True, "duplicate": True}
    bad = dict(hdr, **{"X-DrainBrain-Signature": "00" * 32})
    assert client.post(f"/hook/{dev_id}", content=body, headers=bad).status_code == 401
    assert client.post(f"/hook/{dev_id}", content=body).status_code == 401
    tampered = body.replace(b"boot", b"fault")
    assert client.post(f"/hook/{dev_id}", content=tampered, headers=hdr).status_code == 401
    assert client.post("/hook/999", content=body, headers=hdr).status_code == 404
    n = connect(settings.db_path).execute("SELECT COUNT(*) FROM events WHERE seq=1").fetchone()[0]
    assert n == 1


def test_hook_rejects_wrong_device_uid(client, sim, dev_id):
    ev = event(sim, 1, "boot")
    ev["device_id"] = "someone-else"
    body, hdr = signed(sim, ev)
    assert client.post(f"/hook/{dev_id}", content=body, headers=hdr).status_code == 400


def test_alert_open_and_close_from_events(client, sim, dev_id):
    def push(seq, etype, data=None):
        body, hdr = signed(sim, event(sim, seq, etype, data))
        assert client.post(f"/hook/{dev_id}", content=body, headers=hdr).status_code == 200

    start = sim.sim.seq  # keep seq numbers ahead of anything the poll backfilled
    push(start + 1, "float_alarm")
    push(start + 2, "power_lost")
    push(start + 3, "bottle_low")
    assert {"float_alarm", "power_lost", "bottle_low"} <= set(alert_types(client))
    push(start + 4, "float_cleared")
    push(start + 5, "power_restored")
    push(start + 6, "bottle_inserted")
    assert alert_types(client) == []
    assert {"float_alarm", "power_lost", "bottle_low"} <= set(alert_types(client, "closed"))


def test_alert_not_duplicated_and_ack(client, sim, dev_id):
    for seq in (sim.sim.seq + 1, sim.sim.seq + 2):
        body, hdr = signed(sim, event(sim, seq, "float_alarm"))
        client.post(f"/hook/{dev_id}", content=body, headers=hdr)
    alerts = client.get("/api/v1/alerts").json()
    assert [a["type"] for a in alerts] == ["float_alarm"]
    assert client.post(f"/api/v1/alerts/{alerts[0]['id']}/ack").json() == {"acknowledged": True}
    assert client.get("/api/v1/alerts").json()[0]["ack_by"] == "admin"


# ---- polling + backfill ----------------------------------------------------

def test_poll_status_updates_and_backfills(client, sim, dev_id, settings):
    sim.sim.fault("float_alarm")
    sim.sim.fault("power_lost")
    assert asyncio.run(core.poll_device(settings, dev_id))
    d = client.get("/api/v1/devices").json()[0]
    assert d["online"] == 1 and d["status"]["float"] == "alarm" and d["state"] == "alarm"
    assert d["last_seq"] == sim.sim.seq
    assert {"float_alarm", "power_lost"} <= set(alert_types(client))
    types = [e["type"] for e in client.get(f"/api/v1/devices/{dev_id}/events").json()]
    assert "float_alarm" in types and "power_lost" in types
    # recovery observed by poll alone closes status-derived alerts
    sim.sim.fault("power_restored")
    asyncio.run(core.poll_device(settings, dev_id))
    assert "power_lost" not in alert_types(client)


def test_status_alerts_for_bottle_and_battery(client, sim, dev_id, settings):
    client.post(f"/api/v1/devices/{dev_id}/dispense", json={"ml": 450})  # 10% left
    assert "bottle_low" in alert_types(client)
    client.post(f"/api/v1/devices/{dev_id}/dispense", json={"ml": 100})  # clamps to 50 -> empty
    assert "bottle_empty" in alert_types(client) and "bottle_low" not in alert_types(client)
    client.post(f"/api/v1/devices/{dev_id}/bottle/reset", json={})
    assert alert_types(client) == []


def test_dashboard_pages_render(client, sim, dev_id):
    assert "Fleet" in client.get("/").text
    sim.sim.fault("float_alarm")
    asyncio.run(core.poll_device(client.app.state.settings, dev_id))
    assert "card alarm" in client.get("/fragments/fleet").text
    page = client.get(f"/devices/{dev_id}").text
    assert "Dispense now" in page and "Schedule" in page
    assert "float_alarm" in client.get("/alerts").text


# ---- commands + audit ------------------------------------------------------

def test_dispense_proxy_and_audit(client, sim, dev_id):
    r = client.post(f"/api/v1/devices/{dev_id}/dispense", json={"ml": 25})
    assert r.status_code == 200 and r.json()["actual_ml"] == 25
    assert sim.sim.bottle["remaining_ml"] == 475
    assert client.get("/api/v1/devices").json()[0]["status"]["bottle"]["remaining_ml"] == 475  # refreshed
    audit = client.get(f"/api/v1/devices/{dev_id}/audit").json()
    assert audit[0]["action"] == "dispense" and audit[0]["actor"] == "admin" and audit[0]["result"] == "ok"
    assert json.loads(audit[0]["params_json"]) == {"ml": 25}


def test_command_errors_pass_through_and_are_audited(client, sim, dev_id):
    sim.sim.fault("bottle_removed")
    r = client.post(f"/api/v1/devices/{dev_id}/dispense", json={"ml": 5})
    assert r.status_code == 409 and "bottle_missing" in r.json()["error"]["message"]
    assert client.get(f"/api/v1/devices/{dev_id}/audit").json()[0]["result"] == "error 409 bottle_missing"
    assert client.post(f"/api/v1/devices/{dev_id}/bottle/reset", json={"capacity_ml": 400}).status_code == 200
    assert sim.sim.bottle == {"capacity_ml": 400.0, "remaining_ml": 400.0, "present": True}
    sim.stop()
    assert client.post(f"/api/v1/devices/{dev_id}/dispense", json={"ml": 5}).status_code == 502
    assert "unreachable" in client.get(f"/api/v1/devices/{dev_id}/audit").json()[0]["result"]


def test_schedule_get_put_and_ui_form(client, sim, dev_id):
    doc = {"entries": [{"days": ["mon", "thu"], "time": "09:00", "ml": 10}]}
    assert client.put(f"/api/v1/devices/{dev_id}/schedule", json=doc).status_code == 200
    assert client.get(f"/api/v1/devices/{dev_id}/schedule").json() == doc
    assert '"time": "09:00"' in html.unescape(client.get(f"/devices/{dev_id}").text)
    r = client.post(f"/ui/devices/{dev_id}/schedule", data={"schedule": "{not json"})
    assert "Invalid JSON" in r.text
    r = client.post(f"/ui/devices/{dev_id}/dispense", data={"ml": "5"})
    assert "Dispensed 5 mL" in r.text
    actions = [a["action"] for a in client.get(f"/api/v1/devices/{dev_id}/audit").json()]
    assert "schedule_put" in actions and "dispense" in actions
    assert "schedule_get" not in actions  # reads are not audited


# ---- offline detection -----------------------------------------------------

def test_offline_detection_and_recovery(client, sim, dev_id, settings):
    assert core.check_offline(settings) == []                       # fresh: still online
    assert core.check_offline(settings, int(time.time()) + 3 * 60 - 5) == []  # < 3 intervals
    assert core.check_offline(settings, int(time.time()) + 3 * 60 + 5) == [dev_id]
    assert "offline" in alert_types(client)
    assert client.get("/api/v1/devices").json()[0]["state"] == "offline"
    assert core.check_offline(settings, int(time.time()) + 600) == []  # no duplicate alert
    assert alert_types(client).count("offline") == 1
    assert asyncio.run(core.poll_device(settings, dev_id))             # device answers again
    assert "offline" not in alert_types(client)


def test_poll_failure_when_device_down(client, sim, dev_id, settings):
    sim.stop()
    assert asyncio.run(core.poll_device(settings, dev_id)) is False


# ---- end to end: simulator pushes webhooks to a live hub --------------------

def test_end_to_end_webhook_push_and_catch_up(make_sim, settings):
    hub_port = free_port()
    import uvicorn, threading
    hub = uvicorn.Server(uvicorn.Config(create_app(settings), port=hub_port, log_level="warning"))
    t = threading.Thread(target=hub.run, daemon=True)
    sim = make_sim("e2e", hook_urls=[f"http://127.0.0.1:{hub_port}/hook/1"])
    c = authed(create_app(settings))  # same DB, used for queries only
    # hub is down: events queue on the device
    sim.sim.fault("float_alarm")
    time.sleep(1.5)
    t.start()
    assert wait_for(lambda: hub.started)
    try:
        assert c.post("/api/v1/devices", json={"address": sim.address, "token": TOKEN}).json()["id"] == 1
        # queued float_alarm is delivered once the hub is reachable (retry/backoff on the device)
        assert wait_for(lambda: "float_alarm" in alert_types(c))
        sim.sim.fault("power_lost")
        assert wait_for(lambda: "power_lost" in alert_types(c))
        sim.sim.fault("power_restored")
        assert wait_for(lambda: "power_lost" not in alert_types(c))
        assert sim.sim.cursors[f"http://127.0.0.1:{hub_port}/hook/1"] == sim.sim.seq
    finally:
        hub.should_exit = True
        t.join(5)

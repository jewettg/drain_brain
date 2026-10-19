import asyncio
import json
import secrets
import time
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import Depends, FastAPI, Form, HTTPException, Request
from fastapi.responses import HTMLResponse, JSONResponse, RedirectResponse
from fastapi.security import HTTPBasic, HTTPBasicCredentials
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates

from . import alerts, core, db
from .config import Settings
from .core import DeviceError, SIG_HEADER

HERE = Path(__file__).parent


def fmt_ts(ts):
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(ts)) if ts else "-"


def card_state(d: dict) -> str:
    """CSS class for a fleet card: grey offline, red float alarm, amber warnings, green ok."""
    if not d["online"]:
        return "offline"
    st = d["status"]
    if st.get("float") == "alarm":
        return "alarm"
    b = st.get("bottle") or {}
    if (st.get("power") == "battery" or (st.get("battery_pct") or 100) <= alerts.BATTERY_LOW_PCT
            or (b.get("pct") is not None and b["pct"] <= alerts.BOTTLE_LOW_PCT) or not b.get("present", True)):
        return "warn"
    return "ok"


def create_app(settings: Settings | None = None) -> FastAPI:
    settings = settings or Settings.from_env()
    db.init(settings.db_path)

    @asynccontextmanager
    async def lifespan(app):
        task = asyncio.create_task(core.poll_loop(settings)) if settings.poll_interval > 0 else None
        yield
        if task:
            task.cancel()

    app = FastAPI(title="brain_drain_hub", lifespan=lifespan)
    app.state.settings = settings
    app.mount("/static", StaticFiles(directory=HERE / "static"), name="static")
    tpl = Jinja2Templates(directory=HERE / "templates")
    tpl.env.filters["ts"] = fmt_ts
    basic = HTTPBasic()

    # Placeholder auth: one shared admin password from env (roles/SSO come later).
    def auth(creds: HTTPBasicCredentials = Depends(basic)) -> str:
        if not (secrets.compare_digest(creds.username, "admin")
                and secrets.compare_digest(creds.password, settings.admin_password)):
            raise HTTPException(401, "Unauthorized", headers={"WWW-Authenticate": "Basic"})
        return creds.username

    def conn():
        return db.connect(settings.db_path)

    def get_dev(c, dev_id: int):
        d = c.execute("SELECT * FROM devices WHERE id=?", (dev_id,)).fetchone()
        if d is None:
            raise HTTPException(404, "no such device")
        return d

    def dev_view(r) -> dict:
        d = dict(r)
        d.pop("token_enc")
        d["status"] = json.loads(d.pop("status_json") or "{}")
        d["state"] = card_state(d)
        return d

    def fleet():
        c = conn()
        try:
            return [dev_view(r) for r in c.execute("SELECT * FROM devices ORDER BY name")]
        finally:
            c.close()

    def render(request, name, **ctx):
        return tpl.TemplateResponse(request, name, ctx)

    async def run(dev_id, actor, action, method, path, body=None):
        c = conn()
        try:
            dev = get_dev(c, dev_id)
        finally:
            c.close()
        try:
            return await core.command(settings, dev, actor, action, method, path, body)
        except DeviceError as e:
            raise HTTPException(502 if e.status == 0 else e.status, f"{e.code}: {e.message}")

    # ---- unauthenticated: webhook receiver and health ----------------------
    @app.get("/healthz")
    def healthz():
        return {"ok": True}

    @app.post("/hook/{device_id}")
    async def hook(device_id: int, request: Request):
        body = await request.body()
        c = conn()
        try:
            dev = c.execute("SELECT * FROM devices WHERE id=?", (device_id,)).fetchone()
            if dev is None:
                raise HTTPException(404, "unknown device")
            if not core.verify(core.dev_token(settings, dev), body, request.headers.get(SIG_HEADER)):
                raise HTTPException(401, "bad signature")
            try:
                ev = json.loads(body)
                int(ev["seq"]); str(ev["type"])
            except (ValueError, KeyError, TypeError):
                raise HTTPException(400, "malformed event")
            if dev["uid"] and ev.get("device_id") not in (None, dev["uid"]):
                raise HTTPException(400, "device_id mismatch")
            prev_seq = dev["last_seq"]
            new = core.ingest_event(c, device_id, ev)
            core.touch_seen(c, device_id)
            c.commit()
        finally:
            c.close()
        if new and int(ev["seq"]) > prev_seq + 1:  # gap: pull what we missed
            try:
                c = conn()
                d2 = get_dev(c, device_id)
                c.close()
                d2 = dict(d2); d2["last_seq"] = prev_seq
                await core.backfill(settings, d2)
            except DeviceError:
                pass
        return {"ok": True, "duplicate": not new}

    # ---- JSON API -----------------------------------------------------------
    api = [Depends(auth)]

    @app.get("/api/v1/devices", dependencies=api)
    def api_devices():
        return fleet()

    @app.post("/api/v1/devices", dependencies=api)
    async def api_add(payload: dict, actor: str = Depends(auth)):
        try:
            dev_id = await core.add_device(settings, payload.get("name"), payload["address"], payload["token"], actor)
        except KeyError:
            raise HTTPException(400, "address and token required")
        except DeviceError as e:
            raise HTTPException(502, f"{e.code}: {e.message}")
        await core.poll_device(settings, dev_id)
        return {"id": dev_id}

    @app.get("/api/v1/devices/{dev_id}/events", dependencies=api)
    def api_events(dev_id: int, limit: int = 100):
        c = conn()
        try:
            rows = c.execute("SELECT seq,ts,type,data_json FROM events WHERE device_id=? ORDER BY seq DESC LIMIT ?",
                             (dev_id, limit)).fetchall()
        finally:
            c.close()
        return [{"seq": r["seq"], "ts": r["ts"], "type": r["type"], "data": json.loads(r["data_json"])} for r in rows]

    @app.post("/api/v1/devices/{dev_id}/dispense")
    async def api_dispense(dev_id: int, payload: dict, actor: str = Depends(auth)):
        return await run(dev_id, actor, "dispense", "POST", "/dispense", payload)

    @app.post("/api/v1/devices/{dev_id}/bottle/reset")
    async def api_bottle_reset(dev_id: int, payload: dict | None = None, actor: str = Depends(auth)):
        return await run(dev_id, actor, "bottle_reset", "POST", "/bottle/reset", payload or {})

    @app.get("/api/v1/devices/{dev_id}/schedule", dependencies=api)
    async def api_schedule_get(dev_id: int):
        return await run(dev_id, "admin", "schedule_get", "GET", "/schedule")

    @app.put("/api/v1/devices/{dev_id}/schedule")
    async def api_schedule_put(dev_id: int, payload: dict, actor: str = Depends(auth)):
        return await run(dev_id, actor, "schedule_put", "PUT", "/schedule", payload)

    @app.get("/api/v1/devices/{dev_id}/audit", dependencies=api)
    def api_audit(dev_id: int):
        c = conn()
        try:
            return [dict(r) for r in c.execute("SELECT * FROM audit_log WHERE device_id=? ORDER BY id DESC", (dev_id,))]
        finally:
            c.close()

    @app.get("/api/v1/alerts", dependencies=api)
    def api_alerts(state: str = "open"):
        c = conn()
        try:
            return [dict(r) for r in c.execute("SELECT * FROM alerts WHERE state=? ORDER BY id DESC", (state,))]
        finally:
            c.close()

    @app.post("/api/v1/alerts/{alert_id}/ack")
    def api_ack(alert_id: int, actor: str = Depends(auth)):
        c = conn()
        try:
            ok = alerts.acknowledge(c, alert_id, actor)
            c.execute("INSERT INTO audit_log(ts,actor,action,params_json,result) VALUES(?,?,?,?,?)",
                      (db.now(), actor, "alert_ack", json.dumps({"alert_id": alert_id}), "ok" if ok else "noop"))
            c.commit()
        finally:
            c.close()
        return {"acknowledged": ok}

    # ---- UI (server-rendered + htmx) ----------------------------------------
    def open_alert_count():
        c = conn()
        try:
            return c.execute("SELECT COUNT(*) FROM alerts WHERE state='open' AND ack_by IS NULL").fetchone()[0]
        finally:
            c.close()

    @app.get("/", response_class=HTMLResponse, dependencies=api)
    def ui_dashboard(request: Request):
        return render(request, "dashboard.html", devices=fleet(), nav_alerts=open_alert_count(), error=None)

    @app.get("/fragments/fleet", response_class=HTMLResponse, dependencies=api)
    def ui_fleet(request: Request):
        return render(request, "_cards.html", devices=fleet())

    @app.post("/ui/devices", dependencies=api)
    async def ui_add(request: Request, name: str = Form(""), address: str = Form(...), token: str = Form(...),
                     actor: str = Depends(auth)):
        try:
            dev_id = await core.add_device(settings, name or None, address, token, actor)
        except DeviceError as e:
            return render(request, "dashboard.html", devices=fleet(), nav_alerts=open_alert_count(),
                          error=f"Could not add device: {e.code}: {e.message}")
        await core.poll_device(settings, dev_id)
        return RedirectResponse(f"/devices/{dev_id}", status_code=303)

    @app.post("/ui/devices/{dev_id}/delete", dependencies=api)
    def ui_delete(dev_id: int, actor: str = Depends(auth)):
        c = conn()
        try:
            for t in ("devices WHERE id", "events WHERE device_id", "alerts WHERE device_id",
                      "status_samples WHERE device_id"):
                c.execute(f"DELETE FROM {t}=?", (dev_id,))
            c.execute("INSERT INTO audit_log(ts,actor,device_id,action,result) VALUES(?,?,?,?,?)",
                      (db.now(), actor, dev_id, "device_delete", "ok"))
            c.commit()
        finally:
            c.close()
        return RedirectResponse("/", status_code=303)

    def device_ctx(dev_id: int, **extra):
        c = conn()
        try:
            dev = dev_view(get_dev(c, dev_id))
            events = [dict(r) for r in c.execute(
                "SELECT seq,ts,type,data_json FROM events WHERE device_id=? ORDER BY seq DESC LIMIT 30", (dev_id,))]
            open_alerts = [dict(r) for r in c.execute(
                "SELECT * FROM alerts WHERE device_id=? AND state='open'", (dev_id,))]
        finally:
            c.close()
        return dict(dev=dev, events=events, open_alerts=open_alerts, nav_alerts=open_alert_count(), **extra)

    @app.get("/devices/{dev_id}", response_class=HTMLResponse, dependencies=api)
    async def ui_device(request: Request, dev_id: int):
        try:
            schedule = json.dumps(await run(dev_id, "admin", "schedule_get", "GET", "/schedule"), indent=2)
        except HTTPException:
            schedule = ""
        return render(request, "device.html", **device_ctx(dev_id, schedule=schedule, msg=None))

    @app.get("/fragments/device/{dev_id}", response_class=HTMLResponse, dependencies=api)
    def ui_device_fragment(request: Request, dev_id: int):
        return render(request, "_device_live.html", **device_ctx(dev_id))

    async def action_msg(request, coro, ok_text):
        try:
            await coro
            return render(request, "_msg.html", ok=True, text=ok_text)
        except HTTPException as e:
            return render(request, "_msg.html", ok=False, text=str(e.detail))

    @app.post("/ui/devices/{dev_id}/dispense", response_class=HTMLResponse)
    async def ui_dispense(request: Request, dev_id: int, ml: float = Form(...), actor: str = Depends(auth)):
        return await action_msg(request, run(dev_id, actor, "dispense", "POST", "/dispense", {"ml": ml}),
                                f"Dispensed {ml:g} mL")

    @app.post("/ui/devices/{dev_id}/bottle_reset", response_class=HTMLResponse)
    async def ui_bottle_reset(request: Request, dev_id: int, actor: str = Depends(auth)):
        return await action_msg(request, run(dev_id, actor, "bottle_reset", "POST", "/bottle/reset", {}),
                                "Bottle marked full")

    @app.post("/ui/devices/{dev_id}/schedule", response_class=HTMLResponse)
    async def ui_schedule(request: Request, dev_id: int, schedule: str = Form(...), actor: str = Depends(auth)):
        try:
            doc = json.loads(schedule)
        except ValueError as e:
            return render(request, "_msg.html", ok=False, text=f"Invalid JSON: {e}")
        return await action_msg(request, run(dev_id, actor, "schedule_put", "PUT", "/schedule", doc),
                                "Schedule saved")

    @app.get("/alerts", response_class=HTMLResponse, dependencies=api)
    def ui_alerts(request: Request):
        return render(request, "alerts.html", nav_alerts=open_alert_count(), **alert_lists())

    @app.get("/alerts/fragment", response_class=HTMLResponse, dependencies=api)
    def ui_alerts_fragment(request: Request):
        return render(request, "_alert_lists.html", **alert_lists())

    def alert_lists():
        c = conn()
        try:
            q = ("SELECT a.*, d.name AS device_name FROM alerts a JOIN devices d ON d.id=a.device_id "
                 "WHERE a.state=? ORDER BY a.id DESC LIMIT 50")
            return dict(open_=[dict(r) for r in c.execute(q, ("open",))],
                        closed=[dict(r) for r in c.execute(q, ("closed",))])
        finally:
            c.close()

    @app.post("/ui/alerts/{alert_id}/ack", response_class=HTMLResponse)
    def ui_ack(request: Request, alert_id: int, actor: str = Depends(auth)):
        api_ack(alert_id, actor)
        return render(request, "_alert_lists.html", **alert_lists())

    @app.exception_handler(HTTPException)
    async def http_exc(request: Request, exc: HTTPException):
        return JSONResponse({"error": {"message": exc.detail}}, status_code=exc.status_code, headers=exc.headers)

    return app


def main() -> None:
    import os
    import uvicorn
    uvicorn.run(create_app(), host=os.environ.get("HUB_HOST", "127.0.0.1"), port=int(os.environ.get("HUB_PORT", "8000")))

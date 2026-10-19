#!/usr/bin/env python3
"""drain_brain device simulator: implements the FIRMWARE_PLAN section 7 REST API with in-memory state,
a bottle model, fault injection, and HMAC-signed webhooks.

    python tools/sim_device.py --port 9001 --token secret1 --name "Lab A" --hook-url http://127.0.0.1:8000/hook/1

Fault injection (bearer auth): POST /sim/fault {"fault": "float_alarm|float_dry|power_lost|power_restored|
bottle_removed|bottle_inserted|battery_low"}
"""
import argparse
import hashlib
import hmac
import json
import secrets
import threading
import time

import httpx
import uvicorn
from fastapi import Depends, FastAPI, Header, HTTPException, Request
from fastapi.responses import JSONResponse


class ApiError(Exception):
    def __init__(self, status: int, code: str, message: str = ""):
        self.status, self.code, self.message = status, code, message or code


class Sim:
    def __init__(self, token: str, name: str = "sim", device_id: str | None = None, hook_urls=None):
        self.token, self.name = token, name
        self.device_id = device_id or "sim-" + secrets.token_hex(3)
        self.lock = threading.RLock()
        self.t0 = time.time()
        self.seq = 0
        self.events: list[dict] = []
        self.power, self.battery_pct, self.power_lost_at = "adapter", 100, None
        self.float_state, self.float_wet = "ok", False
        self.bottle = {"capacity_ml": 500.0, "remaining_ml": 500.0, "present": True}
        self.config = {"name": name, "tz": "UTC", "flow_ml_per_s": 2.5, "bottle_low_percent": 15,
                       "webhooks": list(hook_urls or [])}
        self.schedule: dict = {"entries": []}
        self.jobs: dict[str, dict] = {}
        self.cursors: dict[str, int] = {u: 0 for u in self.config["webhooks"]}  # last seq acked per URL
        self.last_error: dict[str, str] = {}
        self.wake = threading.Event()
        self.stop = threading.Event()
        self.sender = threading.Thread(target=self._sender, daemon=True)

    # ---- events and webhooks ----
    def emit(self, etype: str, data: dict | None = None) -> dict:
        with self.lock:
            self.seq += 1
            ev = {"device_id": self.device_id, "seq": self.seq, "time": int(time.time()), "type": etype, "data": data or {}}
            self.events.append(ev)
            del self.events[:-500]  # ring buffer
        self.wake.set()
        return ev

    def _sender(self):
        while not self.stop.is_set():
            self.wake.wait(1.0)
            self.wake.clear()
            self.flush()

    def flush(self):
        """Deliver events in order to every webhook URL, stopping a URL at its first failure (retry next tick)."""
        with self.lock:
            urls = list(self.config["webhooks"])
            for u in urls:
                self.cursors.setdefault(u, self.seq)  # new URLs start at "now"; the hub backfills history
        for u in urls:
            while True:
                with self.lock:
                    pending = [e for e in self.events if e["seq"] > self.cursors[u]]
                if not pending:
                    break
                ev = pending[0]
                body = json.dumps(ev, separators=(",", ":")).encode()
                sig = hmac.new(self.token.encode(), body, hashlib.sha256).hexdigest()
                try:
                    r = httpx.post(u, content=body, timeout=3, headers={
                        "Content-Type": "application/json", "X-DrainBrain-Signature": sig})
                    ok = 200 <= r.status_code < 300
                except httpx.HTTPError as e:
                    ok = False
                    self.last_error[u] = str(e)
                if not ok:
                    break
                self.cursors[u] = ev["seq"]

    # ---- model ----
    def bottle_pct(self) -> float:
        b = self.bottle
        return round(100 * b["remaining_ml"] / b["capacity_ml"], 1) if b["capacity_ml"] else 0.0

    def status(self) -> dict:
        with self.lock:
            return {"power": self.power, "battery_pct": self.battery_pct,
                    "bottle": {**self.bottle, "pct": self.bottle_pct()},
                    "float": self.float_state, "motor": "idle", "mode": "normal", "faults": [],
                    "time_valid": True, "uptime_s": int(time.time() - self.t0), "ip": "127.0.0.1", "rssi": -55,
                    "event_seq": self.seq}

    def dispense(self, body: dict) -> dict:
        with self.lock:
            rate = self.config["flow_ml_per_s"]
            if "ml" in body:
                ml = float(body["ml"])
            elif "seconds" in body:
                ml = float(body["seconds"]) * rate
            else:
                raise ApiError(400, "bad_request", "ml or seconds required")
            if ml <= 0:
                raise ApiError(400, "bad_request", "amount must be positive")
            if not self.bottle["present"]:
                raise ApiError(409, "bottle_missing", "no bottle present")
            if self.float_state == "alarm":
                raise ApiError(409, "float_alarm", "float alarm latched")
            if self.bottle["remaining_ml"] <= 0:
                raise ApiError(409, "bottle_empty", "bottle empty")
            was_low = self.bottle_pct() <= self.config["bottle_low_percent"]
            actual = min(ml, self.bottle["remaining_ml"])
            job_id = secrets.token_hex(4)
            self.emit("dispense_started", {"job": job_id, "ml": ml})
            self.bottle["remaining_ml"] = round(self.bottle["remaining_ml"] - actual, 3)
            job = {"id": job_id, "requested_ml": ml, "actual_ml": actual, "actual_seconds": round(actual / rate, 2),
                   "outcome": "done" if actual == ml else "partial"}
            self.jobs[job_id] = job
            self.emit("dispense_done", job)
            if self.bottle["remaining_ml"] <= 0:
                self.emit("bottle_empty", {})
            elif not was_low and self.bottle_pct() <= self.config["bottle_low_percent"]:
                self.emit("bottle_low", {"pct": self.bottle_pct()})
            return job

    def fault(self, name: str) -> None:
        with self.lock:
            if name == "float_alarm":
                self.float_state, self.float_wet = "alarm", True
                self.emit("float_alarm")
            elif name == "float_dry":
                self.float_wet = False
            elif name == "power_lost":
                self.power, self.power_lost_at = "battery", time.time()
                self.emit("power_lost")
            elif name == "power_restored":
                outage = int(time.time() - (self.power_lost_at or time.time()))
                self.power, self.power_lost_at = "adapter", None
                self.emit("power_restored", {"outage_s": outage})
            elif name == "bottle_removed":
                self.bottle["present"] = False
                self.emit("bottle_removed")
            elif name == "bottle_inserted":
                self.bottle["present"] = True
                self.emit("bottle_inserted")
            elif name == "battery_low":
                self.battery_pct = 15
                self.emit("battery_low", {"pct": 15})
            else:
                raise ApiError(400, "bad_request", f"unknown fault {name}")


def create_app(sim: Sim) -> FastAPI:
    app = FastAPI(title="drain_brain simulator")
    app.state.sim = sim

    @app.exception_handler(ApiError)
    async def _err(request: Request, e: ApiError):
        return JSONResponse({"error": {"code": e.code, "message": e.message}}, status_code=e.status)

    def auth(authorization: str = Header(default="")):
        if not hmac.compare_digest(authorization, f"Bearer {sim.token}"):
            raise ApiError(401, "unauthorized", "bad or missing bearer token")

    a = [Depends(auth)]
    P = "/api/v1"

    @app.get(f"{P}/info")
    def info():
        return {"model": "iFLO-sim", "firmware": "sim-0.1.0", "name": sim.config["name"], "id": sim.device_id}

    @app.get(f"{P}/status", dependencies=a)
    def status():
        return sim.status()

    @app.get(f"{P}/config", dependencies=a)
    def get_config():
        return sim.config

    @app.put(f"{P}/config", dependencies=a)
    def put_config(body: dict):
        with sim.lock:
            sim.config.update(body)
            sim.wake.set()
            return sim.config

    @app.post(f"{P}/dispense", dependencies=a)
    def dispense(body: dict):
        return sim.dispense(body)

    @app.post(f"{P}/dispense/cancel", dependencies=a)
    def cancel():  # sim dispenses are instantaneous, so there is never anything to cancel
        return {"cancelled": False}

    @app.get(f"{P}/dispense/{{job_id}}", dependencies=a)
    def job(job_id: str):
        if job_id not in sim.jobs:
            raise ApiError(404, "not_found", "no such job")
        return sim.jobs[job_id]

    @app.get(f"{P}/schedule", dependencies=a)
    def get_schedule():
        return sim.schedule

    @app.put(f"{P}/schedule", dependencies=a)
    def put_schedule(body: dict):
        if not isinstance(body.get("entries", []), list):
            raise ApiError(400, "bad_schedule", "entries must be a list")
        with sim.lock:
            sim.schedule = body
            sim.emit("schedule_changed")
        return sim.schedule

    @app.get(f"{P}/schedule/next", dependencies=a)
    def schedule_next(n: int = 5):
        return {"runs": []}  # stub: no schedule engine in the simulator

    @app.get(f"{P}/bottle", dependencies=a)
    def bottle():
        return {**sim.bottle, "pct": sim.bottle_pct()}

    @app.post(f"{P}/bottle/reset", dependencies=a)
    def bottle_reset(body: dict | None = None):
        with sim.lock:
            cap = float((body or {}).get("capacity_ml", sim.bottle["capacity_ml"]))
            sim.bottle.update(capacity_ml=cap, remaining_ml=cap, present=True)
            sim.emit("bottle_inserted", {"reset": True, "capacity_ml": cap})
            return {**sim.bottle, "pct": 100.0}

    @app.post(f"{P}/calibration", dependencies=a)
    def calibration(body: dict):
        try:
            rate = float(body["measured_ml"]) / float(body["seconds"])
        except (KeyError, ValueError, ZeroDivisionError):
            raise ApiError(400, "bad_request", "seconds and measured_ml required")
        sim.config["flow_ml_per_s"] = round(rate, 4)
        return {"flow_ml_per_s": sim.config["flow_ml_per_s"]}

    @app.get(f"{P}/events", dependencies=a)
    def events(since: int = 0, limit: int = 100):
        with sim.lock:
            return {"events": [e for e in sim.events if e["seq"] > since][:limit]}

    @app.post(f"{P}/float/clear", dependencies=a)
    def float_clear():
        with sim.lock:
            if sim.float_wet:
                raise ApiError(409, "float_wet", "sensor still wet")
            if sim.float_state == "alarm":
                sim.float_state = "ok"
                sim.emit("float_cleared")
            return {"float": sim.float_state}

    @app.post("/sim/fault", dependencies=a)
    def fault(body: dict):
        sim.fault(body.get("fault", ""))
        return sim.status()

    return app


class Server:
    """Run a simulator in a background thread (used by tests and by `main`)."""

    def __init__(self, sim: Sim, port: int, host: str = "127.0.0.1"):
        self.sim = sim
        self.server = uvicorn.Server(uvicorn.Config(create_app(sim), host=host, port=port, log_level="warning"))
        self.thread = threading.Thread(target=self.server.run, daemon=True)

    def start(self):
        self.sim.sender.start()
        self.thread.start()
        while not self.server.started:
            time.sleep(0.02)
        return self

    def stop(self):
        self.sim.stop.set()
        self.server.should_exit = True
        self.thread.join(5)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", type=int, default=9001)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--token", default="devtoken")
    p.add_argument("--name", default="sim")
    p.add_argument("--device-id", default=None)
    p.add_argument("--hook-url", action="append", default=[], help="webhook URL (repeatable)")
    args = p.parse_args()
    sim = Sim(args.token, args.name, args.device_id, args.hook_url)
    sim.sender.start()
    print(f"simulator '{args.name}' id={sim.device_id} on http://{args.host}:{args.port}/api/v1 token={args.token}")
    uvicorn.run(create_app(sim), host=args.host, port=args.port, log_level="warning")


if __name__ == "__main__":
    main()

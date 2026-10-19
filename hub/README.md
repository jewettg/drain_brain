# brain_drain_hub

Fleet controller for drain_brain devices (see `../documentation/HUB_PLAN.md`). It speaks only the device REST API and signed-webhook
contract from `../documentation/FIRMWARE_PLAN.md` section 7, so it runs today against the bundled simulator. No cloud calls, no telemetry;
the UI vendors htmx locally.

Status: first slice (plan milestones H0 to H3). Python 3.12+, FastAPI, SQLite, Jinja2 + htmx, light theme.

## Install

```
cd hub
python3 -m venv .venv
.venv/bin/pip install -e '.[dev]'
```

## Configuration (environment)

| Variable | Required | Meaning |
| --- | --- | --- |
| `HUB_SECRET_KEY` | yes | Fernet key; device tokens are encrypted at rest with it. Generate: `python -c "from cryptography.fernet import Fernet; print(Fernet.generate_key().decode())"`. Losing it means re-adding devices. |
| `HUB_ADMIN_PASSWORD` | yes | Password for HTTP Basic auth, user `admin`. **Placeholder** until real users and roles exist; run behind TLS. |
| `HUB_DB` | no | SQLite file, default `hub.db` |
| `HUB_POLL_INTERVAL` | no | Seconds between `/status` polls, default 60 (0 disables). A device silent for 3 intervals is offline. |
| `HUB_PUBLIC_URL` | no | Base URL devices use to reach the hub, e.g. `http://192.168.1.10:8000`. If set, adding a device also appends `<url>/hook/<id>` to its `webhooks` config. |
| `HUB_HOST`, `HUB_PORT` | no | Bind address, default `127.0.0.1:8000` |

Run the hub: `python -m brain_drain_hub` (single worker; the poller runs in-process).

## Simulator

`tools/sim_device.py` implements the device API (info, status, config, dispense, dispense/cancel, dispense/{id}, schedule, schedule/next,
bottle, bottle/reset, calibration, events, float/clear) with bearer auth, in-memory state, and a bottle model. Webhooks are sent with
`X-DrainBrain-Signature` (HMAC-SHA256 hex over the raw body, key = device token), in order, retried each second until the hub returns 2xx.
Each instance needs its own `--port`.

```
python tools/sim_device.py --port 9001 --token secret1 --name "Lab A" [--hook-url http://127.0.0.1:8000/hook/1]
```

Fault injection: `POST /sim/fault` (bearer auth) with `{"fault": ...}`, one of `float_alarm`, `float_dry`, `power_lost`,
`power_restored`, `bottle_removed`, `bottle_inserted`, `battery_low`. `float_alarm` latches until `float_dry` then `POST /api/v1/float/clear`.

## Demo

```
export HUB_SECRET_KEY=$(.venv/bin/python -c "from cryptography.fernet import Fernet; print(Fernet.generate_key().decode())")
export HUB_ADMIN_PASSWORD=changeme HUB_POLL_INTERVAL=10 HUB_PUBLIC_URL=http://127.0.0.1:8000
.venv/bin/python -m brain_drain_hub &                                                        # http://127.0.0.1:8000
.venv/bin/python tools/sim_device.py --port 9001 --token secret1 --name "Lab A" &
.venv/bin/python tools/sim_device.py --port 9002 --token secret2 --name "Lab B" &
# add both (or use the form on the dashboard); the hub registers its webhook URL on each
curl -u admin:changeme -H 'content-type: application/json' -d '{"address":"127.0.0.1:9001","token":"secret1"}' localhost:8000/api/v1/devices
curl -u admin:changeme -H 'content-type: application/json' -d '{"address":"127.0.0.1:9002","token":"secret2"}' localhost:8000/api/v1/devices
# open http://127.0.0.1:8000 (user admin), then break things:
curl -H 'Authorization: Bearer secret1' -H 'content-type: application/json' -d '{"fault":"float_alarm"}' localhost:9001/sim/fault   # card turns red, alert opens
curl -H 'Authorization: Bearer secret2' -H 'content-type: application/json' -d '{"fault":"power_lost"}' localhost:9002/sim/fault    # amber
# on a device page: Dispense now, Bottle reset, edit the schedule JSON; acknowledge alerts on the Alerts page
kill %2   # stop a simulator: after 3 poll intervals its card turns grey and an "offline" alert opens
```

## Tests

`.venv/bin/python -m pytest` (starts simulators in-process on free ports; no hardware or network needed).

## What exists

Add/remove devices (token encrypted), `/status` polling, `POST /hook/{id}` with HMAC check and `(device_id, seq)` dedupe, event store,
alerts (float_alarm, power_lost, battery_low, bottle_low, bottle_empty, offline) opened and closed from pushed events and polled status,
`/events?since=` backfill after gaps and reconnects, proxied commands (dispense, bottle reset, schedule get/put) with `audit_log` for every
write, JSON API under `/api/v1`, and the UI (fleet dashboard, device page, alerts with acknowledge).

## NOT done

- Users, roles, sessions, SSO (only the single shared admin password above)
- Alert delivery channels (email, webhook, Slack/Teams), escalation, quiet hours, "power lost longer than N minutes" rules; alerts are in-UI only
- Groups, schedule templates, bulk actions
- Calibration wizard, device config/settings editing, OTA/firmware rollouts, reports/CSV/Prometheus, backup/restore
- mDNS discovery (manual add by address only), HTTPS certificate pinning, retention/pruning of `status_samples` and events
- Contract tests against a frozen device OpenAPI document (the simulator's JSON shapes are inferred from the plan, see below)
- Simulator: dispenses complete instantly (so `dispense/cancel` is a no-op), `schedule/next` is a stub, no schedule engine, no `/wifi`, `/time/sync`, `/ota`, `/reboot`

## Assumed device JSON shapes (the plan leaves these open)

`/status`: `power` (`adapter`|`battery`), `battery_pct`, `bottle {present, capacity_ml, remaining_ml, pct}`, `float` (`ok`|`alarm`), `motor`,
`mode`, `uptime_s`, `rssi`, and `event_seq` (latest event sequence number, used to trigger backfill). `/events` returns `{"events": [...]}`.
Schedule document: `{"entries": [...]}`. Confirm these when the firmware API freezes.

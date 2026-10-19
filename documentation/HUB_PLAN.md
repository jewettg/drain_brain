# brain_drain_hub — Fleet Controller Plan

A standalone, web-based control plane for many drain_brain devices. It speaks only the device REST API and webhook protocol defined in
`FIRMWARE_PLAN.md` §7, so the hub can be built and tested against a simulator before real hardware is ready.

## 1. Goals

- See every device at a glance: online/offline, power source, battery, bottle level, float alarm, last/next dispense.
- Control any device: manual dispense, schedules, calibration, bottle reset, settings, OTA.
- Receive pushed events in real time, keep history, and alert people.
- Manage groups (building, floor, unit) and apply schedule templates across devices.

## 2. Architecture

| Layer | Choice | Notes |
| --- | --- | --- |
| Backend | **Python 3.12, FastAPI, uvicorn** | Matches the stack already used for other internal tools |
| Storage | **SQLite (WAL)** to start; schema portable to PostgreSQL | One file, trivial backup |
| Jobs | In-process asyncio scheduler | Polling, backfill, retention, alert escalation |
| Device I/O | `httpx` async client with per-device token | Cert-fingerprint pinning when the device uses HTTPS |
| Discovery | mDNS (`_drainbrain._tcp`) via `zeroconf`, plus manual add by IP/hostname | |
| UI | Server-rendered templates + htmx (light theme), no build step | Same look as the device dashboard |
| Deploy | Container or systemd service behind a reverse proxy (TLS) | Single worker; no clustering needed at this scale |

The hub does **not** replace device autonomy: devices run their schedules locally and keep working if the hub is down; the hub
backfills events from `/events?since=` when it reconnects.

## 3. Data model (SQLite)

`devices` (id, name, group_id, address, token_ref, fw_version, last_seen, last_seq, tz, online)
`groups` (id, name, parent_id) · `status_samples` (device_id, ts, power, battery_pct, bottle_pct, float, mode)
`events` (device_id, seq, ts, type, data_json, UNIQUE(device_id, seq)) · `schedules` (id, name, json) · `schedule_assignments` (device_id, schedule_id)
`alerts` (id, device_id, type, state, opened_ts, closed_ts, ack_by) · `users`, `sessions`, `api_tokens` · `audit_log`.

Device tokens are stored encrypted at rest (key from an environment variable or a secrets manager), never in the repo.

## 4. Device communication

- **Push (primary)**: devices POST signed events to `POST /hook/{device_id}`; the hub verifies the HMAC, de-duplicates on `(device_id, seq)`, stores, evaluates alerts, and returns `2xx`.
- **Poll (safety net)**: `GET /status` every 60 s (configurable) → updates the dashboard and detects silence; `GET /events?since=` after any gap.
- **Offline detection**: no push and no successful poll for 3 intervals → alert "device offline".
- **Commands**: the hub calls the device REST API directly (`/dispense`, `/schedule`, ...) and records every command in `audit_log`.

## 5. Features

1. **Fleet dashboard**: card/table view with filters by group and state; red for float alarm, amber for battery/bottle/power outage, grey for offline.
2. **Device page**: live status, gauge for bottle, event timeline, schedule editor (reuses the device editor), calibration wizard, settings, firmware.
3. **Schedule templates**: define once ("weekly Mon/Thu 09:00 10 mL"), assign to many devices, with per-device overrides and a diff view before pushing.
4. **Bulk actions**: dispense on selected devices, push schedule, set thresholds, reboot, OTA, with per-device result and retry.
5. **Bottle logistics**: "bottles needing replacement within N days" report based on remaining mL and the schedule.
6. **Alerts**: rules per type (float alarm, power lost > N minutes, battery low, bottle low/empty, offline) with email, webhook, and Slack/Teams
   channels; acknowledge and escalate; quiet hours.
7. **Time handling**: all timestamps stored in UTC, shown in the device's or the viewer's local zone; the hub maps IANA names to the POSIX TZ strings the devices need.
8. **Firmware management**: upload an image once, roll out to groups in waves, stop on failure, show per-device version.
9. **Reporting/export**: CSV/JSON for events, dispenses, and consumption; Prometheus `/metrics` endpoint optional.
10. **Access control**: users with roles (viewer, operator, admin); local accounts first, SSO (OIDC) later; every write is audited.

## 6. Hub REST API (for scripts and the UI)

`/api/v1/devices`, `/devices/{id}`, `/devices/{id}/status`, `/devices/{id}/dispense`, `/devices/{id}/schedule`, `/devices/{id}/events`,
`/groups`, `/schedules`, `/alerts`, `/ota/images`, `/ota/rollouts`, `/users`. Same JSON shapes as the device API wherever a resource mirrors one.

## 7. Testing

- **Device simulator** (`hub/tools/sim_device.py`): implements the device API and emits webhooks, including faults and power events, so the hub is fully testable with no hardware.
- Unit and API tests with pytest; contract tests that validate against the device's OpenAPI document so the hub and firmware cannot drift.

## 8. Repository layout

```
hub/            brain_drain_hub backend + UI (separate package, own README)
hub/tools/      device simulator, load test
```
Same repo as the firmware for now; split into its own repository (`brain_drain_hub`) when it has its own release cadence.

## 9. Milestones

| # | Deliverable |
| --- | --- |
| H0 | Device API OpenAPI document frozen (from firmware M4); simulator |
| H1 | Add/discover devices, status polling, fleet dashboard (read-only) |
| H2 | Webhook receiver, event store, alerts (email/webhook) |
| H3 | Commands: dispense, schedule editor, bottle reset, calibration |
| H4 | Groups, templates, bulk actions, audit log, roles |
| H5 | Firmware rollouts, reports, metrics, backup/restore |

## 10. Open questions

- Where does the hub run (a small VM, an existing internal tools host, or a container on a NAS)? This decides TLS and authentication.
- Alert channels wanted (email only, or Teams/Slack as well)?
- How many devices and sites (decides whether SQLite is enough)?

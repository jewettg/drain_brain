# drain_brain Firmware Plan

Ground-up replacement firmware for the ESP32-S2 in the iFLO Pro. Hardware facts are in `HARDWARE_MAP.md`; nothing from the
original firmware is reused.

## 1. Decisions

| Topic | Choice | Why |
| --- | --- | --- |
| Target | `esp32s2`, single core | The module is an ESP32-S2-SOLO. |
| Framework | **ESP-IDF 5.x (C), built with `idf.py`** | First-class S2 support, `esp_http_server`, SNTP, mDNS, NVS, OTA, LEDC, ADC calibration, and Unity host tests all included. Arduino would hide the sleep, ADC-cal, and OTA control we need. |
| Storage | NVS for settings and counters; LittleFS partition for the web UI and event log | Small, wear-aware, survives OTA. |
| Time | SNTP + POSIX `TZ` strings | See §5. |
| Web | Static single-page UI served from LittleFS, talking to the same REST API | One API for UI, hub, and scripts. |
| Auth | Per-device bearer token set at provisioning; LAN only | See §8. |
| Rollback | Keep `esp32_dump.bin` (full stock flash image) forever | `esptool write_flash 0x0 esp32_dump.bin` restores the vendor firmware. |

Partition table: bootloader `0x1000`, NVS `0x9000` (24 K), otadata, phy_init, **ota_0 / ota_1 1.5 MB each** (same offsets as stock, so
the stock layout is a valid starting point), and the remaining ~0.9 MB as a `storage` LittleFS partition.

## 2. Phase 0 — bring-up probe (do this first, ~1 hour, no network)

A tiny ESP-IDF app that answers every open item in `HARDWARE_MAP.md` §11 before any real code is written. Flash over JP1 with the
BOOT pads shorted. Console output on UART0.

1. **Input probe**: print GPIO 6, 7, 13, 16, 17 (and 19/20 straps) levels on every change. Press each button, trip the float, toggle the bottle
   switch, pull the adapter → proves polarity and *which physical switch is GPIO6 vs GPIO17*.
2. **LED test**: walk GPIO 40, 39, 41, 42 one at a time (1 s each) → confirms colors and polarity.
3. **ADC scan**: read ADC1 channels 0–9 with `esp_adc_cal`; vary battery/adapter/motor load → finds `ADC_CUR`, validates the ×2.37 battery factor.
4. **Drive test (supervised)**: with the line connected and a container ready, pulse GPIO9 for 500 ms, read the current, pulse GPIO10 for
   500 ms. Then, only if the load is a plain motor, hold GPIO9 for 2 s. Record polarity, current, and whether the load holds state.
5. **Optional PWM test**: LEDC on GPIO9 at 20 kHz, duty 100/75/50/25 %, current logged, to see whether speed control works.
6. Exit criteria: updated `HARDWARE_MAP.md` with every **[INF]/[?]** resolved.

Safety limits baked into the probe: maximum 3 s continuous drive, abort on current above a conservative threshold, never run dry.

## 3. Firmware architecture

Components (ESP-IDF components under `firmware/components/`), each with a header-level API and host-side unit tests where logic is pure:

| Component | Responsibility |
| --- | --- |
| `hal_io` | GPIO map in one table (from `HARDWARE_MAP.md`); debounced input events; LED outputs |
| `leds` | Patterns: off, on, slow/fast flash, N-blink code; priority arbiter (alarm > power > status) |
| `actuator` | `dispense_start/stop`, drive modes **`pulse_valve`** (open pulse, hold, close pulse) and **`motor_continuous`** (hold INA, optional PWM); max on-time guard; current monitor; boot-time safe state |
| `power` | External-power state (GPIO16) with timestamps; battery ADC filter, percent, low/critical; outage log |
| `bottle` | Bottle-present state, volume model (§6), reset-to-full |
| `float_sensor` | Debounced flood state, latch with manual clear, push alert |
| `clock` | SNTP, TZ, wall-clock helpers, "time valid" flag |
| `scheduler` | Calendar schedule engine (§5) |
| `store` | NVS wrappers, schema versioning, event ring buffer in LittleFS |
| `net` | Wi-Fi STA, SoftAP provisioning, mDNS |
| `api` | REST server, auth, JSON (cJSON), OpenAPI served at `/api/v1/openapi.json` |
| `notify` | Event queue + webhook push with retry and backoff |
| `ota` | HTTP upload of a signed-by-token image with rollback on failed boot |
| `app` | Event loop wiring; state machine for fault/mode handling |

One FreeRTOS task per concern with a single event queue to `app`; the actuator task is the only writer of GPIO9/10.

### Device state machine
`BOOT → (PROVISIONING | RUNNING)`; `RUNNING` modes: `NORMAL`, `NO_BOTTLE` (dispense paused), `FLOOD` (dispense blocked, red flashing),
`ON_BATTERY` (dispense allowed down to a configurable battery floor, Wi-Fi duty-cycled). Faults can combine; LED priority picks the display.

## 4. Wi-Fi provisioning and persistence

- First boot (or 5 s hold of the reset button, or "forget Wi-Fi" API): start SoftAP `drain_brain-XXXX` (last 4 hex of MAC) with a captive
  portal page. User picks an SSID from a scan, enters the password, and sets a device name and API token.
- Credentials are stored in NVS (`esp_wifi` storage persistent); device name, token, and TZ in the app NVS namespace. NVS encryption is optional
  and recommended only if flash encryption is turned on (it is off today).
- After joining: DHCP (static IP optional), advertise `_drainbrain._tcp` via mDNS (`drain-brain-xxxx.local`), report IP in `/status`.
- Reconnect with exponential backoff; if STA stays down, keep operating the schedule locally and queue notifications.
- Factory reset: erase the app NVS namespace and Wi-Fi config, reboot into SoftAP.

## 5. Time and scheduling

**Clock.** SNTP (pool + optional LAN server) sets system time; `setenv("TZ", posix_string)` + `tzset()` gives local time with correct DST and leap
years through newlib. The device stores a **POSIX TZ string** (for example `CST6CDT,M3.2.0,M11.1.0`); the dashboard and hub map IANA names
(`America/Chicago`) to it, so no tz database is needed on the chip. No RTC battery: time is lost on full power loss. The device persists a "last known
epoch" every 30 minutes and on every dispense (flash-wear-aware) and flags time `approximate` until SNTP succeeds. Policy setting
`dispense_without_time`: `never` (default), `approximate`.

**Schedule model** (JSON, stored in NVS blob):

```json
{
  "entries": [
    {"id": 1, "enabled": true, "type": "weekly", "days": ["mon","thu"], "time": "09:00", "amount": {"ml": 10}},
    {"id": 2, "enabled": true, "type": "interval", "every_days": 3, "time": "06:30", "amount": {"seconds": 4.0}},
    {"id": 3, "enabled": true, "type": "monthly", "day_of_month": 1, "time": "08:00", "amount": {"ml": 20}},
    {"id": 4, "enabled": true, "type": "once", "at": "2026-12-24T07:00", "amount": {"ml": 5}}
  ],
  "missed_policy": "run_once_on_boot",
  "blackout": [{"from": "2026-07-01", "to": "2026-07-14"}]
}
```

- Next-run computation is done in local time with `mktime` and `tm_isdst = -1`; the DST edge cases are handled explicitly: a 02:30 entry on the
  spring-forward day runs at 03:00 (first valid time after the gap), and on the fall-back day runs once (the first occurrence).
- `missed_policy`: `skip`, `run_once_on_boot` (catch up one dispense, never a burst), configurable.
- The scheduler never dispenses when: no bottle, flood latched, battery below floor on battery, time invalid (per policy), or empty bottle.
  Each skip emits an event with a reason.
- This is pure logic with no ESP-IDF dependency, so it ships with host unit tests (Unity or CMake + a small harness) covering leap-day, DST,
  and time-zone transitions.

## 6. Dispense amount and bottle model

- **Amount**: requested in `seconds` (drive time) or `ml`. `ml` is converted using `flow_ml_per_s`.
- **Calibration** (dashboard wizard): dispense N seconds into a measuring cup, enter the measured mL; the device computes and stores
  `flow_ml_per_s`. Because the stock design lost head pressure as the bottle emptied, offer an optional **flow-vs-fill correction table**
  (ml/s at 100 %, 50 %, 10 %) and interpolate; default is a constant.
- **Bottle state**: `capacity_ml` (user setting), `remaining_ml` (decremented per dispense by commanded ml, persisted per dispense), derived
  `percent`, `estimated_doses_left` (remaining ÷ the median scheduled dose), and states `full / ok / low / empty / missing`.
  `POST /bottle/reset` sets remaining to capacity and records the event. The bottle-present switch going from missing to present can *optionally*
  prompt a reset in the dashboard but never resets silently.
- Thresholds: `low_percent` (default 20) and `empty_percent` (default 5) emit events; dispense is blocked at empty unless overridden.
- Manual dispense via the API is always bounded by `max_dispense_seconds` (default 30).

## 7. REST API (v1)

Base path `/api/v1`, JSON, `Authorization: Bearer <token>` on everything except `GET /info`. `Content-Type: application/json`.
Errors: `{"error": {"code": "bottle_missing", "message": "..."}}` with 4xx; `409` for state conflicts.

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/info` | Unauthenticated: model, firmware, device name, id (for discovery/pairing) |
| GET | `/status` | Everything current: power, battery, bottle, float, motor state, mode/faults, time validity, uptime, IP/RSSI |
| GET/PUT | `/config` | Device name, TZ, flow rate, capacity, thresholds, drive mode, limits, webhook list, LED brightness |
| POST | `/dispense` | `{"ml": 10}` or `{"seconds": 4}`; returns a job id; `409` if blocked |
| POST | `/dispense/cancel` | Stop the current dispense and close |
| GET | `/dispense/{id}` | Job result (requested/actual seconds, ml, outcome) |
| GET/PUT | `/schedule` | Whole schedule document (validated, atomically replaced) |
| GET | `/schedule/next` | Next N planned runs in local time and UTC |
| GET | `/bottle` | Bottle model |
| POST | `/bottle/reset` | Mark bottle full (`{"capacity_ml": 500}` optional) |
| POST | `/calibration` | `{"seconds": 30, "measured_ml": 24}` → stores flow rate |
| GET | `/events?since=<id>&limit=` | Persistent event log (ring buffer, ~500 entries) |
| POST | `/float/clear` | Clear a latched flood after the sensor is dry |
| PUT | `/wifi` | Change Wi-Fi credentials (applied after reboot) |
| POST | `/time/sync` | Force SNTP |
| POST | `/leds/test` | Run an LED pattern test |
| POST | `/ota` | Upload a firmware image (`application/octet-stream`) |
| POST | `/reboot`, `/factory-reset` | Maintenance |

**Push events** (device → hub or any URL in `config.webhooks`): `POST` with `{"device_id","seq","time","type","data"}` and an
`X-DrainBrain-Signature` HMAC-SHA256 over the body using the device token. Types: `boot`, `float_alarm`, `float_cleared`, `power_lost`,
`power_restored`, `battery_low`, `battery_critical`, `bottle_removed`, `bottle_inserted`, `bottle_low`, `bottle_empty`, `dispense_started`,
`dispense_done`, `dispense_skipped`, `schedule_changed`, `fault`. Delivery: queued in the persistent event log, retried with backoff until acknowledged
by `2xx`; the hub can also poll `/events?since=` to backfill. `seq` is monotonic so the receiver can de-duplicate.

## 8. Security notes

- API on the LAN only; recommend an isolated IoT VLAN. Token is 32 random bytes shown once at provisioning; rotate through `/config`.
- Offer HTTPS (`esp_https_server`, self-signed certificate generated on first boot) as an option; HTTP with token is the default for simplicity on
  a trusted LAN. Hub pins the certificate fingerprint when HTTPS is used.
- No cloud endpoints, no telemetry, no hardcoded credentials. OTA requires the bearer token and an image that matches the app descriptor.

## 9. Dashboard (served by the device)

Light theme, mobile-friendly. Screens: **Status** (power, battery, bottle gauge, float, last/next dispense, Wi-Fi), **Dispense** (manual,
calibration wizard), **Schedule** (calendar-style editor with a "next 10 runs" preview in local time), **Bottle** (capacity, reset-to-full),
**Events**, **Settings** (Wi-Fi, TZ, thresholds, webhooks, token, OTA). It only calls `/api/v1`, so the same UI works against any device
through the hub later.

Feasibility: yes. App ~600–800 KB, UI ~150 KB gzipped on LittleFS; both fit in 4 MB with two OTA slots.

## 10. Power behavior

- **On adapter power**: stay awake with Wi-Fi on (modem-sleep allowed), scheduler active.
- **On battery** (GPIO16 low): emit `power_lost` with timestamp, turn amber off, keep running the schedule; go to light sleep between events, wake
  on GPIO6/7/13/16 (RTC GPIOs, so ext1 wake is available) and a timer for the next scheduled dispense; Wi-Fi only briefly to push events.
  Dispense is allowed on battery down to `battery_floor_percent`.
- **Restore**: `power_restored` with outage duration.
- Battery percent from §6 of `HARDWARE_MAP.md`, recalibrated after Phase 0; alerts use thresholds, with hysteresis, and fire once per crossing.

## 11. Build, test, and release

```
firmware/             ESP-IDF project (components/, main/, partitions.csv, sdkconfig.defaults)
firmware/test_host/   Host-side unit tests for scheduler, clock math, bottle model
web/                  Static dashboard sources (built into LittleFS image)
tools/                Phase 0 probe, flash helpers, API client script
documentation/        This directory
```

- CI (GitHub Actions): host unit tests + `idf.py build` for `esp32s2`; artifacts are the `.bin` images. No secrets in the repo.
- Versioning: semantic versions stamped into the app descriptor and shown in `/info`.
- Flash: `idf.py -p <port> flash` over JP1 with the BOOT pads shorted for the first image, OTA afterwards.
- License: GPL-3.0 (repo license).

## 12. Milestones

| # | Deliverable | Done when |
| --- | --- | --- |
| M0 | Phase 0 probe results documented | Every open item in `HARDWARE_MAP.md` resolved |
| M1 | `hal_io`, `leds`, `actuator`, `power`, `bottle`, `float_sensor` + serial CLI | All inputs/outputs controllable and stable on the bench; safe boot state |
| M2 | `net`, provisioning, `store`, minimal `api` (`/info`, `/status`, `/dispense`) | Join Wi-Fi, persist, dispense by `curl` |
| M3 | `clock`, `scheduler` + host tests | DST/leap-year tests pass; schedule survives reboot and power loss |
| M4 | Full REST API, events, webhooks, OTA | OpenAPI doc matches behavior; push events reach a test receiver |
| M5 | Web dashboard | Every setting reachable in the UI |
| M6 | Field soak: 2 weeks on the real drain | No missed schedules, no stuck valve, outage events verified by pulling power |

## 13. Risks

| Risk | Mitigation |
| --- | --- |
| Actuator is a latching valve, not a pump | Both drive modes implemented; Phase 0 decides default |
| U1 does not support PWM speed control | Control by time only; document |
| Time loss on power outage | Persisted last-known epoch, `approximate` flag, policy knob |
| Battery percent inaccurate | Thresholds and events instead of exact percent; recalibrate after Phase 0 |
| Dry run or stuck-open valve floods the pan | Hard max on-time, current watchdog, close on boot, close on any fault |
| Bricking | Keep stock dump; UART download mode is always available via BOOT pads |

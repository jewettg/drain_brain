# drain_brain firmware (first slice)

ESP-IDF 5.3 project for the ESP32-S2 in the iFLO Pro. **Builds cleanly (image 0xEB510 bytes, about 61% of a 1.5 MB slot) but has never run on
hardware.** Every pin, polarity, and drive assumption lives in [`main/board.h`](main/board.h) and is marked `ASSUMPTION` until the Phase 0 probe
results are in. **Run the bring-up probe first** ([`documentation/BRINGUP_INSTRUCTIONS.md`](../documentation/BRINGUP_INSTRUCTIONS.md)); in
`pulse_valve` mode this firmware sends a 500 ms close pulse on GPIO10 at every boot, which is the stock behavior for a valve but would briefly run a
plain motor in reverse.

## What is here

| Piece | Where | State |
| --- | --- | --- |
| Schedule engine (weekly, interval, monthly, once, blackout, DST gap/overlap, leap years) | `components/core/db_schedule.c` | Done; 54 host checks pass |
| Bottle volume model, calibration, battery math | `components/core/db_bottle.c` | Done; host-tested |
| Input debounce, LEDs, actuator (pulse-valve and continuous-motor modes), battery ADC | `main/dbhal.c` | Written, untested on hardware |
| NVS settings/bottle/schedule, RAM event ring | `main/store.c` | Written, untested |
| Wi-Fi STA, provisioning AP, SNTP | `main/net.c` | Written, untested |
| REST API (v1) incl. OTA upload | `main/api.c` | Written, untested |
| Signed webhooks (HMAC-SHA256) | `main/notify.c` | Written, untested |
| Main loop: input events, scheduler, dispense jobs, alerts, LED status | `main/app_main.c` | Written, untested |
| Web dashboard (single page, light theme, ~12 KB) | `web/index.html` | Written, untested; embedded in the image |

**Will the web interface fit and work on the device?** Yes. The whole dashboard is one ~12 KB HTML file embedded in the app image; the image
is 61% of an OTA slot with two slots available. The device serves it at `http://<device-ip>/` and it only calls `/api/v1/*`. Not yet exercised
on real hardware.

## Not done yet (honest list)

mDNS discovery, persistent event log and guaranteed webhook delivery (events are a 100-entry RAM ring; webhook retries are 4 attempts then dropped,
the hub backfills via `/events?since=`), HTTPS, battery-mode light sleep and wake-on-pin, "dispense with approximate time" policy,
`/time/sync`, `/leds/test`, current-sense guard and PWM speed control in the main firmware (the probe tests those), a captive portal (you open
`http://192.168.4.1/` yourself), and `/dispense/{id}` keeping only the most recent job.

## Build, test, flash

```
. ~/esp/esp-idf/export.sh                 # ESP-IDF v5.3.x
cd firmware/test_host && make             # host tests, no hardware needed
cd .. && idf.py set-target esp32s2 && idf.py build
idf.py -p PORT flash monitor              # first flash needs the BOOT pads shorted while powering up
```

## First boot

1. The device starts a Wi-Fi AP named `drain-brain-xxxxxx` (password `drainbrain`) and logs the same on the console.
2. Browse to `http://192.168.4.1/`, enter your Wi-Fi, a name, a time zone, and an API token (16+ characters; the page can generate one). Save.
3. The device reboots and joins your network. Hold the reset button 5 s to wipe settings and return to step 1.
4. Everything after that needs `Authorization: Bearer <token>`:

```
curl -H "Authorization: Bearer $TOKEN" http://DEVICE/api/v1/status
curl -H "Authorization: Bearer $TOKEN" -X POST -d '{"ml":5}' http://DEVICE/api/v1/dispense
```

The JSON shapes match what `hub/` expects; the hub's simulator mirrors this API.

## Behavior summary

- Dispense is blocked (409) for: busy, bottle missing, flood latched, battery at or below the floor while on battery, bottle empty.
- Flood latches on a debounced float trip and stays until `POST /float/clear` with the sensor dry; the red LED flashes slowly.
- Scheduled runs need valid SNTP time. A run later than 120 s follows `missed_policy` (`skip`, or `run_once_on_boot`; at most one catch-up).
- LEDs: green on (fast flash while dispensing), blue on when connected (slow flash connecting, fast in setup), amber on with adapter power and off on
  battery, red slow flash on flood and fast flash when the bottle is missing.

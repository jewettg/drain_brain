# drain_brain
New code for iFlo devices that will replace existing proprietary control.

The target hardware is the iFLO Pro (IFLOPROKIT01, FCC ID 2BKR4-IFLOPROKIT01 / 2AUNV-IFLOPROKIT01), built around an **ESP32-S2-SOLO**.
The goal is a ground-up rewrite that uses only hardware facts discovered from the stock firmware (no original code is reused): local-only control over Wi-Fi with a REST API, an on-device dashboard, calendar scheduling, bottle tracking,
and float/power/bottle monitoring. No cloud, no vendor registration, no phone app. A separate fleet controller, **brain_drain_hub**,
manages many devices.

Status: reverse-engineering of the hardware interface is done (not yet bench-verified); firmware implementation has not started.

## Documentation

| Document | Contents |
| --- | --- |
| [documentation/HARDWARE_MAP.md](documentation/HARDWARE_MAP.md) | GPIO map, polarity, motor/valve drive, battery, buttons, open items |
| [documentation/FIRMWARE_PLAN.md](documentation/FIRMWARE_PLAN.md) | Architecture, Phase 0 bring-up probe, scheduling/time, REST API, milestones |
| [documentation/HUB_PLAN.md](documentation/HUB_PLAN.md) | brain_drain_hub plan |
| [tools/re/README.md](tools/re/README.md) | How the firmware analysis was done (no firmware or decompiled code is stored here) |

## Notes

- Keep a copy of the original flash dump of your own device before flashing anything. It restores the stock firmware.
- Third-party firmware images and decompiler output are intentionally not committed.

# iFLO Pro (IFLOPROKIT01) — Hardware Map from Firmware Analysis

What the stock firmware (app `20240410_2.5.13`, ESP-IDF 4.4.6) tells us about how to drive the hardware.
Only hardware behavior is recorded here. No cloud, bottle-registration, or app protocol was studied, and no
original code is reproduced.

**Confidence tags:** **[FW]** read directly from the firmware (high) · **[FW+log]** firmware plus the UART boot
log agree · **[INF]** inferred, needs a bench check · **[?]** unknown.
Nothing here has been verified on the bench yet; `FIRMWARE_PLAN.md` Phase 0 does that safely.

## 1. Corrections to our starting assumptions

| Assumption | Finding |
| --- | --- |
| "ESP32" | It is an **ESP32-S2** (module **ESP32-S2-SOLO**, single-core Xtensa LX7, 4 MB flash, Wi-Fi only, no Bluetooth). Build for target `esp32s2`. |
| "Pump motor, probably on/off" | On this PCB (6.3.0) the firmware drives the white connector as a **bidirectional pair** (H-bridge inputs INA/INB), and calls it a **valve** (Open/Close). See §4. |
| Battery "4×AA" | Manual contradicts itself (2 AA in one place, 4 AA in another). Firmware's battery range is 2.2–3.0 V, which looks like **2×AA**. Measure the pack. See §6. |
| 2 buttons | Firmware has **two** user inputs: a wake/dispense button and a reset/factory-reset button. Which physical switch is which is unconfirmed. See §7. |

## 2. Board identification

- Main board `QAT1P1_V1.1` (2024/03/02). Firmware reads PCB-version straps on **GPIO19/GPIO20**; ours reads `3`
  (PCB 6.3.0) **[FW+log]**. Native USB (GPIO19/20) is therefore not available.
- Power/interface board `QAT1PIK_3.0` (2024/03/01): 24 VAC input, buck regulator, float-switch interceptor, SW1.
- Main-board silkscreen worth knowing: `JP1` UART (VDD, GND, TXD, RXD), `BOOT` two-pad header (short = GPIO0 low
  for download mode), `BAT_ADC`, `ADC_CUR`, `INA1`, `INB1`, `OUTA1`, `OUTB1`, test points `KEY1`, `KEY2`, `PIP`, `3U3`.
- U1 (8-pin SOIC, marking removed) sits between INA1/INB1 and OUTA1/OUTB1 → the motor/valve H-bridge driver.
  R20 = **1 Ω** next to it → low-side current-sense shunt feeding `ADC_CUR` **[INF]**.

## 3. GPIO map (PCB variant 3)

Logic levels are 3.3 V. "Active" is the level the firmware treats as asserted.

### Inputs (all configured with internal pull-ups)

| GPIO | Function | Active level | Notes |
| --- | --- | --- | --- |
| 6 | **User button** (wake + manual dispense) | LOW | Firmware names the wake source "User button" **[FW]**. Debounce/hold timing in §7. |
| 17 | **Reset button** (not a wake source) | LOW | Short press → reboot; hold 5 s → factory reset + reboot **[FW]**. |
| 13 | **Bottle-present switch** (blue connector) | LOW = bottle present, HIGH = ejected | Wake source "Bottle eject". 3 s debounce **[FW+log]**. |
| 7 | **Float switch / PIP** | LOW = flooded | Wake source "Float switch". 2 s debounce. An inverting stage between PIP and the pin is implied by the manual (PIP rises to 3.2 V when clogged) **[FW + INF on inversion]**. |
| 16 | **External power sense** (24 VAC adapter) | HIGH = external power, LOW = on battery | Wake source "External power". Boot log with no adapter reports `BATTERY` **[FW+log]**. |
| 19, 20 | PCB-version straps | — | Read once at boot. |
| 8 | **Battery voltage** (ADC1 channel 7) | analog | See §6. |

### Outputs (all active-high, idle low)

| GPIO | Function | Notes |
| --- | --- | --- |
| 40 | LED1 | Green per board; status/Wi-Fi pattern **[FW order + INF color]** |
| 39 | LED2 | Blue per board; doses-left count and Wi-Fi pattern **[INF color]** |
| 41 | LED3 | Amber per board; power-state LED (off/slow/fast/solid) **[INF color]** |
| 42 | LED4 | Red; the firmware's logs call output 3 `RED_LED` **[FW]** |
| 9 | **INA1** → motor/valve driver input A | "Open" drive **[FW]** |
| 10 | **INB1** → motor/valve driver input B | "Close" drive **[FW]** |
| 15, 2 | Declared outputs, not used by any code path we found | Leave alone until traced **[?]** |

LED colors follow the order output0..3 = LED1..4, plus the confirmed red on output 3. A one-minute blink test settles it.

### Not used by the firmware
`ADC_CUR` (current sense) exists on the board but the firmware only configures ADC1 channel 7. Its GPIO is unknown **[?]**;
Phase 0 scans all ADC1 channels to find it. Older PCB variants (1, 2) use GPIO3 as a plain on/off pump output, not ours.

### UART / programming
JP1: 3.3 V TTL, `TXD`/`RXD` on the module's UART0 (GPIO43/44). Download mode: hold the `BOOT` pads shorted (GPIO0 low) while
resetting. The flash is not encrypted and secure boot is off (the dump was readable and starts at the standard offsets).

## 4. Pump / valve drive (white 2-pin connector, `OUTA1`/`OUTB1`)

How the stock firmware dispenses on PCB variant 3 **[FW]**:

1. **Open**: INA (GPIO9) high for **5 output ticks**, INB low, then both low.
2. Wait the dose time (default **5.0 s**; the unit is 0.1 s and the default is `50`).
3. **Close**: INB (GPIO10) high for 5 ticks, INA low, then both low.

A boot-time **Close** is issued before anything else, and a "finalize pending close" routine waits for both inputs low.
The output engine runs every 10 FreeRTOS ticks; assuming the usual 100 Hz tick that is **100 ms**, so each drive pulse is about
**500 ms** **[INF on tick rate]**.

Interpretation: a *pulse-in-one-direction-to-open, pulse-in-the-other-to-close* scheme means the load is most likely a
**motorized or latching valve** (or a motor that travels to end stops), not a pump that runs while energized. The
non-Pro boards, which have a single output, hold it on for the whole dose, which is the "pump" behavior you expected.
Bench check (power from the adapter, measure across the white connector with a meter before connecting anything else):

- Does polarity reverse between the open and close pulses? (confirms H-bridge/valve)
- Does the load keep drawing current after the pulse ends? (confirms latching vs. holding)

**Dose volume.** The firmware measures a dose only in *time*: a default dose time plus an optional list of up to 33 per-dose
times (`disp-timings`, one entry per dose already used) supplied by the cloud. That list is the "clue" for estimating remaining
fluid: dose duration grows with the number of doses used, which is consistent with a gravity- or pressure-fed bottle losing
head pressure. Remaining doses = bottle `max-doses` − `used-dose-cnt`, and the bottle is declared depleted when they meet.
Without the cloud, we replace all of this with a user-calibrated flow rate (see `FIRMWARE_PLAN.md`).

**Speed control.** The stock firmware never modulates the outputs (strictly digital pulses). Whether PWM on INA or INB changes
the speed depends on U1's part number, which is unmarked. Treat speed control as **unproven**; Phase 0 tests it carefully using
`ADC_CUR` to watch current.

**Safety.** Never run a pump dry. Limit any continuous-drive test to a few seconds with the line connected to liquid, and keep
a hard software maximum on-time.

## 5. Float switch (PIP)

`PIP` is 0 V normally and rises to ≈3.2 V when the float trips (manual). The firmware requires the asserted level to persist
**2 s** before it declares a flood, then raises the red LED (slow flash) and an event. A config flag `float-en` can disable the
feature; the factory default is **disabled (0)** in the config we dumped, so enable it explicitly in the new firmware.

## 6. Power and battery

- **Mains-side power**: GPIO16 high = adapter present. Losing it switches the firmware to "BATTERY" mode, turns the amber LED
  off, logs an event, and reduces activity (deep sleep with wake on button, bottle, float, external power).
  The same pin tells us when power is restored, so **outage start, outage end, and outage duration** are all available from one input.
- **Battery voltage**: ADC1 channel 7 (**GPIO8**), 11 dB attenuation, 13-bit, factory-calibrated through the eFuse (`esp_adc_cal`),
  average of 16 samples. The result is multiplied by **2.37** on this PCB (1.52 on older ones) to get pack millivolts **[FW]**
  (a ≈ 1:2.37 divider).
- **Percent**: linear between **2200 mV (0 %)** and **3000 mV (100 %)**, clamped, filtered so it only decreases (rises only if
  >10 points higher). Levels: **low ≤ 20 %**, **critical ≤ 10 %**. Sampling is skipped while the valve/motor is active, so the
  reading is not pulled down by the load. When external power is present the firmware does not evaluate battery level.
- **Dispense policy**: with battery below `disp-low-bat` (20 %) it refuses to dispense.
- **Open question**: 2200–3000 mV matches 2×AA (3.0 V fresh). If your pack is 4×AA in series (6.0 V) the pin would see ~2.5 V
  after the divider, near the ADC's upper range. Measure the pack and ADC pin once to fix the real divider ratio, then
  recompute the percent curve (alkaline cells are ~1.5 V fresh, ~1.0 V empty; the curve is steep, so use thresholds, not
  precise percent).

## 7. Buttons

| Button | GPIO | Behavior in stock firmware |
| --- | --- | --- |
| **User/wake** | 6 | Wakes from deep sleep. Press: clears the "doses left" LED display. A press held past the configured threshold triggers a manual dispense (when the device is registered/ready; thresholds are in 0.1 s units, ~1–3 s); the button also drives a doses-left LED display and a "go to sleep" action via click-count events whose exact mapping is not fully resolved. **Held while GPIO17 is pressed → factory test mode.** |
| **Reset (SW1?)** | 17 | Short press (release before 5 s) → immediate reboot. Hold **5 s** → "restore factory default config" (wipes Wi-Fi, registration, dose counters, bottle state) then reboot. |

The firmware's own labels settle *what each GPIO does*, but **not which physical switch is wired to which GPIO**. The power-board `SW1`
(your diagnostic sheet) shares the 4-wire connector with `KEY`; the main-board center button is the other candidate. Our answer to
"what does SW1 do" is therefore: **it is one of GPIO6 (wake/dispense) or GPIO17 (reset/factory reset)** — Phase 0's pin probe
resolves it by pressing each switch and watching which GPIO toggles. `iFLO_PRO_SW1_Diagnostic_Procedure.pdf` section 1/2 measurements
(KEY→GND resistance with SW1 pressed) will agree.

## 8. Test mode (for completeness)

Holding the GPIO6 button while GPIO17 is also pressed starts a factory self-test that flashes LEDs in sequence, cycles the valve, and checks
the power-source input. We do not need it, but it confirms the intended signal semantics (valve is cycled, LEDs 0–3 are independent).

## 9. Other behaviors of the stock device we are deliberately not keeping

Cloud registration, bottle QR/registration codes, the `iFLO device` SoftAP provisioning flow, HTTPS OTA from the vendor, and the
doses-from-cloud timing tables. The new firmware replaces them (see `FIRMWARE_PLAN.md`).

## 10. Stock-firmware facts useful for the new design

- Flash layout: NVS `0x9000` (16 K), otadata `0xd000`, phy_init `0xf000`, **ota_0 `0x10000` (1.5 MB)**, ota_1 `0x190000` (1.5 MB),
  config (SPIFFS) `0x310000` (960 K). Stock runs from ota_0; ota_1 is empty.
- Time: SNTP; no battery-backed RTC, so time is lost across a full power loss.
- Original sleep model: deep sleep with ext0/ext1/ULP wake sources on the four input GPIOs above (all RTC-capable on the S2).
- Stock default tasks: input/output polling every 100 ms; the float task logs once per second.

## 11. Open items

1. Which switch is on GPIO6 vs GPIO17 (SW1 question) — Phase 0.
2. Motor vs. latching valve behavior and real pulse width — Phase 0 scope on OUTA1/OUTB1 or meter + `ADC_CUR`.
3. GPIO of `ADC_CUR` — Phase 0 ADC scan.
4. Real battery divider and pack size — meter once.
5. Identity of U1 (marking removed) — determines PWM/speed feasibility.
6. GPIO15 and GPIO2 purpose — trace on the board.
7. FCC external/internal photos and the schematic references have not been mined yet; the internal-photo PDF may show U1 and
   the regulator clearly.

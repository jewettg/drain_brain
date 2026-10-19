# drain_brain Bring-Up Probe — Instructions for the Person With the Device

Thanks for helping. This takes about 60–90 minutes. We are going to put a small **read-mostly test program** on the iFLO Pro's
ESP32-S2 so we can learn, with certainty, which wire does what. The probe only turns something on when you type a command, and the
parts that can move the pump/valve are locked until you type `arm`. Everything is **reversible**: step 9 puts the original
firmware back.

You will send back: (1) a log file, (2) the filled-in results table in section 8, (3) a few photos.

## 1. What you need

**Required**

| Item | Notes |
| --- | --- |
| The iFLO Pro, opened up, with both boards and the 4-wire cable connected | The main board is `QAT1P1_V1.1`. |
| **USB-to-serial adapter with 3.3 V logic** (CP2102, FT232, CH340, etc.) | **Not 5 V.** If it has a 3.3 V/5 V jumper, set 3.3 V. |
| 3 jumper wires (Dupont) | adapter GND, TX, RX to the board's `JP1` |
| Something to briefly short the two **BOOT** pads (LED/button face of the main board) | tweezers or a jumper wire |
| Fresh AA batteries in the device's battery compartment | The device runs from batteries for this test. Do **not** power it from the adapter's 3.3 V pin. |
| Multimeter (DC volts and continuity; DC mA range is a bonus) | |
| A computer with Python 3.8+ | macOS, Linux, or Windows |
| The original flash backup `esp32_dump.bin` | SHA-256 should be `62ccc63f67d624a1ec5545352d4c46268899e382ceb7a8d28e918acadc30f9fe` if it is the copy sent to the project owner. Keep **your own** copy too. |
| A small cup of plain water and a container to catch drips | Do **not** use the vendor's liquid for testing. |
| Phone camera | for the photos in section 7 |

**Optional (some tests are skipped without these; say so in the results)**

- A **24 VAC transformer** and the red/blue power cable (HVAC low-voltage supply or a bench 24 VAC source) to test the external-power and float
  inputs. Only do this if you are comfortable working with it. Never work on live HVAC wiring.
- A float switch or the float-switch wires you can short together.
- A second meter or an oscilloscope.

**Install the tools once**

```
python3 -m pip install esptool pyserial
```

## 2. Safety rules

1. Unplug the 24 VAC and the DC adapter. Work from batteries only unless a step says otherwise.
2. **Never connect the serial adapter's VCC/3.3 V pin** to the board. Only GND, TX, RX.
3. Do not run the pump dry for more than the short pulses in this document. Keep a hand near the batteries so you can cut power.
4. The motor/valve test commands have built-in limits (pulses 0.6 s, holds 3 s). Do not look for ways around them.
5. Stop and ask if anything smokes, gets hot, or makes an unexpected noise.

## 3. Wire the serial adapter

The `JP1` header is on the **module side** of the main board (the face with the ESP32-S2 module), on the right edge beside the module. It is labelled **VDD, GND, TXD, RXD**.

| Board `JP1` | Adapter |
| --- | --- |
| GND | GND |
| TXD | **RX** |
| RXD | **TX** |
| VDD | **not connected** |

TX and RX cross over: the board's TXD goes to the adapter's RX.

Find the port name: macOS `ls /dev/cu.usb*`, Linux `ls /dev/ttyUSB*` (you may need `sudo usermod -aG dialout $USER` and to log in again),
Windows Device Manager → Ports (`COM5`, for example). Below, `PORT` means that name.

## 4. Confirm the backup and enter download mode

1. Make sure you still have the original `esp32_dump.bin` (4,194,304 bytes). If you have any doubt, take a fresh copy now, **twice**, and compare:
   ```
   python3 -m esptool --chip esp32s2 -p PORT -b 460800 read_flash 0 0x400000 backup_a.bin
   python3 -m esptool --chip esp32s2 -p PORT -b 460800 read_flash 0 0x400000 backup_b.bin
   shasum -a 256 backup_a.bin backup_b.bin      # Windows: certutil -hashfile backup_a.bin SHA256
   ```
2. **Download mode**: with the batteries out, short the two `BOOT` pads with tweezers (they are on the **opposite face**, the one with the four LEDs and the push button, at the top corner next to LED4), insert the batteries
   while still shorted, wait 1 second, then release the short.
3. Check it:
   ```
   python3 -m esptool --chip esp32s2 -p PORT chip_id
   ```
   You should see `ESP32-S2`. If you get "Failed to connect": reseat the TX/RX wires (swap them if needed), repeat step 2, and try a slower `-b 115200`.

## 5. Flash the probe

From the folder containing `drain_brain_probe_full_0x0.bin` (in `tools/bringup_probe/prebuilt/` of the project):

```
python3 -m esptool --chip esp32s2 -p PORT -b 460800 write_flash 0x0 drain_brain_probe_full_0x0.bin
```

Verify the file first with `SHA256SUMS` in the same folder if you like. Then take the batteries out for 5 seconds (BOOT pads **not** shorted)
and put them back in. The device is now running the probe, and the **original firmware is not running** until you do step 9.

Open the terminal and log everything:

```
python3 logterm.py PORT probe_log.txt
```

You should see `drain_brain probe 0.1.0 ready. Drive outputs are LOCKED.` and a `probe>` prompt. Type `help` to list commands. If you see nothing,
press Enter, then re-check the wiring. Leave the terminal open for the whole session.

## 6. Tests

Do them in order. After each, type `report` if you like; it prints everything on one screen. Anything unclear: write it down and carry on.

### T1 — Inputs and buttons (needs: batteries only)

The probe prints a line every time an input changes, for example `[ 12345 ms] GPIO6  user/wake button? -> LOW`.

1. Type `levels`. Write the HIGH/LOW values in the table.
2. Press and hold the **main-board center button** for 2 seconds, release. Write down which GPIO changed (6, 17, or neither).
3. Press and hold **SW1 on the power board** (the small board with the DC jack and capacitors; the 4-wire cable must be connected). Write down which GPIO changed.
4. Insert and remove the **bottle** (or press the blue-connector switch). Which GPIO changed, and was LOW the "bottle present" level?
5. (24 VAC only) Connect the 24 VAC supply, wait 5 seconds, then disconnect it. Which GPIO changed (expected GPIO16), HIGH with power or LOW?
6. (24 VAC and float only) With 24 VAC connected and the float interceptor cable plugged in, trip the float switch (lift the float or short its two wires).
   Which GPIO changed (expected GPIO7), and which level means "tripped"?

### T2 — LEDs

Type `ledwalk`. Each LED lights for 1.5 s. Write down the **color you actually see** for LED1..LED4. If nothing lights, type `led all on` and
tell us what happened.

### T3 — Battery voltage and the current-sense pin

1. With a meter, measure the **battery pack voltage** across the battery terminals and write it down. Note how many AA cells are installed.
2. Type `adc 5` (five readings, one per second). Write the **GPIO8** mV values. Two columns matter: the number before the slash is raw,
   after the slash is millivolts at the pin.

### T4 — The motor/valve output (the white 2-pin connector)

Connect your meter's probes across the two pins of the white connector (DC volts). If the load is not attached, still do steps 1–4; only the
mechanical observations will be missing.

1. Type `arm` (unlocks drive for 60 seconds; type it again if it expires).
2. Type `pulse a 500`. Record: meter reading **with sign** during the pulse, and what the load did (click, spin, move, nothing).
3. Type `pulse b 500`. Record: meter reading **with sign**; is it the opposite polarity to step 2?
4. Type `pulse a 500` a second time, then wait 5 seconds. Does the load keep doing anything after the pulse ends (still moving, still drawing current)?
5. **Only if the load turned out to be a plain pump** (it ran during the pulse and stopped when it ended) and it has liquid (water) available:
   type `hold a 2000`, then `hold b 2000`. Record direction/flow. If it is a valve, do **not** do this step.
6. Type `stop` when finished.

### T5 — Find the current-sense pin (optional but valuable)

This finds which GPIO senses motor current.

1. Type `adc 3` and write down GPIO1..GPIO5 and GPIO7 idle readings (GPIO6 and GPIO7 are also digital inputs, so their readings are rough).
2. For each N in 1, 2, 3, 4, 5: type `arm`, `curch N`, `pulse a 500`. The probe prints a **peak** reading for that pin during the pulse.
   The pin whose peak is clearly higher than its idle value is the current-sense pin. Write all five peaks down.
3. Type `curch off`.

### T6 — PWM (optional, only if T4 showed a plain motor and you have liquid)

`arm`, then `pwm a 100 2000`, `pwm a 50 2000`, `pwm a 25 2000`. Record whether the speed or flow visibly changes. Skip if the load is a valve.

### T7 — Wrap up

Type `report` once and keep the output in the log. Then press Ctrl-C to close the terminal. The log file `probe_log.txt` is what we need.

## 7. Photos (phone camera, good light)

1. **U1** (the 8-pin chip below the ESP32-S2 module, left side) as close as you can get; angle the light to try to read any marking.
2. Both sides of the main board, flat and in focus.
3. Both sides of the power board.
4. The battery compartment (how many cells, and the wiring/spring layout).
5. The white connector and what is plugged into it (the pump or valve): a close-up of any label or part number.

## 8. Results sheet (fill in and send back with `probe_log.txt`)

```
Date / your name:
Battery cells installed (count):    Pack voltage measured (V):
24 VAC available?  yes / no         Float switch available?  yes / no

T1  levels at idle:  GPIO6=   GPIO7=   GPIO13=   GPIO16=   GPIO17=   GPIO19=   GPIO20=
    main-board center button  -> GPIO ____   (pressed = HIGH / LOW)
    power-board SW1           -> GPIO ____   (pressed = HIGH / LOW)
    bottle present            -> GPIO13 = HIGH / LOW ;  bottle removed = HIGH / LOW
    external power on         -> GPIO16 = HIGH / LOW   (skipped?)
    float tripped             -> GPIO7  = HIGH / LOW   (skipped?)
T2  LED1 color:        LED2 color:        LED3 color:        LED4 color:
T3  GPIO8 mV (5 readings):                  meter pack voltage:
T4  pulse a 500: meter reading (signed):         load did:
    pulse b 500: meter reading (signed):         opposite polarity? yes / no
    holds state after pulse ends? yes / no / unsure
    pump ran during hold? yes / no / not tested
T5  idle GPIO1..5,7:                       peak during pulse GPIO1..5:
T6  speed changed with PWM? yes / no / not tested
Anything unexpected (noises, heat, resets):
```

## 9. Put the original firmware back

Enter download mode again (section 4, steps 2–3), then:

```
python3 -m esptool --chip esp32s2 -p PORT -b 460800 write_flash 0x0 esp32_dump.bin
```

Remove the batteries for 5 seconds and reinsert. The device should boot the original firmware (its LEDs light and it shows the usual `iFLO device`
Wi-Fi access point). If you cannot reach this point, stop and tell us; the dump is a full copy of the flash, so the device is recoverable.

## 10. What to send

- `probe_log.txt`
- The completed results sheet (section 8)
- The photos
- Any questions or oddities

## Troubleshooting

| Symptom | Likely cause / fix |
| --- | --- |
| `Failed to connect to ESP32-S2` | BOOT pads not shorted at power-up; TX/RX swapped; adapter at 5 V. Redo section 4 step 2. |
| Garbage characters in the terminal | Wrong baud (must be 115200) or adapter at 5 V. |
| Nothing prints, no prompt | Press Enter; check GND is connected; try swapping TX/RX. |
| `LOCKED: type arm first` | Drive commands need `arm`, valid for 60 seconds. |
| Board resets when the load runs | Weak batteries. Use fresh cells. |
| `ABORT: current-sense ... above limit` | The probe's current guard stopped a drive. Note the value and continue. |

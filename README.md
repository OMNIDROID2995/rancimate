<p align="center">
<img src="docs/assets/rancimate-logo.png" alt="RANCIMATE" width="250px" />
<br />
  
[![CC BY-NC-SA 4.0][cc-by-nc-sa-shield]][cc-by-nc-sa]


</p>

# RANCIMATE

**A fork of GaggiMate with special additions and adjustments for the Rancilio Silvia.**

RANCIMATE is a personal fork of [GaggiMate](https://github.com/jniebuhr/gaggimate) by
jniebuhr and contributors. It is tuned for a Rancilio Silvia whose boiler and brew group
are wrapped in insulation on newer models, and it is **not** an official GaggiMate release: please
do not ask the GaggiMate project for support with it. Everything GaggiMate does still
works; the additions are switched off until you enable them.

* [Why a fork?](#why-a-fork)
* [What RANCIMATE adds](#what-rancimate-adds)
* [Settings reference](#settings-reference)
* [Installing](#installing)
* [Updating](#updating)
* [Building from source](#building-from-source)
* [Status and limitations](#status-and-limitations)

## Why a fork?

Insulating the boiler of a Silvia cuts its idle losses to roughly a third: it needs only
about 2–3 % heater duty to hold brew temperature instead of 6–8 %. That is good for energy
and stability, but it breaks the assumptions the stock controller is tuned for. Measured
on this machine with stock GaggiMate:

| Problem | Measured | Cause |
|---|---|---|
| Overshoot after heat-up | +2.5 … +3.1 °C, peak ~5 min after reaching setpoint, > 10 min to settle | The integral term charges to ~40 % duty while heating; an insulated boiler cannot lose that energy again. |
| Temperature sag during a shot | −3.7 … −4.0 °C, minimum ~21 s into the shot | The boiler sensor sees a change ~22 s late (dead time), so feedback reacts when the shot is nearly over. The brew feedforward exists but is off by default. |

The identified plant: dead time L ≈ 22 s, gain k′ ≈ 0.5 K/s at full power, heater 1100 W.
The full analysis and design rationale are in [custom/CONCEPT.md](custom/CONCEPT.md).

## What RANCIMATE adds

### PID advanced for isolated boilers

*Settings → Machine → PID advanced for isolated boilers.* One master switch; **off means
exactly stock GaggiMate behaviour**. When on, the controller board gets these helpers:

* **Integral band.** The integral only accumulates within a few degrees below setpoint,
  so it no longer charges up during heat-up. Errors above setpoint are always integrated,
  so a charged integral can always discharge.
* **Integral authority limit.** The integral share of the heater power is capped (default
  12 %), with the integrator state back-calculated so nothing builds up invisibly.
* **Heat-up power cap.** Near the setpoint the heater power tapers down (default to 35 %
  at setpoint) so less heat is still stored in the element when the target is reached.
  The cap is **never** applied while water flows, so it cannot make the brew sag worse.
* **Brew feedforward that works.** While water flows, the heater gets the power the
  incoming cold water needs: `flow × 4.18 × (brew temperature − tank water temperature)`.
  The brew temperature always comes from the selected profile; the tank water
  temperature, heater wattage, losses and efficiency describe the machine. If no Kff is
  stored, it is derived from the heater wattage (1000 / W), so the feedforward is no
  longer a by-product of autotune.
* **Integral hold during and after a shot.** During a shot and for about one sensor dead
  time afterwards the integral is frozen and the P term softened, so the controller does
  not react to a sag the feedforward has already paid for.
* **Feedforward safety scaling fixed.** Upstream reduced the feedforward to ~70 % as soon
  as the temperature was even 0.01 °C below setpoint, i.e. at the start of most shots.
  RANCIMATE gives full feedforward at or below setpoint and backs off only above it.

### Autotune Watchdog

A switch inside the same section. The display measures **every shot one minute before,
during and three minutes after the draw**, and **every heat-up** for overshoot. It records
the spread (maximum minus minimum), the sag, the overshoot and the idle swing before the
shot.

* **Watchdog on:** after each shot whose spread exceeds the threshold (default 2 °C), it
  nudges one group of parameters in a small, bounded step and sends them to the
  controller once the machine is idle:

  | Observed | Adjustment |
  |---|---|
  | Temperature sags during the shot | brew feedforward factor up |
  | Temperature rises after the shot without a sag | brew feedforward factor down |
  | Overshoot after a sag | P factor during shot down, integral hold longer |
  | Swing already before the shot | integral authority down |
  | Overshoot after a heat-up | power at setpoint down (and integral band narrower if large) |
  | Clean heat-up | power at setpoint slightly up (faster heat-up) |

  While the watchdog is on it owns these values: the fields are read-only and the API
  ignores posted values, so saving other settings cannot undo its adjustments.
* **Watchdog off:** the values stay exactly as they are and can be edited by hand. Shots
  are still measured and shown, so you can tune manually with the same numbers.

Not evaluated: flush and cleaning profiles, shots shorter than 10 s or longer than 120 s,
shots without a stable minute before them, and shots started more than 5 °C away from the
setpoint.

### Other changes

* AMOLED standby screensaver (black screen with clock, status icons and a moving cup
  logo, configurable timeout) and RANCIMATE branding.
* Autotune starts from the stored heater wattage instead of the Gaggia default of 1360 W.
* Firmware version is the upstream version plus `-rancimate` (e.g. `v1.10.0-rancimate`).
* OTA updates, patch notes and app web bundles come from this repository.

## Settings reference

Defaults are the recommended starting point for an insulated Silvia.

| Setting | Default | Range | Meaning |
|---|---|---|---|
| Enable advanced control | off | – | Master switch. Off = stock behaviour. |
| Adapt parameters automatically | off | – | The Autotune Watchdog. |
| Allowed temperature spread | 2.0 °C | 0.3–10 | Target band for the watchdog. |
| Heater wattage | 1100 W | 300–3000 | Silvia 230 V: 1100 W. |
| Cold water temperature (tank) | 15 °C | 1–40 | Temperature of the water the pump draws. **Not** the brew temperature. |
| Heat loss during brew | 15 W | 0–200 | Feedforward model. |
| Heater efficiency | 0.95 | 0.5–1 | Feedforward model. |
| Integral band | 3.0 °C | 0–20 | 0 = off. |
| Integral authority | 12 % | 0–100 | 0 = unlimited. |
| Approach band | 8.0 °C | 0–40 | 0 = off. |
| Power at setpoint | 35 % | 5–100 | Heat-up power cap at setpoint. |
| Brew feedforward factor | 1.00 | 0.3–3 | Watchdog range 0.5–1.6. |
| Integral hold after shot | 22 s | 0–120 | ≈ sensor dead time. 0 = off. |
| P factor during shot | 0.50 | 0.05–2 | |

Suggested first steps: enable advanced control, leave the watchdog off and pull a few
shots to see the measurements, then switch the watchdog on.

## Installing

> [!IMPORTANT]
> RANCIMATE uses **protocol version 17**; stock GaggiMate uses 16. **Display and
> controller must always run RANCIMATE firmware together**, otherwise the version check
> keeps the heater locked.

Coming from stock GaggiMate, flash both boards once over USB. All files are on the
[release page](https://github.com/OMNIDROID2995/rancimate/releases) and in
[firmware/](firmware/). Both boards are ESP32-S3:

```sh
# Display (LilyGo T-RGB); for the headless display use the display-headless-* files
esptool.py --chip esp32s3 write_flash \
  0x0     display-bootloader.bin \
  0x8000  display-partitions.bin \
  0xe000  boot_app0.bin \
  0x10000 display-firmware.bin

# Controller board
esptool.py --chip esp32s3 write_flash \
  0x0     board-bootloader.bin \
  0x8000  board-partitions.bin \
  0xe000  boot_app0.bin \
  0x10000 board-firmware.bin
```

> [!WARNING]
> Always include `boot_app0.bin` at `0xe000`. A board that has received OTA updates may
> boot from its second app slot; without resetting the OTA data the new firmware lands in
> the inactive slot and the board keeps running the old version (on the controller this
> shows up as "Version mismatch, update controller").

`display-filesystem.bin` (offset `0xc90000`) contains the seed profiles. Flash it only on
a fresh install: it **overwrites your saved profiles and shot history**. Settings are kept
in NVS and survive the firmware flash.

## Updating

Once RANCIMATE is installed, *Settings → System → Update* checks the
[latest release](https://github.com/OMNIDROID2995/rancimate/releases/latest) of this
repository and updates display and controller together over the air.

Versions are compared as semver, and `1.10.0-rancimate` sorts **below** `1.10.0`.
Consequently stock GaggiMate will not offer RANCIMATE as an update (use USB, see above),
and a new RANCIMATE build on the same upstream base gets the same version number, so OTA
only offers it once the upstream base moves on. Flash such builds over USB.

## Building from source

Requirements: Python with `venv`, Node.js 22+ and npm. See
[custom/RANCIMATE.md](custom/RANCIMATE.md) for details.

```sh
python3 -m venv env
env/bin/python -m pip install -r custom/requirements-build.txt
npm --prefix web ci

# The web UI must be embedded before the display firmware is built
scripts/build_webui.sh
env/bin/pio run -e controller
env/bin/pio run -e display            # or display-headless
env/bin/pio run -t buildfs -e display # seed profiles (display-filesystem.bin)

# Unit tests, including the thermal shaping and watchdog suite
env/bin/pio test -e native -f test_thermal_shaping
```

## Status and limitations

* Built on one machine for one machine. The control additions are verified by unit tests
  and a simulated insulated boiler; validation on the machine is in progress.
* The watchdog adapts global parameters; per-profile adaptation and a profile-aware,
  look-ahead feedforward are planned (see [custom/CONCEPT.md](custom/CONCEPT.md), section 8).
* No machine can hold temperature with an unlimited flow: at 94 °C a 1100 W heater
  covers about 3.2 ml/s of cold water. For hot water (Americano) use a flow profile of
  ≤ 3 ml/s.
* The GitHub workflows are inherited from upstream and are tied to its infrastructure.

RANCIMATE is licensed like GaggiMate under CC BY-NC-SA 4.0. Original work © jniebuhr and
the GaggiMate contributors.

---

*The original GaggiMate README follows.*


This project upgrades a Gaggia espresso machine with smart controls to improve your coffee-making experience. By adding a display and custom electronics, you can monitor and control the machine more easily.

<img src="docs/assets/gaggimate_poster.jpg" alt="Gaggia Classic Installation" width="500" />

## Features

- **Temperature Control**: Monitor the boiler temperature to ensure optimal brewing conditions.
- **Brew timer**: Set a target duration and run the brewing for the specific time.
- **Steam and Hot Water mode**: Control the pump and valve to run the respective task.
- **Safety Features**: Automatic shutoff if the system becomes unresponsive or overheats.
- **User Interface**: Simple, intuitive display to control and monitor the machine.

## Screenshots and Images

<img src="docs/assets/standby-screen.png" alt="Standby Screen" width="300px" />
<img src="docs/assets/brew-screen.png" alt="Brew Screen" width="300px" />
<img src="docs/assets/pcb_render.png" alt="PCB Render" width="300px" />

### How to buy

You can buy your kit on https://shop.gaggimate.eu/

## How It Works

The display allows you to control the espresso machine and see live temperature updates. If the machine becomes unresponsive or the temperature goes too high, it will automatically turn off for safety.

## Docs

The docs were moved to [https://gaggimate.eu/](https://gaggimate.eu/). You can find all sourcing and assembly information there.
Additional documentation for the WebSocket API can be found in [docs/websocket-api.yaml](docs/websocket-api.yaml).

## Hardware Files

The hardware files were moved to [gaggimate/parts](https://github.com/gaggimate/parts).

## License

This work is licensed under CC BY-NC-SA 4.0. To view a copy of this license, visit https://creativecommons.org/licenses/by-nc-sa/4.0/

[sonar-violations]: https://img.shields.io/sonar/blocker_violations/jniebuhr_gaggimate?server=https%3A%2F%2Fsonarcloud.io&style=for-the-badge
[sonar-shield]: https://img.shields.io/sonar/quality_gate/jniebuhr_gaggimate?server=https%3A%2F%2Fsonarcloud.io&style=for-the-badge
[sonar-tech-debt]: https://img.shields.io/sonar/tech_debt/jniebuhr_gaggimate?server=https%3A%2F%2Fsonarcloud.io&style=for-the-badge
[sonar-url]: https://sonarcloud.io/project/overview?id=jniebuhr_gaggimate
[cc-by-nc-sa]: http://creativecommons.org/licenses/by-nc-sa/4.0/
[cc-by-nc-sa-image]: https://licensebuttons.net/l/by-nc-sa/4.0/88x31.png
[cc-by-nc-sa-shield]: https://img.shields.io/badge/License-CC%20BY--NC--SA%204.0-lightgrey.svg?style=for-the-badge

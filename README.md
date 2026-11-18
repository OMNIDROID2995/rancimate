<p align="center">
<img src="docs/assets/logo.png" alt="Logo" width="250px" />
<br />
  
[![](https://dcbadge.vercel.app/api/server/APw7rgPGPf)](https://discord.gg/APw7rgPGPf)
[![CC BY-NC-SA 4.0][cc-by-nc-sa-shield]][cc-by-nc-sa]
[![Sonar QG][sonar-shield]][sonar-url]
[![Sonar Violations][sonar-violations]][sonar-url]
[![Sonar Tech Debt][sonar-tech-debt]][sonar-url]


</p>

> ## RANCIMATE
>
> A fork of GaggiMate with special additions and adjustments for the Rancilio Silvia.
>
> **RANCIMATE** is a personal fork of [GaggiMate](https://github.com/jniebuhr/gaggimate)
> by jniebuhr and contributors, tuned for a Rancilio Silvia with a heavily insulated
> boiler. It is not an official GaggiMate release and is not supported by the GaggiMate
> project. Firmware versions are the upstream version plus `-rancimate`
> (e.g. `v1.10.0-rancimate`); OTA updates come from the releases of this repository.
>
> Additions on top of GaggiMate:
>
> * **PID advanced for isolated boilers** (Settings → Machine): integral band and
>   authority limit, heat-up power cap, configurable brew feedforward model, integral
>   hold during and after a shot, fixed feedforward safety scaling.
> * **Autotune Watchdog**: measures every shot before, during and after the draw and
>   adapts the parameters above in small steps until the spread stays within a
>   configurable threshold (default 2 °C).
> * AMOLED standby screensaver and RANCIMATE branding.
>
> Display and controller must always be flashed together (protocol version 17).
> Prebuilt binaries: [Releases](https://github.com/OMNIDROID2995/rancimate/releases) and [firmware/](firmware/).
> Details: [custom/RANCIMATE.md](custom/RANCIMATE.md), concept: [custom/concept/](custom/concept/).
> Licensed like GaggiMate under CC BY-NC-SA 4.0.


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

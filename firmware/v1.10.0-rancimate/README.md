# RANCIMATE v1.10.0-rancimate

Based on GaggiMate v1.10.0. Same file set as an upstream GaggiMate release: LilyGo T-RGB
display, headless display and the GaggiMate controller board (all ESP32-S3).

**Always write `boot_app0.bin` at 0xe000 together with the firmware**: after OTA updates a
board may boot from its second app slot and would otherwise keep running the old version.

**Always flash display and controller together** (protocol version 17); a mismatch
keeps the heater locked.

| File | Board | Flash offset (manual flashing) |
|---|---|---|
| `boot_app0.bin` | All boards: resets the OTA data so the board boots the freshly flashed app | 0xe000 |
| `display-bootloader.bin` | Display | 0x0 |
| `display-partitions.bin` | Display | 0x8000 |
| `display-firmware.bin` | Display (also used by OTA) | 0x10000 |
| `display-filesystem.bin` | Display, seed profiles (overwrites saved profiles and shots; not needed for updates) | 0xc90000 |
| `board-bootloader.bin` | Controller | 0x0 |
| `board-partitions.bin` | Controller | 0x8000 |
| `board-firmware.bin` | Controller (also used by OTA) | 0x10000 |
| `display-headless-bootloader.bin` | Headless display | 0x0 |
| `display-headless-partitions.bin` | Headless display | 0x8000 |
| `display-headless-firmware.bin` | Headless display (also used by OTA) | 0x10000 |
| `display-headless-filesystem.bin` | Headless display, seed profiles (same caveat as above) | 0xc90000 |
| `app-web.zip`, `app-web.json` | Web bundle for the mobile app (live update) | – |

Once this firmware is installed, OTA updates come from
https://github.com/OMNIDROID2995/rancimate/releases.

# RANCIMATE v1.10.0-rancimate

Based on GaggiMate v1.10.0. Built for the LilyGo T-RGB display (`display` env) and the
GaggiMate controller board.

**Always flash display and controller together** (protocol version 17); a mismatch
keeps the heater locked.

| File | Board | Flash offset (manual flashing) |
|---|---|---|
| `display-bootloader.bin` | Display | 0x0 |
| `display-partitions.bin` | Display | 0x8000 |
| `display-firmware.bin` | Display (also used by OTA) | 0x10000 |
| `display-filesystem.bin` | Display, seed profiles (overwrites saved profiles and shots; not needed for updates) | 0xc90000 |
| `board-bootloader.bin` | Controller | 0x0 |
| `board-partitions.bin` | Controller | 0x8000 |
| `board-firmware.bin` | Controller (also used by OTA) | 0x10000 |

Once this firmware is installed, OTA updates come from
https://github.com/OMNIDROID2995/rancimate/releases.

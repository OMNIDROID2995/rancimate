# RANCIMATE auf Gaggimate 1.10.0

Basis: Tag `v1.10.0`, Commit `34bf9fc5f6cf89c9929636fc23092b75284092f9`.
Die eigenen Änderungen liegen uncommitted im eigenständigen Git-Repository.

## Eigene Erweiterungen

- AMOLED-Screensaver: `src/display/ui/default/DefaultUI.cpp` und `.h`.
  Im Standby erscheint nach Ablauf des Timers eine schwarze Anzeige mit
  Uhr, Statussymbolen, Tipp-Symbol und beweglichem Tassenlogo. Antippen
  beendet den Screensaver. Animation: 500 ms einblenden, 4 s anzeigen,
  500 ms ausblenden, danach neue Positionen.
- Timer: `src/display/core/Settings.h` und `.cpp`,
  `src/display/plugins/WebUIPlugin.cpp` und
  `web/src/pages/Settings/tabs/GeneralTab.jsx`. Standard: 300 Sekunden;
  `0` deaktiviert den Screensaver. Die API verwendet Sekunden, die
  persistente Einstellung Millisekunden.
- Standby-Schriftzug: `eez-ui/icons/standby-logo.png`,
  `src/display/ui/default/eez/images/ui_image_standby_logo.c`,
  `src/display/ui/default/eez/screens.c` und
  `eez-ui/gaggimate.eez-project`. Die Bitmap ist wie das Original
  392 × 53 Pixel groß; RANCI ist kräftiger als MATE. Das separate Logo
  des Willkommensbildschirms bleibt davon unberührt.
- Screensaver-Tassenlogo:
  `src/display/ui/default/eez/images/ui_image_cup_rm_screensaver.c`.
- Web-Schriftzug: `web/src/components/Navigation.jsx` und `web/index.html`.
  Übersetzungen liegen unter `web/src/locales/`.

Die Grafikdateien und ihre LVGL-C-Bitmaps müssen bei Änderungen
übereinstimmen. `images.h` und `images.c` registrieren die Bildressourcen.

Das ältere Werkzeug `legacy/heatup_logger.py` zeichnet Statusdaten über
WebSocket auf; seine Funktion mit 1.10 wurde nicht geprüft.

### PID advanced for isolated boilers

Web UI: *Einstellungen → Maschine → PID advanced for isolated boilers*.
Konzept: `/home/linux/RANCIMATE/01…07-*.md`.

- **Hauptschalter** (`thermalAdvancedEnabled`, NVS `ta_en`). Aus = Controller
  regelt exakt wie Upstream; alle neuen Felder werden ignoriert.
- **Controller** (`lib/GaggiMateController/src/peripherals/Heater.*`,
  `lib/NayrodPID/src/SimplePID/*`): I-Freigabeband, I-Autoritätsgrenze mit
  Zustandsrückrechnung, Anfahr-Leistungsdeckel (beim Bezug aufgehoben),
  konfigurierbares Feedforward-Wärmemodell und FF-Faktor, Bezugsmodus
  (I-Anteil während und `brewHoldoffS` nach dem Bezug eingefroren, P-Faktor),
  korrigierte FF-Sicherheitsstaffelung (Sprung 1,0 → 0,7 knapp unter Sollwert).
- **Protokoll**: `HeaterSettings` Felder 10–20, `PROTOCOL_VERSION` 17.
  **Display und Controller immer zusammen flashen**, sonst sperrt der
  Versionsabgleich die Heizung.
- **Kff**: Ist der gespeicherte Kff 0, sendet das Display im erweiterten Modus
  `1000 / Heizleistung` (Default 1100 W).
- **Autotune Watchdog** (`src/display/plugins/ThermalWatchdogPlugin.*`, Logik in
  `src/display/core/thermal/ThermalTuner.h`): misst jeden Bezug 60 s davor,
  währenddessen und 180 s danach sowie jedes Aufheizen. Bei eingeschaltetem
  Watchdog werden nach jedem Bezug außerhalb der erlaubten Schwankung
  (Default 2 °C) FF-Faktor, P-Faktor/Haltezeit, I-Grenze, Anfahrleistung bzw.
  I-Band in kleinen, begrenzten Schritten angepasst. Ausgeschaltet bleiben die
  Werte stehen und sind editierbar; gemessen wird weiter. Solange der Watchdog
  aktiv ist, ignoriert die API gepostete Regelparameter.
- Nicht ausgewertet: Utility-Profile (Spülen, Reinigung), Bezüge < 10 s oder
  > 120 s, Bezüge ohne eine Minute stabile Vorlaufzeit, Bezüge mehr als 5 K
  neben dem Sollwert.
- Tests: `env/bin/pio test -e native -f test_thermal_shaping`.

## Bauen

Voraussetzungen: Python mit `venv`, Node.js und npm. Das lokale Python-Umfeld
liegt unter `env/`, Node-Abhängigkeiten unter `web/node_modules/`. PlatformIO
verwendet zusätzlich seine Pakete unter `~/.platformio/`. Falls die Umgebungen
fehlen, können sie anhand der Manifeste wiederhergestellt werden:

```sh
python3 -m venv env
env/bin/python -m pip install -r custom/requirements-build.txt
npm --prefix web ci
```

Nach Änderungen an der Weboberfläche muss sie **vor** dem Firmware-Build
eingebettet werden. Ein gewöhnliches `npm run build` allein reicht nicht:

```sh
npm --prefix web run build
gzip -f web/dist/assets/*.js web/dist/assets/*.css web/dist/*.html
python3 scripts/embed_webui.py --src web/dist --out src/display/webassets
env/bin/pio run -e display
```

Ergebnis: `.pio/build/display/firmware.bin`. Ohne eingebettete Weboberfläche
kann PlatformIO stattdessen einen leeren Platzhalter bauen. Flashen nur nach
vorheriger Freigabe und Prüfung des angeschlossenen Geräts.

## Gesicherter Altstand

`legacy/altstand-aenderungen.patch.gz` enthält die versionierten Änderungen
des alten Checkouts einschließlich binärer Änderungen.
`legacy/altstand-neue-dateien.tar.gz` enthält dessen vier unversionierte
Erweiterungsdateien. Basis ist Commit
`97afdb7d1d89c115df108350e228d0af11f67df4`; der Patch ist nicht für 1.10
gedacht. Die ursprüngliche Screensaver-Notiz und Logo-Vorschauen liegen
ebenfalls unter `legacy/`.

Für eine Wiederherstellung den alten Commit in einem **separaten** Checkout
auschecken, den entpackten Patch dort mit `git apply --binary` anwenden und
das Archiv neuer Dateien in diesen Checkout entpacken. Die bestehenden
1.10-Änderungen dabei nicht überschreiben.

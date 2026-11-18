# RANCIMATE — Thermisches Regelungskonzept für den gedämmten Boiler

Konzeptpaket für die Umstellung der Boiler-Temperaturregelung auf einen
**stark gedämmten Boiler** (Rancilio Silvia, Boiler + Brühgruppe mit Steinwolle
und Aluklebeband eingewickelt).

**Zielzustand**

1. Kein Temperatur-Overshoot beim Aufheizen (heute **+2,5 … +3,1 K**, Abbau > 10 min).
2. Kein zu starkes Absacken beim Bezug (heute **−3,7 … −4,0 K** über 36 s).

**Basis-Firmware:** jetzt `/home/linux/RANCIMATE/src` (eigenständiges Repo, inhaltsgleich mit) `gaggimate-src-1.10` (Upstream-Tag `v1.10.0`, Worktree-Commit `34bf9fc5`)
unter `/home/linux/Dokumente/Headless Gaggimate/gaggimate-src-1.10`.
Alle Pfad- und Zeilenangaben in diesem Paket beziehen sich auf diesen Baum.

### Stand des Arbeitsbaums (geprüft am 18.11.2026)

Der 1.10-Worktree enthält bereits **uncommittete** Änderungen: die Portierung des
AMOLED-Screensavers (`deepStandbyTimeout`, `DefaultUI`) und das
GAGGIMATE→RANCIMATE-Rebranding (Logo, `Navigation.jsx`, `web/index.html`, Locales).

Das ist für dieses Konzept in zweierlei Hinsicht relevant:

* **Kein Konflikt.** Die Screensaver-Arbeit berührt `GeneralTab.jsx`; dieses
  Konzept arbeitet in `MachineTab.jsx` und `CalibrationTab.jsx`. In
  `Settings.h/.cpp` und `WebUIPlugin.cpp` werden andere Properties an anderen
  Stellen ergänzt.
* **Perfekte Vorlage.** `deepStandbyTimeout` ist genau derselbe Fünfschritt
  (Property → Getter/Setter → HTTP-POST → HTTP-GET → Formularfeld), den dieses
  Konzept zehnmal wiederholt. Bei Unklarheit: `git diff` dieser Dateien ansehen.

Alle `file:line`-Angaben in diesem Paket beziehen sich auf den **Arbeitsbaum
inklusive dieser lokalen Änderungen** — also auf das, was im Editor zu sehen ist,
nicht auf den reinen `v1.10.0`-Stand.

---

## Kurzdiagnose

Beide Fehlerbilder sind aus den eigenen Messdaten der Maschine belegt
(`heatup_warm.csv`, `heatup_warm2.csv`, `shot-17.json`, `shot-21.json`,
`americano.csv` in `/home/linux/Dokumente/Headless Gaggimate/`).

| Befund | Ursache | Ebene |
|---|---|---|
| Overshoot +2,5…3,1 K, Spitze erst **285 s nach** Setpoint-Durchgang | I-Anteil lädt sich in der Anfahrphase auf ~40 % Stellgrad auf, während der gedämmte Boiler stationär nur ~2 % braucht. Ohne Wärmeverlust gibt es keinen Weg, diese Ladung abzubauen, außer lange über Setpoint zu stehen. | Regler-Struktur + Parametrierung |
| `DEFAULT_PID` Ki = 1,027 | Für diese Maschine ergibt die SIMC-Regel des eigenen Autotuners **Ki ≈ 0,14…0,17** — der Default ist **6–7× zu hoch**. Er stammt von einer ungedämmten Gaggia Classic. | Parametrierung |
| Absacken −4 K beim Bezug, Minimum bei ~21 s | Totzeit L ≈ 22 s. Die Rückführung erfährt die Störung erst, wenn der Bezug fast vorbei ist. **Feedback kann das strukturell nicht lösen.** | Physik → Feedforward nötig |
| Feedforward wirkungslos | Der Störgrößen-Feedforward (`combinedKff`) ist vollständig implementiert und auf dieser Hardware verdrahtet, steht aber per Default auf **0** und wird nur gesetzt, wenn beim Autotune eine Heizleistung mitgegeben wird. Die Heizleistung ist **gar keine persistente Einstellung** — sie lebt nur als React-State im Browser. | Fehlende Einstellung |

**Konsequenz:** ein relevanter Teil des Problems ist Konfiguration, nicht Code.
Deshalb beginnt der Plan mit **Phase 0 (Recommissioning, ohne Codeänderung)** —
diese sollte gemessen werden, bevor Code angefasst wird. Danach folgen die
strukturellen Lücken, die die Firmware tatsächlich hat.

---

## Dateien in diesem Paket

| Datei | Inhalt | Für wen |
|---|---|---|
| [01-ANALYSE.md](01-ANALYSE.md) | Ist-Zustand des Regelpfads mit `file:line`, Streckenidentifikation aus den Messdaten, quantitative Herleitung beider Fehlerbilder | Verstehen, warum |
| [02-REGELUNGSKONZEPT.md](02-REGELUNGSKONZEPT.md) | Soll-Design: 5 Bausteine, Formeln, Wechselwirkungen, **verworfene Alternativen mit Begründung** | Entscheidungen |
| [03-IMPLEMENTIERUNG.md](03-IMPLEMENTIERUNG.md) | Datei-für-Datei-Changeset mit einsetzbaren Code-Blöcken, in Phasen | Umsetzung Firmware |
| [04-WEBUI-SETTINGS.md](04-WEBUI-SETTINGS.md) | NVS-Properties, HTTP-API, Preact-Formular, i18n, Dämmungs-Preset | Umsetzung Web UI |
| [05-PARAMETER-REFERENZ.md](05-PARAMETER-REFERENZ.md) | Alle Parameter: NVS-Key, Typ, Bereich, Default, Empfehlung für diese Maschine | Nachschlagen |
| [06-TEST-VALIDIERUNG.md](06-TEST-VALIDIERUNG.md) | Native Unit-Tests, Streckenmodell aus den Messdaten, Abnahmekriterien am Gerät | Verifikation |
| [07-PROFILGESTEUERTE-REGELUNG.md](07-PROFILGESTEUERTE-REGELUNG.md) | Erweiterung: Bezugsmodus, Energiebilanz, profilbasierter vorausschauender Feedforward, Temperaturverlauf, Lernen; Befund Sicherheitsstaffelung | Bezugsstabilität |

Leseempfehlung beim ersten Durchgang: **01 → 02 → 03**.
Bei der Umsetzung: **03 und 04 parallel offen**, 05 als Nachschlagewerk.

---

## Phasenplan

| Phase | Inhalt | Code? | Flashen | Erwartete Wirkung |
|---|---|---|---|---|
| **0** | Heizleistung 1100 W eintragen, Autotune neu, `kf` = 0,909 | nein | nein | Overshoot ↓ deutlich, Absacken ↓ deutlich |
| **1** | I-Band + I-Autoritätsgrenze (`SimplePID`) | ja | Controller + Display | Overshoot-Ursache beseitigt |
| **2** | Anfahr-Leistungsdeckel (`Heater`) | ja | Controller + Display | Overshoot-Restwelle + Abklingzeit ↓ |
| **3** | Wärmemodell + Heizleistung als persistente Settings, Web UI | ja | Controller + Display | Feedforward reproduzierbar, bedienbar |
| **4** | Setpoint-Rampe (optional) | ja | Controller + Display | Kosmetik, Default aus |
| **5–8** | Bezugsmodus, Energiebilanz, profilgesteuerter FF, Temperaturverlauf, Lernen — siehe [07](07-PROFILGESTEUERTE-REGELUNG.md) §7 | ja | je nach Phase | Absacken und Nachschwinger beim Bezug ↓ |

Ab Phase 1 ändert sich das Protokoll (`PROTOCOL_VERSION` 16 → 17).
**Controller- und Display-Firmware müssen dann zusammen geflasht werden** — die
Firmware erzwingt exakte Versionsgleichheit
(`src/display/core/Controller.cpp:428`), ein Mismatch sperrt die Regelung.

---

## Verwendung mit Claude Code in VS Code

Dieses Verzeichnis liegt außerhalb des Source-Trees. Beim Arbeiten im Repo also
als Kontext mitgeben, z. B.:

```
Lies /home/linux/RANCIMATE/01-ANALYSE.md und /home/linux/RANCIMATE/03-IMPLEMENTIERUNG.md.
Setze daraus Phase 1 um. Arbeitsverzeichnis ist
/home/linux/Dokumente/Headless Gaggimate/gaggimate-src-1.10.
Halte dich an die dort angegebenen Einfügepunkte und ändere nichts darüber hinaus.
```

Fertige Prompts pro Phase stehen am Ende von [03-IMPLEMENTIERUNG.md](03-IMPLEMENTIERUNG.md).

### Vor dem ersten Commit

Der Source-Baum ist ein **Git-Worktree** von `gaggimate-src` auf dem Tag
`v1.10.0` — also ein detached-HEAD-Zustand ohne eigenen Branch:

```bash
cd "/home/linux/Dokumente/Headless Gaggimate/gaggimate-src-1.10"
git status            # erwartet: HEAD detached at v1.10.0
git switch -c rancimate/thermal-insulated-boiler
```

Erst danach committen, sonst landen die Änderungen auf einem losen HEAD.

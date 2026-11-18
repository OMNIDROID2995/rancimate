# 01 — Analyse: Ist-Zustand der Boiler-Temperaturregelung in GaggiMate 1.10

Alle Pfade relativ zu `/home/linux/Dokumente/Headless Gaggimate/gaggimate-src-1.10`.

---

## 1. Architektur des Regelpfads

GaggiMate ist auf **zwei ESP32** verteilt. Die Temperaturregelung läuft
vollständig auf dem **Controller**; das **Display** liefert nur den Sollwert und
die Reglerparameter.

```
┌─ DISPLAY (ESP32-S3) ──────────────────────────────────────────────┐
│                                                                   │
│  Settings (NVS)                                                   │
│    pid = "Kp,Ki,Kd,Kff"      Settings.h:539  (ein String!)        │
│    temperatureOffset         Settings.h:521                       │
│                                                                   │
│  Controller::getTargetTemp()        Controller.cpp:868            │
│    MODE_BREW  → Profil-/Phasen-Temperatur                         │
│    MODE_STEAM → settings.getTargetSteamTemp()                     │
│    MODE_WATER → settings.getTargetWaterTemp()                     │
│    MODE_STANDBY → 0  (= Heizung aus)                              │
│         │                                                         │
│  Controller::updateControl()        Controller.cpp:1073           │
│    targetTemp += temperatureOffset        :1087-1090              │
│    boiler.setpoint = targetTemp           :1104                   │
│    nur senden wenn boiler != lastBoiler   :1146                   │
│         │                                                         │
│  Controller::setHeaterSettings()    Controller.cpp:974            │
│    parseFloatCsv(pid, 4) → sendHeaterSettings(Kp,Ki,Kd,Kff,...)   │
└─────────┬─────────────────────────────────────────────────────────┘
          │  nanopb über BLE oder UART
          │  BoilerControl{index, mode, setpoint}   gaggimate.proto:80
          │  HeaterSettings{kp,ki,kd,kf,...}        gaggimate.proto:109
┌─────────▼─ CONTROLLER (ESP32) ────────────────────────────────────┐
│                                                                   │
│  onBoilerControl    GaggiMateController.cpp:255                   │
│    → heater->setSetpoint(setpoint)              :271              │
│  onHeaterSettings   GaggiMateController.cpp:338                   │
│    → heater->setFeedforwardScale(settings.kf)   :342              │
│    → heater->setTunings(kp, ki, kd)             :343              │
│                                                                   │
│  Heater::loopTask   Heater.cpp:292   — eigener Task, alle 10 ms   │
│    └─ Heater::loop()            Heater.cpp:41                     │
│         └─ Heater::loopPid()    Heater.cpp:100                    │
│              softPwm(1000 ms)          :101                       │
│              temperature = sensor->read()  :102                   │
│              Störgrößen-Feedforward         :106-126              │
│              simplePid->update()            :129   ← 1 Hz         │
│                                                                   │
│  SimplePID::update()   SimplePID.cpp:15   — der eigentliche PID   │
│  HeaterCoordinator     — nur Dual-Boiler, SSR-Staffelung          │
└───────────────────────────────────────────────────────────────────┘
```

### Zeitraster

| Größe | Wert | Quelle |
|---|---|---|
| Heater-Task-Tick | 10 ms | `Heater.cpp:297` |
| **PID-Abtastung** | **1 Hz** | `setSamplingFrequency(TUNER_OUTPUT_SPAN/1000) = 1,0` — `Heater.cpp:23`, Gate in `SimplePID.cpp:21` |
| Soft-PWM-Fenster | 1000 ms | `Heater.cpp:101`, `TUNER_OUTPUT_SPAN` = 1000 — `Heater.h:13` |
| Stellgrößenbereich | 0 … 1000 | `Heater.cpp:24` → 1 Einheit = 1 ms Einschaltzeit = 0,1 % Duty |
| `updateControl()` | ~20 Hz | `main.cpp:14-16`, `delay(50)` |

Der Stellgrad ist also **1 ms Einschaltzeit pro Einheit** in einem 1-s-Fenster.
`output = 350` bedeutet 35 % Duty.

---

## 2. Der Regler im Detail (`SimplePID.cpp:15-92`)

Paralleler PID mit Vorsteuerungen:

```
error    = setpointFiltered − temperature
Pout     = Kp · error
Iout     = Ki · ∫error dt
Dout     = Kd · d(error)/dt
FFOut    = Kff_sp · d(setpoint)/dt      ← deaktiviert
DistFFOut= Kff · Störgröße               ← Default 0
output   = clamp(Pout+Iout+Dout+FFOut+DistFFOut, 0, 1000)
```

### Was abgeschaltet ist

| Mechanismus | Zustand | Stelle |
|---|---|---|
| Setpoint-Filter (Rampe mit Ratenbegrenzung) | **aus** | `activateSetPointFilter(false)` — `Heater.cpp:25` |
| Setpoint-Feedforward | **aus** | `activateFeedForward(false)` — `Heater.cpp:26` |
| Störgrößen-Feedforward | **implementiert, Gain = 0** | `setFeedforwardScale(0.0f)` — `GaggiMateController.cpp:247`; `DEFAULT_PID` 4. Wert = `0.0` — `constants.h:24` |

Der Setpoint-Filter existiert vollständig inklusive Ratenbegrenzung
(`setpointRatelimits[2] = {-INFINITY, 2}` — `SimplePID.h:63`, also max +2 K/s)
und wird nie benutzt. Der Sollwertsprung erreicht den Regler also **ungefiltert**.

### Anti-Windup (`SimplePID.cpp:70-82`)

```cpp
bool isSaturated = (sumPID < limits[0] || sumPID > limits[1]);
bool isSameSign  = ((error > 0 && sumPID > 0) || (error < 0 && sumPID < 0));
if (isSaturated && isSameSign) {
    feedback_integralState -= error * deltaTime;   // Integration zurücknehmen
    ...
}
```

Das ist **konditionale Integration** und funktioniert korrekt — **solange der
Ausgang sättigt**. Genau darin liegt das Problem: in der Anfahrphase verlässt der
Regler die Sättigung lange bevor der Sollwert erreicht ist, und ab diesem Moment
lädt der I-Anteil ungebremst. Es gibt **keine** Autoritätsgrenze für `Iout` und
**kein** Fehlerband, in dem die Integration freigegeben wird. Siehe Abschnitt 4.

---

## 3. Streckenidentifikation aus den eigenen Messdaten

Quelle: `/home/linux/Dokumente/Headless Gaggimate/heatup_warm2.csv`
(1472 Samples, 760 s, Spalten `t_sec, ct, tt, pr, fl`), gegengeprüft mit
`heatup_warm.csv`.

| Parameter | Wert | Ermittlung |
|---|---|---|
| **L** (Totzeit) | **≈ 22 s** | Sollwert bei t = 7,5 s auf 94 °C; erste Bewegung > +0,5 K bei t = 29,9 s |
| **k′** (Verstärkung bei Vollgas) | **≈ 0,50 K/s** (30 K/min) | maximale Steigung über 20-s-Fenster; 5-Sample-Fenster liefert 0,59 K/s |
| Netto-Abkühlrate bei ~96 °C | −0,004 … −0,008 K/s | Segmente 440–560 s und 600–760 s |
| **Stationärer Haltestellgrad** | **≈ 1,5 … 3 %** | Abkühlrate / k′; Obergrenze, weil die Heizung in diesem Fenster noch nicht ganz aus war |

Zum Vergleich: eine **ungedämmte** Silvia braucht bei 94 °C grob 6–8 % Haltestellgrad.
Die Dämmung senkt den Bedarf also um den Faktor **3–4**. Genau dieser Faktor macht
den Overshoot so zäh — die überschüssige Energie hat keinen Abflussweg.

> **Methodische Einschränkung:** Der Stellgrad wird im CSV nicht mitgeloggt
> (`heatup_logger.py` zeichnet nur `ct/tt/pr/fl` auf). Alle Aussagen über den
> I-Anteil in Abschnitt 4 sind daher **Rückschlüsse aus der Kurvenform**, keine
> direkte Messung. Für die Verifikation ist ein Duty-Log nötig — siehe
> [06-TEST-VALIDIERUNG.md](06-TEST-VALIDIERUNG.md), Abschnitt „Duty-Logging
> nachrüsten“.

### Was die SIMC-Regel der Firmware für diese Strecke liefern würde

Der eigene Autotuner (`Autotune.cpp:156-199`) implementiert Skogestads SIMC-Regel
für Integrator-mit-Verzögerung:

```
τc = 1,5 · L                    (hart verdrahtet, Autotune.cpp:177)
Kc = 1 / (k′ · (τc + L))
Ti = 4 · (τc + L)
Td = τ₂     (Fallback 0,2 · L wenn nicht identifizierbar)
```

Eingesetzt mit L = 22 s, k′ = 0,50 K/s, τ₂ = 0,2·L = 4,4 s
(Faktor 1000 wegen der Skalierung in `Heater.cpp:210`):

| Gain | SIMC für **diese** Maschine | `DEFAULT_PID` (`constants.h:24`) | Verhältnis |
|---|---|---|---|
| Kp | ≈ 31 … 36 | 58,397 | 1,7× zu hoch |
| **Ki** | **≈ 0,14 … 0,17** | **1,027** | **6–7× zu hoch** |
| Kd | ≈ 140 … 160 | 249,055 | 1,6× zu hoch |

`DEFAULT_PID` ist für eine ungedämmte Gaggia Classic parametriert. Für die
gedämmte Silvia ist vor allem **Ki massiv zu groß** — und Ki ist genau der Term,
der den Overshoot erzeugt.

---

## 4. Fehlerbild 1 — Overshoot beim Aufheizen

### Messung

| Datei | Start | Ziel | Spitze | Overshoot | Spitze erreicht | Eingeschwungen |
|---|---|---|---|---|---|---|
| `heatup_warm.csv` | 57,3 °C | 94 °C | 97,13 °C | **+3,13 K** | t = 140 s | nicht in 317 s |
| `heatup_warm2.csv` | 53,8 °C | 94 °C | 96,50 °C | **+2,50 K** | t = 436 s | nicht in 760 s |

Der entscheidende Befund ist nicht die Höhe, sondern die **Form**:
In `heatup_warm2` wird der Sollwert bei t = 151 s durchlaufen, die Spitze aber
erst bei t = 436 s erreicht — die Temperatur steigt also **285 s lang weiter**,
obwohl sie längst über dem Sollwert liegt. Danach fällt sie über 5 Minuten
langsam zurück und ist bei t = 760 s noch bei 94,5 °C.

Reine Totzeit/Speichermasse erklärt das nicht: bei L = 22 s wäre die Spitze
~22–40 s nach dem Durchgang zu erwarten. **285 s bedeutet, dass weiter geheizt wird.**

### Mechanismus

Sättigungsgrenze: der Ausgang verlässt die Sättigung, wenn
`Kp·e + Dout ≤ 1000`. Mit Kp = 58,397 und `Dout = Kd·(−dT/dt) = 249·(−0,5) ≈ −125`:

```
58,397 · e − 125 ≤ 1000   →   e ≤ 19,3 K
```

Ab **19,3 K unter Sollwert** ist der Regler also linear — und erst ab da greift
die konditionale Integration nicht mehr. Von dort bis `e = 0`:

```
∫e dt  ≈  19,3 K · ~20 s  ≈  390 K·s
Iout   =  Ki · ∫e dt  =  1,027 · 390  ≈  400 Einheiten  =  40 % Duty
```

Benötigt wären **1,5–3 %**. Der I-Anteil ist damit um den Faktor **~15–25 zu groß**.

Abbau: der I-Zustand sinkt nur, solange `error < 0`. Bei −2,5 K sind das
2,5 K·s pro Sekunde, also `dIout/dt = 1,027 · 2,5 ≈ 2,6` Einheiten/s.
Um ~380 Einheiten Überschuss abzubauen: **≈ 145 s**.

Das ist die richtige Größenordnung für die beobachteten 285 s (die Schätzung des
Fehlerintegrals ist grob, weil die Anfahrkurve flacher verläuft als exponentiell
angenommen). **Die Kurvenform ist damit erklärt.**

### Zwei unabhängige Verstärker

1. **Ki ist 6–7× zu groß** → Parametrierungsproblem, ohne Code lösbar (Phase 0).
2. **Es gibt keine Autoritätsgrenze und kein Freigabeband für den I-Anteil** →
   Strukturproblem. Auch mit korrektem Ki lädt der Integrator in der Anfahrphase
   mehr als nötig; bei einem fast verlustfreien Boiler ist das nicht gutartig.

Beide müssen adressiert werden. Phase 0 allein reduziert, Phase 1 beseitigt.

---

## 5. Fehlerbild 2 — Absacken beim Bezug

### Messung

| Datei | Dauer | Ziel | Start | Minimum | Absacken | Minimum bei |
|---|---|---|---|---|---|---|
| `shot-17.json` | 36,5 s | 94 °C | 94,7 °C | 91,0 °C | **−3,7 K** | t = 23,0 s |
| `shot-21.json` | 36,6 s | 94 °C | 93,7 °C | 89,7 °C | **−4,0 K** | t = 20,8 s |
| `americano.csv` | Session | 94 °C | 93,5 °C | 84,4 °C | **−9,1 K** | langer Wasserbezug |

### Mechanismus — Feedback kann das strukturell nicht lösen

Leistungsbedarf beim Bezug:

```
P = ṁ · cp · ΔT = 2 g/s · 4,18 J/(g·K) · (94 − 20) K ≈ 620 W
```

Bei 1100 W Heizleistung (Silvia 230 V) sind das **~56 % Duty**, die *ab der
ersten Sekunde* nötig wären. Die Rückführung erfährt die Störung aber erst nach
**L ≈ 22 s**. Das Minimum liegt in beiden Shots bei 21–23 s — also **genau bei
der Totzeit**. Der Bezug ist nach 36 s vorbei, bevor der Regler überhaupt
reagieren konnte.

Das ist kein Tuningproblem. Bei L ≈ 0,6 · Bezugsdauer ist **Störgrößen-Feedforward
die einzige Lösung**: die Leistung muss aus dem *bekannten Durchfluss* vorgesteuert
werden, nicht aus dem gemessenen Temperaturabfall.

### Der Feedforward ist schon da — und aus

`Heater::calculateDisturbanceFeedforwardGain()` — `Heater.cpp:254-274`:

```cpp
powerPerFlowRate = (ρ·cp·(setpoint − incomingWaterTemp) + heatLossWatts/flow) / heaterEfficiency;
gainPerFlowRate  = powerPerFlowRate · combinedKff;
```

mit `combinedKff = 1000 / Heizleistung` (`Heater.cpp:208`).

Nachgerechnet für diese Maschine (1100 W, Sollwert 94 °C, `incomingWaterTemp` 23 °C,
`heatLossWatts` 5 W, `heaterEfficiency` 0,95 — alles aus `Heater.h:86-89`):

```
combinedKff      = 1000 / 1100               = 0,909
powerPerFlowRate = (4,18 · 71 + 5/2) / 0,95  ≈ 315 W pro (ml/s)
gain             = 315 · 0,909               ≈ 286 Einheiten pro (ml/s)
bei 2 ml/s       → 572 Einheiten             = 57 % Duty
```

**57 % gegen 56 % physikalischen Bedarf.** Das Modell ist korrekt und würde das
Absacken nahezu vollständig kompensieren.

Außerdem gibt es eine Sicherheitsstaffelung, die den Feedforward zurücknimmt,
wenn die Temperatur über den Sollwert geht (`calculateSafetyScaling` —
`Heater.cpp:276-290`), sowie eine Glättung mit α = 0,85 (`Heater.cpp:115-117`).

### Warum er trotzdem nicht wirkt

Vier Gründe, alle behebbar:

1. **`combinedKff` ist 0** — `DEFAULT_PID` endet auf `0.0` (`constants.h:24`) und
   `setFeedforwardScale(0.0f)` beim Setup (`GaggiMateController.cpp:247`).
2. **Die Heizleistung ist keine persistente Einstellung.** Sie existiert nur als
   React-State im Browser: `useState(1360)` — `CalibrationTab.jsx:20`. Sie wird
   ausschließlich als Autotune-Argument übertragen (`WebSocketHandler.cpp:322`)
   und ist nach dem Reload verloren. Der Default 1360 W ist Gaggia Classic, nicht
   Silvia (1100 W bei 230 V).
3. **Kff entsteht nur als Nebenprodukt des Autotunes.** Wer nie autotunt, oder
   ohne Wattzahl autotunt, hat dauerhaft Kff = 0 (`Heater.cpp:208`:
   `wattage > 0 ? 1000/wattage : 0`).
4. **Das Wärmemodell ist hart verdrahtet** — `Heater.h:86-89`:
   `incomingWaterTemp = 23 °C`, `heatLossWatts = 5 W`, `heaterEfficiency = 0,95`.
   Keine Setter, nicht über das Protokoll erreichbar. `incomingWaterTemp` ist
   der sensibelste Wert: reales Leitungswasser hat 8–15 °C, das sind ~15 %
   Unterschied im Feedforward.

### Voraussetzung auf dieser Maschine: erfüllt

Der Feedforward ist nur verdrahtet, wenn die Hardware Dimming **und** Drucksensor
hat (`GaggiMateController.cpp:241`), weil er den Durchfluss aus `DimmedPump` liest
(`getPumpFlowPtr()` / `getValveStatusPtr()` — `DimmedPump.h:27,29`).

Aus `americano.csv` und den Shot-Logs: Durchfluss bis **10,6 ml/s**, Druck bis
**13,2 bar** — beides wird gemessen. **Die Voraussetzung ist auf dieser Maschine
erfüllt**, der Pfad ist aktiv, nur der Gain ist null.

---

## 6. Weitere relevante Randbedingungen

### Protokollversion erzwingt gleichzeitiges Flashen

`PROTOCOL_VERSION = 16` (`Protocol.h:28`) wird auf **exakte Gleichheit** geprüft
(`Controller.cpp:428`). Bei Abweichung wird `updateControl()` komplett gesperrt
(`Controller.cpp:1076-1078`) — die Maschine heizt nicht mehr, nur OTA bleibt
erreichbar. Jede Protokolländerung erfordert also Controller **und** Display
zusammen.

### Kein absoluter Übertemperaturschutz in der Software

`thermalRunawayShutdown()` (`GaggiMateController.cpp:580`) wird **nur vom
Sensorfehler-Callback** ausgelöst (`GaggiMateController.cpp:51,59,63,69,74`),
nicht von einer Temperaturschwelle. Die einzige Temperaturgrenze in der Software
ist `MAX_AUTOTUNE_TEMP = 125 °C` und gilt ausschließlich während des Autotunes
(`Heater.h:12`, `Heater.cpp:168`).

Im Normalbetrieb schützt also nur die Hardware (Thermosicherung/Thermostat).

**Für dieses Konzept ist das günstig:** alle vorgeschlagenen Änderungen
*begrenzen* die Heizleistung in der Nähe des Sollwerts. Sie sind damit strikt
sicherheitsneutral bis sicherheitssteigernd — es wird kein neuer Pfad geschaffen,
der mehr Leistung freigeben könnte.

### `getSetpoint()` hat einen externen Konsumenten

`GaggiMateController.cpp:503` vergleicht `heater2->getSetpoint() != target`, um
redundante Aufrufe zu vermeiden. Außerdem Logging auf `:745` und `:754`.

**Folge für Phase 4 (Setpoint-Rampe):** `getSetpoint()` muss weiterhin den
**kommandierten** Sollwert liefern, nicht den Rampenwert. Der Rampenwert braucht
eine eigene Variable. Dasselbe gilt für das Abschalt-Gate
`setpoint <= 0.0f` in `Heater::loop()` (`Heater.cpp:47`) — das muss auf den
kommandierten Wert prüfen, sonst schaltet die Heizung beim Abschalten nicht ab
oder bleibt während der Rampe aus.

### Heizungsparameter werden von Maschinenprofilen gesperrt

Das Formular-Fieldset „Heating Parameters“ ist deaktiviert, wenn ein
Maschinenprofil gewählt ist: `disabled={!!formData.machineProfileSelection}`
(`MachineTab.jsx:212`). Für eine Silvia mit individueller Dämmung muss die
Maschinenprofil-Auswahl also leer sein, sonst sind die Felder read-only.
Siehe [04-WEBUI-SETTINGS.md](04-WEBUI-SETTINGS.md).

### Sollwert-Übertragung ist deltabasiert

`updateControl()` sendet den Boiler-Befehl nur bei Änderung
(`Controller.cpp:1146`), Vergleich über exakte Float-Gleichheit
(`GaggiMateComm.h:34`). Bei ~20 Hz Schleifenfrequenz würde ein display-seitig
kontinuierlich verändertes Setpoint **20 Nachrichten/s** erzeugen.

Das ist der Hauptgrund, warum die Sollwertformung in diesem Konzept
**controllerseitig** liegt und nicht im Display — siehe
[02-REGELUNGSKONZEPT.md](02-REGELUNGSKONZEPT.md), „Verworfene Alternativen“.

---

## 7. Zusammenfassung der Befunde

| # | Befund | Art | Phase |
|---|---|---|---|
| B1 | Ki = 1,027 ist 6–7× größer als SIMC für diese Strecke | Parametrierung | 0 |
| B2 | Heizleistung nicht persistent (nur React-State, Default 1360 W falsch) | fehlende Einstellung | 0 / 3 |
| B3 | `combinedKff` = 0 per Default, entsteht nur via Autotune | Parametrierung | 0 / 3 |
| B4 | Keine Autoritätsgrenze für den I-Anteil | Reglerstruktur | 1 |
| B5 | Kein Freigabeband für die Integration | Reglerstruktur | 1 |
| B6 | Kein Leistungsdeckel in der Anfahrphase | Reglerstruktur | 2 |
| B7 | Wärmemodell (`incomingWaterTemp`, `heatLossWatts`, `heaterEfficiency`) hart verdrahtet | fehlende Einstellung | 3 |
| B8 | Setpoint-Filter vorhanden, aber abgeschaltet | ungenutzte Fähigkeit | 4 |

→ Weiter mit [02-REGELUNGSKONZEPT.md](02-REGELUNGSKONZEPT.md).

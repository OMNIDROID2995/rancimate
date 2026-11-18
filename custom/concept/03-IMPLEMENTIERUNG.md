# 03 — Implementierung: Changeset nach Phasen

Arbeitsverzeichnis: `/home/linux/Dokumente/Headless Gaggimate/gaggimate-src-1.10`

Design und Begründungen: [02-REGELUNGSKONZEPT.md](02-REGELUNGSKONZEPT.md).
Web-UI-Teil: [04-WEBUI-SETTINGS.md](04-WEBUI-SETTINGS.md).
Parametertabelle: [05-PARAMETER-REFERENZ.md](05-PARAMETER-REFERENZ.md).

**Vor dem ersten Commit Branch anlegen** — der Worktree ist detached:

```bash
cd "/home/linux/Dokumente/Headless Gaggimate/gaggimate-src-1.10"
git switch -c rancimate/thermal-insulated-boiler
```

Formatierung nach jeder Phase: `./scripts/format.sh` (clang-format ist über
`.clang-format` konfiguriert).

---

## PHASE 0 — Recommissioning (kein Code)

**Vor jeder Codeänderung durchführen und messen.** Behebt die Befunde B1–B3 und
liefert die Referenzkurve, gegen die alle späteren Phasen bewertet werden.

1. **Referenzmessung aufnehmen** (Ist-Zustand, für den Vergleich) —
   `heatup_logger.py` aus `/home/linux/Dokumente/Headless Gaggimate/` von
   ~55 °C auf 94 °C, mindestens 15 min laufen lassen.

2. **Autotune neu laufen lassen**, Web UI → Settings → Calibration:
   | Feld | Wert |
   |---|---|
   | Test Duration | `150` s (L ≈ 22 s, Default 120 s ist knapp) |
   | Samples / Window | `6` |
   | **Heater Wattage** | **`1100`** (Silvia 230 V — *nicht* der Default 1360) |

   Boiler muss unter 50 °C sein. Der Autotune setzt dabei auch
   `kf = 1000/1100 = 0,909` (`Heater.cpp:208`).

3. **Ergebnis prüfen**, Web UI → Settings → Machine → Heating Parameters.
   Erwartet laut [01-ANALYSE.md](01-ANALYSE.md) §3: Kp ≈ 31–36, **Ki ≈ 0,14–0,17**,
   Kd ≈ 140–160, Kff ≈ 0,909.

   Weicht Ki stark ab (z. B. > 0,4), hat die Identifikation die Strecke nicht
   getroffen — dann Werte von Hand setzen: `35,0 , 0,16 , 150,0` und `kf` `0,909`.

   > Das Fieldset „Heating Parameters“ ist gesperrt, solange ein Maschinenprofil
   > gewählt ist (`MachineTab.jsx:212`). Maschinenprofil-Auswahl muss leer sein.

4. **Nachmessung** mit identischem Protokoll wie Schritt 1.

**Erwartung:** Overshoot von +2,5…3,1 K auf **< 1,5 K**, Abklingzeit deutlich
kürzer; Absacken von −4 K auf **< 2 K**. Abnahmekriterien in
[06-TEST-VALIDIERUNG.md](06-TEST-VALIDIERUNG.md).

Erst wenn das gemessen ist, weiter mit Phase 1.

---

## PHASE 1 — Freigabeband und Autoritätsgrenze für den I-Anteil

Bausteine 1 + 2. Zwei Dateien im Regler, dann die Durchleitung.

### 1.1 `lib/NayrodPID/src/SimplePID/SimplePID.h`

**Public API ergänzen** — nach `void setDisturbanceGain(...)` / `getDisturbanceGain()`
(aktuell Zeile 49–50):

```cpp
    // --- Anti-Windup-Erweiterung fuer nahezu integrierende Strecken ---------
    // Gedaemmte Boiler brauchen stationaer nur ~2 % Stellgrad. Ohne diese
    // beiden Grenzen laedt der I-Anteil in der Anfahrphase auf ~40 % auf und
    // kann ihn danach nur abbauen, indem die Temperatur lange ueber dem
    // Sollwert steht (siehe 01-ANALYSE.md, Abschnitt 4).

    // Integration nur freigeben, solange error <= band. Negative Fehler werden
    // IMMER integriert, damit ein geladener I-Anteil abgebaut werden kann und
    // keine stationaere Abweichung entsteht. 0 = aus (Upstream-Verhalten).
    void setIntegralBand(float band) { integralBand = band; }

    // Klammer auf den I-Beitrag in Stellgroessen-Einheiten. hi <= 0 = unbegrenzt.
    // Der Integratorzustand wird mitgezogen (clampedIntegralOutput), damit er
    // nicht unsichtbar weiterlaeuft.
    void setIntegralOutputLimits(float lo, float hi) {
        integralOutputLimits[0] = lo;
        integralOutputLimits[1] = hi;
    }
```

**Private Member ergänzen** — bei den Feedback-Membern, nach
`float feedback_integralState = 0.0f;` (aktuell Zeile 80):

```cpp
    float integralBand = 0.0f;                              // 0 = immer integrieren
    float integralOutputLimits[2] = {-INFINITY, INFINITY};  // Klammer auf Iout
    float clampedIntegralOutput();                          // Iout + Zustandsrueckrechnung
```

> `INFINITY` ist über das bereits vorhandene `<cmath>` verfügbar
> (`SimplePID.h:3`), und `ctrlOutputLimits` nutzt dasselbe Muster.

### 1.2 `lib/NayrodPID/src/SimplePID/SimplePID.cpp`

**(a) Helfer anlegen** — z. B. direkt vor `SimplePID::update()`:

```cpp
// I-Beitrag inklusive Autoritaetsgrenze. Beim Begrenzen wird der
// Integratorzustand zurueckgerechnet, damit Zustand und Wirkung konsistent
// bleiben: eine reine Ausgangsbegrenzung liesse den Zustand unsichtbar
// weiterlaufen und der Overshoot waere nur verschleiert.
float SimplePID::clampedIntegralOutput() {
    float Iout = gainKi * feedback_integralState;
    if (integralOutputLimits[1] > 0.0f && gainKi != 0.0f) {
        const float clamped = constrain(Iout, integralOutputLimits[0], integralOutputLimits[1]);
        if (clamped != Iout) {
            feedback_integralState = clamped / gainKi;
            Iout = clamped;
        }
    }
    return Iout;
}
```

**(b) Integration umbauen** — aktuell Zeile 59–60:

```cpp
    feedback_integralState += error * deltaTime;
    float Iout = gainKi * feedback_integralState;
```

ersetzen durch:

```cpp
    // Konditionale Integration: grosse POSITIVE Fehler (= Aufheizen, weit unter
    // Sollwert) nicht integrieren. Negative Fehler immer, damit abgebaut werden
    // kann. Asymmetrisch — ein symmetrisches |error| <= band wuerde den I-Anteil
    // genau dann einfrieren, wenn er abgebaut werden muesste.
    const bool integrateThisStep = (integralBand <= 0.0f) || (error <= integralBand);
    if (integrateThisStep) {
        feedback_integralState += error * deltaTime;
    }
    float Iout = clampedIntegralOutput();
```

**(c) Anti-Windup-Block anpassen** — aktuell Zeile 74–82. Das bestehende
Zurücknehmen darf nur greifen, wenn in diesem Schritt überhaupt integriert wurde,
und muss danach neu klammern:

```cpp
    if (isSaturated && isSameSign) {
        // Nur zuruecknehmen, was in diesem Schritt tatsaechlich addiert wurde.
        if (integrateThisStep) {
            feedback_integralState -= error * deltaTime;
        }
        Iout = clampedIntegralOutput();
        sumPID = Pout + Iout + Dout + FFOut + DistFFOut;
        sumPIDsat = constrain(sumPID, ctrlOutputLimits[0], ctrlOutputLimits[1]);
    }
```

Das ursprüngliche `feedback_integralState -= error * deltaTime;` und
`Iout = gainKi * feedback_integralState;` entfallen dabei.

### 1.3 `lib/GaggiMateController/src/peripherals/Heater.h`

**Public** — nach `setFeedforwardScale(...)` (aktuell Zeile 44):

```cpp
    // Anti-Overshoot fuer gedaemmte Boiler. Jeder Parameter 0 = aus.
    // integralBandC    : Fehlerband (K), in dem integriert werden darf
    // integralMaxDuty  : Obergrenze des I-Beitrags, 0..1 (0 = unbegrenzt)
    void setIntegralShaping(float integralBandC, float integralMaxDuty);
```

**Private** — bei den Feedforward-Membern (nach Zeile 89):

```cpp
    float integralBandC = 0.0f;
    float integralMaxDuty = 0.0f;
```

### 1.4 `lib/GaggiMateController/src/peripherals/Heater.cpp`

Neue Methode, z. B. nach `setFeedforwardScale()` (Zeile 90–93):

```cpp
void Heater::setIntegralShaping(float integralBandC, float integralMaxDuty) {
    this->integralBandC = integralBandC;
    this->integralMaxDuty = integralMaxDuty;
    simplePid->setIntegralBand(integralBandC);
    // 0 = unbegrenzt; sonst Anteil von TUNER_OUTPUT_SPAN. Untergrenze bleibt
    // offen: ein negativer I-Anteil ist das legitime Mittel, eine zu hohe
    // Temperatur abzubauen.
    simplePid->setIntegralOutputLimits(-INFINITY, integralMaxDuty > 0.0f
                                                     ? integralMaxDuty * TUNER_OUTPUT_SPAN
                                                     : 0.0f);
    ESP_LOGI(LOG_TAG, "Integral shaping: band=%.2f K, max duty=%.0f %%", integralBandC,
             integralMaxDuty * 100.0f);
}
```

> `<cmath>` ist in `Heater.cpp:5` schon eingebunden.

### 1.5 Protokoll

**`lib/NanoPbComm/proto/gaggimate.proto`** — `HeaterSettings` (ab Zeile 109)
erweitern. Felder 1–9 **unverändert** lassen:

```proto
message HeaterSettings {
    float kp = 1;
    float ki = 2;
    float kd = 3;
    float kf = 4;                         // combined thermal feedforward gain
    bool heater_coordination_enabled = 5; // dual boiler: power heaters one at a time instead of in parallel, brew first
    uint32 handover_ms = 6;               // dual boiler: dead time between heaters when coordinated (clamped 20..5000)
    float steam_kp = 7;
    float steam_ki = 8;
    float steam_kd = 9;

    // Anti-overshoot for insulated boilers. 0 = feature off (upstream behaviour).
    float integral_band_c = 10;   // integrate only while error <= this (degC)
    float integral_max_duty = 11; // cap on the integral contribution, 0..1
    float approach_band_c = 12;   // taper the duty cap within this band below setpoint (degC)
    float approach_max_duty = 13; // duty cap reached at setpoint, 0..1

    // Thermal model for the disturbance feedforward. <= 0 = use controller default.
    float incoming_water_temp_c = 14; // default 23
    float heat_loss_watts = 15;       // default 5
    float heater_efficiency = 16;     // 0..1, default 0.95
    float setpoint_ramp_c_per_min = 17; // 0 = off (phase 4)
}
```

Alle Felder werden schon in Phase 1 angelegt, damit `PROTOCOL_VERSION` **einmal**
steigt und Phase 2–4 ohne weiteren Protokollbruch nachziehen können.

**`lib/NanoPbComm/src/Protocol.h:28`** — Version erhöhen und Kommentar ergänzen:

```cpp
// Protocol 17 adds anti-overshoot shaping and a configurable thermal model to HeaterSettings.
static constexpr uint32_t PROTOCOL_VERSION = 17;
```

> **Speicherhinweis:** Der `Payload`-Union ist auf sein größtes Mitglied
> dimensioniert und `Frame.payloads max_count:6`
> (`lib/NanoPbComm/proto/gaggimate.options`) alloziert sechs davon statisch.
> `HeaterSettings` wächst um 8 × 4 = 32 Byte. Nach dem Build prüfen, ob
> `HeaterSettings` dadurch zum größten Mitglied wird — falls ja, kostet das
> 6 × 32 = 192 Byte RAM. Unkritisch, aber bewusst entscheiden.

### 1.6 Signaturen auf Struct umstellen

`sendHeaterSettings` hat heute 9 positionale Parameter. Mit 8 weiteren wären es
17 — unlesbar und fehleranfällig. Stattdessen den schon vorhandenen
nanopb-Typ `gm::HeaterSettings` (`lib/NanoPbComm/src/Messages.h:19`) direkt
verwenden.

**`lib/NanoPbComm/src/GaggiMateClient.h`** (Zeile 80 und 108) — die beiden
Deklarationen ersetzen:

```cpp
    gm::Payload buildHeaterSettings(const gm::HeaterSettings &settings);
    void sendHeaterSettings(const gm::HeaterSettings &settings);
```

**`lib/NanoPbComm/src/GaggiMateClient.cpp`** (Zeile 152 und 254):

```cpp
gm::Payload GaggiMateClient::buildHeaterSettings(const gm::HeaterSettings &settings) {
    gm::Payload p = gaggimate_Payload_init_zero;
    p.which_content = gaggimate_Payload_heater_tag;
    p.content.heater = settings;
    return p;
}

void GaggiMateClient::sendHeaterSettings(const gm::HeaterSettings &settings) {
    _endpoint.send(buildHeaterSettings(settings));
}
```

**`sim/comms/GaggiMateClient.h`** (Zeile 99 und 116) und
**`sim/comms/GaggiMateClient.cpp`** (Zeile 63 und 107) — gleiche Signatur, Stubs:

```cpp
gm::Payload GaggiMateClient::buildHeaterSettings(const gm::HeaterSettings &) { return {}; }
void GaggiMateClient::sendHeaterSettings(const gm::HeaterSettings &) {}
```

> Den Simulator mitziehen, sonst bricht `pio run -e display-sim`.

### 1.7 `src/display/core/Controller.cpp` — `setHeaterSettings()`

Aktuell Zeile 974–981. Komplett ersetzen:

```cpp
void Controller::setHeaterSettings() {
    float pid[4];
    parseFloatCsv(settings.getPid(), pid, 4, 0.0f);
    float steamPid[3];
    parseFloatCsv(settings.getSteamPid(), steamPid, 3, 0.0f);

    gm::HeaterSettings heater = gaggimate_HeaterSettings_init_zero;
    heater.kp = pid[0];
    heater.ki = pid[1];
    heater.kd = pid[2];
    heater.kf = pid[3];
    heater.heater_coordination_enabled = settings.isHeaterCoordinationEnabled();
    heater.handover_ms = static_cast<uint32_t>(settings.getHeaterHandoverMs());
    heater.steam_kp = steamPid[0];
    heater.steam_ki = steamPid[1];
    heater.steam_kd = steamPid[2];

    // Anti-Overshoot und Waermemodell (0 = aus / Controller-Default)
    heater.integral_band_c = settings.getIntegralBandC();
    heater.integral_max_duty = settings.getIntegralMaxDuty();
    heater.approach_band_c = settings.getApproachBandC();
    heater.approach_max_duty = settings.getApproachMaxDuty();
    heater.incoming_water_temp_c = settings.getIncomingWaterTempC();
    heater.heat_loss_watts = settings.getHeatLossWatts();
    heater.heater_efficiency = settings.getHeaterEfficiency();
    heater.setpoint_ramp_c_per_min = settings.getSetpointRampCPerMin();

    comms.sendHeaterSettings(heater);
}
```

Die Getter kommen aus [04-WEBUI-SETTINGS.md](04-WEBUI-SETTINGS.md) §1 — **diese
Datei zuerst abarbeiten**, sonst kompiliert es nicht.

### 1.8 `lib/GaggiMateController/src/GaggiMateController.cpp` — `onHeaterSettings`

Aktuell Zeile 338–347. Ergänzen:

```cpp
    _comms.onHeaterSettings([this](const gm::HeaterSettings &settings) {
        if (heaterCoordinator != nullptr) {
            heaterCoordinator->configure(settings.heater_coordination_enabled, settings.handover_ms);
        }
        heater->setFeedforwardScale(settings.kf);
        heater->setTunings(settings.kp, settings.ki, settings.kd);
        heater->setIntegralShaping(settings.integral_band_c, settings.integral_max_duty);
        if (heater2 != nullptr) {
            heater2->setTunings(settings.steam_kp, settings.steam_ki, settings.steam_kd);
            // Dual-Boiler: derselbe Daemmungs-Kontext gilt fuer beide Kessel.
            heater2->setIntegralShaping(settings.integral_band_c, settings.integral_max_duty);
        }
    });
```

In Phase 2–4 kommen hier `setApproachLimit(...)`, `setThermalModel(...)` und
`setSetpointRamp(...)` dazu.

### Phase 1 — Prüfliste

- [ ] `pio run -e controller` baut
- [ ] `pio run -e display-headless-n16r8` baut (Custom-Build-Target)
- [ ] `pio run -e display-sim` baut (Stub-Signaturen gezogen)
- [ ] `pio test -e native -f test_autotune_simc` läuft weiter
- [ ] Neuer Test `test_integral_shaping` grün — siehe [06](06-TEST-VALIDIERUNG.md)
- [ ] `PROTOCOL_VERSION` beidseitig 17; **beide** Firmwares geflasht
- [ ] Mit `integralBandC = 0` ist das Verhalten bitidentisch zu Upstream

---

## PHASE 2 — Anfahr-Leistungsdeckel

Baustein 3. Nur `Heater`; Protokollfelder existieren schon aus Phase 1.

### 2.1 `lib/GaggiMateController/src/peripherals/Heater.h`

**Public:**

```cpp
    // Leistungsdeckel in der Anfahrphase. approachBandC 0 = aus.
    void setApproachLimit(float approachBandC, float approachMaxDuty);
```

**Private:**

```cpp
    float approachBandC = 0.0f;
    float approachMaxDuty = 1.0f;
    bool isDrawing() const;        // Pumpe foerdert und Ventil offen
    float computeOutputCap() const;
```

### 2.2 `lib/GaggiMateController/src/peripherals/Heater.cpp`

**(a) Setter:**

```cpp
void Heater::setApproachLimit(float approachBandC, float approachMaxDuty) {
    this->approachBandC = approachBandC;
    this->approachMaxDuty = approachMaxDuty > 0.0f ? std::min(approachMaxDuty, 1.0f) : 1.0f;
    ESP_LOGI(LOG_TAG, "Approach limit: band=%.1f K, max duty=%.0f %%", approachBandC,
             this->approachMaxDuty * 100.0f);
}
```

**(b) Hilfsfunktionen:**

```cpp
// Wird gerade Wasser gezogen? Identische Bedingung wie der Stoergroessen-
// Feedforward in loopPid, damit Deckel und Feedforward nie gegeneinander
// arbeiten.
bool Heater::isDrawing() const {
    return pumpFlowRate != nullptr && *pumpFlowRate > 0.01f && valveStatus != nullptr && *valveStatus != 0;
}

// Stellgroessen-Obergrenze: 100 % ausserhalb des Anfahrbands, linear
// herunterlaufend auf approachMaxDuty am Sollwert und darueber.
float Heater::computeOutputCap() const {
    if (approachBandC <= 0.0f || approachMaxDuty >= 1.0f) {
        return TUNER_OUTPUT_SPAN;
    }
    // WICHTIG: Beim Bezug darf nicht gedeckelt werden. Die Temperatur liegt dann
    // typisch 4 K unter Sollwert, also INNERHALB des Bands, der Feedforward
    // braucht aber ~57 % Stellgrad. Ohne diesen Bypass macht Phase 2 das
    // Absacken schlimmer statt besser (siehe 02-REGELUNGSKONZEPT.md, Baustein 3).
    if (isDrawing()) {
        return TUNER_OUTPUT_SPAN;
    }
    const float distance = setpoint - temperature;
    if (distance >= approachBandC) {
        return TUNER_OUTPUT_SPAN;
    }
    const float ratio = distance > 0.0f ? distance / approachBandC : 0.0f;
    const float duty = approachMaxDuty + (1.0f - approachMaxDuty) * ratio;
    return duty * TUNER_OUTPUT_SPAN;
}
```

**(c) In `loopPid()` einhängen** — unmittelbar **vor** `simplePid->update()`
(aktuell Zeile 129), also nach dem Feedforward-Block:

```cpp
    // Obergrenze VOR update() setzen, damit das vorhandene Anti-Windup in
    // SimplePID::update() die echte Saettigungsgrenze sieht und die Integration
    // korrekt zuruecknimmt. Ein nachtraegliches Begrenzen von `output` wuerde
    // genau dieses Windup erzeugen.
    simplePid->setCtrlOutputLimits(0.0f, computeOutputCap());

    bool pidUpdated = simplePid->update();
```

> Die Reihenfolge ist der ganze Punkt dieses Bausteins. `setCtrlOutputLimits()`
> ist ein reines Feldsetzen (`SimplePID.cpp:153`) und darf jeden Zyklus gerufen
> werden.

### 2.3 Durchleitung

`GaggiMateController.cpp`, in `onHeaterSettings` ergänzen:

```cpp
        heater->setApproachLimit(settings.approach_band_c, settings.approach_max_duty);
        // heater2 analog, falls dualBoiler
```

### Phase 2 — Prüfliste

- [ ] Mit `approachBandC = 0` unverändertes Verhalten
- [ ] **Bezugstest:** Duty erreicht beim Bezug > 50 %, obwohl die Temperatur im
      Anfahrband liegt (prüft den `isDrawing()`-Bypass — der wichtigste Testfall
      der ganzen Umsetzung)
- [ ] Aufheizzeit um ≤ 45 s verlängert
- [ ] Overshoot < 0,5 K

---

## PHASE 3 — Wärmemodell und Heizleistung

Baustein 4. Settings-Teil in [04-WEBUI-SETTINGS.md](04-WEBUI-SETTINGS.md).

### 3.1 `lib/GaggiMateController/src/peripherals/Heater.h`

`heaterEfficiency`, `heatLossWatts` und `incomingWaterTemp` sind heute
hart verdrahtete private Member (Zeile 86–88). Sie bleiben, bekommen aber einen
Setter:

```cpp
    // Waermemodell fuer calculateDisturbanceFeedforwardGain(). Jeder Wert <= 0
    // behaelt den bisherigen Default (23 C / 5 W / 0,95).
    void setThermalModel(float incomingWaterTempC, float heatLossWatts, float efficiency);
```

### 3.2 `lib/GaggiMateController/src/peripherals/Heater.cpp`

```cpp
void Heater::setThermalModel(float incomingWaterTempC, float heatLossW, float efficiency) {
    if (incomingWaterTempC > 0.0f)
        incomingWaterTemp = incomingWaterTempC;
    if (heatLossW > 0.0f)
        heatLossWatts = heatLossW;
    if (efficiency > 0.0f && efficiency <= 1.0f)
        heaterEfficiency = efficiency;
    ESP_LOGI(LOG_TAG, "Thermal model: inlet=%.1f C, loss=%.1f W, eff=%.2f", incomingWaterTemp,
             heatLossWatts, heaterEfficiency);
}
```

> `setThermalFeedforward()` (Zeile 79) setzt `incomingWaterTemp` ebenfalls und
> wird beim Setup mit dem Literal `23.0f` gerufen
> (`GaggiMateController.cpp:246`). **Reihenfolge beachten:** `onHeaterSettings`
> läuft nach dem Setup, überschreibt also korrekt. Wird das Setup-Literal
> angefasst, dann auf `0.0f` setzen, damit „Default behalten“ gilt.

### 3.3 Durchleitung

`GaggiMateController.cpp`, in `onHeaterSettings`:

```cpp
        heater->setThermalModel(settings.incoming_water_temp_c, settings.heat_loss_watts,
                                settings.heater_efficiency);
```

### 3.4 Heizleistung persistieren

Das ist der eigentliche Nutzen dieser Phase — Befund B2. Rein display-seitig:

* `heaterWattage` als Setting (`04-WEBUI-SETTINGS.md` §1)
* `CalibrationTab.jsx:20` liest die Wattzahl aus den Settings statt aus
  `useState(1360)` (`04-WEBUI-SETTINGS.md` §5)
* Aktion „Kff aus Heizleistung berechnen“ → `kf = 1000 / heaterWattage`

### Phase 3 — Prüfliste

- [ ] Wattzahl übersteht Browser-Reload und Neustart
- [ ] „Kff berechnen“ ergibt bei 1100 W exakt `0,909`
- [ ] `incomingWaterTempC = 15` erhöht den Feedforward messbar (~11 %)
- [ ] Absacken beim Espressobezug < 1,5 K

---

## PHASE 4 — Setpoint-Rampe (optional, Default aus)

Baustein 5. **Nur anfangen, wenn Phase 1–3 gemessen und abgenommen sind.**

### 4.1 `Heater.h` — Trennung von kommandiertem und Arbeitssollwert

```cpp
    // Rampenrate in K/min fuer den Arbeits-Sollwert. 0 = aus.
    void setSetpointRamp(float cPerMinute);
```

Bestehendes `float setpoint = 0.0f;` (Zeile 70) **bleibt** der Arbeitswert, auf
den `SimplePID` zeigt (`Heater.cpp:12`). Neu daneben:

```cpp
    float commandedSetpoint = 0.0f;      // vom Display kommandiertes Ziel
    float setpointRampCPerMin = 0.0f;
    unsigned long lastRampMs = 0;
    void advanceSetpointRamp();
```

**`getSetpoint()` (Zeile 32) muss den kommandierten Wert liefern:**

```cpp
    float getSetpoint() { return commandedSetpoint; };
```

> Zwingend: `GaggiMateController.cpp:503` vergleicht
> `heater2->getSetpoint() != target`, um redundante Aufrufe zu vermeiden. Gäbe
> `getSetpoint()` den Rampenwert zurück, würde der Vergleich nie konvergieren.

### 4.2 `Heater.cpp`

**(a) `setSetpoint()` (Zeile 58–63)** umbauen:

```cpp
void Heater::setSetpoint(float setpoint) {
    if (this->commandedSetpoint != setpoint) {
        this->commandedSetpoint = setpoint;
        ESP_LOGV(LOG_TAG, "Set setpoint %f C", setpoint);
    }
    // Abschalten und Rampe-aus wirken sofort. Ebenso ein Kaltstart aus 0:
    // von 0 hochzurampen wuerde die Aufheizzeit sinnlos verlaengern.
    if (setpoint <= 0.0f || setpointRampCPerMin <= 0.0f || this->setpoint <= 0.0f) {
        this->setpoint = setpoint;
    }
}
```

**(b) Abschalt-Gate in `loop()` (Zeile 47)** auf den kommandierten Wert umstellen:

```cpp
    if (sensor->isErrorState() || commandedSetpoint <= 0.0f) {
```

> Ohne diese Änderung heizt die Maschine beim Abschalten weiter, solange der
> Rampenwert noch über 0 liegt.

**(c) Rampe:**

```cpp
void Heater::advanceSetpointRamp() {
    const unsigned long now = millis();
    const unsigned long elapsed = now - lastRampMs;   // wrap-sicher (unsigned)
    lastRampMs = now;
    if (setpointRampCPerMin <= 0.0f || commandedSetpoint <= 0.0f) {
        setpoint = commandedSetpoint;
        return;
    }
    // Beim Bezug nicht rampen — das wuerde die Erholung bremsen.
    if (isDrawing() || elapsed == 0 || elapsed > 5000) {
        setpoint = commandedSetpoint;
        return;
    }
    const float step = setpointRampCPerMin / 60.0f * (elapsed / 1000.0f);
    if (setpoint < commandedSetpoint) {
        setpoint = std::min(setpoint + step, commandedSetpoint);
    } else {
        setpoint = std::max(setpoint - step, commandedSetpoint);
    }
}
```

`elapsed > 5000` fängt den ersten Durchlauf und Pausen ab (z. B. nach Autotune)
und springt dann direkt auf das Ziel, statt eine Riesenstufe zu rampen.

**(d) In `loopPid()`** als erstes nach `temperature = sensor->read();` aufrufen:

```cpp
    advanceSetpointRamp();
```

**(e) `setSetpoint(0.0f)` in `stop()` (Zeile 75)** wirkt durch (a) sofort auf
beide Variablen — nichts zu ändern, aber beim Review prüfen.

### 4.3 Durchleitung

```cpp
        heater->setSetpointRamp(settings.setpoint_ramp_c_per_min);
```

### Phase 4 — Prüfliste

- [ ] `setpointRampCPerMin = 0` → bitidentisch zu Phase 3
- [ ] Abschalten über die UI schaltet die Heizung **sofort** ab
- [ ] Standby → Brüh rampt sichtbar, Kaltstart springt sofort auf Ziel
- [ ] Dampf → Brüh (145 → 94) rampt abwärts ohne Nachheizen
- [ ] Bezug während der Rampe setzt die Rampe aus

---

## Berührte Dateien (Gesamtübersicht)

| Datei | P1 | P2 | P3 | P4 |
|---|:-:|:-:|:-:|:-:|
| `lib/NayrodPID/src/SimplePID/SimplePID.h` | ● | | | |
| `lib/NayrodPID/src/SimplePID/SimplePID.cpp` | ● | | | |
| `lib/GaggiMateController/src/peripherals/Heater.h` | ● | ● | ● | ● |
| `lib/GaggiMateController/src/peripherals/Heater.cpp` | ● | ● | ● | ● |
| `lib/GaggiMateController/src/GaggiMateController.cpp` | ● | ● | ● | ● |
| `lib/NanoPbComm/proto/gaggimate.proto` | ● | | | |
| `lib/NanoPbComm/src/Protocol.h` | ● | | | |
| `lib/NanoPbComm/src/GaggiMateClient.h/.cpp` | ● | | | |
| `sim/comms/GaggiMateClient.h/.cpp` | ● | | | |
| `src/display/core/Controller.cpp` | ● | | | |
| `src/display/core/Settings.h/.cpp` | ● | ● | ● | ● |
| `src/display/plugins/WebUIPlugin.cpp` | ● | ● | ● | ● |
| `web/src/pages/Settings/index.jsx` | ● | ● | ● | ● |
| `web/src/pages/Settings/tabs/MachineTab.jsx` | ● | ● | ● | ● |
| `web/src/pages/Settings/tabs/CalibrationTab.jsx` | | | ● | |
| `web/src/locales/{de,en}/messages.po` | ● | ● | ● | ● |

Keine Überschneidung mit den laufenden Screensaver-/Rebranding-Änderungen
(`DefaultUI.*`, `GeneralTab.jsx`, `Navigation.jsx`, Logo-Assets).

---

## Build- und Flash-Kommandos

```bash
cd "/home/linux/Dokumente/Headless Gaggimate/gaggimate-src-1.10"

# Formatieren
./scripts/format.sh

# Native Tests
pio test -e native -f test_autotune_simc
pio test -e native -f test_integral_shaping     # neu, siehe 06

# Builds
pio run -e controller
pio run -e display-headless-n16r8               # Custom-Build-Target
pio run -e display-sim                          # prueft die Sim-Stubs mit

# Flashen — ab Phase 1 BEIDE, sonst Protokoll-Mismatch
pio run -e controller -t upload
pio run -e display-headless-n16r8 -t upload
```

> Welches Display-Target der Custom-Build nutzt, steht in `platformio.ini`
> (`[env:display-headless-n16r8]`, Zeile 113). Das Board-JSON
> `boards/GM-N16R8-DIO.json` existiert im **alten** Baum `gaggimate-src` und
> muss ggf. nach `gaggimate-src-1.10/boards/` kopiert werden.

---

## Fertige Prompts für Claude Code in VS Code

**Phase 1**

```
Lies /home/linux/RANCIMATE/02-REGELUNGSKONZEPT.md und
/home/linux/RANCIMATE/03-IMPLEMENTIERUNG.md sowie
/home/linux/RANCIMATE/04-WEBUI-SETTINGS.md.

Setze PHASE 1 um. Arbeitsverzeichnis:
/home/linux/Dokumente/Headless Gaggimate/gaggimate-src-1.10

Reihenfolge: zuerst 04-WEBUI-SETTINGS.md Abschnitt 1 (Settings-Properties,
nur die fuer Phase 1 benoetigten), dann 03-IMPLEMENTIERUNG.md Abschnitte
1.1 bis 1.8.

Halte dich exakt an die angegebenen Einfuegepunkte. Aendere nichts darueber
hinaus, insbesondere nicht die vorhandenen uncommitteten Screensaver- und
Rebranding-Aenderungen. Lege vorher einen Branch an (der Worktree ist detached).
Fuehre am Ende ./scripts/format.sh aus und baue -e controller und
-e display-headless-n16r8.
```

**Phase 2**

```
Setze PHASE 2 aus /home/linux/RANCIMATE/03-IMPLEMENTIERUNG.md um
(Anfahr-Leistungsdeckel), inklusive der zugehoerigen Settings und
Web-UI-Felder aus 04-WEBUI-SETTINGS.md.

Besonders wichtig: der isDrawing()-Bypass in computeOutputCap(). Ohne ihn
verschlechtert diese Phase das Temperaturabsacken beim Bezug. Begruendung in
02-REGELUNGSKONZEPT.md, Baustein 3.
```

**Phase 3**

```
Setze PHASE 3 aus /home/linux/RANCIMATE/03-IMPLEMENTIERUNG.md um
(Waermemodell + persistente Heizleistung), inklusive 04-WEBUI-SETTINGS.md
Abschnitt 5 (CalibrationTab auf die persistente Wattzahl umstellen) und
Abschnitt 6 (Daemmungs-Preset).
```

→ Settings und Web UI: [04-WEBUI-SETTINGS.md](04-WEBUI-SETTINGS.md).

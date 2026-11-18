# 04 — Settings und Web UI

Arbeitsverzeichnis: `/home/linux/Dokumente/Headless Gaggimate/gaggimate-src-1.10`

Jede neue Einstellung durchläuft in GaggiMate **fünf Stationen**. Das Muster ist
immer gleich — die laufende Screensaver-Arbeit im Worktree (`deepStandbyTimeout`)
ist ein vollständiges, lokales Beispiel: `git diff src/display/core/Settings.h
src/display/plugins/WebUIPlugin.cpp web/src/pages/Settings/tabs/GeneralTab.jsx`.

```
1. Settings.h          Property<T> + Getter + validierender Setter
2. WebUIPlugin.cpp     POST-Handler  (request->hasArg)
3. WebUIPlugin.cpp     GET-Serializer (doc[...])
4. Settings/index.jsx  Feldliste (nur bei Checkboxen / Form-only)
5. MachineTab.jsx      Eingabefeld
+  locales/*/messages.po
```

---

## 1. `src/display/core/Settings.h`

### 1.1 Properties

Alle neuen Properties zusammen, z. B. direkt **nach** der Pumpen-Sektion
(aktuell Zeile 641–647, `// Pump settings`). Neuer Block:

```cpp
    // Thermische Regelung fuer gedaemmte Boiler (siehe /home/linux/RANCIMATE).
    // Alle Shaping-Parameter: 0 = Funktion aus, Verhalten wie Upstream.
    Property<int> heaterWattage{registry, "tc_watt", DEFAULT_HEATER_WATTAGE};
    Property<float> integralBandC{registry, "tc_ib", 0.0f};
    Property<float> integralMaxDuty{registry, "tc_imd", 0.0f};
    Property<float> approachBandC{registry, "tc_apb", 0.0f};
    Property<float> approachMaxDuty{registry, "tc_apd", 1.0f};
    Property<float> setpointRampCPerMin{registry, "tc_ramp", 0.0f};
    Property<float> incomingWaterTempC{registry, "tc_iwt", 0.0f};
    Property<float> heatLossWatts{registry, "tc_hl", 0.0f};
    Property<float> heaterEfficiency{registry, "tc_eff", 0.0f};
    Property<int> thermalPreset{registry, "tc_pre", THERMAL_PRESET_STOCK};
```

**Zu den NVS-Keys:** ESP32-NVS erlaubt maximal 15 Zeichen. Das Präfix `tc_`
(thermal control) ist im gesamten bestehenden Keyspace frei — geprüft gegen alle
Properties in `Settings.h`. Keys **nie nachträglich ändern**, sonst verliert jede
Maschine ihre Einstellung stillschweigend (Property fällt auf den Default zurück).

**Defaults bewusst auf 0:** Nach dem Flashen verhält sich die Maschine exakt wie
Upstream, bis der Nutzer (oder das Preset) etwas setzt. Das macht jede Phase
einzeln messbar und ein Rollback trivial.

### 1.2 Konstanten

In `src/display/core/constants.h`, bei den übrigen Defaults (nach Zeile 25):

```cpp
constexpr int DEFAULT_HEATER_WATTAGE = 1100; // Rancilio Silvia 230 V
constexpr int MIN_HEATER_WATTAGE = 300;
constexpr int MAX_HEATER_WATTAGE = 3000;

// Daemmungs-Presets fuer die thermische Regelung
constexpr int THERMAL_PRESET_STOCK = 0;     // ungedaemmt, Shaping aus
constexpr int THERMAL_PRESET_INSULATED = 1; // gedaemmter Boiler
constexpr int THERMAL_PRESET_CUSTOM = 2;    // manuell
```

> Der Autotune-Bereich in der Web UI ist auf 300–1500 W begrenzt
> (`CalibrationTab.jsx:207-208`). `MAX_HEATER_WATTAGE = 3000` ist firmwareseitig
> großzügiger; die UI-Grenze bleibt davon unberührt.

### 1.3 Getter und Setter

Validierende Setter **inline im Header** — das ist das Muster der
NTC-Einstellungen (`Settings.h:155-186`) und spart Änderungen an `Settings.cpp`.
Einfügen bei den übrigen Gettern, z. B. nach `getMaxPumpPower()` (Zeile 380):

```cpp
    int getHeaterWattage() const { return heaterWattage.get(); }
    float getIntegralBandC() const { return integralBandC.get(); }
    float getIntegralMaxDuty() const { return integralMaxDuty.get(); }
    float getApproachBandC() const { return approachBandC.get(); }
    float getApproachMaxDuty() const { return approachMaxDuty.get(); }
    float getSetpointRampCPerMin() const { return setpointRampCPerMin.get(); }
    float getIncomingWaterTempC() const { return incomingWaterTempC.get(); }
    float getHeatLossWatts() const { return heatLossWatts.get(); }
    float getHeaterEfficiency() const { return heaterEfficiency.get(); }
    int getThermalPreset() const { return thermalPreset.get(); }

    void setHeaterWattage(int value) {
        if (value >= MIN_HEATER_WATTAGE && value <= MAX_HEATER_WATTAGE)
            heaterWattage.set(value);
    }
    // 0 = aus. Nicht unter ~1 K sinnvoll: zu klein laesst eine stationaere
    // Abweichung stehen (siehe 02-REGELUNGSKONZEPT.md, Baustein 1).
    void setIntegralBandC(float value) {
        if (std::isfinite(value) && value >= 0.0f && value <= 20.0f)
            integralBandC.set(value);
    }
    void setIntegralMaxDuty(float value) {
        if (std::isfinite(value) && value >= 0.0f && value <= 1.0f)
            integralMaxDuty.set(value);
    }
    void setApproachBandC(float value) {
        if (std::isfinite(value) && value >= 0.0f && value <= 40.0f)
            approachBandC.set(value);
    }
    // Untergrenze 0,05: ein Deckel von 0 wuerde die Heizung am Sollwert
    // vollstaendig abschalten und die Temperatur dauerhaft darunter halten.
    void setApproachMaxDuty(float value) {
        if (std::isfinite(value) && value >= 0.05f && value <= 1.0f)
            approachMaxDuty.set(value);
    }
    void setSetpointRampCPerMin(float value) {
        if (std::isfinite(value) && value >= 0.0f && value <= 600.0f)
            setpointRampCPerMin.set(value);
    }
    // 0 = Controller-Default (23 C) behalten.
    void setIncomingWaterTempC(float value) {
        if (std::isfinite(value) && value >= 0.0f && value <= 40.0f)
            incomingWaterTempC.set(value);
    }
    void setHeatLossWatts(float value) {
        if (std::isfinite(value) && value >= 0.0f && value <= 200.0f)
            heatLossWatts.set(value);
    }
    void setHeaterEfficiency(float value) {
        if (std::isfinite(value) && value >= 0.0f && value <= 1.0f)
            heaterEfficiency.set(value);
    }
    void setThermalPreset(int value) {
        if (value >= THERMAL_PRESET_STOCK && value <= THERMAL_PRESET_CUSTOM)
            thermalPreset.set(value);
    }
```

`<cmath>` (für `std::isfinite`) ist in `Settings.h:11` bereits eingebunden.

---

## 2. `src/display/plugins/WebUIPlugin.cpp` — POST

In `handleSettings()`, bei den übrigen Heizungsparametern — direkt **nach** dem
`pid`-Handler (aktuell Zeile 531–532):

```cpp
            if (request->hasArg("heaterWattage"))
                settings->setHeaterWattage(request->arg("heaterWattage").toInt());
            if (request->hasArg("integralBandC"))
                settings->setIntegralBandC(request->arg("integralBandC").toFloat());
            if (request->hasArg("integralMaxDuty"))
                settings->setIntegralMaxDuty(request->arg("integralMaxDuty").toFloat());
            if (request->hasArg("approachBandC"))
                settings->setApproachBandC(request->arg("approachBandC").toFloat());
            if (request->hasArg("approachMaxDuty"))
                settings->setApproachMaxDuty(request->arg("approachMaxDuty").toFloat());
            if (request->hasArg("setpointRampCPerMin"))
                settings->setSetpointRampCPerMin(request->arg("setpointRampCPerMin").toFloat());
            if (request->hasArg("incomingWaterTempC"))
                settings->setIncomingWaterTempC(request->arg("incomingWaterTempC").toFloat());
            if (request->hasArg("heatLossWatts"))
                settings->setHeatLossWatts(request->arg("heatLossWatts").toFloat());
            if (request->hasArg("heaterEfficiency"))
                settings->setHeaterEfficiency(request->arg("heaterEfficiency").toFloat());
            if (request->hasArg("thermalPreset"))
                settings->setThermalPreset(request->arg("thermalPreset").toInt());
```

### Durchreichen zum Controller: schon erledigt

`controller->setHeaterSettings()` wird im POST-Pfad bereits gerufen
(`WebUIPlugin.cpp:743`, direkt nach `pluginManager->trigger("settings:changed")`)
— zusammen mit `setPressureScale()`, `setMachineHardware()`, `setScaleFactors()`
und `setPumpModelCoeffs()`. **Hier ist also nichts zu ergänzen**; die neuen
Felder wandern automatisch mit, sobald `setHeaterSettings()` sie füllt
([03-IMPLEMENTIERUNG.md](03-IMPLEMENTIERUNG.md) §1.7).

Die Duty-Werte werden als **Anteil 0…1** übertragen, nicht in Prozent. Die
Prozent-Darstellung passiert ausschließlich im Formular (siehe §4) — damit gibt
es genau eine Umrechnungsstelle.

---

## 3. `src/display/plugins/WebUIPlugin.cpp` — GET

Im Settings-Serializer, nach `doc["pid"]` (aktuell Zeile 790):

```cpp
    doc["heaterWattage"] = settings.getHeaterWattage();
    doc["integralBandC"] = settings.getIntegralBandC();
    doc["integralMaxDuty"] = settings.getIntegralMaxDuty();
    doc["approachBandC"] = settings.getApproachBandC();
    doc["approachMaxDuty"] = settings.getApproachMaxDuty();
    doc["setpointRampCPerMin"] = settings.getSetpointRampCPerMin();
    doc["incomingWaterTempC"] = settings.getIncomingWaterTempC();
    doc["heatLossWatts"] = settings.getHeatLossWatts();
    doc["heaterEfficiency"] = settings.getHeaterEfficiency();
    doc["thermalPreset"] = settings.getThermalPreset();
```

---

## 4. `web/src/pages/Settings/tabs/MachineTab.jsx`

Die neuen Felder gehören in die bestehende Sektion **„Heating Parameters“**
(`<details>` ab Zeile 208), nach dem `temperatureOffset`-Grid (endet Zeile 297)
und **innerhalb** des `<fieldset>`, das auf Zeile 212 beginnt.

> **Konsequenz von `disabled={!!formData.machineProfileSelection}` (Zeile 212):**
> Die Felder sind read-only, solange ein Maschinenprofil gewählt ist. Für diese
> Maschine ist das korrekt — die Dämmung ist individuell und gehört nicht in ein
> geteiltes Profil. In der UI wird das durch den Hinweistext unten sichtbar
> gemacht.

```jsx
              {/* Anti-Overshoot fuer gedaemmte Boiler */}
              <div className='border-base-content/10 border-t pt-4'>
                <p className='text-base-content/70 mb-3 text-sm'>
                  <Trans>
                    For a heavily insulated boiler the steady-state holding power is only
                    a few percent. These limits stop the integral term from charging up
                    during warm-up, which is what causes a slow, long-lasting overshoot.
                    Set a value to 0 to disable that limit.
                  </Trans>
                </p>
                <div className='grid grid-cols-1 gap-4 md:grid-cols-2'>
                  <InputGroupField
                    label={t`Integral Band`}
                    htmlFor='integralBandC'
                    unit='K'
                    helpText={t`Integrate only within this distance below the setpoint. Larger errors (warm-up) are skipped; errors above the setpoint always integrate so the term can unwind. 0 = off. Recommended: 3.`}
                    noMargin
                  >
                    <input
                      id='integralBandC'
                      name='integralBandC'
                      type='number'
                      step='0.1'
                      min='0'
                      max='20'
                      className='grow'
                      placeholder='3.0'
                      value={formData.integralBandC}
                      onChange={onChange('integralBandC')}
                    />
                  </InputGroupField>
                  <InputGroupField
                    label={t`Integral Authority`}
                    htmlFor='integralMaxDuty'
                    unit='%'
                    helpText={t`Upper limit on the integral term's share of heater power. An insulated boiler needs roughly 2% to hold temperature. 0 = unlimited. Recommended: 12.`}
                    noMargin
                  >
                    <input
                      id='integralMaxDuty'
                      name='integralMaxDuty'
                      type='number'
                      step='1'
                      min='0'
                      max='100'
                      className='grow'
                      placeholder='12'
                      value={formData.integralMaxDuty}
                      onChange={onChange('integralMaxDuty')}
                    />
                  </InputGroupField>
                  <InputGroupField
                    label={t`Approach Band`}
                    htmlFor='approachBandC'
                    unit='K'
                    helpText={t`Start tapering heater power this far below the setpoint. 0 = off. Recommended: 8.`}
                    noMargin
                  >
                    <input
                      id='approachBandC'
                      name='approachBandC'
                      type='number'
                      step='0.5'
                      min='0'
                      max='40'
                      className='grow'
                      placeholder='8.0'
                      value={formData.approachBandC}
                      onChange={onChange('approachBandC')}
                    />
                  </InputGroupField>
                  <InputGroupField
                    label={t`Approach Power Limit`}
                    htmlFor='approachMaxDuty'
                    unit='%'
                    helpText={t`Heater power allowed at the setpoint. Lower values reduce overshoot but lengthen warm-up — 35% adds roughly 30 s. Suspended automatically while water is being drawn.`}
                    noMargin
                  >
                    <input
                      id='approachMaxDuty'
                      name='approachMaxDuty'
                      type='number'
                      step='1'
                      min='5'
                      max='100'
                      className='grow'
                      placeholder='35'
                      value={formData.approachMaxDuty}
                      onChange={onChange('approachMaxDuty')}
                    />
                  </InputGroupField>
                </div>
              </div>

              {/* Waermemodell fuer den Stoergroessen-Feedforward */}
              <div className='border-base-content/10 border-t pt-4'>
                <p className='text-base-content/70 mb-3 text-sm'>
                  <Trans>
                    Feeds the flow-based feedforward that counteracts the temperature drop
                    during a shot. With a long sensor dead time, feedback alone reacts too
                    late — these values let the controller add the power the incoming water
                    will need before the drop shows up.
                  </Trans>
                </p>
                <div className='grid grid-cols-1 gap-4 md:grid-cols-2'>
                  <InputGroupField
                    label={t`Heater Wattage`}
                    htmlFor='heaterWattage'
                    unit='W'
                    helpText={t`Rancilio Silvia 230 V: 1100 W · Silvia 120 V: 950 W · Gaggia Classic: 1360 W. Also used as the Autotune default and to derive Kff.`}
                    noMargin
                  >
                    <input
                      id='heaterWattage'
                      name='heaterWattage'
                      type='number'
                      step='10'
                      min='300'
                      max='3000'
                      className='grow'
                      placeholder='1100'
                      value={formData.heaterWattage}
                      onChange={onChange('heaterWattage')}
                    />
                  </InputGroupField>
                  <InputGroupField
                    label={t`Inlet Water Temperature`}
                    htmlFor='incomingWaterTempC'
                    unit='°C'
                    unitAriaLabel={t`celsius`}
                    helpText={t`Temperature of the water entering the boiler. 0 = controller default (23 °C). Tap water is typically 8–15 °C; too high a value underestimates the power needed during a shot.`}
                    noMargin
                  >
                    <input
                      id='incomingWaterTempC'
                      name='incomingWaterTempC'
                      type='number'
                      step='0.5'
                      min='0'
                      max='40'
                      className='grow'
                      placeholder='15'
                      value={formData.incomingWaterTempC}
                      onChange={onChange('incomingWaterTempC')}
                    />
                  </InputGroupField>
                  <InputGroupField
                    label={t`Boiler Heat Loss`}
                    htmlFor='heatLossWatts'
                    unit='W'
                    helpText={t`Standing heat loss at brew temperature. 0 = controller default (5 W). Insulated boiler roughly 15 W, bare boiler 60–90 W.`}
                    noMargin
                  >
                    <input
                      id='heatLossWatts'
                      name='heatLossWatts'
                      type='number'
                      step='1'
                      min='0'
                      max='200'
                      className='grow'
                      placeholder='15'
                      value={formData.heatLossWatts}
                      onChange={onChange('heatLossWatts')}
                    />
                  </InputGroupField>
                  <InputGroupField
                    label={t`Setpoint Ramp`}
                    htmlFor='setpointRampCPerMin'
                    unit={t`K/min`}
                    helpText={t`Rate limit applied to setpoint changes. 0 = off (recommended). A cold start always jumps straight to the target; the ramp only shapes changes from an already-warm boiler.`}
                    noMargin
                  >
                    <input
                      id='setpointRampCPerMin'
                      name='setpointRampCPerMin'
                      type='number'
                      step='5'
                      min='0'
                      max='600'
                      className='grow'
                      placeholder='0'
                      value={formData.setpointRampCPerMin}
                      onChange={onChange('setpointRampCPerMin')}
                    />
                  </InputGroupField>
                </div>
                {!!formData.machineProfileSelection && (
                  <p className='text-warning mt-3 text-sm'>
                    <Trans>
                      These fields are managed by the selected machine profile. Clear the
                      machine profile selection to edit them.
                    </Trans>
                  </p>
                )}
              </div>
```

`heaterEfficiency` wird **nicht** ins Formular aufgenommen. 0,95 ist für einen
Tauchheizkörper eine gute Annahme und ein weiterer Knopf ohne praktischen Nutzen;
die Property existiert für den Fall, dass sie gebraucht wird.

### Prozent ↔ Anteil

`integralMaxDuty` und `approachMaxDuty` werden im Formular in **Prozent** (0–100)
eingegeben, in NVS und über das Protokoll aber als **Anteil** (0–1) geführt.
Die Umrechnung gehört in `web/src/pages/Settings/index.jsx`, analog zu
`splitPidString` — in `transformFetchedSettings()` (× 100) und im
Submit-Pfad (÷ 100), dort wo `pid`/`kf` zusammengesetzt werden (Zeile 217–218).

Alternative, falls das zu fehleranfällig erscheint: Felder direkt als Anteil
(0…1, `step='0.01'`) anzeigen und `unit` auf `0–1` setzen. Dann entfällt die
Umrechnung komplett. **Für die erste Umsetzung ist diese Variante die
sicherere** — sie hat eine Fehlerquelle weniger und kann später kosmetisch
nachgezogen werden.

---

## 5. `web/src/pages/Settings/tabs/CalibrationTab.jsx` — Phase 3

Heute lebt die Heizleistung nur im Browser: `useState(1360)` (Zeile 20). Sie wird
ausschließlich als Autotune-Argument gesendet (Zeile 27) und ist nach einem
Reload verloren — der Grund, warum `kf` in der Praxis fast immer 0 bleibt
(Befund B2/B3 in [01-ANALYSE.md](01-ANALYSE.md)).

**Umstellen auf:**

1. Initialwert aus den Settings statt `1360`:
   ```jsx
   const [autotuneWattage, setAutotuneWattage] = useState(
     machine.value?.settings?.heaterWattage ?? 1100
   );
   ```
   Den tatsächlichen Zugriffspfad auf die geladenen Settings in dieser Komponente
   prüfen — im `MachineTab` kommt er über `formData`. Falls hier nicht verfügbar,
   per `apiService` laden oder als Prop durchreichen.

2. Hinweis ergänzen, dass die Wattzahl in Machine → Heating Parameters dauerhaft
   gepflegt wird und hier nur für den Testlauf gilt.

3. Nach erfolgreichem Autotune zeigt das Ergebnis (Zeile 94, `{autotuneResult}`)
   den PID-String inklusive Kff. Dort ergänzen, dass Kff = 1000 / Wattzahl ist —
   bei 1100 W also 0,909.

**Zusätzlich: Aktion „Kff aus Heizleistung berechnen“** in der
Heating-Parameters-Sektion des `MachineTab`:

```jsx
<button
  type='button'
  className='btn btn-sm'
  onClick={() => onChange('kf')({
    target: { value: (1000 / (Number(formData.heaterWattage) || 1100)).toFixed(3) },
  })}
>
  <Trans>Derive Kff from heater wattage</Trans>
</button>
```

Damit ist der Feedforward unabhängig davon erreichbar, ob je ein Autotune lief.

> `kf` steht in `FORM_ONLY_KEYS` (`Settings/index.jsx:90`) und wird beim
> Speichern mit `pid` zu `"Kp,Ki,Kd,Kff"` zusammengesetzt (Zeile 217–218). Der
> Button muss also das Formularfeld `kf` setzen, **nicht** direkt POSTen.

---

## 6. Dämmungs-Preset

Ein Auswahlfeld, das die sechs Werte in einem Schritt setzt — damit niemand
sechs Zahlen aus der Dokumentation abtippen muss.

```jsx
const THERMAL_PRESETS = {
  // Upstream-Verhalten: jedes Shaping aus
  0: { integralBandC: 0, integralMaxDuty: 0, approachBandC: 0,
       approachMaxDuty: 1, setpointRampCPerMin: 0, heatLossWatts: 0 },
  // Gedaemmter Boiler (Steinwolle + Aluklebeband)
  1: { integralBandC: 3.0, integralMaxDuty: 0.12, approachBandC: 8.0,
       approachMaxDuty: 0.35, setpointRampCPerMin: 0, heatLossWatts: 15 },
};
```

Bedienlogik:

* Auswahl `Stock` / `Insulated` schreibt den Satz in `formData`.
* Jede manuelle Änderung eines dieser Felder setzt `thermalPreset` auf
  `THERMAL_PRESET_CUSTOM` (2), damit das Feld nicht behauptet, ein Preset sei
  aktiv, während die Werte abweichen.
* `thermalPreset` ist reine Anzeige-/Komfortinformation. **Die Firmware liest
  ausschließlich die Einzelwerte** — es gibt keine Preset-Auflösung im C++-Code.
  Das hält den Controller frei von UI-Zustand.

---

## 7. i18n

Neue Strings in `web/src/locales/*/messages.po`. Die Locales sind im Worktree
bereits durch die Rebranding-Arbeit verändert — beim Ergänzen also `git diff`
beachten und nicht überschreiben.

Mindestens `de` und `en` pflegen; die übrigen Sprachen fallen auf die msgid
(= englischer Text) zurück.

| msgid | de |
|---|---|
| `Integral Band` | `Integral-Band` |
| `Integral Authority` | `Integral-Autorität` |
| `Approach Band` | `Anfahr-Band` |
| `Approach Power Limit` | `Anfahr-Leistungsgrenze` |
| `Heater Wattage` | `Heizleistung` |
| `Inlet Water Temperature` | `Zulauf-Wassertemperatur` |
| `Boiler Heat Loss` | `Boiler-Wärmeverlust` |
| `Setpoint Ramp` | `Sollwert-Rampe` |
| `Derive Kff from heater wattage` | `Kff aus Heizleistung berechnen` |
| `Insulated boiler` | `Gedämmter Boiler` |
| `Stock (no insulation)` | `Serie (ungedämmt)` |

Der Lingui-Workflow steht in `web/package.json` (`extract` / `compile`). Nach dem
Ergänzen Web UI neu bauen: `./scripts/build_webui.sh`.

---

## 8. Prüfliste Web UI

- [ ] `GET /api/settings` liefert alle zehn neuen Schlüssel
- [ ] `POST` mit Werten außerhalb des Bereichs wird verworfen (Setter-Guard), die
      übrigen Felder bleiben unberührt
- [ ] Reload des Browsers verliert nichts
- [ ] Neustart des Displays verliert nichts (NVS)
- [ ] Preset `Insulated` setzt alle sechs Werte
- [ ] Manuelle Änderung schaltet die Anzeige auf `Custom`
- [ ] „Kff berechnen“ ergibt bei 1100 W `0.909`
- [ ] Gewähltes Maschinenprofil macht die Felder read-only **und** zeigt den Hinweis
- [ ] Settings-Export/-Import (`Settings/index.jsx:410`) nimmt die neuen Felder mit

---

## 9. Wo Änderungen wirksam werden

```
Browser  ──POST /api/settings──►  WebUIPlugin::handleSettings
                                       │  settings->setXxx(...)
                                       │  controller->setHeaterSettings()   ← nicht vergessen
                                       ▼
                                  Controller::setHeaterSettings()
                                       │  gm::HeaterSettings fuellen
                                       ▼
                                  comms.sendHeaterSettings(heater)
                                       │  nanopb, PRIO_NORMAL
                                       ▼
                       GaggiMateController::onHeaterSettings
                                       │  heater->setIntegralShaping(...)
                                       │  heater->setApproachLimit(...)
                                       │  heater->setThermalModel(...)
                                       ▼
                                  Heater / SimplePID
```

Zusätzlich wird `setHeaterSettings()` beim Verbindungsaufbau und nach einem
Controller-Neustart gerufen (`Controller.cpp:453`, `:641`, `:1424`) — die Werte
überleben also einen Reconnect, ohne dass dafür etwas zu tun ist.

→ Parameterwerte für diese Maschine: [05-PARAMETER-REFERENZ.md](05-PARAMETER-REFERENZ.md).

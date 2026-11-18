# 06 — Test und Validierung

Arbeitsverzeichnis: `/home/linux/Dokumente/Headless Gaggimate/gaggimate-src-1.10`

---

## 1. Duty-Logging nachrüsten — zuerst erledigen

Die bisherigen Messungen (`heatup_warm*.csv`) loggen nur `ct/tt/pr/fl`. Der
**Stellgrad fehlt** — und genau der ist die Messgröße, an der sich alle Aussagen
dieses Konzepts überprüfen lassen (I-Anteil-Aufladung, Anfahrdeckel,
Feedforward-Höhe).

**Gute Nachricht: der Stellgrad wird bereits übertragen.** In `evt:status` steckt
das Feld **`hp`** (Heizleistung in %, `WebSocketHandler.cpp:498`
→ `Controller::getCurrentHeaterPower()` → `SensorData.boilers[0].power`
→ `Heater::getDutyCycle()`). **Keine Firmwareänderung nötig** — nur der Logger
muss die Spalte mitschreiben.

In `/home/linux/Dokumente/Headless Gaggimate/heatup_logger.py` sind es zwei
Zeilen — die Kopfzeile (Zeile 45) und die Datenzeile (Zeile 74):

```python
# Zeile 45
writer.writerow(["t_sec", "ct", "tt", "pr", "fl", "hp"])

# Zeile 74 — hp vorher wie ct/tt/pr/fl aus der Nachricht lesen
writer.writerow([t, ct, tt, pr, fl, hp])
```

Das Feld heißt in `evt:status` schlicht `hp` und ist der Stellgrad in Prozent
(0…100), gerundet auf drei Dezimalstellen.

Damit lässt sich direkt nachsehen, ob der I-Anteil wirklich auf ~40 % auflädt —
die zentrale, bislang nur *erschlossene* Annahme aus
[01-ANALYSE.md](01-ANALYSE.md) §4.

> **Diese Messung vor Phase 1 machen.** Bestätigt sie die Annahme nicht, ist die
> Diagnose falsch und Phase 1 der falsche Hebel. Bestätigt sie sie, ist der Rest
> des Konzepts belegt statt plausibel.

---

## 2. Native Unit-Tests

### 2.1 Vorhandene Tests dürfen nicht brechen

```bash
pio test -e native -f test_autotune_simc
pio test -e native -f test_heater_coordinator
pio test -e native -f test_puckflow_latch
pio test -e native -f test_button_handler
```

### 2.2 Neuer Test `test/test_integral_shaping/test_integral_shaping.cpp`

Testet die **echte** `SimplePID`-Implementierung gegen ein Streckenmodell, das
aus den Messdaten dieser Maschine parametriert ist.

**Muster:** wie `test_autotune_simc.cpp` — die Übersetzungseinheit direkt
einbinden und die Arduino-Abhängigkeiten vorher selbst definieren. `SimplePID.cpp`
braucht `millis()`, `constrain()`, `PI` und `ESP_LOGV`. Der leere gemeinsame Stub
(`test/test_puckflow_latch/native_stubs/Arduino.h`, über `-I` in
`platformio.ini:161` im Suchpfad) erfüllt den `#include <Arduino.h>`; alles
Weitere definiert der Test. **Er muss nicht angefasst werden** — kein anderes
`-I`-Verzeichnis in `[env:native]` enthält eine `Arduino.h`, die ihn verdecken
könnte.

```cpp
// Unit tests: Anti-Windup-Erweiterung von SimplePID fuer gedaemmte Boiler.
// Host-seitig: pio test -e native -f test_integral_shaping
//
// Streckenmodell aus den Messdaten der Maschine (siehe 01-ANALYSE.md, Abschnitt 3):
//   Totzeit L        = 22 s
//   Verstaerkung k'  = 0,50 K/s bei Vollgas
//   Waermeverlust    = 8,1e-5 1/s  (-> 0,006 K/s bei 94 C, Umgebung 20 C)
//   Haltestellgrad   = ~1,2 %

#include <unity.h>

#include <cmath>
#include <deque>
#include <vector>

// --- Arduino-Ersatz fuer die direkt eingebundene TU ------------------------
#define ESP_LOGV(...) ((void)0)
static unsigned long g_now_ms = 0;
static unsigned long millis() { return g_now_ms; }
#ifndef PI
#define PI 3.14159265358979323846
#endif
template <typename T> static T constrain(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

#include "SimplePID/SimplePID.cpp"

// --- Streckenmodell --------------------------------------------------------
namespace {

constexpr float kSpan = 1000.0f;   // TUNER_OUTPUT_SPAN
constexpr float kDeadTimeS = 22.0f;
constexpr float kGainKps = 0.50f;      // K/s bei 100 % Duty
constexpr float kLossPerK = 8.1e-5f;   // 1/s
constexpr float kAmbientC = 20.0f;

class Boiler {
  public:
    explicit Boiler(float startC) : temperature(startC), delayLine(static_cast<size_t>(kDeadTimeS), 0.0f) {}

    // Ein Schritt von 1 s. duty in Stellgroessen-Einheiten (0..1000).
    void step(float duty) {
        delayLine.push_back(duty / kSpan);
        const float applied = delayLine.front();
        delayLine.pop_front();
        temperature += kGainKps * applied - kLossPerK * (temperature - kAmbientC);
    }

    float temp() const { return temperature; }

  private:
    float temperature;
    std::deque<float> delayLine;
};

struct RunResult {
    float peak = -1000.0f;
    float peakAtS = 0.0f;
    float crossedAtS = -1.0f;
    float finalTemp = 0.0f;
    float maxIntegralDuty = 0.0f;
};

// Geschlossener Kreis, 1 Hz, wie auf dem Geraet (Heater.cpp:23).
RunResult run(SimplePID &pid, float *output, float *temperature, float startC, float target, int durationS) {
    Boiler boiler(startC);
    RunResult r;
    for (int t = 0; t < durationS; ++t) {
        *temperature = boiler.temp();
        g_now_ms = static_cast<unsigned long>(t) * 1000UL + 1UL;
        pid.update();
        boiler.step(*output);

        if (boiler.temp() > r.peak) {
            r.peak = boiler.temp();
            r.peakAtS = static_cast<float>(t);
        }
        if (r.crossedAtS < 0.0f && boiler.temp() >= target) {
            r.crossedAtS = static_cast<float>(t);
        }
    }
    r.finalTemp = boiler.temp();
    return r;
}

SimplePID *makePid(float *output, float *temperature, float *setpoint, float kp, float ki, float kd) {
    auto *pid = new SimplePID(output, temperature, setpoint);
    pid->setSamplingFrequency(1.0f);
    pid->setCtrlOutputLimits(0.0f, kSpan);
    pid->activateSetPointFilter(false);
    pid->activateFeedForward(false);
    pid->setControllerPIDGains(kp, ki, kd, 0.0f);
    pid->reset();
    pid->setMode(SimplePID::Control::automatic);
    return pid;
}

} // namespace

// --- A: Reproduktion des Fehlerbilds --------------------------------------

// Mit den Upstream-Defaults und ohne Shaping muss das Modell den gemessenen
// Overshoot reproduzieren: +2..3 K, Spitze deutlich NACH dem Durchgang.
void test_upstream_defaults_overshoot(void) {
    float out = 0.0f, temp = 54.0f, sp = 94.0f;
    SimplePID *pid = makePid(&out, &temp, &sp, 58.397f, 1.027f, 249.055f);
    RunResult r = run(*pid, &out, &temp, 54.0f, 94.0f, 900);

    TEST_ASSERT_TRUE_MESSAGE(r.peak - 94.0f > 1.5f, "Modell reproduziert den Overshoot nicht");
    TEST_ASSERT_TRUE_MESSAGE(r.peakAtS - r.crossedAtS > 60.0f, "Spitze muss deutlich nach dem Durchgang liegen");
    delete pid;
}

// --- B: Wirkung des Freigabebands ----------------------------------------

void test_integral_band_removes_overshoot(void) {
    float out = 0.0f, temp = 54.0f, sp = 94.0f;
    SimplePID *pid = makePid(&out, &temp, &sp, 58.397f, 1.027f, 249.055f);
    pid->setIntegralBand(3.0f);
    RunResult r = run(*pid, &out, &temp, 54.0f, 94.0f, 900);

    TEST_ASSERT_TRUE_MESSAGE(r.peak - 94.0f < 1.0f, "Freigabeband senkt den Overshoot nicht unter 1 K");
    delete pid;
}

// Negative Fehler muessen IMMER integrieren, sonst bleibt eine stationaere
// Abweichung stehen. Start weit UEBER dem Sollwert.
void test_negative_error_always_integrates(void) {
    float out = 0.0f, temp = 99.0f, sp = 94.0f;
    SimplePID *pid = makePid(&out, &temp, &sp, 58.397f, 1.027f, 249.055f);
    pid->setIntegralBand(3.0f);
    RunResult r = run(*pid, &out, &temp, 99.0f, 94.0f, 3000);

    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.0f, 94.0f, r.finalTemp,
                                     "Kreis laeuft nicht auf den Sollwert ein");
    delete pid;
}

// --- C: Autoritaetsgrenze -------------------------------------------------

void test_integral_authority_is_clamped(void) {
    float out = 0.0f, temp = 54.0f, sp = 94.0f;
    SimplePID *pid = makePid(&out, &temp, &sp, 58.397f, 1.027f, 249.055f);
    pid->setIntegralOutputLimits(-INFINITY, 0.12f * kSpan);
    run(*pid, &out, &temp, 54.0f, 94.0f, 900);
    // Nach der Rueckrechnung darf der Zustand die Grenze nicht ueberschreiten.
    TEST_ASSERT_TRUE_MESSAGE(1.027f * pid->getIntegralStateForTest() <= 0.12f * kSpan + 1.0f,
                             "I-Zustand laeuft ueber die Klammer hinaus");
    delete pid;
}

// --- D: Rueckfall auf Upstream -------------------------------------------

// band = 0 und keine Klammer muessen bitidentisch zu Upstream sein.
void test_disabled_matches_upstream(void) {
    float outA = 0.0f, tA = 54.0f, spA = 94.0f;
    SimplePID *a = makePid(&outA, &tA, &spA, 58.397f, 1.027f, 249.055f);
    RunResult ra = run(*a, &outA, &tA, 54.0f, 94.0f, 600);

    float outB = 0.0f, tB = 54.0f, spB = 94.0f;
    SimplePID *b = makePid(&outB, &tB, &spB, 58.397f, 1.027f, 249.055f);
    b->setIntegralBand(0.0f);
    b->setIntegralOutputLimits(-INFINITY, 0.0f);
    RunResult rb = run(*b, &outB, &tB, 54.0f, 94.0f, 600);

    TEST_ASSERT_EQUAL_FLOAT(ra.peak, rb.peak);
    TEST_ASSERT_EQUAL_FLOAT(ra.finalTemp, rb.finalTemp);
    delete a;
    delete b;
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_upstream_defaults_overshoot);
    RUN_TEST(test_integral_band_removes_overshoot);
    RUN_TEST(test_negative_error_always_integrates);
    RUN_TEST(test_integral_authority_is_clamped);
    RUN_TEST(test_disabled_matches_upstream);
    return UNITY_END();
}
```

> **Test C braucht einen Lesezugriff auf den Integratorzustand.** Entweder in
> `SimplePID.h` einen Getter ergänzen
> (`float getIntegralStateForTest() const { return feedback_integralState; }`)
> oder Test C streichen und die Klammer indirekt über den Stellgradverlauf
> prüfen. Der Getter ist der ehrlichere Weg — dann aber als solcher benennen und
> nicht im Produktionscode verwenden.

**Test A ist der wichtigste.** Er validiert das Modell gegen die Realität:
reproduziert es den gemessenen Overshoot nicht, sind die Tests B–D wertlos, weil
sie gegen eine Fantasiestrecke prüfen. Nach dem Duty-Logging aus §1 lässt sich
`kLossPerK` und `kGainKps` zusätzlich nachjustieren.

### 2.3 Phase 2 testbar machen (empfohlene Verfeinerung)

`Heater::computeOutputCap()` aus [03-IMPLEMENTIERUNG.md](03-IMPLEMENTIERUNG.md)
§2.2(b) ist eine reine Funktion von (Sollwert, Temperatur, Band, Deckel,
`drawing`), steckt aber als Member in einer Klasse voller FreeRTOS- und
Hardwareabhängigkeiten — natürlich nicht testbar.

**Besser:** die Formel in einen header-only Helfer ziehen,
`lib/GaggiMateController/src/peripherals/ApproachLimit.h`:

```cpp
#pragma once

// Stellgroessen-Obergrenze in der Anfahrphase. Rein, header-only, ohne
// Arduino-Abhaengigkeiten — damit native testbar.
//
// 100 % ausserhalb des Bands, linear auf maxDuty am Sollwert und darueber.
// drawing == true hebt den Deckel auf: beim Bezug liegt die Temperatur im Band,
// der Stoergroessen-Feedforward braucht dort aber volle Autoritaet.
inline float approachDutyCap(float setpoint, float temperature, float bandC, float maxDuty, bool drawing) {
    if (bandC <= 0.0f || maxDuty >= 1.0f || drawing) {
        return 1.0f;
    }
    const float distance = setpoint - temperature;
    if (distance >= bandC) {
        return 1.0f;
    }
    const float ratio = distance > 0.0f ? distance / bandC : 0.0f;
    return maxDuty + (1.0f - maxDuty) * ratio;
}
```

`Heater::computeOutputCap()` delegiert dann:

```cpp
float Heater::computeOutputCap() const {
    return approachDutyCap(setpoint, temperature, approachBandC, approachMaxDuty, isDrawing()) * TUNER_OUTPUT_SPAN;
}
```

Damit wird der sicherheitsrelevante `drawing`-Bypass zu einer einzeiligen
Zusicherung:

```cpp
void test_approach_cap_lifted_while_drawing(void) {
    // 4 K unter Sollwert, also mitten im 8-K-Band
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.675f, approachDutyCap(94.0f, 90.0f, 8.0f, 0.35f, false));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.000f, approachDutyCap(94.0f, 90.0f, 8.0f, 0.35f, true));
}

void test_approach_cap_edges(void) {
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.00f, approachDutyCap(94.0f, 86.0f, 8.0f, 0.35f, false)); // Bandrand
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.35f, approachDutyCap(94.0f, 94.0f, 8.0f, 0.35f, false)); // am Sollwert
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.35f, approachDutyCap(94.0f, 97.0f, 8.0f, 0.35f, false)); // darueber
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.00f, approachDutyCap(94.0f, 90.0f, 0.0f, 0.35f, false)); // aus
}
```

`-I lib/GaggiMateController/src` steht schon in `[env:native]`
(`platformio.ini:158`) — der Helfer ist ohne weitere Konfiguration einbindbar.

---

## 3. Validierung am Gerät

### 3.1 Messprotokoll (für jede Phase identisch)

**Aufheiztest**

1. Maschine aus, auf ~55 °C abkühlen lassen (vergleichbar mit `heatup_warm2.csv`).
2. Logger starten: `python3 heatup_logger.py <IP> phaseN_heatup.csv`
3. Maschine einschalten, Zieltemperatur 94 °C.
4. **Mindestens 15 Minuten** laufen lassen — die Abklingphase ist die eigentliche
   Messgröße, nicht die Spitze.

**Bezugstest**

1. Aus dem eingeschwungenen Zustand (≥ 5 min bei 94 °C).
2. Logger starten.
3. Normalen Espressobezug fahren, gleiches Profil wie `shot-21.json`
   (~36 s, Spitzenfluss ~10 ml/s).
4. 3 Minuten nachlaufen lassen — ein Überschwinger *nach* dem Bezug wäre ein
   Zeichen für zu großes `kf`.

Auswertung mit demselben Python-Schnipsel, der die Ausgangsmessung
charakterisiert hat ([01-ANALYSE.md](01-ANALYSE.md) §3/§4).

### 3.2 Abnahmekriterien

Ausgangswerte zum Vergleich: Overshoot **+2,50 K** (Spitze 285 s nach Durchgang,
nach 760 s noch nicht eingeschwungen), Absacken **−4,0 K**.

| Kriterium | Ausgang | nach P0 | nach P1 | nach P2 | nach P3 |
|---|---|---|---|---|---|
| Overshoot | +2,50 K | ≤ 1,5 K | ≤ 1,0 K | **≤ 0,5 K** | ≤ 0,5 K |
| Spitze nach Durchgang | 285 s | ≤ 90 s | ≤ 60 s | ≤ 45 s | ≤ 45 s |
| Innerhalb +0,3 K nach Durchgang | nie (>760 s) | ≤ 5 min | ≤ 3 min | ≤ 2 min | ≤ 2 min |
| Haltestellgrad eingeschwungen (`hp`) | — | 1–4 % | 1–4 % | 1–4 % | 1–4 % |
| Absacken beim Bezug | −4,0 K | ≤ 2,5 K | ≤ 2,5 K | ≤ 2,5 K | **≤ 1,5 K** |
| **Duty beim Bezug (`hp`)** | — | — | — | **≥ 50 %** | ≥ 50 % |
| Überschwinger nach Bezug | — | — | — | — | ≤ 1,0 K |
| Aufheizzeit 55 → 94 °C | ~150 s | ~150 s | ~150 s | ≤ 195 s | ≤ 195 s |

**Die Zeile „Duty beim Bezug" ist das wichtigste Einzelkriterium der Phase 2.**
Liegt `hp` während des Bezugs unter 50 %, greift der Anfahrdeckel fälschlich —
der `isDrawing()`-Bypass fehlt oder wirkt nicht. Dann **Phase 2 zurücknehmen**,
bevor weiter optimiert wird: ohne den Bypass macht sie das Absacken schlimmer.

### 3.3 Weitere Betriebsfälle

Nach Phase 2 zusätzlich prüfen — hier zeigen sich Wechselwirkungen, die der
Aufheiztest nicht erfasst:

| Fall | Worauf achten |
|---|---|
| Aufwachen aus Standby (Boiler noch 70 °C) | Overshoot ≤ 0,5 K; der Deckel greift sofort |
| Brüh → Dampf (94 → 145 °C) | erreicht 145 °C; `hp` wird am Ziel gedeckelt |
| Dampf → Brüh (145 → 94 °C) | kein Nachheizen; negativer I-Anteil baut ab. **Zähe Phase**, bei starker Dämmung ggf. Kühlspülung — inhärent, keine Regressionsursache |
| Langer Wasserbezug / Americano | Temperatur bleibt über ~88 °C; Vergleich mit `americano.csv` (−9,1 K) |
| Backflush-Profil | Kein Dauerheizen bei geschlossenem Ventil |
| Autotune | läuft noch durch; Deckel und I-Band greifen nicht hinein (`loopAutotune` setzt `output` direkt, `Heater.cpp:159-161`) |
| Verbindungsabbruch BLE/UART | Heizung geht aus (`handlePing`-Timeout); nach Reconnect werden die Werte neu gesendet (`Controller.cpp:453`) |

---

## 4. Rollback

**Kein Reflash nötig.** Alle Shaping-Parameter sind so entworfen, dass `0` das
Upstream-Verhalten herstellt:

| Problem | Maßnahme |
|---|---|
| Overshoot schlimmer als vorher | `integralBandC = 0`, `integralMaxDuty = 0` |
| Absacken schlimmer als vorher | `approachBandC = 0` — und den `isDrawing()`-Bypass im Code prüfen |
| Temperatur erreicht den Sollwert nicht | `approachMaxDuty` erhöhen (0,5 … 0,7) oder `approachBandC` verkleinern |
| Stationär leicht unter Sollwert | `integralBandC` erhöhen (3 → 5 K) oder `integralMaxDuty` erhöhen |
| Überschwinger nach dem Bezug | `kf` zurück auf 1000/Wattzahl; nicht höher |
| Alles verdächtig | Preset `Stock` → sämtliches Shaping aus, nur die neuen PID-Gains bleiben |

Vollständiger Rückbau auf Upstream-Verhalten: Preset `Stock` **und**
`pid` auf `58.397,1.027,249.055,0.0` zurücksetzen.

---

## 5. Prüfliste vor dem Abschluss

**Build**

- [ ] `./scripts/format.sh` ohne Diff danach
- [ ] `pio run -e controller`
- [ ] `pio run -e display-headless-n16r8`
- [ ] `pio run -e display-sim`
- [ ] `pio test -e native` (alle Suiten)

**Protokoll**

- [ ] `PROTOCOL_VERSION == 17` in beiden Firmwares
- [ ] Beide geflasht; Display zeigt keinen „Version mismatch"
- [ ] Bewusst einseitig flashen und prüfen, dass die Maschine sauber in den
      Mismatch-Zustand geht und per OTA erreichbar bleibt

**Regression**

- [ ] Preset `Stock` → Verhalten wie vor den Änderungen
- [ ] Autotune läuft durch und schreibt plausible Gains
- [ ] Screensaver und RANCIMATE-Branding unverändert funktionsfähig
- [ ] Shot-History / Visualizer unverändert
- [ ] Dampf- und Wasserbezug unverändert bedienbar

**Dokumentation**

- [ ] Gemessene Werte in die Tabelle in §3.2 eintragen
- [ ] Endgültige Parameter in [05-PARAMETER-REFERENZ.md](05-PARAMETER-REFERENZ.md)
      nachtragen, falls von den Empfehlungen abgewichen wurde
- [ ] Messdateien (`phaseN_heatup.csv`, `phaseN_shot.csv`) aufbewahren — sie sind
      die Referenz für die nächste Firmware-Generation

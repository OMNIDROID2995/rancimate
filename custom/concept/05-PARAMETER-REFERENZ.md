# 05 — Parameter-Referenz

Nachschlagewerk. Herleitung in [01-ANALYSE.md](01-ANALYSE.md) und
[02-REGELUNGSKONZEPT.md](02-REGELUNGSKONZEPT.md).

---

## Identifizierte Streckenparameter

Aus den Messdaten dieser Maschine (`heatup_warm2.csv`, 760 s, gegengeprüft mit
`heatup_warm.csv`):

| Größe | Symbol | Wert | Bedeutung |
|---|---|---|---|
| Totzeit | L | **≈ 22 s** | Heizung ein → erste Sensorreaktion |
| Streckenverstärkung | k′ | **≈ 0,50 K/s** (30 K/min) | Anstieg bei Vollgas |
| Parasitäre Verzögerung | τ₂ | ≈ 4,4 s (Fallback 0,2·L) | nicht separat identifiziert |
| Netto-Abkühlrate bei 96 °C | — | −0,004 … −0,008 K/s | gedämmt |
| Stationärer Haltestellgrad | — | **≈ 1,5 … 3 %** | abgeleitet, Obergrenze |
| Heizleistung | — | 1100 W | Silvia 230 V |

Zum Vergleich ungedämmt: Haltestellgrad ~6–8 %.

---

## Neue Einstellungen

Duty-Werte firmware- und protokollseitig als **Anteil 0…1**.

| Einstellung | NVS | Typ | Bereich | Default | **Empfehlung** | Proto |
|---|---|---|---|---|---|---|
| `heaterWattage` | `tc_watt` | int | 300…3000 W | 1100 | **1100** | — |
| `integralBandC` | `tc_ib` | float | 0…20 K | 0 (aus) | **3,0** | 10 |
| `integralMaxDuty` | `tc_imd` | float | 0…1 | 0 (unbegr.) | **0,12** | 11 |
| `approachBandC` | `tc_apb` | float | 0…40 K | 0 (aus) | **8,0** | 12 |
| `approachMaxDuty` | `tc_apd` | float | 0,05…1 | 1,0 | **0,35** | 13 |
| `incomingWaterTempC` | `tc_iwt` | float | 0…40 °C | 0 (→23) | **15,0** | 14 |
| `heatLossWatts` | `tc_hl` | float | 0…200 W | 0 (→5) | **15,0** | 15 |
| `heaterEfficiency` | `tc_eff` | float | 0…1 | 0 (→0,95) | 0 (Default) | 16 |
| `setpointRampCPerMin` | `tc_ramp` | float | 0…600 | 0 (aus) | **0 (aus)** | 17 |
| `thermalPreset` | `tc_pre` | int | 0…2 | 0 | 1 (Insulated) | — |

`0` heißt bei jedem Shaping-Parameter **Funktion aus** und bei den
Modellparametern **Controller-Default behalten**. Nach dem Flashen verhält sich
die Maschine also exakt wie Upstream, bis etwas gesetzt wird.

---

## PID-Gains

| Gain | Upstream `DEFAULT_PID` | SIMC für **diese** Strecke | Empfehlung |
|---|---|---|---|
| Kp | 58,397 | 31 … 36 | Autotune, Fallback **35,0** |
| **Ki** | **1,027** | **0,14 … 0,17** | Autotune, Fallback **0,16** |
| Kd | 249,055 | 140 … 160 | Autotune, Fallback **150,0** |
| Kff | 0,0 | 1000/1100 = 0,909 | **0,909** |

Als String in der Web UI: Feld `PID Values` = `35.0,0.16,150.0`, Feld
`Thermal Feedforward Gain` = `0.909`. In NVS liegt beides als ein String
`"35.0,0.16,150.0,0.909"` (`Settings.h:539`, Key `pid`).

**Ki ist der kritische Wert.** Der Upstream-Default ist für diese Maschine
6–7× zu groß und erzeugt den beobachteten Overshoot. Siehe
[01-ANALYSE.md](01-ANALYSE.md) §4.

---

## Herleitung der Empfehlungen

### `integralBandC = 3,0 K`

Bei 0,5 K/s Anstieg sind 3 K rund 6 s im Band:

```
∫e dt ≈ 3 · 6 / 2 = 9 K·s   →   Iout = Ki · 9
```

Mit Ki = 0,16 praktisch null, mit dem alten Ki = 1,027 rund 9 Einheiten (0,9 %)
statt ~400. Der Baustein wirkt also auch, wenn Phase 0 nicht gelaufen ist.

**Nicht kleiner als ~2 K.** Zu klein lässt eine stationäre Abweichung stehen,
die nur der P-Anteil trägt. **Nicht größer als ~5 K**, sonst beginnt der Windup
wieder.

### `integralMaxDuty = 0,12`

Haltebedarf ~2 % → 12 % sind **6× Reserve** für kalte Umgebung,
Frischwassernachfüllung und Alterung. Deutlich unter den ~40 %, die heute
auftreten.

Bei einer **ungedämmten** Maschine (6–8 % Haltebedarf) müsste der Wert auf
0,25…0,30 steigen — darum ist er einstellbar und nicht fest.

### `approachBandC = 8,0 K` und `approachMaxDuty = 0,35`

Anstieg im Band: 0,35 · 0,5 = **0,175 K/s** → die letzten 8 K dauern ~46 s statt
~16 s. **Aufheizzeit +~30 s** — der bewusst eingegangene Kompromiss.

Zu aggressiv (z. B. 0,15) verlängert die Aufheizzeit auf ~107 s für die letzten
8 K und riskiert, dass der P-Anteil am Sollwert nicht mehr durchkommt.

### `incomingWaterTempC = 15,0 °C`

Realistisches Leitungswasser. Der Controller-Default 23 °C unterschätzt den
Leistungsbedarf beim Bezug um

```
(94 − 15) / (94 − 23) − 1 ≈ 11 %
```

Im Winter ggf. auf 10 °C, im Sommer 18 °C. Der sensibelste Modellparameter.

### `heatLossWatts = 15,0 W`

Schätzung für den gedämmten Boiler (Default 5 W ist für „well-insulated“
dokumentiert, aber optimistisch; blank wären 60–90 W). Geht nur über
`heatLossWatts / currentFlowRate` ein (`Heater.cpp:267`) und ist damit ein
kleiner Korrekturterm. **Konservativ halten (≤ 25 W), nicht als Tuningknopf
verwenden** — bei kleinem Durchfluss wächst der Term stark an.

### `setpointRampCPerMin = 0`

Default aus. Nach Baustein 1–3 ist der Zusatznutzen klein. Wer ihn testen will:
**40 K/min** ist ein sinnvoller Startwert (0,67 K/s, also knapp über dem
Vollgas-Anstieg von 0,5 K/s — die Rampe greift damit nur beim Abwärtswechsel
Dampf → Brüh wirklich ein).

### `kf = 0,909`

```
combinedKff = TUNER_OUTPUT_SPAN / Heizleistung = 1000 / 1100 = 0,909
```

Daraus im Feedforward (Sollwert 94 °C, Zulauf 15 °C, Verlust 15 W, η 0,95):

```
powerPerFlowRate = (1,0 · 4,18 · 79 + 15/flow) / 0,95
bei 2 ml/s       ≈ (330,2 + 7,5) / 0,95 ≈ 355 W pro (ml/s)
gain             = 355 · 0,909 ≈ 323 Einheiten pro (ml/s)
bei 2 ml/s       → 646 Einheiten = 65 % Duty
```

Physikalischer Bedarf: 2 · 4,18 · 79 = 661 W = **60 % von 1100 W**. Modell und
Physik stimmen auf wenige Prozent überein.

**`kf` nicht freihändig über 1000/W erhöhen.** Der Wert ist physikalisch
abgeleitet, kein Tuningparameter. Zu groß erzeugt einen Überschwinger *nach* dem
Bezug — den der gedämmte Boiler wieder nur langsam abbaut.
`calculateSafetyScaling()` (`Heater.cpp:276`) dämpft das oberhalb des Sollwerts,
aber erst nachdem die Energie drin ist.

---

## Autotune-Parameter

| Feld | Default | Empfehlung | Begründung |
|---|---|---|---|
| Test Duration | 120 s | **150 s** | L ≈ 22 s + Anstieg bis zur Inflexion; 120 s ist knapp |
| Samples / Window | 6 | 6 | unverändert |
| Heater Wattage | 1360 W | **1100 W** | Silvia 230 V; Default ist Gaggia Classic |

Voraussetzung: Boilertemperatur **unter 50 °C**. Der Autotune bricht über
125 °C ab (`MAX_AUTOTUNE_TEMP`, `Heater.h:12`).

Bei Timeout bleiben die bisherigen Gains erhalten (`Heater.cpp:188-200`) — es
wird nie mit Nullen überschrieben.

---

## Preset-Sätze

| Parameter | Stock (0) | **Insulated (1)** |
|---|---|---|
| `integralBandC` | 0 | 3,0 |
| `integralMaxDuty` | 0 | 0,12 |
| `approachBandC` | 0 | 8,0 |
| `approachMaxDuty` | 1,0 | 0,35 |
| `setpointRampCPerMin` | 0 | 0 |
| `heatLossWatts` | 0 (→5) | 15,0 |

`heaterWattage` und `incomingWaterTempC` sind **nicht** Teil der Presets — sie
beschreiben die Maschine bzw. die Umgebung, nicht die Dämmung.

---

## Protokoll

| | |
|---|---|
| Version vorher | 16 (`Protocol.h:28`) |
| Version nachher | **17** |
| Neue Felder | `HeaterSettings` 10–17 |
| Prüfung | exakte Gleichheit, `Controller.cpp:428` |
| Bei Mismatch | `updateControl()` gesperrt (`Controller.cpp:1076`) — **Maschine heizt nicht**, OTA bleibt erreichbar |

**Beide Firmwares zusammen flashen.**

---

## Grenzwerte in der bestehenden Firmware

Zur Orientierung, unverändert:

| Konstante | Wert | Ort |
|---|---|---|
| `MIN_TEMP` / `MAX_TEMP` | 0 / 160 °C | `constants.h:19-20` |
| `MAX_AUTOTUNE_TEMP` | 125 °C | `Heater.h:12` |
| `TUNER_OUTPUT_SPAN` | 1000 | `Heater.h:13` |
| PID-Abtastung | 1 Hz | `Heater.cpp:23` |
| Soft-PWM-Fenster | 1000 ms | `Heater.cpp:101` |
| SIMC-Clamp Kp (vor ×1000) | 0,02 … 0,30 | `Autotune.cpp:188` |
| SIMC-Clamp Kd (vor ×1000) | 0,05 … 0,80 | `Autotune.cpp:189` |
| τc-Faktor | 1,5 · L (fest) | `Autotune.cpp:177` |
| FF-Aktivierungsschwelle | flow > 0,01 ml/s **und** Ventil offen | `Heater.cpp:106` |
| FF-Glättung | α = 0,85 | `Heater.cpp:115` |
| Kein Übertemperaturschutz im Normalbetrieb | nur Sensorfehler | `GaggiMateController.cpp:580` |

→ Verifikation: [06-TEST-VALIDIERUNG.md](06-TEST-VALIDIERUNG.md).

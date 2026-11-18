# 02 — Regelungskonzept

Soll-Design für den gedämmten Boiler. Begründungen und Messwerte in
[01-ANALYSE.md](01-ANALYSE.md).

---

## Leitgedanke

Der gedämmte Boiler ist regelungstechnisch eine **nahezu reine Integratorstrecke
mit großer Totzeit**: L ≈ 22 s, k′ ≈ 0,5 K/s, stationärer Haltestellgrad ~2 %.
Daraus folgen zwei Konsequenzen, die das ganze Konzept tragen:

1. **Zugeführte Energie verschwindet nicht wieder.** Jede Überversorgung in der
   Anfahrphase landet als Überschwinger und muss *ausgesessen* werden. Also:
   **Energiezufuhr in der Nähe des Sollwerts strikt begrenzen** — und zwar an der
   Quelle (I-Anteil, Stellgrößendeckel), nicht durch nachträgliche Korrektur.

2. **Bei L ≈ 0,6 × Bezugsdauer ist die Rückführung beim Bezug blind.** Also:
   **Störungen vorsteuern**, nicht ausregeln.

Diese beiden Sätze sind komplementär. Baustein 1–3 begrenzen, Baustein 4 steuert vor.

---

## Die fünf Bausteine

| # | Baustein | Wirkt gegen | Ort | Phase |
|---|---|---|---|---|
| 1 | Freigabeband für die Integration | Overshoot (Ursache) | `SimplePID` | 1 |
| 2 | Autoritätsgrenze für den I-Anteil | Overshoot (Absicherung) | `SimplePID` | 1 |
| 3 | Anfahr-Leistungsdeckel | Overshoot-Restwelle, Abklingzeit | `Heater` | 2 |
| 4 | Wärmemodell + Heizleistung konfigurierbar | Absacken beim Bezug | `Heater` + Settings | 3 |
| 5 | Setpoint-Rampe | Kosmetik, Default aus | `Heater` | 4 |

---

### Baustein 1 — Freigabeband für die Integration (`integralBand`)

**Problem:** Der I-Anteil lädt sich über die gesamte lineare Anfahrphase
(~19 K Fehlerweite) auf ~40 % Stellgrad auf, obwohl stationär ~2 % gebraucht werden.

**Lösung:** Integration nur freigeben, wenn der Fehler klein ist — oder negativ.

```cpp
const bool integrate = (integralBand <= 0.0f) || (error <= integralBand);
```

**Die Asymmetrie ist der Kern dieses Bausteins.** Gefiltert wird nur der *große
positive* Fehler (= weit unter Sollwert = Aufheizen). Negative Fehler werden
**immer** integriert, damit

* ein geladener I-Anteil jederzeit abgebaut werden kann, und
* keine dauerhafte stationäre Abweichung entsteht, wenn die Maschine über dem
  Sollwert hängt.

Ein symmetrisches `|error| <= band` hätte den Fehler, den I-Anteil bei großer
negativer Abweichung einzufrieren — genau dann, wenn er abgebaut werden müsste.

**Wirkung:** Bei `integralBand = 3 K` beginnt die Integration erst 3 K unter dem
Sollwert. Bei 0,5 K/s Anstieg sind das ~6 s Anfahrzeit im Band:

```
∫e dt ≈ 3 K · 6 s / 2 ≈ 9 K·s   →   Iout = 1,027 · 9 ≈ 9 Einheiten ≈ 0,9 % Duty
```

statt ~400 Einheiten. Danach wächst der I-Anteil normal auf den tatsächlich
benötigten Haltewert. **Das beseitigt die Ursache des Overshoots.**

**Trade-off:** Hängt die Maschine dauerhaft mehr als `integralBand` unter dem
Sollwert (z. B. Dauerdampf, Heizung zu klein), integriert sie nicht auf und es
bleibt eine stationäre Abweichung, die allein der P-Anteil trägt. Deshalb nicht
zu klein wählen — **3 K ist die Empfehlung**, nicht 1 K. Bei `0` ist der Baustein
aus (Upstream-Verhalten).

---

### Baustein 2 — Autoritätsgrenze für den I-Anteil (`integralMaxDuty`)

**Problem:** Auch mit Freigabeband ist der I-Anteil unbegrenzt. Bei falschem Ki,
nach einem Sensoraussetzer oder bei einem langen Dampfbezug kann er Werte
annehmen, die der gedämmte Boiler nicht mehr abbauen kann.

**Lösung:** harte Klammer auf `Iout`, **mit Rückrechnung des Integratorzustands**:

```cpp
float SimplePID::clampedIntegralOutput() {
    float Iout = gainKi * feedback_integralState;
    if (integralOutputLimits[1] > 0.0f && gainKi != 0.0f) {
        const float c = constrain(Iout, integralOutputLimits[0], integralOutputLimits[1]);
        if (c != Iout) {
            feedback_integralState = c / gainKi;   // echtes Clamping
            Iout = c;
        }
    }
    return Iout;
}
```

Die Rückrechnung ist nicht optional. Würde man nur den *Ausgang* begrenzen, liefe
`feedback_integralState` unsichtbar weiter und müsste beim Abbau erst den
unsichtbaren Überschuss abtragen — der Overshoot wäre nur verschleiert. Die
Rückrechnung hält Zustand und Wirkung konsistent.

**Wirkung:** Deckelt den I-Beitrag bei `integralMaxDuty = 0,12` auf 12 % Duty.
Bei ~2 % Haltebedarf ist das **6× Reserve** — genug für kaltes Umgebungsklima,
Frischwassernachfüllung und Alterung, aber weit unter den 40 %, die heute auftreten.

**Untergrenze:** `integralOutputLimits[0]` bleibt bei `-INFINITY`. Ein negativer
I-Anteil ist das legitime Mittel, um eine zu hohe Temperatur abzubauen — den zu
begrenzen wäre kontraproduktiv.

---

### Baustein 3 — Anfahr-Leistungsdeckel (`approachBand`, `approachMaxDuty`)

**Problem:** Bausteine 1–2 begrenzen den I-Anteil. Bei Kp = 58 liefert der
P-Anteil aber allein bei 10 K Fehler schon 584 Einheiten = 58 % Duty. Das
Heizelement wird dadurch sehr heiß; diese gespeicherte Energie fließt nach dem
Abschalten weiter in das Wasser und erzeugt die Restwelle.

**Lösung:** Stellgrößen-Obergrenze dynamisch absenken, sobald die Temperatur in
ein Band unter dem Sollwert eintritt — linear von 100 % am Bandrand auf
`approachMaxDuty` am Sollwert:

```
d = setpoint − temperature
cap = 1,0                                                    für d ≥ approachBand
cap = approachMaxDuty + (1 − approachMaxDuty) · d/approachBand   für 0 < d < approachBand
cap = approachMaxDuty                                        für d ≤ 0
```

**Umsetzung über die vorhandene API — das ist der Trick dieses Bausteins:**

```cpp
simplePid->setCtrlOutputLimits(0.0f, cap * TUNER_OUTPUT_SPAN);   // VOR update()
```

`setCtrlOutputLimits()` existiert bereits (`SimplePID.cpp:153`) und wird bisher
nur einmal beim Setup gerufen. Wird die Obergrenze *vor* `update()` gesetzt,
**sieht das bestehende Anti-Windup die echte Sättigungsgrenze** und nimmt die
Integration korrekt zurück (`SimplePID.cpp:70-82`).

> Ein nachträgliches `output = min(output, cap)` **nach** `update()` wäre falsch:
> der Regler würde seine eigene Begrenzung nicht kennen und gegen sie
> integrieren — klassisches Windup. Reihenfolge ist hier entscheidend.

**Wirkung und Kosten:** Bei `approachBand = 8 K`, `approachMaxDuty = 0,35`
beträgt der Anstieg auf dem letzten Stück ~0,35 · 0,5 = 0,175 K/s, die letzten
8 K dauern also ~46 s statt ~16 s. **Die Aufheizzeit verlängert sich um ~30 s.**
Dafür ist das Heizelement beim Erreichen des Sollwerts nur mäßig übertemperiert.
Das ist der bewusst eingegangene Kompromiss.

#### Kritische Wechselwirkung: Deckel muss beim Bezug aufgehoben werden

Beim Bezug sackt die Temperatur um ~4 K ab — die Maschine ist damit **innerhalb**
des 8-K-Anfahrbands und würde auf 35 % gedeckelt. Der Feedforward braucht aber
**~57 %**. Der Deckel würde Baustein 4 aushebeln.

**Deshalb: Deckel aussetzen, solange Wasser gezogen wird.** Dieselbe Bedingung,
die den Feedforward aktiviert (`Heater.cpp:106`):

```cpp
const bool drawing = pumpFlowRate && *pumpFlowRate > 0.01f
                  && valveStatus  && *valveStatus != 0;
const float cap = drawing ? 1.0f : computeApproachCap();
```

Diese Zeile ist der wichtigste Einzelpunkt der ganzen Umsetzung. Ohne sie
verbessert Phase 2 den Overshoot und verschlechtert das Absacken.

---

### Baustein 4 — Wärmemodell und Heizleistung konfigurierbar

**Problem:** Siehe [01-ANALYSE.md](01-ANALYSE.md) §5, Befunde B2/B3/B7. Das
Feedforward-Modell ist korrekt, aber unkonfigurierbar und per Default aus.

**Lösung:** Vier Dinge erreichbar machen:

| Größe | heute | künftig |
|---|---|---|
| Heizleistung | React-State, Default 1360 W (`CalibrationTab.jsx:20`) | **persistente Einstellung**, speist Autotune *und* Kff |
| `incomingWaterTemp` | hart 23 °C (`Heater.h:86`) | Einstellung, Default 15 °C |
| `heatLossWatts` | hart 5 W (`Heater.h:88`) | Einstellung, Default 15 W (gedämmt) |
| `heaterEfficiency` | hart 0,95 (`Heater.h:87`) | Einstellung, Default 0,95 |

Dazu in der Web UI eine Aktion **„Kff aus Heizleistung berechnen“**
(`kf = 1000 / heaterWattage`), damit der Feedforward nicht länger ein
Nebenprodukt des Autotunes ist.

**`incomingWaterTemp` zuerst.** Er ist der sensibelste Parameter: 23 °C statt
realer 15 °C unterschätzt den Leistungsbedarf um
`(94−15)/(94−23) − 1 ≈ 11 %`. Für Phase 0 reicht `kf` + Heizleistung; die
Modellparameter sind Phase 3.

**Zu `heatLossWatts`:** Der Term `heatLossWatts / currentFlowRate`
(`Heater.cpp:267`) wächst bei kleinem Durchfluss stark an. Der Feedforward ist
aber ohnehin erst ab `flow > 0,01 ml/s` aktiv, und das Ergebnis läuft gegen die
Stellgrößengrenze. Praktisch unkritisch — aber ein Grund, `heatLossWatts`
konservativ (≤ 25 W) zu halten und nicht als Tuningknopf zu missbrauchen.

---

### Baustein 5 — Setpoint-Rampe (optional, Default aus)

**Lösung:** Der Heater führt einen internen Arbeits-Sollwert, der dem
kommandierten mit begrenzter Rate folgt (`setpointRampCPerMin`, 0 = aus).
SimplePID zeigt auf den Arbeitswert.

**Warum optional:** Nach Baustein 1–3 ist der Nutzen klein — die Rampe
unterdrückt denselben Windup-Mechanismus, den das Freigabeband schon an der
Wurzel abschneidet. Sie wird aufgenommen, weil sie beim Wechsel Dampf → Brüh
(145 → 94 °C) und bei Profilen mit Phasentemperaturen sauberere Übergänge liefert.

**Zwei Fallen, die sauber behandelt werden müssen** (siehe
[01-ANALYSE.md](01-ANALYSE.md) §6):

1. `getSetpoint()` muss den **kommandierten** Wert zurückgeben —
   `GaggiMateController.cpp:503` vergleicht darauf.
2. Das Abschalt-Gate `setpoint <= 0.0f` (`Heater.cpp:47`) muss auf den
   kommandierten Wert prüfen. Sonst heizt die Maschine beim Abschalten weiter
   (Rampenwert noch > 0) oder bleibt beim Einschalten aus.

Außerdem: Rampe beim Bezug aussetzen (`drawing`), sonst bremst sie die Erholung.

---

## Zusammenspiel

```
Heater::loopPid()                                      [1 Hz]
  │
  ├─ temperature = sensor->read()
  │
  ├─ drawing = (flow > 0,01) && (valve offen)
  │
  ├─ BAUSTEIN 5:  Arbeits-Sollwert Richtung Ziel rampen   (aus wenn drawing)
  │
  ├─ BAUSTEIN 4:  Störgrößen-Feedforward berechnen        (nur wenn drawing)
  │                 setDisturbanceFeedforward(flow, gain)
  │
  ├─ BAUSTEIN 3:  cap = drawing ? 1,0 : computeApproachCap()
  │                 setCtrlOutputLimits(0, cap · 1000)
  │
  └─ simplePid->update()
        ├─ BAUSTEIN 1:  integrieren nur wenn error ≤ integralBand
        ├─ BAUSTEIN 2:  Iout klammern + Zustand rückrechnen
        └─ vorhandenes Anti-Windup sieht jetzt den echten cap
```

### Abdeckungsmatrix

| Betriebsfall | Wirksamer Baustein |
|---|---|
| Kaltstart → 94 °C | 3 (Deckel), 1 (kein Windup) |
| Aufwachen aus Standby (warm) | 1, 3 |
| Dampf → Brüh (145 → 94 °C) | 1 (negativer Fehler integriert → Abbau), 5 |
| Espressobezug (36 s, ~2 ml/s) | 4 (Feedforward), 3 **ausgesetzt** |
| Langer Wasserbezug / Americano | 4, 3 ausgesetzt, 1 (Fehler > Band → kein Windup) |
| Leerlauf halten | 2 (Deckel), I-Anteil trägt ~2 % |

Jeder Baustein ist über seinen Parameter **einzeln abschaltbar** (`0` = aus) und
fällt dann exakt auf Upstream-Verhalten zurück. Das ist Absicht: so lässt sich
die Wirkung am Gerät einzeln messen.

---

## Verworfene Alternativen

Dokumentiert, damit sie nicht erneut vorgeschlagen werden.

### Sollwertformung im Display statt im Controller — verworfen

Reizvoll, weil ohne Protokolländerung und ohne Controller-Flash.

Verworfen, weil:
* **Nachrichtenflut.** `updateControl()` läuft bei ~20 Hz, Boiler-Befehle gehen
  bei exakter Float-Ungleichheit raus (`Controller.cpp:1146`,
  `GaggiMateComm.h:34`) → 20 BLE-Nachrichten/s dauerhaft.
* **Die wirksamen Bausteine sind so nicht erreichbar.** Das Display kann nur den
  Sollwert setzen. I-Band, I-Klammer und Leistungsdeckel sitzen im Regelkreis.
  Display-seitig bliebe nur Baustein 5 — der schwächste.
* **Verbindungsabhängig.** Eine Rampe, die über eine Funkstrecke getaktet wird,
  bleibt bei Aussetzern auf einem Zwischenwert stehen.

### Vorhandenen Setpoint-Filter von SimplePID aktivieren — verworfen

`activateSetPointFilter(true)` wäre eine Einzeiler-Rampe.

Verworfen, weil der Filter zweiter Ordnung mit Dämpfung `setpointFiltXi = 1,2`,
Eckfrequenz 0,005 Hz und einer Verzögerungs-Deque (`setpointDelaySamples`)
arbeitet (`SimplePID.cpp:94-118`). Er ist für die *Synchronisation des
Setpoint-Feedforwards* gebaut, nicht als Ratenbegrenzer. Seine effektive Rampe
ist aus den Parametern nicht ablesbar und nicht in K/min angebbar — in der Web UI
nicht sinnvoll bedienbar. Baustein 5 nutzt stattdessen einen expliziten
Ratenbegrenzer im `Heater`; der Filter bleibt unberührt aus.

### Stationäre Vorsteuerung (Bias-Term) — verworfen

Ein konstanter Offset in Höhe des Haltestellgrads (~2 %), damit der I-Anteil
nicht von null aufbauen muss.

Verworfen als **redundant**: Mit Baustein 1 ist der I-Anteil ohnehin klein, und
der Haltewert von ~20 Einheiten ist innerhalb der Klammer aus Baustein 2 schnell
erreicht. Ein zusätzlicher Bias wäre ein weiterer Parameter ohne messbaren
Zusatznutzen — und ein weiterer Weg, versehentlich zu überheizen.

### τc-Faktor des Autotuners konfigurierbar machen — verworfen für dieses Konzept

`τc = 1,5 · L` ist hart verdrahtet (`Autotune.cpp:177`). Ein größeres τc gäbe
konservativere Gains.

Nicht aufgenommen, weil es das **Identifikationsergebnis** verändert statt den
Regler zu verbessern, und weil die SIMC-Gains für diese Strecke bereits brauchbar
sind (siehe [01-ANALYSE.md](01-ANALYSE.md) §3) — das Problem ist, dass sie *nicht
benutzt* werden, weil `DEFAULT_PID` drinsteht. Phase 0 löst das durch Autotune,
nicht durch Verändern der Regel. Sollte sich nach Phase 0–3 zeigen, dass die
Gains zu aggressiv sind, ist das ein eigener, kleiner Folgeschritt.

### Temperatur-Boost per Profilphase — kein Codebedarf

Profile können pro Phase eine Temperatur überschreiben
(`schema/profile.json`, Feld `phase.temperature`). Ein Vorheiz-Boost vor dem
Bezug ist damit **heute schon** möglich, ohne Firmwareänderung.

Als Ersatz für Baustein 4 trotzdem ungeeignet: ein Boost ist eine Steuerung
ohne Bezug zum tatsächlichen Durchfluss und wirkt bei L = 22 s zu spät oder zu
früh. Als *Ergänzung* nach Phase 3 aber legitim — und ein guter erster Versuch,
solange Phase 3 nicht umgesetzt ist.

---

## Risiken

| Risiko | Wirkung | Gegenmaßnahme |
|---|---|---|
| Anfahrdeckel beim Bezug nicht ausgesetzt | Absacken wird **schlimmer** | `drawing`-Bypass; expliziter Testfall in [06](06-TEST-VALIDIERUNG.md) |
| `integralBand` zu klein (≤ 1 K) | Stationäre Abweichung unter Sollwert | Default 3 K; Band-Logik asymmetrisch |
| I-Klammer ohne Zustandsrückrechnung | Overshoot nur verschleiert | `clampedIntegralOutput()` rechnet zurück |
| Protokoll-Mismatch nach einseitigem Flash | Maschine heizt nicht mehr (OTA bleibt) | Immer beide Firmwares; Hinweis im Phasenplan |
| `getSetpoint()`-Semantik bei Rampe geändert | `GaggiMateController.cpp:503` bricht | Rampe in separater Variable; Phase 4 separat testen |
| Kff zu groß gesetzt | Überschwinger **nach** dem Bezug | `calculateSafetyScaling` (`Heater.cpp:276`) dämpft über Sollwert; `kf = 1000/W` nicht freihändig erhöhen |
| Aufheizzeit +~30 s | Komfort | `approachBand`/`approachMaxDuty` einstellbar; bewusster Kompromiss |

→ Weiter mit [03-IMPLEMENTIERUNG.md](03-IMPLEMENTIERUNG.md).

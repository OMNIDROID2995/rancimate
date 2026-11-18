# 07 — Profilgesteuerte Temperaturregelung beim Bezug

Erweiterung zu [02-REGELUNGSKONZEPT.md](02-REGELUNGSKONZEPT.md). Die Bausteine 1–5
bleiben unverändert gültig. Dieses Dokument ergänzt die Bausteine **6–10**, die
die Information aus dem laufenden Bezugsprofil nutzen.

Basis: `/home/linux/RANCIMATE/src` (eigenständiges Repo, Stand `v1.10.0` plus
RANCIMATE-Änderungen). Pfade beziehen sich auf diesen Baum.

---

## 1. Was nach Phase 0–3 noch fehlt

Phase 0–3 lösen den Overshoot an der Wurzel und schalten den Durchfluss-Feedforward
scharf. Vier Lücken bleiben, und alle haben dieselbe Ursache: **Der Controller kennt
nur die Gegenwart, das Display kennt den Plan.**

| # | Restproblem | Warum Phase 0–3 es nicht lösen |
|---|---|---|
| R1 | Der Feedforward reagiert auf den *gemessenen* Durchfluss und kommt deshalb um die Trägheit des Heizstabs zu spät (τ₂ ≈ 4,4 s, [05](05-PARAMETER-REFERENZ.md)) | Den kommenden Durchfluss kennt nur das Profil. |
| R2 | Am Bezugsende läuft die im Heizstab gespeicherte Wärme nach und erzeugt einen Nachschwinger | Der FF endet, wenn der Fluss endet. Dass der Fluss gleich endet, weiß nur das Display. |
| R3 | Der Sensor zeigt den Einbruch erst ~22 s später, meist **nach** dem Shot. P- und I-Anteil reagieren dann auf eine Störung, die der FF schon bezahlt hat. | Der Regler kann einen alten Einbruch nicht von einem neuen unterscheiden. |
| R4 | Phasentemperaturen im Profil (`phase.temperature`) sind beim gedämmten Boiler wirkungslos. Abkühlen ohne Wasserentnahme dauert bei −0,006 K/s Stunden, und ein Anstieg wird erst nach 22 s sichtbar. | Der PID kann einem Sollwertsprung innerhalb von 30 s nicht folgen. |

Zusätzlich hat die Code-Durchsicht einen **Fehler im vorhandenen Feedforward**
ergeben (§6), der in Phase 3 mitbehoben werden sollte.

---

## 2. Leitgedanke: Während des Bezugs über die Energiebilanz regeln

Während Wasser fließt, ist der Boiler kein Integrator mit 22 s Totzeit mehr,
den man über den Sensor führen könnte. Bekannt sind aber alle Energieflüsse:

```
C · dT/dt  =  η · P_Heiz · duty  −  ṁ · c_p · (T − T_zu)  −  P_Verlust
```

Die Wärmekapazität folgt direkt aus der identifizierten Streckenverstärkung:

```
C = η · P_Heiz / k′ = 0,95 · 1100 W / 0,50 K/s ≈ 2,1 kJ/K      (≈ 500 ml Wasseräquivalent inkl. Messing)
```

Daraus folgen drei Zahlen, die dieses Dokument tragen:

| Größe | Rechnung | Wert |
|---|---|---|
| Energie für einen 40-ml-Shot | 40 g · 4,18 · (94 − 15) | **≈ 13,2 kJ ≈ 12,6 s Vollgas** |
| Abkühlrate durch 2 ml/s Durchfluss ohne Heizung | 2 · 4,18 · 79 / 2100 | **≈ −0,32 K/s** |
| Physikalische Durchflussgrenze bei 94 °C | 0,95 · 1100 / (4,18 · 79) | **≈ 3,2 ml/s** |

**Daraus folgt:** Bei 2 ml/s kann der Regler die Boilertemperatur mit rund
**±0,3 K/s in beide Richtungen** bewegen. Nach oben heizt er mehr, nach unten
heizt er weniger und lässt das Frischwasser kühlen. Ohne Durchfluss geht es nur
nach oben. Während des Bezugs gibt es also echte Stellautorität, aber keine
zeitnahe Messung. Das spricht für eine **Steuerung über die Energiebilanz**
während des Bezugs und für eine **Korrektur über den Sensor danach**.

Das Profil liefert die Größen, die diese Steuerung vorausschauend macht.

---

## 3. Was das Profil liefern kann

Alles davon steht in `BrewProcess` (`src/display/core/process/BrewProcess.h`)
bereits zur Laufzeit zur Verfügung:

| Information | Quelle im Display | Verfügbar bei |
|---|---|---|
| Geplanter Durchfluss jetzt und in *t* Sekunden | `getPumpFlow()`, `transitionAlpha()`, nächste Phase | **Fluss-Phasen** (`PUMP_TARGET_FLOW`) |
| Durchfluss bei Druck-Phasen | nur gemessen (`currentFlow`), Puck-abhängig | Druck-Phasen: nur Gegenwart, Prognose erst über Baustein 10 |
| Voraussichtliches Bezugsende | Phasendauer bzw. Volumenziel und `VolumetricRateCalculator` (wird schon für den Stopp genutzt) | alle Profile |
| Sollwertverlauf | `getTemperature()` je Phase | Profile mit Phasentemperaturen |
| Art des Bezugs | `profile.utility` (Spülen, Heißwasser) | alle |
| Geplante Gesamtmenge | `profile.getTotalVolume()` | volumetrische Profile |

---

## 4. Die Bausteine 6–10

| # | Baustein | Löst | Ort | Profil nötig? |
|---|---|---|---|---|
| 6 | Bezugsmodus: I-Anteil einfrieren, Nachlaufsperre | R3 | Controller | nein |
| 7 | Energiebilanz-Nachführung | R3, Modellfehler | Controller | nein |
| 8 | Vorausschauender Feedforward (Vorlauf und Auslauf) | R1, R2 | Display + Controller | **ja** |
| 9 | Temperaturverlauf über die Energiebilanz | R4 | Display + Controller | **ja** |
| 10 | Lernender Profil-Trim | Restfehler, Druck-Profile | Display | **ja** |

Baustein 6 ist die Voraussetzung für 7–9. Wie bei 1–5 gilt: **Jeder Baustein ist
über seinen Parameter abschaltbar (`0` = aus) und fällt dann auf das vorherige
Verhalten zurück.**

### Baustein 6 — Bezugsmodus im Heater

**Problem:** Der I-Anteil integriert während und nach dem Shot einen Fehler, den
der Sensor verspätet anzeigt. Diese Ladung muss der gedämmte Boiler danach
aussitzen (derselbe Mechanismus wie beim Aufheiz-Overshoot).

**Lösung:** Der Heater kennt einen Zustand `BrewState`:

```
IDLE ──(drawing ODER hint.active)──► BREWING ──(Fluss endet)──► HOLDOFF ──(t > holdoff)──► IDLE
```

* **BREWING:** I-Anteil eingefroren (`feedback_integralState` wird weder erhöht
  noch verringert). Der P-Anteil bleibt, aber mit dem Faktor `brewKpScale`
  (Default 1,0; Empfehlung 0,5), weil der Fehler, auf den er reagiert, 22 s alt ist.
  Der Anfahrdeckel (Baustein 3) ist aufgehoben, wie in 02 bereits vorgesehen.
* **HOLDOFF:** Dauer `brewHoldoffS` (Default 0 = aus; Empfehlung **≈ L = 22 s**).
  Der I-Anteil bleibt eingefroren. Die P-Reaktion ist auf `brewHoldoffMaxDuty`
  begrenzt. Danach greift wieder der normale Regler mit I-Band.

Umsetzung in `SimplePID`: Ein Flag `integrationFrozen` ergänzt die schon geplante
Bedingung von Baustein 1:

```cpp
const bool integrate = !integrationFrozen && ((integralBand <= 0.0f) || (error <= integralBand));
```

Diese Bedingung ist dieselbe Stelle wie in Phase 1. Baustein 6 kostet dort eine
Zeile. Der Zustandsautomat sitzt in `Heater::loopPid()` neben der
`drawing`-Bedingung aus Baustein 3.

**Ohne Display-Hinweis funktionsfähig:** `drawing` (Ventil offen und Fluss
> 0,01 ml/s) reicht als Auslöser. Das Profil macht den Eintritt nur früher
(Baustein 8).

### Baustein 7 — Energiebilanz-Nachführung

**Problem:** Der FF rechnet mit geschätzten Größen (T_zu, η, P_Heiz). Ein Fehler
von 10 % bei T_zu bedeutet bei 13 kJ etwa 1,3 kJ, also rund 0,6 K. Diesen Fehler
sieht der Sensor erst 22 s später, und dann reagiert der Regler wieder auf alte
Information.

**Lösung:** Der Heater bilanziert ab Eintritt in BREWING:

```
E_ab  += ṁ · c_p · (T_soll − T_zu) · Δt          // vom Wasser entnommen
E_zu  += η · P_Heiz · duty · Δt                   // vom Heizstab geliefert
D      = E_ab − E_zu                              // Defizit (> 0: zu wenig geheizt)
```

Am Ende von BREWING ist **D** die beste verfügbare Schätzung dessen, was fehlt.
Das ist früher und genauer als der Sensor, der dasselbe erst nach 22 s und über
die Schale gefiltert zeigt. In HOLDOFF wird D als **begrenzter Energieimpuls**
nachgeliefert:

```
duty_holdoff = clamp(D / (η · P_Heiz · T_impuls), 0, brewHoldoffMaxDuty)
```

D wird um die gelieferte Energie verringert. Ist D negativ (überheizt), gibt es
keinen Impuls. Den Abbau übernimmt dann die Rückführung nach HOLDOFF; ein
negativer I-Anteil ist in Baustein 1 ausdrücklich erlaubt.

**Robustheit:** Die Bilanz läuft nur über ein Fenster von höchstens Shot + L.
Ein Modellfehler erzeugt deshalb einen **begrenzten** Restfehler, keine Drift.
Diesen Restfehler regelt danach der normale PID aus. Obergrenze für D:
`brewEnergyCapKJ` (Empfehlung 15 kJ) als Schutz gegen falsche Flusswerte.

> **Sicherheitsrandbedingung:** In der Software gibt es keinen absoluten
> Übertemperaturschutz ([01 §6](01-ANALYSE.md)). Der Impuls ist deshalb doppelt
> begrenzt (Energie und Duty) und entfällt vollständig, sobald
> `temperature > setpoint + 1 K`.

### Baustein 8 — Vorausschauender Feedforward

**Problem:** R1 und R2. Der Feedforward folgt dem Fluss *jetzt*, der Heizstab
gibt seine Wärme aber erst mit τ₂ ≈ 4 s Verzögerung ans Wasser ab.

**Lösung:** Der Feedforward arbeitet mit dem **geplanten Fluss in τ_v Sekunden**:

```
flow_ff = max(flow_gemessen, flow_plan(t + τ_v))     während BREWING
flow_ff = flow_plan(t + τ_v)                         wenn das Ende innerhalb τ_v bevorsteht
```

* **Vorlauf:** Beim Start (Präinfusion mit wenig Fluss) beginnt die Heizleistung
  τ_v früher. Bei Fluss-Profilen ist der Plan exakt bekannt.
* **Auslauf:** Endet der Bezug innerhalb von τ_v (Zeitziel oder volumetrische
  Prognose, die das Display für den Pumpenstopp ohnehin rechnet), fährt der FF
  vorher herunter. Die im Heizstab gespeicherte Wärme deckt dann das Ende ab
  statt eines Nachschwingers.
* **Druck-Phasen:** Der Plan entspricht dem gemessenen Fluss (keine Prognose),
  nur der Auslauf wirkt. Eine Prognose liefert erst Baustein 10.

`τ_v` = `brewFfLeadS`: Default 0 (aus), Startwert für Versuche **4 s** (≈ τ₂).
Der Wert ist nicht identifiziert und wird am Gerät ermittelt (§8).

Sicherheitsregeln im Controller, unabhängig vom Display:

1. Ohne offenes Ventil wird Vorlauf-FF höchstens `brewFfLeadS` lang gewährt, danach
   fällt er weg. Ein hängengebliebener Hinweis heizt so nicht dauerhaft.
2. Der Hinweis hat ein Ablaufdatum (`valid_ms`). Bleibt er aus, etwa bei einem
   BLE-Aussetzer, gilt der gemessene Fluss wie heute.
3. Der Vorlauf-FF unterliegt der Sicherheitsstaffelung (§6, korrigiert).

### Baustein 9 — Temperaturverlauf über die Energiebilanz

**Problem:** R4. Ein Profil mit Phasentemperaturen (z. B. 95 °C Präinfusion,
dann 92 °C) wird heute als Sollwertsprung an den PID gegeben. Beim gedämmten
Boiler passiert innerhalb des Shots fast nichts, danach schwingt es.

**Lösung:** Während BREWING wird der Sollwertverlauf **gesteuert**, nicht geregelt:

```
P_soll = ṁ · c_p · (T_soll(t) − T_zu) / η   +   C · dT_soll/dt / η
```

Der erste Term ist der vorhandene Feedforward. Der zweite Term ist neu: Er
liefert für +1 K rund 2,1 kJ zusätzlich (≈ 2 s Vollgas) bzw. hält für −1 K rund
3 s die Heizung zurück, während das Frischwasser kühlt.

* `SimplePID` hat dafür bereits einen Sollwert-Feedforward (`setpointDerivative *
  gainFF`, `SimplePID.cpp:44-45`). Er ist derzeit aus und an den Sollwertfilter
  gekoppelt. Baustein 9 übergibt `dT_soll/dt` aus dem Display-Hinweis direkt;
  `gainFF = C / (η · P_Heiz) · 1000 = 1000 / k′`, also genau der Wert, den der
  Autotuner schon als `setpoint_FF` loggt (`Heater.cpp:220`).
* Grenze: Die erreichbare Rate ist ±0,3 K/s bei 2 ml/s und ohne Durchfluss null
  nach unten. Das Display begrenzt `dT_soll/dt` vor dem Senden auf das, was beim
  geplanten Fluss möglich ist, und zeigt im Profil-Editor an, wenn ein
  Temperatursprung physikalisch nicht erreichbar ist.
* Die Bilanz von Baustein 7 rechnet mit `T_soll(t)` statt mit dem festen Sollwert.

**Ehrlicher Vorbehalt:** Was hier gesteuert wird, ist die Boiler-Mitteltemperatur.
Der Sensor kann den Verlauf erst 22 s später bestätigen, und die Temperatur am
Brühkopf liegt durch die Brühgruppe noch einmal verzögert und versetzt. Ob ein
gesteuerter Verlauf in der Tasse ankommt, lässt sich nur mit einer Messung am
Brühkopf prüfen (§8).

### Baustein 10 — Lernender Profil-Trim (optional, zuletzt)

**Problem:** Druck-Profile haben keinen planbaren Fluss, und alle Modellfehler
wiederholen sich von Shot zu Shot gleich.

**Lösung:** Iterative Learning im Display, das den Controller nicht berührt:

* Nach jedem Shot wertet das Display den Temperaturverlauf im Fenster
  [Start + L, Ende + L] aus, also das, was der Sensor zum Shot tatsächlich
  gesehen hat. Bezugsdaten liegen in der Shot-History vor.
* Pro Profil werden zwei Werte gespeichert:
  * `ffTrim` (0,8 … 1,2): Korrekturfaktor auf den Feedforward. Lag die
    Temperatur im Mittel 1 K zu tief, steigt der Trim um `gain · 1 K · C / E_shot`.
  * `flowTrace`: geglätteter Flussverlauf über der Zeit, der bei Druck-Phasen
    als `flow_plan` für Baustein 8 dient.
* Lernrate klein (Empfehlung 0,3), Trim eng begrenzt, Lernen nur bei
  „normalen“ Shots (Dauer ±30 % zum Profil, kein Abbruch).

---

## 5. Protokoll und Aufteilung Display ↔ Controller

### 5.1 Neue Nachricht

In `lib/NanoPbComm/proto/gaggimate.proto`, freie Tag-Nummer 14 im `Payload`:

```proto
// Brew thermal hint (display -> controller). Sent while a brew process runs,
// ~2 Hz plus on phase change. Pure hint: the controller must work without it.
message BrewThermalHint {
    bool active = 1;              // a brew process is running
    float flow_forecast = 2;      // planned flow at t + lead (ml/s); < 0 = unknown
    float setpoint_rate = 3;      // dT_target/dt (K/s), already clamped by the display
    float remaining_s = 4;        // predicted time to end of draw; < 0 = unknown
    float ff_trim = 5;            // per-profile learned FF factor, 0 = 1.0
    uint32 valid_ms = 6;          // hint expires after this
}
```

`BrewThermalHint brew_hint = 14;` im `oneof content`.

Neue Einstellungen in `HeaterSettings` (Felder 1–17 sind durch Phase 1–4 belegt):

```proto
    float brew_kp_scale = 18;          // P scale while BREWING, 0 = 1.0
    float brew_holdoff_s = 19;         // 0 = Baustein 6/7 off
    float brew_holdoff_max_duty = 20;  // 0..1
    float brew_energy_cap_kj = 21;     // 0 = Baustein 7 off
    float brew_ff_lead_s = 22;         // 0 = Baustein 8 off
    bool brew_setpoint_ff = 23;        // Baustein 9
```

**Diese Felder gehören in denselben Protokollschritt 16 → 17 wie Phase 1.** Dann
müssen Display und Controller nur einmal gemeinsam geflasht werden, und alle
späteren Phasen schalten nur noch Parameter frei.

**Nachrichtenlast:** 2 Hz und nur während des Bezugs. Damit trifft die
Begründung aus [02](02-REGELUNGSKONZEPT.md) gegen display-seitige
Sollwertformung (20 Hz dauerhaft) hier nicht zu. Der Hinweis läuft über die
vorhandene Coalescing-Queue; ein verlorener Hinweis wird vom nächsten ersetzt.

### 5.2 Wer macht was

| Aufgabe | Display | Controller |
|---|---|---|
| Profil auswerten, Fluss-/Endprognose, `dT_soll/dt` begrenzen | ✔ (`BrewProcess`, neu: `getThermalHint()`) | — |
| Hinweis senden | ✔ (`Controller::updateControl()`, eigener 500-ms-Takt) | — |
| Bezugsmodus, Bilanz, FF, Sicherheitsgrenzen | — | ✔ (`Heater`, `SimplePID`) |
| Lernen (Baustein 10) | ✔ (Profilspeicher, Shot-History) | — |
| Einstellungen / Web UI | ✔ (`Settings`, `WebUIPlugin`, `MachineTab.jsx`) | übernimmt sie über `HeaterSettings` |

Leitregel: **Die Echtzeitregelung bleibt vollständig im Controller.** Das
Display liefert nur Prognosen. Fällt die Verbindung aus, verhält sich der
Controller wie mit Bausteinen 1–7 ohne Profilwissen.

---

## 6. Befund: Sprungstelle in der Feedforward-Sicherheitsstaffelung

`Heater::calculateSafetyScaling()` (`lib/GaggiMateController/src/peripherals/Heater.cpp:276-290`)
mit `tempError = temperature − setpoint`:

| tempError | Faktor |
|---|---|
| > +1 K | 0 |
| 0 … +1 K | 1,0 → 0,7 |
| **−0,01 K** | **0,70** |
| −1 K … 0 | 0,7 → 1,0 |
| < −1 K | 1,0 |

Bei exakt 0 liefert der obere Zweig 1,0, schon 0,01 K darunter der untere Zweig
0,7. **Knapp unter dem Sollwert, also im typischen Zustand beim Shot-Start, gibt
der Feedforward nur ~70–80 % ab.** `shot-21.json` startete bei 93,7 °C für 94 °C:
Faktor ≈ 0,79. Weil der Faktor auf dem 22 s verzögerten Sensor beruht, ändert er
sich zudem während des Shots aus Gründen, die mit dem aktuellen Wasser nichts zu
tun haben.

Die Kommentare im Code („Scale from 70% to 100% as temperature drops“) und der
Funktionsname legen nahe, dass gemeint war: **volle Leistung unter dem Sollwert,
Rücknahme darüber.** Korrektur für Phase 3:

```cpp
float Heater::calculateSafetyScaling(float tempError) {
    // tempError = temperature - setpoint. Full FF at or below setpoint,
    // linear back-off to 0 at +1 K above (sensor lags ~L, so stay generous).
    if (tempError <= 0.0f) return 1.0f;
    if (tempError >= 1.0f) return 0.0f;
    return 1.0f - tempError;
}
```

Allein das sollte das Absacken messbar verringern. Deshalb gehört es in die
erste Messreihe nach Phase 3 und wird getrennt bewertet.

---

## 7. Ergänzter Phasenplan

| Phase | Inhalt | Profil? | Flashen |
|---|---|---|---|
| 0 | Recommissioning (wie [README](README.md)) | — | nein |
| 1 | I-Band, I-Klammer **+ alle Protokollfelder aus 03 und §5.1** (`PROTOCOL_VERSION` 17) | — | **beide** |
| 2 | Anfahrdeckel | — | Controller |
| 3 | Wärmemodell, Heizleistung, **Korrektur §6** | — | beide (Web UI) |
| **5** | **Bezugsmodus + Energiebilanz (Bausteine 6, 7)** | nein | Controller |
| **6** | **`BrewThermalHint` senden, vorausschauender FF (Baustein 8)** | ja | beide |
| **7** | **Temperaturverlauf über Bilanz (Baustein 9)** | ja | beide |
| 4 | Setpoint-Rampe (optional; nach Baustein 9 nur noch für Dampf → Brüh relevant) | — | Controller |
| **8** | **Lernender Trim (Baustein 10, optional)** | ja | Display |

Reihenfolge-Begründung: Phase 5 wirkt ohne Profil, ist die Voraussetzung für 6–8
und schließt die größte verbleibende Lücke (R3). Phase 6 bringt den größten
profilabhängigen Effekt mit dem geringsten Modellrisiko. Phase 7 ist der
aufwendigste Baustein und lohnt sich nur, wenn Profile mit Phasentemperaturen
tatsächlich genutzt werden.

---

## 8. Validierung (Ergänzung zu [06](06-TEST-VALIDIERUNG.md))

### 8.1 Messung am Brühkopf: einmal nötig

Der Boilersensor sitzt außen auf der Schale und ist 22 s verzögert. Er ist für
*Brühtemperaturstabilität* nur ein Ersatzmaß. Für die Abnahme von Baustein 8 und
9 wird mindestens eine Messreihe mit einem Thermoelement im Siebträger (z. B.
Siebträger mit Fühlerbohrung oder Thermoelement im Wasserstrahl bei Blindsieb)
gebraucht. Ohne diese Messung lässt sich τ_v nicht sinnvoll einstellen.

### 8.2 Ermittlung von τ_v

Fluss-Profil, fest 2 ml/s, 30 s. Drei Shots je mit `brewFfLeadS` = 0 / 3 / 6 s,
dazwischen jeweils ≥ 10 min Erholung. Kriterium ist die Spannweite der
Brühkopftemperatur in [5 s, Ende]. Der beste Wert wird übernommen.

### 8.3 Abnahmekriterien (Boilersensor, Shot 30–40 s, ~2 ml/s)

| Kriterium | heute | nach Phase 3 (Erwartung) | Ziel nach Phase 5–6 |
|---|---|---|---|
| Absacken | −3,7 … −4,0 K | < −1,5 K | **≤ ±0,8 K** |
| Nachschwinger nach dem Shot | nicht separat gemessen | ? | **≤ +0,5 K** |
| Zurück im Band ±0,3 K nach Shot-Ende | > 2 min | ? | **≤ 90 s** |
| Bilanzrest D am Ende von BREWING | — | — | `|D| ≤ 1,5 kJ` (sonst Modell nachstellen) |

D und der Zustand von `BrewState` gehören ins Duty-Logging aus
[06 §1](06-TEST-VALIDIERUNG.md), damit jede Messung zeigt, was der Regler gerade tat.

### 8.4 Testfälle, die explizit geprüft werden

* Verbindungsabbruch während des Shots: Hinweis läuft ab → FF auf gemessenem Fluss, kein Dauerheizen.
* Shot vom Nutzer abgebrochen: Auslauf greift nicht, HOLDOFF und Bilanz trotzdem korrekt.
* Spülen / Heißwasser (`utility`, bis 10 ml/s): FF sättigt, D wird groß → Cap greift.
  Erwartung: Absacken bleibt physikalisch bedingt (§2, Grenze 3,2 ml/s).
* Zwei Shots direkt hintereinander: zweiter Shot startet in HOLDOFF des ersten.

---

## 9. Profilseitige Empfehlung ohne Code

Aus der Durchflussgrenze von ≈ 3,2 ml/s (§2) folgt eine Empfehlung, die sofort
wirkt: **Heißwasser für Americano als Fluss-Profil mit ≤ 3 ml/s statt mit
Vollgas** beziehen. Dann kann die Heizung mithalten, und die −9,1 K aus
`americano.csv` sollten sich auf das Feedforward-Niveau eines Espressos
reduzieren. Das dauert länger, schont aber auch den nächsten Shot.

---

## 10. Verworfene Alternativen

### Vollständiges Profil an den Controller übertragen — verworfen

Damit würde der Controller selbst planen. Verworfen, weil Profile groß und
variabel sind (Ziele, Übergänge, Volumen-/Verhältnisziele mit Waagen-Prognose)
und die Logik dazu schon im Display existiert. Sie zu duplizieren hieße, zwei
Implementierungen synchron zu halten. Der Hinweis mit fünf Zahlen reicht.

### Smith-Prädiktor statt Bausteine 1–3 — zurückgestellt

Für eine Integratorstrecke mit Totzeit ist ein Prädiktor die klassische
Lösung: Der PID regelt auf `T_mess + k′·∫_{t−L}^{t}(u − u₀)dτ`, also auf
die Temperatur inklusive der Wärme, die schon unterwegs ist. Das würde den
Overshoot strukturell beheben. Zurückgestellt, weil Bausteine 1–3 einfacher,
einzeln abschaltbar und schon ausgearbeitet sind. Wenn nach Phase 3 eine
Restwelle bleibt, ist das der nächste Kandidat. Bausteine 6/7 sind bereits eine
auf den Bezug beschränkte Form davon.

### Vorheizen über erhöhten Sollwert vor dem Shot — verworfen

Das Display weiß nicht, wann ein Shot beginnt, und ein höherer Sollwert braucht
beim gedämmten Boiler Minuten zum Abbau. Baustein 8 erreicht dasselbe zur
richtigen Zeit.

# RANCIMATE — Thermal control concept for an insulated boiler

Design rationale behind *PID advanced for isolated boilers* and the *Autotune Watchdog*.
Implementation notes and file locations: [RANCIMATE.md](RANCIMATE.md).

Machine: Rancilio Silvia (230 V, 1100 W heater), boiler and brew group wrapped in rock
wool and aluminium tape, GaggiMate controller with dimmer, pressure and flow sensing.

---

## 1. Goals

1. No temperature overshoot after heating up (stock: **+2.5 … +3.1 K**, > 10 min to settle).
2. Much less sag during a shot (stock: **−3.7 … −4.0 K** over a 36 s shot).
3. Stay within a configurable band (default 2 °C) before, during and after shots,
   adapting automatically over several shots if desired.

## 2. Plant identification

Identified from heat-up and shot logs recorded on the machine (June 2026):

| Quantity | Symbol | Value | Meaning |
|---|---|---|---|
| Dead time | L | **≈ 22 s** | heater on → first sensor reaction (sensor on the boiler shell) |
| Plant gain | k′ | **≈ 0.50 K/s** | temperature rise at full power |
| Parasitic lag | τ₂ | ≈ 4.4 s | heater element lag (not identified separately) |
| Net cooling at 96 °C | – | −0.004 … −0.008 K/s | insulated |
| Holding duty | – | **≈ 1.5 … 3 %** | uninsulated Silvia: ~6–8 % |
| Heat capacity | C = η·P/k′ | ≈ 2.1 kJ/K | ≈ 500 ml water equivalent incl. brass |

The insulated boiler is close to a **pure integrator with a long dead time**. Two
consequences carry the whole concept:

1. **Energy that goes in does not come out again.** Any excess during heat-up ends up as
   overshoot that has to be waited out. So energy input near the setpoint must be limited
   at the source (integral term, power cap), not corrected afterwards.
2. **With L ≈ 0.6 × shot duration, feedback is blind during a shot.** So disturbances must
   be fed forward, not regulated away.

## 3. Root causes in stock GaggiMate 1.10

### 3.1 Overshoot after heat-up

In `heatup_warm2.csv` the setpoint is crossed at t = 151 s, but the peak is reached only at
t = 436 s: the temperature keeps rising for **285 s** above setpoint. Dead time alone would
put the peak 22–40 s after the crossing, so the heater keeps heating.

The output leaves saturation at `Kp·e + Dout ≤ 1000`, i.e. about 19 K below setpoint with
the default gains (Kp 58.4, Kd 249). From there to the setpoint the integral collects

```
∫e dt ≈ 19.3 K · ~20 s ≈ 390 K·s   →   Iout = Ki · ∫e dt = 1.027 · 390 ≈ 400 units = 40 % duty
```

while only 1.5–3 % is needed. The integral can only discharge while the temperature is
above setpoint, at roughly 2.6 units/s at −2.5 K, which takes minutes. Two independent
amplifiers:

* **Ki is 6–7× too high** for this plant. The SIMC rule of GaggiMate's own autotuner gives
  Ki ≈ 0.14–0.17 here; the default stems from an uninsulated Gaggia Classic.
* **The integral has no band and no authority limit**, so even a correct Ki charges more
  than an almost lossless boiler can shed.

### 3.2 Sag during a shot

Power needed while brewing:

```
P = ṁ · c_p · ΔT = 2 g/s · 4.18 J/(g·K) · (94 − 20) K ≈ 620 W   (≈ 56 % duty at 1100 W)
```

needed from the first second, but feedback learns about it only after L ≈ 22 s. Both
measured shots have their minimum at 21–23 s, exactly at the dead time. This is not a
tuning problem; only feedforward from the known flow can solve it.

GaggiMate already contains a correct flow feedforward, but in 1.10 it is effectively off:

1. `combinedKff` defaults to 0 and is only set as a by-product of autotune with a wattage.
2. The heater wattage is not a stored setting (browser state only, default 1360 W).
3. The thermal model (inlet 23 °C, loss 5 W, efficiency 0.95) is hard-coded.
4. **Safety scaling bug:** the factor jumps from 1.0 at exactly the setpoint to 0.7 just
   below it, so a shot starting 0.3 K low gets only ~80 % of its feedforward.

### 3.3 Physical limit

At 94 °C a 1100 W heater can heat at most `0.95 · 1100 / (4.18 · 79) ≈ 3.2 ml/s` of 15 °C
water. Above that, no controller can hold temperature (Americano water at full flow sagged
−9.1 K). Hot water should use a flow profile of ≤ 3 ml/s.

## 4. Building blocks (implemented)

All blocks sit behind one master switch; off = stock behaviour. Each block can be switched
off on its own by setting its parameter to 0.

| # | Block | Fights | Where |
|---|---|---|---|
| 1 | Integral band | overshoot (cause) | `SimplePID` |
| 2 | Integral authority limit with state back-calculation | overshoot (safeguard) | `SimplePID` |
| 3 | Heat-up power cap, lifted while drawing | overshoot remainder | `Heater` |
| 4 | Configurable feedforward model + heater wattage + factor, fixed safety scaling | sag | `Heater`, display settings |
| 6 | Brew mode: integral frozen during the shot and for a hold time after it, softened P | post-shot overshoot | `Heater`, `SimplePID` |
| W | Autotune Watchdog | everything, over several shots | display plugin |

### Block 1 — Integral band

```cpp
const bool integrate = !frozen && ((integralBand <= 0) || (error <= integralBand));
```

The asymmetry is the point: only large **positive** errors (far below setpoint) are not
integrated. Negative errors are always integrated, so a charged integral can always
discharge and no steady-state offset remains above setpoint. A symmetric `|error| <= band`
would freeze the integral exactly when it must discharge. With a 3 K band the integral
collects ~9 K·s during heat-up instead of ~390 K·s.

### Block 2 — Integral authority limit

`Iout` is clamped (default 12 % duty) and the integrator state is back-calculated
(`state = clamped / Ki`). Clamping only the output would let the state run on invisibly
and merely hide the overshoot. The lower bound stays open: a negative integral is the
legitimate way to shed excess temperature.

### Block 3 — Heat-up power cap

Within `approachBand` below setpoint the output ceiling tapers linearly from 100 % to
`approachMaxDuty` (default 8 K, 35 %). The ceiling is set via `setCtrlOutputLimits()`
**before** `update()`, so the existing anti-windup sees the real saturation limit;
clamping the output afterwards would cause windup.

**The cap is lifted while water is drawn** (valve open and flow > 0.01 ml/s). During a
shot the temperature is inside the band, but the feedforward needs ~57 %. Without this
bypass the cap would make the sag worse. Cost: heat-up takes roughly 30 s longer.

### Block 4 — Feedforward model

```
power per (ml/s) = (c_p · (T_brew − T_tank) + P_loss / flow) / η
feedforward      = flow · power per (ml/s) · Kff · ffScale
```

`T_brew` is the setpoint from the selected profile. `T_tank` (default 15 °C), heater
wattage (1100 W), loss (15 W) and efficiency (0.95) describe the machine. `T_tank` is the
most sensitive input: 23 °C instead of 15 °C underestimates the need by ~11 %. If no Kff
is stored, the display sends `1000 / wattage`. The safety scaling gives full feedforward
at or below setpoint and backs off linearly to 0 at +1 K.

### Block 6 — Brew mode

```
IDLE ──(drawing)──► BREWING ──(draw ended > 1.5 s)──► HOLDOFF ──(hold time over)──► IDLE
```

In BREWING and HOLDOFF the integrator is frozen and P is multiplied by `brewKpScale`
(default 0.5). The hold time defaults to ≈ L (22 s). The sensor shows the shot's
disturbance late, mostly after the shot; without this the integral charges on a sag the
feedforward has already paid for and overshoots afterwards.

### Interaction (controller loop, 1 Hz)

```
Heater::loopPid()
  ├─ temperature = sensor.read()
  ├─ drawing = flow > 0.01 && valve open
  ├─ brew state machine (block 6)
  ├─ disturbance feedforward (block 4, only while drawing)
  ├─ freeze integral / scale P (block 6)
  ├─ output ceiling = drawing ? 100 % : approach cap (block 3)
  └─ SimplePID::update()
        ├─ integral band (block 1)
        ├─ integral clamp + back-calculation (block 2)
        └─ existing anti-windup sees the real ceiling
```

## 5. Autotune Watchdog

The display measures, at 1 Hz:

* **Shots:** 60 s before, the draw itself, and 180 s after it. Metrics relative to the
  setpoint at shot start: idle swing before the shot, sag (minimum up to 25 s after the
  draw, covering the dead time), overshoot (maximum until the end of the window) and the
  total spread (maximum − minimum over the whole window).
* **Heat-ups:** armed when the temperature is ≥ 5 K below setpoint, watched for 10 min
  after the setpoint is crossed; metric is the overshoot.

A shot passes if its spread is within the threshold (default 2 °C). With the watchdog on,
each failed shot triggers **one** bounded step; sag and overshoot are never corrected in
the same step, so the rules cannot fight each other:

| Observation | Step | Limits |
|---|---|---|
| sag > threshold/2 and ≥ overshoot | ffScale + 0.05·sag (0.03–0.15) | 0.5–1.6 |
| overshoot > threshold/2, no sag | ffScale − 0.05·overshoot | 0.5–1.6 |
| overshoot > threshold/2 after a sag | brewKpScale × 0.85, hold time + 5 s | 0.2–1.0, 10–60 s |
| idle swing > threshold/2 | integral authority × 0.85 | 4–30 % |
| heat-up overshoot > threshold/2 | approach power × 0.85 (+ integral band × 0.85 if > threshold) | 10–60 %, 1–6 K |
| clean heat-up (< 0.2 K) | approach power × 1.05 | 10–60 % |

Parameters at a limit are reported, not "adjusted" again. New values are sent to the
controller only while no process is running. While the watchdog is on it owns these
values: the web UI shows them read-only and the API ignores posted values, so saving
other settings cannot undo an adjustment.

Skipped: utility profiles (flush, cleaning), shots < 10 s or > 120 s, shots without a
stable minute before them, shots more than 5 K away from the setpoint, shots interrupted
before the dip became visible, and a heat-up interrupted by a shot or setpoint change.

## 6. Parameters

Duty values are fractions 0–1 in firmware and protocol, percent in the web UI.

| Setting | NVS key | Proto field | Default | Range |
|---|---|---|---|---|
| Master switch | `ta_en` | 10 | off | – |
| Integral band | `ta_ib` | 11 | 3.0 K | 0–20 (0 = off) |
| Integral authority | `ta_imd` | 12 | 0.12 | 0–1 (0 = unlimited) |
| Approach band | `ta_apb` | 13 | 8.0 K | 0–40 (0 = off) |
| Power at setpoint | `ta_apd` | 14 | 0.35 | 0.05–1 |
| Tank water temperature | `ta_iwt` | 15 | 15 °C | 1–40 |
| Heat loss | `ta_hl` | 16 | 15 W | 0–200 |
| Heater efficiency | `ta_eff` | 17 | 0.95 | 0.5–1 |
| Feedforward factor | `ta_ffs` | 18 | 1.0 | 0.3–3 |
| Integral hold after shot | `ta_bho` | 19 | 22 s | 0–120 (0 = off) |
| P factor during shot | `ta_bkp` | 20 | 0.5 | 0.05–2 |
| Heater wattage | `ta_watt` | – (display) | 1100 W | 300–3000 |
| Watchdog | `ta_wd` | – (display) | off | – |
| Allowed spread | `ta_wdt` | – (display) | 2.0 °C | 0.3–10 |

Recommended PID gains for this plant (SIMC with τc = 1.5·L): `pid` = `35.0,0.16,150.0`,
Kff = 0.909 (1000 / 1100 W).

## 7. Validation

Unit tests (`pio test -e native -f test_thermal_shaping`) cover the integral shaping
against a simulated insulated boiler (integrator, 22 s dead time, 4.4 s lag), the shot and
heat-up recorders, all adaptation rules and their limits.

On the machine, per stage:

| Criterion | Stock | Target |
|---|---|---|
| Heat-up overshoot | +2.5 K | ≤ 0.5 K |
| Peak after crossing the setpoint | 285 s | ≤ 45 s |
| Within +0.3 K after crossing | never (> 760 s) | ≤ 2 min |
| Holding duty, settled | – | 1–4 % |
| Sag during a 30–40 s shot at ~2 ml/s | −4.0 K | ≤ 1.5 K |
| **Heater duty during a shot** | – | **≥ 50 %** (otherwise the cap bypass is broken) |
| Overshoot after a shot | – | ≤ 1.0 K |
| Heat-up time 55 → 94 °C | ~150 s | ≤ 195 s |

The boiler sensor is only a proxy for brew water temperature. At least one series with a
thermocouple in the portafilter is needed to judge stability in the cup.

## 8. Roadmap: profile-aware control

The controller knows only the present; the display knows the profile. Planned blocks:

* **Energy balance after the shot.** Track `E_out = Σ flow·c_p·(T_set − T_tank)·dt` and
  `E_in = Σ η·P·duty·dt` during the shot; deliver the remaining deficit as a bounded pulse
  in the hold time instead of waiting 22 s for the sensor.
* **Look-ahead feedforward.** The display sends a small hint at ~2 Hz while brewing
  (planned flow a few seconds ahead, predicted end of the draw). The heater starts earlier
  and tapers before the known end, compensating the element lag. The controller must keep
  working without the hint.
* **Phase temperatures via energy balance.** During flow the boiler temperature can be
  steered by about ±0.3 K/s in both directions (`C·dT/dt` term), which makes profile phase
  temperatures effective on an insulated boiler.
* **Per-profile learning.** Store a feedforward trim and a typical flow trace per profile,
  so pressure profiles also get a flow forecast.

## 9. Rejected alternatives

* **Setpoint shaping on the display.** Would need ~20 BLE messages/s, cannot reach the
  effective blocks (integral, power cap) and stalls on connection drops.
* **SimplePID's built-in setpoint filter as a ramp.** Second-order filter built for
  feedforward synchronisation; its effective rate cannot be expressed in K/min.
* **Constant holding bias.** Redundant with blocks 1–2 and another way to overheat.
* **Changing the autotuner's τc.** Changes identification instead of control; the SIMC
  gains are fine, the problem was that they were not used.
* **Pre-shot setpoint boost.** The display does not know when a shot starts, and a raised
  setpoint takes minutes to come down on an insulated boiler.

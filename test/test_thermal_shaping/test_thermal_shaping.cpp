// RANCIMATE: insulated-boiler thermal shaping.
// Host-side, no ESP32 runtime — pio test -e native -f test_thermal_shaping.
//
// Groups:
//   A — SimplePID integral shaping against a simulated insulated boiler
//       (integrator + 22 s dead time + 4.4 s lag, identified on the real machine)
//   B — ShotRecorder metrics (before / during / after a shot)
//   C — Watchdog adaptation rules and their limits
//   D — HeatupRecorder

#include <unity.h>

#include <cmath>
#include <cstdint>
#include <deque>

// --- Minimal Arduino surface for SimplePID.cpp --------------------------------
static uint32_t fakeMillis = 0;
static uint32_t millis() { return fakeMillis; }
template <typename T, typename L, typename H> static T constrain(T v, L lo, H hi) {
    return v < lo ? static_cast<T>(lo) : (v > hi ? static_cast<T>(hi) : v);
}
#define ESP_LOGV(...) ((void)0)
#ifndef PI
#define PI 3.14159265358979323846
#endif
#include "SimplePID/SimplePID.cpp"

#include <display/core/thermal/ThermalTuner.h>

using namespace thermal;

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// A — plant simulation
// ---------------------------------------------------------------------------

namespace {
// Identified on the machine (05-PARAMETER-REFERENZ.md): L = 22 s, k' = 0.5 K/s at
// full power, holding duty ~2 % at 94 C (=> 0.01 K/s loss against 20 C ambient).
struct InsulatedBoiler {
    static constexpr int DEAD_TIME_S = 22;
    static constexpr float GAIN = 0.5f; // K/s at full power
    static constexpr float TAU2 = 4.4f;
    static constexpr float LOSS = 0.01f / (94.0f - 20.0f);
    std::deque<float> pipeline = std::deque<float>(DEAD_TIME_S, 0.0f);
    float lagged = 0.0f;
    float water = 74.0f;  // true water temperature
    float sensor = 74.0f; // what the controller sees (delayed water temperature)
    std::deque<float> sensorDelay = std::deque<float>(0);

    void step(float duty) { // 1 s
        pipeline.push_back(duty);
        const float delayed = pipeline.front();
        pipeline.pop_front();
        lagged += (delayed - lagged) / TAU2;
        water += GAIN * lagged - LOSS * (water - 20.0f);
        sensor = water;
    }
};

// Heat-up from 74 C to 94 C; returns overshoot in K over 15 min.
float simulateHeatup(bool shaped) {
    float output = 0.0f, input = 74.0f, setpoint = 94.0f;
    SimplePID pid(&output, &input, &setpoint);
    pid.setSamplingFrequency(1.0f);
    pid.setCtrlOutputLimits(0.0f, 1000.0f);
    pid.activateSetPointFilter(false);
    pid.activateFeedForward(false);
    pid.setControllerPIDGains(58.397f, 1.027f, 249.055f, 0.0f); // upstream DEFAULT_PID
    pid.reset();
    pid.setMode(SimplePID::Control::automatic);
    if (shaped) {
        pid.setIntegralBand(3.0f);
        pid.setIntegralOutputLimits(-INFINITY, 0.12f * 1000.0f);
    }
    InsulatedBoiler plant;
    float peak = 0.0f;
    fakeMillis = 1000;
    for (int t = 0; t < 900; t++) {
        input = plant.sensor;
        fakeMillis += 1000;
        pid.update();
        plant.step(output / 1000.0f);
        peak = std::max(peak, plant.sensor);
    }
    return peak - setpoint;
}
} // namespace

void test_integral_shaping_reduces_heatup_overshoot() {
    const float plain = simulateHeatup(false);
    const float shaped = simulateHeatup(true);
    printf("heat-up overshoot: upstream %.2f K, shaped %.2f K\n", plain, shaped);
    TEST_ASSERT_TRUE(plain > 1.0f); // the model reproduces the defect
    TEST_ASSERT_TRUE(shaped < plain * 0.6f);
}

void test_integral_clamp_back_calculates_state() {
    float output = 0.0f, input = 80.0f, setpoint = 94.0f;
    SimplePID pid(&output, &input, &setpoint);
    pid.setSamplingFrequency(1.0f);
    pid.setCtrlOutputLimits(0.0f, 1000.0f);
    pid.setControllerPIDGains(0.0f, 1.0f, 0.0f, 0.0f); // pure I
    pid.reset();
    pid.setMode(SimplePID::Control::automatic);
    pid.setIntegralOutputLimits(-INFINITY, 100.0f);
    fakeMillis = 1000;
    for (int i = 0; i < 50; i++) {
        fakeMillis += 1000;
        pid.update();
    }
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, pid.getIntegralOutput());
    // One step above setpoint must discharge immediately, not first burn off a hidden surplus.
    input = 95.0f;
    fakeMillis += 1000;
    pid.update();
    TEST_ASSERT_TRUE(pid.getIntegralOutput() < 100.0f);
}

void test_frozen_integrator_holds_state() {
    float output = 0.0f, input = 93.0f, setpoint = 94.0f;
    SimplePID pid(&output, &input, &setpoint);
    pid.setSamplingFrequency(1.0f);
    pid.setCtrlOutputLimits(0.0f, 1000.0f);
    pid.setControllerPIDGains(0.0f, 1.0f, 0.0f, 0.0f);
    pid.reset();
    pid.setMode(SimplePID::Control::automatic);
    fakeMillis = 1000;
    for (int i = 0; i < 5; i++) {
        fakeMillis += 1000;
        pid.update();
    }
    const float before = pid.getIntegralOutput();
    pid.setIntegrationFrozen(true);
    input = 90.0f;
    for (int i = 0; i < 20; i++) {
        fakeMillis += 1000;
        pid.update();
    }
    TEST_ASSERT_FLOAT_WITHIN(0.001f, before, pid.getIntegralOutput());
}

void test_negative_error_always_integrates() {
    float output = 0.0f, input = 99.0f, setpoint = 94.0f;
    SimplePID pid(&output, &input, &setpoint);
    pid.setSamplingFrequency(1.0f);
    pid.setCtrlOutputLimits(-1000.0f, 1000.0f);
    pid.setControllerPIDGains(0.0f, 1.0f, 0.0f, 0.0f);
    pid.reset();
    pid.setMode(SimplePID::Control::automatic);
    pid.setIntegralBand(3.0f); // -5 K error is outside |band| but must still integrate
    fakeMillis = 1000;
    fakeMillis += 1000;
    pid.update();
    TEST_ASSERT_TRUE(pid.getIntegralOutput() < 0.0f);
}

// ---------------------------------------------------------------------------
// B — ShotRecorder
// ---------------------------------------------------------------------------

namespace {
// Drives a recorder through 60 s before, `shotS` brewing and 180 s after.
// tempAt(t) gets seconds relative to brew start.
template <typename F> ShotMetrics runShot(F tempAt, int shotS = 30, bool evaluable = true, int preS = 60) {
    ShotRecorder rec;
    unsigned long t = 1000000;
    for (int s = -preS; s < 0; s++)
        rec.sample(t + s * 1000L, tempAt(static_cast<float>(s)), 94.0f);
    rec.brewStart(t, 94.0f, evaluable);
    for (int s = 0; s < shotS; s++)
        rec.sample(t + s * 1000L, tempAt(static_cast<float>(s)), 94.0f);
    rec.brewEnd(t + shotS * 1000L);
    ShotMetrics m;
    for (int s = shotS; s <= shotS + ShotRecorder::POST_WINDOW_S + 1; s++) {
        rec.sample(t + s * 1000L, tempAt(static_cast<float>(s)), 94.0f);
        if (rec.takeResult(m))
            return m;
    }
    return m;
}

// Sag with its minimum ~21 s after start, like shot-21.json, then recovery.
float saggingShot(float t) {
    if (t < 0.0f)
        return 94.0f;
    if (t < 21.0f)
        return 94.0f - 3.0f * (t / 21.0f);
    if (t < 90.0f)
        return 91.0f + 3.2f * ((t - 21.0f) / 69.0f);
    return 94.2f;
}
} // namespace

void test_recorder_measures_dip() {
    const ShotMetrics m = runShot(saggingShot);
    TEST_ASSERT_TRUE(m.valid);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 3.0f, m.dip);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.2f, m.overshoot);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 3.2f, m.spread);
    TEST_ASSERT_FALSE(shotPasses(m, 2.0f));
}

void test_recorder_passes_stable_shot() {
    const ShotMetrics m = runShot([](float t) { return 94.0f + 0.3f * std::sin(t / 20.0f); });
    TEST_ASSERT_TRUE(m.valid);
    TEST_ASSERT_TRUE(shotPasses(m, 2.0f));
}

void test_recorder_skips_utility_and_cold_and_short() {
    TEST_ASSERT_FALSE(runShot(saggingShot, 30, false).valid);      // flush profile
    TEST_ASSERT_FALSE(runShot(saggingShot, 30, true, 10).valid);   // no "before"
    TEST_ASSERT_FALSE(runShot([](float) { return 80.0f; }).valid); // still heating
    TEST_ASSERT_FALSE(runShot(saggingShot, 5).valid);              // flush-length
}

void test_recorder_back_to_back_closes_previous() {
    ShotRecorder rec;
    unsigned long t = 1000000;
    for (int s = -60; s < 0; s++)
        rec.sample(t + s * 1000L, 94.0f, 94.0f);
    rec.brewStart(t, 94.0f, true);
    for (int s = 0; s < 30; s++)
        rec.sample(t + s * 1000L, 93.0f, 94.0f);
    rec.brewEnd(t + 30000);
    for (int s = 30; s < 40; s++)
        rec.sample(t + s * 1000L, 92.0f, 94.0f);
    rec.brewStart(t + 40000, 94.0f, true); // second shot only 10 s after the first ended
    ShotMetrics m;
    TEST_ASSERT_TRUE(rec.takeResult(m));
    TEST_ASSERT_FALSE(m.valid); // dip not yet visible -> must not drive adaptation
}

void test_recorder_mode_change_aborts() {
    ShotRecorder rec;
    unsigned long t = 1000000;
    for (int s = -60; s < 0; s++)
        rec.sample(t + s * 1000L, 94.0f, 94.0f);
    rec.brewStart(t, 94.0f, true);
    for (int s = 0; s < 30; s++)
        rec.sample(t + s * 1000L, 93.0f, 94.0f);
    rec.brewEnd(t + 30000);
    for (int s = 30; s < 100; s++)
        rec.sample(t + s * 1000L, 93.5f, 94.0f);
    rec.sample(t + 100000, 93.5f, 145.0f); // switched to steam: evaluate what we have
    ShotMetrics m;
    TEST_ASSERT_TRUE(rec.takeResult(m));
    TEST_ASSERT_TRUE(m.valid);
    TEST_ASSERT_EQUAL(ShotRecorder::State::IDLE, rec.getState());
}

// ---------------------------------------------------------------------------
// C — adaptation rules
// ---------------------------------------------------------------------------

void test_dip_raises_feedforward() {
    ShotMetrics m;
    m.valid = true;
    m.dip = 3.0f;
    m.overshoot = 0.2f;
    m.spread = 3.2f;
    TuneParams p;
    const Decision d = adaptAfterShot(p, m, 2.0f);
    TEST_ASSERT_TRUE(d.changed);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.15f, d.next.ffScale);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, p.brewKpScale, d.next.brewKpScale);
}

void test_overshoot_without_dip_lowers_feedforward() {
    ShotMetrics m;
    m.valid = true;
    m.dip = 0.1f;
    m.overshoot = 2.5f;
    m.spread = 2.6f;
    const Decision d = adaptAfterShot(TuneParams(), m, 2.0f);
    TEST_ASSERT_TRUE(d.changed);
    TEST_ASSERT_TRUE(d.next.ffScale < 1.0f);
}

void test_overshoot_after_dip_softens_recovery() {
    ShotMetrics m;
    m.valid = true;
    m.dip = 1.0f;
    m.overshoot = 1.6f;
    m.spread = 2.6f;
    TuneParams p;
    const Decision d = adaptAfterShot(p, m, 2.0f);
    TEST_ASSERT_TRUE(d.changed);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, p.ffScale, d.next.ffScale);
    TEST_ASSERT_TRUE(d.next.brewKpScale < p.brewKpScale);
    TEST_ASSERT_TRUE(d.next.brewHoldoffS > p.brewHoldoffS);
}

void test_idle_oscillation_reduces_integral_authority() {
    ShotMetrics m;
    m.valid = true;
    m.idleSpread = 1.5f;
    m.spread = 2.2f;
    TuneParams p;
    const Decision d = adaptAfterShot(p, m, 2.0f);
    TEST_ASSERT_TRUE(d.changed);
    TEST_ASSERT_TRUE(d.next.integralMaxDuty < p.integralMaxDuty);
}

void test_passing_or_invalid_shot_changes_nothing() {
    ShotMetrics m;
    m.valid = true;
    m.dip = 0.8f;
    m.spread = 1.0f;
    TEST_ASSERT_FALSE(adaptAfterShot(TuneParams(), m, 2.0f).changed);
    m.valid = false;
    m.spread = 5.0f;
    m.dip = 5.0f;
    TEST_ASSERT_FALSE(adaptAfterShot(TuneParams(), m, 2.0f).changed);
}

void test_adaptation_respects_limits() {
    ShotMetrics m;
    m.valid = true;
    m.dip = 4.0f;
    m.spread = 4.0f;
    TuneParams p;
    for (int i = 0; i < 50; i++)
        p = adaptAfterShot(p, m, 2.0f).next;
    TEST_ASSERT_FLOAT_WITHIN(0.001f, TuneLimits::FF_MAX, p.ffScale);
    TEST_ASSERT_FALSE(adaptAfterShot(p, m, 2.0f).changed); // at the limit: reported, not looped
}

// ---------------------------------------------------------------------------
// D — HeatupRecorder
// ---------------------------------------------------------------------------

void test_heatup_overshoot_detected_and_corrected() {
    HeatupRecorder rec;
    unsigned long t = 0;
    float temp = 75.0f;
    HeatupMetrics m;
    bool got = false;
    for (int s = 0; s < 1200 && !got; s++, t += 1000) {
        if (temp < 94.0f)
            temp += 0.3f;
        else if (s < 400)
            temp = std::min(96.5f, temp + 0.02f);
        else
            temp = std::max(94.0f, temp - 0.01f);
        rec.sample(t, temp, 94.0f, false);
        got = rec.takeResult(m);
    }
    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 2.5f, m.overshoot);
    TuneParams p;
    const Decision d = adaptAfterHeatup(p, m, 2.0f);
    TEST_ASSERT_TRUE(d.next.approachMaxDuty < p.approachMaxDuty);
    TEST_ASSERT_TRUE(d.next.integralBandC < p.integralBandC);
}

void test_heatup_interrupted_by_brew_is_discarded() {
    HeatupRecorder rec;
    unsigned long t = 0;
    HeatupMetrics m;
    for (int s = 0; s < 100; s++, t += 1000)
        rec.sample(t, 75.0f + s * 0.3f, 94.0f, false);
    for (int s = 0; s < 900; s++, t += 1000) {
        rec.sample(t, 95.0f, 94.0f, s < 30); // a shot right after reaching temperature
        TEST_ASSERT_FALSE(rec.takeResult(m));
    }
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_integral_shaping_reduces_heatup_overshoot);
    RUN_TEST(test_integral_clamp_back_calculates_state);
    RUN_TEST(test_frozen_integrator_holds_state);
    RUN_TEST(test_negative_error_always_integrates);
    RUN_TEST(test_recorder_measures_dip);
    RUN_TEST(test_recorder_passes_stable_shot);
    RUN_TEST(test_recorder_skips_utility_and_cold_and_short);
    RUN_TEST(test_recorder_back_to_back_closes_previous);
    RUN_TEST(test_recorder_mode_change_aborts);
    RUN_TEST(test_dip_raises_feedforward);
    RUN_TEST(test_overshoot_without_dip_lowers_feedforward);
    RUN_TEST(test_overshoot_after_dip_softens_recovery);
    RUN_TEST(test_idle_oscillation_reduces_integral_authority);
    RUN_TEST(test_passing_or_invalid_shot_changes_nothing);
    RUN_TEST(test_adaptation_respects_limits);
    RUN_TEST(test_heatup_overshoot_detected_and_corrected);
    RUN_TEST(test_heatup_interrupted_by_brew_is_discarded);
    return UNITY_END();
}

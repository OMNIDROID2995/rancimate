#ifndef THERMALTUNER_H
#define THERMALTUNER_H

// RANCIMATE: shot-to-shot thermal watchdog for heavily insulated boilers.
//
// Pure C++ (no Arduino), so the evaluation and adaptation rules run in the
// native unit tests. ThermalWatchdogPlugin feeds it 1 Hz samples plus brew
// start/end events and applies the returned parameters.
//
// Idea: the boiler sensor shows a brew ~L = 22 s late, so a single shot cannot
// be "regulated" live. Instead each shot is evaluated as a whole (before,
// during, after) and the shaping parameters are nudged in small, bounded steps
// until the temperature stays inside the configured band.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <vector>

namespace thermal {

// Parameters the watchdog is allowed to adapt. Everything else (wattage,
// thermal model, PID gains) stays the user's.
struct TuneParams {
    float integralBandC = 3.0f;
    float integralMaxDuty = 0.12f;
    float approachBandC = 8.0f;
    float approachMaxDuty = 0.35f;
    float ffScale = 1.0f;
    float brewHoldoffS = 22.0f;
    float brewKpScale = 0.5f;
};

// Bounds every adaptation stays inside.
struct TuneLimits {
    static constexpr float FF_MIN = 0.5f, FF_MAX = 1.6f;
    static constexpr float IMAX_MIN = 0.04f, IMAX_MAX = 0.30f;
    static constexpr float IBAND_MIN = 1.0f, IBAND_MAX = 6.0f;
    static constexpr float APPROACH_MIN = 0.10f, APPROACH_MAX = 0.60f;
    static constexpr float KP_SCALE_MIN = 0.2f, KP_SCALE_MAX = 1.0f;
    static constexpr float HOLDOFF_MIN = 10.0f, HOLDOFF_MAX = 60.0f;
};

struct ShotMetrics {
    bool valid = false;
    const char *invalidReason = "";
    float setpoint = 0.0f;
    float durationS = 0.0f;
    float idleSpread = 0.0f; // max - min in the minute before the shot
    float dip = 0.0f;        // setpoint - min from start to end + lag (>= 0)
    float overshoot = 0.0f;  // max - setpoint from start to end of post window (>= 0)
    float spread = 0.0f;     // max - min over pre + shot + post
    float minTemp = 0.0f;
    float maxTemp = 0.0f;
};

struct HeatupMetrics {
    bool valid = false;
    float setpoint = 0.0f;
    float overshoot = 0.0f; // max - setpoint after the first crossing
};

struct Decision {
    TuneParams next;
    bool changed = false;
    char reason[128] = "";
};

inline float clampf(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }

// --- Recorder -----------------------------------------------------------

class ShotRecorder {
  public:
    static constexpr int PRE_WINDOW_S = 60;   // "before"
    static constexpr int LAG_S = 25;          // sensor dead time + margin: the dip appears up to here
    static constexpr int POST_WINDOW_S = 180; // "after"
    static constexpr int MIN_PRE_SAMPLES = 30;
    static constexpr int MIN_SHOT_S = 10;              // flushes are not shots
    static constexpr int MAX_SHOT_S = 120;             // longer draws are not espresso
    static constexpr float MAX_START_DEVIATION = 5.0f; // machine must be at temperature
    static constexpr float SETPOINT_CHANGE = 0.5f;     // mode change aborts the post window
    static constexpr size_t MAX_SAMPLES = 600;

    enum class State { IDLE, BREWING, POST };

    // One sample per second. setpoint <= 0 (standby) clears the "before" buffer.
    void sample(unsigned long tMs, float temperature, float setpoint) {
        if (!std::isfinite(temperature) || temperature <= 0.0f)
            return;
        switch (state) {
        case State::IDLE:
            if (setpoint <= 0.0f) {
                pre.clear();
                return;
            }
            pre.push_back({tMs, temperature});
            while (pre.size() > static_cast<size_t>(PRE_WINDOW_S))
                pre.pop_front();
            return;
        case State::BREWING:
            push(tMs, temperature);
            return;
        case State::POST:
            if (setpoint <= 0.0f || std::fabs(setpoint - refSetpoint) > SETPOINT_CHANGE) {
                finish(tMs, true);
                return;
            }
            push(tMs, temperature);
            if (tMs - endMs >= static_cast<unsigned long>(POST_WINDOW_S) * 1000UL)
                finish(tMs, false);
            return;
        }
    }

    // `evaluable` is false for utility profiles (flush, cleaning).
    void brewStart(unsigned long tMs, float setpoint, bool evaluable) {
        if (state != State::IDLE)
            finish(tMs, true); // back-to-back shot: close the previous one early
        startMs = tMs;
        refSetpoint = setpoint;
        skip = !evaluable;
        shot.assign(pre.begin(), pre.end());
        preCount = pre.size();
        state = State::BREWING;
    }

    void brewEnd(unsigned long tMs) {
        if (state != State::BREWING)
            return;
        endMs = tMs;
        state = State::POST;
    }

    State getState() const { return state; }

    // Returns true once per finished shot; `out` then holds the metrics.
    bool takeResult(ShotMetrics &out) {
        if (!hasResult)
            return false;
        out = result;
        hasResult = false;
        return true;
    }

  private:
    struct Sample {
        unsigned long tMs;
        float temperature;
    };

    void push(unsigned long tMs, float temperature) {
        if (shot.size() < MAX_SAMPLES)
            shot.push_back({tMs, temperature});
    }

    void finish(unsigned long tMs, bool early) {
        result = evaluate(tMs, early);
        hasResult = true;
        state = State::IDLE;
        pre.clear(); // the next "before" window must not contain this shot
        shot.clear();
    }

    ShotMetrics evaluate(unsigned long tMs, bool early) const {
        ShotMetrics m;
        m.setpoint = refSetpoint;
        const unsigned long end = endMs > startMs ? endMs : tMs;
        m.durationS = static_cast<float>(end - startMs) / 1000.0f;
        if (skip) {
            m.invalidReason = "utility profile";
            return m;
        }
        if (preCount < static_cast<size_t>(MIN_PRE_SAMPLES)) {
            m.invalidReason = "no stable phase before the shot";
            return m;
        }
        if (m.durationS < MIN_SHOT_S || m.durationS > MAX_SHOT_S) {
            m.invalidReason = "shot length outside 10-120 s";
            return m;
        }
        if (early && tMs - end < static_cast<unsigned long>(LAG_S + 30) * 1000UL) {
            m.invalidReason = "interrupted before the dip was visible";
            return m;
        }
        float preMin = INFINITY, preMax = -INFINITY, lagMin = INFINITY, postMax = -INFINITY;
        float allMin = INFINITY, allMax = -INFINITY;
        for (size_t i = 0; i < shot.size(); i++) {
            const Sample &s = shot[i];
            allMin = std::min(allMin, s.temperature);
            allMax = std::max(allMax, s.temperature);
            if (i < preCount) {
                preMin = std::min(preMin, s.temperature);
                preMax = std::max(preMax, s.temperature);
                continue;
            }
            postMax = std::max(postMax, s.temperature);
            if (s.tMs <= end + static_cast<unsigned long>(LAG_S) * 1000UL)
                lagMin = std::min(lagMin, s.temperature);
        }
        if (std::fabs(preMin - refSetpoint) > MAX_START_DEVIATION || std::fabs(preMax - refSetpoint) > MAX_START_DEVIATION) {
            m.invalidReason = "machine was not at temperature";
            return m;
        }
        if (!std::isfinite(lagMin) || !std::isfinite(postMax)) {
            m.invalidReason = "no samples during the shot";
            return m;
        }
        m.valid = true;
        m.idleSpread = preMax - preMin;
        m.dip = std::max(0.0f, refSetpoint - lagMin);
        m.overshoot = std::max(0.0f, postMax - refSetpoint);
        m.minTemp = allMin;
        m.maxTemp = allMax;
        m.spread = allMax - allMin;
        return m;
    }

    State state = State::IDLE;
    std::deque<Sample> pre;
    std::vector<Sample> shot;
    size_t preCount = 0;
    unsigned long startMs = 0;
    unsigned long endMs = 0;
    float refSetpoint = 0.0f;
    bool skip = false;
    bool hasResult = false;
    ShotMetrics result;
};

// Detects a heat-up (cold start, wake from standby, steam -> brew) and measures
// its overshoot: the original defect of the insulated boiler.
class HeatupRecorder {
  public:
    static constexpr float ARM_DISTANCE = 5.0f;       // at least this far below setpoint
    static constexpr unsigned long WATCH_MS = 600000; // overshoot peak arrives ~5 min after crossing

    void sample(unsigned long tMs, float temperature, float setpoint, bool brewing) {
        if (!std::isfinite(temperature) || setpoint <= 0.0f || brewing || (armed && std::fabs(setpoint - armedSetpoint) > 0.5f)) {
            armed = crossed = false; // a shot or a setpoint change spoils the measurement
            if (setpoint > 0.0f && !brewing && setpoint - temperature >= ARM_DISTANCE)
                arm(setpoint);
            return;
        }
        if (!armed) {
            if (setpoint - temperature >= ARM_DISTANCE)
                arm(setpoint);
            return;
        }
        if (!crossed) {
            if (temperature >= setpoint) {
                crossed = true;
                crossedMs = tMs;
                peak = temperature;
            }
            return;
        }
        peak = std::max(peak, temperature);
        if (tMs - crossedMs >= WATCH_MS) {
            result.valid = true;
            result.setpoint = armedSetpoint;
            result.overshoot = std::max(0.0f, peak - armedSetpoint);
            hasResult = true;
            armed = crossed = false;
        }
    }

    bool takeResult(HeatupMetrics &out) {
        if (!hasResult)
            return false;
        out = result;
        hasResult = false;
        return true;
    }

  private:
    void arm(float setpoint) {
        armed = true;
        crossed = false;
        armedSetpoint = setpoint;
    }

    bool armed = false;
    bool crossed = false;
    float armedSetpoint = 0.0f;
    unsigned long crossedMs = 0;
    float peak = 0.0f;
    bool hasResult = false;
    HeatupMetrics result;
};

// --- Adaptation rules ------------------------------------------------------

inline bool shotPasses(const ShotMetrics &m, float threshold) { return m.valid && m.spread <= threshold; }

// One adaptation per failed shot, small bounded steps. Dip and overshoot are
// never corrected in the same step so the rules cannot fight each other.
inline Decision adaptAfterShot(const TuneParams &cur, const ShotMetrics &m, float threshold) {
    Decision d;
    d.next = cur;
    if (!m.valid || shotPasses(m, threshold))
        return d;
    const float half = threshold * 0.5f;
    int n = 0;
    // Only a value that actually moved counts as a change; a parameter parked at
    // its limit must not be reported as "adjusted" on every shot.
    auto note = [&](const char *fmt, float a, float b) {
        if (std::fabs(a - b) > 1e-4f && n < static_cast<int>(sizeof(d.reason)) - 1)
            n += std::snprintf(d.reason + n, sizeof(d.reason) - n, fmt, a, b);
    };

    if (m.dip > half && m.dip >= m.overshoot) {
        // Shot sagged: the feedforward delivered too little energy.
        const float step = clampf(0.05f * m.dip, 0.03f, 0.15f);
        d.next.ffScale = clampf(cur.ffScale + step, TuneLimits::FF_MIN, TuneLimits::FF_MAX);
        note("FF %.2f->%.2f (dip); ", cur.ffScale, d.next.ffScale);
    } else if (m.overshoot > half) {
        if (m.dip < 0.5f) {
            // Rose without a preceding dip: the feedforward over-delivered.
            const float step = clampf(0.05f * m.overshoot, 0.03f, 0.15f);
            d.next.ffScale = clampf(cur.ffScale - step, TuneLimits::FF_MIN, TuneLimits::FF_MAX);
            note("FF %.2f->%.2f (overshoot); ", cur.ffScale, d.next.ffScale);
        } else {
            // Overshoot after a dip: the feedback recovered too hard on the late sensor signal.
            d.next.brewKpScale = clampf(cur.brewKpScale * 0.85f, TuneLimits::KP_SCALE_MIN, TuneLimits::KP_SCALE_MAX);
            d.next.brewHoldoffS = clampf(cur.brewHoldoffS + 5.0f, TuneLimits::HOLDOFF_MIN, TuneLimits::HOLDOFF_MAX);
            note("brewKp %.2f->%.2f, ", cur.brewKpScale, d.next.brewKpScale);
            note("holdoff %.0f->%.0f s; ", cur.brewHoldoffS, d.next.brewHoldoffS);
        }
    }
    if (m.idleSpread > half) {
        // Oscillating while idle: the integral has too much authority.
        d.next.integralMaxDuty = clampf(cur.integralMaxDuty * 0.85f, TuneLimits::IMAX_MIN, TuneLimits::IMAX_MAX);
        note("Imax %.0f->%.0f %% (idle); ", cur.integralMaxDuty * 100.0f, d.next.integralMaxDuty * 100.0f);
    }
    d.changed = n > 0;
    return d;
}

inline Decision adaptAfterHeatup(const TuneParams &cur, const HeatupMetrics &m, float threshold) {
    Decision d;
    d.next = cur;
    if (!m.valid)
        return d;
    int n = 0;
    if (m.overshoot > threshold * 0.5f) {
        d.next.approachMaxDuty = clampf(cur.approachMaxDuty * 0.85f, TuneLimits::APPROACH_MIN, TuneLimits::APPROACH_MAX);
        n += std::snprintf(d.reason + n, sizeof(d.reason) - n, "approach %.0f->%.0f %% (heat-up +%.1f K); ",
                           cur.approachMaxDuty * 100.0f, d.next.approachMaxDuty * 100.0f, m.overshoot);
        if (m.overshoot > threshold) {
            d.next.integralBandC = clampf(cur.integralBandC * 0.85f, TuneLimits::IBAND_MIN, TuneLimits::IBAND_MAX);
            n += std::snprintf(d.reason + n, sizeof(d.reason) - n, "Iband %.1f->%.1f K; ", cur.integralBandC,
                               d.next.integralBandC);
        }
    } else if (m.overshoot < 0.2f) {
        // Clean heat-up: give back a little speed.
        d.next.approachMaxDuty = clampf(cur.approachMaxDuty * 1.05f, TuneLimits::APPROACH_MIN, TuneLimits::APPROACH_MAX);
        if (d.next.approachMaxDuty != cur.approachMaxDuty)
            n += std::snprintf(d.reason + n, sizeof(d.reason) - n, "approach %.0f->%.0f %% (clean heat-up); ",
                               cur.approachMaxDuty * 100.0f, d.next.approachMaxDuty * 100.0f);
    }
    d.changed = n > 0;
    return d;
}

} // namespace thermal

#endif // THERMALTUNER_H

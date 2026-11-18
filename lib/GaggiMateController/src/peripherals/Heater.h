#ifndef HEATER_H
#define HEATER_H
#include "Autotune/Autotune.h"
#include "Max31855Thermocouple.h"
#include "TemperatureSensor.h"
#include <SimplePID/SimplePID.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <memory>

constexpr float MAX_AUTOTUNE_TEMP = 125.0f;
constexpr float TUNER_OUTPUT_SPAN = 1000.0f;

using heater_error_callback_t = std::function<void()>;
using heater_autotune_fail_callback_t = std::function<void()>;
// Kff = combinedKff = output units per watt (disturbance feedforward), derived
// in Heater::loopAutotune as 1000 / heaterWattage when wattage > 0; 0 when the
// caller didn't supply a wattage.
using pid_result_callback_t = std::function<void(float Kp, float Ki, float Kd, float Kff)>;

class HeaterCoordinator;

// RANCIMATE: advanced control for heavily insulated boilers. Every field is
// only honoured while `enabled` is true; disabled = exact upstream behaviour.
// Values <= 0 mean "feature off" (or "keep controller default" for the
// thermal model), so each building block can be measured on its own.
struct ThermalShaping {
    bool enabled = false;
    float integralBandC = 0.0f;      // integrate only while error <= band (K)
    float integralMaxDuty = 0.0f;    // cap on the integral contribution, 0..1
    float approachBandC = 0.0f;      // taper the duty cap within this band below setpoint (K)
    float approachMaxDuty = 1.0f;    // duty cap reached at setpoint, 0..1
    float incomingWaterTempC = 0.0f; // feedforward model; <= 0 keeps 23 C
    float heatLossWatts = 0.0f;      // <= 0 keeps 5 W
    float heaterEfficiency = 0.0f;   // <= 0 keeps 0.95
    float ffScale = 1.0f;            // multiplier on the disturbance feedforward
    float brewHoldoffS = 0.0f;       // freeze I while brewing and this long after; 0 = off
    float brewKpScale = 1.0f;        // P multiplier while brewing / in hold-off
};

enum class BrewThermalState : uint8_t { IDLE, BREWING, HOLDOFF };

class Heater {
  public:
    Heater(TemperatureSensor *sensor, uint8_t heaterPin, const heater_error_callback_t &error_callback,
           const pid_result_callback_t &pid_callback, const heater_autotune_fail_callback_t &autotune_fail_callback = nullptr);
    void setup();
    void loop();

    void setSetpoint(float setpoint);
    float getSetpoint() { return setpoint; };
    float getDutyCycle() const { return output / TUNER_OUTPUT_SPAN * 100.0f; }
    void setTunings(float Kp, float Ki, float Kd);
    void autotune(int testTimeSec, int windowSize, int heaterWattage);
    bool isAutotuning() const { return autotuning; }
    // Safety off: clears the setpoint and aborts a running autotune.
    void stop();
    // Must be attached before setup() starts the heater task.
    void setCoordinator(HeaterCoordinator *coordinator) { this->coordinator = coordinator; }

    // Thermal feedforward control
    void setThermalFeedforward(float *pumpFlowPtr = nullptr, float incomingWaterTemp = 23.0f, int *valveStatusPtr = nullptr);
    void setFeedforwardScale(float combinedKff); // Set combined Kff value (output units per watt)
    void setThermalShaping(const ThermalShaping &shaping);
    BrewThermalState getBrewThermalState() const { return brewState; }

  private:
    void setupPid();
    void setupAutotune(int testTimeSec, int windowSize, int heaterWattage);
    void loopPid();
    void loopAutotune();
    float softPwm(uint32_t windowSize);
    void requestOutput(bool on);
    void writeOutput(bool on);
    void plot(float optimumOutput, float outputScale, uint8_t everyNth);
    float calculateDisturbanceFeedforwardGain();
    float calculateSafetyScaling(float tempError);
    bool isDrawing() const;
    void updateBrewState(bool drawing);
    float computeOutputCap(bool drawing) const;
    void applyThermalModel();
    TemperatureSensor *sensor;
    uint8_t heaterPin;
    xTaskHandle taskHandle;
    std::unique_ptr<SimplePID> simplePid;
    std::unique_ptr<Autotune> autotuner;

    heater_error_callback_t error_callback;
    pid_result_callback_t pid_callback;
    heater_autotune_fail_callback_t autotune_fail_callback;
    HeaterCoordinator *coordinator = nullptr;

    float temperature = 0.0f;
    float output = 0.0f;
    float setpoint = 0.0f;
    int plotCount = 0;

    unsigned long windowStartTime = 0;

    // Autotune variables
    std::atomic<bool> autotuning{false};
    // Stashed at autotune-start (BLE field 3) so loopAutotune can derive
    // combinedKff = 1000 / wattage on completion. 0 ⇒ caller didn't supply
    // wattage (older display firmware) ⇒ skip combinedKff derivation.
    int autotuneHeaterWattage = 0;

    // Thermal feedforward variables
    float *pumpFlowRate = nullptr;
    int *valveStatus = nullptr;
    float lastSafetyFactor = 1.0f; // For smooth safety scaling transitions
    float incomingWaterTemp = 23.0f;
    float heaterEfficiency = 0.95f;       // 95% efficiency (immersion heater)
    float heatLossWatts = 5.0f;           // 5W heat loss (well-insulated boiler)
    float combinedKff = 0.0f;             // Combined feedforward gain (output units per watt) - disabled by default
    float setupIncomingWaterTemp = 23.0f; // value from setThermalFeedforward, restored when shaping is off

    // Advanced shaping state (RANCIMATE)
    ThermalShaping shaping;
    BrewThermalState brewState = BrewThermalState::IDLE;
    unsigned long brewStateChanged = 0;
    unsigned long lastDrawing = 0;
    static constexpr unsigned long DRAW_END_DEBOUNCE_MS = 1500; // short valve/flow gaps stay one brew

    // Thermal model constants
    static constexpr float WATER_DENSITY = 1.0f;        // g/ml
    static constexpr float WATER_SPECIFIC_HEAT = 4.18f; // J/(g·°C)

    const char *LOG_TAG = "Heater";
    static void loopTask(void *arg);

    friend class HeaterCoordinator;
};

#endif // HEATER_H

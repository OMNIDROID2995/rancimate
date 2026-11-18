#include "Heater.h"
#include "HeaterCoordinator.h"
#include <Arduino.h>
#include <algorithm>
#include <cmath>

Heater::Heater(TemperatureSensor *sensor, uint8_t heaterPin, const heater_error_callback_t &error_callback,
               const pid_result_callback_t &pid_callback, const heater_autotune_fail_callback_t &autotune_fail_callback)
    : sensor(sensor), heaterPin(heaterPin), taskHandle(nullptr), error_callback(error_callback), pid_callback(pid_callback),
      autotune_fail_callback(autotune_fail_callback) {

    simplePid = std::make_unique<SimplePID>(&output, &temperature, &setpoint);
    autotuner = std::make_unique<Autotune>();
}

void Heater::setup() {
    pinMode(heaterPin, OUTPUT);
    setupPid();
    xTaskCreate(loopTask, "Heater::loop", configMINIMAL_STACK_SIZE * 4, this, 1, &taskHandle);
}

void Heater::setupPid() {
    simplePid->setSamplingFrequency(TUNER_OUTPUT_SPAN / 1000.0f);
    simplePid->setCtrlOutputLimits(0.0f, TUNER_OUTPUT_SPAN);
    simplePid->activateSetPointFilter(false);
    simplePid->activateFeedForward(false);
    simplePid->reset();
}

void Heater::setupAutotune(int testTimeSec, int windowSize, int heaterWattage) {
    // SIMC rebuild: first BLE arg = test-duration seconds (see Autotune.h),
    // not legacy phase-margin aggressiveness. Slope threshold + confirmation
    // count owned by Autotune.h defaults (0.1 °C/s, 5 confirms) — quantisation
    // resilience. Wattage stashed for loopAutotune → combinedKff = 1000 / W.
    autotuner->setWindowsize(windowSize > 0 ? windowSize : 6);
    autotuner->setTimeOut(static_cast<float>(testTimeSec > 0 ? testTimeSec : 120));
    autotuneHeaterWattage = heaterWattage > 0 ? heaterWattage : 0;
    autotuner->reset();
}

void Heater::loop() {
    if (!sensor->isErrorState() && autotuning) {
        loopAutotune();
        return;
    }

    if (sensor->isErrorState() || setpoint <= 0.0f) {
        simplePid->setMode(SimplePID::Control::manual);
        requestOutput(false);
        temperature = sensor->read();
        return;
    }
    simplePid->setMode(SimplePID::Control::automatic);

    loopPid();
}

void Heater::setSetpoint(float setpoint) {
    if (this->setpoint != setpoint) {
        this->setpoint = setpoint;
        ESP_LOGV(LOG_TAG, "Set setpoint %f°C", setpoint);
    }
}

void Heater::setTunings(float Kp, float Ki, float Kd) {
    if (simplePid->getKp() != Kp || simplePid->getKi() != Ki || simplePid->getKd() != Kd) {
        simplePid->setControllerPIDGains(Kp, Ki, Kd, 0.0f);
        simplePid->reset();
        ESP_LOGV(LOG_TAG, "Set tunings to Kp: %f, Ki: %f, Kd: %f", Kp, Ki, Kd);
    }
}

void Heater::stop() {
    autotuning = false;
    setSetpoint(0.0f);
    requestOutput(false); // cut the relay now instead of on the heater task's next tick
}

void Heater::setThermalFeedforward(float *pumpFlowPtr, float incomingWaterTemp, int *valveStatusPtr) {
    pumpFlowRate = pumpFlowPtr;
    valveStatus = valveStatusPtr;
    setupIncomingWaterTemp = incomingWaterTemp;
    applyThermalModel(); // shaping (if enabled) keeps precedence over the setup literal

    ESP_LOGI(LOG_TAG, "Thermal feedforward setup - incoming water temp: %.1f°C, valve tracking: %s", incomingWaterTemp,
             valveStatusPtr ? "enabled" : "disabled");
    ESP_LOGI(LOG_TAG, "Feedforward will be %s based on Kff value (currently %.3f)", combinedKff > 0.0f ? "ENABLED" : "DISABLED",
             combinedKff);
}

void Heater::setFeedforwardScale(float combinedKff) {
    this->combinedKff = combinedKff;
    ESP_LOGI(LOG_TAG, "Combined feedforward gain (Kff) set to: %.3f output units per watt", combinedKff);
}

void Heater::setThermalShaping(const ThermalShaping &newShaping) {
    shaping = newShaping;
    if (!std::isfinite(shaping.ffScale) || shaping.ffScale <= 0.0f)
        shaping.ffScale = 1.0f;
    shaping.ffScale = std::min(shaping.ffScale, 3.0f);
    if (!std::isfinite(shaping.brewKpScale) || shaping.brewKpScale <= 0.0f)
        shaping.brewKpScale = 1.0f;
    shaping.brewKpScale = std::min(shaping.brewKpScale, 2.0f);
    if (!(shaping.approachMaxDuty > 0.0f) || shaping.approachMaxDuty > 1.0f)
        shaping.approachMaxDuty = 1.0f;

    if (shaping.enabled) {
        simplePid->setIntegralBand(std::max(0.0f, shaping.integralBandC));
        // 0 = unlimited; otherwise a share of TUNER_OUTPUT_SPAN. The lower bound
        // stays open: a negative integral is how an over-temperature discharges.
        simplePid->setIntegralOutputLimits(
            -INFINITY, shaping.integralMaxDuty > 0.0f ? std::min(shaping.integralMaxDuty, 1.0f) * TUNER_OUTPUT_SPAN : 0.0f);
    } else {
        simplePid->setIntegralBand(0.0f);
        simplePid->setIntegralOutputLimits(-INFINITY, INFINITY);
        simplePid->setIntegrationFrozen(false);
        simplePid->setProportionalScale(1.0f);
        simplePid->setCtrlOutputLimits(0.0f, TUNER_OUTPUT_SPAN);
        brewState = BrewThermalState::IDLE;
    }
    applyThermalModel();
    ESP_LOGI(LOG_TAG,
             "Thermal shaping %s: Iband=%.1fK Imax=%.0f%% approach=%.1fK/%.0f%% inlet=%.1fC loss=%.1fW eff=%.2f "
             "ff=%.2f holdoff=%.0fs brewKp=%.2f",
             shaping.enabled ? "ON" : "off", shaping.integralBandC, shaping.integralMaxDuty * 100.0f, shaping.approachBandC,
             shaping.approachMaxDuty * 100.0f, incomingWaterTemp, heatLossWatts, heaterEfficiency, shaping.ffScale,
             shaping.brewHoldoffS, shaping.brewKpScale);
}

void Heater::applyThermalModel() {
    const bool on = shaping.enabled;
    incomingWaterTemp = on && shaping.incomingWaterTempC > 0.0f ? shaping.incomingWaterTempC : setupIncomingWaterTemp;
    heatLossWatts = on && shaping.heatLossWatts > 0.0f ? shaping.heatLossWatts : 5.0f;
    heaterEfficiency =
        on && shaping.heaterEfficiency > 0.0f && shaping.heaterEfficiency <= 1.0f ? shaping.heaterEfficiency : 0.95f;
}

// Same condition that gates the disturbance feedforward, so cap and feedforward never fight.
bool Heater::isDrawing() const {
    return pumpFlowRate != nullptr && *pumpFlowRate > 0.01f && valveStatus != nullptr && *valveStatus != 0;
}

// IDLE -> BREWING while water is drawn -> HOLDOFF for brewHoldoffS after the draw
// ends -> IDLE. The sensor sees a brew ~L (22 s) late, mostly after the shot; while
// BREWING/HOLDOFF the integral is frozen so it does not charge on a disturbance the
// feedforward has already paid for.
void Heater::updateBrewState(bool drawing) {
    const unsigned long now = millis();
    if (drawing)
        lastDrawing = now;
    if (!shaping.enabled || shaping.brewHoldoffS <= 0.0f) {
        brewState = BrewThermalState::IDLE;
        return;
    }
    switch (brewState) {
    case BrewThermalState::IDLE:
        if (drawing) {
            brewState = BrewThermalState::BREWING;
            brewStateChanged = now;
        }
        break;
    case BrewThermalState::BREWING:
        if (!drawing && now - lastDrawing > DRAW_END_DEBOUNCE_MS) {
            brewState = BrewThermalState::HOLDOFF;
            brewStateChanged = now;
        }
        break;
    case BrewThermalState::HOLDOFF:
        if (drawing) {
            brewState = BrewThermalState::BREWING;
            brewStateChanged = now;
        } else if (now - brewStateChanged > static_cast<unsigned long>(shaping.brewHoldoffS * 1000.0f)) {
            brewState = BrewThermalState::IDLE;
            brewStateChanged = now;
        }
        break;
    }
}

// Output ceiling: 100 % outside the approach band, tapering linearly down to
// approachMaxDuty at setpoint and above. Never capped while water is drawn: the
// temperature is then typically inside the band, but the feedforward needs ~57 %.
float Heater::computeOutputCap(bool drawing) const {
    if (!shaping.enabled || shaping.approachBandC <= 0.0f || shaping.approachMaxDuty >= 1.0f || drawing) {
        return TUNER_OUTPUT_SPAN;
    }
    const float distance = setpoint - temperature;
    if (distance >= shaping.approachBandC) {
        return TUNER_OUTPUT_SPAN;
    }
    const float ratio = distance > 0.0f ? distance / shaping.approachBandC : 0.0f;
    return (shaping.approachMaxDuty + (1.0f - shaping.approachMaxDuty) * ratio) * TUNER_OUTPUT_SPAN;
}

void Heater::autotune(int testTimeSec, int windowSize, int heaterWattage) {
    setupAutotune(testTimeSec, windowSize, heaterWattage);
    autotuning = true;
}

void Heater::loopPid() {
    softPwm(TUNER_OUTPUT_SPAN);
    temperature = sensor->read();
    const bool drawing = isDrawing();
    updateBrewState(drawing);

    // Calculate and set disturbance feedforward BEFORE PID update
    // Only apply thermal feedforward when Kf>0, valve is open, and water is flowing
    if (combinedKff > 0.0f && drawing) {
        float currentFlowRate = *pumpFlowRate; // Use raw flow rate for fast response
        float disturbanceGain = calculateDisturbanceFeedforwardGain();

        // Apply smoothed temperature-based safety scaling
        float tempError = temperature - setpoint;
        float rawSafetyFactor = calculateSafetyScaling(tempError);

        // Smooth safety factor transitions to reduce oscillations
        const float safetyAlpha = 0.85f; // Faster response for quicker feedforward
        float safetyFactor = safetyAlpha * rawSafetyFactor + (1.0f - safetyAlpha) * lastSafetyFactor;
        lastSafetyFactor = safetyFactor;

        disturbanceGain *= safetyFactor;
        if (shaping.enabled)
            disturbanceGain *= shaping.ffScale;

        // Set the disturbance feedforward in SimplePID
        simplePid->setDisturbanceFeedforward(currentFlowRate, disturbanceGain);

    } else {
        simplePid->setDisturbanceFeedforward(0.0f, 0.0f);
    }

    if (shaping.enabled) {
        const bool brewing = brewState != BrewThermalState::IDLE;
        simplePid->setIntegrationFrozen(brewing);
        simplePid->setProportionalScale(brewing ? shaping.brewKpScale : 1.0f);
        // Ceiling BEFORE update() so the existing anti-windup sees the real
        // saturation limit; clamping `output` afterwards would cause windup.
        simplePid->setCtrlOutputLimits(0.0f, computeOutputCap(drawing));
    }

    // Now run PID with proper feedforward integrated
    bool pidUpdated = simplePid->update();

    if (pidUpdated) {
        plot(output, 1.0f, 1);
    }
}

void Heater::loopAutotune() {
    simplePid->setMode(SimplePID::Control::manual);
    autotuner->reset();
    long microseconds;
    long loopInterval = (static_cast<long>(TUNER_OUTPUT_SPAN) - 1L) * 1000L;
    while (autotuning && !autotuner->isFinished()) {
        microseconds = micros();
        // Re-check sensor every iteration. Heater::loop entry gate sampled
        // once — mid-test fault would leave relay stuck at full power for
        // rest of window. Max31855Thermocouple::read() returns 0 on fault;
        // neither overtemp guard nor autotuner state machine detects it.
        if (sensor->isErrorState()) {
            output = 0.0f;
            autotuning = false;
            softPwm(TUNER_OUTPUT_SPAN);
            ESP_LOGE(LOG_TAG, "Autotune aborted: sensor fault mid-test");
            if (error_callback) {
                error_callback();
            }
            return;
        }
        temperature = sensor->read();
        output = 0.0f;
        if (autotuner->maxPowerOn) {
            output = TUNER_OUTPUT_SPAN;
        }
        ESP_LOGI(LOG_TAG, "Autotuner Cycle: Temperature=%.2f", temperature);
        autotuner->update(temperature, millis() / 1000.0f);
        while (autotuning && micros() - microseconds < loopInterval) {
            softPwm(TUNER_OUTPUT_SPAN);
            vTaskDelay(1 / portTICK_PERIOD_MS);
        }
        if (temperature > MAX_AUTOTUNE_TEMP) {
            output = 0.0f;
            autotuning = false;
            softPwm(TUNER_OUTPUT_SPAN);
            // Overtemp abort. Preserve NVS gains — skip pid_callback. Surface
            // as runaway so display drops machine into standby.
            ESP_LOGE(LOG_TAG, "Autotune aborted: temperature %.1f°C exceeds %.1f°C", temperature, MAX_AUTOTUNE_TEMP);
            if (error_callback) {
                error_callback();
            }
            return;
        }
    }
    output = 0.0f;
    softPwm(TUNER_OUTPUT_SPAN);
    if (!autotuning.exchange(false)) {
        ESP_LOGW(LOG_TAG, "Autotune aborted by safety stop");
        return;
    }

    if (autotuner->isTimedOut()) {
        // Reaction/inflection never detected in window. Keep NVS PID — never
        // call pid_callback with zeros (#672 crash). Use dedicated
        // autotune-fail callback (→ ERROR_CODE_AUTOTUNE_TIMEOUT) so display
        // doesn't mistake it for runaway + force pump/valve off.
        // getTimeOut() = configured window. getSystemDelay() only set on
        // success path — reads 0 here.
        ESP_LOGW(LOG_TAG, "Autotune timed out (no inflection within %.1f s) — gains preserved", autotuner->getTimeOut());
        if (autotune_fail_callback) {
            autotune_fail_callback();
        }
        return;
    }

    // Disturbance feedforward (output units per watt of heat loss). With
    // TUNER_OUTPUT_SPAN=1000 and a known heater wattage, "1 watt of
    // compensation maps to (1000 / wattage) PWM duty units". 0 when wattage
    // wasn't supplied — preserves prior no-disturbance-FF behaviour.
    // Variable name avoids shadowing the Heater member combinedKff (Sonar
    // cpp:S1117).
    const float kffFromWattage = autotuneHeaterWattage > 0 ? TUNER_OUTPUT_SPAN / static_cast<float>(autotuneHeaterWattage) : 0.0f;

    pid_callback(autotuner->getKp() * 1000.0f, autotuner->getKi() * 1000.0f, autotuner->getKd() * 1000.0f, kffFromWattage);

    setTunings(autotuner->getKp() * 1000.0f, autotuner->getKi() * 1000.0f, autotuner->getKd() * 1000.0f);
    setFeedforwardScale(kffFromWattage);

    ESP_LOGI(LOG_TAG, "Autotuning finished: Kp=%.4f, Ki=%.4f, Kd=%.4f, Kff=%.4f", autotuner->getKp() * 1000.0f,
             autotuner->getKi() * 1000.0f, autotuner->getKd() * 1000.0f, kffFromWattage);
    // Setpoint-FF (SIMC's 1/k') logged for diagnostics — currently unrouted
    // (SimplePID gainFF stays 0); useful for sanity-checking the identifier
    // without affecting the runtime FF path that uses kffFromWattage above.
    ESP_LOGI(LOG_TAG, "SIMC params: L=%.2fs k'=%.4f°C/s tau2=%.2fs fc=%.4fHz wattage=%dW setpoint_FF=%.2f",
             autotuner->getSystemDelay(), autotuner->getSystemGain(), autotuner->getSystemTau2(), autotuner->getCrossoverFreq(),
             autotuneHeaterWattage, autotuner->getKff() * 1000.0f);
}

float Heater::softPwm(uint32_t windowSize) {
    // software PWM timer
    unsigned long msNow = millis();
    if (msNow - windowStartTime >= windowSize) {
        windowStartTime = msNow;
    }
    float optimumOutput = output;

    requestOutput(static_cast<unsigned long>(optimumOutput) > (msNow - windowStartTime));
    return optimumOutput;
}

void Heater::requestOutput(bool on) {
    if (coordinator != nullptr)
        coordinator->request(this, on);
    else
        writeOutput(on);
}

void Heater::writeOutput(bool on) { digitalWrite(heaterPin, on ? HIGH : LOW); }

void Heater::plot(float optimumOutput, float outputScale, uint8_t everyNth) {
    if (plotCount >= everyNth) {
        plotCount = 1;
        ESP_LOGI(LOG_TAG, "PID Plot: output=%.2f, input=%.2f, setpoint=%.2f", optimumOutput * outputScale, temperature, setpoint);
    } else
        plotCount++;
}

float Heater::calculateDisturbanceFeedforwardGain() {
    if (combinedKff <= 0.0f || !pumpFlowRate || *pumpFlowRate <= 0.01f) {
        return 0.0f;
    }

    float currentFlowRate = *pumpFlowRate; // Use raw flow rate for fast response

    // Calculate temperature difference (target - incoming water temperature)
    float tempDelta = setpoint - incomingWaterTemp;
    if (tempDelta <= 0.0f)
        return 0.0f;

    // Calculate thermal power needed per ml/s of flow (Watts per ml/s)
    float powerPerFlowRate = WATER_DENSITY * WATER_SPECIFIC_HEAT * tempDelta + (heatLossWatts / currentFlowRate);
    powerPerFlowRate /= heaterEfficiency;

    // Apply combined Kff directly (output units per watt)
    float gainPerFlowRate = powerPerFlowRate * combinedKff;

    return gainPerFlowRate;
}

float Heater::calculateSafetyScaling(float tempError) {
    // tempError = temperature - setpoint
    if (shaping.enabled) {
        // RANCIMATE fix: the upstream curve below jumps from 1.0 at exactly 0 K to
        // 0.7 just below setpoint, so a shot started 0.3 K low only got ~80 % of
        // its feedforward. Full FF at or below setpoint, linear back-off to 0 at +1 K.
        if (tempError <= 0.0f)
            return 1.0f;
        if (tempError >= 1.0f)
            return 0.0f;
        return 1.0f - tempError;
    }
    // Use smoother, less aggressive safety scaling to reduce oscillations
    if (tempError > 1.0f) {
        return 0.0f; // No FF if more than 1.0°C above setpoint
    } else if (tempError >= 0.0f) {
        // Gradual reduction: 100% at 0°C error, 70% at +1.0°C error
        return 0.7f + 0.3f * (1.0f - tempError / 1.0f);
    } else if (tempError > -1.0f) {
        // Scale from 70% to 100% as temperature drops below setpoint
        return 0.7f + 0.3f * std::abs(tempError) / 1.0f;
    } else {
        return 1.0f; // Full FF when more than 1.0°C below setpoint
    }
}

void Heater::loopTask(void *arg) {
    TickType_t lastWake = xTaskGetTickCount();
    auto *heater = static_cast<Heater *>(arg);
    while (true) {
        heater->loop();
        xTaskDelayUntil(&lastWake, pdMS_TO_TICKS(10));
    }
}

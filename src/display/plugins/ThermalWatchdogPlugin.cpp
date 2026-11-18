#include "ThermalWatchdogPlugin.h"
#include <display/core/Controller.h>
#include <display/core/PluginManager.h>
#include <display/core/process/BrewProcess.h>

namespace {
constexpr unsigned long SAMPLE_INTERVAL_MS = 1000;
constexpr const char *LOG_TAG = "ThermalWatchdog";
} // namespace

void ThermalWatchdogPlugin::setup(Controller *controller, PluginManager *pluginManager) {
    this->controller = controller;
    pluginManager->on("controller:brew:start", [this](Event const &) { onBrewStart(); });
    pluginManager->on("controller:brew:end", [this](Event const &) { onBrewEnd(); });
}

void ThermalWatchdogPlugin::loop() {
    const unsigned long now = millis();
    if (now - lastSample < SAMPLE_INTERVAL_MS)
        return;
    lastSample = now;

    Settings &settings = controller->getSettings();
    std::unique_lock<std::mutex> lock(recorderMutex);
    if (!settings.isThermalAdvancedEnabled() || controller->isAutotuning() || controller->isErrorState()) {
        shotRecorder = thermal::ShotRecorder();
        heatupRecorder = thermal::HeatupRecorder();
        pendingApply = false;
        return;
    }

    // Only the brew boiler setpoint counts; standby/steam/water reset the "before" window.
    const float setpoint = controller->getMode() == MODE_BREW ? controller->getTargetTemp() : 0.0f;
    const float temperature = controller->getCurrentTemp();
    // A shot and its post window belong to the shot evaluation; a deep post-shot dip
    // must not also be scored as a heat-up (that would adapt twice for one event).
    const bool shotInProgress = shotRecorder.getState() != thermal::ShotRecorder::State::IDLE;
    shotRecorder.sample(now, temperature, setpoint);
    heatupRecorder.sample(now, temperature, setpoint, shotInProgress || controller->isActive());

    thermal::ShotMetrics shot;
    const bool haveShot = shotRecorder.takeResult(shot);
    thermal::HeatupMetrics heatup;
    const bool haveHeatup = heatupRecorder.takeResult(heatup);
    lock.unlock();
    if (haveShot)
        handleShot(shot);
    if (haveHeatup)
        handleHeatup(heatup);

    // Never change the heater configuration in the middle of a process.
    if (pendingApply && !controller->isActive()) {
        controller->setHeaterSettings();
        pendingApply = false;
    }
}

void ThermalWatchdogPlugin::onBrewStart() {
    if (!controller->getSettings().isThermalAdvancedEnabled())
        return;
    bool evaluable = false;
    {
        std::lock_guard<std::recursive_mutex> guard(controller->getProcessLock());
        Process *current = controller->getProcess();
        if (current != nullptr && current->getType() == MODE_BREW)
            evaluable = !static_cast<BrewProcess *>(current)->isUtility();
    }
    const float setpoint = controller->getMode() == MODE_BREW ? controller->getTargetTemp() : 0.0f;
    std::lock_guard<std::mutex> lock(recorderMutex);
    shotRecorder.brewStart(millis(), setpoint, evaluable && setpoint > 0.0f);
}

void ThermalWatchdogPlugin::onBrewEnd() {
    std::lock_guard<std::mutex> lock(recorderMutex);
    shotRecorder.brewEnd(millis());
}

void ThermalWatchdogPlugin::handleShot(const thermal::ShotMetrics &m) {
    Settings &settings = controller->getSettings();
    if (!m.valid) {
        ESP_LOGI(LOG_TAG, "Shot not evaluated: %s", m.invalidReason);
        settings.recordThermalWatchdogResult(false, false, 0.0f, String("Shot not evaluated: ") + m.invalidReason);
        return;
    }
    const float threshold = settings.getThermalWatchdogThreshold();
    const bool passed = thermal::shotPasses(m, threshold);
    char summary[112];
    snprintf(summary, sizeof(summary), "Shot %.0f s: spread %.1f K (dip %.1f, overshoot %.1f, idle %.1f). ", m.durationS,
             m.spread, m.dip, m.overshoot, m.idleSpread);
    String action(summary);
    if (passed) {
        action += "Within band.";
    } else if (!settings.isThermalWatchdogEnabled()) {
        action += "Outside band; watchdog off, nothing changed.";
    } else {
        const thermal::Decision decision = thermal::adaptAfterShot(settings.getThermalTuneParams(), m, threshold);
        if (decision.changed) {
            settings.setThermalTuneParams(decision.next);
            pendingApply = true;
            action += String("Adjusted: ") + decision.reason;
        } else {
            action += "Outside band, all parameters at their limits.";
        }
    }
    ESP_LOGI(LOG_TAG, "%s", action.c_str());
    settings.recordThermalWatchdogResult(true, passed, m.spread, action);
}

void ThermalWatchdogPlugin::handleHeatup(const thermal::HeatupMetrics &m) {
    Settings &settings = controller->getSettings();
    char summary[64];
    snprintf(summary, sizeof(summary), "Heat-up overshoot %.1f K. ", m.overshoot);
    String action(summary);
    if (settings.isThermalWatchdogEnabled()) {
        const thermal::Decision decision =
            thermal::adaptAfterHeatup(settings.getThermalTuneParams(), m, settings.getThermalWatchdogThreshold());
        if (decision.changed) {
            settings.setThermalTuneParams(decision.next);
            pendingApply = true;
            action += String("Adjusted: ") + decision.reason;
        } else {
            action += "No change.";
        }
    } else {
        action += "Watchdog off, nothing changed.";
    }
    ESP_LOGI(LOG_TAG, "%s", action.c_str());
    settings.recordThermalWatchdogResult(false, false, 0.0f, action);
}

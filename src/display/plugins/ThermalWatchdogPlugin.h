#ifndef THERMALWATCHDOGPLUGIN_H
#define THERMALWATCHDOGPLUGIN_H

#include <display/core/Plugin.h>
#include <display/core/thermal/ThermalTuner.h>
#include <mutex>

class Controller;
class PluginManager;

// RANCIMATE: "PID advanced for isolated boilers" watchdog.
//
// While the advanced mode is on, every shot is measured before, during and
// after the draw (see thermal::ShotRecorder) and every heat-up is checked for
// overshoot. With the watchdog switch on, failed shots nudge the shaping
// parameters (thermal::adaptAfterShot / adaptAfterHeatup) until the spread
// stays within the configured threshold. With the switch off it only reports.
class ThermalWatchdogPlugin : public Plugin {
  public:
    void setup(Controller *controller, PluginManager *pluginManager) override;
    void loop() override;

  private:
    void onBrewStart();
    void onBrewEnd();
    void handleShot(const thermal::ShotMetrics &metrics);
    void handleHeatup(const thermal::HeatupMetrics &metrics);

    Controller *controller = nullptr;
    thermal::ShotRecorder shotRecorder;
    thermal::HeatupRecorder heatupRecorder;
    unsigned long lastSample = 0;
    bool pendingApply = false;
    std::mutex recorderMutex; // brew events may arrive on another task than loop()
};

#endif // THERMALWATCHDOGPLUGIN_H

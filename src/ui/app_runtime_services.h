#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <huxerui/state.h>

namespace clashflux::ui {

// Application tasks survive retirement and recreation of the main window.
// Guards live in the Runtime, not in process-global once_flags.
struct AppRuntimeServices {
    // Foreground-service notification taps arrive through Android application
    // activation, including when the UI process is started by the tap.
    huxerui::State<std::uint64_t> backgroundSettingsRequests{0};
    std::uint64_t handledBackgroundSettingsRequests = 0;
    bool persistenceStarted = false;
    bool startupActivationHandled = false;
    bool profilesPumpStarted = false;
    bool profileRefreshStarted = false;
    bool externalResourceRefreshStarted = false;
    bool proxiesPumpStarted = false;
    bool corePumpStarted = false;
    bool vpnPumpStarted = false;
    bool settingsPumpStarted = false;
    std::function<void()> showWindow;
    std::function<void()> installAppUpdate;
    huxerui::State<std::string> startupError{std::string{}};
    bool startupErrorReported = false;
};

} // namespace clashflux::ui

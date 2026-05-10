#pragma once
#include <Arduino.h>

namespace netcfg {
    using PortalEnterCb = std::function<void(const String& ssid, const String& password)>;
    using StatusCb      = std::function<void(const String&)>;
    using TickCb        = std::function<void()>;

    // Non-blocking. Call begin() once, then process() in your main loop.
    // While disconnected, runs WiFiManager (saved creds + captive portal).
    void begin(PortalEnterCb onPortal = nullptr, StatusCb onStatus = nullptr);
    void process();         // call frequently from loop()
    String apSsid();
    bool isConnected();
    bool isPortalActive();
}

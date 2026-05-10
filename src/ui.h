#pragma once
#include <Arduino.h>

namespace ui {
    void begin();
    void showSetupHint(const String& ssid, const String& password, const String& help);
    void showConnecting(const String& msg);
    void showWaitingForToken(const String& portalUrl);
    void showOverview();              // creates/refreshes overview screen
    void showIncidents();             // creates/refreshes incidents screen
    void notifyDataRefreshed();       // call after pd::refresh() updates
    void setStatusOnline(bool online);

    enum class Screen { None, Connecting, Setup, Waiting, Overview, Incidents };
    Screen currentScreen();

    // Pending timeline fetch handoff (set by tap, drained by main loop).
    bool   timelineFetchPending(String& outIncidentId);
    void   applyTimeline();   // call after pd::fetchTimeline succeeded; reads pd::lastTimeline()

    // Pending mutation (ack/resolve) handoff.
    bool   actionPending(String& outIncidentId, String& outNewStatus);
    void   applyActionResult(bool ok, const String& msg);

    // Snooze handoff: status="snooze:<seconds>"; note handoff: status="note:<text>"
    bool   snoozePending(String& outIncidentId, int& outSeconds);
    bool   notePending(String& outIncidentId, String& outText);

    // Force-refresh request from pull-down gesture
    bool   refreshRequested();

    // On-call data update
    void   notifyOnCallsRefreshed();   // reads main.cpp's g_uiOnCallsBuffer
    void   notifyMttr(int seconds, int sampleCount);

    // Status bar clock (HH:MM)
    void   setClockText(const String& hhmm);
}

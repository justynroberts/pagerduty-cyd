#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>

#include "config.h"
#include "storage.h"
#include "display.h"
#include "ui.h"
#include "wifi_setup.h"
#include "portal.h"
#include "pagerduty.h"

static uint32_t lastPollMs = 0;
static bool firstPollDone = false;
static bool wasConnected = false;
static bool portalServerStarted = false;
static String portalIpShown;

// Shared with portal.cpp — token validation handoff so the HTTP request returns instantly.
volatile bool g_pdTokenJustSaved = false;
volatile bool g_pdTokenValidated = false;
String        g_pdValidationError;

// Shared with ui.cpp — buffer for pending timeline fetch.
std::vector<pd::LogEntry> g_uiTimelineBuffer;
std::vector<pd::OnCall>   g_uiOnCallsBuffer;
static uint32_t lastOnCallFetchMs = 0;
static uint32_t lastMttrFetchMs = 0;
static uint32_t lastClockTickMs = 0;

// Callbacks fire on the WiFi task (core 0). We must NOT touch LVGL there;
// instead we stash state and the main loop applies it.
static volatile bool   g_uiPortalDirty = false;
static volatile bool   g_uiStatusDirty = false;
static String          g_uiPortalSsid, g_uiPortalPwd;
static String          g_uiStatusMsg;
static portMUX_TYPE    g_uiMux = portMUX_INITIALIZER_UNLOCKED;

static void onPortalEnter(const String& ssid, const String& pwd) {
    Serial.printf("[wifi] AP up: %s / %s\n", ssid.c_str(), pwd.c_str());
    portENTER_CRITICAL(&g_uiMux);
    g_uiPortalSsid = ssid; g_uiPortalPwd = pwd;
    g_uiPortalDirty = true;
    portEXIT_CRITICAL(&g_uiMux);
}
static void onWiFiStatus(const String& msg) {
    Serial.printf("[wifi] %s\n", msg.c_str());
    portENTER_CRITICAL(&g_uiMux);
    g_uiStatusMsg = msg;
    g_uiStatusDirty = true;
    portEXIT_CRITICAL(&g_uiMux);
}

void setup() {
    Serial.begin(115200);
    delay(150);
    Serial.println("\n[boot] PagerDuty CYD");

    pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
    digitalWrite(LED_R, HIGH); digitalWrite(LED_G, HIGH); digitalWrite(LED_B, HIGH);

    storage::begin();

    // One-time: clear stale WiFi creds from earlier dev sessions.
    {
        Preferences p; p.begin("pdcyd_meta", false);
        if (!p.getBool("wifi_wiped_v1", false)) {
            Serial.println("[boot] wiping stale WiFi creds (one-time)");
            WiFi.mode(WIFI_STA);
            WiFi.disconnect(true, true);
            p.putBool("wifi_wiped_v1", true);
        }
        p.end();
    }

    display::begin();
    ui::begin();
    ui::showConnecting("Booting...");
    display::tick();

    netcfg::begin(onPortalEnter, onWiFiStatus);
}

void loop() {
    netcfg::process();

    // Drain UI updates queued by the WiFi task (core 0).
    if (g_uiPortalDirty || g_uiStatusDirty) {
        String ssid, pwd, msg;
        bool portalDirty = false, statusDirty = false;
        portENTER_CRITICAL(&g_uiMux);
        if (g_uiPortalDirty) { ssid = g_uiPortalSsid; pwd = g_uiPortalPwd; portalDirty = true; g_uiPortalDirty = false; }
        if (g_uiStatusDirty) { msg = g_uiStatusMsg; statusDirty = true; g_uiStatusDirty = false; }
        portEXIT_CRITICAL(&g_uiMux);
        if (portalDirty) {
            ui::showSetupHint(ssid, pwd,
                "Join this network from your phone. The captive portal will open automatically.");
        } else if (statusDirty && !netcfg::isConnected() && !netcfg::isPortalActive()) {
            ui::showConnecting(msg);
        }
    }

    display::tick();

    if (netcfg::isConnected()) {
        if (!wasConnected) {
            wasConnected = true;
            Serial.printf("[wifi] connected, ip=%s\n", WiFi.localIP().toString().c_str());
            if (!portalServerStarted) {
                portal::begin();
                portalServerStarted = true;
                if (MDNS.begin("pagerduty")) {
                    MDNS.addService("http", "tcp", 80);
                    Serial.println("[mdns] http://pagerduty.local/");
                } else {
                    Serial.println("[mdns] failed to start");
                }
                // NTP — UTC; user can pick TZ later
                configTime(0, 0, "pool.ntp.org", "time.google.com");
                Serial.println("[ntp] sync started");
            }
        }
        // State-driven screen selection — corrects any race where the
        // initial transition was missed or got overwritten.
        ui::Screen want;
        if (storage::hasToken() && firstPollDone) want = ui::Screen::Overview;
        else if (storage::hasToken())             want = ui::Screen::Overview;       // building / first poll
        else                                      want = ui::Screen::Waiting;
        ui::Screen now = ui::currentScreen();
        if (want == ui::Screen::Waiting && now != ui::Screen::Waiting) {
            ui::showWaitingForToken(String("http://pagerduty.local\n") + WiFi.localIP().toString());
        } else if (want == ui::Screen::Overview && now != ui::Screen::Overview && now != ui::Screen::Incidents) {
            ui::showOverview();
            ui::setStatusOnline(true);
        }
        portal::loop();

        // Clock tick — once per 30s update HH:MM
        if (millis() - lastClockTickMs > 30000 || lastClockTickMs == 0) {
            lastClockTickMs = millis();
            time_t now = time(nullptr);
            if (now > 1700000000) {
                struct tm tmu; gmtime_r(&now, &tmu);
                char buf[8]; strftime(buf, sizeof(buf), "%H:%M", &tmu);
                ui::setClockText(buf);
            }
        }

        // Pull-to-refresh
        if (ui::refreshRequested()) {
            Serial.println("[ui] refresh requested");
            pd::refresh();
            ui::notifyDataRefreshed();
            lastPollMs = millis();
        }

        // On-call refresh (every 5 min, or when on-call screen empty)
        if (storage::hasToken() &&
            (millis() - lastOnCallFetchMs > 300000 || lastOnCallFetchMs == 0)) {
            lastOnCallFetchMs = millis();
            String oerr;
            g_uiOnCallsBuffer.clear();
            if (pd::fetchOnCalls(g_uiOnCallsBuffer, oerr)) {
                ui::notifyOnCallsRefreshed();
            } else {
                Serial.printf("[pd] oncalls failed: %s\n", oerr.c_str());
            }
        }

        // MTTR refresh (every 5 min)
        if (storage::hasToken() &&
            (millis() - lastMttrFetchMs > 300000 || lastMttrFetchMs == 0)) {
            lastMttrFetchMs = millis();
            int mttr = 0; String merr;
            int n = pd::fetchMTTR(mttr, merr, 24);
            ui::notifyMttr(mttr, n);
        }

        // Snooze pending
        {
            String sId; int sSec;
            if (ui::snoozePending(sId, sSec)) {
                String serr;
                bool ok = pd::snoozeIncident(sId, sSec, serr);
                ui::applyActionResult(ok, ok ? String("Snoozed") : ("Fail: " + serr));
                if (ok) { pd::refresh(); ui::notifyDataRefreshed(); lastPollMs = millis(); }
            }
        }
        // Note pending
        {
            String nId, nText;
            if (ui::notePending(nId, nText)) {
                String nerr;
                bool ok = pd::addNote(nId, nText, nerr);
                ui::applyActionResult(ok, ok ? String("Note added") : ("Fail: " + nerr));
            }
        }

        // Pending mutation (ack/resolve) — drain and fire
        {
            String aId, aStatus;
            if (ui::actionPending(aId, aStatus)) {
                String aerr;
                Serial.printf("[pd] action %s on %s\n", aStatus.c_str(), aId.c_str());
                bool ok = pd::updateIncidentStatus(aId, aStatus, aerr);
                if (ok) {
                    String msg = (aStatus == "acknowledged") ? "ACK'd" : "RESOLVED";
                    ui::applyActionResult(true, msg);
                    // Force immediate poll so dashboard reflects the change.
                    pd::refresh();
                    ui::notifyDataRefreshed();
                    lastPollMs = millis();
                } else {
                    ui::applyActionResult(false, "Fail: " + aerr);
                }
            }
        }

        // Drill-in: timeline fetch when user taps an incident
        {
            String wantId;
            if (ui::timelineFetchPending(wantId)) {
                String terr;
                g_uiTimelineBuffer.clear();
                bool ok = pd::fetchTimeline(wantId, g_uiTimelineBuffer, terr);
                if (!ok) {
                    Serial.printf("[pd] timeline failed: %s\n", terr.c_str());
                }
                ui::applyTimeline();
            }
        }

        // Validate a freshly-saved token from the web portal (off the request thread).
        if (g_pdTokenJustSaved) {
            g_pdTokenJustSaved = false;
            String err;
            bool ok = pd::validateTokenVerbose(storage::getToken(), err);
            g_pdTokenValidated  = ok;
            g_pdValidationError = ok ? String() : err;
            if (ok) {
                pd::refresh();
                ui::showOverview();
                firstPollDone = true;
                lastPollMs = millis();
            }
        }

        // Poll PagerDuty
        if (storage::hasToken()) {
            if (!firstPollDone || (millis() - lastPollMs) > PD_POLL_INTERVAL_MS) {
                digitalWrite(LED_B, LOW);
                bool ok = pd::refresh();
                digitalWrite(LED_B, HIGH);
                digitalWrite(LED_G, ok ? LOW : HIGH);
                digitalWrite(LED_R, ok ? HIGH : LOW);
                lastPollMs = millis();
                if (!firstPollDone && ok) {
                    firstPollDone = true;
                    ui::showOverview();
                } else {
                    ui::notifyDataRefreshed();
                }
                ui::setStatusOnline(true);
            }
        } else if (storage::hasToken()) {
            // Token was just entered via the web portal.
            ui::showOverview();
        }
    } else {
        wasConnected = false;
        ui::setStatusOnline(false);
    }

    delay(5);
}

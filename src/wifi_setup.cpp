#include "wifi_setup.h"
#include "config.h"

#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Self-contained captive portal: AP + DNS hijack + WebServer.
// Stores creds in NVS namespace "wifi_cfg".

static String g_apSsid;
static netcfg::PortalEnterCb s_onPortal;
static netcfg::StatusCb      s_onStatus;
static volatile bool g_apActive  = false;
static volatile bool g_connected = false;

static const uint16_t DNS_PORT = 53;
static DNSServer  dns;
static WebServer  http(80);
static String     g_lastError;
static String     g_lastSsidTried;

static String makeApSsid() {
    uint64_t mac = ESP.getEfuseMac();
    char buf[8];
    snprintf(buf, sizeof(buf), "%04X", (uint16_t)(mac & 0xFFFF));
    return String(AP_SSID_PREFIX) + buf;
}

static String htmlEscape(const String& s) {
    String o; o.reserve(s.length()+8);
    for (size_t i=0;i<s.length();++i){char c=s[i];
        switch(c){case '<':o+="&lt;";break;case '>':o+="&gt;";break;
        case '&':o+="&amp;";break;case '"':o+="&quot;";break;default:o+=c;}}
    return o;
}

static String formPage(const String& msg, bool err) {
    int n = WiFi.scanComplete();
    // If never triggered or last attempt failed, kick a fresh scan.
    if (n == WIFI_SCAN_FAILED || n == -2) {
        WiFi.scanNetworks(true, false);
        n = WIFI_SCAN_RUNNING;
    }

    String h;
    h.reserve(4096);
    h += F("<!doctype html><html><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>PagerDuty CYD setup</title><style>"
           "body{font-family:-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif;"
           "background:#0b0f0d;color:#e6f5ee;margin:0;padding:24px}"
           ".w{max-width:520px;margin:0 auto}"
           "h1{color:#00ff41;font-size:22px;margin:6px 0}"
           ".sub{color:#7a8a82;font-size:13px;margin-bottom:18px}"
           ".card{background:#11171a;border:1px solid #1f2a25;border-radius:14px;padding:18px;margin-bottom:14px}"
           "label{display:block;font-size:12px;letter-spacing:.12em;text-transform:uppercase;color:#7a8a82;margin:12px 0 6px}"
           "input,select{width:100%;padding:11px 12px;border-radius:10px;border:1px solid #1f2a25;"
           "background:#0a0f0d;color:#e6f5ee;font-size:15px;outline:none;box-sizing:border-box}"
           "input:focus,select:focus{border-color:#00ff41}"
           "button{appearance:none;border:0;background:linear-gradient(180deg,#00ff41,#00b82e);"
           "color:#062;font-weight:800;padding:12px;border-radius:10px;width:100%;font-size:15px;"
           "letter-spacing:.06em;text-transform:uppercase;cursor:pointer;margin-top:16px}"
           "button.secondary{background:#1c2622;color:#cfe9d8;font-weight:600;margin-top:10px}"
           ".banner{padding:10px 12px;border-radius:10px;margin-bottom:12px;font-size:13px}"
           ".ok{background:#102b1a;color:#aef0c0;border:1px solid #1f5532}"
           ".err{background:#2a1011;color:#ffb6bb;border:1px solid #5a1d22}"
           ".small{color:#7a8a82;font-size:12px;margin-top:8px}"
           ".scan-state{color:#7a8a82;font-size:12px;margin:6px 0 0}"
           "</style></head><body><div class=\"w\">"
           "<h1>Connect to WiFi</h1>"
           "<div class=\"sub\">PagerDuty CYD &middot; first-time setup</div>");

    if (msg.length()) {
        h += "<div class=\"banner ";
        h += err ? "err" : "ok";
        h += "\">";
        h += htmlEscape(msg);
        h += "</div>";
    }

    h += F("<form method=\"POST\" action=\"/wifi\" class=\"card\">"
           "<label>Network</label><select name=\"ssid\">");
    h += "<option value=\"\">-- pick a network or type manually below --</option>";
    if (n > 0) {
        for (int i = 0; i < n && i < 25; ++i) {
            String s = WiFi.SSID(i);
            int rssi = WiFi.RSSI(i);
            h += "<option value=\"" + htmlEscape(s) + "\">"
                 + htmlEscape(s) + " (" + String(rssi) + " dBm)</option>";
        }
    }
    h += F("</select>");
    if (n == WIFI_SCAN_RUNNING) {
        h += "<p class=\"scan-state\">Scanning networks&hellip; refresh in a few seconds.</p>";
    } else if (n == 0) {
        h += "<p class=\"scan-state\">No networks found. Try rescanning, or type your SSID manually.</p>";
    } else if (n > 0) {
        h += "<p class=\"scan-state\">" + String(n) + " network" + String(n==1?"":"s") + " found.</p>";
    }
    h += F("<label>Or type SSID manually <small>(hidden / not listed)</small></label>"
           "<input type=\"text\" name=\"ssid_manual\" placeholder=\"My WiFi\" autocomplete=\"off\">"
           "<label>Password</label>"
           "<input type=\"password\" name=\"pass\" autocomplete=\"new-password\">"
           "<button type=\"submit\">Save &amp; connect</button>"
           "<div class=\"small\">2.4 GHz networks only. ESP32 doesn't speak 5 GHz or WPA3-only.</div>"
           "</form>"
           "<form method=\"POST\" action=\"/rescan\" class=\"card\" style=\"padding:14px 18px\">"
           "<button type=\"submit\" class=\"secondary\">Rescan networks</button>"
           "</form>"
           "</div></body></html>");
    return h;
}

static void handleRoot()  { http.send(200, "text/html", formPage(g_lastError, g_lastError.length() > 0)); g_lastError = ""; }
static void redirectRoot(){ http.sendHeader("Location", "http://192.168.4.1/"); http.send(302, "text/plain", ""); }
static void handleRescan(){
    WiFi.scanDelete();
    WiFi.scanNetworks(true, false);
    http.sendHeader("Location", "/"); http.send(302, "text/plain", "");
}

static void saveCreds(const String& ssid, const String& pass) {
    Preferences p; p.begin("wifi_cfg", false);
    p.putString("ssid", ssid);
    p.putString("pass", pass);
    p.end();
}
static bool loadCreds(String& ssid, String& pass) {
    Preferences p; p.begin("wifi_cfg", true);
    ssid = p.getString("ssid", "");
    pass = p.getString("pass", "");
    p.end();
    return ssid.length() > 0;
}

static bool tryConnect(const String& ssid, const String& pass, uint32_t timeoutMs) {
    Serial.printf("[wifi] connecting to '%s' ...\n", ssid.c_str());
    WiFi.disconnect(false, false);
    delay(50);
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t start = millis();
    wl_status_t st = WL_IDLE_STATUS;
    while (millis() - start < timeoutMs) {
        st = WiFi.status();
        if (st == WL_CONNECTED) {
            Serial.printf("[wifi] connected ip=%s\n", WiFi.localIP().toString().c_str());
            return true;
        }
        if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) {
            Serial.printf("[wifi] connect failed status=%d\n", (int)st);
            break;
        }
        delay(150);
    }
    Serial.printf("[wifi] connect timed out, last status=%d\n", (int)WiFi.status());
    return false;
}

static void handleSave() {
    String ssid = http.arg("ssid_manual");
    if (ssid.length() == 0) ssid = http.arg("ssid");
    String pass = http.arg("pass");
    ssid.trim();
    g_lastSsidTried = ssid;

    if (ssid.length() == 0) {
        g_lastError = "SSID is empty.";
        http.sendHeader("Location", "/"); http.send(302, "text/plain", "");
        return;
    }

    // Reply IMMEDIATELY before we tear down AP, so the phone gets the page.
    String pre = String("Connecting to ") + ssid + "...";
    String pageMsg = "Saved. Trying to connect to '" + ssid + "'. The device will switch off this WiFi.";
    http.send(200, "text/html",
        "<html><head><meta charset=\"utf-8\"><meta http-equiv=\"refresh\" content=\"4;url=/\"></head>"
        "<body style=\"background:#0b0f0d;color:#e6f5ee;font-family:sans-serif;padding:24px\">"
        "<h1 style=\"color:#00ff41\">" + htmlEscape(pre) + "</h1>"
        "<p>" + htmlEscape(pageMsg) + "</p></body></html>");
    delay(200);

    saveCreds(ssid, pass);

    // Stop AP/portal so STA can take the radio.
    dns.stop();
    http.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    g_apActive = false;

    if (tryConnect(ssid, pass, 25000)) {
        g_connected = true;
        if (s_onStatus) s_onStatus(String("Connected to ") + ssid);
        return;  // task ends; main loop sees connected=true
    }

    // Failed — reboot to start a fresh portal session with new attempt.
    Serial.println("[wifi] connect failed, rebooting to retry portal");
    delay(800);
    ESP.restart();
}

static void portalLoop() {
    g_apActive = true;
    Serial.printf("[wifi] AP up: %s ip=%s\n",
                  g_apSsid.c_str(), WiFi.softAPIP().toString().c_str());
    if (s_onPortal) s_onPortal(g_apSsid, AP_PASSWORD);

    // Async scan; passive=false so the radio actively probes. show_hidden=false,
    // 300ms/channel keeps the AP responsive while still finding most networks.
    WiFi.scanNetworks(true, false, false, 300);

    dns.start(DNS_PORT, "*", WiFi.softAPIP());

    http.on("/",                HTTP_GET,  handleRoot);
    http.on("/wifi",            HTTP_POST, handleSave);
    http.on("/rescan",          HTTP_POST, handleRescan);
    // Captive-portal redirect endpoints used by phones to detect login walls
    http.on("/generate_204",    HTTP_GET,  redirectRoot);
    http.on("/gen_204",         HTTP_GET,  redirectRoot);
    http.on("/hotspot-detect.html", HTTP_GET, handleRoot);
    http.on("/library/test/success.html", HTTP_GET, handleRoot);
    http.on("/connecttest.txt", HTTP_GET, redirectRoot);
    http.on("/ncsi.txt",        HTTP_GET, redirectRoot);
    http.on("/redirect",        HTTP_GET, redirectRoot);
    http.onNotFound([](){ http.sendHeader("Location", "http://192.168.4.1/"); http.send(302, "text/plain", ""); });
    http.begin();

    uint32_t lastScanKick = millis();
    while (!g_connected) {
        dns.processNextRequest();
        http.handleClient();
        // Self-heal the scan: if it failed/finished-empty, kick it again every 8s.
        if (millis() - lastScanKick > 8000) {
            int n = WiFi.scanComplete();
            if (n == WIFI_SCAN_FAILED || n == -2 || n == 0) {
                WiFi.scanDelete();
                WiFi.scanNetworks(true, false, false, 300);
            }
            lastScanKick = millis();
        }
        delay(2);
    }
}

static void wifiTask(void*) {
    if (s_onStatus) s_onStatus("Connecting WiFi...");

    // 1. Try saved creds (with retry — first attempt often times out).
    String ssid, pass;
    if (loadCreds(ssid, pass)) {
        if (s_onStatus) s_onStatus(String("Connecting to ") + ssid);
        WiFi.mode(WIFI_STA);
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (attempt > 0) {
                Serial.printf("[wifi] retry %d\n", attempt);
                delay(1500);
            }
            if (tryConnect(ssid, pass, 25000)) {
                g_connected = true;
                if (s_onStatus) s_onStatus(String("Connected to ") + ssid);
                vTaskDelete(NULL);
                return;
            }
        }
        Serial.println("[wifi] saved creds failed after retries, opening portal");
    } else {
        Serial.println("[wifi] no saved creds, opening portal");
    }

    // 2. Open portal.
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(g_apSsid.c_str(), AP_PASSWORD);
    delay(200);
    portalLoop();   // blocks until g_connected

    if (s_onStatus) s_onStatus("Connected");
    vTaskDelete(NULL);
}

void netcfg::begin(PortalEnterCb onPortal, StatusCb onStatus) {
    g_apSsid   = makeApSsid();
    s_onPortal = onPortal;
    s_onStatus = onStatus;
    g_connected = false;
    g_apActive  = false;
    xTaskCreatePinnedToCore(wifiTask, "wifi", 8192, nullptr, 1, nullptr, 0);
}

void netcfg::process() {
    if (!g_connected && WiFi.status() == WL_CONNECTED) g_connected = true;
}

String netcfg::apSsid()         { return g_apSsid.length() ? g_apSsid : makeApSsid(); }
bool   netcfg::isConnected()    { return g_connected || WiFi.status() == WL_CONNECTED; }
bool   netcfg::isPortalActive() { return g_apActive; }

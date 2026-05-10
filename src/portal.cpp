#include "portal.h"
#include "config.h"
#include "storage.h"
#include "pagerduty.h"

#include <WiFi.h>
#include <WebServer.h>

static WebServer server(PORTAL_HTTP_PORT);

static const char PAGE_HEAD[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PagerDuty CYD</title>
<style>
:root{--bg:#0b0f0d;--panel:#11171a;--ink:#e6f5ee;--mut:#7a8a82;--ok:#00ff41;--err:#ff4757;--accent:#00ff41}
*{box-sizing:border-box}
body{margin:0;font-family:-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif;background:radial-gradient(ellipse at top,#0e1a14,#070a08);color:var(--ink);min-height:100vh}
.wrap{max-width:520px;margin:0 auto;padding:28px 18px}
.brand{display:flex;align-items:center;gap:10px;margin-bottom:6px}
.dot{width:10px;height:10px;border-radius:50%;background:var(--ok);box-shadow:0 0 12px var(--ok)}
.brand h1{font-size:14px;letter-spacing:.18em;text-transform:uppercase;color:var(--mut);margin:0}
.title{font-weight:800;font-size:30px;letter-spacing:-.5px;color:var(--ok);margin:6px 0 4px}
.sub{color:var(--mut);font-size:13px;margin-bottom:22px}
.card{background:var(--panel);border:1px solid #1f2a25;border-radius:14px;padding:18px}
label{display:block;font-size:12px;letter-spacing:.12em;text-transform:uppercase;color:var(--mut);margin:14px 0 6px}
input[type=text],input[type=password],input[type=email]{
 width:100%;padding:12px 14px;border-radius:10px;border:1px solid #1f2a25;background:#0a0f0d;color:var(--ink);font-size:15px;outline:none}
input:focus{border-color:var(--ok);box-shadow:0 0 0 3px rgba(0,255,65,.15)}
button{appearance:none;border:0;background:linear-gradient(180deg,#00ff41,#00b82e);color:#062;font-weight:800;
 padding:12px 16px;border-radius:10px;cursor:pointer;width:100%;margin-top:18px;font-size:15px;letter-spacing:.06em;text-transform:uppercase}
button.secondary{background:#1c2622;color:#cfe9d8;font-weight:600}
.row{display:flex;gap:10px}
.row>*{flex:1}
.note{margin-top:16px;color:var(--mut);font-size:12px;line-height:1.5}
.kvs{display:grid;grid-template-columns:auto 1fr;gap:6px 14px;font-size:13px;color:#cfe9d8}
.kvs b{color:var(--mut);font-weight:500}
.banner{margin:12px 0;padding:10px 12px;border-radius:10px;background:#102b1a;color:#aef0c0;border:1px solid #1f5532;font-size:13px}
.banner.err{background:#2a1011;color:#ffb6bb;border-color:#5a1d22}
hr{border:0;border-top:1px solid #1f2a25;margin:18px 0}
small{color:var(--mut)}
</style></head><body><div class="wrap">
<div class="brand"><span class="dot"></span><h1>PagerDuty CYD &middot; setup</h1></div>
)HTML";

static const char PAGE_FOOT[] PROGMEM = R"HTML(
</div></body></html>
)HTML";

static String htmlEscape(const String& s) {
    String o; o.reserve(s.length()+8);
    for (size_t i=0;i<s.length();++i){char c=s[i];
        switch(c){case '<':o+="&lt;";break;case '>':o+="&gt;";break;
        case '&':o+="&amp;";break;case '"':o+="&quot;";break;default:o+=c;}}
    return o;
}

static void sendStatus(const String& banner, bool err=false) {
    String html = FPSTR(PAGE_HEAD);
    html += "<div class=\"title\">Configure</div>";
    html += "<div class=\"sub\">Enter your PagerDuty <b>User</b> REST API token. Stored only on this device.</div>";
    if (banner.length()) {
        html += String("<div class=\"banner ") + (err?"err":"") + "\">" + htmlEscape(banner) + "</div>";
    }
    String region = storage::getRegion();
    html += "<form method=\"POST\" action=\"/save\" class=\"card\">";
    html += "<label>Region</label>";
    html += "<select name=\"region\">";
    html += String("<option value=\"us\"") + (region=="us"?" selected":"") + ">US (api.pagerduty.com)</option>";
    html += String("<option value=\"eu\"") + (region=="eu"?" selected":"") + ">EU (api.eu.pagerduty.com)</option>";
    html += "</select>";
    html += "<label>User API Token</label><input type=\"password\" name=\"token\" placeholder=\"u+...\" autocomplete=\"off\" required>";
    html += "<div class=\"note\" style=\"margin-top:6px\">Use a <b>User</b> token from <i>My Profile &rarr; User Settings &rarr; Create API User Token</i>. "
            "Account-level tokens also work; <b>integration keys do not</b>.</div>";
    html += "<label>Account email <small>(optional, for from-address)</small></label>";
    html += "<input type=\"email\" name=\"email\" value=\"" + htmlEscape(storage::getEmail()) + "\" placeholder=\"you@example.com\">";
    html += "<button type=\"submit\">Save</button>";
    html += "</form>";

    html += "<hr><div class=\"card\"><div class=\"kvs\">";
    html += "<b>SSID</b><span>" + htmlEscape(WiFi.SSID()) + "</span>";
    html += "<b>IP</b><span>" + WiFi.localIP().toString() + "</span>";
    html += "<b>RSSI</b><span>" + String(WiFi.RSSI()) + " dBm</span>";
    html += "<b>Token</b><span>" + String(storage::hasToken()?"set":"not set") + "</span>";
    if (pd::counts().lastFetchMs) {
        html += "<b>Last poll</b><span>" + String((millis()-pd::counts().lastFetchMs)/1000) + "s ago" +
                (pd::counts().ok?" ok":" err: "+htmlEscape(pd::counts().error)) + "</span>";
    }
    html += "</div>";
    html += "<form method=\"POST\" action=\"/forget\" style=\"margin-top:14px\">";
    html += "<button type=\"submit\" class=\"secondary\">Forget WiFi &amp; token</button></form>";
    html += "</div>";
    html += "<p class=\"note\">Token is stored in NVS on the device. Use a <b>read-only</b> token where possible.</p>";
    html += FPSTR(PAGE_FOOT);
    server.send(200, "text/html", html);
}

static void handleRoot()  { sendStatus("", false); }

// Token to be validated by the main loop, not on this request thread.
extern volatile bool   g_pdTokenJustSaved;
extern volatile bool   g_pdTokenValidated;
extern String          g_pdValidationError;

static void handleSave() {
    String token = server.arg("token");
    String email = server.arg("email");
    token.trim(); email.trim();
    if (token.length() < 10) { sendStatus("Token looks too short.", true); return; }

    String region = server.arg("region");
    if (region != "eu") region = "us";
    storage::setRegion(region);
    storage::setToken(token);
    storage::setEmail(email);
    g_pdTokenJustSaved   = true;
    g_pdTokenValidated   = false;
    g_pdValidationError  = "";

    server.sendHeader("Location", "/saved");
    server.send(303, "text/plain", "");
}

static void handleSaved() {
    String banner;
    bool err = false;
    if (g_pdValidationError.length()) { banner = "Validation failed: " + g_pdValidationError; err = true; }
    else if (g_pdTokenValidated)      { banner = "Token verified. Open the device."; }
    else                              { banner = "Token saved. Verifying with PagerDuty... refresh in a few seconds."; }
    sendStatus(banner, err);
}

static void handleForget() {
    storage::clearAll();
    String html = FPSTR(PAGE_HEAD);
    html += "<div class=\"title\">Cleared</div><div class=\"sub\">Restarting and re-opening captive portal&hellip;</div>";
    html += FPSTR(PAGE_FOOT);
    server.send(200, "text/html", html);
    delay(500);
    ESP.restart();
}

void portal::begin() {
    server.on("/",       HTTP_GET,  handleRoot);
    server.on("/save",   HTTP_POST, handleSave);
    server.on("/saved",  HTTP_GET,  handleSaved);
    server.on("/forget", HTTP_POST, handleForget);
    server.onNotFound([](){ server.sendHeader("Location","/"); server.send(302,"text/plain",""); });
    server.begin();
    Serial.printf("[portal] http on http://%s/\n", WiFi.localIP().toString().c_str());
}

void portal::loop()        { server.handleClient(); }
String portal::currentIP() { return WiFi.localIP().toString(); }

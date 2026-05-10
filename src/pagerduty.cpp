#include "pagerduty.h"
#include "config.h"
#include "storage.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

namespace pd {

static String apiBase() {
    return storage::getRegion() == "eu" ? PD_API_BASE_EU : PD_API_BASE_US;
}

static ListFilter g_listFilter;
void setListFilter(const ListFilter& f) { g_listFilter = f; }
const ListFilter& getListFilter() { return g_listFilter; }

static Counts g_counts;
static std::vector<Incident> g_recent;

bool isConfigured() { return storage::hasToken(); }
const Counts& counts() { return g_counts; }
const std::vector<Incident>& recent() { return g_recent; }

static bool httpGetJson(const String& url, JsonDocument& doc, String& err) {
    if (WiFi.status() != WL_CONNECTED) { err = "wifi"; return false; }
    String token = storage::getToken();
    if (token.length() < 10) { err = "no token"; return false; }

    WiFiClientSecure client;
    client.setInsecure();          // PD api uses public CA; CYD has limited cert store
    client.setTimeout(10000);

    HTTPClient http;
    http.setTimeout(10000);
    if (!http.begin(client, url)) { err = "http begin"; return false; }
    http.addHeader("Authorization", "Token token=" + token);
    http.addHeader("Accept", "application/vnd.pagerduty+json;version=2");
    http.addHeader("Content-Type", "application/json");

    int code = http.GET();
    if (code != 200) {
        err = "HTTP " + String(code);
        http.end();
        return false;
    }
    DeserializationError je = deserializeJson(doc, http.getStream());
    http.end();
    if (je) { err = String("json: ") + je.c_str(); return false; }
    return true;
}

static int tryHttpsGet(const String& url, const String& token, String& body, String& err) {
    Serial.printf("[pd] heap free=%u largest=%u\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap());
    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(20);
    client.setTimeout(20000);

    HTTPClient http;
    http.setReuse(false);
    http.setTimeout(20000);
    http.setConnectTimeout(15000);
    http.setUserAgent("pagerduty-cyd/1.0");

    if (!http.begin(client, url)) { err = "http begin failed"; return -2; }
    http.addHeader("Authorization", "Token token=" + token);
    http.addHeader("Accept", "application/vnd.pagerduty+json;version=2");
    http.addHeader("Connection", "close");

    int code = http.GET();
    if (code <= 0) {
        err = String("HTTP ") + code + " (" + HTTPClient::errorToString(code) + ")";
    } else if (code != 200) {
        body = http.getString();
        if (body.length() > 160) body = body.substring(0, 160) + "...";
        err = "HTTP " + String(code);
        if (body.length()) err += " — " + body;
    } else {
        body = http.getString();
    }
    http.end();
    return code;
}

static bool diagnoseConnectivity(String& err) {
    String host = (storage::getRegion() == "eu") ? "api.eu.pagerduty.com" : "api.pagerduty.com";
    IPAddress ip;
    Serial.printf("[pd] DNS lookup %s ...\n", host.c_str());
    if (!WiFi.hostByName(host.c_str(), ip)) {
        err = String("DNS failed for ") + host;
        Serial.println("[pd] DNS failed");
        return false;
    }
    Serial.printf("[pd] %s -> %s; trying TCP:443\n", host.c_str(), ip.toString().c_str());

    WiFiClient tcp;
    tcp.setTimeout(10);
    bool ok = tcp.connect(ip, 443, 10000);
    if (!ok) {
        err = String("TCP 443 to ") + host + " (" + ip.toString() + ") refused/blocked";
        Serial.println("[pd] TCP connect failed");
        return false;
    }
    Serial.println("[pd] TCP 443 OK");
    tcp.stop();
    return true;
}

bool validateTokenVerbose(const String& token, String& err) {
    if (WiFi.status() != WL_CONNECTED) { err = "no WiFi"; return false; }

    if (!diagnoseConnectivity(err)) return false;

    String url = apiBase() + "/users/me";
    Serial.printf("[pd] validate %s heap=%u\n", url.c_str(), (unsigned)ESP.getFreeHeap());

    String body;
    int code = tryHttpsGet(url, token, body, err);
    if (code == 200) return true;

    Serial.printf("[pd] validate retry after error: %s\n", err.c_str());
    delay(800);
    body = ""; err = "";
    code = tryHttpsGet(url, token, body, err);
    return code == 200;
}

bool validateToken(const String& token) {
    String e; return validateTokenVerbose(token, e);
}

bool updateIncidentStatus(const String& incidentId, const String& newStatus, String& err) {
    if (WiFi.status() != WL_CONNECTED) { err = "no WiFi"; return false; }
    // User tokens (u+...) carry the user identity, so From: is only needed for
    // account-level tokens. Send From: when we have an email; let PD decide.
    String email = storage::getEmail();
    String url = apiBase() + "/incidents/" + incidentId;
    Serial.printf("[pd] PUT %s status=%s\n", url.c_str(), newStatus.c_str());

    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(20);
    client.setTimeout(20000);

    HTTPClient http;
    http.setReuse(false);
    http.setTimeout(20000);
    http.setConnectTimeout(15000);
    http.setUserAgent("pagerduty-cyd/1.0");

    if (!http.begin(client, url)) { err = "http begin failed"; return false; }
    http.addHeader("Authorization", "Token token=" + storage::getToken());
    http.addHeader("Accept", "application/vnd.pagerduty+json;version=2");
    http.addHeader("Content-Type", "application/json");
    if (email.length()) http.addHeader("From", email);
    http.addHeader("Connection", "close");

    String body = String("{\"incident\":{\"type\":\"incident_reference\",\"status\":\"") + newStatus + "\"}}";
    int code = http.PUT(body);
    if (code != 200) {
        String resp = http.getString();
        if (resp.length() > 160) resp = resp.substring(0, 160) + "...";
        err = String("HTTP ") + code;
        if (resp.length()) err += " — " + resp;
        Serial.printf("[pd] PUT failed: %s\n", err.c_str());
    }
    http.end();
    return code == 200;
}

bool snoozeIncident(const String& incidentId, int durationSeconds, String& err) {
    if (WiFi.status() != WL_CONNECTED) { err = "no WiFi"; return false; }
    String email = storage::getEmail();
    if (email.length() == 0) { err = "no From email"; return false; }
    String url = apiBase() + "/incidents/" + incidentId + "/snooze";
    WiFiClientSecure client; client.setInsecure(); client.setHandshakeTimeout(20); client.setTimeout(20000);
    HTTPClient http; http.setReuse(false); http.setTimeout(20000); http.setConnectTimeout(15000);
    if (!http.begin(client, url)) { err = "http begin"; return false; }
    http.addHeader("Authorization", "Token token=" + storage::getToken());
    http.addHeader("Accept", "application/vnd.pagerduty+json;version=2");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("From", email);
    String body = String("{\"duration\":") + durationSeconds + "}";
    int code = http.POST(body);
    if (code != 200) { err = "HTTP " + String(code) + " " + http.getString().substring(0,120); }
    http.end();
    return code == 200;
}

bool addNote(const String& incidentId, const String& content, String& err) {
    if (WiFi.status() != WL_CONNECTED) { err = "no WiFi"; return false; }
    String email = storage::getEmail();
    if (email.length() == 0) { err = "no From email"; return false; }
    String url = apiBase() + "/incidents/" + incidentId + "/notes";
    WiFiClientSecure client; client.setInsecure(); client.setHandshakeTimeout(20); client.setTimeout(20000);
    HTTPClient http; http.setReuse(false); http.setTimeout(20000); http.setConnectTimeout(15000);
    if (!http.begin(client, url)) { err = "http begin"; return false; }
    http.addHeader("Authorization", "Token token=" + storage::getToken());
    http.addHeader("Accept", "application/vnd.pagerduty+json;version=2");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("From", email);
    String esc; esc.reserve(content.length() + 8);
    for (size_t i=0;i<content.length();++i){char c=content[i]; if (c=='"'||c=='\\') esc += '\\'; esc += c;}
    String body = String("{\"note\":{\"content\":\"") + esc + "\"}}";
    int code = http.POST(body);
    if (code != 201 && code != 200) { err = "HTTP " + String(code); }
    http.end();
    return code == 200 || code == 201;
}

bool fetchOnCalls(std::vector<OnCall>& out, String& err, int limit) {
    out.clear();
    if (WiFi.status() != WL_CONNECTED) { err = "no WiFi"; return false; }
    String url = apiBase() + "/oncalls?limit=" + String(limit) +
                 "&include[]=users&include[]=schedules&include[]=escalation_policies";
    WiFiClientSecure client; client.setInsecure(); client.setHandshakeTimeout(20); client.setTimeout(20000);
    HTTPClient http; http.setReuse(false); http.setTimeout(20000); http.setConnectTimeout(15000);
    if (!http.begin(client, url)) { err = "http begin"; return false; }
    http.addHeader("Authorization", "Token token=" + storage::getToken());
    http.addHeader("Accept", "application/vnd.pagerduty+json;version=2");
    int code = http.GET();
    if (code != 200) { err = "HTTP " + String(code); http.end(); return false; }
    JsonDocument doc;
    if (deserializeJson(doc, http.getStream())) { err = "json"; http.end(); return false; }
    http.end();
    for (JsonObject oc : doc["oncalls"].as<JsonArray>()) {
        OnCall e;
        e.user             = oc["user"]["summary"]                | "";
        e.schedule         = oc["schedule"]["summary"]            | "(direct)";
        e.escalationPolicy = oc["escalation_policy"]["summary"]   | "";
        e.level            = oc["escalation_level"]               | 0;
        out.push_back(e);
    }
    return true;
}

int fetchMTTR(int& mttrSeconds, String& err, int hoursBack) {
    mttrSeconds = 0;
    if (WiFi.status() != WL_CONNECTED) { err = "no WiFi"; return 0; }
    // Time math: PD accepts ISO8601 in `since` param.
    time_t now = time(nullptr);
    if (now < 1700000000) { err = "no clock"; return 0; }
    time_t since = now - hoursBack * 3600;
    struct tm tmu; gmtime_r(&since, &tmu);
    char sb[32]; strftime(sb, sizeof(sb), "%Y-%m-%dT%H:%M:%SZ", &tmu);
    String url = apiBase() + "/incidents?statuses%5B%5D=resolved&since=" + String(sb) + "&limit=50&time_zone=UTC";

    WiFiClientSecure client; client.setInsecure(); client.setHandshakeTimeout(20); client.setTimeout(20000);
    HTTPClient http; http.setReuse(false); http.setTimeout(20000); http.setConnectTimeout(15000);
    if (!http.begin(client, url)) { err = "http begin"; return 0; }
    http.addHeader("Authorization", "Token token=" + storage::getToken());
    http.addHeader("Accept", "application/vnd.pagerduty+json;version=2");
    int code = http.GET();
    if (code != 200) { err = "HTTP " + String(code); http.end(); return 0; }
    JsonDocument doc;
    if (deserializeJson(doc, http.getStream())) { err = "json"; http.end(); return 0; }
    http.end();

    long sumSec = 0;
    int n = 0;
    auto parseIso = [](const char* s) -> time_t {
        if (!s || !*s) return 0;
        struct tm tm{}; int y,M,d,h,m,sec;
        if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y,&M,&d,&h,&m,&sec) != 6) return 0;
        tm.tm_year=y-1900; tm.tm_mon=M-1; tm.tm_mday=d;
        tm.tm_hour=h; tm.tm_min=m; tm.tm_sec=sec;
        // Avoid timegm (not always present); compute UTC seconds from epoch manually.
        // Days since 1970-01-01 for this Y/M/D:
        static const int mDays[] = {0,31,59,90,120,151,181,212,243,273,304,334};
        int yy = tm.tm_year + 1900;
        long days = (yy - 1970) * 365L + (yy - 1969)/4 - (yy - 1901)/100 + (yy - 1601)/400;
        days += mDays[tm.tm_mon];
        if (tm.tm_mon > 1 && ((yy%4==0 && yy%100!=0) || yy%400==0)) days += 1;
        days += tm.tm_mday - 1;
        return (time_t)(days * 86400L + tm.tm_hour * 3600L + tm.tm_min * 60L + tm.tm_sec);
    };
    for (JsonObject inc : doc["incidents"].as<JsonArray>()) {
        const char* c = inc["created_at"]  | "";
        const char* r = inc["resolved_at"] | "";
        time_t ct = parseIso(c), rt = parseIso(r);
        if (ct > 0 && rt > ct) { sumSec += (rt - ct); n++; }
    }
    if (n > 0) mttrSeconds = (int)(sumSec / n);
    return n;
}

bool fetchTimeline(const String& incidentId, std::vector<LogEntry>& out, String& err, int limit) {
    out.clear();
    if (WiFi.status() != WL_CONNECTED) { err = "no WiFi"; return false; }
    String url = apiBase() + "/incidents/" + incidentId + "/log_entries?limit=" + String(limit) + "&time_zone=UTC";

    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(20);
    client.setTimeout(20000);
    HTTPClient http;
    http.setReuse(false);
    http.setTimeout(20000);
    http.setConnectTimeout(15000);
    http.setUserAgent("pagerduty-cyd/1.0");

    if (!http.begin(client, url)) { err = "http begin failed"; return false; }
    http.addHeader("Authorization", "Token token=" + storage::getToken());
    http.addHeader("Accept", "application/vnd.pagerduty+json;version=2");
    http.addHeader("Connection", "close");

    int code = http.GET();
    if (code != 200) {
        err = String("HTTP ") + code;
        http.end();
        return false;
    }
    JsonDocument doc;
    DeserializationError je = deserializeJson(doc, http.getStream());
    http.end();
    if (je) { err = String("json: ") + je.c_str(); return false; }

    for (JsonObject le : doc["log_entries"].as<JsonArray>()) {
        LogEntry e;
        e.type      = le["type"]            | "";
        e.summary   = le["summary"]         | "";
        e.createdAt = le["created_at"]      | "";
        e.agent     = le["agent"]["summary"] | "";
        out.push_back(e);
    }
    return true;
}

static bool fetchIncidents(const char* statusList, JsonDocument& doc, String& err) {
    String url = apiBase() + "/incidents?limit=14&time_zone=UTC&" + statusList;
    return httpGetJson(url, doc, err);
}

static String buildFilteredQuery() {
    // Server-side narrowing — fewer bytes back, less LVGL pool churn.
    // No trailing '&' — caller appends.
    String q;
    auto append = [&](const String& s){ if (q.length()) q += "&"; q += s; };
    switch (g_listFilter.status) {
        case ListFilter::S_TRIGGERED: append("statuses%5B%5D=triggered"); break;
        case ListFilter::S_ACKED:     append("statuses%5B%5D=acknowledged"); break;
        default: append("statuses%5B%5D=triggered");
                 append("statuses%5B%5D=acknowledged"); break;
    }
    if (g_listFilter.highOnly) append("urgencies%5B%5D=high");
    if (g_listFilter.serviceId.length()) append("service_ids%5B%5D=" + g_listFilter.serviceId);
    return q;
}

bool refresh() {
    g_counts.ok = false;
    g_counts.error = "";

    Serial.printf("[pd] poll (s=%d high=%d) heap=%u\n",
                  (int)g_listFilter.status, (int)g_listFilter.highOnly,
                  (unsigned)ESP.getFreeHeap());
    String params = buildFilteredQuery();
    JsonDocument doc;
    String err;
    if (!fetchIncidents(params.c_str(), doc, err)) {
        g_counts.error = err;
        g_counts.lastFetchMs = millis();
        Serial.printf("[pd] poll FAIL: %s\n", err.c_str());
        return false;
    }

    g_recent.clear();
    int triggered = 0, acked = 0;
    int total = doc["incidents"].as<JsonArray>().size();
    Serial.printf("[pd] api returned %d incidents\n", total);
    for (JsonObject inc : doc["incidents"].as<JsonArray>()) {
        Incident i;
        i.id        = inc["id"]              | "";
        i.number    = String((int)(inc["incident_number"] | 0));
        i.title     = inc["title"]           | "";
        i.status    = inc["status"]          | "";
        i.urgency   = inc["urgency"]         | "";
        i.service   = inc["service"]["summary"] | "";
        i.createdAt = inc["created_at"]      | "";
        if (inc["assignments"].is<JsonArray>() && inc["assignments"].size() > 0) {
            i.responder = inc["assignments"][0]["assignee"]["summary"] | "";
        }
        Serial.printf("[pd]   #%s status=%s urg=%s\n",
                      i.number.c_str(), i.status.c_str(), i.urgency.c_str());
        if (i.status == "triggered") triggered++;
        else if (i.status == "acknowledged") acked++;
        g_recent.push_back(i);
        if (g_recent.size() >= 14) break;   // cap to keep LVGL pool happy
    }
    g_counts.triggered = triggered;
    g_counts.acknowledged = acked;
    g_counts.ok = true;
    g_counts.lastFetchMs = millis();
    Serial.printf("[pd] poll OK: %d triggered, %d acked\n", triggered, acked);
    return true;
}

}

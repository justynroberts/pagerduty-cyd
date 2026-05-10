#pragma once
#include <Arduino.h>
#include <vector>

namespace pd {

struct Incident {
    String id;
    String number;
    String title;
    String status;     // triggered | acknowledged | resolved
    String urgency;    // high | low
    String service;
    String createdAt;
    String responder;        // first assignee summary, or "" if unassigned
};

struct Counts {
    int triggered = 0;
    int acknowledged = 0;
    int resolved24h = 0;
    bool ok = false;        // last fetch succeeded
    String error;           // last error msg
    uint32_t lastFetchMs = 0;
};

bool isConfigured();

// Server-side narrowing for the Incidents list.
struct ListFilter {
    enum Status { S_ANY = 0, S_TRIGGERED, S_ACKED } status = S_ANY;
    bool   highOnly = false;
    String serviceId;     // when set, restricts to one service
};
void setListFilter(const ListFilter& f);
const ListFilter& getListFilter();

bool refresh();                          // synchronous; populates counts and recent incidents
const Counts& counts();
const std::vector<Incident>& recent();   // open incidents (triggered + acknowledged)

// Optional: validate a token (returns true if /users/me succeeds)
bool validateToken(const String& token);
bool validateTokenVerbose(const String& token, String& err);

// Per-incident timeline (log entries)
struct LogEntry {
    String type;        // trigger_log_entry, acknowledge_log_entry, note_log_entry, ...
    String summary;
    String createdAt;
    String agent;
};
bool fetchTimeline(const String& incidentId, std::vector<LogEntry>& out, String& err, int limit = 20);

// Mutating actions (require From: <user-email> header).
// newStatus must be "acknowledged" or "resolved".
bool updateIncidentStatus(const String& incidentId, const String& newStatus, String& err);
bool snoozeIncident(const String& incidentId, int durationSeconds, String& err);
bool addNote(const String& incidentId, const String& content, String& err);

// On-call now
struct OnCall {
    String user;
    String schedule;
    String escalationPolicy;
    int    level;
};
bool fetchOnCalls(std::vector<OnCall>& out, String& err, int limit = 25);

// MTTR over recent resolved incidents (seconds). Returns count actually used.
int  fetchMTTR(int& mttrSeconds, String& err, int hoursBack = 24);

}

#include "storage.h"
#include "config.h"
#include <Preferences.h>

static Preferences prefs;

void storage::begin() {
    prefs.begin(NVS_NS, false);
}

String storage::getToken() {
    return prefs.getString(NVS_KEY_TOKEN, "");
}

void storage::setToken(const String& token) {
    prefs.putString(NVS_KEY_TOKEN, token);
}

String storage::getEmail() {
    return prefs.getString(NVS_KEY_USEREMAIL, "");
}

void storage::setEmail(const String& email) {
    prefs.putString(NVS_KEY_USEREMAIL, email);
}

String storage::getRegion() {
    String r = prefs.getString(NVS_KEY_REGION, "us");
    if (r != "us" && r != "eu") r = "us";
    return r;
}

void storage::setRegion(const String& r) {
    prefs.putString(NVS_KEY_REGION, (r == "eu") ? "eu" : "us");
}

void storage::clearAll() {
    prefs.clear();
}

bool storage::hasToken() {
    return prefs.getString(NVS_KEY_TOKEN, "").length() > 10;
}

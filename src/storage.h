#pragma once
#include <Arduino.h>

namespace storage {
    void begin();
    String getToken();
    void   setToken(const String& token);
    String getEmail();
    void   setEmail(const String& email);
    String getRegion();          // "us" or "eu" (default "us")
    void   setRegion(const String& r);
    void   clearAll();
    bool   hasToken();
}

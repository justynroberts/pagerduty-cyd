#pragma once
#include <Arduino.h>

namespace display {
    void begin();
    void tick();          // call frequently from loop()
    void setBacklight(uint8_t pct);  // 0..100
}

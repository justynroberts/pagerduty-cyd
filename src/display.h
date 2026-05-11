#pragma once
#include <Arduino.h>

namespace display {
    void begin();
    void tick();          // call frequently from loop()
    void setBacklight(uint8_t pct);  // 0..100
    bool touched();       // raw touch state — usable before LVGL is running
    // Show a "hold to factory-reset" prompt on the bare TFT (LVGL not required).
    // Returns true if the user held for the full duration.
    bool factoryResetPrompt(uint32_t holdMs = 2000);
}

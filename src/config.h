#pragma once

// Display: 320x240 landscape (rotation 1)
#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240

// Touch (XPT2046) — separate SPI bus
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_SCLK 25
#define TOUCH_CS   33
#define TOUCH_IRQ  36

// Touch calibration (raw -> pixel mapping); refined later if needed.
#define TOUCH_X_MIN 200
#define TOUCH_X_MAX 3700
#define TOUCH_Y_MIN 240
#define TOUCH_Y_MAX 3800

// Onboard RGB LED (active-low)
#define LED_R 4
#define LED_G 16
#define LED_B 17

// Captive portal
#define AP_SSID_PREFIX "PagerDuty-CYD-"
#define AP_PASSWORD    "pdsetup1"

// PagerDuty
#define PD_POLL_INTERVAL_MS 30000
#define PD_API_BASE_US "https://api.pagerduty.com"
#define PD_API_BASE_EU "https://api.eu.pagerduty.com"

// HTTP web portal
#define PORTAL_HTTP_PORT 80

// NVS keys
#define NVS_NS         "pdcyd"
#define NVS_KEY_TOKEN  "pd_token"
#define NVS_KEY_USEREMAIL "pd_email"
#define NVS_KEY_REGION "pd_region"   // "us" or "eu"

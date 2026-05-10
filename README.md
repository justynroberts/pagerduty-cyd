# PagerDuty CYD Dashboard

A live PagerDuty incident dashboard running on a $10 ESP32 "CYD" (Cheap Yellow Display, ESP32-2432S028R v2). Touch screen, four swipeable dashboards, server-side filtering, captive-portal setup, tap-to-ack.

![overview](docs/overview.jpg)

## What it does

- **Overview** — large OPEN (red) and ACK (yellow) counters; tap either card to drill into a filtered list.
- **Incidents** — scrollable list with `ALL / TRIG / ACK / SVC` filter chips. Server-side filtering keeps the response small.
- **Services** — open-incident counts grouped by service, sorted by impact. Tap a row to filter Incidents to that service.
- **On-Call** — top on-call rotations with level + escalation policy + schedule.
- **Incident detail** — title, service, responder, full `/log_entries` timeline, and a one-tap **ACK** button (`PUT /incidents/{id}` with `From:` header).
- **Status icon** — taps into a Status screen (WiFi, IP, RSSI, region, free heap, last poll).
- Auto-rotates dashboards every 10 s; swipe left/right to navigate manually; swipe down to force-refresh; tap any of the four page dots to jump.

## Setup

The device boots into a captive portal (`PagerDuty-CYD-XXXX` / `pdsetup1`).

1. Join the AP from your phone — captive portal opens automatically.
2. Pick your home WiFi, enter the password.
3. Once connected, the device shows its IP and `pagerduty.local`. Open it in a browser.
4. Paste a **PagerDuty User REST API token** (account-level tokens also work; integration keys do not). Pick US or EU region.
5. Done — dashboard appears within a few seconds.

## Build

```bash
git clone https://github.com/justynroberts/pagerduty-cyd
cd pagerduty-cyd
pio run -t upload   # PlatformIO + ESP32 toolchain
```

Targets the ESP32-2432S028R v2 (USB-C, ST7789). For the older ILI9341 variant, change the `TFT_*` flags in `platformio.ini`.

## Architecture

| File | Job |
|---|---|
| `src/main.cpp` | Boot, main loop, polling cadence, NTP, mDNS |
| `src/wifi_setup.cpp` | Custom captive portal (no WiFiManager — too flaky on ESP32 Arduino 2.x) |
| `src/portal.cpp` | HTTP server for token entry + region selection |
| `src/pagerduty.cpp` | REST client: incidents, on-calls, log_entries, ack/snooze/note actions |
| `src/storage.cpp` | NVS — stored token + region |
| `src/display.cpp` | TFT_eSPI + LVGL bring-up, XPT2046 touch on a separate SPI bus |
| `src/ui.cpp` | All LVGL screens — modal (setup/connecting/waiting), 4 rotating dashboards, incident detail |
| `tools/png_to_lvgl.py` | PNG → LVGL C-array converter for icons/logo |
| `assets/` | Source PNGs (logo, severity badges, hero icons) |

WiFi runs on core 0 (FreeRTOS task), LVGL on core 1 — UI never blocks on TLS handshakes. Cross-thread state is moved with `portMUX_TYPE` critical sections.

## Memory

Build sits at ~50% flash, ~35% RAM. LVGL pool is 56 KB, draw buffer is 8 lines × 320 px, list rendering is capped at 14 rows so the pool never runs out under load. Server-side filter chips keep the API response under 4 KB regardless of how many incidents you have open.

## License

MIT.

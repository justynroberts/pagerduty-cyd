# I built a PagerDuty incident dashboard on a $10 ESP32

A while back I picked up one of those "Cheap Yellow Displays" — an ESP32 with a 2.8" touchscreen for around ten bucks. It sat in a drawer waiting for a problem to solve. Then on a quiet on-call rotation I caught myself reaching for my phone to check PagerDuty *again*, and the use case became obvious.

The result is a permanently-on incident dashboard for my desk. Open and acknowledged counters as the front screen, swipeable views for the live incident list, services with open issues, and who's currently on call. Tap any incident to drill into its full timeline with one-tap ACK. No phone, no laptop, no tab open in the corner of a monitor.

Source: https://github.com/justynroberts/pagerduty-cyd

## What I wanted from it

A few non-negotiables before I started writing code:

- **Glanceable.** Numbers big enough to read from across the room. If a high-urgency incident fires I should see it without focusing my eyes.
- **Touch, not buttons.** A keyboard on a 320×240 screen is a non-starter, so any action has to be a tap or a swipe.
- **Self-contained setup.** Drop a fresh device on a colleague's desk → they paste a PagerDuty token via captive portal → it works. No re-flashing.
- **Stays running.** Survives WiFi blips, doesn't crash on a memory leak after a week, doesn't need a watchdog reset every morning.

All of those turned out to be harder than they sound on a chip with 320 KB of RAM.

## The hardware

The board is the ESP32-2432S028R v2 — the USB-C revision with the ST7789 display driver. About £6–10 depending on where you buy. It's notorious for poor color accuracy and worse documentation, but for a desk display it's perfect.

Inside there's an ESP32 with WiFi and Bluetooth, a 2.8" 320×240 TFT, a resistive touch panel (XPT2046 on a separate SPI bus from the display), a tiny piezo, an LDR for ambient light, and a single RGB LED. Plenty for a dashboard.

The screen is **not** great. Off-axis viewing is dim, color shifts heavily, and the contrast ratio is mediocre. The first lesson was: design for the worst case. Body text at default `#7A8A82` mid-gray on a near-black background was unreadable across the room. Bumping it to `#B5C4BC` made everything legible without losing the dark-mode aesthetic.

## The stack

- **PlatformIO + Arduino-ESP32**. ESP-IDF would give finer control but the LVGL + TFT_eSPI ecosystem assumes Arduino, and that's a fight I didn't want to pick.
- **LVGL 8.3.x** for the UI. Pre-rendered bitmap fonts, dirty-region tracking, swipe gestures, the lot. Works inside 56 KB of memory with care.
- **TFT_eSPI** for the display, configured entirely via build flags rather than the usual `User_Setup.h` voodoo.
- **A custom captive portal** (originally WiFiManager — kept silently dropping form submissions on ESP32 Arduino 2.x; replaced it with ~100 lines of `WebServer` + `DNSServer` and never looked back).
- **PagerDuty REST API v2**. Token in NVS, US/EU region selectable.

The two cores actually matter here. WiFi and TLS handshakes can take 2-3 seconds, and you absolutely cannot block the UI for that long. So the WiFi/network code lives on core 0 as a FreeRTOS task, and LVGL runs on core 1. The two communicate through a couple of `portMUX_TYPE`-protected globals — strings get queued from core 0, the main loop drains them on core 1 and applies them to LVGL. Without that split, every WiFi reconnect would freeze the screen.

## What it ended up looking like

Five rotating screens, auto-rotating every 10 seconds, swipeable, with tappable page dots at the bottom for direct navigation:

1. **Overview** — the PagerDuty wordmark across the top, then two huge numeric cards: OPEN (red) and ACK (yellow). Tap either to drill into the filtered list. There's a small ⚙ icon top-right that pops a Status screen with WiFi info, free heap, last poll, region.
2. **Incidents** — scrollable list with severity badges as 24×24 PNGs, color-coded by status. Filter chips at the top for ALL / TRIG / ACK / SVC. Each chip change triggers a server-side narrowing of the API call so the response stays small no matter how many incidents you have open.
3. **Services** — every service with open incidents, sorted by impact, color-coded by whether it has triggered or only acked issues. Tap a row to drill into Incidents filtered to that service.
4. **On-Call** — current on-call rotation across schedules, with level number and escalation policy.

Tap an incident → detail screen with title, service, responder, plus the full timeline pulled from `/incidents/{id}/log_entries`. Color-coded by event type. A big BACK button on the left, ACK on the right, no other actions to confuse anyone.

## Things that bit me

**LVGL screen transitions.** Calling `lv_scr_load()` from the modal connecting screen to a rotating dashboard appeared to work — the active screen pointer changed correctly — but pixels never refreshed. Turned out the state-driven check in my main loop was firing `lv_scr_load_anim()` repeatedly while an animation was already in flight, restarting it every iteration so it never completed. The fix was to use a synchronous `lv_scr_load_anim(scr, NONE, 0, 0, false)` for the first transition out of a modal screen, then animations only between rotating screens.

**LVGL memory pool exhaustion.** Each rebuild of the incidents list (every 30 s on poll) clean-and-creates ~40 widgets. That's fine for one screen, but I was rebuilding all four list screens on every poll regardless of which was visible. The pool fragmented after a couple of minutes and the next allocation returned NULL silently — `lv_label_create` on a NULL parent then crashes inside `lv_obj_mark_layout_as_dirty`. Fixed by lazily refreshing only the visible screen + the one being navigated to.

**TLS handshake failures.** ESP32 + `WiFiClientSecure` + `setInsecure()` is reliable for short HTTPS calls but will silently truncate larger responses, especially when the LVGL pool eats into the heap. The clue was an "incomplete input" error from ArduinoJson. Fixes that helped: bump `setHandshakeTimeout(20)`, drop the `Connection: close` header (so the socket stays open for the whole transaction), and reduce the API limit from 25 to 14 so the response stays small.

**Auto-reverting dashboards.** I had a state-driven main-loop check that forced the screen back to Overview if it wasn't on a known rotating screen. When I added a fifth rotating screen but forgot to update `currentScreen()` to recognize it, the auto-rotate would land on the new screen for ~30 ms before getting yanked back to Overview. Looked like the rotation was broken; was actually the recognition logic.

**Color order.** The CYD v2 wants `TFT_BGR` and inversion off, but my brain kept telling me `TFT_RGB` was correct. I flipped this back and forth three times before checking a known-pure-red color and watching it render as blue.

**WiFi router moods.** First-boot connect to my home WiFi fails about one in three times. Saved-creds retry works on the second attempt. I added a 3-attempt retry loop with a generous 25-second timeout per attempt, and a fallback to captive portal only after all three fail. Symptom otherwise was "device boots straight to setup mode for no reason".

## Memory & performance

Final build sits at:
- **Flash: 50% used** (~1.55 MB of 3 MB partition)
- **RAM: 35% used** (~115 KB of 327 KB)

LVGL pool is 56 KB — peaks around 30 KB when an incident detail with timeline is open. Free heap stays at ~160 KB.

Server-side filter narrowing was the single biggest improvement. Before: 25 incidents pulled per poll regardless of how many you cared about. After: tap "TRIG" → API call gets `?statuses[]=triggered`, response is only triggered incidents, list is tiny, LVGL pool barely notices.

The WiFi/UI core split keeps everything responsive even during a 2-second TLS handshake. The screen never freezes; the worst you'll see is the LIVE indicator briefly flicking to OFFLINE during a reconnect.

## What's next

Things I haven't built yet but probably will:

- **Webhook receiver** — host a `POST /webhook` endpoint and have PagerDuty push to it. Brings new-incident latency from "30 s polling" down to "as fast as the network allows", probably under 100 ms.
- **Auto-dim on the LDR** — the CYD has a light sensor on GPIO 34. Turn the backlight down at night, full brightness during the day. Free hardware feature, small code.
- **Buzzer on new triggered incident** — tiny piezo on board. A short beep when the open count goes up.
- **OTA updates** through the same web portal that takes the API token. ESP32's `Update.h` is straightforward.

The whole thing took maybe 3 evenings of writing code, plus another 2 chasing the gremlins above. Total cost about £8 in hardware. Worth it for the satisfaction of glancing at my desk and seeing OPEN: 0 in big green numbers.

Code, schematics for the hardware, and the LVGL-array converter tool are all on GitHub: https://github.com/justynroberts/pagerduty-cyd

If you build one, send me a photo.

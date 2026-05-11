#include "ui.h"
#include "config.h"
#include "pagerduty.h"
#include "storage.h"

#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>
#include <map>
#include <vector>

#include "assets_gen.h"
#include "fonts/fonts.h"

// Pick severity badge image for an incident — strictly by status.
static const lv_img_dsc_t* sevBadge(const pd::Incident& i) {
    if (i.status == "triggered")     return &sev_p1; // red 1
    if (i.status == "acknowledged")  return &sev_p3; // yellow 3
    return &sev_p5;                                  // gray 5 (resolved/other)
}

// ============================================================================
// PagerDuty palette
// ============================================================================
#define COL_BG        lv_color_hex(0x0B0F0D)
#define COL_PANEL     lv_color_hex(0x11171A)
#define COL_PANEL_HI  lv_color_hex(0x182026)
#define COL_INK       lv_color_hex(0xE6F5EE)
#define COL_MUT       lv_color_hex(0xB5C4BC)
#define COL_DIM       lv_color_hex(0x3A4540)
#define COL_LINE      lv_color_hex(0x1F2A25)
#define COL_PD_GREEN  lv_color_hex(0x00FF41)
#define COL_OPEN      lv_color_hex(0xFF4757)
#define COL_ACK       lv_color_hex(0xFFC400)
#define COL_OK        lv_color_hex(0x00FF41)
#define COL_INFO      lv_color_hex(0x4A8FE7)

// ============================================================================
// State
// ============================================================================
static bool g_inited = false;
static bool g_dashboardStarted = false;

// Modal screens (not in rotation)
static lv_obj_t* scr_setup       = nullptr;
static lv_obj_t* scr_connecting  = nullptr;
static lv_obj_t* scr_waiting     = nullptr;
static lv_obj_t* scr_detail      = nullptr;

// Rotating dashboard screens
enum RotIdx { ROT_OVERVIEW = 0, ROT_INCIDENTS, ROT_SERVICES, ROT_ONCALL, ROT_COUNT };
static lv_obj_t* scr_rot[ROT_COUNT] = {nullptr};
static lv_obj_t* scr_info_off = nullptr;          // non-rotating Status screen (icon-tap)
static const char* ROT_TITLE[ROT_COUNT] = { "OVERVIEW", "INCIDENTS", "SERVICES", "ON-CALL" };
static int g_rotIdx = 0;
static lv_timer_t* g_rotateTimer = nullptr;
static uint32_t g_pauseUntilMs = 0;
static const uint32_t ROTATE_PERIOD_MS = 10000;
static const uint32_t MANUAL_PAUSE_MS  = 30000;

// Per-screen children we need to update on data refresh
static lv_obj_t* lbl_open = nullptr;
static lv_obj_t* lbl_ack = nullptr;
static lv_obj_t* lbl_health = nullptr;
static lv_obj_t* card_health = nullptr;
static lv_obj_t* lbl_overview_status = nullptr;
static lv_obj_t* lbl_overview_mttr = nullptr;
static lv_obj_t* chart_overview = nullptr;
static lv_chart_series_t* chart_series = nullptr;
static lv_obj_t* incidents_list = nullptr;
static lv_obj_t* lbl_incidents_empty = nullptr;
static lv_obj_t* incidents_filterbar = nullptr;
static lv_obj_t* services_list = nullptr;
static lv_obj_t* oncall_list = nullptr;
static lv_obj_t* info_grid = nullptr;
static lv_obj_t* clock_label[ROT_COUNT] = {nullptr};

// Filter state for incidents screen
enum FilterMode { FLT_ALL = 0, FLT_TRIG, FLT_ACK, FLT_SERVICE };
static FilterMode g_filter = FLT_ALL;
static String     g_filterService = "";

// Pull-to-refresh + force flag
static volatile bool g_refreshNow = false;

// Snooze + note pending
static String g_pendingSnoozeId;
static int    g_pendingSnoozeSec = 0;
static String g_pendingNoteId;
static String g_pendingNoteText;

// Snooze modal
static lv_obj_t* snooze_modal = nullptr;

// Per-screen page-dots row + status indicator (kept as arrays so we can update each)
static lv_obj_t* page_dots[ROT_COUNT][ROT_COUNT] = {{nullptr}};
static lv_obj_t* status_dot[ROT_COUNT] = {nullptr};
static lv_obj_t* status_label[ROT_COUNT] = {nullptr};
static bool      g_online = true;

// Drill-down state
static pd::Incident g_detailInc;
static lv_obj_t* detail_timeline_list = nullptr;
static lv_obj_t* detail_timeline_loading = nullptr;
static lv_obj_t* detail_toast = nullptr;
static String g_pendingTimelineId;
static String g_pendingActionId;
static String g_pendingActionStatus;
static std::vector<pd::LogEntry> g_lastTimeline;
static lv_obj_t* btn_ack = nullptr;
static lv_obj_t* btn_resolve = nullptr;

// ============================================================================
// Forward decls
// ============================================================================
static void buildOverview();
static void buildIncidents();
static void buildServices();
static void buildInfo();
static void rebuildIncidentsList();
static void rebuildServicesList();
static void rebuildInfo();
static void updateOverview();
static void rotateTo(int idx, bool fromUser);
static void onScreenGesture(lv_event_t* e);
static void onRotateTick(lv_timer_t* t);
static void onIncidentClicked(lv_event_t* e);
static void buildIncidentDetail(const pd::Incident& inc);
static void onDetailBack(lv_event_t* e);

// ============================================================================
// Helpers
// ============================================================================
static void styleBg(lv_obj_t* o) {
    lv_obj_set_style_bg_color(o, COL_BG, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

// Compact status bar (top): online dot + screen title + WiFi indicator
static void addStatusBar(lv_obj_t* parent, int rotIdx) {
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_set_size(bar, SCREEN_WIDTH, 22);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // Tappable HOME button at left — always returns to Overview
    lv_obj_t* home = lv_btn_create(bar);
    lv_obj_set_size(home, 30, 22);
    lv_obj_align(home, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(home, COL_PANEL_HI, 0);
    lv_obj_set_style_border_width(home, 0, 0);
    lv_obj_set_style_radius(home, 4, 0);
    lv_obj_t* hi = lv_label_create(home);
    lv_label_set_text(hi, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(hi, COL_PD_GREEN, 0);
    lv_obj_set_style_text_font(hi, &outfit_bold_14, 0);
    lv_obj_center(hi);
    lv_obj_add_event_cb(home, [](lv_event_t*){ rotateTo(ROT_OVERVIEW, true); },
                        LV_EVENT_CLICKED, nullptr);

    // online dot (next to home)
    lv_obj_t* d = lv_obj_create(bar);
    lv_obj_set_size(d, 6, 6);
    lv_obj_align(d, LV_ALIGN_LEFT_MID, 36, 0);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(d, COL_PD_GREEN, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_set_style_shadow_color(d, COL_PD_GREEN, 0);
    lv_obj_set_style_shadow_width(d, 6, 0);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
    if (rotIdx >= 0 && rotIdx < ROT_COUNT) status_dot[rotIdx] = d;

    // title (center)
    lv_obj_t* t = lv_label_create(bar);
    lv_label_set_text(t, ROT_TITLE[rotIdx]);
    lv_obj_set_style_text_color(t, COL_INK, 0);
    lv_obj_set_style_text_font(t, &outfit_bold_12, 0);
    lv_obj_set_style_text_letter_space(t, 3, 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 0);

    // status label (right)
    lv_obj_t* s = lv_label_create(bar);
    lv_label_set_text(s, "LIVE");
    lv_obj_set_style_text_color(s, COL_PD_GREEN, 0);
    lv_obj_set_style_text_font(s, &outfit_bold_12, 0);
    lv_obj_set_style_text_letter_space(s, 2, 0);
    lv_obj_align(s, LV_ALIGN_RIGHT_MID, -8, 0);
    if (rotIdx >= 0 && rotIdx < ROT_COUNT) status_label[rotIdx] = s;

    // clock label (right of LIVE) — small HH:MM
    lv_obj_t* cl = lv_label_create(bar);
    lv_label_set_text(cl, "--:--");
    lv_obj_set_style_text_color(cl, COL_MUT, 0);
    lv_obj_set_style_text_font(cl, &outfit_bold_12, 0);
    lv_obj_align(cl, LV_ALIGN_RIGHT_MID, -42, 0);
    if (rotIdx >= 0 && rotIdx < ROT_COUNT) clock_label[rotIdx] = cl;
}

// Page-dot indicator at the bottom (one row per screen so it's child of that screen)
// Each dot is also tappable — direct navigation between rotating screens.
static void addPageDots(lv_obj_t* parent, int rotIdx) {
    int n = ROT_COUNT;
    int dotW = 14, gap = 8;   // bigger to be tap-friendly
    int rowW = n * dotW + (n-1) * gap;
    int x0 = (SCREEN_WIDTH - rowW) / 2;
    int y  = SCREEN_HEIGHT - 18;
    for (int i = 0; i < n; ++i) {
        lv_obj_t* dt = lv_obj_create(parent);
        lv_obj_set_size(dt, dotW, dotW);
        lv_obj_set_pos(dt, x0 + i*(dotW+gap), y);
        lv_obj_set_style_radius(dt, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dt, 0, 0);
        lv_obj_set_style_bg_color(dt, (i == rotIdx) ? COL_PD_GREEN : COL_DIM, 0);
        lv_obj_set_style_bg_opa(dt, LV_OPA_COVER, 0);
        lv_obj_clear_flag(dt, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(dt, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(dt, (void*)(intptr_t)i);
        lv_obj_add_event_cb(dt, [](lv_event_t* e){
            int target = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
            rotateTo(target, true);
        }, LV_EVENT_CLICKED, nullptr);
        if (rotIdx >= 0 && rotIdx < ROT_COUNT) page_dots[rotIdx][i] = dt;
    }
}

static lv_obj_t* makeRotScreen(int rotIdx) {
    lv_obj_t* s = lv_obj_create(nullptr);
    styleBg(s);
    addStatusBar(s, rotIdx);
    addPageDots(s, rotIdx);
    // Capture swipe gestures at screen level
    lv_obj_add_event_cb(s, onScreenGesture, LV_EVENT_GESTURE, nullptr);
    return s;
}

// ============================================================================
// Modal: setup hint / connecting / waiting (kept simple — no rotation here)
// ============================================================================

// Old simple header used for modal screens
static void simpleHeader(lv_obj_t* parent, const char* title) {
    lv_obj_t* hdr = lv_obj_create(parent);
    lv_obj_set_size(hdr, SCREEN_WIDTH, 22);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* d = lv_obj_create(hdr);
    lv_obj_set_size(d, 8, 8);
    lv_obj_align(d, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(d, COL_PD_GREEN, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_set_style_shadow_color(d, COL_PD_GREEN, 0);
    lv_obj_set_style_shadow_width(d, 8, 0);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* l = lv_label_create(hdr);
    lv_label_set_text(l, title);
    lv_obj_set_style_text_color(l, COL_MUT, 0);
    lv_obj_set_style_text_font(l, &outfit_bold_12, 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
}

void ui::begin() {
    if (g_inited) return;
    lv_obj_set_style_bg_color(lv_scr_act(), COL_BG, 0);
    g_inited = true;
}

void ui::showSetupHint(const String& ssid, const String& password, const String& help) {
    if (!scr_setup) scr_setup = lv_obj_create(nullptr);
    else            lv_obj_clean(scr_setup);
    styleBg(scr_setup);
    simpleHeader(scr_setup, "FIRST-TIME SETUP");

    lv_obj_t* hero = lv_img_create(scr_setup);
    lv_img_set_src(hero, &hero_wifi);
    lv_obj_align(hero, LV_ALIGN_TOP_LEFT, 14, 30);

    lv_obj_t* t = lv_label_create(scr_setup);
    lv_label_set_text(t, "Connect WiFi");
    lv_obj_set_style_text_color(t, COL_PD_GREEN, 0);
    lv_obj_set_style_text_font(t, &outfit_bold_22, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 120, 32);

    lv_obj_t* sub = lv_label_create(scr_setup);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(sub, 190);
    lv_label_set_text(sub, "Join this AP with your phone");
    lv_obj_set_style_text_color(sub, COL_INK, 0);
    lv_obj_set_style_text_font(sub, &outfit_bold_14, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 120, 60);

    lv_obj_t* card = lv_obj_create(scr_setup);
    lv_obj_set_size(card, SCREEN_WIDTH - 32, 92);
    lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -52);
    lv_obj_set_style_bg_color(card, COL_PANEL, 0);
    lv_obj_set_style_border_color(card, COL_PD_GREEN, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* k1 = lv_label_create(card);
    lv_label_set_text(k1, "SSID");
    lv_obj_set_style_text_color(k1, COL_MUT, 0);
    lv_obj_set_style_text_font(k1, &outfit_bold_12, 0);
    lv_obj_align(k1, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t* v1 = lv_label_create(card);
    lv_label_set_long_mode(v1, LV_LABEL_LONG_DOT);
    lv_obj_set_width(v1, SCREEN_WIDTH - 60);
    lv_label_set_text(v1, ssid.c_str());
    lv_obj_set_style_text_color(v1, COL_INK, 0);
    lv_obj_set_style_text_font(v1, &outfit_bold_18, 0);
    lv_obj_align(v1, LV_ALIGN_TOP_LEFT, 0, 14);

    lv_obj_t* k2 = lv_label_create(card);
    lv_label_set_text(k2, "PASSWORD");
    lv_obj_set_style_text_color(k2, COL_MUT, 0);
    lv_obj_set_style_text_font(k2, &outfit_bold_12, 0);
    lv_obj_align(k2, LV_ALIGN_TOP_LEFT, 0, 44);

    lv_obj_t* v2 = lv_label_create(card);
    lv_label_set_long_mode(v2, LV_LABEL_LONG_DOT);
    lv_obj_set_width(v2, SCREEN_WIDTH - 60);
    lv_label_set_text(v2, password.c_str());
    lv_obj_set_style_text_color(v2, COL_INK, 0);
    lv_obj_set_style_text_font(v2, &outfit_bold_18, 0);
    lv_obj_align(v2, LV_ALIGN_TOP_LEFT, 0, 58);

    lv_obj_t* h = lv_label_create(scr_setup);
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(h, SCREEN_WIDTH - 24);
    lv_label_set_text(h, help.c_str());
    lv_obj_set_style_text_color(h, COL_INK, 0);
    lv_obj_set_style_text_font(h, &outfit_bold_14, 0);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_scr_load(scr_setup);
}

void ui::showConnecting(const String& msg) {
    if (!scr_connecting) scr_connecting = lv_obj_create(nullptr);
    else                 lv_obj_clean(scr_connecting);
    styleBg(scr_connecting);
    simpleHeader(scr_connecting, "STARTING UP");

    // PagerDuty logo at the top
    lv_obj_t* logo = lv_img_create(scr_connecting);
    lv_img_set_src(logo, &logo_pd);
    lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 30);

    lv_obj_t* sp = lv_spinner_create(scr_connecting, 1500, 60);
    lv_obj_set_size(sp, 50, 50);
    lv_obj_align(sp, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_arc_color(sp, COL_LINE, LV_PART_MAIN);
    lv_obj_set_style_arc_color(sp, COL_PD_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(sp, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(sp, 4, LV_PART_INDICATOR);

    lv_obj_t* l = lv_label_create(scr_connecting);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, SCREEN_WIDTH - 24);
    String pretty = msg;
    int idx = pretty.indexOf(" to ");
    if (idx >= 0) pretty = pretty.substring(0, (unsigned)(idx + 3)) + "\n" + pretty.substring((unsigned)(idx + 4));
    lv_label_set_text(l, pretty.c_str());
    lv_obj_set_style_text_color(l, COL_INK, 0);
    lv_obj_set_style_text_font(l, &outfit_bold_16, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(l, 4, 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 50);

    lv_scr_load(scr_connecting);
}

void ui::showWaitingForToken(const String& portalUrl) {
    if (!scr_waiting) scr_waiting = lv_obj_create(nullptr);
    else              lv_obj_clean(scr_waiting);
    styleBg(scr_waiting);
    simpleHeader(scr_waiting, "ENTER API TOKEN");

    lv_obj_t* hero = lv_img_create(scr_waiting);
    lv_img_set_src(hero, &hero_token);
    lv_obj_align(hero, LV_ALIGN_TOP_LEFT, 14, 30);

    lv_obj_t* t = lv_label_create(scr_waiting);
    lv_label_set_text(t, "Open in browser:");
    lv_obj_set_style_text_color(t, COL_INK, 0);
    lv_obj_set_style_text_font(t, &outfit_bold_14, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 120, 32);

    lv_obj_t* url = lv_label_create(scr_waiting);
    lv_label_set_long_mode(url, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(url, 190);
    lv_label_set_text(url, portalUrl.c_str());
    lv_obj_set_style_text_color(url, COL_PD_GREEN, 0);
    lv_obj_set_style_text_font(url, &outfit_bold_18, 0);
    lv_obj_set_style_text_line_space(url, 2, 0);
    lv_obj_align(url, LV_ALIGN_TOP_LEFT, 120, 50);

    lv_obj_t* h = lv_label_create(scr_waiting);
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(h, SCREEN_WIDTH - 24);
    lv_label_set_text(h, "Paste your PagerDuty USER REST API token to begin.\nSame WiFi as this device.");
    lv_obj_set_style_text_color(h, COL_INK, 0);
    lv_obj_set_style_text_font(h, &outfit_bold_14, 0);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -10);

    lv_scr_load(scr_waiting);
}

// ============================================================================
// Rotating screens
// ============================================================================

static lv_obj_t* makeStat(lv_obj_t* parent, lv_color_t accent,
                          const char* label, int x, int y, int w, int h) {
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_bg_color(c, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, accent, 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_border_opa(c, LV_OPA_70, 0);
    lv_obj_set_style_radius(c, 12, 0);
    lv_obj_set_style_pad_all(c, 6, 0);
    lv_obj_set_style_shadow_color(c, accent, 0);
    lv_obj_set_style_shadow_width(c, 14, 0);
    lv_obj_set_style_shadow_opa(c, LV_OPA_30, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl = lv_label_create(c);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, accent, 0);
    lv_obj_set_style_text_font(lbl, &outfit_bold_12, 0);
    lv_obj_set_style_text_letter_space(lbl, 2, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 2);
    return c;
}
static lv_obj_t* makeBigNumber(lv_obj_t* card, lv_color_t color, const lv_font_t* font) {
    lv_obj_t* v = lv_label_create(card);
    lv_obj_set_style_text_color(v, color, 0);
    lv_obj_set_style_text_font(v, font, 0);
    lv_label_set_text(v, "-");
    lv_obj_align(v, LV_ALIGN_BOTTOM_MID, 0, -2);
    return v;
}

static void buildOverview() {
    if (scr_rot[ROT_OVERVIEW]) return;
    lv_obj_t* s = makeRotScreen(ROT_OVERVIEW);
    scr_rot[ROT_OVERVIEW] = s;

    // Big PagerDuty logo at the top
    lv_obj_t* logo = lv_img_create(s);
    lv_img_set_src(logo, &logo_pd);
    lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 30);

    // Small INFO icon — tap → Status screen
    lv_obj_t* infoBtn = lv_btn_create(s);
    lv_obj_set_size(infoBtn, 30, 22);
    lv_obj_align(infoBtn, LV_ALIGN_TOP_RIGHT, -6, 28);
    lv_obj_set_style_bg_color(infoBtn, COL_PANEL_HI, 0);
    lv_obj_set_style_border_color(infoBtn, COL_LINE, 0);
    lv_obj_set_style_border_width(infoBtn, 1, 0);
    lv_obj_set_style_radius(infoBtn, 6, 0);
    lv_obj_t* iL = lv_label_create(infoBtn);
    lv_label_set_text(iL, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(iL, COL_PD_GREEN, 0);
    lv_obj_set_style_text_font(iL, &outfit_bold_14, 0);
    lv_obj_center(iL);
    lv_obj_add_event_cb(infoBtn, [](lv_event_t*){
        if (!scr_info_off) return;
        rebuildInfo();
        lv_scr_load_anim(scr_info_off, LV_SCR_LOAD_ANIM_OVER_LEFT, 200, 0, false);
    }, LV_EVENT_CLICKED, nullptr);

    // Two smaller tappable cards below the logo
    int cardW = 100, cardH = 90, gap = 16;
    int x0 = (SCREEN_WIDTH - 2*cardW - gap) / 2;
    int y0 = 100;

    // OPEN — red, tap → incidents filtered to triggered
    lv_obj_t* c_open = makeStat(s, COL_OPEN, "OPEN", x0, y0, cardW, cardH);
    lbl_open = makeBigNumber(c_open, COL_OPEN, &outfit_bold_48);
    lv_obj_add_flag(c_open, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c_open, [](lv_event_t*){
        g_filter = FLT_TRIG;
        g_filterService = "";
        rotateTo(ROT_INCIDENTS, true);
        rebuildIncidentsList();
    }, LV_EVENT_CLICKED, nullptr);

    // ACK — yellow, tap → incidents filtered to acknowledged
    lv_obj_t* c_ack = makeStat(s, COL_ACK, "ACK", x0 + cardW + gap, y0, cardW, cardH);
    lbl_ack = makeBigNumber(c_ack, COL_ACK, &outfit_bold_48);
    lv_obj_add_flag(c_ack, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c_ack, [](lv_event_t*){
        g_filter = FLT_ACK;
        g_filterService = "";
        rotateTo(ROT_INCIDENTS, true);
        rebuildIncidentsList();
    }, LV_EVENT_CLICKED, nullptr);

    // Health/sparkline/MTTR removed — keep dashboard clean.
    lbl_health = nullptr;
    card_health = nullptr;
    chart_overview = nullptr;
    chart_series = nullptr;
    lbl_overview_mttr = nullptr;

    // Bottom status row above page dots
    lbl_overview_status = lv_label_create(s);
    lv_label_set_text(lbl_overview_status, "WAITING FOR DATA");
    lv_obj_set_style_text_color(lbl_overview_status, COL_MUT, 0);
    lv_obj_set_style_text_font(lbl_overview_status, &outfit_bold_12, 0);
    lv_obj_set_style_text_letter_space(lbl_overview_status, 2, 0);
    lv_obj_align(lbl_overview_status, LV_ALIGN_BOTTOM_MID, 0, -28);
}

static void onFilterChip(lv_event_t* e) {
    int mode = (int)(intptr_t)lv_event_get_user_data(e);
    g_filter = (FilterMode)mode;
    if (mode != FLT_SERVICE) g_filterService = "";

    // Mirror the chip choice into the server-side filter so the next poll
    // pulls only the rows we'll actually show.
    pd::ListFilter pf;
    switch (mode) {
        case FLT_TRIG:    pf.status = pd::ListFilter::S_TRIGGERED; break;
        case FLT_ACK:     pf.status = pd::ListFilter::S_ACKED;     break;
        case FLT_SERVICE: pf.serviceId = g_filterService;           break;
        default:          break;
    }
    pd::setListFilter(pf);
    g_refreshNow = true;   // pick it up on next loop tick
    rebuildIncidentsList();
    // Update chip styles
    if (incidents_filterbar) {
        for (int i = 0; i < 4; ++i) {
            lv_obj_t* c = lv_obj_get_child(incidents_filterbar, i);
            if (!c) continue;
            bool on = (i == mode);
            lv_obj_set_style_bg_color(c, on ? COL_PD_GREEN : COL_PANEL, 0);
            lv_obj_t* lb = lv_obj_get_child(c, 0);
            if (lb) lv_obj_set_style_text_color(lb, on ? lv_color_hex(0x062) : COL_MUT, 0);
        }
    }
}

static void addFilterChip(lv_obj_t* parent, const char* label, int mode, int x) {
    lv_obj_t* c = lv_btn_create(parent);
    lv_obj_set_size(c, 60, 18);
    lv_obj_set_pos(c, x, 0);
    lv_obj_set_style_bg_color(c, mode == g_filter ? COL_PD_GREEN : COL_PANEL, 0);
    lv_obj_set_style_border_color(c, COL_LINE, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 9, 0);
    lv_obj_set_style_shadow_width(c, 0, 0);
    lv_obj_t* lb = lv_label_create(c);
    lv_label_set_text(lb, label);
    lv_obj_set_style_text_font(lb, &outfit_bold_12, 0);
    lv_obj_set_style_text_color(lb, mode == g_filter ? lv_color_hex(0x062) : COL_MUT, 0);
    lv_obj_center(lb);
    lv_obj_add_event_cb(c, onFilterChip, LV_EVENT_CLICKED, (void*)(intptr_t)mode);
}

static void buildIncidents() {
    if (scr_rot[ROT_INCIDENTS]) return;
    lv_obj_t* s = makeRotScreen(ROT_INCIDENTS);
    scr_rot[ROT_INCIDENTS] = s;

    // Filter chip bar
    incidents_filterbar = lv_obj_create(s);
    lv_obj_set_size(incidents_filterbar, SCREEN_WIDTH - 12, 22);
    lv_obj_set_pos(incidents_filterbar, 6, 24);
    lv_obj_set_style_bg_color(incidents_filterbar, COL_BG, 0);
    lv_obj_set_style_border_width(incidents_filterbar, 0, 0);
    lv_obj_set_style_pad_all(incidents_filterbar, 0, 0);
    lv_obj_clear_flag(incidents_filterbar, LV_OBJ_FLAG_SCROLLABLE);
    addFilterChip(incidents_filterbar, "ALL",   FLT_ALL,     0);
    addFilterChip(incidents_filterbar, "TRIG",  FLT_TRIG,    52);
    addFilterChip(incidents_filterbar, "ACK",   FLT_ACK,     104);
    addFilterChip(incidents_filterbar, "SVC",   FLT_SERVICE, 156);

    incidents_list = lv_obj_create(s);
    lv_obj_set_size(incidents_list, SCREEN_WIDTH - 12, SCREEN_HEIGHT - 22 - 22 - 26);
    lv_obj_set_pos(incidents_list, 6, 50);
    lv_obj_set_style_bg_color(incidents_list, COL_BG, 0);
    lv_obj_set_style_border_width(incidents_list, 0, 0);
    lv_obj_set_style_pad_all(incidents_list, 2, 0);
    lv_obj_set_scrollbar_mode(incidents_list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_scroll_dir(incidents_list, LV_DIR_VER);
    lv_obj_add_flag(incidents_list, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lbl_incidents_empty = lv_label_create(s);
    lv_label_set_text(lbl_incidents_empty, "No open incidents");
    lv_obj_set_style_text_color(lbl_incidents_empty, COL_MUT, 0);
    lv_obj_set_style_text_font(lbl_incidents_empty, &outfit_bold_14, 0);
    lv_obj_align(lbl_incidents_empty, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(lbl_incidents_empty, LV_OBJ_FLAG_HIDDEN);
}

static void buildServices() {
    if (scr_rot[ROT_SERVICES]) return;
    lv_obj_t* s = makeRotScreen(ROT_SERVICES);
    scr_rot[ROT_SERVICES] = s;

    services_list = lv_obj_create(s);
    lv_obj_set_size(services_list, SCREEN_WIDTH - 12, SCREEN_HEIGHT - 22 - 22);
    lv_obj_set_pos(services_list, 6, 24);
    lv_obj_set_style_bg_color(services_list, COL_BG, 0);
    lv_obj_set_style_border_width(services_list, 0, 0);
    lv_obj_set_style_pad_all(services_list, 2, 0);
    lv_obj_set_scrollbar_mode(services_list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_scroll_dir(services_list, LV_DIR_VER);
    lv_obj_add_flag(services_list, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

static void buildOnCall() {
    if (scr_rot[ROT_ONCALL]) return;
    lv_obj_t* s = makeRotScreen(ROT_ONCALL);
    scr_rot[ROT_ONCALL] = s;

    oncall_list = lv_obj_create(s);
    lv_obj_set_size(oncall_list, SCREEN_WIDTH - 12, SCREEN_HEIGHT - 22 - 22);
    lv_obj_set_pos(oncall_list, 6, 24);
    lv_obj_set_style_bg_color(oncall_list, COL_BG, 0);
    lv_obj_set_style_border_width(oncall_list, 0, 0);
    lv_obj_set_style_pad_all(oncall_list, 2, 0);
    lv_obj_set_scrollbar_mode(oncall_list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_scroll_dir(oncall_list, LV_DIR_VER);
    lv_obj_add_flag(oncall_list, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

static void onInfoBack(lv_event_t*) {
    if (scr_rot[ROT_OVERVIEW]) {
        lv_scr_load_anim(scr_rot[ROT_OVERVIEW], LV_SCR_LOAD_ANIM_OVER_RIGHT, 200, 0, false);
    }
}

static void buildInfo() {
    if (scr_info_off) return;
    scr_info_off = lv_obj_create(nullptr);
    styleBg(scr_info_off);

    // Compact header with BACK button
    lv_obj_t* hdr = lv_obj_create(scr_info_off);
    lv_obj_set_size(hdr, SCREEN_WIDTH, 22);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, COL_PANEL, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* back = lv_btn_create(hdr);
    lv_obj_set_size(back, 50, 20);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_color(back, COL_PD_GREEN, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_set_style_radius(back, 5, 0);
    lv_obj_t* bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " BACK");
    lv_obj_set_style_text_font(bl, &outfit_bold_12, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(0x062), 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, onInfoBack, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* t = lv_label_create(hdr);
    lv_label_set_text(t, "STATUS");
    lv_obj_set_style_text_color(t, COL_INK, 0);
    lv_obj_set_style_text_font(t, &outfit_bold_12, 0);
    lv_obj_set_style_text_letter_space(t, 3, 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 0);

    info_grid = lv_obj_create(scr_info_off);
    lv_obj_set_size(info_grid, SCREEN_WIDTH - 16, SCREEN_HEIGHT - 30);
    lv_obj_set_pos(info_grid, 8, 26);
    lv_obj_set_style_bg_color(info_grid, COL_BG, 0);
    lv_obj_set_style_border_width(info_grid, 0, 0);
    lv_obj_set_style_pad_all(info_grid, 2, 0);
    lv_obj_set_style_pad_row(info_grid, 2, 0);
    lv_obj_set_flex_flow(info_grid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info_grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(info_grid, LV_OBJ_FLAG_SCROLLABLE);

    // Allow swipe right to go back too
    lv_obj_add_event_cb(scr_info_off, [](lv_event_t* e){
        lv_indev_t* in = lv_indev_get_act();
        if (!in) return;
        if (lv_indev_get_gesture_dir(in) == LV_DIR_RIGHT) onInfoBack(e);
    }, LV_EVENT_GESTURE, nullptr);
}

// ============================================================================
// Refreshers
// ============================================================================

static void updateOverview() {
    if (!scr_rot[ROT_OVERVIEW]) return;
    const auto& c = pd::counts();
    if (lbl_open) lv_label_set_text_fmt(lbl_open, "%d", c.triggered);
    if (lbl_ack)  lv_label_set_text_fmt(lbl_ack,  "%d", c.acknowledged);

    if (lbl_overview_status) {
        if (c.ok) {
            uint32_t age = (millis() - c.lastFetchMs) / 1000;
            char buf[64];
            snprintf(buf, sizeof(buf), "LAST POLL %us AGO  -  %d OPEN", (unsigned)age, c.triggered + c.acknowledged);
            lv_label_set_text(lbl_overview_status, buf);
        } else if (c.error.length()) {
            String s = String("ERR: ") + c.error;
            lv_label_set_text(lbl_overview_status, s.c_str());
        }
    }
}

static lv_color_t severityColor(const pd::Incident& i) {
    if (i.status == "triggered")    return COL_OPEN;   // red
    if (i.status == "acknowledged") return COL_ACK;    // yellow
    return COL_OK;                                     // green
}

static bool incidentMatchesFilter(const pd::Incident& i) {
    switch (g_filter) {
        case FLT_TRIG:    return i.status == "triggered";
        case FLT_ACK:     return i.status == "acknowledged";
        case FLT_SERVICE: return g_filterService.length() == 0 ? true
                                                                : i.service == g_filterService;
        case FLT_ALL:
        default:          return true;
    }
}

static void rebuildIncidentsList() {
    if (!scr_rot[ROT_INCIDENTS]) return;
    lv_obj_clean(incidents_list);
    const auto& list = pd::recent();

    int shown = 0;
    for (size_t i = 0; i < list.size(); ++i) {
        if (!incidentMatchesFilter(list[i])) continue;
        shown++;
    }
    if (shown == 0) {
        lv_obj_clear_flag(lbl_incidents_empty, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(lbl_incidents_empty, LV_OBJ_FLAG_HIDDEN);

    int rowIdx = 0;
    const int ROW_H = 60, ROW_GAP = 4, INNER_W = SCREEN_WIDTH - 16;
    for (size_t i = 0; i < list.size(); ++i) {
        if (!incidentMatchesFilter(list[i])) continue;
        const pd::Incident& inc = list[i];
        lv_color_t accent = severityColor(inc);

        lv_obj_t* row = lv_obj_create(incidents_list);
        lv_obj_set_size(row, INNER_W, ROW_H);
        lv_obj_set_pos(row, 0, rowIdx * (ROW_H + ROW_GAP));
        rowIdx++;
        lv_obj_set_style_bg_color(row, COL_PANEL, 0);
        lv_obj_set_style_border_color(row, COL_LINE, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void*)i);
        lv_obj_add_event_cb(row, onIncidentClicked, LV_EVENT_CLICKED, nullptr);

        // Severity badge (24x24 PNG)
        lv_obj_t* badgeImg = lv_img_create(row);
        lv_img_set_src(badgeImg, sevBadge(inc));
        lv_obj_align(badgeImg, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_clear_flag(badgeImg, LV_OBJ_FLAG_CLICKABLE);

        // Status pill (TRIG/ACK) at top-right of the row
        lv_obj_t* urg = lv_label_create(row);
        lv_label_set_text(urg, (inc.status == "triggered" ? "TRIG" : "ACK"));
        lv_obj_set_style_text_color(urg, accent, 0);
        lv_obj_set_style_text_font(urg, &outfit_bold_12, 0);
        lv_obj_set_style_text_letter_space(urg, 1, 0);
        lv_obj_align(urg, LV_ALIGN_TOP_RIGHT, 0, 0);

        // Title row #1 — explicit absolute pos, zero label padding
        char prefixed[128];
        snprintf(prefixed, sizeof(prefixed), "#%s  %s",
                 inc.number.c_str(), inc.title.c_str());
        lv_obj_t* title = lv_label_create(row);
        lv_obj_set_style_pad_all(title, 0, 0);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_width(title, INNER_W - 80);
        lv_obj_set_height(title, 18);
        lv_label_set_text(title, prefixed);
        lv_obj_set_style_text_color(title, COL_INK, 0);
        lv_obj_set_style_text_font(title, &outfit_bold_14, 0);
        lv_obj_set_style_pad_top(title, 0, 0);
        lv_obj_set_style_pad_bottom(title, 0, 0);
        lv_obj_set_pos(title, 30, 2);

        // Service row #2 — y=32 so there's a guaranteed 12 px gap below title
        lv_obj_t* svc = lv_label_create(row);
        lv_obj_set_style_pad_all(svc, 0, 0);
        lv_label_set_long_mode(svc, LV_LABEL_LONG_DOT);
        lv_obj_set_width(svc, INNER_W - 40);
        lv_obj_set_height(svc, 16);
        lv_label_set_text(svc, inc.service.length() ? inc.service.c_str() : "—");
        lv_obj_set_style_text_color(svc, COL_MUT, 0);
        lv_obj_set_style_text_font(svc, &outfit_bold_12, 0);
        lv_obj_set_pos(svc, 30, 32);
    }
}

static void rebuildServicesList() {
    if (!scr_rot[ROT_SERVICES]) return;
    lv_obj_clean(services_list);
    const auto& list = pd::recent();

    // Tally per service (name -> {triggered, acked})
    std::map<String, std::pair<int,int>> tally;
    for (const auto& inc : list) {
        if (inc.service.length() == 0) continue;
        auto& p = tally[inc.service];
        if (inc.status == "triggered") p.first++;
        else if (inc.status == "acknowledged") p.second++;
    }
    if (tally.empty()) {
        lv_obj_t* none = lv_label_create(services_list);
        lv_label_set_text(none, "No services with open incidents");
        lv_obj_set_style_text_color(none, COL_MUT, 0);
        lv_obj_set_style_text_font(none, &outfit_bold_14, 0);
        return;
    }
    // Sort: triggered desc, then acked desc, then name
    std::vector<std::pair<String, std::pair<int,int>>> rows(tally.begin(), tally.end());
    typedef std::pair<String, std::pair<int,int>> SvcRow;
    std::sort(rows.begin(), rows.end(), [](const SvcRow& a, const SvcRow& b){
        if (a.second.first  != b.second.first)  return a.second.first  > b.second.first;
        if (a.second.second != b.second.second) return a.second.second > b.second.second;
        return a.first < b.first;
    });

    static std::vector<String> svcKeys; svcKeys.clear();
    for (const auto& kv : rows) svcKeys.push_back(kv.first);

    int idxN = 0;
    const int SVC_ROW_H = 40, SVC_GAP = 4, SVC_W = SCREEN_WIDTH - 16;
    for (const auto& kv : rows) {
        int trig = kv.second.first, ack = kv.second.second;
        lv_color_t accent = (trig > 0) ? COL_OPEN : (ack > 0 ? COL_ACK : COL_OK);

        lv_obj_t* row = lv_obj_create(services_list);
        lv_obj_set_size(row, SVC_W, SVC_ROW_H);
        lv_obj_set_pos(row, 0, idxN * (SVC_ROW_H + SVC_GAP));
        lv_obj_set_style_bg_color(row, COL_PANEL, 0);
        lv_obj_set_style_border_color(row, COL_LINE, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void*)(intptr_t)idxN);
        lv_obj_add_event_cb(row, [](lv_event_t* e){
            int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
            if (idx < 0 || idx >= (int)svcKeys.size()) return;
            g_filter = FLT_SERVICE;
            g_filterService = svcKeys[idx];
            rotateTo(ROT_INCIDENTS, true);
            rebuildIncidentsList();
        }, LV_EVENT_CLICKED, nullptr);
        idxN++;

        lv_obj_t* badge = lv_obj_create(row);
        lv_obj_set_size(badge, 4, 24);
        lv_obj_align(badge, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_bg_color(badge, accent, 0);
        lv_obj_set_style_border_width(badge, 0, 0);
        lv_obj_set_style_radius(badge, 2, 0);
        lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* name = lv_label_create(row);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, SCREEN_WIDTH - 130);
        lv_label_set_text(name, kv.first.c_str());
        lv_obj_set_style_text_color(name, COL_INK, 0);
        lv_obj_set_style_text_font(name, &outfit_bold_14, 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 12, 0);

        // count pills
        char buf[32];
        snprintf(buf, sizeof(buf), "%d / %d", trig, ack);
        lv_obj_t* cnt = lv_label_create(row);
        lv_label_set_text(cnt, buf);
        lv_obj_set_style_text_color(cnt, accent, 0);
        lv_obj_set_style_text_font(cnt, &outfit_bold_16, 0);
        lv_obj_align(cnt, LV_ALIGN_RIGHT_MID, -4, 0);
    }
}

static std::vector<pd::OnCall> g_lastOnCalls;
extern std::vector<pd::OnCall> g_uiOnCallsBuffer;   // defined in main.cpp

static void rebuildOnCallList() {
    if (!scr_rot[ROT_ONCALL]) return;
    lv_obj_clean(oncall_list);
    if (g_lastOnCalls.empty()) {
        lv_obj_t* none = lv_label_create(oncall_list);
        lv_label_set_text(none, "No on-call data\n(swipe down to refresh)");
        lv_obj_set_style_text_color(none, COL_MUT, 0);
        lv_obj_set_style_text_font(none, &outfit_bold_14, 0);
        return;
    }
    int rowIdx = 0;
    const int OC_ROW_H = 52, OC_GAP = 4, OC_W = SCREEN_WIDTH - 16;
    for (const auto& oc : g_lastOnCalls) {
        lv_color_t accent = (oc.level == 1) ? COL_PD_GREEN : (oc.level == 2 ? COL_ACK : COL_MUT);
        lv_obj_t* row = lv_obj_create(oncall_list);
        lv_obj_set_size(row, OC_W, OC_ROW_H);
        lv_obj_set_pos(row, 0, rowIdx * (OC_ROW_H + OC_GAP));
        rowIdx++;
        lv_obj_set_style_bg_color(row, COL_PANEL, 0);
        lv_obj_set_style_border_color(row, accent, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // Level pill at left
        char lv[8]; snprintf(lv, sizeof(lv), "L%d", oc.level);
        lv_obj_t* lvl = lv_label_create(row);
        lv_label_set_text(lvl, lv);
        lv_obj_set_style_text_color(lvl, accent, 0);
        lv_obj_set_style_text_font(lvl, &outfit_bold_18, 0);
        lv_obj_set_pos(lvl, 0, 12);

        // User name
        lv_obj_t* user = lv_label_create(row);
        lv_label_set_long_mode(user, LV_LABEL_LONG_DOT);
        lv_obj_set_width(user, OC_W - 40);
        lv_label_set_text(user, oc.user.length() ? oc.user.c_str() : "(direct)");
        lv_obj_set_style_text_color(user, COL_INK, 0);
        lv_obj_set_style_text_font(user, &outfit_bold_14, 0);
        lv_obj_set_pos(user, 30, 2);

        // Escalation policy + schedule (combined, dimmer second line)
        String sub = oc.escalationPolicy.length() ? oc.escalationPolicy : String("(no EP)");
        if (oc.schedule.length() && oc.schedule != "(direct)") sub += "  -  " + oc.schedule;
        lv_obj_t* sl = lv_label_create(row);
        lv_label_set_long_mode(sl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(sl, OC_W - 40);
        lv_label_set_text(sl, sub.c_str());
        lv_obj_set_style_text_color(sl, COL_MUT, 0);
        lv_obj_set_style_text_font(sl, &outfit_bold_12, 0);
        lv_obj_set_pos(sl, 30, 24);
    }
}

static void addInfoRow(lv_obj_t* parent, const char* k, const String& v, lv_color_t col=COL_INK) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), 26);
    lv_obj_set_style_bg_color(row, COL_PANEL, 0);
    lv_obj_set_style_border_color(row, COL_LINE, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lk = lv_label_create(row);
    lv_label_set_text(lk, k);
    lv_obj_set_style_text_color(lk, COL_MUT, 0);
    lv_obj_set_style_text_font(lk, &outfit_bold_12, 0);
    lv_obj_set_style_text_letter_space(lk, 1, 0);
    lv_obj_align(lk, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t* lv = lv_label_create(row);
    lv_label_set_long_mode(lv, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lv, SCREEN_WIDTH - 110);
    lv_label_set_text(lv, v.c_str());
    lv_obj_set_style_text_color(lv, col, 0);
    lv_obj_set_style_text_font(lv, &outfit_bold_14, 0);
    lv_obj_align(lv, LV_ALIGN_RIGHT_MID, -4, 0);
}

static void rebuildInfo() {
    if (!scr_info_off || !info_grid) return;
    lv_obj_clean(info_grid);

    addInfoRow(info_grid, "WIFI",   WiFi.SSID());
    addInfoRow(info_grid, "IP",     WiFi.localIP().toString());
    addInfoRow(info_grid, "RSSI",   String(WiFi.RSSI()) + " dBm");
    addInfoRow(info_grid, "REGION", storage::getRegion() == "eu" ? "EU" : "US");
    String url = String("http://pagerduty.local");
    addInfoRow(info_grid, "PORTAL", url, COL_PD_GREEN);
    addInfoRow(info_grid, "HEAP",   String((unsigned)ESP.getFreeHeap()) + " B free");
    const auto& c = pd::counts();
    if (c.lastFetchMs) {
        uint32_t age = (millis() - c.lastFetchMs) / 1000;
        addInfoRow(info_grid, "LAST POLL",
            String(age) + "s " + (c.ok ? "OK" : ("ERR: " + c.error)),
            c.ok ? COL_OK : COL_OPEN);
    } else {
        addInfoRow(info_grid, "LAST POLL", "pending");
    }
}

// ============================================================================
// Rotation + gestures
// ============================================================================

static void setActiveDots(int idx) {
    for (int i = 0; i < ROT_COUNT; ++i) {
        if (page_dots[idx][i]) {
            lv_obj_set_style_bg_color(page_dots[idx][i],
                (i == idx) ? COL_PD_GREEN : COL_DIM, 0);
        }
    }
}

static void rotateTo(int idx, bool fromUser) {
    idx = ((idx % ROT_COUNT) + ROT_COUNT) % ROT_COUNT;
    if (!scr_rot[idx]) return;

    lv_disp_t* d = lv_disp_get_default();
    // Already on the target, or animation already heading there — no-op.
    if (lv_scr_act() == scr_rot[idx] && d->scr_to_load == nullptr) return;
    if (d->scr_to_load == scr_rot[idx]) return;

    setActiveDots(idx);

    // First transition into the dashboard from a modal screen (Connecting/Setup/Waiting)
    // — use a hard load so we don't fight an in-flight animation.
    bool fromModal = (lv_scr_act() == scr_connecting ||
                      lv_scr_act() == scr_setup ||
                      lv_scr_act() == scr_waiting);
    if (fromModal) {
        // Use anim with time=0 (no animation) — uses LVGL's full load machinery
        // and handles dirty-region tracking properly.
        lv_scr_load_anim(scr_rot[idx], LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
    } else {
        lv_scr_load_anim_t anim = LV_SCR_LOAD_ANIM_MOVE_LEFT;
        if (idx < g_rotIdx) anim = LV_SCR_LOAD_ANIM_MOVE_RIGHT;
        lv_scr_load_anim(scr_rot[idx], anim, 220, 0, false);
    }
    g_rotIdx = idx;
    if (fromUser) g_pauseUntilMs = millis() + MANUAL_PAUSE_MS;

    // Lazy-refresh the screen we just landed on
    if (idx == ROT_INCIDENTS) rebuildIncidentsList();
    if (idx == ROT_SERVICES)  rebuildServicesList();
}

static void onScreenGesture(lv_event_t* e) {
    lv_indev_t* in = lv_indev_get_act();
    if (!in) return;
    lv_dir_t d = lv_indev_get_gesture_dir(in);
    if (d == LV_DIR_LEFT)        rotateTo(g_rotIdx + 1, true);
    else if (d == LV_DIR_RIGHT)  rotateTo(g_rotIdx - 1, true);
    else if (d == LV_DIR_BOTTOM) {
        g_refreshNow = true;
        // brief flash of LIVE indicator to acknowledge the gesture
        for (int i = 0; i < ROT_COUNT; ++i) if (status_label[i])
            lv_label_set_text(status_label[i], "REFRESH");
    }
}

static void onRotateTick(lv_timer_t*) {
    if (!g_dashboardStarted) return;
    if (lv_scr_act() == scr_detail) return;
    // Only rotate if user is on a rotating screen
    bool onRot = false;
    for (int i = 0; i < ROT_COUNT; ++i) if (lv_scr_act() == scr_rot[i]) { onRot = true; break; }
    if (!onRot) return;
    if (millis() < g_pauseUntilMs) return;
    rotateTo(g_rotIdx + 1, false);
}

// ============================================================================
// Incident drill-down
// ============================================================================

static void hideToastTimer_fwd(lv_timer_t* t);
static void showToast(const String& msg, lv_color_t color) {
    if (!detail_toast) return;
    lv_label_set_text(detail_toast, msg.c_str());
    lv_obj_set_style_text_color(detail_toast, color, 0);
    lv_obj_clear_flag(detail_toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(hideToastTimer_fwd, 4000, nullptr);
}
static void hideToastTimer_fwd(lv_timer_t* t) {
    if (detail_toast) lv_obj_add_flag(detail_toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_del(t);
}

static void buildIncidentDetail(const pd::Incident& inc) {
    if (!scr_detail) {
        scr_detail = lv_obj_create(nullptr);
        styleBg(scr_detail);
    } else {
        lv_obj_clean(scr_detail);
    }
    g_detailInc = inc;
    detail_timeline_list = nullptr;
    detail_timeline_loading = nullptr;

    lv_color_t accent = severityColor(inc);

    // Header
    lv_obj_t* hdr = lv_obj_create(scr_detail);
    lv_obj_set_size(hdr, SCREEN_WIDTH, 26);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, COL_PANEL, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    char numbuf[40];
    snprintf(numbuf, sizeof(numbuf), "INCIDENT #%s  -  %s",
             inc.number.c_str(),
             inc.status == "triggered" ? "TRIG" : (inc.status == "acknowledged" ? "ACK" : inc.status.c_str()));
    lv_obj_t* tt = lv_label_create(hdr);
    lv_label_set_text(tt, numbuf);
    lv_obj_set_style_text_color(tt, accent, 0);
    lv_obj_set_style_text_font(tt, &outfit_bold_12, 0);
    lv_obj_set_style_text_letter_space(tt, 2, 0);
    lv_obj_align(tt, LV_ALIGN_CENTER, 0, 0);

    // Top accent bar (full width, 4px, color by severity)
    lv_obj_t* accent_bar = lv_obj_create(scr_detail);
    lv_obj_set_size(accent_bar, SCREEN_WIDTH, 4);
    lv_obj_set_pos(accent_bar, 0, 26);
    lv_obj_set_style_bg_color(accent_bar, accent, 0);
    lv_obj_set_style_border_width(accent_bar, 0, 0);
    lv_obj_set_style_radius(accent_bar, 0, 0);
    lv_obj_clear_flag(accent_bar, LV_OBJ_FLAG_SCROLLABLE);

    // Severity badge — top-right, larger (placed in title area)
    lv_obj_t* sevImg = lv_img_create(scr_detail);
    lv_img_set_src(sevImg, sevBadge(inc));
    lv_obj_set_pos(sevImg, SCREEN_WIDTH - 36, 36);

    // Title wraps to 2 lines next to badge
    lv_obj_t* title = lv_label_create(scr_detail);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, SCREEN_WIDTH - 50);
    lv_label_set_text(title, inc.title.c_str());
    lv_obj_set_style_text_color(title, COL_INK, 0);
    lv_obj_set_style_text_font(title, &outfit_bold_16, 0);
    lv_obj_set_style_text_line_space(title, 1, 0);
    lv_obj_set_pos(title, 8, 36);

    // Compact meta row: service · responder
    String meta = inc.service.length() ? inc.service : String("-");
    if (inc.responder.length()) meta += "  -  " + inc.responder;
    else                         meta += "  -  unassigned";
    lv_obj_t* metaLbl = lv_label_create(scr_detail);
    lv_label_set_long_mode(metaLbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(metaLbl, SCREEN_WIDTH - 16);
    lv_label_set_text(metaLbl, meta.c_str());
    lv_obj_set_style_text_color(metaLbl, COL_MUT, 0);
    lv_obj_set_style_text_font(metaLbl, &outfit_bold_12, 0);
    lv_obj_set_pos(metaLbl, 8, 90);

    // TIMELINE section header
    lv_obj_t* tlHdr = lv_label_create(scr_detail);
    lv_label_set_text(tlHdr, "TIMELINE");
    lv_obj_set_style_text_color(tlHdr, COL_PD_GREEN, 0);
    lv_obj_set_style_text_font(tlHdr, &outfit_bold_12, 0);
    lv_obj_set_style_text_letter_space(tlHdr, 3, 0);
    lv_obj_set_pos(tlHdr, 8, 114);

    // Timeline list (scrollable) — leaves a 32 px action bar at the bottom
    detail_timeline_list = lv_obj_create(scr_detail);
    lv_obj_set_size(detail_timeline_list, SCREEN_WIDTH - 16, SCREEN_HEIGHT - 166);
    lv_obj_set_pos(detail_timeline_list, 8, 132);
    lv_obj_set_style_bg_color(detail_timeline_list, COL_BG, 0);
    lv_obj_set_style_border_width(detail_timeline_list, 0, 0);
    lv_obj_set_style_pad_all(detail_timeline_list, 2, 0);
    lv_obj_set_style_pad_row(detail_timeline_list, 3, 0);
    lv_obj_set_flex_flow(detail_timeline_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(detail_timeline_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(detail_timeline_list, LV_SCROLLBAR_MODE_ACTIVE);
    // Let swipe gestures bubble up to scr_detail so the user can swipe-back
    lv_obj_set_scroll_dir(detail_timeline_list, LV_DIR_VER);
    lv_obj_add_flag(detail_timeline_list, LV_OBJ_FLAG_GESTURE_BUBBLE);

    // Loading placeholder inside the list
    detail_timeline_loading = lv_label_create(detail_timeline_list);
    lv_label_set_text(detail_timeline_loading, "Loading timeline...");
    lv_obj_set_style_text_color(detail_timeline_loading, COL_MUT, 0);
    lv_obj_set_style_text_font(detail_timeline_loading, &outfit_bold_12, 0);

    // ====== Action bar at bottom: BACK · ACK · RESOLVE ======
    auto makeAction = [&](const char* label, lv_color_t color, int x, int w,
                          lv_event_cb_t cb, bool enabled) -> lv_obj_t* {
        lv_obj_t* b = lv_btn_create(scr_detail);
        lv_obj_set_size(b, w, 28);
        lv_obj_set_pos(b, x, SCREEN_HEIGHT - 32);
        lv_obj_set_style_bg_color(b, enabled ? color : COL_DIM, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_radius(b, 6, 0);
        if (enabled) {
            lv_obj_set_style_shadow_color(b, color, 0);
            lv_obj_set_style_shadow_width(b, 8, 0);
            lv_obj_set_style_shadow_opa(b, LV_OPA_30, 0);
        }
        lv_obj_t* lbl = lv_label_create(b);
        lv_label_set_text(lbl, label);
        lv_obj_set_style_text_font(lbl, &outfit_bold_12, 0);
        lv_obj_set_style_text_color(lbl, enabled ? lv_color_hex(0x062) : COL_MUT, 0);
        lv_obj_set_style_text_letter_space(lbl, 2, 0);
        lv_obj_center(lbl);
        if (enabled) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
        else         lv_obj_add_state(b, LV_STATE_DISABLED);
        return b;
    };

    // Two-button bar: BACK + ACK only — clean and unambiguous, no keyboard needed
    int gap = 8;
    int btnW = (SCREEN_WIDTH - 12 - gap) / 2;
    int x0 = 6;

    makeAction(LV_SYMBOL_LEFT " BACK", COL_PD_GREEN, x0, btnW, onDetailBack, true);

    bool canAck = (inc.status == "triggered");
    btn_ack = makeAction("ACK", COL_ACK, x0 + btnW + gap, btnW,
        [](lv_event_t*){
            g_pendingActionId = g_detailInc.id;
            g_pendingActionStatus = "acknowledged";
            showToast("Acking...", COL_ACK);
        }, canAck);
    btn_resolve = nullptr;

    // Toast strip just above the action bar — hidden until something is shown
    detail_toast = lv_label_create(scr_detail);
    lv_label_set_long_mode(detail_toast, LV_LABEL_LONG_DOT);
    lv_obj_set_width(detail_toast, SCREEN_WIDTH - 16);
    lv_label_set_text(detail_toast, "");
    lv_obj_set_style_text_color(detail_toast, COL_PD_GREEN, 0);
    lv_obj_set_style_text_font(detail_toast, &outfit_bold_12, 0);
    lv_obj_set_style_text_align(detail_toast, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(detail_toast, COL_BG, 0);
    lv_obj_set_style_bg_opa(detail_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(detail_toast, 2, 0);
    lv_obj_set_pos(detail_toast, 8, SCREEN_HEIGHT - 54);
    lv_obj_add_flag(detail_toast, LV_OBJ_FLAG_HIDDEN);

    // Allow swipe right OR long-press anywhere to go back (escape hatch)
    lv_obj_add_event_cb(scr_detail, [](lv_event_t* e){
        lv_indev_t* in = lv_indev_get_act();
        if (!in) return;
        lv_dir_t d = lv_indev_get_gesture_dir(in);
        if (d == LV_DIR_RIGHT) onDetailBack(e);
    }, LV_EVENT_GESTURE, nullptr);
    lv_obj_add_flag(scr_detail, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr_detail, onDetailBack, LV_EVENT_LONG_PRESSED, nullptr);
}

static lv_color_t logEntryColor(const String& type) {
    if (type.indexOf("trigger")     >= 0) return COL_OPEN;
    if (type.indexOf("acknowledge") >= 0) return COL_ACK;
    if (type.indexOf("resolve")     >= 0) return COL_OK;
    if (type.indexOf("note")        >= 0) return COL_INFO;
    if (type.indexOf("escalate")    >= 0) return lv_color_hex(0xFF8C42);
    return COL_MUT;
}

static void renderTimeline() {
    if (!detail_timeline_list) return;
    lv_obj_clean(detail_timeline_list);

    if (g_lastTimeline.empty()) {
        lv_obj_t* none = lv_label_create(detail_timeline_list);
        lv_label_set_text(none, "(no timeline events)");
        lv_obj_set_style_text_color(none, COL_MUT, 0);
        lv_obj_set_style_text_font(none, &outfit_bold_12, 0);
        return;
    }

    int maxRows = 8;
    int shown = 0;
    for (const auto& le : g_lastTimeline) {
        if (shown >= maxRows) break;
        shown++;
        lv_color_t accent = logEntryColor(le.type);

        lv_obj_t* row = lv_obj_create(detail_timeline_list);
        if (!row) { Serial.println("[ui] timeline row alloc fail"); break; }
        lv_obj_set_size(row, lv_pct(100), 26);
        lv_obj_set_style_bg_color(row, COL_PANEL, 0);
        lv_obj_set_style_border_color(row, accent, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // One label per row: "HH:MM  summary [— agent]"
        String t = le.createdAt;
        int tIdx = t.indexOf('T');
        if (tIdx >= 0 && (int)t.length() >= tIdx + 6) t = t.substring((unsigned)(tIdx + 1), (unsigned)(tIdx + 6));
        else t = "--:--";

        String line = t + "  " + le.summary;
        if (le.agent.length()) line += "  -  " + le.agent;

        lv_obj_t* lbl = lv_label_create(row);
        if (!lbl) break;
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, SCREEN_WIDTH - 30);
        lv_label_set_text(lbl, line.c_str());
        lv_obj_set_style_text_color(lbl, COL_INK, 0);
        lv_obj_set_style_text_font(lbl, &outfit_bold_12, 0);
        lv_obj_center(lbl);
    }
}

bool ui::timelineFetchPending(String& outId) {
    if (g_pendingTimelineId.length() == 0) return false;
    outId = g_pendingTimelineId;
    return true;
}

extern std::vector<pd::LogEntry> g_uiTimelineBuffer;  // defined in main.cpp

void ui::applyTimeline() {
    g_pendingTimelineId = "";
    g_lastTimeline = ::g_uiTimelineBuffer;
    renderTimeline();
}

bool ui::actionPending(String& outId, String& outStatus) {
    if (g_pendingActionId.length() == 0) return false;
    outId = g_pendingActionId;
    outStatus = g_pendingActionStatus;
    return true;
}

void ui::applyActionResult(bool ok, const String& msg) {
    g_pendingActionId = "";
    g_pendingActionStatus = "";
    showToast(msg, ok ? COL_PD_GREEN : COL_OPEN);
}

bool ui::snoozePending(String& outId, int& outSec) {
    if (g_pendingSnoozeId.length() == 0) return false;
    outId = g_pendingSnoozeId;
    outSec = g_pendingSnoozeSec;
    return true;
}

bool ui::notePending(String& outId, String& outText) {
    if (g_pendingNoteId.length() == 0) return false;
    outId = g_pendingNoteId;
    outText = g_pendingNoteText;
    return true;
}

bool ui::refreshRequested() {
    if (!g_refreshNow) return false;
    g_refreshNow = false;
    return true;
}

void ui::notifyOnCallsRefreshed() {
    g_lastOnCalls = ::g_uiOnCallsBuffer;
    rebuildOnCallList();
}

void ui::notifyMttr(int seconds, int sampleCount) {
    if (!lbl_overview_mttr) return;
    if (sampleCount == 0) {
        lv_label_set_text(lbl_overview_mttr, "MTTR --");
    } else {
        char buf[32];
        if (seconds < 60)              snprintf(buf, sizeof(buf), "MTTR %ds (%d)", seconds, sampleCount);
        else if (seconds < 3600)       snprintf(buf, sizeof(buf), "MTTR %dm (%d)", seconds/60, sampleCount);
        else                           snprintf(buf, sizeof(buf), "MTTR %dh%02dm",  seconds/3600, (seconds%3600)/60);
        lv_label_set_text(lbl_overview_mttr, buf);
    }
}

void ui::setClockText(const String& hhmm) {
    for (int i = 0; i < ROT_COUNT; ++i) if (clock_label[i])
        lv_label_set_text(clock_label[i], hhmm.c_str());
}

static void onIncidentClicked(lv_event_t* e) {
    Serial.println("[ui] incident click");
    lv_obj_t* row = lv_event_get_target(e);
    size_t idx = (size_t)lv_obj_get_user_data(row);
    const auto& list = pd::recent();
    Serial.printf("[ui] click idx=%u list_size=%u heap=%u\n",
                  (unsigned)idx, (unsigned)list.size(), (unsigned)ESP.getFreeHeap());
    if (idx >= list.size()) return;
    Serial.printf("[ui] building detail for #%s\n", list[idx].number.c_str());
    buildIncidentDetail(list[idx]);
    Serial.println("[ui] detail built");
    g_pendingTimelineId = list[idx].id;
    lv_scr_load_anim(scr_detail, LV_SCR_LOAD_ANIM_OVER_LEFT, 200, 0, false);
    Serial.println("[ui] detail loaded");
}

static void onDetailBack(lv_event_t*) {
    if (!scr_rot[ROT_INCIDENTS]) return;
    lv_scr_load_anim(scr_rot[ROT_INCIDENTS], LV_SCR_LOAD_ANIM_OVER_RIGHT, 200, 0, false);
}

// ============================================================================
// Public API
// ============================================================================

static void buildAll() {
    buildOverview();
    buildIncidents();
    buildServices();
    buildOnCall();
    buildInfo();
}

void ui::showOverview() {
    bool firstTime = !g_dashboardStarted;
    if (firstTime) {
        buildAll();
        g_dashboardStarted = true;
        if (!g_rotateTimer) {
            g_rotateTimer = lv_timer_create(onRotateTick, ROTATE_PERIOD_MS, nullptr);
        }
    }
    notifyDataRefreshed();
    rotateTo(ROT_OVERVIEW, true);
    // After the first transition into the dashboard, free the modal screens —
    // their persistence has been confusing LVGL's refresh.
    if (firstTime) {
        if (scr_connecting) { lv_obj_del(scr_connecting); scr_connecting = nullptr; }
        if (scr_setup)      { lv_obj_del(scr_setup);      scr_setup = nullptr;      }
        if (scr_waiting)    { lv_obj_del(scr_waiting);    scr_waiting = nullptr;    }
        Serial.printf("[ui] dashboard live act=%p heap=%u\n",
                      lv_scr_act(), (unsigned)ESP.getFreeHeap());
    }
}

void ui::showIncidents() {
    if (!g_dashboardStarted) showOverview();
    rotateTo(ROT_INCIDENTS, true);
}

void ui::notifyDataRefreshed() {
    updateOverview();   // Overview always updates (sparkline needs continuous data).
    // Only refresh visible-list screens to avoid LVGL pool fragmentation from churn.
    lv_obj_t* a = lv_scr_act();
    if (a == scr_rot[ROT_INCIDENTS]) rebuildIncidentsList();
    if (a == scr_rot[ROT_SERVICES])  rebuildServicesList();
    if (a == scr_info_off)           rebuildInfo();
}

ui::Screen ui::currentScreen() {
    lv_obj_t* s = lv_scr_act();
    // Any rotating screen + detail = "in dashboard" (no transition needed)
    for (int i = 0; i < ROT_COUNT; ++i) {
        if (s == scr_rot[i]) {
            return (i == ROT_INCIDENTS) ? Screen::Incidents : Screen::Overview;
        }
    }
    if (s == scr_detail || s == scr_info_off) return Screen::Overview;
    if (s == scr_waiting)            return Screen::Waiting;
    if (s == scr_setup)              return Screen::Setup;
    if (s == scr_connecting)         return Screen::Connecting;
    return Screen::None;
}

void ui::setStatusOnline(bool online) {
    g_online = online;
    for (int i = 0; i < ROT_COUNT; ++i) {
        if (status_dot[i])
            lv_obj_set_style_bg_color(status_dot[i], online ? COL_PD_GREEN : COL_OPEN, 0);
        if (status_label[i]) {
            lv_label_set_text(status_label[i], online ? "LIVE" : "OFFLINE");
            lv_obj_set_style_text_color(status_label[i], online ? COL_PD_GREEN : COL_OPEN, 0);
        }
    }
}

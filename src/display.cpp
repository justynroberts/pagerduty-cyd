#include "display.h"
#include "config.h"

#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <lvgl.h>

static TFT_eSPI tft = TFT_eSPI(SCREEN_HEIGHT, SCREEN_WIDTH); // 240x320 native
static SPIClass touchSpi = SPIClass(VSPI);
static XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

#define DRAW_BUF_LINES 8
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[SCREEN_WIDTH * DRAW_BUF_LINES];
static lv_disp_drv_t  disp_drv;
static lv_indev_drv_t indev_drv;

static void flushCb(lv_disp_drv_t* d, const lv_area_t* a, lv_color_t* px) {
    uint32_t w = a->x2 - a->x1 + 1;
    uint32_t h = a->y2 - a->y1 + 1;
    tft.startWrite();
    tft.setAddrWindow(a->x1, a->y1, w, h);
    tft.pushPixels((uint16_t*)px, w * h);
    tft.endWrite();
    lv_disp_flush_ready(d);
}

static void readCb(lv_indev_drv_t*, lv_indev_data_t* data) {
    if (!ts.tirqTouched() || !ts.touched()) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    TS_Point p = ts.getPoint();
    // Map raw -> pixels (landscape, rotation 1).
    int16_t x = map(p.x, TOUCH_X_MIN, TOUCH_X_MAX, 0, SCREEN_WIDTH  - 1);
    int16_t y = map(p.y, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, SCREEN_HEIGHT - 1);
    if (x < 0) x = 0; if (x >= SCREEN_WIDTH)  x = SCREEN_WIDTH  - 1;
    if (y < 0) y = 0; if (y >= SCREEN_HEIGHT) y = SCREEN_HEIGHT - 1;
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = x;
    data->point.y = y;
}

void display::begin() {
    tft.begin();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    touchSpi.begin(TOUCH_SCLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
    ts.begin(touchSpi);
    ts.setRotation(1);

    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, SCREEN_WIDTH * DRAW_BUF_LINES);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = SCREEN_WIDTH;
    disp_drv.ver_res  = SCREEN_HEIGHT;
    disp_drv.flush_cb = flushCb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = readCb;
    lv_indev_drv_register(&indev_drv);
}

void display::tick() { lv_timer_handler(); }

void display::setBacklight(uint8_t pct) {
    digitalWrite(TFT_BL, pct > 0 ? HIGH : LOW);
}

bool display::touched() { return ts.touched(); }

bool display::factoryResetPrompt(uint32_t holdMs) {
    // Sample once — if nothing pressed at boot, bail immediately.
    if (!ts.touched()) return false;

    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.drawString("FACTORY RESET", SCREEN_WIDTH / 2, 60);
    tft.setTextSize(1);
    tft.setTextColor(0xC638, TFT_BLACK);   // muted grey
    tft.drawString("Keep holding to wipe WiFi + token", SCREEN_WIDTH / 2, 92);
    tft.drawString("Release to cancel", SCREEN_WIDTH / 2, 108);

    const int barW = 220, barH = 14, barX = (SCREEN_WIDTH - barW) / 2, barY = 150;
    tft.drawRect(barX, barY, barW, barH, 0x07E0);   // green outline
    uint32_t start = millis();
    uint32_t last  = 0;
    while (millis() - start < holdMs) {
        if (!ts.touched()) {
            tft.fillScreen(TFT_BLACK);
            return false;
        }
        uint32_t fillW = ((millis() - start) * (barW - 2)) / holdMs;
        if (fillW != last) {
            tft.fillRect(barX + 1, barY + 1, fillW, barH - 2, 0x07E0);
            last = fillW;
        }
        delay(15);
    }
    tft.fillRect(barX + 1, barY + 1, barW - 2, barH - 2, 0x07E0);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(2);
    tft.drawString("WIPED", SCREEN_WIDTH / 2, 190);
    delay(700);
    return true;
}

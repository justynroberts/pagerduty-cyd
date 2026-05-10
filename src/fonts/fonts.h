#pragma once
#include <lvgl.h>

extern "C" {
extern const lv_font_t outfit_thin_12;
extern const lv_font_t outfit_thin_14;
extern const lv_font_t outfit_thin_16;
extern const lv_font_t outfit_thin_18;
extern const lv_font_t outfit_thin_22;
extern const lv_font_t outfit_thin_48;
extern const lv_font_t outfit_bold_12;
extern const lv_font_t outfit_bold_14;
extern const lv_font_t outfit_bold_16;
extern const lv_font_t outfit_bold_18;
extern const lv_font_t outfit_bold_22;
extern const lv_font_t outfit_bold_48;
}

// Pick weights for our use cases.
// Body text uses Thin for an airy, modern look.
// Numbers and emphasis use Bold for hierarchy.
#define FONT_TINY        (&outfit_thin_12)
#define FONT_BODY        (&outfit_thin_14)
#define FONT_BODY_BOLD   (&outfit_bold_14)
#define FONT_HEADER      (&outfit_bold_16)
#define FONT_SUB         (&outfit_thin_18)
#define FONT_HEADER_LG   (&outfit_bold_22)
#define FONT_HERO        (&outfit_bold_48)

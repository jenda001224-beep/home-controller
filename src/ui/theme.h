#pragma once
#include <lvgl.h>

// ============================================================
//  Apple Home-inspired colour palette
// ============================================================
#define C_BG          lv_color_hex(0x1C1C1E)   // system background
#define C_BG2         lv_color_hex(0x2C2C2E)   // card background
#define C_BG3         lv_color_hex(0x3A3A3C)   // pressed / subtle
#define C_ACCENT      lv_color_hex(0xFF9500)   // Apple orange (active)
#define C_ACCENT2     lv_color_hex(0x0A84FF)   // Apple blue (secondary)
#define C_TEXT        lv_color_hex(0xFFFFFF)   // primary text
#define C_TEXT2       lv_color_hex(0x8E8E93)   // secondary text
#define C_BORDER      lv_color_hex(0x38383A)   // subtle border
#define C_GREEN       lv_color_hex(0x30D158)   // on-state glow
#define C_RED         lv_color_hex(0xFF453A)   // off / error

#define RADIUS_CARD   12
#define RADIUS_BTN    10

// Reusable style helpers
static inline void style_card(lv_obj_t* obj) {
    lv_obj_set_style_bg_color(obj, C_BG2,    0);
    lv_obj_set_style_bg_opa(obj,   LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj,   RADIUS_CARD,  0);
    lv_obj_set_style_border_width(obj, 1,      0);
    lv_obj_set_style_border_color(obj, C_BORDER, 0);
    lv_obj_set_style_shadow_width(obj, 0,      0);
}

static inline void style_label_primary(lv_obj_t* label) {
    lv_obj_set_style_text_color(label, C_TEXT,  0);
    lv_obj_set_style_text_font(label,  &lv_font_montserrat_14, 0);
}

static inline void style_label_secondary(lv_obj_t* label) {
    lv_obj_set_style_text_color(label, C_TEXT2, 0);
    lv_obj_set_style_text_font(label,  &lv_font_montserrat_12, 0);
}

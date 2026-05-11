#pragma once

#define TFT_WIDTH   320
#define TFT_HEIGHT  480

#define PIN_LCD_SCLK  17
#define PIN_LCD_MOSI  18
#define PIN_LCD_MISO  -1
#define PIN_LCD_DC    11
#define PIN_LCD_CS    12
#define PIN_LCD_RST   10
#define PIN_LCD_BL    46

#define PIN_TOUCH_SDA  1
#define PIN_TOUCH_SCL  3
#define PIN_TOUCH_INT  21
#define PIN_TOUCH_RST  -1
#define TOUCH_I2C_ADDR 0x15

#define PIN_RESET_BTN  0
#define SETUP_AP_NAME  "HomeController-Setup"

// HomeKit pairing code (8 digits, displayed as XXX-XX-XXX)
#define HK_PAIRING_CODE "46645544"    // → 466-45-544
#define HK_SETUP_ID     "HC01"
#define HK_DISPLAY_CODE "466-45-544"  // formatted for display

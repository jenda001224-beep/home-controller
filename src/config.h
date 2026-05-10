#pragma once

// ============================================================
//  Display — T-Display S3 Pro
//  WiFi + HA credentials are entered via the setup portal
//  (not hardcoded here). Verify pins against your board:
//  https://github.com/Xinyuan-LilyGO/T-Display-S3-Pro
// ============================================================
#define TFT_WIDTH   320
#define TFT_HEIGHT  480

#define PIN_LCD_SCLK  17
#define PIN_LCD_MOSI  18
#define PIN_LCD_MISO  -1
#define PIN_LCD_DC    11
#define PIN_LCD_CS    12
#define PIN_LCD_RST   10
#define PIN_LCD_BL    46

// Touch (I2C CST816S)
#define PIN_TOUCH_SDA  1
#define PIN_TOUCH_SCL  3
#define PIN_TOUCH_INT  21
#define PIN_TOUCH_RST  -1
#define TOUCH_I2C_ADDR 0x15

// Hold this GPIO LOW on boot for 3 s to wipe config and re-enter setup
#define PIN_RESET_BTN  0   // boot button on most LilyGO boards

// AP name shown when device needs setup
#define SETUP_AP_NAME "HomeController-Setup"
e PIN_LCD_SCLK  17
#define PIN_LCD_MOSI  18
#define PIN_LCD_MISO  -1
#define PIN_LCD_DC    11
#define PIN_LCD_CS    12
#define PIN_LCD_RST   10
#define PIN_LCD_BL    46

// Touch (I2C CST816S)
#define PIN_TOUCH_SDA  1
#define PIN_TOUCH_SCL  3
#define PIN_TOUCH_INT  21
#define PIN_TOUCH_RST  -1
#define TOUCH_I2C_ADDR 0x15

// ============================================================
//  UI Behaviour
// ============================================================
// How many entity types to show (others are hidden)
#define SHOW_LIGHTS   true
#define SHOW_SWITCHES true
#define SHOW_FANS     true

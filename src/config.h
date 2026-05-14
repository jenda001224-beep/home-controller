#pragma once

#define TFT_WIDTH   320
#define TFT_HEIGHT  480

// -- Display SPI (from LilyGO utilities.h) --
#define PIN_LCD_SCLK  18
#define PIN_LCD_MOSI  17
#define PIN_LCD_MISO   8
#define PIN_LCD_DC     9
#define PIN_LCD_CS    39
#define PIN_LCD_RST   47
#define PIN_LCD_BL    48

// -- Touch I2C (CST226SE via SensorsLib) --
#define PIN_TOUCH_SDA   5
#define PIN_TOUCH_SCL   6
#define PIN_TOUCH_INT  21   // interrupt / wakeup source
#define PIN_TOUCH_RST  13
#define CST226SE_ADDR  0x5A // correct address (NOT 0x15 which was CST816S)

// -- Physical buttons --
#define PIN_BOOT_BTN    0   // BOOT button (factory reset + deep sleep wake)
#define PIN_BTN1       12   // right top button (deep sleep wake)
#define PIN_BTN2       16   // right bottom button (deep sleep wake)
#define PIN_RESET_BTN   PIN_BOOT_BTN

// -- LED --
#define PIN_LED        38   // white LED

// -- Battery ADC --
#define PIN_BAT_ADC     4   // 1:2 voltage divider

// -- App constants --
#define APP_NAME       "SwitchPro"
#define APP_VERSION    "v5.29"
#define SETUP_AP_NAME  "SwitchPro-Setup"

// -- Deep sleep default (seconds, 0 = never) --
#define SLEEP_SEC_DEFAULT  10

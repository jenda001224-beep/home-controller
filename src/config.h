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

// -- Touch I2C (CST226SE) --
#define PIN_TOUCH_SDA   5
#define PIN_TOUCH_SCL   6
#define PIN_TOUCH_INT  21
#define PIN_TOUCH_RST  13
#define TOUCH_I2C_ADDR 0x15

// -- Buttons & LED --
#define PIN_RESET_BTN   0   // BOOT button
#define PIN_LED        38   // white LED

// -- Battery (voltage divider -> ADC, T-Display S3 Pro) --
#define PIN_BAT_ADC     4   // BAT_ADC through 1:2 divider

// -- App constants --
#define APP_NAME       "SwitchPro"
#define SETUP_AP_NAME  "SwitchPro-Setup"

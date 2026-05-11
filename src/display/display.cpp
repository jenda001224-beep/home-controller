#include "display.h"
#include "config.h"
#include <LovyanGFX.hpp>
#include <lvgl.h>
#include <Wire.h>
#include <touch/TouchDrvCSTXXX.hpp>

// ── LovyanGFX — display only (no touch driver, handled by SensorLib) ────────

class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7796 _panel;
    lgfx::Bus_SPI      _bus;
    lgfx::Light_PWM    _light;

public:
    LGFX() {
        {
            auto cfg = _bus.config();
            cfg.spi_host    = SPI2_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = 80000000;
            cfg.freq_read   = 20000000;
            cfg.spi_3wire   = false;
            cfg.use_lock    = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk    = PIN_LCD_SCLK;
            cfg.pin_mosi    = PIN_LCD_MOSI;
            cfg.pin_miso    = PIN_LCD_MISO;
            cfg.pin_dc      = PIN_LCD_DC;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs           = PIN_LCD_CS;
            cfg.pin_rst          = PIN_LCD_RST;
            cfg.pin_busy         = -1;
            cfg.panel_width      = TFT_WIDTH;
            cfg.panel_height     = TFT_HEIGHT;
            cfg.offset_rotation  = 0;
            cfg.readable         = false;
            cfg.invert           = false;
            cfg.rgb_order        = false;
            cfg.dlen_16bit       = false;
            cfg.bus_shared       = true;
            _panel.config(cfg);
        }
        {
            auto cfg = _light.config();
            cfg.pin_bl      = PIN_LCD_BL;
            cfg.invert      = false;
            cfg.freq        = 44100;
            cfg.pwm_channel = 7;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        setPanel(&_panel);
    }
};

static LGFX lcd;

// ── SensorLib touch (CST226SE) ───────────────────────────────────────────────

static TouchDrvCSTXXX touch;
volatile bool g_home_pressed = false;

static void home_button_cb(void*) {
    g_home_pressed = true;
}

// ── LVGL glue ────────────────────────────────────────────────────────────────

static lv_disp_draw_buf_t draw_buf;
static lv_color_t* buf1 = nullptr;
static lv_color_t* buf2 = nullptr;

static void lvgl_flush(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* px_map) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    lcd.startWrite();
    lcd.setAddrWindow(area->x1, area->y1, w, h);
    lcd.writePixels((lgfx::rgb565_t*)px_map, w * h);
    lcd.endWrite();
    lv_disp_flush_ready(drv);
}

static void lvgl_touch_read(lv_indev_drv_t*, lv_indev_data_t* data) {
    const TouchPoints& pts = touch.getTouchPoints();
    if (pts.hasPoints()) {
        const TouchPoint& p = pts.getPoint(0);
        data->state   = LV_INDEV_STATE_PR;
        data->point.x = (lv_coord_t)p.x;
        data->point.y = (lv_coord_t)p.y;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

// ── Public init ──────────────────────────────────────────────────────────────

void display_init() {
    // Force backlight on before PWM init
    pinMode(PIN_LCD_BL, OUTPUT);
    digitalWrite(PIN_LCD_BL, HIGH);

    lcd.init();
    lcd.setRotation(0);
    lcd.setBrightness(200);

    // Touch — CST226SE via SensorLib
    Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL);
    if (!touch.begin(Wire, PIN_TOUCH_RST, PIN_TOUCH_SDA, PIN_TOUCH_SCL)) {
        Serial.println("Touch init failed");
    }
    touch.setHomeButtonCallback(home_button_cb, nullptr);

    // LVGL
    lv_init();

    size_t buf_px = TFT_WIDTH * 20;
    buf1 = (lv_color_t*)ps_malloc(buf_px * sizeof(lv_color_t));
    buf2 = (lv_color_t*)ps_malloc(buf_px * sizeof(lv_color_t));
    if (!buf1 || !buf2) {
        static lv_color_t fb1[TFT_WIDTH * 10];
        static lv_color_t fb2[TFT_WIDTH * 10];
        buf1 = fb1; buf2 = fb2;
        buf_px = TFT_WIDTH * 10;
    }
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_px);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = TFT_WIDTH;
    disp_drv.ver_res  = TFT_HEIGHT;
    disp_drv.flush_cb = lvgl_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_read;
    lv_indev_drv_register(&indev_drv);
}

void display_set_brightness(uint8_t level) {
    lcd.setBrightness(level);
}

#include "display.h"
#include "config.h"

// ============================================================
//  LovyanGFX — T-Display S3 Pro
//  Adjust pins in config.h if your board revision differs.
//  Reference: https://github.com/Xinyuan-LilyGO/T-Display-S3-Pro
// ============================================================
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7796   _panel;
    lgfx::Bus_SPI        _bus;
    lgfx::Light_PWM      _light;
    lgfx::Touch_CST816S  _touch;

public:
    LGFX() {
        // SPI bus
        {
            auto cfg = _bus.config();
            cfg.spi_host   = SPI2_HOST;
            cfg.spi_mode   = 0;
            cfg.freq_write = 80000000;
            cfg.freq_read  = 20000000;
            cfg.spi_3wire  = true;
            cfg.use_lock   = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk   = PIN_LCD_SCLK;
            cfg.pin_mosi   = PIN_LCD_MOSI;
            cfg.pin_miso   = PIN_LCD_MISO;
            cfg.pin_dc     = PIN_LCD_DC;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        // Panel
        {
            auto cfg = _panel.config();
            cfg.pin_cs          = PIN_LCD_CS;
            cfg.pin_rst         = PIN_LCD_RST;
            cfg.pin_busy        = -1;
            cfg.panel_width     = TFT_WIDTH;
            cfg.panel_height    = TFT_HEIGHT;
            cfg.offset_x        = 0;
            cfg.offset_y        = 0;
            cfg.offset_rotation = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable         = true;
            cfg.invert           = false;
            cfg.rgb_order        = false;
            cfg.dlen_16bit       = false;
            cfg.bus_shared       = true;
            _panel.config(cfg);
        }
        // Backlight
        {
            auto cfg = _light.config();
            cfg.pin_bl     = PIN_LCD_BL;
            cfg.invert     = false;
            cfg.freq       = 44100;
            cfg.pwm_channel = 7;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        // Touch (I2C CST816S)
        {
            auto cfg = _touch.config();
            cfg.x_min           = 0;
            cfg.x_max           = TFT_WIDTH  - 1;
            cfg.y_min           = 0;
            cfg.y_max           = TFT_HEIGHT - 1;
            cfg.pin_int         = PIN_TOUCH_INT;
            cfg.bus_shared      = false;
            cfg.offset_rotation = 0;
            cfg.i2c_port = 0;
            cfg.i2c_addr = TOUCH_I2C_ADDR;
            cfg.pin_sda  = PIN_TOUCH_SDA;
            cfg.pin_scl  = PIN_TOUCH_SCL;
            cfg.freq     = 400000;
            _touch.config(cfg);
            _panel.setTouch(&_touch);
        }
        setPanel(&_panel);
    }
};

static LGFX lcd;

// LVGL draw buffers — allocate in PSRAM
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf1 = nullptr;
static lv_color_t *buf2 = nullptr;

static void lvgl_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px_map) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    lcd.startWrite();
    lcd.setAddrWindow(area->x1, area->y1, w, h);
    lcd.writePixels((lgfx::rgb565_t*)px_map, w * h);
    lcd.endWrite();
    lv_disp_flush_ready(drv);
}

static void lvgl_touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    uint16_t x = 0, y = 0;
    bool pressed = lcd.getTouch(&x, &y);
    data->state   = pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
    data->point.x = x;
    data->point.y = y;
}

void display_init() {
    // GPIO 15 = power enable for the T-Display S3 Pro display module.
    // Without this the screen has no power and stays completely dead.
    pinMode(15, OUTPUT);
    digitalWrite(15, HIGH);
    delay(50);

    // Force backlight ON before LovyanGFX configures its PWM.
    pinMode(PIN_LCD_BL, OUTPUT);
    digitalWrite(PIN_LCD_BL, HIGH);

    lcd.init();
    lcd.setRotation(0);
    lcd.setBrightness(200);

    lv_init();

    // Use PSRAM for draw buffers (full row × 20 lines each)
    size_t buf_px = TFT_WIDTH * 20;
    buf1 = (lv_color_t*)ps_malloc(buf_px * sizeof(lv_color_t));
    buf2 = (lv_color_t*)ps_malloc(buf_px * sizeof(lv_color_t));
    if (!buf1 || !buf2) {
        // Fall back to internal RAM with smaller buffer
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
    indev_drv.type     = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb  = lvgl_touch_read;
    lv_indev_drv_register(&indev_drv);
}

void display_set_brightness(uint8_t level) {
    lcd.setBrightness(level);
}

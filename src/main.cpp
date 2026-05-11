#include <Arduino.h>
#include <Preferences.h>
#include <WiFiManager.h>
#include <lvgl.h>

#include "config.h"
#include "display/display.h"
#include "ha/ha_client.h"
#include "ui/ui.h"

static HAClient    ha;
static UI          ui;
static Preferences prefs;

static volatile bool g_update_pending = false;
static HAEntity      g_pending_entity;

static String cfg_ha_host;
static String cfg_ha_port;
static String cfg_ha_token;

static void load_config() {
    prefs.begin("hc_cfg", true);
    cfg_ha_host  = prefs.getString("ha_host",  "");
    cfg_ha_port  = prefs.getString("ha_port",  "8123");
    cfg_ha_token = prefs.getString("ha_token", "");
    prefs.end();
}

static bool config_is_valid() {
    return cfg_ha_host.length() > 0 && cfg_ha_token.length() > 10;
}

static void run_setup_portal() {
    ui.set_status("Setup mode\n"
                  "1. Join WiFi:\n" SETUP_AP_NAME "\n"
                  "2. Open Safari:\n"
                  "http://192.168.4.1");
    // Give LVGL time to fully render before WiFi takes over
    for (int i = 0; i < 20; i++) { lv_timer_handler(); delay(10); }

    WiFiManager wm;
    wm.setTitle("Home Controller Setup");
    wm.setDarkMode(true);
    wm.setConfigPortalBlocking(false);

    WiFiManagerParameter p_host ("ha_host",  "Home Assistant IP or hostname",
                                  cfg_ha_host.c_str(), 64);
    WiFiManagerParameter p_port ("ha_port",  "HA Port (default 8123)",
                                  cfg_ha_port.c_str(), 6);
    WiFiManagerParameter p_token("ha_token", "HA Long-Lived Access Token", "", 300);

    wm.addParameter(&p_host);
    wm.addParameter(&p_port);
    wm.addParameter(&p_token);

    bool saved = false;
    wm.setSaveParamsCallback([&]() {
        prefs.begin("hc_cfg", false);
        prefs.putString("ha_host",  p_host.getValue());
        prefs.putString("ha_port",  p_port.getValue());
        prefs.putString("ha_token", p_token.getValue());
        prefs.end();
        saved = true;
    });

    wm.startConfigPortal(SETUP_AP_NAME);

    while (!saved) {
        wm.process();
        lv_timer_handler();
        delay(5);
    }

    ui.set_status("Saved!\nRebooting...");
    for (int i = 0; i < 30; i++) { lv_timer_handler(); delay(10); }
    ESP.restart();
}

void setup() {
    Serial.begin(115200);
    pinMode(PIN_RESET_BTN, INPUT_PULLUP);

    display_init();
    ui.begin(&ha);
    ui.set_status("Starting...");
    lv_timer_handler();

    load_config();

    // Hold boot button 3 s on power-up to wipe config
    if (digitalRead(PIN_RESET_BTN) == LOW) {
        uint32_t held = millis();
        ui.set_status("Hold 3 s to reset...");
        lv_timer_handler();
        while (digitalRead(PIN_RESET_BTN) == LOW) {
            lv_timer_handler();
            if (millis() - held > 3000) {
                prefs.begin("hc_cfg", false);
                prefs.clear();
                prefs.end();
                WiFiManager wm;
                wm.resetSettings();
                ui.set_status("Config cleared.\nRebooting...");
                lv_timer_handler();
                delay(1500);
                ESP.restart();
            }
        }
    }

    if (!config_is_valid()) {
        run_setup_portal();
        return;
    }

    ui.set_status("Connecting to WiFi...");
    lv_timer_handler();

    WiFi.mode(WIFI_STA);
    WiFi.begin();   // uses credentials saved by WiFiManager
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 40) {
        delay(500);
        lv_timer_handler();
        tries++;
    }
    if (WiFi.status() != WL_CONNECTED) {
        run_setup_portal();
        return;
    }

    ui.set_status("Connecting to\nHome Assistant...");
    lv_timer_handler();

    ha.on_ready([&]() { ui.build_home(); });
    ha.on_update([&](const HAEntity& e) {
        g_pending_entity = e;
        g_update_pending = true;
    });

    uint16_t port = (uint16_t)cfg_ha_port.toInt();
    ha.begin(cfg_ha_host.c_str(), port ? port : 8123, cfg_ha_token.c_str());
}

void loop() {
    ha.loop();
    lv_timer_handler();
    if (g_update_pending) {
        g_update_pending = false;
        ui.on_entity_update(g_pending_entity);
    }
    delay(5);
}

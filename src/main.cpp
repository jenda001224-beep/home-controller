#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <lvgl.h>
#include "config.h"
#include "display/display.h"
#include "dirigera/dirigera_client.h"
#include "ui/ui.h"

// -- Globals --

static DirigeraClient dc;
static UI             ui;

static volatile bool g_update_pending = false;
static HAEntity      g_pending_entity;
static volatile bool g_demo_pending   = false;

static Preferences prefs;

// -- Settings (persisted in NVS "hc_cfg") --

static uint8_t  cfg_brightness = 200;
static uint16_t cfg_sleep_sec  = SLEEP_SEC_DEFAULT;
static uint8_t  cfg_grid_cols  = 2;

static void load_settings() {
    prefs.begin("hc_cfg", true);
    cfg_brightness = prefs.getUChar("bri",    200);
    cfg_sleep_sec  = prefs.getUShort("sleep", SLEEP_SEC_DEFAULT);
    cfg_grid_cols  = prefs.getUChar("grid",   2);
    prefs.end();
}
static void save_settings() {
    prefs.begin("hc_cfg", false);
    prefs.putUChar("bri",    cfg_brightness);
    prefs.putUShort("sleep", cfg_sleep_sec);
    prefs.putUChar("grid",   cfg_grid_cols);
    prefs.end();
}

// -- Helpers --

static void flush_ui(int ms = 100) {
    uint32_t t = millis();
    while (millis() - t < (uint32_t)ms) { lv_timer_handler(); delay(10); }
}
static void blink_led(int n, int on_ms = 150, int off_ms = 150) {
    pinMode(PIN_LED, OUTPUT);
    for (int i = 0; i < n; i++) {
        digitalWrite(PIN_LED, HIGH); delay(on_ms);
        digitalWrite(PIN_LED, LOW);  delay(off_ms);
    }
}

// -- Battery + charging --

static int read_battery_pct() {
    analogSetPinAttenuation(PIN_BAT_ADC, ADC_11db);
    int   raw = analogRead(PIN_BAT_ADC);
    float v   = (raw / 4095.0f) * 3.3f * 2.0f;
    return constrain((int)((v - 3.0f) / 1.2f * 100.0f), 0, 100);
}
// VBUS detection: T-Display S3 Pro has no dedicated VBUS sense GPIO.
// As a proxy we check if the ADC reads above 4.1 V (typical charged/charging voltage).
static bool is_charging() {
    analogSetPinAttenuation(PIN_BAT_ADC, ADC_11db);
    int   raw = analogRead(PIN_BAT_ADC);
    float v   = (raw / 4095.0f) * 3.3f * 2.0f;
    return v > 4.10f;
}

// -- Deep sleep --

static void go_to_sleep() {
    Serial.println("Deep sleep...");
    display_set_brightness(0);
    flush_ui(50);
    uint64_t mask = (1ULL << PIN_BOOT_BTN) | (1ULL << PIN_BTN1) |
                    (1ULL << PIN_BTN2)      | (1ULL << PIN_TOUCH_INT);
    esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();
}

// -- DIRIGERA config storage --

static String load_dirigera_ip()    { prefs.begin("hc_dr",true); String v=prefs.getString("ip","");    prefs.end(); return v; }
static String load_dirigera_token() { prefs.begin("hc_dr",true); String v=prefs.getString("token",""); prefs.end(); return v; }
static void save_dirigera(const String& ip, const String& token) {
    prefs.begin("hc_dr",false); prefs.putString("ip",ip); prefs.putString("token",token); prefs.end();
}
static void clear_dirigera() { prefs.begin("hc_dr",false); prefs.clear(); prefs.end(); }

// ===================================================================
//  Web server
// ===================================================================

static WebServer app_srv(80);
enum class PairState { IDLE, WAITING, DONE, FAILED };
static PairState pair_state = PairState::IDLE;
static String    pair_ip;

// Common CSS shared between both pages
static const char CSS[] PROGMEM =
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{background:#1c1c1e;color:#fff;font-family:-apple-system,sans-serif;padding:24px}"
    "h1{font-size:22px;margin-bottom:6px}"
    ".sub{color:#8e8e93;font-size:14px;margin-bottom:24px}"
    ".card{background:#2c2c2e;border-radius:16px;padding:20px;margin-bottom:16px;border:1px solid #38383a}"
    ".card h2{font-size:11px;text-transform:uppercase;letter-spacing:.08em;color:#636366;margin-bottom:12px}"
    "input[type=text]{display:block;width:100%;background:#3a3a3c;border:1px solid #48484a;"
    "  color:#fff;border-radius:10px;padding:12px 14px;font-size:16px;margin-bottom:12px}"
    "input[type=range]{width:100%;margin:8px 0 4px;accent-color:#ff9500}"
    "select{width:100%;background:#3a3a3c;border:1px solid #48484a;color:#fff;"
    "  border-radius:10px;padding:10px 14px;font-size:15px;margin-top:6px}"
    ".row{display:flex;gap:10px}"
    ".btn{flex:1;background:#ff9500;border:none;color:#fff;border-radius:12px;"
    "  padding:14px;font-size:16px;font-weight:600;cursor:pointer}"
    ".btn:active{opacity:.7}"
    ".btn.gray{background:#3a3a3c;color:#ebebf5}"
    ".status{margin-top:10px;font-size:13px;color:#8e8e93;text-align:center;min-height:18px}"
    ".ok{color:#30d158}.err{color:#ff453a}"
    "label{font-size:13px;color:#8e8e93;display:block;margin-top:12px}"
    ".info{font-size:13px;color:#8e8e93;line-height:1.9}"
    ".info b{color:#fff}"
    "</style>";

// Settings card — built at runtime so current values appear pre-filled
static String settings_card_html() {
    // Use explicit String concatenation to avoid raw-string )" termination issues
    String h = "<div class='card'><h2>Settings</h2>";
    h += "<label>Brightness <span id='bv'>" + String(cfg_brightness) + "</span></label>";
    h += "<input type='range' min='10' max='255' value='" + String(cfg_brightness) + "' id='bri'>";
    h += "<label>Sleep after</label>";
    h += "<select id='slp'>";
    h += "<option value='0'"  + String(cfg_sleep_sec==0  ?" selected":"") + ">Never</option>";
    h += "<option value='5'"  + String(cfg_sleep_sec==5  ?" selected":"") + ">5 seconds</option>";
    h += "<option value='10'" + String(cfg_sleep_sec==10 ?" selected":"") + ">10 seconds</option>";
    h += "<option value='30'" + String(cfg_sleep_sec==30 ?" selected":"") + ">30 seconds</option>";
    h += "<option value='60'" + String(cfg_sleep_sec==60 ?" selected":"") + ">1 minute</option>";
    h += "</select>";
    h += "<label>Grid columns</label>";
    h += "<select id='grd'>";
    h += "<option value='2'" + String(cfg_grid_cols==2?" selected":"") + ">2 columns</option>";
    h += "<option value='3'" + String(cfg_grid_cols==3?" selected":"") + ">3 columns</option>";
    h += "</select>";
    h += "<div class='status' id='ss'></div></div>";
    return h;
}

// Common JS for settings
static const char SETTINGS_JS[] PROGMEM =
    "<script>"
    "document.getElementById('bri').oninput=function(){"
    "  document.getElementById('bv').textContent=this.value;};"
    "document.getElementById('bri').onchange=function(){"
    "  save('brightness',this.value);};"
    "document.getElementById('slp').onchange=function(){save('sleep',this.value);};"
    "document.getElementById('grd').onchange=function(){save('grid',this.value);};"
    "function save(k,v){"
    "  var s=document.getElementById('ss');"
    "  s.textContent='Saving...';s.className='status';"
    "  fetch('/settings_set?'+k+'='+v).then(function(r){return r.json();})"
    "  .then(function(d){s.textContent=d.ok?'Saved':'Error';"
    "    s.className='status '+(d.ok?'ok':'err');}).catch(function(){"
    "    s.textContent='Error';s.className='status err';});}"
    "</script>";

static String page_setup() {
    String h = "<!DOCTYPE html><html lang='en'><head>";
    h += "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
    h += "<title>SwitchPro Setup</title>";
    h += String(FPSTR(CSS));
    h += "</head><body>";
    h += "<h1>&#127968; SwitchPro</h1><p class='sub'>Device setup</p>";

    // Pairing card
    h += "<div class='card'><h2>DIRIGERA Hub</h2>";
    h += "<input type='text' id='ip' placeholder='Hub IP (e.g. 192.168.1.50)' inputmode='decimal'>";
    h += "<button class='btn' id='pbtn'>Pair with DIRIGERA</button>";
    h += "<div class='status' id='ps'></div>";
    h += "<p style='font-size:12px;color:#636366;margin-top:8px'>Enter IP, tap Pair, then press the button on the back of DIRIGERA within 30s</p>";
    h += "</div>";

    // Demo card
    h += "<div class='card'><h2>Preview</h2>";
    h += "<p style='font-size:13px;color:#8e8e93;margin-bottom:12px'>Load fake rooms to see how the UI looks.</p>";
    h += "<div class='row'>";
    h += "<button class='btn' id='demobtn'>&#127916; Load Demo</button>";
    h += "<button class='btn gray' id='clrbtn'>Clear</button>";
    h += "</div><div class='status' id='ds'></div></div>";

    h += settings_card_html();

    // JS — no inline handlers, all via addEventListener
    h += "<script>"
         "document.getElementById('pbtn').addEventListener('click',function(){"
         "  var ip=document.getElementById('ip').value.trim();"
         "  if(!ip){set('ps','Enter IP','err');return;}"
         "  this.disabled=true;set('ps','Contacting hub...');"
         "  fetch('/pair_start?ip='+encodeURIComponent(ip)).then(function(r){return r.json();})"
         "  .then(function(d){if(d.ok){set('ps','Press button on DIRIGERA!');pollPair();}"
         "    else{set('ps','Error: '+d.msg,'err');document.getElementById('pbtn').disabled=false;}})"
         "  .catch(function(){set('ps','Error','err');document.getElementById('pbtn').disabled=false;});"
         "});"
         "function pollPair(){"
         "  setTimeout(function(){"
         "    fetch('/pair_status').then(function(r){return r.json();})"
         "    .then(function(d){"
         "      if(d.done)set('ps','Paired! Rebooting...','ok');"
         "      else if(d.failed){set('ps','Timed out. Try again.','err');document.getElementById('pbtn').disabled=false;}"
         "      else{set('ps','Waiting...');pollPair();}"
         "    }).catch(pollPair);"
         "  },1500);"
         "}"
         "document.getElementById('demobtn').addEventListener('click',function(){"
         "  set('ds','Loading...');"
         "  fetch('/demo').then(function(r){return r.json();})"
         "  .then(function(d){set('ds',d.ok?'Done!':'Error',d.ok?'ok':'err');})"
         "  .catch(function(){set('ds','Error','err');});"
         "});"
         "document.getElementById('clrbtn').addEventListener('click',function(){fetch('/clear_demo');set('ds','Cleared');});"
         "function set(id,msg,cls){"
         "  var el=document.getElementById(id);"
         "  el.textContent=msg;el.className='status '+(cls||'');}"
         "</script>";
    h += String(FPSTR(SETTINGS_JS));
    h += "</body></html>";
    return h;
}

static String page_live() {
    String h = "<!DOCTYPE html><html lang='en'><head>";
    h += "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
    h += "<title>SwitchPro</title>";
    h += String(FPSTR(CSS));
    h += "</head><body>";
    h += "<h1>&#127968; SwitchPro</h1><p class='sub'>Device control</p>";

    // Demo card
    h += "<div class='card'><h2>Demo</h2>";
    h += "<p style='font-size:13px;color:#8e8e93;margin-bottom:12px'>Load fake rooms to preview the UI.</p>";
    h += "<div class='row'>";
    h += "<button class='btn' id='demobtn'>&#127916; Load Demo</button>";
    h += "<button class='btn gray' id='refbtn'>&#8635; Refresh</button>";
    h += "</div><div class='status' id='status'></div></div>";

    // Status card
    h += "<div class='card'><h2>Status</h2><div class='info' id='info'>Loading...</div></div>";

    h += settings_card_html();

    h += "<script>"
         "document.getElementById('demobtn').addEventListener('click',function(){"
         "  setS('Loading...');"
         "  fetch('/demo').then(function(r){return r.json();})"
         "  .then(function(d){setS(d.ok?'Demo loaded!':'Error',d.ok?'ok':'err');})"
         "  .catch(function(){setS('Error','err');});"
         "});"
         "document.getElementById('refbtn').addEventListener('click',function(){"
         "  setS('Refreshing...');"
         "  fetch('/refresh').then(function(r){return r.json();})"
         "  .then(function(d){setS(d.ok?'Done!':'Error',d.ok?'ok':'err');loadInfo();})"
         "  .catch(function(){setS('Error','err');});"
         "});"
         "function setS(msg,cls){"
         "  var el=document.getElementById('status');"
         "  el.textContent=msg;el.className='status '+(cls||'');}"
         "function loadInfo(){"
         "  fetch('/status').then(function(r){return r.json();})"
         "  .then(function(d){"
         "    document.getElementById('info').innerHTML="
         "      '<b>IP:</b> '+d.ip+'<br><b>WiFi:</b> '+d.ssid+"
         "      '<br><b>DIRIGERA:</b> '+d.dirigera+"
         "      '<br><b>Devices:</b> '+d.devices+"
         "      '<br><b>Uptime:</b> '+d.uptime+'s';"
         "  }).catch(function(){document.getElementById('info').textContent='Failed';});"
         "}"
         "loadInfo();"
         "</script>";
    h += String(FPSTR(SETTINGS_JS));
    h += "</body></html>";
    return h;
}

// ---- Route handlers ----

static void handle_root() {
    String dip = load_dirigera_ip();
    app_srv.send(200, "text/html;charset=utf-8", dip.isEmpty() ? page_setup() : page_live());
}
static void handle_pair_start() {
    if (!app_srv.hasArg("ip")) { app_srv.send(400,"application/json","{\"ok\":false,\"msg\":\"No IP\"}"); return; }
    pair_ip = app_srv.arg("ip"); pair_state = PairState::WAITING;
    app_srv.send(200,"application/json","{\"ok\":true}");
}
static void handle_pair_status() {
    if      (pair_state == PairState::DONE)   app_srv.send(200,"application/json","{\"done\":true}");
    else if (pair_state == PairState::FAILED) app_srv.send(200,"application/json","{\"failed\":true}");
    else                                       app_srv.send(200,"application/json","{\"done\":false,\"failed\":false}");
}
static void handle_demo()       { g_demo_pending=true; app_srv.send(200,"application/json","{\"ok\":true}"); }
static void handle_clear_demo() { app_srv.send(200,"application/json","{\"ok\":true}"); delay(200); ESP.restart(); }
static void handle_refresh()    { app_srv.send(200,"application/json","{\"ok\":true}"); }
static void handle_status() {
    String b = "{\"ip\":\""+WiFi.localIP().toString()+"\","
               "\"ssid\":\""+WiFi.SSID()+"\","
               "\"dirigera\":\""+load_dirigera_ip()+"\","
               "\"devices\":"+String(dc.entities().size())+","
               "\"uptime\":"+String(millis()/1000)+"}";
    app_srv.send(200,"application/json",b);
}
static void handle_settings_set() {
    bool changed = false;
    if (app_srv.hasArg("brightness")) {
        cfg_brightness = (uint8_t)constrain(app_srv.arg("brightness").toInt(), 10, 255);
        display_set_brightness(cfg_brightness); changed = true;
    }
    if (app_srv.hasArg("sleep")) {
        cfg_sleep_sec = (uint16_t)app_srv.arg("sleep").toInt(); changed = true;
    }
    if (app_srv.hasArg("grid")) {
        cfg_grid_cols = (uint8_t)constrain(app_srv.arg("grid").toInt(), 2, 3);
        ui.set_grid_cols(cfg_grid_cols); changed = true;
    }
    if (changed) save_settings();
    app_srv.send(200,"application/json","{\"ok\":true}");
}
static void handle_settings_get() {
    app_srv.send(200,"application/json",
        "{\"brightness\":"+String(cfg_brightness)+
        ",\"sleep\":"+String(cfg_sleep_sec)+
        ",\"grid\":"+String(cfg_grid_cols)+"}");
}

static void start_app_server() {
    app_srv.on("/",             HTTP_GET, handle_root);
    app_srv.on("/pair_start",   HTTP_GET, handle_pair_start);
    app_srv.on("/pair_status",  HTTP_GET, handle_pair_status);
    app_srv.on("/demo",         HTTP_GET, handle_demo);
    app_srv.on("/clear_demo",   HTTP_GET, handle_clear_demo);
    app_srv.on("/refresh",      HTTP_GET, handle_refresh);
    app_srv.on("/status",       HTTP_GET, handle_status);
    app_srv.on("/settings_set", HTTP_GET, handle_settings_set);
    app_srv.on("/settings",     HTTP_GET, handle_settings_get);
    app_srv.begin();
    Serial.println("App server on :80");
}

// -- setup / loop --

void setup() {
    Serial.begin(115200);
    pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
    pinMode(PIN_BTN1,     INPUT_PULLUP);
    pinMode(PIN_BTN2,     INPUT_PULLUP);

    load_settings();
    display_init();
    display_set_brightness(cfg_brightness);

    ui.begin(&dc);
    ui.set_grid_cols(cfg_grid_cols);
    ui.set_status("Starting...");
    flush_ui(200);
    blink_led(3);

    // Factory reset: hold BOOT 3 s
    if (digitalRead(PIN_BOOT_BTN) == LOW) {
        uint32_t held = millis();
        ui.set_status("Hold 3s to\nfactory reset...");
        flush_ui(50);
        while (digitalRead(PIN_BOOT_BTN) == LOW) {
            lv_timer_handler();
            if (millis() - held > 3000) {
                clear_dirigera();
                WiFiManager wm; wm.resetSettings();
                ui.set_status("Reset done.\nRebooting...");
                flush_ui(1500); ESP.restart();
            }
        }
    }

    // WiFi
    ui.set_status("Connecting to WiFi...");
    flush_ui(50);
    WiFiManager wm;
    wm.setTitle(APP_NAME);
    wm.setDarkMode(true);
    wm.setCustomHeadElement(
        "<style>"
        "body,div,form,section{background:#1c1c1e!important;color:#fff!important}"
        ".wrap,.main{background:#2c2c2e!important;border:1px solid #38383a!important;box-shadow:none!important}"
        "h1,h2,h3,p,label,li{color:#fff!important}"
        "input{background:#3a3a3c!important;border:1px solid #48484a!important;color:#fff!important;border-radius:10px!important}"
        "button,input[type=submit]{background:#ff9500!important;color:#fff!important;border:none!important;border-radius:12px!important}"
        "a,a:visited{color:#ff9500!important}"
        "</style>"
    );
    wm.setConfigPortalBlocking(false);
    if (!wm.autoConnect(SETUP_AP_NAME)) {
        ui.set_status("WiFi setup:\nJoin " SETUP_AP_NAME "\nOpen Safari:\nhttp://192.168.4.1");
        flush_ui(50);
        while (WiFi.status() != WL_CONNECTED) { wm.process(); lv_timer_handler(); delay(5); }
    }
    Serial.printf("WiFi: %s\n", WiFi.localIP().toString().c_str());

    start_app_server();

    String dip  = load_dirigera_ip();
    String dtok = load_dirigera_token();

    if (dip.isEmpty() || dtok.isEmpty()) {
        ui.set_status(("Pair DIRIGERA:\nhttp://" + WiFi.localIP().toString()).c_str());
        flush_ui(50);
        return;
    }

    ui.set_status(("DIRIGERA\n" + dip + "\nLoading devices...").c_str());
    flush_ui(100);

    dc.on_ready([&]() { ui.build_home(); });
    dc.on_update([&](const HAEntity& e) { g_pending_entity = e; g_update_pending = true; });
    dc.begin(dip, dtok);
}

static uint32_t g_bat_last    = 0;
static bool     g_was_charging = false;

void loop() {
    app_srv.handleClient();
    dc.loop();
    lv_timer_handler();

    if (g_home_pressed) {
        g_home_pressed  = false;
        g_last_activity = millis();
        ui.go_home();
    }
    if (digitalRead(PIN_BTN1) == LOW || digitalRead(PIN_BTN2) == LOW)
        g_last_activity = millis();

    if (g_update_pending) { g_update_pending=false; ui.on_entity_update(g_pending_entity); }

    if (g_demo_pending) {
        g_demo_pending = false;
        dc.on_ready([&]() { ui.build_home(); });
        dc.load_demo();
    }

    // DIRIGERA pairing state machine
    if (pair_state == PairState::WAITING) {
        pair_state = PairState::IDLE;
        String ip_str = WiFi.localIP().toString();
        ui.set_status(("Pair:\nhttp://"+ip_str+"\n\nPress button on hub!").c_str());
        flush_ui(50);
        String token;
        bool ok = DirigeraClient::pair(pair_ip, token);
        if (ok) {
            save_dirigera(pair_ip, token);
            pair_state = PairState::DONE;
            ui.set_status("Paired!\nRebooting...");
            flush_ui(2000); ESP.restart();
        } else {
            pair_state = PairState::FAILED;
            ui.set_status(("Pair:\nhttp://"+ip_str+"\n\nFailed. Try again.").c_str());
            flush_ui(50);
        }
    }

    // Battery + charging indicator (every 15 s)
    if (millis() - g_bat_last > 15000) {
        g_bat_last = millis();
        int  pct      = read_battery_pct();
        bool charging = is_charging();
        ui.set_battery(pct, charging);
        if (charging && !g_was_charging)
            blink_led(3, 80, 80);   // quick flash when USB plugged in
        g_was_charging = charging;
    }

    // Deep sleep after inactivity
    if (cfg_sleep_sec > 0 &&
        millis() - g_last_activity > (uint32_t)cfg_sleep_sec * 1000) {
        go_to_sleep();
    }

    delay(5);
}

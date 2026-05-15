#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <lvgl.h>
#include <WiFiClientSecure.h>
#include <HTTPUpdate.h>
#include <Wire.h>
#include <XPowersLib.h>
#include "config.h"
#include "display/display.h"
#include "dirigera/dirigera_client.h"
#include "ui/ui.h"

// OTA firmware URL (GitHub Pages)
static const char* OTA_URL =
    "https://jenda001224-beep.github.io/home-controller/firmware.bin";

// -- RTC memory: survives deep sleep, cleared on power-cycle / hard reset --
// Used to skip the slow WiFiManager path after a normal wake-up.
RTC_DATA_ATTR static bool    rtc_wifi_valid = false;
RTC_DATA_ATTR static uint8_t rtc_channel    = 0;
RTC_DATA_ATTR static uint8_t rtc_bssid[6]  = {0};

// -- Globals --

static DirigeraClient dc;
static UI             ui;

static volatile bool g_update_pending = false;
static HAEntity      g_pending_entity;
static volatile bool g_demo_pending   = false;
static volatile bool g_ota_pending    = false;

// Sleep is only allowed after the home screen has been successfully built.
// This prevents the startup/pairing screen from triggering deep sleep.
static bool g_sleep_enabled = false;

static Preferences prefs;

// -- Settings (persisted in NVS "hc_cfg") --

static uint8_t  cfg_brightness = 200;
static uint16_t cfg_sleep_sec  = SLEEP_SEC_DEFAULT;
static uint8_t  cfg_grid_cols  = 1;

static void load_settings() {
    prefs.begin("hc_cfg", true);
    cfg_brightness = prefs.getUChar("bri",    200);
    cfg_sleep_sec  = prefs.getUShort("sleep", SLEEP_SEC_DEFAULT);
    cfg_grid_cols  = 1;   // always list — ignore any old NVS value
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

// -- PMU (SY6970 power management chip) --
// The T-Display S3 Pro has an SY6970 on the same I2C bus as the touch controller.
// Wire is already initialized by display_init() before pmu_init() is called.

static PowersSY6970 pmu;
static bool         g_pmu_ok = false;

static void pmu_init() {
    // SY6970 slave address is 0x6A (SY6970_SLAVE_ADDRESS from XPowersLib)
    g_pmu_ok = pmu.init(Wire, PIN_TOUCH_SDA, PIN_TOUCH_SCL, SY6970_SLAVE_ADDRESS);
    if (!g_pmu_ok) {
        Serial.println("[PMU] SY6970 not found");
        return;
    }
    Serial.println("[PMU] SY6970 OK");

    pmu.setInputCurrentLimit(500);      // 500 mA USB input
    pmu.setChargeTargetVoltage(4352);   // 4.352 V target (board spec)
    pmu.setPrechargeCurr(64);           // 64 mA pre-charge
    pmu.setChargerConstantCurr(192);    // 192 mA CC (< half of 470 mAh capacity)
    pmu.enableADCMeasure();             // required before reading voltages
    pmu.enableCharge();                 // battery is connected
    pmu.enableStatLed();                // charge indicator LED
}

// Returns pct (0-100), charging (USB present), v (volts or -1 if unknown).
// NOTE: per SY6970 datasheet, VBAT ADC is unreliable when VBUS is present.
static void read_battery(int& pct, bool& charging, float& v) {
    if (!g_pmu_ok) { pct = 0; charging = false; v = -1.0f; return; }

    charging    = pmu.isVbusIn();
    int batt_mv = pmu.getBattVoltage();

    // Use battery voltage regardless of VBUS — may be slightly inaccurate while
    // charging but gives user a useful indication. Linear 3400–4200 mV = 0–100 %.
    v   = batt_mv / 1000.0f;
    pct = constrain((int)((batt_mv - 3400) * 100 / 800), 0, 100);

    Serial.printf("[PMU] vbus=%s vbat=%d mV pct=%d\n",
                  charging ? "yes" : "no", batt_mv, pct);
}

// -- Deep sleep --

static void go_to_sleep() {
    Serial.println("Deep sleep...");

    // Cache connected AP info so setup() can reconnect in ~2 s instead of ~8 s.
    if (WiFi.status() == WL_CONNECTED) {
        memcpy(rtc_bssid, WiFi.BSSID(), 6);
        rtc_channel   = (uint8_t)WiFi.channel();
        rtc_wifi_valid = true;
    } else {
        rtc_wifi_valid = false;
    }

    display_set_brightness(0);
    flush_ui(50);

    // Wait up to 500 ms for touch to release — prevents immediate re-wake.
    for (int i = 0; i < 50 && digitalRead(PIN_TOUCH_INT) == LOW; i++) delay(10);

    uint64_t mask = (1ULL << PIN_BOOT_BTN) | (1ULL << PIN_BTN1) |
                    (1ULL << PIN_BTN2)      | (1ULL << PIN_TOUCH_INT);
    esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);

    // Deep sleep: CPU + RAM off, only RTC domain powered (~30 µA chip, ~1 mA PMU).
    // Wake causes a full reset — setup() runs from the beginning.
    esp_deep_sleep_start();
    // does not return
}

// -- OTA update --

static void do_ota_update() {
    g_sleep_enabled = false;   // don't sleep while updating
    g_last_activity = millis();

    ui.set_status("Downloading update\nPlease wait...");
    flush_ui(300);

    WiFiClientSecure client;
    client.setInsecure();                // skip cert check — home device, acceptable
    httpUpdate.rebootOnUpdate(true);     // auto-reboot on success
    httpUpdate.setLedPin(PIN_LED, HIGH);

    httpUpdate.onStart([]() {
        ui.set_status("Flashing firmware\nDo not unplug...");
        lv_timer_handler();
    });
    httpUpdate.onProgress([](int cur, int tot) {
        if (tot > 0) {
            char buf[48];
            snprintf(buf, sizeof(buf), "Updating %d%%\nDo not unplug...", cur * 100 / tot);
            ui.set_status(buf);
            lv_timer_handler();
        }
    });
    httpUpdate.onEnd([]() {
        ui.set_status("Done!\nRebooting...");
        lv_timer_handler();
    });

    t_httpUpdate_return ret = httpUpdate.update(client, OTA_URL);
    // Reach here only on failure (success = reboot)
    String err = httpUpdate.getLastErrorString();
    Serial.printf("[OTA] failed: %s\n", err.c_str());
    ui.set_status(("Update failed:\n" + err).c_str());
    flush_ui(4000);
    g_sleep_enabled = true;
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
    h += "<label>Layout</label>";
    h += "<select id='grd'>";
    h += "<option value='1'" + String(cfg_grid_cols==1?" selected":"") + ">List (1 column)</option>";
    h += "<option value='2'" + String(cfg_grid_cols==2?" selected":"") + ">Grid (2 columns)</option>";
    h += "<option value='3'" + String(cfg_grid_cols==3?" selected":"") + ">Compact (3 columns)</option>";
    h += "</select>";
    h += "<div class='status' id='ss'></div></div>";
    // Firmware update card
    h += "<div class='card'><h2>Firmware</h2>";
    h += "<p style='font-size:13px;color:#8e8e93;margin-bottom:12px'>Current: <b style='color:#fff'>" APP_VERSION "</b></p>";
    h += "<button class='btn' id='otabtn'>&#8635; Check for Updates</button>";
    h += "<div class='status' id='otas'></div></div>";
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
    "document.getElementById('otabtn').addEventListener('click',function(){"
    "  var btn=this,s=document.getElementById('otas');"
    "  btn.disabled=true;s.innerHTML='Checking...';s.className='status';"
    "  fetch('/ota_check').then(function(r){return r.json();})"
    "  .then(function(d){"
    "    if(d.error){s.textContent='Error: '+d.msg;s.className='status err';btn.disabled=false;return;}"
    "    if(d.update_available){"
    "      var h='<b>'+d.version+' available</b><br><small style=\"color:#8e8e93\">Current: '+d.current+'</small><br>';"
    "      if(d.changes&&d.changes.length){h+='<ul style=\"margin:8px 0 12px 16px;font-size:13px\">';d.changes.forEach(function(c){h+='<li>'+c+'</li>';});h+='</ul>';}"
    "      h+='<button class=\"btn\" id=\"dobtn\">Install '+d.version+'</button>';"
    "      s.innerHTML=h;s.className='status';"
    "      document.getElementById('dobtn').addEventListener('click',function(){"
    "        this.disabled=true;s.textContent='Installing... watch device screen';s.className='status';"
    "        fetch('/ota_update').then(function(r){return r.json();})"
    "        .then(function(){s.textContent='Downloading — device reboots when done';s.className='status ok';})"
    "        .catch(function(){s.textContent='Error';s.className='status err';});"
    "      });"
    "    }else{"
    "      s.textContent='\\u2713 Up to date ('+d.current+')';s.className='status ok';btn.disabled=false;"
    "    }"
    "  }).catch(function(){s.textContent='Check failed';s.className='status err';btn.disabled=false;});"
    "});"
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
        cfg_grid_cols = (uint8_t)constrain(app_srv.arg("grid").toInt(), 1, 3);
        ui.set_grid_cols(cfg_grid_cols); changed = true;
    }
    if (changed) save_settings();
    app_srv.send(200,"application/json","{\"ok\":true}");
}
static void handle_ota_check() {
    WiFiClientSecure vc;
    vc.setInsecure();
    HTTPClient http;
    http.begin(vc, "https://jenda001224-beep.github.io/home-controller/version.json");
    http.setTimeout(12000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.addHeader("Cache-Control", "no-cache");
    http.addHeader("User-Agent", "ESP32-SwitchPro/1.0");
    int code = http.GET();
    if (code == 200) {
        String body = http.getString();
        http.end();
        DynamicJsonDocument doc(512);
        if (deserializeJson(doc, body) == DeserializationError::Ok) {
            String latest = doc["version"] | String("");
            doc["current"]          = APP_VERSION;
            doc["update_available"] = (!latest.isEmpty() && latest != APP_VERSION);
            String out; serializeJson(doc, out);
            app_srv.send(200, "application/json", out);
            return;
        }
        app_srv.send(200, "application/json",
            "{\"error\":true,\"msg\":\"Bad response from server\",\"current\":\"" APP_VERSION "\"}");
        return;
    }
    String errMsg = (code < 0)
        ? String(HTTPClient::errorToString(code))
        : ("HTTP " + String(code));
    http.end();
    Serial.printf("[OTA] check failed: %s\n", errMsg.c_str());
    app_srv.send(200, "application/json",
        "{\"error\":true,\"msg\":\"" + errMsg + "\",\"current\":\"" APP_VERSION "\"}");
}

static void handle_ota_update() {
    app_srv.send(200, "application/json",
        "{\"ok\":true,\"msg\":\"Update started — watch the device screen\"}");
    g_ota_pending = true;
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
    app_srv.on("/ota_check",    HTTP_GET, handle_ota_check);
    app_srv.on("/ota_update",   HTTP_GET, handle_ota_update);
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
    display_init();           // also calls Wire.begin(SDA, SCL) for touch
    pmu_init();               // SY6970 PMU — Wire already up from display_init()
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

    // WiFi — fast reconnect after deep sleep, full WiFiManager on first boot
    bool wifi_done = false;

    if (rtc_wifi_valid) {
        // Fast path: use cached BSSID/channel to skip AP scan (~2 s vs ~8 s)
        ui.set_status("Reconnecting...");
        flush_ui(50);
        WiFi.mode(WIFI_STA);
        // WiFi.begin() with no args reads credentials from NVS (stored by WiFiManager)
        WiFi.begin("", "", rtc_channel, rtc_bssid, true);
        uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 5000) {
            lv_timer_handler(); delay(10);
        }
        wifi_done = (WiFi.status() == WL_CONNECTED);
        if (!wifi_done) {
            rtc_wifi_valid = false;   // stale cache — fall through to WiFiManager
            Serial.println("Fast reconnect failed, trying WiFiManager");
        }
    }

    if (!wifi_done) {
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
    }

    // Modem sleep: WiFi radio sleeps between DTIM beacons when not transmitting.
    // Saves ~20–40 mA average with negligible latency impact on a home network.
    WiFi.setSleep(true);
    Serial.printf("WiFi: %s\n", WiFi.localIP().toString().c_str());

    start_app_server();

    String dip  = load_dirigera_ip();
    String dtok = load_dirigera_token();

    // Register update callback unconditionally — needed for demo mode too.
    // (In demo, _set_power / set_brightness call _on_update directly on main thread;
    //  without this the UI never reflects on/off or slider changes.)
    dc.on_update([&](const HAEntity& e) { g_pending_entity = e; g_update_pending = true; });

    if (dip.isEmpty() || dtok.isEmpty()) {
        ui.set_status(("Pair DIRIGERA:\nhttp://" + WiFi.localIP().toString()).c_str());
        flush_ui(50);
        return;
    }

    ui.set_status(("DIRIGERA\n" + dip + "\nLoading devices...").c_str());
    flush_ui(100);

    dc.on_ready([&]() {
        ui.build_home();
        { int p; bool c; float v; read_battery(p, c, v); ui.set_battery(p, c, v); }
        g_sleep_enabled = true;
        g_last_activity = millis();
    });
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
    // Any touch or button press resets the inactivity timer.
    if (digitalRead(PIN_BTN1) == LOW || digitalRead(PIN_BTN2) == LOW ||
        digitalRead(PIN_TOUCH_INT) == LOW)
        g_last_activity = millis();

    if (g_update_pending) { g_update_pending=false; ui.on_entity_update(g_pending_entity); }

    if (g_ota_pending) { g_ota_pending = false; do_ota_update(); }

    if (g_demo_pending) {
        g_demo_pending = false;
        dc.on_ready([&]() {
            ui.build_home();
            { int p; bool c; float v; read_battery(p, c, v); ui.set_battery(p, c, v); }
            g_sleep_enabled = true;
            g_last_activity = millis();
        });
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
        int pct; bool charging; float bv;
        read_battery(pct, charging, bv);
        ui.set_battery(pct, charging, bv);
        if (charging && !g_was_charging)
            blink_led(3, 80, 80);   // quick flash when USB plugged in
        g_was_charging = charging;
    }

    // Light sleep after inactivity — only once home screen is showing
    if (g_sleep_enabled && cfg_sleep_sec > 0 &&
        millis() - g_last_activity > (uint32_t)cfg_sleep_sec * 1000) {
        go_to_sleep();
    }

    delay(5);
}

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
#include <map>
#include <algorithm>
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

// -- Hidden devices (NVS "hc_hide" / "ids") --

static String load_hidden_ids() {
    prefs.begin("hc_hide", true); String v = prefs.getString("ids",""); prefs.end(); return v;
}
static void save_hidden_ids(const String& ids) {
    prefs.begin("hc_hide", false); prefs.putString("ids", ids); prefs.end();
}

// -- Device meta: custom names + display order (NVS "hc_meta") --

// order: comma-separated entity IDs; names: JSON object {"id":"name",...}
static String load_device_order() {
    prefs.begin("hc_meta", true); String v = prefs.getString("order",""); prefs.end(); return v;
}
static String load_device_names() {
    prefs.begin("hc_meta", true); String v = prefs.getString("names",""); prefs.end(); return v;
}
static void save_device_meta(const String& order, const String& names) {
    prefs.begin("hc_meta", false);
    prefs.putString("order", order);
    prefs.putString("names", names);
    prefs.end();
}

// Runtime copies — kept in sync with NVS and applied to ui after each change.
static std::vector<String>       g_entity_order;
static std::map<String,String>   g_custom_names;

// Parse NVS strings → runtime containers and push to UI.
static void apply_device_meta() {
    // Parse order CSV
    g_entity_order.clear();
    String order_csv = load_device_order();
    if (!order_csv.isEmpty()) {
        int start = 0;
        while (start < (int)order_csv.length()) {
            int comma = order_csv.indexOf(',', start);
            if (comma < 0) comma = order_csv.length();
            String tok = order_csv.substring(start, comma);
            tok.trim();
            if (!tok.isEmpty()) g_entity_order.push_back(tok);
            start = comma + 1;
        }
    }
    // Parse names JSON
    g_custom_names.clear();
    String names_json = load_device_names();
    if (!names_json.isEmpty()) {
        DynamicJsonDocument doc(4096);
        if (deserializeJson(doc, names_json) == DeserializationError::Ok) {
            for (JsonPair p : doc.as<JsonObject>()) {
                String name = p.value().as<String>();
                if (!name.isEmpty()) g_custom_names[p.key().c_str()] = name;
            }
        }
    }
    ui.set_entity_order(g_entity_order);
    ui.set_custom_names(g_custom_names);
}

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
// Device visibility card — rendered into page_live() only (requires paired DIRIGERA)
static String devices_card_html() {
    String h = "<div class='card'><h2>Devices</h2>";
    h += "<p style='font-size:13px;color:#8e8e93;margin-bottom:14px'>"
         "Reorder with \xe2\x96\xb2\xe2\x96\xbc, rename by typing, uncheck to hide. "
         "Changes are local to SwitchPro only.</p>";
    h += "<div id='devlist' style='margin-bottom:12px'>"
         "<span style='color:#636366;font-size:13px'>Loading...</span></div>";
    h += "<div class='row'>";
    h += "<button class='btn' id='devbtn' disabled>Apply</button>";
    h += "<button class='btn gray' id='showall'>Show All</button>";
    h += "</div>";
    h += "<div class='status' id='devs'></div></div>";
    return h;
}

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

    h += devices_card_html();

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
         // -- Devices section --
         "var devData=[];"
         "var BS='background:#3a3a3c;border:none;color:#fff;border-radius:6px;padding:3px 8px;cursor:pointer;font-size:13px';"
         "function mkBtn(t,fn){var b=document.createElement('button');b.textContent=t;b.style.cssText=BS;b.addEventListener('click',fn);return b;}"
         "function renderDevices(){"
         "  var list=document.getElementById('devlist');"
         "  list.innerHTML='';"
         "  if(!devData.length){list.innerHTML='<span style=\"color:#636366;font-size:13px\">No devices found</span>';return;}"
         "  devData.forEach(function(d,i){"
         "    var row=document.createElement('div');"
         "    row.style.cssText='display:flex;align-items:center;gap:8px;padding:8px 0;border-bottom:1px solid #38383a';"
         "    row.dataset.id=d.id;"
         "    var col=document.createElement('div');"
         "    col.style.cssText='display:flex;flex-direction:column;gap:2px';"
         "    var u=mkBtn('\\u25b2',function(){moveItem(devData.indexOf(d),-1);});"
         "    var dn=mkBtn('\\u25bc',function(){moveItem(devData.indexOf(d),1);});"
         "    if(i===0)u.style.opacity='0.25';"
         "    if(i===devData.length-1)dn.style.opacity='0.25';"
         "    col.appendChild(u);col.appendChild(dn);row.appendChild(col);"
         "    var cb=document.createElement('input');"
         "    cb.type='checkbox';cb.checked=!d.hidden;"
         "    cb.style.cssText='width:18px;height:18px;accent-color:#ff9500;flex-shrink:0';"
         "    row.appendChild(cb);"
         "    var ico=document.createElement('span');"
         "    ico.textContent=d.type==='light'?'\\ud83d\\udca1':'\\ud83d\\udd0c';"
         "    row.appendChild(ico);"
         "    var info=document.createElement('div');"
         "    info.style.cssText='flex:1;min-width:0';"
         "    if(d.area){var ar=document.createElement('div');ar.textContent=d.area;ar.style.cssText='font-size:11px;color:#636366;margin-bottom:2px';info.appendChild(ar);}"
         "    var inp=document.createElement('input');"
         "    inp.type='text';inp.placeholder=d.name;inp.value=d.custom_name||'';"
         "    inp.style.cssText='width:100%;background:#1c1c1e;border:1px solid #38383a;color:#fff;border-radius:6px;padding:4px 8px;font-size:14px;margin:0';"
         "    info.appendChild(inp);"
         "    row.appendChild(info);"
         "    list.appendChild(row);"
         "  });"
         "  document.getElementById('devbtn').disabled=false;"
         "}"
         "function moveItem(i,dir){"
         "  var j=i+dir;"
         "  if(j<0||j>=devData.length)return;"
         "  var t=devData[i];devData[i]=devData[j];devData[j]=t;"
         "  renderDevices();"
         "}"
         "function loadDevices(){"
         "  fetch('/api/devices').then(function(r){return r.json();})"
         "  .then(function(list){devData=list;renderDevices();})"
         "  .catch(function(){document.getElementById('devlist').innerHTML='<span style=\"color:#ff453a\">Load failed</span>';});"
         "}"
         "document.getElementById('devbtn').addEventListener('click',function(){"
         "  var rows=document.getElementById('devlist').children;"
         "  var order=[],names={},hidden=[];"
         "  Array.from(rows).forEach(function(row){"
         "    var id=row.dataset.id;if(!id)return;"
         "    order.push(id);"
         "    var cb=row.querySelector('input[type=checkbox]');if(cb&&!cb.checked)hidden.push(id);"
         "    var inp=row.querySelector('input[type=text]');if(inp&&inp.value.trim())names[id]=inp.value.trim();"
         "  });"
         "  var s=document.getElementById('devs');"
         "  s.textContent='Saving...';s.className='status';"
         "  fetch('/api/save_devices',{method:'POST',headers:{'Content-Type':'application/json'},"
         "    body:JSON.stringify({order:order,names:names,hidden:hidden})})"
         "  .then(function(r){return r.json();})"
         "  .then(function(d){s.textContent=d.ok?'\\u2713 Saved':'Error';s.className='status '+(d.ok?'ok':'err');})"
         "  .catch(function(){s.textContent='Error';s.className='status err';});"
         "});"
         "document.getElementById('showall').addEventListener('click',function(){"
         "  document.querySelectorAll('#devlist input[type=checkbox]').forEach(function(cb){cb.checked=true;});"
         "  document.getElementById('devbtn').disabled=false;"
         "});"
         "loadDevices();"
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
               "\"raw_devices\":"+String(dc.dbg_raw_count())+","
               "\"raw_types\":\""+dc.dbg_types()+"\","
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
        DynamicJsonDocument doc(1024);
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

// Diagnostic: proxy a raw DIRIGERA /v1/devices request and return the response.
// Lets us see exactly what DIRIGERA sends (or why it fails) without serial output.
static void handle_proxy_dirigera() {
    String dip  = load_dirigera_ip();
    String dtok = load_dirigera_token();
    if (dip.isEmpty() || dtok.isEmpty()) {
        app_srv.send(200, "application/json", "{\"error\":\"no credentials stored\"}");
        return;
    }
    WiFiClientSecure vc;
    vc.setInsecure();
    HTTPClient http;
    String url = "https://" + dip + ":8443/v1/devices";
    http.begin(vc, url);
    http.addHeader("Authorization", "Bearer " + dtok);
    http.addHeader("Accept", "application/json");
    http.setTimeout(8000);
    int code = http.GET();
    String body = http.getString();
    http.end();
    // Return HTTP code + first 3000 chars of response body
    String out = "{\"http_code\":" + String(code) +
                 ",\"body_len\":" + String(body.length()) +
                 ",\"body_preview\":\"";
    // Escape quotes in body preview
    String preview = body.substring(0, 3000);
    preview.replace("\\", "\\\\");
    preview.replace("\"", "\\\"");
    preview.replace("\n", "\\n");
    preview.replace("\r", "");
    out += preview + "\"}";
    app_srv.send(200, "application/json", out);
}

// -- Device visibility API --

// Escape a string for embedding in a JSON string value.
static String json_esc(const String& s) {
    String out;
    out.reserve(s.length() + 4);
    for (char c : s) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') {}  // skip CR
        else                out += c;
    }
    return out;
}

// GET /api/devices — list all entities sorted by user-defined order,
// including custom_name so the web UI can pre-fill rename inputs.
static void handle_api_devices() {
    // Build area_id → area_name lookup
    auto area_name = [&](const String& aid) -> String {
        for (const auto& a : dc.areas()) if (a.id == aid) return a.name;
        return "";
    };

    // Collect and sort like _build_tabs does
    const auto& entities = dc.entities();
    std::vector<const HAEntity*> sorted;
    for (const auto& e : entities) sorted.push_back(&e);
    if (!g_entity_order.empty()) {
        std::stable_sort(sorted.begin(), sorted.end(),
            [](const HAEntity* a, const HAEntity* b) {
                // Use g_entity_order to find position
                auto pos = [](const String& id) -> int {
                    for (int i = 0; i < (int)g_entity_order.size(); i++)
                        if (g_entity_order[i] == id) return i;
                    return (int)g_entity_order.size();
                };
                return pos(a->entity_id) < pos(b->entity_id);
            });
    }

    String out = "[";
    bool first = true;
    for (const auto* ep : sorted) {
        if (!first) out += ",";
        first = false;
        const HAEntity& e = *ep;
        String type_str = (e.type == EntityType::LIGHT) ? "light" : "outlet";
        bool hid = ui.is_hidden(e.entity_id);
        String custom = "";
        auto it = g_custom_names.find(e.entity_id);
        if (it != g_custom_names.end()) custom = it->second;
        out += "{\"id\":\"" + json_esc(e.entity_id) +
               "\",\"name\":\"" + json_esc(e.friendly_name) +
               "\",\"area\":\"" + json_esc(area_name(e.area_id)) +
               "\",\"type\":\"" + type_str +
               "\",\"hidden\":" + (hid ? "true" : "false") +
               ",\"custom_name\":\"" + json_esc(custom) + "\"}";
    }
    out += "]";
    app_srv.send(200, "application/json", out);
}

// GET /api/set_hidden?ids=id1,id2  — replace the entire hidden list
static void handle_api_set_hidden() {
    String ids = app_srv.arg("ids");   // empty string = show all
    save_hidden_ids(ids);
    ui.set_hidden_ids(ids);
    ui.build_home();
    app_srv.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/save_devices — unified: order, custom names, hidden state.
// Body: {"order":["id1","id2"],"names":{"id1":"name"},"hidden":["id3"]}
static void handle_api_save_devices() {
    String body = app_srv.arg("plain");
    DynamicJsonDocument doc(8192);
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        app_srv.send(400, "application/json", "{\"ok\":false,\"msg\":\"Bad JSON\"}");
        return;
    }

    // Build order CSV
    String order_csv = "";
    g_entity_order.clear();
    for (JsonVariant v : doc["order"].as<JsonArray>()) {
        String id = v.as<String>();
        if (!order_csv.isEmpty()) order_csv += ",";
        order_csv += id;
        g_entity_order.push_back(id);
    }

    // Build hidden CSV
    String hidden_csv = "";
    for (JsonVariant v : doc["hidden"].as<JsonArray>()) {
        if (!hidden_csv.isEmpty()) hidden_csv += ",";
        hidden_csv += v.as<String>();
    }

    // Build names map and persist as JSON
    g_custom_names.clear();
    String names_json = "";
    JsonObject names_obj = doc["names"].as<JsonObject>();
    for (JsonPair p : names_obj) {
        String name = p.value().as<String>();
        if (!name.isEmpty()) g_custom_names[p.key().c_str()] = name;
    }
    serializeJson(names_obj, names_json);

    save_device_meta(order_csv, names_json);
    save_hidden_ids(hidden_csv);

    ui.set_entity_order(g_entity_order);
    ui.set_custom_names(g_custom_names);
    ui.set_hidden_ids(hidden_csv);
    ui.build_home();

    app_srv.send(200, "application/json", "{\"ok\":true}");
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
    app_srv.on("/ota_check",        HTTP_GET, handle_ota_check);
    app_srv.on("/ota_update",       HTTP_GET, handle_ota_update);
    app_srv.on("/api/devices",       HTTP_GET,  handle_api_devices);
    app_srv.on("/api/set_hidden",    HTTP_GET,  handle_api_set_hidden);
    app_srv.on("/api/save_devices",  HTTP_POST, handle_api_save_devices);
    app_srv.on("/proxy_dirigera",    HTTP_GET,  handle_proxy_dirigera);
    app_srv.begin();
    Serial.println("App server on :80");
}

// -- Auto-OTA on cold boot --
// Fetches version.json; if a newer version exists, calls do_ota_update() which
// flashes the firmware and reboots. Only runs on full power-on (not deep-sleep wake).

static void check_ota_on_boot() {
    ui.set_status("Checking updates...");
    flush_ui(50);

    WiFiClientSecure vc;
    vc.setInsecure();
    HTTPClient http;
    http.begin(vc, "https://jenda001224-beep.github.io/home-controller/version.json");
    http.setTimeout(8000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.addHeader("Cache-Control", "no-cache");
    http.addHeader("User-Agent", "ESP32-SwitchPro/1.0");

    int code = http.GET();
    if (code == 200) {
        String body = http.getString();
        http.end();
        DynamicJsonDocument doc(1024);
        if (deserializeJson(doc, body) == DeserializationError::Ok) {
            String latest = doc["version"] | String("");
            if (!latest.isEmpty() && latest != APP_VERSION) {
                Serial.printf("[OTA] boot: updating %s -> %s\n", APP_VERSION, latest.c_str());
                do_ota_update();  // reboots on success; returns only on failure
                return;
            }
            Serial.printf("[OTA] boot: up to date (%s)\n", APP_VERSION);
            return;
        }
    }
    http.end();
    Serial.printf("[OTA] boot check failed: HTTP %d\n", code);
    // Non-fatal — continue normal boot
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

    // Detect wake cause once — used for WiFi strategy and auto-OTA decision.
    esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
    bool is_cold_boot = (wake_cause == ESP_SLEEP_WAKEUP_UNDEFINED);

    // WiFi — three-tier reconnect strategy:
    //   1. Fast path  (BSSID+channel hint) — ~1.5 s typical on sleep wake
    //   2. Medium path (stored NVS creds, full scan) — catches AP channel change
    //   3. Full WiFiManager — first boot or creds wiped
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);   // full radio power while connecting — enabled after link up
    bool wifi_done = false;

    if (rtc_wifi_valid) {
        // Fast path: BSSID/channel hint, stored SSID/pass read from NVS by SDK
        ui.set_status("Reconnecting...");
        flush_ui(50);
        WiFi.begin("", "", rtc_channel, rtc_bssid, true);
        uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 3000) {
            lv_timer_handler(); delay(10);
        }
        wifi_done = (WiFi.status() == WL_CONNECTED);
        if (!wifi_done) {
            WiFi.disconnect(true); delay(100);
            Serial.println("[WiFi] BSSID hint failed, trying plain reconnect");
        }
    }

    if (!wifi_done && rtc_wifi_valid) {
        // Medium path: stored NVS credentials, full channel scan (AP may have moved)
        WiFi.begin();
        uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 5000) {
            lv_timer_handler(); delay(10);
        }
        wifi_done = (WiFi.status() == WL_CONNECTED);
        if (!wifi_done) {
            rtc_wifi_valid = false;
            Serial.println("[WiFi] plain reconnect failed, falling back to WiFiManager");
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
    Serial.printf("[WiFi] %s (wake=%d)\n", WiFi.localIP().toString().c_str(), (int)wake_cause);

    start_app_server();

    // Auto-OTA: on full power-on (not sleep wake), fetch version.json and update if newer.
    // On sleep wake we skip this to keep wake latency low.
    if (is_cold_boot) {
        check_ota_on_boot();
        // If update was available, do_ota_update() rebooted the device.
        // If we reach here: either up to date, or check failed — continue normally.
    }

    // Apply persisted device meta (order, custom names) and visibility filter.
    apply_device_meta();
    ui.set_hidden_ids(load_hidden_ids());

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
static bool     g_btn1_prev    = HIGH;   // BTN1 top-right  (HIGH = not pressed)
static bool     g_btn2_prev    = HIGH;   // BTN2 bottom-right

void loop() {
    app_srv.handleClient();
    dc.loop();
    lv_timer_handler();

    if (g_home_pressed) {
        g_home_pressed  = false;
        g_last_activity = millis();
        ui.go_home();
    }
    // Button edge detection: drive the active detail-panel slider.
    // BTN1 (top)  → increase; BTN2 (bottom) → decrease.
    bool btn1 = digitalRead(PIN_BTN1);
    bool btn2 = digitalRead(PIN_BTN2);
    if (!btn1 || !btn2 || !digitalRead(PIN_TOUCH_INT)) g_last_activity = millis();
    if (g_btn1_prev && !btn1) { ui.btn_slider_step(+1); g_last_activity = millis(); }
    if (g_btn2_prev && !btn2) { ui.btn_slider_step(-1); g_last_activity = millis(); }
    g_btn1_prev = btn1;
    g_btn2_prev = btn2;

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

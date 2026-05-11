#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <HomeSpan.h>
#include <ArduinoJson.h>
#include <lvgl.h>

#include "config.h"
#include "display/display.h"
#include "homekit/light_config.h"
#include "homekit/hk_client.h"
#include "homekit/hk_accessory.h"
#include "ui/ui.h"

// ── Globals ────────────────────────────────────────────────────────────────

static HKClient hk;
static UI       ui;

static volatile bool g_update_pending = false;
static HAEntity      g_pending_entity;

// ── Helpers ────────────────────────────────────────────────────────────────

// Render LVGL a few frames so the display actually shows the status message
static void flush_ui(int ms = 100) {
    uint32_t t = millis();
    while (millis() - t < (uint32_t)ms) {
        lv_timer_handler();
        delay(10);
    }
}

// Blink the backlight N times on startup — visual confirmation firmware runs
static void blink_backlight(int n = 3) {
    for (int i = 0; i < n; i++) {
        display_set_brightness(0);
        delay(120);
        display_set_brightness(200);
        delay(180);
    }
}

// ── WiFi setup (non-blocking portal so display stays live) ─────────────────

static void run_wifi_portal() {
    ui.set_status("WiFi setup:\n1. Join WiFi:\n" SETUP_AP_NAME
                  "\n2. Open Safari:\nhttp://192.168.4.1");
    flush_ui(200);

    WiFiManager wm;
    wm.setTitle("Home Controller");
    wm.setDarkMode(true);
    wm.setConfigPortalBlocking(false);
    wm.startConfigPortal(SETUP_AP_NAME);

    while (WiFi.status() != WL_CONNECTED) {
        wm.process();
        lv_timer_handler();
        delay(5);
    }
}

// ── Light config web server ────────────────────────────────────────────────

static const char CONFIG_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html><html lang="en">
<head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Home Controller — Light Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#1c1c1e;color:#fff;font-family:-apple-system,sans-serif;padding:20px}
h1{font-size:22px;margin-bottom:6px}
p.sub{color:#8e8e93;font-size:14px;margin-bottom:20px}
.card{background:#2c2c2e;border-radius:16px;padding:16px;margin-bottom:16px}
.card h2{font-size:14px;text-transform:uppercase;letter-spacing:.08em;color:#8e8e93;margin-bottom:12px}
.light-row{display:flex;gap:8px;align-items:center;margin-bottom:10px;flex-wrap:wrap}
.light-row input,.light-row select{background:#3a3a3c;border:1px solid #48484a;color:#fff;
  border-radius:8px;padding:8px 10px;font-size:14px;flex:1;min-width:0}
.light-row select{flex:0 0 90px}
.del{background:#ff453a;border:none;color:#fff;border-radius:8px;padding:8px 12px;cursor:pointer;font-size:14px}
.add-btn{background:#3a3a3c;border:1px dashed #636366;color:#8e8e93;border-radius:8px;
  padding:10px;width:100%;cursor:pointer;font-size:14px;margin-top:4px}
.save-btn{background:#ff9500;border:none;color:#fff;border-radius:12px;padding:14px;
  width:100%;font-size:17px;font-weight:600;cursor:pointer;margin-top:8px}
.code{background:#3a3a3c;border-radius:10px;padding:12px;text-align:center;
  font-size:22px;font-weight:700;letter-spacing:4px;color:#ff9500;margin:8px 0}
.ok{display:none;background:#30d158;border-radius:12px;padding:14px;text-align:center;font-size:16px;margin-top:12px}
</style>
</head>
<body>
<h1>&#127968; Home Controller</h1>
<p class="sub">Configure your lights, then open Apple Home and add the accessory.</p>

<div class="card">
<h2>Your Lights</h2>
<div id="list"></div>
<button class="add-btn" onclick="addLight()">+ Add light</button>
</div>

<div class="card">
<h2>Apple Home Pairing Code</h2>
<div class="code" id="code">loading...</div>
<p style="color:#8e8e93;font-size:13px;text-align:center">
  Open Apple Home → + → Add Accessory → Enter code above
</p>
</div>

<button class="save-btn" onclick="save()">Save &amp; Reboot</button>
<div class="ok" id="ok">&#10003; Saved! Rebooting…</div>

<script>
var lights = [];
var idx = 0;

function addLight(n,r,t){
  n=n||''; r=r||''; t=t||'dimmer';
  var id='l'+idx++;
  lights.push({id:id,name:n,room:r,type:t});
  render();
}

function delLight(id){
  lights=lights.filter(function(l){return l.id!==id});
  render();
}

function render(){
  var el=document.getElementById('list');
  el.innerHTML='';
  lights.forEach(function(l){
    var row=document.createElement('div');
    row.className='light-row';
    row.innerHTML=
      '<input placeholder="Light name (e.g. Ceiling)" value="'+esc(l.name)+'" oninput="upd(\''+l.id+'\',\'name\',this.value)">'+
      '<input placeholder="Room (e.g. Living)" value="'+esc(l.room)+'" oninput="upd(\''+l.id+'\',\'room\',this.value)">'+
      '<select onchange="upd(\''+l.id+'\',\'type\',this.value)">'+
        '<option value="switch"'+(l.type=='switch'?' selected':'')+'>Switch</option>'+
        '<option value="dimmer"'+(l.type=='dimmer'?' selected':'')+'>Dimmer</option>'+
        '<option value="color"'+(l.type=='color'?' selected':'')+'>Color</option>'+
      '</select>'+
      '<button class="del" onclick="delLight(\''+l.id+'\')">&#10005;</button>';
    el.appendChild(row);
  });
}

function upd(id,key,val){
  lights.forEach(function(l){if(l.id===id)l[key]=val});
}

function esc(s){return (s||'').replace(/"/g,'&quot;')}

function save(){
  // Re-number IDs
  lights.forEach(function(l,i){l.id='l'+i});
  fetch('/save',{
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify(lights)
  }).then(function(){
    document.getElementById('ok').style.display='block';
  }).catch(function(e){alert('Error: '+e)});
}

// Fetch pairing code from device
fetch('/code').then(function(r){return r.text()})
  .then(function(t){document.getElementById('code').textContent=t})
  .catch(function(){document.getElementById('code').textContent='466-45-544'});

// If reopening setup with existing config
fetch('/lights').then(function(r){return r.json()})
  .then(function(arr){
    lights=arr; idx=arr.length; render();
  }).catch(function(){});
</script>
</body></html>
)rawhtml";

static WebServer* cfg_server = nullptr;
static bool       cfg_saved  = false;

static void handle_config_root() {
    cfg_server->send(200, "text/html; charset=utf-8",
                     String(FPSTR(CONFIG_HTML)));
}

static void handle_config_lights() {
    std::vector<LightCfg> existing;
    lc_load(existing);
    String json = "[";
    for (size_t i = 0; i < existing.size(); i++) {
        if (i) json += ",";
        json += "{\"id\":\"" + existing[i].id + "\","
                "\"name\":\"" + existing[i].name + "\","
                "\"room\":\"" + existing[i].room + "\","
                "\"type\":\"";
        switch (existing[i].type) {
            case LightType::SWITCH: json += "switch"; break;
            case LightType::COLOR:  json += "color";  break;
            default:                json += "dimmer"; break;
        }
        json += "\"}";
    }
    json += "]";
    cfg_server->send(200, "application/json", json);
}

static void handle_config_code() {
    cfg_server->send(200, "text/plain", HK_DISPLAY_CODE);
}

static void handle_config_save() {
    if (!cfg_server->hasArg("plain")) {
        cfg_server->send(400, "text/plain", "No body");
        return;
    }
    String body = cfg_server->arg("plain");

    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        cfg_server->send(400, "text/plain", "Bad JSON");
        return;
    }

    std::vector<LightCfg> lights;
    int i = 0;
    for (JsonObject obj : doc.as<JsonArray>()) {
        LightCfg cfg;
        cfg.id   = "l" + String(i++);
        cfg.name = obj["name"].as<String>();
        cfg.room = obj["room"].as<String>();
        String t = obj["type"].as<String>();
        if (t == "switch") cfg.type = LightType::SWITCH;
        else if (t == "color") cfg.type = LightType::COLOR;
        else cfg.type = LightType::DIMMER;
        if (cfg.name.length() > 0 && cfg.room.length() > 0)
            lights.push_back(cfg);
    }

    if (lights.empty()) {
        cfg_server->send(400, "text/plain", "No valid lights");
        return;
    }

    lc_save(lights);
    cfg_server->send(200, "text/plain", "OK");
    cfg_saved = true;
}

static void run_light_config_server() {
    String ip = WiFi.localIP().toString();
    ui.set_status(("Configure lights:\nhttp://" + ip).c_str());
    flush_ui(200);

    cfg_server = new WebServer(80);
    cfg_server->on("/",       HTTP_GET,  handle_config_root);
    cfg_server->on("/lights", HTTP_GET,  handle_config_lights);
    cfg_server->on("/code",   HTTP_GET,  handle_config_code);
    cfg_server->on("/save",   HTTP_POST, handle_config_save);
    cfg_server->begin();

    while (!cfg_saved) {
        cfg_server->handleClient();
        lv_timer_handler();
        delay(5);
    }

    cfg_server->stop();
    delete cfg_server;
    cfg_server = nullptr;

    ui.set_status("Saved! Rebooting...");
    flush_ui(1500);
    ESP.restart();
}

// ── setup / loop ───────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    pinMode(PIN_RESET_BTN, INPUT_PULLUP);

    // Display — force backlight on immediately, then init LVGL
    display_init();
    ui.begin(&hk);
    ui.set_status("Starting...");
    flush_ui(200);

    // 3 backlight blinks = firmware is alive
    blink_backlight(3);

    // ── Factory reset: hold BOOT button 3 s ──────────────────────────────
    if (digitalRead(PIN_RESET_BTN) == LOW) {
        uint32_t held = millis();
        ui.set_status("Hold 3 s to\nfactory reset...");
        flush_ui(50);
        while (digitalRead(PIN_RESET_BTN) == LOW) {
            lv_timer_handler();
            if (millis() - held > 3000) {
                Preferences prefs;
                prefs.begin("hc_cfg", false); prefs.clear(); prefs.end();
                lc_clear();
                WiFiManager wm; wm.resetSettings();
                ui.set_status("Reset done.\nRebooting...");
                flush_ui(1500);
                ESP.restart();
            }
        }
    }

    // ── WiFi ─────────────────────────────────────────────────────────────
    ui.set_status("Connecting to WiFi...");
    flush_ui(50);

    WiFiManager wm;
    wm.setConfigPortalBlocking(false);
    wm.setTitle("Home Controller");
    wm.setDarkMode(true);

    if (!wm.autoConnect(SETUP_AP_NAME)) {
        // Portal started — update display and keep looping
        ui.set_status("WiFi setup:\n1. Join WiFi:\n" SETUP_AP_NAME
                      "\n2. Open Safari:\nhttp://192.168.4.1");
        flush_ui(50);
        while (WiFi.status() != WL_CONNECTED) {
            wm.process();
            lv_timer_handler();
            delay(5);
        }
    }

    Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());

    // ── Light config ─────────────────────────────────────────────────────
    if (!lc_has_config()) {
        run_light_config_server();
        // never returns (reboots)
    }

    std::vector<LightCfg> light_cfgs;
    lc_load(light_cfgs);

    hk.init(light_cfgs);
    hk.on_ready([&]() { ui.build_home(); });
    hk.on_update([&](const HAEntity& e) {
        g_pending_entity = e;
        g_update_pending = true;
    });

    // ── HomeSpan setup ───────────────────────────────────────────────────
    homeSpan.setPairingCode(HK_PAIRING_CODE);
    homeSpan.setQRID(HK_SETUP_ID);
    homeSpan.setHostNameSuffix("");
    homeSpan.setLogLevel(0);  // quiet Serial
    homeSpan.begin(Category::Bridges, "Home Controller", "HC");

    // Bridge accessory (required by HAP spec)
    new SpanAccessory();
    new Service::AccessoryInformation();
    new Characteristic::Identify();
    new Characteristic::Name("Home Controller");
    new Characteristic::Manufacturer("DIY");
    new Characteristic::Model("T-Display-S3-Pro");
    new Characteristic::SerialNumber("HC001");
    new Characteristic::FirmwareRevision("2.0");

    // One accessory per configured light
    for (const auto& cfg : light_cfgs) {
        new SpanAccessory();
        new Service::AccessoryInformation();
        new Characteristic::Identify();
        new Characteristic::Name(cfg.name.c_str());
        new Characteristic::Manufacturer("DIY");
        new Characteristic::Model("Smart Light");
        new Characteristic::SerialNumber(cfg.id.c_str());
        new Characteristic::FirmwareRevision("1.0");

        DEV_Light* dev = new DEV_Light(cfg, &hk);
        hk.register_dev(dev);
    }

    // Show pairing code briefly, then build home screen
    ui.set_status("Apple Home\nPairing code:\n" HK_DISPLAY_CODE
                  "\n\nOpen Apple Home\n+ Add Accessory");
    flush_ui(4000);

    hk.fire_ready();  // triggers ui.build_home()
}

void loop() {
    homeSpan.poll();
    lv_timer_handler();
    if (g_update_pending) {
        g_update_pending = false;
        ui.on_entity_update(g_pending_entity);
    }
    delay(5);
}

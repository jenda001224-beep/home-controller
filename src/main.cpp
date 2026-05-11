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

// -- Helpers --

static void flush_ui(int ms = 100) {
    uint32_t t = millis();
    while (millis() - t < (uint32_t)ms) { lv_timer_handler(); delay(10); }
}

static void blink_led(int n = 3) {
    pinMode(PIN_LED, OUTPUT);
    for (int i = 0; i < n; i++) {
        digitalWrite(PIN_LED, HIGH); delay(150);
        digitalWrite(PIN_LED, LOW);  delay(150);
    }
}

// -- Battery --

static int read_battery_pct() {
    analogSetPinAttenuation(PIN_BAT_ADC, ADC_11db);
    int   raw = analogRead(PIN_BAT_ADC);
    float v   = (raw / 4095.0f) * 3.3f * 2.0f;
    int   pct = (int)((v - 3.0f) / 1.2f * 100.0f);
    return constrain(pct, 0, 100);
}

// -- DIRIGERA config storage --

static String load_dirigera_ip()    { prefs.begin("hc_dr",true); String v=prefs.getString("ip","");    prefs.end(); return v; }
static String load_dirigera_token() { prefs.begin("hc_dr",true); String v=prefs.getString("token",""); prefs.end(); return v; }
static void save_dirigera(const String& ip, const String& token) {
    prefs.begin("hc_dr",false);
    prefs.putString("ip",    ip);
    prefs.putString("token", token);
    prefs.end();
}
static void clear_dirigera() {
    prefs.begin("hc_dr",false); prefs.clear(); prefs.end();
}

// =============================================================
//  Single web server — starts right after WiFi, serves both
//  the DIRIGERA pairing page and the live demo/status page.
// =============================================================

static WebServer app_srv(80);

// Pairing state machine
enum class PairState { IDLE, WAITING, DONE, FAILED };
static PairState pair_state = PairState::IDLE;
static String    pair_ip;

// ---- HTML pages ----

static const char PAGE_SETUP[] PROGMEM = R"html(
<!DOCTYPE html><html lang="en">
<head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>SwitchPro Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#1c1c1e;color:#fff;font-family:-apple-system,sans-serif;padding:24px}
h1{font-size:22px;margin-bottom:6px}
.sub{color:#8e8e93;font-size:14px;margin-bottom:24px}
.card{background:#2c2c2e;border-radius:16px;padding:20px;margin-bottom:16px;border:1px solid #38383a}
.card h2{font-size:11px;text-transform:uppercase;letter-spacing:.08em;color:#636366;margin-bottom:12px}
input{display:block;width:100%;background:#3a3a3c;border:1px solid #48484a;color:#fff;
  border-radius:10px;padding:12px 14px;font-size:16px;margin-bottom:12px}
.row{display:flex;gap:10px}
.btn{flex:1;background:#ff9500;border:none;color:#fff;border-radius:12px;
  padding:14px;font-size:16px;font-weight:600;cursor:pointer}
.btn:active{opacity:.7}
.btn.gray{background:#3a3a3c;color:#ebebf5}
.status{margin-top:12px;font-size:14px;color:#8e8e93;text-align:center;min-height:20px}
.ok{color:#30d158}.err{color:#ff453a}
.hint{margin-top:8px;font-size:13px;color:#636366;line-height:1.8}
</style>
</head>
<body>
<h1>&#127968; SwitchPro</h1>
<p class="sub">Device setup</p>

<div class="card">
<h2>DIRIGERA Hub</h2>
<input type="text" id="ip" placeholder="Hub IP (e.g. 192.168.1.50)" inputmode="decimal">
<button class="btn" id="pair-btn" onclick="startPair()">Pair with DIRIGERA</button>
<div class="status" id="pair-status"></div>
<div class="hint">
1. Enter DIRIGERA hub IP<br>
2. Tap Pair<br>
3. Press the button on the <b>back</b> of DIRIGERA (within 30s)
</div>
</div>

<div class="card">
<h2>Preview</h2>
<p style="font-size:13px;color:#8e8e93;margin-bottom:12px">See how the UI looks with demo data.</p>
<div class="row">
  <button class="btn" onclick="loadDemo()">&#127916; Load Demo</button>
  <button class="btn gray" onclick="clearDemo()">Clear</button>
</div>
<div class="status" id="demo-status"></div>
</div>

<script>
function startPair() {
  var ip = document.getElementById('ip').value.trim();
  if (!ip) { setStatus('pair-status','Enter the hub IP','err'); return; }
  document.getElementById('pair-btn').disabled = true;
  setStatus('pair-status','Contacting hub...');
  fetch('/pair_start?ip='+encodeURIComponent(ip))
    .then(function(r){return r.json()})
    .then(function(d){
      if (d.ok) { setStatus('pair-status','Press the button on the back of DIRIGERA!'); pollPair(); }
      else { setStatus('pair-status','Error: '+d.msg,'err'); document.getElementById('pair-btn').disabled=false; }
    }).catch(function(){ setStatus('pair-status','Network error','err'); document.getElementById('pair-btn').disabled=false; });
}
function pollPair() {
  setTimeout(function(){
    fetch('/pair_status').then(function(r){return r.json()}).then(function(d){
      if (d.done) { setStatus('pair-status','Paired! Rebooting...','ok'); }
      else if (d.failed) { setStatus('pair-status','Timed out. Try again.','err'); document.getElementById('pair-btn').disabled=false; }
      else { setStatus('pair-status','Waiting for button press...'); pollPair(); }
    }).catch(function(){pollPair()});
  }, 1500);
}
function loadDemo() {
  setStatus('demo-status','Loading...');
  fetch('/demo').then(function(r){return r.json()}).then(function(d){
    setStatus('demo-status', d.ok ? 'Demo loaded on device!' : 'Error', d.ok ? 'ok' : 'err');
  }).catch(function(){ setStatus('demo-status','Error','err'); });
}
function clearDemo() {
  fetch('/clear_demo');
  setStatus('demo-status','Cleared');
}
function setStatus(id, msg, cls) {
  var el=document.getElementById(id);
  el.textContent=msg; el.className='status '+(cls||'');
}
</script>
</body></html>
)html";

static const char PAGE_LIVE[] PROGMEM = R"html(
<!DOCTYPE html><html lang="en">
<head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>SwitchPro</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#1c1c1e;color:#fff;font-family:-apple-system,sans-serif;padding:24px}
h1{font-size:22px;margin-bottom:6px}
.sub{color:#8e8e93;font-size:14px;margin-bottom:24px}
.card{background:#2c2c2e;border-radius:16px;padding:20px;margin-bottom:16px;border:1px solid #38383a}
.card h2{font-size:11px;text-transform:uppercase;letter-spacing:.08em;color:#636366;margin-bottom:12px}
.row{display:flex;gap:10px}
.btn{flex:1;background:#ff9500;border:none;color:#fff;border-radius:12px;
  padding:14px;font-size:16px;font-weight:600;cursor:pointer}
.btn:active{opacity:.7}
.btn.gray{background:#3a3a3c;color:#ebebf5}
.status{margin-top:12px;font-size:13px;color:#8e8e93;text-align:center;min-height:18px}
.ok{color:#30d158}.err{color:#ff453a}
.info{font-size:13px;color:#8e8e93;line-height:1.9}
.info b{color:#fff}
</style>
</head>
<body>
<h1>&#127968; SwitchPro</h1>
<p class="sub">Device control</p>

<div class="card">
<h2>Demo</h2>
<p style="font-size:13px;color:#8e8e93;margin-bottom:12px">Load fake rooms onto the display.</p>
<div class="row">
  <button class="btn" onclick="runDemo()">&#127916; Load Demo</button>
  <button class="btn gray" onclick="doRefresh()">&#8635; Refresh</button>
</div>
<div class="status" id="status"></div>
</div>

<div class="card">
<h2>Status</h2>
<div class="info" id="info">Loading...</div>
</div>

<script>
function runDemo(){
  setStatus('Loading...');
  fetch('/demo').then(r=>r.json()).then(d=>setStatus(d.ok?'Demo loaded!':'Error',d.ok?'ok':'err')).catch(()=>setStatus('Error','err'));
}
function doRefresh(){
  setStatus('Refreshing...');
  fetch('/refresh').then(r=>r.json()).then(d=>{setStatus(d.ok?'Done!':'Error',d.ok?'ok':'err');loadInfo();}).catch(()=>setStatus('Error','err'));
}
function setStatus(msg,cls){var el=document.getElementById('status');el.textContent=msg;el.className='status '+(cls||'');}
function loadInfo(){
  fetch('/status').then(r=>r.json()).then(d=>{
    document.getElementById('info').innerHTML=
      '<b>IP:</b> '+d.ip+'<br><b>WiFi:</b> '+d.ssid+'<br><b>DIRIGERA:</b> '+d.dirigera+'<br><b>Devices:</b> '+d.devices+'<br><b>Uptime:</b> '+d.uptime+'s';
  }).catch(()=>{document.getElementById('info').textContent='Failed to load';});
}
loadInfo();
</script>
</body></html>
)html";

// ---- Route handlers ----

static void handle_root() {
    String dip = load_dirigera_ip();
    if (dip.isEmpty()) {
        app_srv.send(200, "text/html;charset=utf-8", String(FPSTR(PAGE_SETUP)));
    } else {
        app_srv.send(200, "text/html;charset=utf-8", String(FPSTR(PAGE_LIVE)));
    }
}

static void handle_pair_start() {
    if (!app_srv.hasArg("ip")) {
        app_srv.send(400, "application/json", "{\"ok\":false,\"msg\":\"No IP\"}");
        return;
    }
    pair_ip    = app_srv.arg("ip");
    pair_state = PairState::WAITING;
    app_srv.send(200, "application/json", "{\"ok\":true}");
}

static void handle_pair_status() {
    String json;
    if      (pair_state == PairState::DONE)   json = "{\"done\":true}";
    else if (pair_state == PairState::FAILED)  json = "{\"failed\":true}";
    else                                        json = "{\"done\":false,\"failed\":false}";
    app_srv.send(200, "application/json", json);
}

static void handle_demo() {
    g_demo_pending = true;
    app_srv.send(200, "application/json", "{\"ok\":true}");
}

static void handle_clear_demo() {
    // reboot back to real data — simplest approach
    app_srv.send(200, "application/json", "{\"ok\":true}");
    delay(300);
    ESP.restart();
}

static void handle_refresh() {
    app_srv.send(200, "application/json", "{\"ok\":true}");
    // dc.loop() will re-poll on next cycle
}

static void handle_status() {
    String body = "{\"ip\":\"" + WiFi.localIP().toString() + "\","
                  "\"ssid\":\"" + WiFi.SSID() + "\","
                  "\"dirigera\":\"" + load_dirigera_ip() + "\","
                  "\"devices\":" + String(dc.entities().size()) + ","
                  "\"uptime\":" + String(millis()/1000) + "}";
    app_srv.send(200, "application/json", body);
}

static void start_app_server() {
    app_srv.on("/",            HTTP_GET, handle_root);
    app_srv.on("/pair_start",  HTTP_GET, handle_pair_start);
    app_srv.on("/pair_status", HTTP_GET, handle_pair_status);
    app_srv.on("/demo",        HTTP_GET, handle_demo);
    app_srv.on("/clear_demo",  HTTP_GET, handle_clear_demo);
    app_srv.on("/refresh",     HTTP_GET, handle_refresh);
    app_srv.on("/status",      HTTP_GET, handle_status);
    app_srv.begin();
    Serial.println("App server started on :80");
}

// -- setup / loop --

void setup() {
    Serial.begin(115200);
    pinMode(PIN_RESET_BTN, INPUT_PULLUP);

    display_init();
    ui.begin(&dc);
    ui.set_status("Starting...");
    flush_ui(200);

    blink_led(3);

    // Factory reset: hold BOOT 3 s
    if (digitalRead(PIN_RESET_BTN) == LOW) {
        uint32_t held = millis();
        ui.set_status("Hold 3s to\nfactory reset...");
        flush_ui(50);
        while (digitalRead(PIN_RESET_BTN) == LOW) {
            lv_timer_handler();
            if (millis() - held > 3000) {
                clear_dirigera();
                WiFiManager wm; wm.resetSettings();
                ui.set_status("Reset done.\nRebooting...");
                flush_ui(1500);
                ESP.restart();
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
        "body,div,section{background:#1c1c1e!important;color:#fff!important}"
        ".wrap{background:#2c2c2e!important;border:1px solid #38383a!important;box-shadow:none!important}"
        "h1,h2,h3,p,label,li,div{color:#fff!important}"
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

    // Start web server immediately after WiFi — demo always available
    start_app_server();

    String dip  = load_dirigera_ip();
    String dtok = load_dirigera_token();

    if (dip.isEmpty() || dtok.isEmpty()) {
        // Show pairing instructions and let loop() handle the rest
        ui.set_status(("Pair DIRIGERA:\nhttp://" + WiFi.localIP().toString()).c_str());
        flush_ui(50);
        return;
    }

    // Connect to DIRIGERA
    ui.set_status(("DIRIGERA\n" + dip + "\nLoading devices...").c_str());
    flush_ui(100);

    dc.on_ready([&]() { ui.build_home(); });
    dc.on_update([&](const HAEntity& e) {
        g_pending_entity = e;
        g_update_pending = true;
    });
    dc.begin(dip, dtok);
}

static uint32_t g_bat_last = 0;

void loop() {
    app_srv.handleClient();
    dc.loop();
    lv_timer_handler();

    // Home button
    if (g_home_pressed) {
        g_home_pressed = false;
        ui.go_home();
    }

    if (g_update_pending) {
        g_update_pending = false;
        ui.on_entity_update(g_pending_entity);
    }

    if (g_demo_pending) {
        g_demo_pending = false;
        // Set callbacks if not already set (e.g. when in pairing mode)
        dc.on_ready([&]() { ui.build_home(); });
        dc.load_demo();
    }

    // Handle DIRIGERA pairing state machine
    if (pair_state == PairState::WAITING) {
        pair_state = PairState::IDLE;   // prevent re-entry
        String ip_str = WiFi.localIP().toString();
        ui.set_status(("Pair DIRIGERA:\nhttp://" + ip_str + "\n\nPress button on hub!").c_str());
        flush_ui(50);

        String token;
        bool ok = DirigeraClient::pair(pair_ip, token);
        if (ok) {
            save_dirigera(pair_ip, token);
            pair_state = PairState::DONE;
            ui.set_status("Paired!\nRebooting...");
            flush_ui(2000);
            ESP.restart();
        } else {
            pair_state = PairState::FAILED;
            ui.set_status(("Pair DIRIGERA:\nhttp://" + ip_str + "\n\nFailed. Try again.").c_str());
            flush_ui(50);
        }
    }

    // Battery update every 30 s
    if (millis() - g_bat_last > 30000) {
        g_bat_last = millis();
        ui.set_battery(read_battery_pct());
    }

    delay(5);
}

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

// ── Globals ──────────────────────────────────────────────────────────────────

static DirigeraClient dc;
static UI             ui;

static volatile bool g_update_pending = false;
static HAEntity      g_pending_entity;

static Preferences prefs;

// ── Helpers ──────────────────────────────────────────────────────────────────

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

// ── DIRIGERA config storage ──────────────────────────────────────────────────

static String load_dirigera_ip()    { prefs.begin("hc_dr",true); String v=prefs.getString("ip","");    prefs.end(); return v; }
static String load_dirigera_token() { prefs.begin("hc_dr",true); String v=prefs.getString("token",""); prefs.end(); return v; }
static void   save_dirigera(const String& ip, const String& token) {
    prefs.begin("hc_dr",false);
    prefs.putString("ip",    ip);
    prefs.putString("token", token);
    prefs.end();
}
static void clear_dirigera() {
    prefs.begin("hc_dr",false); prefs.clear(); prefs.end();
}

// ── Setup web server (DIRIGERA pairing) ──────────────────────────────────────

static const char SETUP_HTML[] PROGMEM = R"html(
<!DOCTYPE html><html lang="en">
<head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Home Controller Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#1c1c1e;color:#fff;font-family:-apple-system,sans-serif;padding:24px}
h1{font-size:22px;margin-bottom:6px}p.sub{color:#8e8e93;font-size:14px;margin-bottom:24px}
.card{background:#2c2c2e;border-radius:16px;padding:20px;margin-bottom:16px}
.card h2{font-size:12px;text-transform:uppercase;letter-spacing:.08em;color:#8e8e93;margin-bottom:12px}
input{display:block;width:100%;background:#3a3a3c;border:1px solid #48484a;color:#fff;
  border-radius:10px;padding:12px 14px;font-size:16px;margin-bottom:12px}
.btn{display:block;width:100%;background:#ff9500;border:none;color:#fff;border-radius:12px;
  padding:14px;font-size:17px;font-weight:600;cursor:pointer}
.btn:disabled{opacity:.4}
.status{margin-top:14px;font-size:14px;color:#8e8e93;text-align:center;min-height:20px}
.ok{color:#30d158}.err{color:#ff453a}
.steps{margin-top:8px;font-size:13px;color:#636366;line-height:1.8;padding-left:4px}
</style>
</head>
<body>
<h1>&#127968; Home Controller</h1>
<p class="sub">Connect to your IKEA DIRIGERA hub</p>

<div class="card">
<h2>DIRIGERA Hub</h2>
<input type="text" id="ip" placeholder="Hub IP address (e.g. 192.168.1.50)"
       inputmode="decimal">
<button class="btn" id="btn" onclick="startPair()">Pair with DIRIGERA</button>
<div class="status" id="status"></div>
<div class="steps">
1. Enter the IP address of your DIRIGERA hub<br>
2. Click Pair<br>
3. Press the action button on the <b>back</b> of DIRIGERA (within 30s)<br>
4. Done — device reboots and connects
</div>
</div>

<script>
var polling = false;

function startPair() {
  var ip = document.getElementById('ip').value.trim();
  if (!ip) { setStatus('Enter the hub IP address','err'); return; }
  document.getElementById('btn').disabled = true;
  setStatus('Contacting hub...');
  fetch('/pair_start?ip='+encodeURIComponent(ip))
    .then(function(r){return r.json()})
    .then(function(d){
      if (d.ok) {
        setStatus('&#128276; Press the button on the back of DIRIGERA now!');
        pollStatus();
      } else {
        setStatus('Error: '+d.msg,'err');
        document.getElementById('btn').disabled=false;
      }
    }).catch(function(e){
      setStatus('Network error','err');
      document.getElementById('btn').disabled=false;
    });
}

function pollStatus() {
  setTimeout(function(){
    fetch('/pair_status').then(function(r){return r.json()}).then(function(d){
      if (d.done) {
        setStatus('&#10003; Paired! Rebooting...','ok');
      } else if (d.failed) {
        setStatus('Timed out — button not pressed in time. Try again.','err');
        document.getElementById('btn').disabled=false;
      } else {
        setStatus(d.msg || 'Waiting for button press...');
        pollStatus();
      }
    }).catch(function(){pollStatus()});
  }, 1500);
}

function setStatus(msg,cls) {
  var el = document.getElementById('status');
  el.innerHTML = msg;
  el.className = 'status '+(cls||'');
}
</script>
</body></html>
)html";

static WebServer* cfg_srv = nullptr;

// Pairing state machine
enum class PairState { IDLE, WAITING, DONE, FAILED };
static PairState  pair_state = PairState::IDLE;
static String     pair_ip;

static void handle_root()    { cfg_srv->send(200,"text/html;charset=utf-8", String(FPSTR(SETUP_HTML))); }
static void handle_pair_status() {
    String json;
    if (pair_state == PairState::DONE)   json = "{\"done\":true}";
    else if (pair_state == PairState::FAILED) json = "{\"failed\":true}";
    else json = "{\"done\":false,\"failed\":false,\"msg\":\"Waiting...\"}";
    cfg_srv->send(200,"application/json", json);
}
static void handle_pair_start() {
    if (!cfg_srv->hasArg("ip")) { cfg_srv->send(400,"application/json","{\"ok\":false,\"msg\":\"No IP\"}"); return; }
    pair_ip    = cfg_srv->arg("ip");
    pair_state = PairState::WAITING;
    cfg_srv->send(200,"application/json","{\"ok\":true}");
}

static void run_setup_server() {
    String ip_str = WiFi.localIP().toString();
    ui.set_status(("Configure:\nhttp://" + ip_str).c_str());
    flush_ui(200);

    cfg_srv = new WebServer(80);
    cfg_srv->on("/",            HTTP_GET, handle_root);
    cfg_srv->on("/pair_start",  HTTP_GET, handle_pair_start);
    cfg_srv->on("/pair_status", HTTP_GET, handle_pair_status);
    cfg_srv->begin();

    while (true) {
        cfg_srv->handleClient();
        lv_timer_handler();

        if (pair_state == PairState::WAITING) {
            ui.set_status(("Configure:\nhttp://" + ip_str + "\n\nPress button on\nDIRIGERA hub!").c_str());
            flush_ui(50);
            pair_state = PairState::IDLE;  // prevent re-entry

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
                ui.set_status(("Configure:\nhttp://" + ip_str + "\n\nPairing failed.\nTry again.").c_str());
                flush_ui(50);
            }
        }
        delay(5);
    }
}

// ── setup / loop ─────────────────────────────────────────────────────────────

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
    wm.setTitle("Home Controller");
    wm.setDarkMode(true);
    wm.setConfigPortalBlocking(false);
    if (!wm.autoConnect(SETUP_AP_NAME)) {
        ui.set_status("WiFi setup:\nJoin " SETUP_AP_NAME "\nOpen Safari:\nhttp://192.168.4.1");
        flush_ui(50);
        while (WiFi.status() != WL_CONNECTED) { wm.process(); lv_timer_handler(); delay(5); }
    }
    Serial.printf("WiFi: %s\n", WiFi.localIP().toString().c_str());

    // DIRIGERA config
    String dip   = load_dirigera_ip();
    String dtok  = load_dirigera_token();
    if (dip.isEmpty() || dtok.isEmpty()) {
        run_setup_server();  // never returns
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

void loop() {
    dc.loop();
    lv_timer_handler();

    // Home button → close detail / go to home screen
    if (g_home_pressed) {
        g_home_pressed = false;
        ui.go_home();
    }

    if (g_update_pending) {
        g_update_pending = false;
        ui.on_entity_update(g_pending_entity);
    }
    delay(5);
}

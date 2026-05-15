#include "dirigera_client.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <mbedtls/sha256.h>
#include <mbedtls/base64.h>
#include <esp_random.h>
#include <math.h>

// Static FreeRTOS members
QueueHandle_t DirigeraClient::s_patch_q   = nullptr;
TaskHandle_t  DirigeraClient::s_patch_task = nullptr;
TaskHandle_t  DirigeraClient::s_fetch_task = nullptr;

// ── Helpers ─────────────────────────────────────────────────────────────────
// NOTE: secure_client() is used only on the main thread (pairing flow).
//       Background tasks create their own WiFiClientSecure on the stack.

static WiFiClientSecure& secure_client() {
    static WiFiClientSecure c;
    c.setInsecure();  // DIRIGERA uses self-signed cert
    return c;
}

// GET that creates its own WiFiClientSecure — safe to call from any task
static bool http_get_bg(const String& url, const String& token, String& out) {
    WiFiClientSecure vc;
    vc.setInsecure();
    HTTPClient http;
    http.begin(vc, url);
    http.addHeader("Authorization", "Bearer " + token);
    http.addHeader("Accept", "application/json");
    http.setTimeout(8000);
    int code = http.GET();
    if (code == 200) { out = http.getString(); http.end(); return true; }
    Serial.printf("DIRIGERA GET %d  %s\n", code, url.c_str());
    http.end(); return false;
}

// ── OAuth PKCE ───────────────────────────────────────────────────────────────

static String gen_verifier() {
    const char* alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    String v; v.reserve(128);
    for (int i = 0; i < 128; i++) v += alpha[esp_random() % 62];
    return v;
}

static String gen_challenge(const String& verifier) {
    uint8_t hash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts_ret(&ctx, 0);
    mbedtls_sha256_update_ret(&ctx, (const uint8_t*)verifier.c_str(), verifier.length());
    mbedtls_sha256_finish_ret(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    uint8_t b64[64]; size_t b64_len;
    mbedtls_base64_encode(b64, sizeof(b64), &b64_len, hash, 32);

    String out;
    for (size_t i = 0; i < b64_len; i++) {
        char c = (char)b64[i];
        if (c == '+') out += '-';
        else if (c == '/') out += '_';
        else if (c == '=') break;
        else out += c;
    }
    return out;
}

/* static */ bool DirigeraClient::pair(const String& hub_ip, String& out_token) {
    String verifier  = gen_verifier();
    String challenge = gen_challenge(verifier);
    String base      = "https://" + hub_ip + ":8443/v1";

    // Step 1: get authorization code
    String auth_url = base + "/oauth/authorize"
        "?audience=homesmart.local&response_type=code"
        "&code_challenge=" + challenge + "&code_challenge_method=S256";

    HTTPClient http;
    http.begin(secure_client(), auth_url);
    http.addHeader("Accept", "application/json");
    http.setTimeout(10000);
    int code = http.GET();
    if (code != 200) {
        Serial.printf("DIRIGERA auth failed: %d\n", code);
        http.end(); return false;
    }
    String resp = http.getString();
    http.end();

    DynamicJsonDocument doc(256);
    deserializeJson(doc, resp);
    String auth_code = doc["code"].as<String>();
    if (auth_code.isEmpty()) return false;

    // Step 2: poll for token (user must press button within 30s)
    String token_url = base + "/oauth/token";
    String body = "code=" + auth_code +
                  "&name=HomeController" +
                  "&grant_type=authorization_code" +
                  "&code_verifier=" + verifier;

    for (int attempt = 0; attempt < 15; attempt++) {
        delay(2000);
        HTTPClient th;
        th.begin(secure_client(), token_url);
        th.addHeader("Content-Type", "application/x-www-form-urlencoded");
        th.setTimeout(5000);
        int tc = th.POST(body);
        if (tc == 200) {
            String tr = th.getString();
            th.end();
            DynamicJsonDocument td(512);
            deserializeJson(td, tr);
            out_token = td["access_token"].as<String>();
            return !out_token.isEmpty();
        }
        th.end();
    }
    return false;
}

// ── Device parsing ───────────────────────────────────────────────────────────

static void rgb_to_hs(uint8_t r, uint8_t g, uint8_t b, float& h, float& s) {
    float rf = r/255.f, gf = g/255.f, bf = b/255.f;
    float mx = max({rf,gf,bf}), mn = min({rf,gf,bf}), d = mx-mn;
    s = mx == 0 ? 0 : d/mx;
    if (d == 0) { h = 0; return; }
    if (mx == rf)      h = 60.f * fmodf((gf-bf)/d, 6.f);
    else if (mx == gf) h = 60.f * ((bf-rf)/d + 2.f);
    else               h = 60.f * ((rf-gf)/d + 4.f);
    if (h < 0) h += 360.f;
}

static void hs_to_rgb(float h, float s, uint8_t& r, uint8_t& g, uint8_t& b) {
    float c = s, x = c*(1-fabsf(fmodf(h/60.f,2.f)-1)), m = 1-c;
    float rf,gf,bf;
    int seg = (int)(h/60.f)%6;
    switch(seg){case 0:rf=c;gf=x;bf=0;break;case 1:rf=x;gf=c;bf=0;break;
                case 2:rf=0;gf=c;bf=x;break;case 3:rf=0;gf=x;bf=c;break;
                case 4:rf=x;gf=0;bf=c;break;default:rf=c;gf=0;bf=x;}
    r=(uint8_t)((rf+m)*255); g=(uint8_t)((gf+m)*255); b=(uint8_t)((bf+m)*255);
}

// ── _fetch_devices ───────────────────────────────────────────────────────────
// Called exclusively from the fetch task (core 0).
// HTTP and JSON parsing happen without holding _mutex.
// Only the merge step takes _mutex — that window is very short.

void DirigeraClient::_fetch_devices() {
    if (WiFi.status() != WL_CONNECTED) return;

    String resp;
    if (!http_get_bg("https://" + _hub_ip + ":8443/v1/devices", _token, resp)) return;

    DynamicJsonDocument doc(32768);
    if (deserializeJson(doc, resp) != DeserializationError::Ok) return;

    // Parse into local vectors (no lock needed — not shared yet)
    std::vector<HAArea>   new_areas;
    std::vector<HAEntity> new_entities;
    std::vector<String>   seen_rooms;

    // Debug: collect raw device count + all types seen (before filter)
    int    dbg_raw = 0;
    String dbg_types;
    for (JsonObject dev : doc.as<JsonArray>()) {
        String dtype = dev["type"].as<String>();
        dbg_raw++;
        if (dbg_types.length() < 200) {
            if (dbg_types.length()) dbg_types += ",";
            dbg_types += dtype;
        }
    }
    Serial.printf("[DIRIGERA] %d devices total, types: %s\n", dbg_raw, dbg_types.c_str());
    // Store for web /status endpoint (no mutex needed — only written here, read on main thread)
    _dbg_raw_count = dbg_raw;
    _dbg_types     = dbg_types;

    for (JsonObject dev : doc.as<JsonArray>()) {
        String dtype = dev["type"].as<String>();
        if (dtype != "LIGHT" && dtype != "OUTLET") continue;

        String room_id   = dev["room"]["id"].as<String>();
        String room_name = dev["room"]["name"].as<String>();
        if (room_name.isEmpty()) room_name = "No room";
        if (room_id.isEmpty())   room_id   = "noroom";

        bool seen = false;
        for (auto& s : seen_rooms) if (s == room_id) { seen = true; break; }
        if (!seen) {
            seen_rooms.push_back(room_id);
            HAArea a; a.id = room_id; a.name = room_name;
            new_areas.push_back(a);
        }

        JsonObject attr = dev["attributes"];
        HAEntity e;
        e.entity_id     = dev["id"].as<String>();
        e.friendly_name = attr["customName"].as<String>();
        if (e.friendly_name.isEmpty()) e.friendly_name = dtype;
        e.area_id = room_id;
        e.state   = attr["isOn"].as<bool>() ? "on" : "off";

        if (dtype == "LIGHT") {
            e.type = EntityType::LIGHT;
            e.supports_brightness = attr.containsKey("lightLevel");
            e.supports_color      = attr.containsKey("colorHue");
            if (e.supports_brightness)
                e.brightness = (uint8_t)(attr["lightLevel"].as<int>() * 255 / 100);
            if (e.supports_color) {
                float h = attr["colorHue"].as<float>();
                float s = attr["colorSaturation"].as<float>();
                hs_to_rgb(h, s, e.r, e.g, e.b);
            }
        } else {
            e.type = EntityType::SWITCH;
        }

        new_entities.push_back(e);
    }

    // ── Merge under mutex ────────────────────────────────────────
    xSemaphoreTake(_mutex, portMAX_DELAY);

    // Use _fetched_once (not _entities.empty()) so on_ready fires exactly once
    // even when DIRIGERA returns 0 LIGHT/OUTLET devices (empty new_entities).
    bool first_load = !_fetched_once;

    if (first_load) {
        _fetched_once = true;
        _areas    = new_areas;
        _entities = new_entities;
        xSemaphoreGive(_mutex);
        _ready_pending = true;   // main thread will call _on_ready() once
        return;
    }

    // Subsequent fetches: merge and track what changed
    for (auto& ne : new_entities) {
        bool found = false;
        for (auto& oe : _entities) {
            if (oe.entity_id == ne.entity_id) {
                bool changed = (oe.state != ne.state || oe.brightness != ne.brightness);
                oe = ne;
                if (changed && _upd_count < MAX_UPD)
                    ne.entity_id.toCharArray(_upd_ids[_upd_count++], sizeof(_upd_ids[0]));
                found = true;
                break;
            }
        }
        if (!found) _entities.push_back(ne);
    }

    xSemaphoreGive(_mutex);
}

// ── FreeRTOS tasks ───────────────────────────────────────────────────────────

/* static */
void DirigeraClient::_patch_worker(void* arg) {
    DirigeraClient* self = (DirigeraClient*)arg;
    PatchJob job;
    for (;;) {
        if (xQueueReceive(s_patch_q, &job, portMAX_DELAY) == pdTRUE) {
            if (WiFi.status() != WL_CONNECTED) continue;
            WiFiClientSecure vc;
            vc.setInsecure();
            HTTPClient http;
            http.begin(vc, job.url);
            http.addHeader("Authorization", "Bearer " + self->_token);
            http.addHeader("Content-Type", "application/json");
            http.setTimeout(5000);
            int code = http.PATCH(job.body);
            http.end();
            if (code < 200 || code >= 300)
                Serial.printf("[PATCH] %d  %s\n", code, job.url);
        }
    }
}

/* static */
void DirigeraClient::_fetch_task_fn(void* arg) {
    DirigeraClient* self = (DirigeraClient*)arg;
    // First fetch immediately on startup
    self->_fetch_devices();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        self->_fetch_devices();
    }
}

void DirigeraClient::_ensure_tasks() {
    if (s_patch_q) return;   // already started

    s_patch_q = xQueueCreate(6, sizeof(PatchJob));

    // PATCH task: core 0, priority 2, 12 KB stack
    xTaskCreatePinnedToCore(_patch_worker,  "http_patch", 12288, this,
                            2, &s_patch_task, 0);
    // Fetch task: core 0, priority 1 (lower than PATCH), 14 KB stack
    xTaskCreatePinnedToCore(_fetch_task_fn, "http_fetch", 14336, this,
                            1, &s_fetch_task, 0);
}

// ── Async PATCH helper ───────────────────────────────────────────────────────

bool DirigeraClient::_patch(const String& device_id, const String& body) {
    if (!s_patch_q) return false;   // demo mode — no queue, skip silently
    PatchJob job;
    String url = "https://" + _hub_ip + ":8443/v1/devices/" + device_id;
    url.toCharArray (job.url,  sizeof(job.url));
    body.toCharArray(job.body, sizeof(job.body));
    return xQueueSend(s_patch_q, &job, 0) == pdTRUE;
}

// ── Entity lookup ────────────────────────────────────────────────────────────

// _find_entity_locked: call while _mutex is already held (internal use).
HAEntity* DirigeraClient::_find_entity_locked(const String& id) {
    for (auto& e : _entities) if (e.entity_id == id) return &e;
    return nullptr;
}

// find_entity: public, main-thread only.
// The background task holds _mutex only during the short merge phase (~1 ms
// every 5 s).  LVGL callbacks that call this are safe in practice.
HAEntity* DirigeraClient::find_entity(const String& id) {
    return _find_entity_locked(id);
}

// ── Public API ───────────────────────────────────────────────────────────────

void DirigeraClient::begin(const String& hub_ip, const String& token) {
    _hub_ip = hub_ip;
    _token  = token;
    _mutex  = xSemaphoreCreateMutex();
    _ensure_tasks();
    // Fetch starts immediately in _fetch_task_fn; _on_ready fires via loop()
}

// loop() — runs on main/Arduino thread (core 1).
// No HTTP here. Just checks flags set by background tasks and fires callbacks
// on the main thread so LVGL is never called from a background task.
void DirigeraClient::loop() {
    // 1. First fetch completed → build the home screen
    if (_ready_pending) {
        _ready_pending = false;
        if (_on_ready) _on_ready();
    }

    // 2. Subsequent fetches found changed entities → notify UI
    if (_upd_count > 0 && _mutex) {
        // Snapshot changed entities under mutex, release before calling LVGL
        HAEntity snap[MAX_UPD];
        int n = 0;

        if (xSemaphoreTake(_mutex, 0) == pdTRUE) {   // non-blocking; skip if busy
            n = min(_upd_count, MAX_UPD);
            for (int i = 0; i < n; i++) {
                HAEntity* e = _find_entity_locked(String(_upd_ids[i]));
                if (e) snap[i] = *e;
                else   snap[i].entity_id = "";   // mark invalid
            }
            _upd_count = 0;
            xSemaphoreGive(_mutex);
        }

        for (int i = 0; i < n; i++) {
            if (!snap[i].entity_id.isEmpty() && _on_update)
                _on_update(snap[i]);
        }
    }
}

// ── Optimistic state changes (main thread) ───────────────────────────────────
// These are called from LVGL event callbacks — NEVER block with portMAX_DELAY
// here or the watchdog fires.  Use a non-blocking mutex take (timeout = 0):
// if the fetch task happens to be in its 5 ms merge window we just skip the
// optimistic update; the PATCH still goes out and the fetch confirms in ≤ 5 s.

void DirigeraClient::_set_power(const String& id, bool on) {
    HAEntity snap;
    bool notify = false;

    if (_mutex && xSemaphoreTake(_mutex, 0) == pdTRUE) {
        HAEntity* e = _find_entity_locked(id);
        if (e) { e->state = on ? "on" : "off"; snap = *e; notify = true; }
        xSemaphoreGive(_mutex);
    }

    if (notify && _on_update) _on_update(snap);
    _patch(id, "[{\"attributes\":{\"isOn\":" + String(on ? "true" : "false") + "}}]");
}

void DirigeraClient::toggle(const String& id) {
    bool cur_on = false, found = false;
    if (_mutex && xSemaphoreTake(_mutex, 0) == pdTRUE) {
        HAEntity* e = _find_entity_locked(id);
        if (e) { cur_on = e->is_on(); found = true; }
        xSemaphoreGive(_mutex);
    }
    if (found) _set_power(id, !cur_on);
}

void DirigeraClient::turn_on (const String& id) { _set_power(id, true);  }
void DirigeraClient::turn_off(const String& id) { _set_power(id, false); }

void DirigeraClient::set_brightness(const String& id, uint8_t val255) {
    HAEntity snap;
    bool notify = false;

    if (_mutex && xSemaphoreTake(_mutex, 0) == pdTRUE) {
        HAEntity* e = _find_entity_locked(id);
        if (e) { e->brightness = val255; snap = *e; notify = true; }
        xSemaphoreGive(_mutex);
    }

    if (notify && _on_update) _on_update(snap);
    int pct = max(1, (int)(val255 * 100 / 255));
    _patch(id, "[{\"attributes\":{\"lightLevel\":" + String(pct) + "}}]");
}

void DirigeraClient::set_color(const String& id, uint8_t r, uint8_t g, uint8_t b) {
    float h, s;
    rgb_to_hs(r, g, b, h, s);

    HAEntity snap;
    bool notify = false;

    if (_mutex && xSemaphoreTake(_mutex, 0) == pdTRUE) {
        HAEntity* e = _find_entity_locked(id);
        if (e) { e->r = r; e->g = g; e->b = b; snap = *e; notify = true; }
        xSemaphoreGive(_mutex);
    }

    if (notify && _on_update) _on_update(snap);
    String body = "[{\"attributes\":{\"colorHue\":" + String(h, 2) +
                  ",\"colorSaturation\":" + String(s, 3) + "}}]";
    _patch(id, body);
}

// ── Demo (main thread only) ───────────────────────────────────────────────────

void DirigeraClient::load_demo() {
    // demo doesn't use the background task — runs fully on main thread.
    // _mutex may be null if begin() was never called (no DIRIGERA configured).
    if (!_mutex) _mutex = xSemaphoreCreateMutex();
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _areas.clear();
    _entities.clear();

    auto area = [&](const char* id, const char* name) {
        HAArea a; a.id = id; a.name = name; _areas.push_back(a);
    };
    auto light = [&](const char* id, const char* name, const char* room,
                     bool on, bool bri, bool col, uint8_t r=255, uint8_t g=200, uint8_t b=80) {
        HAEntity e;
        e.entity_id = id; e.friendly_name = name; e.area_id = room;
        e.type = EntityType::LIGHT; e.state = on ? "on" : "off";
        e.supports_brightness = bri; e.supports_color = col;
        e.brightness = on ? 180 : 80;
        e.r = r; e.g = g; e.b = b;
        _entities.push_back(e);
    };
    auto outlet = [&](const char* id, const char* name, const char* room, bool on) {
        HAEntity e;
        e.entity_id = id; e.friendly_name = name; e.area_id = room;
        e.type = EntityType::SWITCH; e.state = on ? "on" : "off";
        _entities.push_back(e);
    };

    area("r1", "Living Room");
    area("r2", "Bedroom");
    area("r3", "Kitchen");

    light("d1", "Ceiling",    "r1", true,  true,  true,  255, 200,  80);
    light("d2", "Floor Lamp", "r1", false, true,  false);
    outlet("d3","TV",         "r1", true);
    outlet("d4","Fan",        "r1", false);

    light("d5", "Bed Light",  "r2", true,  true,  true,  255, 120, 60);
    light("d6", "Desk Lamp",  "r2", false, true,  false);
    outlet("d7","Charger",    "r2", true);

    light("d8", "Counter",    "r3", true,  true,  false);
    outlet("d9","Coffee Mach","r3", false);

    xSemaphoreGive(_mutex);

    if (_on_ready) _on_ready();
}

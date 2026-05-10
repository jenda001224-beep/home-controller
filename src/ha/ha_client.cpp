#include "ha_client.h"
#include <Arduino.h>

// ============================================================
//  HAClient — Home Assistant WebSocket API
//  Protocol: https://developers.home-assistant.io/docs/api/websocket
// ============================================================

void HAClient::begin(const char* host, uint16_t port, const char* token) {
    _token = token;
    _state = State::DISCONNECTED;

    _ws.begin(host, port, "/api/websocket");
    _ws.onEvent([this](WStype_t t, uint8_t* p, size_t l) { _ws_event(t, p, l); });
    _ws.setReconnectInterval(5000);
}

void HAClient::loop() {
    _ws.loop();
}

// ---- Public control API ----

void HAClient::toggle(const String& eid) {
    String domain = eid.substring(0, eid.indexOf('.'));
    _call_service(domain, "toggle", eid);
    // Optimistic local update
    auto* e = find_entity(eid);
    if (e) {
        e->state = e->is_on() ? "off" : "on";
        if (_update_cb) _update_cb(*e);
    }
}

void HAClient::turn_on(const String& eid) {
    String domain = eid.substring(0, eid.indexOf('.'));
    _call_service(domain, "turn_on", eid);
    auto* e = find_entity(eid);
    if (e) { e->state = "on"; if (_update_cb) _update_cb(*e); }
}

void HAClient::turn_off(const String& eid) {
    String domain = eid.substring(0, eid.indexOf('.'));
    _call_service(domain, "turn_off", eid);
    auto* e = find_entity(eid);
    if (e) { e->state = "off"; if (_update_cb) _update_cb(*e); }
}

void HAClient::set_brightness(const String& eid, uint8_t brightness) {
    StaticJsonDocument<128> extra_doc;
    JsonObject extra = extra_doc.to<JsonObject>();
    extra["brightness"] = brightness;
    _call_service("light", "turn_on", eid, &extra);
    auto* e = find_entity(eid);
    if (e) { e->brightness = brightness; e->state = "on"; if (_update_cb) _update_cb(*e); }
}

void HAClient::set_color(const String& eid, uint8_t r, uint8_t g, uint8_t b) {
    StaticJsonDocument<128> extra_doc;
    JsonObject extra = extra_doc.to<JsonObject>();
    JsonArray rgb = extra.createNestedArray("rgb_color");
    rgb.add(r); rgb.add(g); rgb.add(b);
    _call_service("light", "turn_on", eid, &extra);
    auto* e = find_entity(eid);
    if (e) { e->r = r; e->g = g; e->b = b; e->state = "on"; if (_update_cb) _update_cb(*e); }
}

HAEntity* HAClient::find_entity(const String& eid) {
    for (auto& e : _entities)
        if (e.entity_id == eid) return &e;
    return nullptr;
}

// ---- Internal ----

void HAClient::_ws_event(WStype_t type, uint8_t* payload, size_t length) {
    if (type == WStype_TEXT) {
        _on_message((const char*)payload, length);
    } else if (type == WStype_DISCONNECTED) {
        _state = State::DISCONNECTED;
        Serial.println("[HA] Disconnected");
    } else if (type == WStype_CONNECTED) {
        Serial.println("[HA] WebSocket connected, waiting for auth_required");
        _state = State::AUTH_PENDING;
    }
}

void HAClient::_on_message(const char* json, size_t /*len*/) {
    DynamicJsonDocument doc(32768);
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("[HA] JSON error: %s\n", err.c_str());
        return;
    }

    const char* msg_type = doc["type"];
    if (!msg_type) return;

    // --- Auth handshake ---
    if (strcmp(msg_type, "auth_required") == 0) {
        StaticJsonDocument<256> auth;
        auth["type"]         = "auth";
        auth["access_token"] = _token;
        _send(auth);
        Serial.println("[HA] Auth sent");
        return;
    }

    if (strcmp(msg_type, "auth_ok") == 0) {
        Serial.println("[HA] Authenticated — loading areas");
        _state = State::LOAD_AREAS;
        _send_get_areas();
        return;
    }

    if (strcmp(msg_type, "auth_invalid") == 0) {
        Serial.println("[HA] Auth FAILED — check HA_TOKEN in config.h");
        return;
    }

    // --- Result messages ---
    if (strcmp(msg_type, "result") == 0) {
        int id = doc["id"];
        bool success = doc["success"];
        if (!success) {
            Serial.printf("[HA] Request %d failed\n", id);
            return;
        }

        if (id == _id_areas) {
            // Parse area list
            _areas.clear();
            for (JsonObject area : doc["result"].as<JsonArray>()) {
                HAArea a;
                a.id   = area["area_id"].as<String>();
                a.name = area["name"].as<String>();
                _areas.push_back(a);
            }
            Serial.printf("[HA] %d areas loaded\n", (int)_areas.size());
            _state = State::LOAD_ENTITIES;
            _send_get_entity_registry();

        } else if (id == _id_entities) {
            // Map entity → area
            for (JsonObject entry : doc["result"].as<JsonArray>()) {
                const char* eid  = entry["entity_id"];
                if (!eid) continue;
                EntityType t = domain_to_type(String(eid));
                if (t == EntityType::UNKNOWN) continue;

                HAEntity e;
                e.entity_id    = eid;
                e.friendly_name = entry["name"] | entry["original_name"] | eid;
                e.area_id      = entry["area_id"] | "";
                e.type         = t;
                _entities.push_back(e);
            }
            Serial.printf("[HA] %d entities loaded\n", (int)_entities.size());
            _state = State::LOAD_STATES;
            _send_get_states();

        } else if (id == _id_states) {
            for (JsonObject state_obj : doc["result"].as<JsonArray>()) {
                _apply_state(state_obj);
            }
            Serial.println("[HA] States applied — subscribing");
            _state = State::SUBSCRIBING;
            _send_subscribe();

        } else if (id == _id_sub) {
            Serial.println("[HA] Subscribed to state changes — READY");
            _state = State::READY;
            if (_ready_cb) _ready_cb();
        }
        return;
    }

    // --- Live state_changed events ---
    if (strcmp(msg_type, "event") == 0) {
        JsonObject event = doc["event"];
        const char* event_type = event["event_type"];
        if (event_type && strcmp(event_type, "state_changed") == 0) {
            _apply_state_changed(event);
        }
    }
}

void HAClient::_send(JsonDocument& doc) {
    String out;
    serializeJson(doc, out);
    _ws.sendTXT(out);
}

void HAClient::_send_get_areas() {
    _id_areas = _msg_id++;
    StaticJsonDocument<64> doc;
    doc["id"]   = _id_areas;
    doc["type"] = "config/area_registry/list";
    _send(doc);
}

void HAClient::_send_get_entity_registry() {
    _id_entities = _msg_id++;
    StaticJsonDocument<64> doc;
    doc["id"]   = _id_entities;
    doc["type"] = "config/entity_registry/list";
    _send(doc);
}

void HAClient::_send_get_states() {
    _id_states = _msg_id++;
    StaticJsonDocument<64> doc;
    doc["id"]   = _id_states;
    doc["type"] = "get_states";
    _send(doc);
}

void HAClient::_send_subscribe() {
    _id_sub = _msg_id++;
    StaticJsonDocument<64> doc;
    doc["id"]         = _id_sub;
    doc["type"]       = "subscribe_events";
    doc["event_type"] = "state_changed";
    _send(doc);
}

void HAClient::_call_service(const String& domain, const String& service,
                              const String& eid, JsonObject* extra) {
    DynamicJsonDocument doc(512);
    doc["id"]      = _msg_id++;
    doc["type"]    = "call_service";
    doc["domain"]  = domain;
    doc["service"] = service;
    JsonObject data = doc.createNestedObject("service_data");
    data["entity_id"] = eid;
    if (extra) {
        for (auto kv : *extra) data[kv.key()] = kv.value();
    }
    _send(doc);
}

void HAClient::_apply_state(JsonObject& obj) {
    const char* eid = obj["entity_id"];
    if (!eid) return;
    auto* e = find_entity(String(eid));
    if (!e) return;

    e->state = obj["state"].as<String>();
    JsonObject attr = obj["attributes"];
    if (attr) {
        if (!attr["friendly_name"].isNull())
            e->friendly_name = attr["friendly_name"].as<String>();
        if (!attr["brightness"].isNull())
            e->brightness = attr["brightness"].as<int>();
        if (!attr["rgb_color"].isNull()) {
            JsonArray rgb = attr["rgb_color"].as<JsonArray>();
            if (rgb.size() == 3) {
                e->r = rgb[0]; e->g = rgb[1]; e->b = rgb[2];
            }
        }
        // Detect capabilities from supported_color_modes
        if (!attr["supported_color_modes"].isNull()) {
            e->supports_brightness = true;
            for (JsonVariant m : attr["supported_color_modes"].as<JsonArray>()) {
                String mode = m.as<String>();
                if (mode == "rgb" || mode == "hs" || mode == "xy" || mode == "rgbw")
                    e->supports_color = true;
            }
        }
    }
}

void HAClient::_apply_state_changed(JsonObject& event) {
    JsonObject new_state = event["data"]["new_state"];
    if (new_state.isNull()) return;
    _apply_state(new_state);

    const char* eid = new_state["entity_id"];
    if (eid && _update_cb) {
        auto* e = find_entity(String(eid));
        if (e) _update_cb(*e);
    }
}

#pragma once
#include <Arduino.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <vector>
#include "entity.h"

// Called whenever entity state changes — update the UI
using EntityUpdateCb = std::function<void(const HAEntity&)>;
// Called when initial load is complete
using ReadyCb        = std::function<void()>;

class HAClient {
public:
    void begin(const char* host, uint16_t port, const char* token);
    void loop();

    // Call services
    void toggle(const String& entity_id);
    void turn_on(const String& entity_id);
    void turn_off(const String& entity_id);
    void set_brightness(const String& entity_id, uint8_t brightness);
    void set_color(const String& entity_id, uint8_t r, uint8_t g, uint8_t b);

    // Data access
    const std::vector<HAArea>&   areas()    const { return _areas; }
    const std::vector<HAEntity>& entities() const { return _entities; }
    HAEntity* find_entity(const String& entity_id);

    void on_update(EntityUpdateCb cb) { _update_cb = cb; }
    void on_ready (ReadyCb cb)        { _ready_cb  = cb; }

    bool is_ready() const { return _state == State::READY; }

private:
    enum class State {
        DISCONNECTED, AUTH_PENDING,
        LOAD_AREAS, LOAD_ENTITIES, LOAD_STATES, SUBSCRIBING, READY
    };

    WebSocketsClient _ws;
    const char*      _token = nullptr;
    State            _state = State::DISCONNECTED;
    int              _msg_id = 1;

    std::vector<HAArea>   _areas;
    std::vector<HAEntity> _entities;

    EntityUpdateCb _update_cb;
    ReadyCb        _ready_cb;

    // Maps msg id → what we're waiting for
    int _id_areas = 0, _id_entities = 0, _id_states = 0, _id_sub = 0;

    void _ws_event(WStype_t type, uint8_t* payload, size_t length);
    void _on_message(const char* json, size_t len);
    void _send(JsonDocument& doc);
    void _send_get_areas();
    void _send_get_entity_registry();
    void _send_get_states();
    void _send_subscribe();
    void _call_service(const String& domain, const String& service,
                       const String& entity_id, JsonObject* extra = nullptr);
    void _apply_state(JsonObject& state_obj);
    void _apply_state_changed(JsonObject& event);
};

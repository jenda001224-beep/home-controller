#pragma once
#include <Arduino.h>
#include <vector>

enum class EntityType { LIGHT, SWITCH, FAN, UNKNOWN };

struct HAEntity {
    String entity_id;
    String friendly_name;
    String state;
    String area_id;
    EntityType type = EntityType::UNKNOWN;

    uint8_t brightness = 255;
    uint8_t r = 255, g = 255, b = 255;
    bool supports_color      = false;
    bool supports_brightness = false;

    bool is_on() const { return state == "on"; }
};

struct HAArea {
    String id;
    String name;
};

inline EntityType domain_to_type(const String& entity_id) {
    if (entity_id.startsWith("light."))  return EntityType::LIGHT;
    if (entity_id.startsWith("switch.")) return EntityType::SWITCH;
    if (entity_id.startsWith("fan."))    return EntityType::FAN;
    return EntityType::UNKNOWN;
}

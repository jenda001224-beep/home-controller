#pragma once
#include <Arduino.h>
#include <vector>

enum class EntityType { LIGHT, SWITCH, FAN, UNKNOWN };

// Icon codepoints from LVGL built-in symbol font
static const char* icon_for_type(EntityType t, bool on) {
    switch (t) {
        case EntityType::LIGHT:  return on ? LV_SYMBOL_IMAGE  : LV_SYMBOL_TINT;
        case EntityType::SWITCH: return LV_SYMBOL_POWER;
        case EntityType::FAN:    return LV_SYMBOL_REFRESH;
        default:                 return LV_SYMBOL_SETTINGS;
    }
}

struct HAEntity {
    String entity_id;
    String friendly_name;
    String state;          // "on" / "off" / etc.
    String area_id;
    EntityType type = EntityType::UNKNOWN;

    // Light extras
    uint8_t  brightness = 255;       // 0–255
    uint8_t  r = 255, g = 255, b = 255;
    bool     supports_color      = false;
    bool     supports_brightness = false;

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

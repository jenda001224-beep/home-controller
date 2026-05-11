#pragma once
#include <Arduino.h>
#include <vector>

enum class LightType { SWITCH, DIMMER, COLOR };

struct LightCfg {
    String id;     // "l0", "l1", ...
    String name;   // display name
    String room;   // room / area name
    LightType type = LightType::DIMMER;
};

bool lc_load(std::vector<LightCfg>& out);
void lc_save(const std::vector<LightCfg>& lights);
bool lc_has_config();
void lc_clear();

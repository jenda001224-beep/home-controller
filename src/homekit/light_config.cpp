#include "light_config.h"
#include <ArduinoJson.h>
#include <Preferences.h>

static const char* NVS_NS  = "hc_lc";
static const char* NVS_KEY = "cfg";

bool lc_load(std::vector<LightCfg>& out) {
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    String json = prefs.getString(NVS_KEY, "");
    prefs.end();

    if (json.isEmpty()) return false;

    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, json) != DeserializationError::Ok) return false;

    JsonArray arr = doc.as<JsonArray>();
    for (JsonObject obj : arr) {
        LightCfg cfg;
        cfg.id   = obj["id"].as<String>();
        cfg.name = obj["name"].as<String>();
        cfg.room = obj["room"].as<String>();
        String t = obj["type"].as<String>();
        if (t == "switch") cfg.type = LightType::SWITCH;
        else if (t == "color") cfg.type = LightType::COLOR;
        else cfg.type = LightType::DIMMER;
        out.push_back(cfg);
    }
    return !out.empty();
}

void lc_save(const std::vector<LightCfg>& lights) {
    DynamicJsonDocument doc(4096);
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& l : lights) {
        JsonObject obj = arr.createNestedObject();
        obj["id"]   = l.id;
        obj["name"] = l.name;
        obj["room"] = l.room;
        switch (l.type) {
            case LightType::SWITCH: obj["type"] = "switch"; break;
            case LightType::COLOR:  obj["type"] = "color";  break;
            default:                obj["type"] = "dimmer"; break;
        }
    }
    String json;
    serializeJson(doc, json);

    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.putString(NVS_KEY, json);
    prefs.end();
}

bool lc_has_config() {
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    String v = prefs.getString(NVS_KEY, "");
    prefs.end();
    return v.length() > 2;
}

void lc_clear() {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.clear();
    prefs.end();
}

#include "hk_client.h"
#include <math.h>

// ── Colour helpers ──────────────────────────────────────────────────────────

static void rgb_to_hs(uint8_t r, uint8_t g, uint8_t b, float& h, float& s) {
    float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
    float mx = max({rf, gf, bf}), mn = min({rf, gf, bf});
    float d = mx - mn;
    s = (mx == 0.0f) ? 0.0f : (d / mx) * 100.0f;
    if (d == 0.0f) { h = 0.0f; return; }
    if (mx == rf)       h = 60.0f * fmodf((gf - bf) / d, 6.0f);
    else if (mx == gf)  h = 60.0f * ((bf - rf) / d + 2.0f);
    else                h = 60.0f * ((rf - gf) / d + 4.0f);
    if (h < 0.0f) h += 360.0f;
}

static void hs_to_rgb(float h, float s, uint8_t& r, uint8_t& g, uint8_t& b) {
    float sf = s / 100.0f;
    float c = sf;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = 1.0f - c;
    float rf, gf, bf;
    int seg = (int)(h / 60.0f) % 6;
    switch (seg) {
        case 0: rf=c; gf=x; bf=0; break;
        case 1: rf=x; gf=c; bf=0; break;
        case 2: rf=0; gf=c; bf=x; break;
        case 3: rf=0; gf=x; bf=c; break;
        case 4: rf=x; gf=0; bf=c; break;
        default:rf=c; gf=0; bf=x; break;
    }
    r = (uint8_t)((rf + m) * 255.0f);
    g = (uint8_t)((gf + m) * 255.0f);
    b = (uint8_t)((bf + m) * 255.0f);
}

// ── HKClient ────────────────────────────────────────────────────────────────

void HKClient::init(const std::vector<LightCfg>& cfgs) {
    _cfgs = cfgs;

    // Build unique room areas
    std::vector<String> seen;
    for (const auto& c : cfgs) {
        bool found = false;
        for (const auto& s : seen) if (s == c.room) { found = true; break; }
        if (!found) {
            seen.push_back(c.room);
            HAArea a;
            a.id = c.room;
            a.name = c.room;
            _areas.push_back(a);
        }
    }

    // Build entities
    for (const auto& c : cfgs) {
        HAEntity e;
        e.entity_id          = c.id;
        e.friendly_name      = c.name;
        e.area_id            = c.room;
        e.type               = EntityType::LIGHT;
        e.state              = "off";
        e.brightness         = 255;
        e.r = 255; e.g = 255; e.b = 255;
        e.supports_brightness = (c.type == LightType::DIMMER || c.type == LightType::COLOR);
        e.supports_color      = (c.type == LightType::COLOR);
        _entities.push_back(e);
    }
}

void HKClient::register_dev(DEV_Light* dev) {
    _devs.push_back(dev);
}

void HKClient::fire_ready() {
    if (_on_ready) _on_ready();
}

HAEntity* HKClient::find_entity(const String& id) {
    for (auto& e : _entities) if (e.entity_id == id) return &e;
    return nullptr;
}

DEV_Light* HKClient::_find_dev(const String& id) {
    for (auto* d : _devs) if (d->light_id == id) return d;
    return nullptr;
}

void HKClient::_sync_entity_from_dev(HAEntity& e, DEV_Light* dev) {
    e.state = dev->power->getVal<bool>() ? "on" : "off";
    if (dev->level)
        e.brightness = (uint8_t)(dev->level->getVal<int>() * 255 / 100);
    if (dev->hue && dev->sat) {
        hs_to_rgb(dev->hue->getVal<float>(),
                  dev->sat->getVal<float>(),
                  e.r, e.g, e.b);
    }
}

void HKClient::_hk_update(const String& light_id) {
    HAEntity*  e   = find_entity(light_id);
    DEV_Light* dev = _find_dev(light_id);
    if (!e || !dev) return;
    _sync_entity_from_dev(*e, dev);
    if (_on_update) _on_update(*e);
}

void HKClient::_set_power(const String& id, bool on) {
    DEV_Light* dev = _find_dev(id);
    HAEntity*  e   = find_entity(id);
    if (!dev || !e) return;
    dev->power->setVal(on);
    e->state = on ? "on" : "off";
    if (_on_update) _on_update(*e);
}

void HKClient::toggle(const String& id) {
    DEV_Light* dev = _find_dev(id);
    if (!dev) return;
    _set_power(id, !dev->power->getVal<bool>());
}
void HKClient::turn_on (const String& id) { _set_power(id, true);  }
void HKClient::turn_off(const String& id) { _set_power(id, false); }

void HKClient::set_brightness(const String& id, uint8_t val255) {
    DEV_Light* dev = _find_dev(id);
    HAEntity*  e   = find_entity(id);
    if (!dev || !dev->level || !e) return;
    int pct = max(1, (int)(val255 * 100 / 255));
    dev->level->setVal(pct);
    e->brightness = val255;
    if (_on_update) _on_update(*e);
}

void HKClient::set_color(const String& id, uint8_t r, uint8_t g, uint8_t b) {
    DEV_Light* dev = _find_dev(id);
    HAEntity*  e   = find_entity(id);
    if (!dev || !dev->hue || !e) return;
    float h, s;
    rgb_to_hs(r, g, b, h, s);
    dev->hue->setVal(h);
    dev->sat->setVal(s);
    e->r = r; e->g = g; e->b = b;
    if (_on_update) _on_update(*e);
}

// DEV_Light::update() needs full HKClient definition, so it lives here
boolean DEV_Light::update() {
    client->_hk_update(light_id);
    return true;
}

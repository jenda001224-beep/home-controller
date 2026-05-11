#pragma once
#include <functional>
#include <vector>
#include <Arduino.h>
#include "light_config.h"
#include "hk_accessory.h"
#include "../ha/entity.h"

// Callbacks match the existing HAClient interface
using EntityUpdateCb = std::function<void(const HAEntity&)>;
using ReadyCb        = std::function<void()>;

// ============================================================
//  HKClient — drop-in replacement for HAClient.
//  Wraps HomeSpan accessories and exposes the same API so
//  the existing UI code works unchanged.
// ============================================================
class HKClient {
public:
    // Call before homeSpan.begin() to prepare entity/area lists
    void init(const std::vector<LightCfg>& cfgs);
    // Call once per DEV_Light after creating it with new
    void register_dev(DEV_Light* dev);
    // Call after homeSpan accessories are set up to trigger on_ready
    void fire_ready();

    // HAClient-compatible interface ---------------------------
    const std::vector<HAArea>&   areas()    const { return _areas; }
    const std::vector<HAEntity>& entities() const { return _entities; }
    HAEntity* find_entity(const String& id);

    void toggle(const String& id);
    void turn_on(const String& id);
    void turn_off(const String& id);
    void set_brightness(const String& id, uint8_t val255);  // 0-255 scale
    void set_color(const String& id, uint8_t r, uint8_t g, uint8_t b);

    void on_update(EntityUpdateCb cb) { _on_update = cb; }
    void on_ready(ReadyCb cb)         { _on_ready  = cb; }

    // Called by DEV_Light::update() (during homeSpan.poll())
    void _hk_update(const String& light_id);

private:
    std::vector<LightCfg>   _cfgs;
    std::vector<HAArea>     _areas;
    std::vector<HAEntity>   _entities;
    std::vector<DEV_Light*> _devs;

    EntityUpdateCb _on_update;
    ReadyCb        _on_ready;

    DEV_Light* _find_dev(const String& id);
    void       _set_power(const String& id, bool on);
    void       _sync_entity_from_dev(HAEntity& e, DEV_Light* dev);
};

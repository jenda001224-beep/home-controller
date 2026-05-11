#pragma once
#include <HomeSpan.h>
#include "light_config.h"

// Forward declaration — avoids circular include with hk_client.h
class HKClient;

// ============================================================
//  HomeSpan LightBulb service for each configured light.
//  update() is called by HomeSpan (within homeSpan.poll())
//  when Apple Home changes a characteristic.
// ============================================================
struct DEV_Light : Service::LightBulb {
    SpanCharacteristic* power;
    SpanCharacteristic* level    = nullptr;  // brightness 1-100
    SpanCharacteristic* hue      = nullptr;  // 0-360
    SpanCharacteristic* sat      = nullptr;  // 0-100

    String     light_id;
    HKClient*  client;

    DEV_Light(const LightCfg& cfg, HKClient* c) : Service::LightBulb() {
        light_id = cfg.id;
        client   = c;

        power = new Characteristic::On(false);

        if (cfg.type == LightType::DIMMER || cfg.type == LightType::COLOR) {
            level = new Characteristic::Brightness(100);
            level->setRange(1, 100, 1);
        }
        if (cfg.type == LightType::COLOR) {
            hue = new Characteristic::Hue(0.0);
            sat = new Characteristic::Saturation(0.0);
        }
    }

    // Implemented in hk_client.cpp (needs full HKClient definition)
    boolean update() override;
};

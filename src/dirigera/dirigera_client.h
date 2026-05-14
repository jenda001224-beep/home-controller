#pragma once
#include <Arduino.h>
#include <vector>
#include <functional>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "../ha/entity.h"

using EntityUpdateCb = std::function<void(const HAEntity&)>;
using ReadyCb        = std::function<void()>;

// ============================================================
//  DirigeraClient — talks to IKEA DIRIGERA hub REST API
//  https://[hub_ip]:8443/v1   (self-signed TLS, skip verify)
// ============================================================
class DirigeraClient {
public:
    void begin(const String& hub_ip, const String& token);
    void loop();   // call every ~5 s to poll state

    // HAClient-compatible interface
    const std::vector<HAArea>&   areas()    const { return _areas; }
    const std::vector<HAEntity>& entities() const { return _entities; }
    HAEntity* find_entity(const String& id);

    void toggle(const String& id);
    void turn_on(const String& id);
    void turn_off(const String& id);
    void set_brightness(const String& id, uint8_t val255);   // 0-255
    void set_color(const String& id, uint8_t r, uint8_t g, uint8_t b);

    void on_update(EntityUpdateCb cb) { _on_update = cb; }
    void on_ready(ReadyCb cb)         { _on_ready  = cb; }

    // Returns true on success; sets out_token
    static bool pair(const String& hub_ip, String& out_token);

    // Populate with fake data for demo/testing (fires on_ready)
    void load_demo();

private:
    String _hub_ip;
    String _token;
    uint32_t _last_poll = 0;

    std::vector<HAArea>   _areas;
    std::vector<HAEntity> _entities;

    EntityUpdateCb _on_update;
    ReadyCb        _on_ready;

    // Async PATCH via background FreeRTOS task —
    // PATCH calls are non-blocking; main loop / LVGL never stalls waiting for HTTP.
    struct PatchJob {
        char url[200];   // "https://x.x.x.x:8443/v1/devices/<uuid>" ≈ 80 chars
        char body[80];   // largest body: colorHue+colorSaturation ≈ 65 chars
    };
    static QueueHandle_t  s_patch_q;
    static TaskHandle_t   s_patch_task;
    static void _patch_worker(void* arg);
    void _ensure_patch_task();

    bool _patch(const String& device_id, const String& json_body);  // now async
    void _fetch_devices();   // synchronous GET (polling, every 5 s)
    void _set_power(const String& id, bool on);
};

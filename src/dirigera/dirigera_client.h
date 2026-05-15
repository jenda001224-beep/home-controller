#pragma once
#include <Arduino.h>
#include <vector>
#include <functional>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "../ha/entity.h"

using EntityUpdateCb = std::function<void(const HAEntity&)>;
using ReadyCb        = std::function<void()>;

// ============================================================
//  DirigeraClient — talks to IKEA DIRIGERA hub REST API
//  https://[hub_ip]:8443/v1   (self-signed TLS, skip verify)
//
//  v5.33: _fetch_devices() moved to a background FreeRTOS task
//  (core 0) so lv_timer_handler() never stalls waiting for HTTP.
//  dc.loop() only dispatches callbacks on the main thread — no I/O.
// ============================================================
class DirigeraClient {
public:
    void begin(const String& hub_ip, const String& token);
    void loop();   // call from Arduino loop() — dispatches callbacks, zero blocking

    // HAClient-compatible interface
    const std::vector<HAArea>&   areas()    const { return _areas; }
    const std::vector<HAEntity>& entities() const { return _entities; }
    // Main-thread only — do not cache the pointer across yield points
    HAEntity* find_entity(const String& id);

    void toggle(const String& id);
    void turn_on(const String& id);
    void turn_off(const String& id);
    void set_brightness(const String& id, uint8_t val255);   // 0-255
    void set_color(const String& id, uint8_t r, uint8_t g, uint8_t b);

    void on_update(EntityUpdateCb cb) { _on_update = cb; }
    void on_ready(ReadyCb cb)         { _on_ready  = cb; }

    // Debug accessors (main-thread only)
    int    dbg_raw_count() const { return _dbg_raw_count; }
    String dbg_types()     const { return _dbg_types;     }

    // Returns true on success; sets out_token
    static bool pair(const String& hub_ip, String& out_token);

    // Populate with fake data for demo/testing (fires on_ready)
    void load_demo();

private:
    String _hub_ip;
    String _token;

    std::vector<HAArea>   _areas;
    std::vector<HAEntity> _entities;

    EntityUpdateCb _on_update;
    ReadyCb        _on_ready;

    // ── Thread safety ────────────────────────────────────────────
    // _mutex guards _entities, _areas, and the update-ID ring below.
    // Take with xSemaphoreTake before touching those fields.
    SemaphoreHandle_t _mutex = nullptr;

    // Background worker signals main thread via these flags/buffers —
    // written by fetch task (core 0), read+cleared by loop() (core 1).
    volatile bool _ready_pending  = false;
    volatile bool _fetched_once   = false;  // set on first fetch; prevents repeated on_ready

    // Debug: populated after each fetch, readable from main thread via accessors
    int    _dbg_raw_count = 0;   // total devices in DIRIGERA response (before type filter)
    String _dbg_types;           // comma-separated list of all device types seen

    static const int MAX_UPD = 8;
    char _upd_ids[MAX_UPD][64];   // entity_ids that changed in last fetch
    int  _upd_count = 0;          // how many valid entries; guarded by _mutex

    // ── Async PATCH (background FreeRTOS task) ────────────────────
    struct PatchJob {
        char url[200];   // "https://x.x.x.x:8443/v1/devices/<uuid>"
        char body[80];   // largest body: colorHue+colorSaturation ≈ 65 chars
    };
    static QueueHandle_t  s_patch_q;
    static TaskHandle_t   s_patch_task;
    static void _patch_worker(void* arg);

    // ── Periodic fetch (background FreeRTOS task) ─────────────────
    static TaskHandle_t   s_fetch_task;
    static void _fetch_task_fn(void* arg);

    // ── Internal helpers ──────────────────────────────────────────
    void _ensure_tasks();
    bool _patch(const String& device_id, const String& json_body);  // async
    void _fetch_devices();      // runs on fetch task; updates _entities under mutex
    void _set_power(const String& id, bool on);
    HAEntity* _find_entity_locked(const String& id);  // call while _mutex is held
};

#pragma once
#include <lvgl.h>
#include <vector>
#include "../ha/entity.h"
#include "../ha/ha_client.h"

class UI {
public:
    void begin(HAClient* ha);

    // Call after HA finishes loading
    void build_home();

    // Called by HAClient callback on every state change
    void on_entity_update(const HAEntity& e);

    // Splash / loading
    void show_splash();
    void set_status(const char* msg);

private:
    HAClient* _ha = nullptr;

    // Screens
    lv_obj_t* _scr_splash = nullptr;
    lv_obj_t* _scr_home   = nullptr;
    lv_obj_t* _scr_detail = nullptr;

    lv_obj_t* _splash_status = nullptr;

    // Home screen widgets
    lv_obj_t* _tabview   = nullptr;
    // Per-area tile grids keyed by area index
    std::vector<lv_obj_t*> _grids;

    // Detail panel
    lv_obj_t* _detail_panel   = nullptr;
    lv_obj_t* _detail_title   = nullptr;
    lv_obj_t* _detail_switch  = nullptr;
    lv_obj_t* _detail_brightness = nullptr;
    lv_obj_t* _detail_colorwheel = nullptr;
    lv_obj_t* _detail_close   = nullptr;

    String _detail_entity_id;

    // Map entity_id → tile object so we can update it live
    struct TileRef { String entity_id; lv_obj_t* tile; lv_obj_t* icon; lv_obj_t* name_lbl; lv_obj_t* state_lbl; };
    std::vector<TileRef> _tiles;

    void _build_tabs();
    void _build_tiles_for_area(lv_obj_t* grid, const String& area_id, const String& area_name);
    void _add_tile(lv_obj_t* grid, const HAEntity& entity);
    void _update_tile(TileRef& ref, const HAEntity& e);

    void _show_detail(const String& entity_id);
    void _close_detail();
    void _detail_update(const HAEntity& e);

    static void _tile_clicked(lv_event_t* ev);
    static void _tile_long_press(lv_event_t* ev);
    static void _detail_switch_changed(lv_event_t* ev);
    static void _brightness_changed(lv_event_t* ev);
    static void _color_changed(lv_event_t* ev);
    static void _close_detail_cb(lv_event_t* ev);
};

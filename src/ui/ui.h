#pragma once
#include <lvgl.h>
#include <vector>
#include "../ha/entity.h"
#include "../dirigera/dirigera_client.h"

class UI {
public:
    void begin(DirigeraClient* dc);
    void build_home();
    void on_entity_update(const HAEntity& e);
    void show_splash();
    void set_status(const char* msg);
    void go_home();
    void set_battery(int pct);   // 0-100, call from main loop

private:
    DirigeraClient* _dc = nullptr;

    lv_obj_t* _scr_splash  = nullptr;
    lv_obj_t* _scr_home    = nullptr;
    lv_obj_t* _splash_status = nullptr;
    lv_obj_t* _tabview     = nullptr;
    lv_obj_t* _bat_label   = nullptr;   // header battery label
    std::vector<lv_obj_t*> _grids;

    // Detail panel
    lv_obj_t* _detail_panel      = nullptr;
    lv_obj_t* _detail_title      = nullptr;
    lv_obj_t* _detail_switch     = nullptr;
    lv_obj_t* _detail_brightness = nullptr;
    lv_obj_t* _detail_colorwheel = nullptr;

    String _detail_entity_id;

    struct TileRef {
        String    entity_id;
        lv_obj_t* tile;
        lv_obj_t* icon;
        lv_obj_t* name_lbl;
        lv_obj_t* state_lbl;
    };
    std::vector<TileRef> _tiles;

    void _build_tabs();
    void _add_tile(lv_obj_t* grid, const HAEntity& entity);
    void _update_tile(TileRef& ref, const HAEntity& e);

    void _show_detail(const String& entity_id);
    void _close_detail();
    void _detail_update(const HAEntity& e);

    static void _tile_clicked(lv_event_t* ev);
    static void _detail_switch_changed(lv_event_t* ev);
    static void _brightness_changed(lv_event_t* ev);
    static void _color_changed(lv_event_t* ev);
    static void _close_detail_cb(lv_event_t* ev);
};

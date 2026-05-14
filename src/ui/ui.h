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

    void set_battery(int pct, bool charging = false);
    void set_grid_cols(uint8_t cols);
    uint8_t grid_cols() const { return _grid_cols; }

private:
    DirigeraClient* _dc        = nullptr;
    uint8_t         _grid_cols = 1;   // 1=list, 2=grid, 3=compact grid

    lv_obj_t* _scr_splash    = nullptr;
    lv_obj_t* _scr_home      = nullptr;
    lv_obj_t* _splash_status = nullptr;
    lv_obj_t* _tabview       = nullptr;
    lv_obj_t* _bat_label     = nullptr;
    std::vector<lv_obj_t*> _grids;

    // Detail panel
    lv_obj_t* _detail_panel      = nullptr;
    lv_obj_t* _detail_title      = nullptr;
    lv_obj_t* _detail_switch     = nullptr;
    lv_obj_t* _detail_bri_view   = nullptr;
    lv_obj_t* _detail_col_view   = nullptr;
    lv_obj_t* _detail_bri_pill   = nullptr;   // the pill container
    lv_obj_t* _detail_bri_fill   = nullptr;   // fill rectangle inside pill
    lv_obj_t* _detail_colorwheel = nullptr;
    String    _detail_entity_id;
    int       _detail_pill_h     = 0;         // cached pill height for drag calc
    uint32_t  _bri_last_send_ms  = 0;         // throttle brightness HTTP sends

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

    static void _tile_clicked(lv_event_t* ev);       // tap → toggle on/off
    static void _tile_long_pressed(lv_event_t* ev);  // hold → open detail
    static void _detail_switch_changed(lv_event_t* ev);
    static void _bri_slider_cb(lv_event_t* ev);  // native lv_slider VALUE_CHANGED
    static void _color_changed(lv_event_t* ev);
    static void _close_detail_cb(lv_event_t* ev);
    static void _go_color_cb(lv_event_t* ev);
    static void _go_bri_cb(lv_event_t* ev);
};

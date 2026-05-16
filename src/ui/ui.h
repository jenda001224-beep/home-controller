#pragma once
#include <lvgl.h>
#include <vector>
#include <map>
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

    void set_battery(int pct, bool charging, float v = -1.0f);
    void set_grid_cols(uint8_t cols);
    uint8_t grid_cols() const { return _grid_cols; }

    // Device visibility filter — comma-separated entity IDs to hide
    void set_hidden_ids(const String& ids);
    bool is_hidden(const String& id) const;

    // Device ordering and custom names (local only, not sent to DIRIGERA)
    void set_entity_order(const std::vector<String>& order);
    void set_custom_names(const std::map<String,String>& names);
    String get_display_name(const String& id, const String& fallback) const;

    // Physical button control: adjusts the active slider in the detail panel.
    // dir=+1 → increase brightness / move hue knob up; dir=-1 → decrease.
    void btn_slider_step(int dir);

private:
    DirigeraClient* _dc        = nullptr;
    uint8_t         _grid_cols = 1;   // 1=list, 2=grid, 3=compact grid
    std::vector<String> _hidden_ids;   // entity IDs to skip when building tiles
    std::vector<String> _entity_order; // user-defined display order (entity IDs)
    std::map<String,String> _custom_names; // entity_id → custom display name

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
    uint32_t  _col_last_send_ms  = 0;         // throttle colour HTTP sends
    bool      _detail_updating   = false;     // guard: programmatic slider/wheel update in progress

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

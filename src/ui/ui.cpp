#include "ui.h"
#include "theme.h"
#include "config.h"
#include <Arduino.h>

static const char* entity_icon(const HAEntity& e) {
    if (e.type == EntityType::LIGHT)  return e.is_on() ? LV_SYMBOL_IMAGE : LV_SYMBOL_TINT;
    if (e.type == EntityType::SWITCH) return LV_SYMBOL_POWER;
    return LV_SYMBOL_SETTINGS;
}

static const char* bat_icon(int pct) {
    if (pct > 80) return LV_SYMBOL_BATTERY_FULL;
    if (pct > 55) return LV_SYMBOL_BATTERY_3;
    if (pct > 30) return LV_SYMBOL_BATTERY_2;
    if (pct > 10) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

// -- begin / splash --

void UI::begin(DirigeraClient* dc) {
    _dc = dc;
    show_splash();
}

void UI::show_splash() {
    _scr_splash = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(_scr_splash, C_BG, 0);
    lv_obj_set_style_bg_opa(_scr_splash, LV_OPA_COVER, 0);
    lv_scr_load(_scr_splash);

    // Large home icon — orange, above the name
    lv_obj_t* icon = lv_label_create(_scr_splash);
    lv_label_set_text(icon, LV_SYMBOL_HOME);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(icon, C_ACCENT, 0);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(icon, TFT_WIDTH - 40);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -70);

    // App name
    lv_obj_t* title = lv_label_create(_scr_splash);
    lv_label_set_text(title, APP_NAME);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, C_TEXT2, 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(title, TFT_WIDTH - 40);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -30);

    // Status line
    _splash_status = lv_label_create(_scr_splash);
    lv_label_set_text(_splash_status, "Starting...");
    lv_obj_set_style_text_color(_splash_status, C_TEXT2, 0);
    lv_obj_set_style_text_font(_splash_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(_splash_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(_splash_status, TFT_WIDTH - 40);
    lv_obj_align(_splash_status, LV_ALIGN_CENTER, 0, 20);
    lv_label_set_long_mode(_splash_status, LV_LABEL_LONG_WRAP);

    // Spinner — orange on gray
    lv_obj_t* spin = lv_spinner_create(_scr_splash, 1000, 60);
    lv_obj_set_size(spin, 36, 36);
    lv_obj_align(spin, LV_ALIGN_CENTER, 0, 90);
    lv_obj_set_style_arc_color(spin, C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spin, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spin, C_BG3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spin, 3, LV_PART_MAIN);
}

void UI::set_status(const char* msg) {
    if (_splash_status) lv_label_set_text(_splash_status, msg);
}

// -- Home screen --

void UI::build_home() {
    if (_scr_home) {
        lv_obj_del(_scr_home);
        _scr_home  = nullptr;
        _tabview   = nullptr;
        _bat_label = nullptr;
        _detail_panel      = nullptr;
        _detail_brightness = nullptr;
        _detail_colorwheel = nullptr;
        _detail_entity_id  = "";
        _tiles.clear();
        _grids.clear();
    }
    _scr_home = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(_scr_home, C_BG, 0);
    lv_obj_set_style_bg_opa(_scr_home, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(_scr_home, 0, 0);
    lv_obj_clear_flag(_scr_home, LV_OBJ_FLAG_SCROLLABLE);
    _build_tabs();
    lv_scr_load_anim(_scr_home, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, false);
}

void UI::go_home() {
    _close_detail();
    if (_scr_home) lv_scr_load_anim(_scr_home, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
}

void UI::set_grid_cols(uint8_t cols) {
    _grid_cols = (cols == 3) ? 3 : 2;
}

void UI::set_battery(int pct, bool charging) {
    if (!_bat_label) return;
    char buf[28];
    if (charging) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_CHARGE " %d%%", pct);
        lv_obj_set_style_text_color(_bat_label, C_GREEN, 0);
    } else {
        snprintf(buf, sizeof(buf), "%s %d%%", bat_icon(pct), pct);
        lv_obj_set_style_text_color(_bat_label, C_TEXT2, 0);
    }
    lv_label_set_text(_bat_label, buf);
}

void UI::_build_tabs() {
    // Header
    lv_obj_t* header = lv_obj_create(_scr_home);
    lv_obj_set_size(header, TFT_WIDTH, 48);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, C_BG, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(header, 0, 0);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, LV_SYMBOL_HOME "  " APP_NAME);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(title, C_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16, 0);

    _bat_label = lv_label_create(header);
    lv_label_set_text(_bat_label, LV_SYMBOL_BATTERY_FULL " --");
    lv_obj_set_style_text_font(_bat_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_bat_label, C_TEXT2, 0);
    lv_obj_align(_bat_label, LV_ALIGN_RIGHT_MID, -12, 0);

    _tabview = lv_tabview_create(_scr_home, LV_DIR_TOP, 38);
    lv_obj_set_pos(_tabview, 0, 48);
    lv_obj_set_size(_tabview, TFT_WIDTH, TFT_HEIGHT - 48);
    lv_obj_set_style_bg_color(_tabview, C_BG, 0);
    lv_obj_set_style_bg_opa(_tabview, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(_tabview, C_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_tabview, LV_OPA_COVER, LV_PART_MAIN);

    // Zero out the tabview's inner content panel padding so grids fill edge-to-edge
    lv_obj_t* tv_content = lv_obj_get_child(_tabview, 1);
    if (tv_content) {
        lv_obj_set_style_pad_all(tv_content, 0, 0);
        lv_obj_set_style_pad_gap(tv_content, 0, 0);
    }

    lv_obj_t* tab_bar = lv_tabview_get_tab_btns(_tabview);
    lv_obj_set_style_bg_color(tab_bar, C_BG, 0);
    lv_obj_set_style_text_color(tab_bar, C_TEXT2, 0);
    lv_obj_set_style_text_color(tab_bar, C_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_side(tab_bar, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(tab_bar, C_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(tab_bar, 2, LV_PART_ITEMS | LV_STATE_CHECKED);

    const auto& areas    = _dc->areas();
    const auto& entities = _dc->entities();

    // Helper: zero out the default padding LVGL adds to tab content panels so
    // our own grid padding is the only spacing and tile widths are predictable.
    auto prep_tab = [](lv_obj_t* tab) {
        lv_obj_set_style_pad_all(tab,    0, 0);
        lv_obj_set_style_pad_gap(tab,    0, 0);
        lv_obj_set_style_border_width(tab, 0, 0);
        lv_obj_set_style_bg_color(tab, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(tab,   0, 0);
    };

    auto make_grid = [&](lv_obj_t* tab) -> lv_obj_t* {
        prep_tab(tab);
        lv_obj_t* g = lv_obj_create(tab);
        // 100% of the tab content width; height grows with content (tab itself scrolls)
        lv_obj_set_size(g, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(g, C_BG, 0);
        lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(g, 0, 0);
        lv_obj_set_layout(g, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
        // START alignment so tiles anchor to left edge; centering via padding_all
        lv_obj_set_flex_align(g, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(g,    10, 0);
        lv_obj_set_style_pad_column(g, 10, 0);
        lv_obj_set_style_pad_all(g,    12, 0);
        return g;
    };

    if (areas.empty()) {
        lv_obj_t* tab = lv_tabview_add_tab(_tabview, "All");
        lv_obj_t* g   = make_grid(tab);
        _grids.push_back(g);
        for (const auto& e : entities) _add_tile(g, e);
    } else {
        for (const auto& area : areas) {
            int count = 0;
            for (const auto& e : entities) if (e.area_id == area.id) count++;
            if (count == 0) continue;
            lv_obj_t* tab = lv_tabview_add_tab(_tabview, area.name.c_str());
            lv_obj_t* g   = make_grid(tab);
            _grids.push_back(g);
            for (const auto& e : entities) if (e.area_id == area.id) _add_tile(g, e);
        }
    }
}

// -- Tiles --

void UI::_add_tile(lv_obj_t* grid, const HAEntity& entity) {
    // Tile width depends on grid_cols
    int gap     = 10;
    int padding = 12;
    int tw = (_grid_cols == 3)
        ? (TFT_WIDTH - padding*2 - gap*2) / 3
        : (TFT_WIDTH - padding*2 - gap) / 2;
    int th_ = (_grid_cols == 3) ? 84 : 110;

    lv_obj_t* tile = lv_obj_create(grid);
    lv_obj_set_size(tile, tw, th_);
    style_card(tile);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(tile, 10, 0);

    if (entity.is_on()) {
        lv_obj_set_style_bg_color(tile, C_BG3, 0);
        lv_obj_set_style_border_color(tile, C_ACCENT, 0);
        lv_obj_set_style_border_width(tile, 1, 0);
    }

    lv_obj_t* icon = lv_label_create(tile);
    lv_label_set_text(icon, entity_icon(entity));
    lv_obj_set_style_text_font(icon, (_grid_cols == 3) ? &lv_font_montserrat_18 : &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(icon, entity.is_on() ? C_ACCENT : C_TEXT2, 0);
    lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t* name_lbl = lv_label_create(tile);
    lv_label_set_text(name_lbl, entity.friendly_name.c_str());
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(name_lbl, lv_pct(100));
    style_label_primary(name_lbl);
    lv_obj_align(name_lbl, LV_ALIGN_BOTTOM_LEFT, 0, -16);

    lv_obj_t* state_lbl = lv_label_create(tile);
    lv_label_set_text(state_lbl, entity.is_on() ? "On" : "Off");
    style_label_secondary(state_lbl);
    lv_obj_align(state_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tile, _tile_clicked, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_set_user_data(tile, (void*)entity.entity_id.c_str());
    lv_obj_set_style_bg_color(tile, C_BG3, LV_STATE_PRESSED);

    TileRef ref;
    ref.entity_id = entity.entity_id;
    ref.tile      = tile;
    ref.icon      = icon;
    ref.name_lbl  = name_lbl;
    ref.state_lbl = state_lbl;
    _tiles.push_back(ref);
}

void UI::_update_tile(TileRef& ref, const HAEntity& e) {
    lv_label_set_text(ref.icon, entity_icon(e));
    lv_label_set_text(ref.state_lbl, e.is_on() ? "On" : "Off");
    lv_obj_set_style_text_color(ref.icon, e.is_on() ? C_ACCENT : C_TEXT2, 0);
    lv_obj_set_style_bg_color(ref.tile, e.is_on() ? C_BG3 : C_BG2, 0);
    lv_obj_set_style_border_color(ref.tile, e.is_on() ? C_ACCENT : C_BORDER, 0);
}

void UI::on_entity_update(const HAEntity& e) {
    for (auto& ref : _tiles) {
        if (ref.entity_id == e.entity_id) { _update_tile(ref, e); break; }
    }
    if (_detail_entity_id == e.entity_id && _detail_panel) _detail_update(e);
}

// -- Detail panel --

void UI::_show_detail(const String& entity_id) {
    const HAEntity* ep = _dc->find_entity(entity_id);
    if (!ep) return;
    const HAEntity& e = *ep;
    _detail_entity_id = entity_id;

    lv_obj_t* backdrop = lv_obj_create(_scr_home);
    lv_obj_set_size(backdrop, TFT_WIDTH, TFT_HEIGHT);
    lv_obj_set_style_bg_color(backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(backdrop, LV_OPA_50, 0);
    lv_obj_set_style_border_width(backdrop, 0, 0);
    lv_obj_align(backdrop, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(backdrop, _close_detail_cb, LV_EVENT_SHORT_CLICKED, this);

    int panel_h = e.supports_color ? 440 : (e.supports_brightness ? 310 : 220);
    _detail_panel = lv_obj_create(_scr_home);
    lv_obj_set_size(_detail_panel, TFT_WIDTH, panel_h);
    lv_obj_align(_detail_panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(_detail_panel, C_BG2, 0);
    lv_obj_set_style_radius(_detail_panel, 20, 0);
    lv_obj_set_style_border_width(_detail_panel, 0, 0);
    lv_obj_clear_flag(_detail_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(_detail_panel, 20, 0);

    lv_obj_t* handle = lv_obj_create(_detail_panel);
    lv_obj_set_size(handle, 40, 4);
    lv_obj_align(handle, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(handle, C_BG3, 0);
    lv_obj_set_style_border_width(handle, 0, 0);
    lv_obj_set_style_radius(handle, 4, 0);

    _detail_title = lv_label_create(_detail_panel);
    lv_label_set_text(_detail_title, e.friendly_name.c_str());
    lv_obj_set_style_text_font(_detail_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(_detail_title, C_TEXT, 0);
    lv_obj_align(_detail_title, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t* sw_row = lv_obj_create(_detail_panel);
    lv_obj_set_size(sw_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(sw_row, 0, 0);
    lv_obj_set_style_border_width(sw_row, 0, 0);
    lv_obj_set_style_pad_all(sw_row, 0, 0);
    lv_obj_align(sw_row, LV_ALIGN_TOP_LEFT, 0, 52);
    lv_obj_set_layout(sw_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(sw_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sw_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* sw_lbl = lv_label_create(sw_row);
    lv_label_set_text(sw_lbl, "Power");
    lv_obj_set_style_text_color(sw_lbl, C_TEXT, 0);
    lv_obj_set_style_text_font(sw_lbl, &lv_font_montserrat_16, 0);

    _detail_switch = lv_switch_create(sw_row);
    lv_obj_set_style_bg_color(_detail_switch, C_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (e.is_on()) lv_obj_add_state(_detail_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(_detail_switch, _detail_switch_changed, LV_EVENT_VALUE_CHANGED, this);

    int next_y = 100;

    if (e.supports_brightness) {
        lv_obj_t* br_lbl = lv_label_create(_detail_panel);
        lv_label_set_text(br_lbl, LV_SYMBOL_IMAGE "  Brightness");
        lv_obj_set_style_text_color(br_lbl, C_TEXT2, 0);
        lv_obj_set_style_text_font(br_lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(br_lbl, LV_ALIGN_TOP_LEFT, 0, next_y);

        _detail_brightness = lv_slider_create(_detail_panel);
        lv_obj_set_size(_detail_brightness, lv_pct(100), 28);
        lv_slider_set_range(_detail_brightness, 1, 255);
        lv_slider_set_value(_detail_brightness, e.brightness, LV_ANIM_OFF);
        lv_obj_align(_detail_brightness, LV_ALIGN_TOP_MID, 0, next_y + 24);
        lv_obj_set_style_bg_color(_detail_brightness, C_ACCENT, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(_detail_brightness, C_BG3,    LV_PART_MAIN);
        lv_obj_set_style_bg_color(_detail_brightness, C_TEXT,   LV_PART_KNOB);
        lv_obj_set_style_radius(_detail_brightness, 14, LV_PART_MAIN);
        lv_obj_set_style_radius(_detail_brightness, 14, LV_PART_INDICATOR);
        lv_obj_set_style_pad_all(_detail_brightness, 10, LV_PART_KNOB);
        lv_obj_add_event_cb(_detail_brightness, _brightness_changed, LV_EVENT_VALUE_CHANGED, this);
        next_y += 80;
    }

    if (e.supports_color) {
        lv_obj_t* col_lbl = lv_label_create(_detail_panel);
        lv_label_set_text(col_lbl, LV_SYMBOL_EDIT "  Colour");
        lv_obj_set_style_text_color(col_lbl, C_TEXT2, 0);
        lv_obj_set_style_text_font(col_lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(col_lbl, LV_ALIGN_TOP_LEFT, 0, next_y);

        int wheel_size = TFT_WIDTH - 80;
        _detail_colorwheel = lv_colorwheel_create(_detail_panel, true);
        lv_obj_set_size(_detail_colorwheel, wheel_size, wheel_size);
        lv_obj_align(_detail_colorwheel, LV_ALIGN_TOP_MID, 0, next_y + 24);
        lv_colorwheel_set_rgb(_detail_colorwheel, lv_color_make(e.r, e.g, e.b));
        lv_obj_add_event_cb(_detail_colorwheel, _color_changed, LV_EVENT_VALUE_CHANGED, this);
    }
}

void UI::_close_detail() {
    if (!_detail_panel) return;
    lv_obj_t* parent = lv_obj_get_parent(_detail_panel);
    uint32_t cnt = lv_obj_get_child_cnt(parent);
    if (cnt >= 2) lv_obj_del(lv_obj_get_child(parent, cnt - 2));
    lv_obj_del(_detail_panel);
    _detail_panel       = nullptr;
    _detail_brightness  = nullptr;
    _detail_colorwheel  = nullptr;
    _detail_entity_id   = "";
}

void UI::_detail_update(const HAEntity& e) {
    if (!_detail_panel) return;
    if (_detail_switch) {
        if (e.is_on()) lv_obj_add_state(_detail_switch, LV_STATE_CHECKED);
        else           lv_obj_clear_state(_detail_switch, LV_STATE_CHECKED);
    }
    if (_detail_brightness && e.supports_brightness)
        lv_slider_set_value(_detail_brightness, e.brightness, LV_ANIM_ON);
    if (_detail_colorwheel && e.supports_color)
        lv_colorwheel_set_rgb(_detail_colorwheel, lv_color_make(e.r, e.g, e.b));
}

// -- Event callbacks --

void UI::_tile_clicked(lv_event_t* ev) {
    UI* self        = (UI*)lv_event_get_user_data(ev);
    lv_obj_t* tile  = lv_event_get_target(ev);
    const char* eid = (const char*)lv_obj_get_user_data(tile);
    if (eid) self->_show_detail(String(eid));
}

void UI::_detail_switch_changed(lv_event_t* ev) {
    UI* self     = (UI*)lv_event_get_user_data(ev);
    lv_obj_t* sw = lv_event_get_target(ev);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (self->_dc && !self->_detail_entity_id.isEmpty()) {
        if (on) self->_dc->turn_on(self->_detail_entity_id);
        else    self->_dc->turn_off(self->_detail_entity_id);
    }
}

void UI::_brightness_changed(lv_event_t* ev) {
    UI* self     = (UI*)lv_event_get_user_data(ev);
    lv_obj_t* sl = lv_event_get_target(ev);
    if (self->_dc && !self->_detail_entity_id.isEmpty())
        self->_dc->set_brightness(self->_detail_entity_id, (uint8_t)lv_slider_get_value(sl));
}

void UI::_color_changed(lv_event_t* ev) {
    UI* self     = (UI*)lv_event_get_user_data(ev);
    lv_obj_t* cw = lv_event_get_target(ev);
    lv_color_t c = lv_colorwheel_get_rgb(cw);
    // Convert to 32-bit to safely extract R/G/B bytes regardless of LV_COLOR_16_SWAP
    lv_color32_t c32;
    c32.full = lv_color_to32(c);
    if (self->_dc && !self->_detail_entity_id.isEmpty())
        self->_dc->set_color(self->_detail_entity_id, c32.ch.red, c32.ch.green, c32.ch.blue);
}

void UI::_close_detail_cb(lv_event_t* ev) {
    ((UI*)lv_event_get_user_data(ev))->_close_detail();
}

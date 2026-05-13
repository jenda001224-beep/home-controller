#include "ui.h"
#include "theme.h"
#include "config.h"
#include <Arduino.h>

static const char* entity_icon(const HAEntity& e) {
    // Use the same reliable symbol per entity type — color shows on/off state
    if (e.type == EntityType::LIGHT)  return LV_SYMBOL_IMAGE;   // picture/light glyph
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

    // Explicit background rectangle — screen bg_color alone is unreliable in LVGL 8
    lv_obj_t* bg = lv_obj_create(_scr_splash);
    lv_obj_set_size(bg, TFT_WIDTH, TFT_HEIGHT);
    lv_obj_align(bg, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bg, C_BG, 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_radius(bg, 0, 0);
    lv_obj_set_style_pad_all(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);

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
    _close_detail();   // must happen before _scr_home is deleted
    if (_scr_home) {
        lv_obj_del(_scr_home);
        _scr_home  = nullptr;
        _tabview   = nullptr;
        _bat_label = nullptr;
        _detail_panel      = nullptr;
        _detail_title      = nullptr;
        _detail_switch     = nullptr;
        _detail_bri_view   = nullptr;
        _detail_col_view   = nullptr;
        _detail_bri_pill   = nullptr;
        _detail_bri_fill   = nullptr;
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

    // Explicit background rectangle — screen bg_color alone is unreliable in LVGL 8
    lv_obj_t* bg = lv_obj_create(_scr_home);
    lv_obj_set_size(bg, TFT_WIDTH, TFT_HEIGHT);
    lv_obj_align(bg, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bg, C_BG, 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_radius(bg, 0, 0);
    lv_obj_set_style_pad_all(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);

    _build_tabs();
    lv_scr_load_anim(_scr_home, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}

void UI::go_home() {
    _close_detail();
    if (_scr_home) lv_scr_load_anim(_scr_home, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
}

void UI::set_grid_cols(uint8_t cols) {
    _grid_cols = (cols >= 1 && cols <= 3) ? cols : 2;
}

void UI::set_battery(int pct, bool charging) {
    if (!_bat_label) return;
    char buf[28];
    if (charging) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_CHARGE " %d%%", pct);
        lv_obj_set_style_text_color(_bat_label, C_GREEN, 0);
    } else {
        snprintf(buf, sizeof(buf), "%s %d%%", bat_icon(pct), pct);
        // Red below 20%, orange below 40%, gray otherwise
        lv_color_t col = (pct < 20) ? C_RED : (pct < 40) ? C_ACCENT : C_TEXT2;
        lv_obj_set_style_text_color(_bat_label, col, 0);
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

    // Zero out the tabview's inner content panel padding so grids fill edge-to-edge.
    // Also disable horizontal swipe — it was consuming all tile tap events.
    lv_obj_t* tv_content = lv_obj_get_child(_tabview, 1);
    if (tv_content) {
        lv_obj_set_style_pad_all(tv_content, 0, 0);
        lv_obj_set_style_pad_gap(tv_content, 0, 0);
        // Disable ALL scrolling/gesture detection on the tabview content container.
        // lv_obj_set_scroll_dir(NONE) alone leaves LV_OBJ_FLAG_SCROLLABLE set,
        // which causes LVGL's gesture detector to get stuck and never fire click events.
        lv_obj_clear_flag(tv_content, LV_OBJ_FLAG_SCROLLABLE);
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
        // Lock to vertical scroll only so tiles never drift off to the right
        lv_obj_set_scroll_dir(tab, LV_DIR_VER);
        lv_obj_add_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    };

    auto make_grid = [&](lv_obj_t* tab) -> lv_obj_t* {
        prep_tab(tab);
        lv_obj_t* g = lv_obj_create(tab);
        lv_obj_set_size(g, TFT_WIDTH, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(g, TFT_WIDTH, 0);   // hard cap — never wider than screen
        lv_obj_set_style_bg_color(g, C_BG, 0);
        lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(g, 0, 0);
        // Grid itself must NOT scroll — the tab container scrolls vertically
        lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_layout(g, LV_LAYOUT_FLEX);
        if (_grid_cols == 1) {
            lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(g, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        } else {
            lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
            lv_obj_set_flex_align(g, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        }
        lv_obj_set_style_pad_row(g,    8, 0);
        lv_obj_set_style_pad_column(g, 8, 0);
        lv_obj_set_style_pad_all(g,    8, 0);
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
    const int padding = 8;
    bool list = (_grid_cols == 1);

    // --- List mode: full-width horizontal row ---
    if (list) {
        lv_obj_t* tile = lv_obj_create(grid);
        // Explicit pixels: screen width minus 8px padding on each side
        lv_obj_set_size(tile, TFT_WIDTH - 16, 62);
        style_card(tile);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_clip_corner(tile, true, 0);
        lv_obj_set_style_pad_all(tile, 0, 0);
        lv_obj_set_ext_click_area(tile, 6);   // wider invisible hit zone
        lv_obj_set_layout(tile, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        if (entity.is_on()) {
            lv_obj_set_style_bg_color(tile, C_BG3, 0);
            lv_obj_set_style_border_color(tile, C_ACCENT, 0);
            lv_obj_set_style_border_width(tile, 1, 0);
        }

        // Icon column — NOT clickable so taps pass through to the tile
        lv_obj_t* icon_box = lv_obj_create(tile);
        lv_obj_set_size(icon_box, 52, 58);
        lv_obj_set_style_bg_opa(icon_box, 0, 0);
        lv_obj_set_style_border_width(icon_box, 0, 0);
        lv_obj_set_style_pad_all(icon_box, 0, 0);
        lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* icon = lv_label_create(icon_box);
        lv_label_set_text(icon, entity_icon(entity));
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_22, 0);
        lv_obj_set_style_text_color(icon, entity.is_on() ? C_ACCENT : C_TEXT2, 0);
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, 0);

        // Text column — NOT clickable so taps pass through to the tile
        lv_obj_t* text_box = lv_obj_create(tile);
        lv_obj_set_size(text_box, LV_SIZE_CONTENT, 58);
        lv_obj_set_flex_grow(text_box, 1);
        lv_obj_set_style_bg_opa(text_box, 0, 0);
        lv_obj_set_style_border_width(text_box, 0, 0);
        lv_obj_set_style_pad_all(text_box, 0, 0);
        lv_obj_clear_flag(text_box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(text_box, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* name_lbl = lv_label_create(text_box);
        lv_label_set_text(name_lbl, entity.friendly_name.c_str());
        lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(name_lbl, TFT_WIDTH - 16 - 52 - 40);  // screen - pad - icon - chevron
        lv_obj_set_style_text_color(name_lbl, C_TEXT, 0);
        lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 0, -8);

        lv_obj_t* state_lbl = lv_label_create(text_box);
        lv_label_set_text(state_lbl, entity.is_on() ? "On" : "Off");
        lv_obj_set_style_text_color(state_lbl, entity.is_on() ? C_ACCENT : C_TEXT2, 0);
        lv_obj_set_style_text_font(state_lbl, &lv_font_montserrat_12, 0);
        lv_obj_align(state_lbl, LV_ALIGN_LEFT_MID, 0, 10);

        // Chevron
        lv_obj_t* chev = lv_label_create(tile);
        lv_label_set_text(chev, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(chev, C_BG3, 0);
        lv_obj_set_style_text_font(chev, &lv_font_montserrat_14, 0);
        lv_obj_set_style_pad_right(chev, 12, 0);

        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tile, _tile_clicked,      LV_EVENT_SHORT_CLICKED, this); // tap = toggle
        lv_obj_add_event_cb(tile, _tile_long_pressed, LV_EVENT_LONG_PRESSED,  this); // hold = detail
        lv_obj_set_user_data(tile, (void*)entity.entity_id.c_str());
        lv_obj_set_style_bg_color(tile, C_BG3, LV_STATE_PRESSED);

        TileRef ref;
        ref.entity_id = entity.entity_id;
        ref.tile      = tile;
        ref.icon      = icon;
        ref.name_lbl  = name_lbl;
        ref.state_lbl = state_lbl;
        _tiles.push_back(ref);
        return;
    }

    // --- Grid mode (2 or 3 columns) ---
    const int gap = 8;
    int tw = (_grid_cols == 3)
        ? (TFT_WIDTH - padding*2 - gap*2) / 3
        : (TFT_WIDTH - padding*2 - gap)   / 2;
    const int th_ = (_grid_cols == 3) ? 72 : 80;

    lv_obj_t* tile = lv_obj_create(grid);
    lv_obj_set_size(tile, tw, th_);
    style_card(tile);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(tile, 8, 0);

    if (entity.is_on()) {
        lv_obj_set_style_bg_color(tile, C_BG3, 0);
        lv_obj_set_style_border_color(tile, C_ACCENT, 0);
        lv_obj_set_style_border_width(tile, 1, 0);
    }

    lv_obj_t* icon = lv_label_create(tile);
    lv_label_set_text(icon, entity_icon(entity));
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(icon, entity.is_on() ? C_ACCENT : C_TEXT2, 0);
    lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t* name_lbl = lv_label_create(tile);
    lv_label_set_text(name_lbl, entity.friendly_name.c_str());
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(name_lbl, lv_pct(100));
    lv_obj_set_style_text_color(name_lbl, C_TEXT, 0);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(name_lbl, LV_ALIGN_BOTTOM_LEFT, 0, -14);

    lv_obj_t* state_lbl = lv_label_create(tile);
    lv_label_set_text(state_lbl, entity.is_on() ? "On" : "Off");
    lv_obj_set_style_text_color(state_lbl, C_TEXT2, 0);
    lv_obj_set_style_text_font(state_lbl, &lv_font_montserrat_12, 0);
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

// -- Detail panel (fullscreen iOS Control Center style) --

void UI::_show_detail(const String& entity_id) {
    const HAEntity* ep = _dc->find_entity(entity_id);
    if (!ep) return;
    const HAEntity& e = *ep;
    _detail_entity_id = entity_id;

    // Full-screen panel on lv_layer_top() — above everything, never clipped
    _detail_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_detail_panel, TFT_WIDTH, TFT_HEIGHT);
    lv_obj_align(_detail_panel, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(_detail_panel, C_BG, 0);
    lv_obj_set_style_bg_opa(_detail_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(_detail_panel, 0, 0);
    lv_obj_set_style_border_width(_detail_panel, 0, 0);
    lv_obj_set_style_pad_all(_detail_panel, 0, 0);
    lv_obj_clear_flag(_detail_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_detail_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_detail_panel, _close_detail_cb, LV_EVENT_SHORT_CLICKED, this);

    // Handle bar (drag indicator)
    lv_obj_t* handle = lv_obj_create(_detail_panel);
    lv_obj_set_size(handle, 48, 5);
    lv_obj_align(handle, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_bg_color(handle, C_BG3, 0);
    lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(handle, 0, 0);
    lv_obj_set_style_radius(handle, 3, 0);

    // Close button — top-right corner, large touch target
    lv_obj_t* close_btn = lv_btn_create(_detail_panel);
    lv_obj_set_size(close_btn, 44, 44);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_set_style_bg_color(close_btn, C_BG2, 0);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(close_btn, 22, 0);
    lv_obj_set_style_shadow_width(close_btn, 0, 0);
    lv_obj_set_style_border_width(close_btn, 0, 0);
    lv_obj_set_ext_click_area(close_btn, 8);
    lv_obj_add_event_cb(close_btn, _close_detail_cb, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_t* close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(close_lbl, C_TEXT2, 0);
    lv_obj_set_style_text_font(close_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(close_lbl, LV_ALIGN_CENTER, 0, 0);

    // Device name
    _detail_title = lv_label_create(_detail_panel);
    lv_label_set_text(_detail_title, e.friendly_name.c_str());
    lv_obj_set_style_text_font(_detail_title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(_detail_title, C_TEXT, 0);
    lv_obj_align(_detail_title, LV_ALIGN_TOP_MID, 0, 26);

    // Power row
    lv_obj_t* sw_row = lv_obj_create(_detail_panel);
    lv_obj_set_size(sw_row, TFT_WIDTH - 40, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(sw_row, 0, 0);
    lv_obj_set_style_border_width(sw_row, 0, 0);
    lv_obj_set_style_pad_all(sw_row, 0, 0);
    lv_obj_align(sw_row, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_set_layout(sw_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(sw_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sw_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(sw_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* sw_lbl = lv_label_create(sw_row);
    lv_label_set_text(sw_lbl, "Power");
    lv_obj_set_style_text_color(sw_lbl, C_TEXT, 0);
    lv_obj_set_style_text_font(sw_lbl, &lv_font_montserrat_16, 0);

    _detail_switch = lv_switch_create(sw_row);
    lv_obj_set_style_bg_color(_detail_switch, C_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (e.is_on()) lv_obj_add_state(_detail_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(_detail_switch, _detail_switch_changed, LV_EVENT_VALUE_CHANGED, this);

    // ---- Brightness view ----
    _detail_bri_view = lv_obj_create(_detail_panel);
    lv_obj_set_size(_detail_bri_view, TFT_WIDTH, TFT_HEIGHT - 110);
    lv_obj_align(_detail_bri_view, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(_detail_bri_view, 0, 0);
    lv_obj_set_style_border_width(_detail_bri_view, 0, 0);
    lv_obj_set_style_pad_all(_detail_bri_view, 0, 0);
    lv_obj_clear_flag(_detail_bri_view, LV_OBJ_FLAG_SCROLLABLE);

    if (e.supports_brightness) {
        // Sun icon above pill
        lv_obj_t* sun = lv_label_create(_detail_bri_view);
        lv_label_set_text(sun, LV_SYMBOL_IMAGE);
        lv_obj_set_style_text_font(sun, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(sun, C_TEXT2, 0);
        lv_obj_align(sun, LV_ALIGN_TOP_MID, 0, 8);

        // The iOS vertical pill
        const int PILL_W = 80;
        const int PILL_H = TFT_HEIGHT - 110 - 48 - 20; // ~262px
        _detail_pill_h = PILL_H;

        _detail_bri_pill = lv_obj_create(_detail_bri_view);
        lv_obj_set_size(_detail_bri_pill, PILL_W, PILL_H);
        lv_obj_align(_detail_bri_pill, LV_ALIGN_TOP_MID, 0, 40);
        lv_obj_set_style_bg_color(_detail_bri_pill, C_BG2, 0);
        lv_obj_set_style_bg_opa(_detail_bri_pill, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(_detail_bri_pill, PILL_W / 2, 0);
        lv_obj_set_style_border_width(_detail_bri_pill, 0, 0);
        lv_obj_set_style_pad_all(_detail_bri_pill, 0, 0);
        lv_obj_clear_flag(_detail_bri_pill, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(_detail_bri_pill, LV_OBJ_FLAG_CLICKABLE);

        // Fill (orange, from bottom, height = brightness fraction of pill)
        int fill_h = (int)((int)e.brightness * PILL_H / 255);
        if (fill_h < PILL_W / 2) fill_h = PILL_W / 2; // min visible cap
        _detail_bri_fill = lv_obj_create(_detail_bri_pill);
        lv_obj_set_size(_detail_bri_fill, PILL_W, fill_h);
        lv_obj_align(_detail_bri_fill, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(_detail_bri_fill, C_ACCENT, 0);
        lv_obj_set_style_bg_opa(_detail_bri_fill, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(_detail_bri_fill, PILL_W / 2, 0);
        lv_obj_set_style_border_width(_detail_bri_fill, 0, 0);
        lv_obj_set_style_pad_all(_detail_bri_fill, 0, 0);

        // Drag events on pill
        lv_obj_add_event_cb(_detail_bri_pill, _bri_drag_cb, LV_EVENT_PRESSING, this);

        if (e.supports_color) {
            lv_obj_t* hint = lv_label_create(_detail_bri_view);
            lv_label_set_text(hint, LV_SYMBOL_EDIT "  Colour");
            lv_obj_set_style_text_color(hint, C_TEXT2, 0);
            lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
            lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
            lv_obj_add_flag(hint, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(hint, _go_color_cb, LV_EVENT_SHORT_CLICKED, this);
            lv_obj_add_event_cb(_detail_bri_pill, _go_color_cb, LV_EVENT_SHORT_CLICKED, this);
        }
    } else if (!e.supports_brightness && e.supports_color) {
        lv_obj_add_flag(_detail_bri_view, LV_OBJ_FLAG_HIDDEN);
    }

    // ---- Colour view ----
    if (e.supports_color) {
        _detail_col_view = lv_obj_create(_detail_panel);
        lv_obj_set_size(_detail_col_view, TFT_WIDTH, TFT_HEIGHT - 110);
        lv_obj_align(_detail_col_view, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_opa(_detail_col_view, 0, 0);
        lv_obj_set_style_border_width(_detail_col_view, 0, 0);
        lv_obj_set_style_pad_all(_detail_col_view, 0, 0);
        lv_obj_clear_flag(_detail_col_view, LV_OBJ_FLAG_SCROLLABLE);
        if (e.supports_brightness) lv_obj_add_flag(_detail_col_view, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t* back_btn = lv_btn_create(_detail_col_view);
        lv_obj_set_size(back_btn, TFT_WIDTH - 40, 36);
        lv_obj_align(back_btn, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_bg_color(back_btn, C_BG3, 0);
        lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(back_btn, 10, 0);
        lv_obj_set_style_shadow_width(back_btn, 0, 0);
        lv_obj_add_event_cb(back_btn, _go_bri_cb, LV_EVENT_SHORT_CLICKED, this);
        lv_obj_t* back_lbl = lv_label_create(back_btn);
        lv_label_set_text(back_lbl, LV_SYMBOL_LEFT "  Brightness");
        lv_obj_set_style_text_color(back_lbl, C_TEXT, 0);
        lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(back_lbl, LV_ALIGN_LEFT_MID, 0, 0);

        int wheel_sz = TFT_WIDTH - 40;
        _detail_colorwheel = lv_colorwheel_create(_detail_col_view, true);
        lv_obj_set_size(_detail_colorwheel, wheel_sz, wheel_sz);
        lv_obj_align(_detail_colorwheel, LV_ALIGN_TOP_MID, 0, 46);
        lv_colorwheel_set_rgb(_detail_colorwheel, lv_color_make(e.r, e.g, e.b));
        lv_obj_add_event_cb(_detail_colorwheel, _color_changed, LV_EVENT_VALUE_CHANGED, this);
    }
}

void UI::_close_detail() {
    if (!_detail_panel) return;
    lv_obj_del(_detail_panel);
    _detail_panel       = nullptr;
    _detail_title       = nullptr;
    _detail_switch      = nullptr;
    _detail_bri_view    = nullptr;
    _detail_col_view    = nullptr;
    _detail_bri_pill    = nullptr;
    _detail_bri_fill    = nullptr;
    _detail_colorwheel  = nullptr;
    _detail_entity_id   = "";
}

void UI::_detail_update(const HAEntity& e) {
    if (!_detail_panel) return;
    if (_detail_switch) {
        if (e.is_on()) lv_obj_add_state(_detail_switch, LV_STATE_CHECKED);
        else           lv_obj_clear_state(_detail_switch, LV_STATE_CHECKED);
    }
    if (_detail_bri_fill && _detail_bri_pill && e.supports_brightness) {
        int fill_h = (int)((int)e.brightness * _detail_pill_h / 255);
        if (fill_h < 40) fill_h = 40;
        lv_obj_set_height(_detail_bri_fill, fill_h);
        lv_obj_align(_detail_bri_fill, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    if (_detail_colorwheel && e.supports_color)
        lv_colorwheel_set_rgb(_detail_colorwheel, lv_color_make(e.r, e.g, e.b));
}

// -- Event callbacks --

void UI::_tile_clicked(lv_event_t* ev) {
    // Short tap → toggle on/off immediately
    UI* self        = (UI*)lv_event_get_user_data(ev);
    lv_obj_t* tile  = lv_event_get_target(ev);
    const char* eid = (const char*)lv_obj_get_user_data(tile);
    if (!eid || !self->_dc) return;
    const HAEntity* ep = self->_dc->find_entity(String(eid));
    if (!ep) return;
    if (ep->is_on()) self->_dc->turn_off(String(eid));
    else             self->_dc->turn_on(String(eid));
}

void UI::_tile_long_pressed(lv_event_t* ev) {
    // Long press → open brightness/color detail panel
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

void UI::_bri_drag_cb(lv_event_t* ev) {
    UI* self = (UI*)lv_event_get_user_data(ev);
    if (!self->_detail_bri_pill || !self->_detail_bri_fill) return;

    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);

    lv_area_t pill_area;
    lv_obj_get_coords(self->_detail_bri_pill, &pill_area);
    lv_coord_t pill_top = pill_area.y1;
    lv_coord_t rel_y    = pt.y - pill_top;
    int ph = self->_detail_pill_h;
    if (rel_y < 0)  rel_y = 0;
    if (rel_y > ph) rel_y = ph;

    // Top = 100%, bottom = 0%
    uint8_t bri = (uint8_t)(255 - (rel_y * 255 / ph));
    if (bri < 1)   bri = 1;
    if (bri > 255) bri = 255;

    int fill_h = (int)(bri * ph / 255);
    if (fill_h < 40) fill_h = 40;
    lv_obj_set_height(self->_detail_bri_fill, fill_h);
    lv_obj_align(self->_detail_bri_fill, LV_ALIGN_BOTTOM_MID, 0, 0);

    if (self->_dc && !self->_detail_entity_id.isEmpty())
        self->_dc->set_brightness(self->_detail_entity_id, bri);
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
    UI* self = (UI*)lv_event_get_user_data(ev);
    lv_obj_t* target = lv_event_get_target(ev);
    lv_obj_t* current = lv_event_get_current_target(ev);
    // Fire only when clicking directly on the registered object (panel bg or close btn)
    // Prevents event bubble from child widgets accidentally closing the panel
    if (target != current) return;
    self->_close_detail();
}

void UI::_go_color_cb(lv_event_t* ev) {
    UI* self = (UI*)lv_event_get_user_data(ev);
    if (self->_detail_bri_view) lv_obj_add_flag(self->_detail_bri_view, LV_OBJ_FLAG_HIDDEN);
    if (self->_detail_col_view) lv_obj_clear_flag(self->_detail_col_view, LV_OBJ_FLAG_HIDDEN);
}

void UI::_go_bri_cb(lv_event_t* ev) {
    UI* self = (UI*)lv_event_get_user_data(ev);
    if (self->_detail_col_view) lv_obj_add_flag(self->_detail_col_view, LV_OBJ_FLAG_HIDDEN);
    if (self->_detail_bri_view) lv_obj_clear_flag(self->_detail_bri_view, LV_OBJ_FLAG_HIDDEN);
}

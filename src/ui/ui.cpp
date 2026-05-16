#include "ui.h"
#include "theme.h"
#include "config.h"
#include "fonts/fonts.h"
#include <Arduino.h>

// Runtime copies of the const font objects with fallback set to built-in Montserrat.
// This is necessary because:
//   1. Custom fonts cover text ranges (0x20-0x7F, 0xA0-0xFF, 0x100-0x17F) but not
//      LVGL symbol glyphs (U+F000+).
//   2. lv_font_t is const in flash; we can't patch it in-place.
//   3. LVGL 8 follows font.fallback for missing glyphs, solving symbols + diacritics
//      in one place without per-label logic.
// Initialised in UI::begin() before any LVGL objects are created.
static lv_font_t _f12, _f14, _f22;

static constexpr int TILE_PAD  = 60;                       // visible margin on each side
static constexpr int TILE_W    = TFT_WIDTH - TILE_PAD * 2; // = 200px (62.5% of 320)
static constexpr int TILE_H    = 68;                        // slightly taller = less squished
static constexpr int TILE_GAP  = 8;                        // small gap between tile rows

static const char* entity_icon(const HAEntity& e) {
    // Use the same reliable symbol per entity type — color shows on/off state
    if (e.type == EntityType::LIGHT)  return LV_SYMBOL_IMAGE;   // picture/light glyph
    if (e.type == EntityType::SWITCH) return LV_SYMBOL_POWER;
    return LV_SYMBOL_SETTINGS;
}

// Extract hue (0-359) from RGB — used for the hue slider initial position
static uint16_t rgb_to_hue(uint8_t r, uint8_t g, uint8_t b) {
    float rf = r / 255.f, gf = g / 255.f, bf = b / 255.f;
    float mx = (rf > gf) ? ((rf > bf) ? rf : bf) : ((gf > bf) ? gf : bf);
    float mn = (rf < gf) ? ((rf < bf) ? rf : bf) : ((gf < bf) ? gf : bf);
    float d  = mx - mn;
    if (d < 0.001f) return 0;
    float h;
    if      (mx == rf) h = 60.f * fmodf((gf - bf) / d, 6.f);
    else if (mx == gf) h = 60.f * ((bf - rf) / d + 2.f);
    else               h = 60.f * ((rf - gf) / d + 4.f);
    if (h < 0) h += 360.f;
    return (uint16_t)h;
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

    // Build runtime font objects: Czech glyphs from our font, symbols from built-in Montserrat.
    _f12 = font_cs_12; _f12.fallback = &lv_font_montserrat_12;
    _f14 = font_cs_14; _f14.fallback = &lv_font_montserrat_14;
    _f22 = font_cs_22; _f22.fallback = &lv_font_montserrat_22;

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
    lv_obj_set_style_text_font(_splash_status, &_f14, 0);
    lv_obj_set_style_text_align(_splash_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(_splash_status, TFT_WIDTH - 40);
    lv_obj_align(_splash_status, LV_ALIGN_CENTER, 0, 20);
    lv_label_set_long_mode(_splash_status, LV_LABEL_LONG_WRAP);

    // Version
    lv_obj_t* ver = lv_label_create(_scr_splash);
    lv_label_set_text(ver, APP_VERSION);
    lv_obj_set_style_text_color(ver, C_TEXT2, 0);
    lv_obj_set_style_text_font(ver, &_f12, 0);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_MID, 0, -12);

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
        // If _scr_home is the currently displayed screen, switch away first.
        // Deleting the active screen while LVGL holds a pointer to it causes LoadProhibited.
        if (lv_scr_act() == _scr_home) {
            lv_obj_t* safe = _scr_splash ? _scr_splash : lv_obj_create(nullptr);
            lv_scr_load(safe);
        }
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

void UI::set_battery(int pct, bool charging, float v) {
    if (!_bat_label) return;
    char buf[20];

    if (v < 0) {
        // PMU not available
        lv_label_set_text(_bat_label, "--");
        lv_obj_set_style_text_color(_bat_label, C_TEXT2, 0);
        return;
    }
    if (charging) {
        // USB connected: bolt + percent
        snprintf(buf, sizeof(buf), LV_SYMBOL_CHARGE " %d%%", pct);
        lv_obj_set_style_text_color(_bat_label, C_GREEN, 0);
    } else {
        // On battery: icon + percent
        snprintf(buf, sizeof(buf), "%s %d%%", bat_icon(pct), pct);
        lv_color_t col = (pct < 20) ? C_RED : (pct < 40) ? C_ACCENT : C_TEXT;
        lv_obj_set_style_text_color(_bat_label, col, 0);
    }
    lv_label_set_text(_bat_label, buf);
}

void UI::set_hidden_ids(const String& ids) {
    _hidden_ids.clear();
    if (ids.isEmpty()) return;
    int start = 0;
    while (start < (int)ids.length()) {
        int comma = ids.indexOf(',', start);
        if (comma < 0) comma = ids.length();
        String tok = ids.substring(start, comma);
        tok.trim();
        if (!tok.isEmpty()) _hidden_ids.push_back(tok);
        start = comma + 1;
    }
}

bool UI::is_hidden(const String& id) const {
    for (auto& h : _hidden_ids) if (h == id) return true;
    return false;
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
    lv_obj_set_style_text_font(title, &_f22, 0);
    lv_obj_set_style_text_color(title, C_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16, 0);

    _bat_label = lv_label_create(header);
    lv_label_set_text(_bat_label, LV_SYMBOL_BATTERY_FULL " --");
    lv_obj_set_style_text_font(_bat_label, &_f14, 0);
    lv_obj_set_style_text_color(_bat_label, C_TEXT, 0);
    lv_obj_align(_bat_label, LV_ALIGN_RIGHT_MID, -TILE_PAD, 0);  // align with tile right edge

    _tabview = lv_tabview_create(_scr_home, LV_DIR_TOP, 38);
    lv_obj_set_pos(_tabview, 0, 48);
    lv_obj_set_size(_tabview, TFT_WIDTH, TFT_HEIGHT - 48);
    lv_obj_set_style_bg_color(_tabview, C_BG, 0);
    lv_obj_set_style_bg_opa(_tabview, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(_tabview, C_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_tabview, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(_tabview, 0, 0);    // kill any theme-applied tabview padding

    // Zero out the tabview's inner content panel padding so grids fill edge-to-edge.
    // Also disable horizontal swipe — it was consuming all tile tap events.
    lv_obj_t* tv_content = lv_obj_get_child(_tabview, 1);
    if (tv_content) {
        // Zero padding only — do NOT touch scroll flags.
        // tv_content uses flex ROW to lay out tabs; clearing SCROLLABLE breaks
        // the tab sizing so pct(100) children get wrong reference width.
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

    // Two-level layout:
    //   tab page  — NON-scrollable, so horizontal swipes fall through to tv_content → tab switch
    //   inner grid — fills tab page, handles vertical scroll; tiles live here
    // This fixes: (1) tile tap/long-press blocked by scroll detection on parent,
    //             (2) horizontal swipe swallowed before reaching tv_content.
    auto make_grid = [&](lv_obj_t* tab) -> lv_obj_t* {
        // Tab page: transparent, non-scrollable wrapper
        lv_obj_set_style_pad_all(tab, 0, 0);
        lv_obj_set_style_border_width(tab, 0, 0);
        lv_obj_set_style_bg_color(tab, C_BG, 0);
        lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
        lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

        // Inner grid: fills tab, owns vertical scroll and tile layout
        lv_obj_t* g = lv_obj_create(tab);
        lv_obj_set_size(g, lv_pct(100), lv_pct(100));
        lv_obj_set_style_pad_all(g,    TILE_PAD, 0);  // outer horizontal/vertical margin
        lv_obj_set_style_pad_row(g,    TILE_GAP, 0);  // small row gap between tiles
        lv_obj_set_style_pad_column(g, TILE_GAP, 0);
        lv_obj_set_style_border_width(g, 0, 0);
        lv_obj_set_style_bg_color(g, C_BG, 0);
        lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
        lv_obj_set_scroll_dir(g, LV_DIR_VER);
        lv_obj_add_flag(g, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_layout(g, LV_LAYOUT_FLEX);
        if (_grid_cols == 1) {
            lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
        } else {
            lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
            lv_obj_set_flex_align(g, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        }
        return g;
    };

    if (areas.empty()) {
        lv_obj_t* tab = lv_tabview_add_tab(_tabview, "All");
        lv_obj_t* g   = make_grid(tab);
        _grids.push_back(g);
        if (entities.empty()) {
            // No supported devices (LIGHT / OUTLET) found on the hub.
            // Show raw device count (from last fetch) to help diagnose filtering issues.
            int raw = _dc ? _dc->dbg_raw_count() : -1;
            String msg = "No devices found.";
            if (raw == 0) {
                msg += "\n\nDIRIGERA returned\nan empty response.";
            } else if (raw > 0) {
                msg += "\n\nDIRIGERA has " + String(raw) + " device(s)\nbut none are lights\nor outlets.";
                String types = _dc->dbg_types();
                if (!types.isEmpty()) msg += "\n\nTypes: " + types;
            } else {
                msg += "\n\nMake sure your DIRIGERA\nhas lights or outlets.";
            }
            lv_obj_t* lbl = lv_label_create(g);
            lv_label_set_text(lbl, msg.c_str());
            lv_obj_set_style_text_color(lbl, C_TEXT2, 0);
            lv_obj_set_style_text_font(lbl, &_f14, 0);
            lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(lbl, TILE_W);
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
            lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 40);
        } else {
            for (const auto& e : entities)
                if (!is_hidden(e.entity_id)) _add_tile(g, e);
        }
    } else {
        for (const auto& area : areas) {
            int count = 0;
            for (const auto& e : entities)
                if (e.area_id == area.id && !is_hidden(e.entity_id)) count++;
            if (count == 0) continue;
            lv_obj_t* tab = lv_tabview_add_tab(_tabview, area.name.c_str());
            lv_obj_t* g   = make_grid(tab);
            _grids.push_back(g);
            for (const auto& e : entities)
                if (e.area_id == area.id && !is_hidden(e.entity_id)) _add_tile(g, e);
        }
    }
}

// -- Tiles --

void UI::_add_tile(lv_obj_t* grid, const HAEntity& entity) {
    bool list = (_grid_cols == 1);

    // --- List mode: full-width horizontal row ---
    if (list) {
        lv_obj_t* tile = lv_obj_create(grid);
        lv_obj_set_size(tile, TILE_W, TILE_H);   // explicit pixels — no percentage chain
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
        lv_obj_set_size(icon_box, 52, TILE_H);
        lv_obj_set_style_bg_opa(icon_box, 0, 0);
        lv_obj_set_style_border_width(icon_box, 0, 0);
        lv_obj_set_style_pad_all(icon_box, 0, 0);
        lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* icon = lv_label_create(icon_box);
        lv_label_set_text(icon, entity_icon(entity));
        lv_obj_set_style_text_font(icon, &_f22, 0);
        lv_obj_set_style_text_color(icon, entity.is_on() ? C_ACCENT : C_TEXT2, 0);
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, 0);

        // Text column — NOT clickable so taps pass through to the tile
        lv_obj_t* text_box = lv_obj_create(tile);
        lv_obj_set_size(text_box, LV_SIZE_CONTENT, TILE_H);
        lv_obj_set_flex_grow(text_box, 1);
        lv_obj_set_style_bg_opa(text_box, 0, 0);
        lv_obj_set_style_border_width(text_box, 0, 0);
        lv_obj_set_style_pad_all(text_box, 0, 0);
        lv_obj_clear_flag(text_box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(text_box, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* name_lbl = lv_label_create(text_box);
        lv_label_set_text(name_lbl, entity.friendly_name.c_str());
        lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(name_lbl, TILE_W - 52 - 40);  // tile - icon_box - chevron
        lv_obj_set_style_text_color(name_lbl, C_TEXT, 0);
        lv_obj_set_style_text_font(name_lbl, &_f14, 0);
        lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 0, -8);

        lv_obj_t* state_lbl = lv_label_create(text_box);
        lv_label_set_text(state_lbl, entity.is_on() ? "On" : "Off");
        lv_obj_set_style_text_color(state_lbl, entity.is_on() ? C_ACCENT : C_TEXT2, 0);
        lv_obj_set_style_text_font(state_lbl, &_f12, 0);
        lv_obj_align(state_lbl, LV_ALIGN_LEFT_MID, 0, 10);

        // Chevron
        lv_obj_t* chev = lv_label_create(tile);
        lv_label_set_text(chev, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(chev, C_BG3, 0);
        lv_obj_set_style_text_font(chev, &_f14, 0);
        lv_obj_set_style_pad_right(chev, 12, 0);

        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tile, _tile_clicked, LV_EVENT_SHORT_CLICKED, this); // tap = open detail
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
    const int gap = TILE_PAD;
    int tw = (_grid_cols == 3)
        ? (TFT_WIDTH - TILE_PAD*2 - gap*2) / 3
        : (TFT_WIDTH - TILE_PAD*2 - gap)   / 2;
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
    lv_obj_set_style_text_font(name_lbl, &_f12, 0);
    lv_obj_align(name_lbl, LV_ALIGN_BOTTOM_LEFT, 0, -14);

    lv_obj_t* state_lbl = lv_label_create(tile);
    lv_label_set_text(state_lbl, entity.is_on() ? "On" : "Off");
    lv_obj_set_style_text_color(state_lbl, C_TEXT2, 0);
    lv_obj_set_style_text_font(state_lbl, &_f12, 0);
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
    // Absorb touches so they don't pass through to home screen tiles.
    // Do NOT register a close callback here — the only way to close is the ✕ button.
    lv_obj_add_flag(_detail_panel, LV_OBJ_FLAG_CLICKABLE);

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
    lv_obj_set_style_text_font(_detail_title, &_f22, 0);
    lv_obj_set_style_text_color(_detail_title, C_TEXT, 0);
    lv_obj_align(_detail_title, LV_ALIGN_TOP_MID, 0, 26);

    // ---- Brightness view ----
    _detail_bri_view = lv_obj_create(_detail_panel);
    lv_obj_set_size(_detail_bri_view, TFT_WIDTH, TFT_HEIGHT - 110);
    lv_obj_align(_detail_bri_view, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(_detail_bri_view, 0, 0);
    lv_obj_set_style_border_width(_detail_bri_view, 0, 0);
    lv_obj_set_style_pad_all(_detail_bri_view, 0, 0);
    lv_obj_clear_flag(_detail_bri_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(_detail_bri_view, LV_OBJ_FLAG_CLICKABLE);  // don't intercept button taps

    if (e.supports_brightness) {
        // Colour button above the pill — only if device supports colour.
        // NOT on the pill itself; touching the pill must only adjust brightness.
        int pill_top = 8;
        if (e.supports_color) {
            lv_obj_t* col_btn = lv_btn_create(_detail_bri_view);
            lv_obj_set_size(col_btn, TILE_W, 36);
            lv_obj_align(col_btn, LV_ALIGN_TOP_MID, 0, 8);
            lv_obj_set_style_bg_color(col_btn, C_BG3, 0);
            lv_obj_set_style_bg_opa(col_btn, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(col_btn, 12, 0);
            lv_obj_set_style_border_width(col_btn, 0, 0);
            lv_obj_set_style_shadow_width(col_btn, 0, 0);
            lv_obj_add_event_cb(col_btn, _go_color_cb, LV_EVENT_SHORT_CLICKED, this);
            lv_obj_t* col_lbl = lv_label_create(col_btn);
            lv_label_set_text(col_lbl, LV_SYMBOL_EDIT "  Colour");
            lv_obj_set_style_text_color(col_lbl, C_TEXT, 0);
            lv_obj_set_style_text_font(col_lbl, &_f14, 0);
            lv_obj_align(col_lbl, LV_ALIGN_CENTER, 0, 0);
            pill_top = 52;   // push pill below the button
        }

        // Vertical lv_slider — native widget handles drag in both directions reliably.
        // Width=120, height>width → LVGL treats it as vertical.
        // Knob is made transparent and huge so touching anywhere on the pill works.
        const int PILL_W = 120;
        const int PILL_H = (TFT_HEIGHT - 110) - pill_top - 16;
        _detail_pill_h = PILL_H;

        _detail_bri_pill = lv_slider_create(_detail_bri_view);
        lv_obj_set_size(_detail_bri_pill, PILL_W, PILL_H);
        lv_obj_align(_detail_bri_pill, LV_ALIGN_TOP_MID, 0, pill_top);
        lv_slider_set_range((lv_obj_t*)_detail_bri_pill, 1, 255);
        lv_slider_set_value((lv_obj_t*)_detail_bri_pill, (int)e.brightness, LV_ANIM_OFF);

        // Track (dark pill background)
        lv_obj_set_style_bg_color(_detail_bri_pill, C_BG2, LV_PART_MAIN);
        lv_obj_set_style_radius(_detail_bri_pill, PILL_W / 2, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(_detail_bri_pill, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(_detail_bri_pill, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(_detail_bri_pill, 0, LV_PART_MAIN);

        // Indicator (orange fill from bottom)
        lv_obj_set_style_bg_color(_detail_bri_pill, C_ACCENT, LV_PART_INDICATOR);
        lv_obj_set_style_radius(_detail_bri_pill, PILL_W / 2, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(_detail_bri_pill, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_border_width(_detail_bri_pill, 0, LV_PART_INDICATOR);
        lv_obj_set_style_pad_all(_detail_bri_pill, 0, LV_PART_INDICATOR);

        // Knob: transparent, reasonable size (12px extra each side = easy to grab without
        // leaking hit area outside the slider and stealing taps from nearby buttons)
        lv_obj_set_style_bg_opa(_detail_bri_pill, 0, LV_PART_KNOB);
        lv_obj_set_style_border_opa(_detail_bri_pill, 0, LV_PART_KNOB);
        lv_obj_set_style_shadow_width(_detail_bri_pill, 0, LV_PART_KNOB);
        lv_obj_set_style_pad_all(_detail_bri_pill, 12, LV_PART_KNOB);

        // _detail_bri_fill kept null — lv_slider draws its own indicator
        _detail_bri_fill = nullptr;

        // VALUE_CHANGED = visual only (no HTTP). RELEASED = send to hub.
        lv_obj_add_event_cb(_detail_bri_pill, _bri_slider_cb, LV_EVENT_VALUE_CHANGED, this);
        lv_obj_add_event_cb(_detail_bri_pill, _bri_slider_cb, LV_EVENT_RELEASED,       this);
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
        lv_obj_clear_flag(_detail_col_view, LV_OBJ_FLAG_CLICKABLE);
        if (e.supports_brightness) lv_obj_add_flag(_detail_col_view, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t* back_btn = lv_btn_create(_detail_col_view);
        lv_obj_set_size(back_btn, TILE_W, 36);
        lv_obj_align(back_btn, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_bg_color(back_btn, C_BG3, 0);
        lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(back_btn, 10, 0);
        lv_obj_set_style_shadow_width(back_btn, 0, 0);
        lv_obj_add_event_cb(back_btn, _go_bri_cb, LV_EVENT_SHORT_CLICKED, this);
        lv_obj_t* back_lbl = lv_label_create(back_btn);
        lv_label_set_text(back_lbl, LV_SYMBOL_LEFT "  Brightness");
        lv_obj_set_style_text_color(back_lbl, C_TEXT, 0);
        lv_obj_set_style_text_font(back_lbl, &_f14, 0);
        lv_obj_align(back_lbl, LV_ALIGN_LEFT_MID, 0, 0);

        // Hue pill — same visual style as brightness pill but with rainbow gradient.
        // Back button sits at y=0 (h=36), pill starts at y=44.
        const int HUE_W = 120;
        const int HUE_H = (TFT_HEIGHT - 110) - 44 - 8;   // available height minus margins

        // ── Rainbow background: N colour bands clipped to pill shape ──────────
        lv_obj_t* hue_bg = lv_obj_create(_detail_col_view);
        lv_obj_set_size(hue_bg, HUE_W, HUE_H);
        lv_obj_align(hue_bg, LV_ALIGN_TOP_MID, 0, 44);
        lv_obj_set_style_radius(hue_bg, HUE_W / 2, 0);
        lv_obj_set_style_clip_corner(hue_bg, true, 0);
        lv_obj_set_style_border_width(hue_bg, 0, 0);
        lv_obj_set_style_pad_all(hue_bg, 0, 0);
        lv_obj_set_style_bg_opa(hue_bg, 0, 0);
        lv_obj_clear_flag(hue_bg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        static constexpr int HUE_BANDS = 36;   // 10° per band
        for (int i = 0; i < HUE_BANDS; i++) {
            int y0 = HUE_H *  i      / HUE_BANDS;
            int y1 = HUE_H * (i + 1) / HUE_BANDS + 1;   // +1 fills 1-px rounding gaps
            lv_color_t col = lv_color_hsv_to_rgb((uint16_t)(360 * i / HUE_BANDS), 100, 100);
            lv_obj_t* band = lv_obj_create(hue_bg);
            lv_obj_set_pos(band, 0, y0);
            lv_obj_set_size(band, HUE_W, y1 - y0);
            lv_obj_set_style_bg_color(band, col, 0);
            lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(band, 0, 0);
            lv_obj_set_style_radius(band, 0, 0);
            lv_obj_set_style_pad_all(band, 0, 0);
            lv_obj_clear_flag(band, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        }

        // ── Hue slider: transparent track, white horizontal-bar knob ──────────
        // Sits exactly on top of hue_bg to intercept drag events.
        _detail_colorwheel = lv_slider_create(_detail_col_view);
        lv_obj_set_size(_detail_colorwheel, HUE_W, HUE_H);
        lv_obj_align(_detail_colorwheel, LV_ALIGN_TOP_MID, 0, 44);
        lv_slider_set_range(_detail_colorwheel, 0, 359);
        // Vertical LVGL slider: value=0 at BOTTOM, value=359 at TOP.
        // Rainbow is drawn top=red(0°), bottom=red(360°).
        // Invert so knob position matches the colour underneath it.
        lv_slider_set_value(_detail_colorwheel, 359 - (int)rgb_to_hue(e.r, e.g, e.b), LV_ANIM_OFF);

        // Track — fully transparent (rainbow shows through)
        lv_obj_set_style_bg_opa    (_detail_colorwheel, 0, LV_PART_MAIN);
        lv_obj_set_style_border_opa(_detail_colorwheel, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(_detail_colorwheel, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all   (_detail_colorwheel, 0, LV_PART_MAIN);
        lv_obj_set_style_radius    (_detail_colorwheel, HUE_W / 2, LV_PART_MAIN);

        // Indicator — transparent
        lv_obj_set_style_bg_opa    (_detail_colorwheel, 0, LV_PART_INDICATOR);
        lv_obj_set_style_border_opa(_detail_colorwheel, 0, LV_PART_INDICATOR);
        lv_obj_set_style_pad_all   (_detail_colorwheel, 0, LV_PART_INDICATOR);

        // Knob — white semi-transparent horizontal bar (negative pad shrinks height)
        lv_obj_set_style_bg_color  (_detail_colorwheel, lv_color_white(), LV_PART_KNOB);
        lv_obj_set_style_bg_opa    (_detail_colorwheel, LV_OPA_70,        LV_PART_KNOB);
        lv_obj_set_style_radius    (_detail_colorwheel, 4,                LV_PART_KNOB);
        lv_obj_set_style_border_width(_detail_colorwheel, 0,              LV_PART_KNOB);
        lv_obj_set_style_shadow_width(_detail_colorwheel, 8,              LV_PART_KNOB);
        lv_obj_set_style_shadow_color(_detail_colorwheel, lv_color_black(),LV_PART_KNOB);
        lv_obj_set_style_shadow_opa  (_detail_colorwheel, LV_OPA_40,      LV_PART_KNOB);
        lv_obj_set_style_pad_left  (_detail_colorwheel, 0,   LV_PART_KNOB);  // full width
        lv_obj_set_style_pad_right (_detail_colorwheel, 0,   LV_PART_KNOB);
        lv_obj_set_style_pad_top   (_detail_colorwheel, -50, LV_PART_KNOB);  // thin bar
        lv_obj_set_style_pad_bottom(_detail_colorwheel, -50, LV_PART_KNOB);

        // VALUE_CHANGED fires on every pixel of drag — throttle HTTP to 500 ms.
        // RELEASED sends a final accurate value when the finger lifts.
        lv_obj_add_event_cb(_detail_colorwheel, _color_changed, LV_EVENT_VALUE_CHANGED, this);
        lv_obj_add_event_cb(_detail_colorwheel, _color_changed, LV_EVENT_RELEASED,       this);
    }

    // ---- Toggle button — created LAST so it is highest in z-order ----
    // LVGL hit-tests from last-created to first; the button wins over any view behind it.
    _detail_switch = lv_btn_create(_detail_panel);
    lv_obj_set_size(_detail_switch, TILE_W, 56);
    lv_obj_align(_detail_switch, LV_ALIGN_TOP_MID, 0, 58);
    lv_obj_set_style_radius(_detail_switch, 14, 0);
    lv_obj_set_style_shadow_width(_detail_switch, 0, 0);
    lv_obj_set_style_border_width(_detail_switch, 0, 0);
    lv_obj_set_style_bg_color(_detail_switch, e.is_on() ? C_ACCENT : C_BG3, 0);
    lv_obj_set_style_bg_color(_detail_switch, e.is_on() ? C_ACCENT : C_BG2, LV_STATE_PRESSED);
    lv_obj_t* sw_lbl = lv_label_create(_detail_switch);
    lv_label_set_text(sw_lbl, e.is_on() ? LV_SYMBOL_POWER "  On" : LV_SYMBOL_POWER "  Off");
    lv_obj_set_style_text_color(sw_lbl, e.is_on() ? lv_color_black() : C_TEXT2, 0);
    lv_obj_set_style_text_font(sw_lbl, &lv_font_montserrat_18, 0);
    lv_obj_align(sw_lbl, LV_ALIGN_CENTER, 0, 0);
    // Use RELEASED (not SHORT_CLICKED) — fires unconditionally on finger-up,
    // regardless of how long the press lasted or what else was happening.
    lv_obj_add_event_cb(_detail_switch, _detail_switch_changed, LV_EVENT_RELEASED, this);
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
        bool on = e.is_on();
        lv_obj_set_style_bg_color(_detail_switch, on ? C_ACCENT : C_BG3, 0);
        lv_obj_t* lbl = lv_obj_get_child(_detail_switch, 0);
        if (lbl) {
            lv_label_set_text(lbl, on ? LV_SYMBOL_POWER "  On" : LV_SYMBOL_POWER "  Off");
            lv_obj_set_style_text_color(lbl, on ? lv_color_black() : C_TEXT2, 0);
        }
    }
    // Guard: lv_slider_set_value and lv_colorwheel_set_rgb fire VALUE_CHANGED
    // synchronously, which would re-enter _bri_slider_cb / _color_changed and
    // call set_brightness / set_color → _on_update → infinite loop → watchdog.
    _detail_updating = true;
    if (_detail_bri_pill && e.supports_brightness)
        lv_slider_set_value(_detail_bri_pill, (int)e.brightness, LV_ANIM_OFF);
    if (_detail_colorwheel && e.supports_color)
        lv_slider_set_value(_detail_colorwheel, 359 - (int)rgb_to_hue(e.r, e.g, e.b), LV_ANIM_OFF);
    _detail_updating = false;
}

// -- Event callbacks --

void UI::_tile_clicked(lv_event_t* ev) {
    // Tap → open detail panel; power toggle is the switch inside the panel
    UI* self       = (UI*)lv_event_get_user_data(ev);
    lv_obj_t* tile = lv_event_get_target(ev);
    for (const auto& ref : self->_tiles) {
        if (ref.tile == tile) { self->_show_detail(ref.entity_id); return; }
    }
}

void UI::_tile_long_pressed(lv_event_t* ev) {
    // Unused — kept so the static declaration compiles (no tiles bind this any more)
    (void)ev;
}

void UI::_detail_switch_changed(lv_event_t* ev) {
    // Called on SHORT_CLICKED — just toggle whatever the current state is
    UI* self = (UI*)lv_event_get_user_data(ev);
    if (!self->_dc || self->_detail_entity_id.isEmpty()) return;
    const HAEntity* ep = self->_dc->find_entity(self->_detail_entity_id);
    if (!ep) return;
    if (ep->is_on()) self->_dc->turn_off(self->_detail_entity_id);
    else             self->_dc->turn_on(self->_detail_entity_id);
}

void UI::_bri_slider_cb(lv_event_t* ev) {
    UI* self = (UI*)lv_event_get_user_data(ev);
    if (self->_detail_updating) return;  // programmatic update — don't echo back
    if (!self->_detail_bri_pill || !self->_dc || self->_detail_entity_id.isEmpty()) return;

    int val = lv_slider_get_value(self->_detail_bri_pill);
    uint8_t bri = (uint8_t)constrain(val, 1, 255);

    bool released = (lv_event_get_code(ev) == LV_EVENT_RELEASED);
    uint32_t now  = millis();
    // Send on finger release OR at most every 300 ms while dragging
    if (released || now - self->_bri_last_send_ms >= 300) {
        self->_bri_last_send_ms = now;
        self->_dc->set_brightness(self->_detail_entity_id, bri);
    }
}

void UI::_color_changed(lv_event_t* ev) {
    UI* self = (UI*)lv_event_get_user_data(ev);
    if (self->_detail_updating) return;  // programmatic update — don't echo back
    if (!self->_detail_colorwheel || !self->_dc || self->_detail_entity_id.isEmpty()) return;

    // Vertical slider: value=0 bottom, value=359 top; rainbow is drawn top=red(0°).
    // Invert so slider position matches the hue displayed under the knob.
    float hue = 359.0f - (float)lv_slider_get_value(self->_detail_colorwheel);  // 0–359

    bool released = (lv_event_get_code(ev) == LV_EVENT_RELEASED);
    uint32_t now  = millis();
    if (released || now - self->_col_last_send_ms >= 500) {
        self->_col_last_send_ms = now;
        self->_dc->set_color_hs(self->_detail_entity_id, hue, 1.0f);
    }
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
    if (self->_detail_switch)   lv_obj_add_flag(self->_detail_switch, LV_OBJ_FLAG_HIDDEN);
}

void UI::_go_bri_cb(lv_event_t* ev) {
    UI* self = (UI*)lv_event_get_user_data(ev);
    if (self->_detail_col_view) lv_obj_add_flag(self->_detail_col_view, LV_OBJ_FLAG_HIDDEN);
    if (self->_detail_bri_view) lv_obj_clear_flag(self->_detail_bri_view, LV_OBJ_FLAG_HIDDEN);
    if (self->_detail_switch)   lv_obj_clear_flag(self->_detail_switch, LV_OBJ_FLAG_HIDDEN);
}

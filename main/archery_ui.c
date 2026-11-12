// main/archery_ui.c —— 射箭计分器界面。
//
// 设计取向：深绿靶场底色 + 蓝/红/金靶环配色，一屏一件事；顶部标题、底部提示，
// 数字用 Montserrat、中文用应用自带 Noto Sans SC 子集。界面按"用户视角"布局，
// BSP 的 BSP_LCD_ROTATE_180 负责抵消屏幕倒装，所以这里不需要再做翻转。
//
// 本文件只做渲染，不保存状态：所有页面都由 archery_flow_t + archery_history_t
// 推导，按键处理在 archery_flow.c，存储/计时在 archery_app.c。
#include "archery_ui.h"

#include "archery_fonts.h"
#include "archery_layout.h"
#include "archery_logbuf.h"
#include "archery_name.h"
#include "archery_wifi.h"
#include "archery_wifi_store.h"
#include "esp_bt.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "archery_qr.h"
#include "archery_version.h"
#include "archery_voice.h"
#include "archery_ws_client.h"
#include "archery_wifi_view.h"
#include "bsp_pins.h"
#include "bsp_display.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

// 靶场配色。
#define C_BG        0x0C1512
#define C_CARD      0x16241D
#define C_CARD_SEL  0x24402F
#define C_LINE      0x2C4436
#define C_TEXT      0xF2F6F1
#define C_MUTED     0x93AB9C
#define C_GOLD      0xE9C46A
#define C_RED       0xD1495B
#define C_BLUE      0x3E7CB1
#define C_GREEN     0x63C08A

static lv_obj_t *s_screen;
static lv_obj_t *s_timer_title;
static lv_obj_t *s_timer_clock;
static lv_obj_t *s_clock_label;
static bool s_title_visible;
static lv_obj_t *s_ruler_items[160];
static uint8_t s_ruler_count;
static lv_obj_t *s_timer_hint;
static const archery_wifi_view_t *s_wifi_view;

// SSID 可能包含任意中文，用 LVGL 自带的 CJK 字体覆盖，避免缺字方块。
static const lv_font_t *ssid_font(void) {
#if LV_FONT_SOURCE_HAN_SANS_SC_16_CJK
    return &lv_font_source_han_sans_sc_16_cjk;
#else
    return ARCHERY_FONT_BODY;
#endif
}
static const char *s_voice_status;

// ---------------------------------------------------------------------------
// 基础控件
// ---------------------------------------------------------------------------

static lv_obj_t *base_screen(void) {
    s_title_visible = false;
    s_ruler_count = 0;
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, BSP_LCD_W, BSP_LCD_H);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_radius(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    archery_font_audit(font, text);
    return label;
}

static lv_obj_t *centered_label(lv_obj_t *scr, const char *text,
                                const lv_font_t *font, uint32_t color, int y) {
    lv_obj_t *label = make_label(scr, text, font, color);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

static lv_obj_t *make_card(lv_obj_t *parent, archery_rect_t rect,
                           bool selected, bool danger) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, rect.x, rect.y);
    lv_obj_set_size(card, rect.w, rect.h);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(selected ? C_CARD_SEL : C_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, selected ? 2 : 1, 0);
    lv_obj_set_style_border_color(card,
        lv_color_hex(danger ? C_RED : (selected ? C_GOLD : C_LINE)), 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

// 顶部右侧电量：只有一个小电池壳 + 填充条；读数不可用时保持空心。
static void add_battery(lv_obj_t *scr, int soc) {
    lv_obj_t *shell = lv_obj_create(scr);
    lv_obj_set_pos(shell, 202, 4);
    lv_obj_set_size(shell, 24, 11);
    lv_obj_set_style_radius(shell, 3, 0);
    lv_obj_set_style_bg_opa(shell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(shell, 1, 0);
    lv_obj_set_style_border_color(shell, lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_pad_all(shell, 0, 0);
    lv_obj_remove_flag(shell, LV_OBJ_FLAG_SCROLLABLE);
    if (soc < 0) return;

    int fill_w = (soc * 18) / 100;
    if (fill_w < 1) fill_w = 1;
    const uint32_t color = soc < 20 ? C_RED : (soc < 50 ? C_GOLD : C_GREEN);
    lv_obj_t *fill = lv_obj_create(scr);
    lv_obj_set_pos(fill, 205, 7);
    lv_obj_set_size(fill, fill_w, 5);
    lv_obj_set_style_radius(fill, 2, 0);
    lv_obj_set_style_bg_color(fill, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(fill, 0, 0);
    lv_obj_set_style_pad_all(fill, 0, 0);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
}

// 已联网时电池下方四段短横线，点亮段数表示信号强弱。
static void add_signal_bars(lv_obj_t *scr, uint32_t color, uint8_t bars) {
    for (uint8_t i = 0; i < 4; i++) {
        lv_obj_t *bar = lv_obj_create(scr);
        lv_obj_set_pos(bar, 202 + i * 6, 17);
        lv_obj_set_size(bar, 5, 3);
        lv_obj_set_style_radius(bar, 1, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        if (bars > 0 && i >= bars) lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
    }
}

// 顶部状态栏：左侧时间；电池与下方图形表示电量与网络状态。
static void add_status_bar(lv_obj_t *scr, const archery_flow_t *flow,
                           const archery_wifi_view_t *view) {
    char time_text[8] = "--:--";
    if (flow->clock_set) {
        archery_datetime_t dt;
        archery_seconds_to_datetime(flow->clock_now, &dt);
        snprintf(time_text, sizeof(time_text), "%02u:%02u", (unsigned)dt.hour,
                 (unsigned)dt.minute);
    }
    lv_obj_t *clock = make_label(scr, time_text, ARCHERY_FONT_BODY, C_MUTED);
    s_clock_label = clock;
    if (s_title_visible) {
        lv_obj_align(clock, LV_ALIGN_TOP_LEFT, 8, 4);
    } else {
        lv_obj_align(clock, LV_ALIGN_TOP_RIGHT, -42, 4);
    }

    // 只有联网时才画信号格；未联网/配网中留空。
    if (view && view->sta_connected) {
        const int8_t rssi = view->sta_rssi;
        const uint8_t bars = rssi >= -55 ? 4 : (rssi >= -65 ? 3 : (rssi >= -75 ? 2 : 1));
        add_signal_bars(scr, C_GREEN, bars);
    }
}

static void add_title(lv_obj_t *scr, const char *text) {
    s_title_visible = true;
    centered_label(scr, text, ARCHERY_FONT_TITLE, C_TEXT, 8);
}

static void add_hint(lv_obj_t *scr, const char *text, int y) {
    centered_label(scr, text, ARCHERY_FONT_BODY, C_MUTED, y);
}

// 首页/完成页的靶环图案：蓝(6-7) - 红(8-9) - 金(10) - 靶心。
static void add_target(lv_obj_t *scr, int center_x, int center_y) {
    static const struct { int diameter; uint32_t color; } rings[] = {
        {104, C_BLUE}, {74, C_RED}, {44, C_GOLD}, {16, C_TEXT},
    };
    for (size_t i = 0; i < sizeof(rings) / sizeof(rings[0]); i++) {
        lv_obj_t *dot = lv_obj_create(scr);
        lv_obj_set_size(dot, rings[i].diameter, rings[i].diameter);
        lv_obj_set_pos(dot, center_x - rings[i].diameter / 2,
                       center_y - rings[i].diameter / 2);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(rings[i].color), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    }
}

// 一排小圆点表示"第几组 / 共几组"进度。
static void add_group_dots(lv_obj_t *scr, uint8_t groups, uint8_t done, int y) {
    const int gap = 12;
    const int total = (int)groups * gap - (gap - 8);
    const int start_x = (BSP_LCD_W - total) / 2;
    for (uint8_t i = 0; i < groups; i++) {
        lv_obj_t *dot = lv_obj_create(scr);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_pos(dot, start_x + i * gap, y);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot,
            lv_color_hex(i < done ? C_GOLD : C_LINE), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    }
}

// 环值槽位的界面文字：X/10..0 用数字；0 环在所有靶型上都显示为"0"。
static const char *target_name(uint8_t target) {
    switch (target) {
    case ARCHERY_TARGET_TRIPLE_40H: return "三联靶";
    case ARCHERY_TARGET_FULL_40: return "40 全靶";
    case ARCHERY_TARGET_FULL_80: return "80 全靶";
    case ARCHERY_TARGET_FULL_122: return "122 全靶";
    default: return "40 半靶";
    }
}

static const char *target_ring_range(uint8_t target) {
    if (target == ARCHERY_TARGET_HALF_40 || target == ARCHERY_TARGET_TRIPLE_40H) {
        return "6-10 环";
    }
    return "0-10 环";
}

static const char *slot_label(uint8_t target, uint8_t slot) {
    const uint8_t code = archery_target_slot_code(target, slot);
    if (code == ARCHERY_SCORE_MISS) return "0";
    return archery_score_text(code);
}

static void format_duration(char *buffer, size_t size, uint16_t seconds) {
    snprintf(buffer, size, "%u:%02u", (unsigned)(seconds / 60u),
             (unsigned)(seconds % 60u));
}

// "9.5" 形式的平均环数文本。
static void format_average(char *buffer, size_t size, uint16_t average_x10) {
    snprintf(buffer, size, "%u.%u", (unsigned)(average_x10 / 10u),
             (unsigned)(average_x10 % 10u));
}

// ---------------------------------------------------------------------------
// 各页面
// ---------------------------------------------------------------------------

static void render_home(lv_obj_t *scr, const archery_flow_t *flow) {
    add_target(scr, 120, 70);
    const char *device_name = archery_name_get();
    const lv_font_t *name_font =
        strcmp(device_name, ARCHERY_NAME_DEFAULT) == 0 ? ARCHERY_FONT_TITLE : ssid_font();
    centered_label(scr, device_name, name_font, C_TEXT, 126);
    char range[48];
    snprintf(range, sizeof(range), "%s · %s", target_name(flow->settings.target),
             target_ring_range(flow->settings.target));
    centered_label(scr, range, ARCHERY_FONT_BODY, C_MUTED, 156);

    static const char *items[ARCHERY_HOME_ITEMS] = {"开始训练", "历史记录", "日期时间",
                                                    "射箭工具", "设置", "联网"};
    for (uint8_t i = 0; i < ARCHERY_HOME_ITEMS; i++) {
        const archery_rect_t rect = {32, (int16_t)(178 + i * 20), 176, 20};
        const bool selected = flow->home_select == i;
        lv_obj_t *card = make_card(scr, rect, selected, false);
        lv_obj_t *label = make_label(card, items[i], ARCHERY_FONT_BODY,
                                     selected ? C_GOLD : C_TEXT);
        lv_obj_center(label);
    }
    add_hint(scr, "上/下 选择 · 确定 进入", 300);
}

static void ruler_register(lv_obj_t *obj) {
    if (obj && s_ruler_count < (uint8_t)(sizeof(s_ruler_items) / sizeof(s_ruler_items[0]))) {
        s_ruler_items[s_ruler_count++] = obj;
    }
}

static void ruler_rect(lv_obj_t *scr, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *line = lv_obj_create(scr);
    ruler_register(line);
    lv_obj_set_pos(line, x, y);
    lv_obj_set_size(line, w, h);
    lv_obj_set_style_bg_color(line, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
}

static void render_tools(lv_obj_t *scr, const archery_flow_t *flow) {
    add_title(scr, "射箭工具");
    static const char *names[] = {"瞄具刻度", "刻度估算", "节拍器", "测距", "参考尺"};
    for (uint8_t row = 0; row < 5; row++) {
        const archery_rect_t rect = {20, (int16_t)(66 + row * 38), 200, 34};
        const bool selected = flow->tools_row == row;
        lv_obj_t *card = make_card(scr, rect, selected, false);
        lv_obj_t *name = make_label(card, names[row], ARCHERY_FONT_BODY,
                                    selected ? C_GOLD : C_TEXT);
        lv_obj_center(name);
    }
    add_hint(scr, "上/下 选择 · 确定 进入", 270);
    add_hint(scr, "长按确定 返回主页", 294);
}

static void render_ruler(lv_obj_t *scr, const archery_flow_t *flow) {
    (void)flow;
    // 屏幕有效高度 320 px 对应 40 mm：8 px/mm。
    s_ruler_count = 0;
    // 屏幕有效高度 320 px 对应 40 mm，固定 8 px/mm。
    const int pmm = 8;
    const int ruler_px = 320;
    const int vshift = (flow ? flow->ruler_offset : 0) * pmm;
    const int metric_x = 5;     // 外壳遮挡约 1～2px，内缩到可见区
    const int imperial_x = 234; // 右侧同样内缩
    const int axis_top = -15 * pmm + vshift;
    ruler_rect(scr, metric_x, axis_top, 1, 70 * pmm, C_GOLD);
    ruler_rect(scr, imperial_x, axis_top, 1, 70 * pmm, C_GOLD);
    char text[12];
    // 连续创建 -15～55 mm 的刻度；移动范围 ±15 mm，50 mm 可进入视野。
    for (int mm = -15; mm <= 55; mm++) {
        const int y = mm * pmm + vshift;
        int len = 6;
        if (mm % 10 == 0) len = 16;
        else if (mm % 5 == 0) len = 10;
        ruler_rect(scr, metric_x, y, len, 1, C_TEXT);
        if (mm >= 0 && mm <= 50 && mm % 10 == 0) {
            snprintf(text, sizeof(text), "%d", mm);
            lv_obj_t *label = make_label(scr, text, ARCHERY_FONT_BODY, C_TEXT);
            ruler_register(label);
            const int ly = y - 9;
            lv_obj_set_pos(label, metric_x + len + 4, ly);
        }
    }
    // 英制：1 in = 25.4 mm，0.1 in = 20.32 px；覆盖约 -0.4～2.0 in。
    for (int tenth = -4; tenth <= 20; tenth++) {
        const int y = (int)(tenth * 20.32f + 0.5f) + vshift;
        const int len = (tenth % 5 == 0) ? 16 : 6;
        ruler_rect(scr, imperial_x - len, y, len, 1, C_TEXT);
        if (tenth >= 0 && tenth % 5 == 0) {
            if (tenth == 0) snprintf(text, sizeof(text), "0");
            else if (tenth == 5) snprintf(text, sizeof(text), "0.5");
            else if (tenth == 10) snprintf(text, sizeof(text), "1");
            else if (tenth == 15) snprintf(text, sizeof(text), "1.5");
            else snprintf(text, sizeof(text), "2");
            lv_obj_t *label = make_label(scr, text, ARCHERY_FONT_BODY, C_TEXT);
            ruler_register(label);
            const int ly = y - 9;
            lv_obj_set_pos(label, imperial_x - len - 34, ly);
        }
    }
    // mm/in 固定在屏幕顶角，不随刻度平移。
    lv_obj_t *mm_title = make_label(scr, "mm", ARCHERY_FONT_BODY, C_GOLD);
    lv_obj_set_pos(mm_title, 52, 2);
    lv_obj_t *in_title = make_label(scr, "in", ARCHERY_FONT_BODY, C_GOLD);
    lv_obj_set_pos(in_title, 158, 2);
}

static void render_sight_calc(lv_obj_t *scr, const archery_flow_t *flow) {
    add_title(scr, "刻度估算");
    char line[96];
    snprintf(line, sizeof(line), "目标距离 %u m", (unsigned)flow->calc_distance);
    centered_label(scr, line, ARCHERY_FONT_TITLE, C_GOLD, 76);
    centered_label(scr,
                   flow->calc_mode == ARCHERY_SIGHT_LINEAR ? "算法：线性" : "算法：抛物线（三点）",
                   ARCHERY_FONT_BODY, C_MUTED, 112);
    int16_t mark = 0;
    if (archery_sight_estimate(&flow->sight, (uint16_t)(flow->calc_distance * 10u),
                               (archery_sight_fit_t)flow->calc_mode, &mark)) {
        char value[24];
        char suffix[24] = "";
        uint16_t first = 0, last = 0;
        bool have = false;
        for (uint8_t i = 0; i < flow->sight.count; i++) {
            if (flow->sight.items[i].mark_tenths == 0) continue;
            const uint16_t d = flow->sight.items[i].distance_tenths;
            if (!have || d < first) first = d;
            if (!have || d > last) last = d;
            have = true;
        }
        if (have && (flow->calc_distance * 10u < first || flow->calc_distance * 10u > last)) {
            snprintf(suffix, sizeof(suffix), "（外推）");
        }
        archery_sight_format(mark, value, sizeof(value));
        snprintf(line, sizeof(line), "估算刻度 %s%s", value, suffix);
        centered_label(scr, line, ARCHERY_FONT_TITLE, C_TEXT, 156);
    } else {
        centered_label(scr, "超出范围，需至少两个已知刻度",
                       ARCHERY_FONT_BODY, C_MUTED, 156);
    }
    add_hint(scr, "上/下 调距离 · 确定 换算法", 216);
    add_hint(scr, "长按确定 返回工具", 244);
    centered_label(scr, "刻度估值仅供参考", ARCHERY_FONT_BODY, C_GOLD, 282);
}

static void render_metronome(lv_obj_t *scr, const archery_flow_t *flow) {
    add_title(scr, "节拍器");
    char line[48];
    snprintf(line, sizeof(line), "%u BPM", (unsigned)flow->metro_bpm);
    centered_label(scr, line, ARCHERY_FONT_TITLE, C_GOLD, 38);
    if (flow->metro_running) {
        if (flow->metro_beat == 0) {
            centered_label(scr, "预备", ARCHERY_FONT_BODY, C_MUTED, 64);
        } else {
            snprintf(line, sizeof(line), "第 %u / %u 拍", (unsigned)flow->metro_beat,
                     (unsigned)flow->metro_accent);
            centered_label(scr, line, ARCHERY_FONT_BODY, C_TEXT, 64);
        }
    } else {
        centered_label(scr, "已停止", ARCHERY_FONT_BODY, C_MUTED, 64);
    }

    static const char *names[] = {"开始/停止", "BPM", "重音", "预备拍", "音量", "自动停止", "预设"};
    char value[24];
    const uint8_t visible = 7;
    const uint8_t offset = 0;  // 7 行完整显示，无需滚动
    for (uint8_t i = 0; i < visible; i++) {
        const uint8_t row = (uint8_t)(offset + i);
        const archery_rect_t rect = {20, (int16_t)(84 + i * 28), 200, 26};
        const bool selected = flow->metro_row == row;
        const bool editing = selected && flow->metro_edit && row != 0u;
        lv_obj_t *card = make_card(scr, rect, selected, false);
        if (editing) {
            lv_obj_set_style_border_width(card, 3, 0);
            lv_obj_set_style_border_color(card, lv_color_hex(C_GOLD), 0);
        }
        lv_obj_t *name = make_label(card, names[row], ARCHERY_FONT_BODY,
                                    selected ? C_GOLD : C_TEXT);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 10, 0);
        switch (row) {
        case 0:
            snprintf(value, sizeof(value), "%s", flow->metro_running ? "停止" : "开始");
            break;
        case 1: snprintf(value, sizeof(value), "%u", (unsigned)flow->metro_bpm); break;
        case 2: snprintf(value, sizeof(value), "每 %u 拍", (unsigned)flow->metro_accent); break;
        case 3: snprintf(value, sizeof(value), "%u 拍", (unsigned)flow->metro_countin); break;
        case 4: snprintf(value, sizeof(value), "%u%%", (unsigned)(flow->settings.beep_volume * 20u)); break;
        case 5:
            if (flow->metro_stop_min == 0) snprintf(value, sizeof(value), "关");
            else snprintf(value, sizeof(value), "%u 分钟", (unsigned)flow->metro_stop_min);
            break;
        default: {
            static const char *presets[] = {"自定义", "慢速 40", "基础 60", "快速 90"};
            snprintf(value, sizeof(value), "%s", presets[flow->metro_preset]);
            break;
        }
        }
        lv_obj_t *val = make_label(card, value,
                                   editing ? ARCHERY_FONT_TITLE : ARCHERY_FONT_BODY,
                                   selected ? C_GOLD : C_TEXT);
        lv_obj_align(val, LV_ALIGN_RIGHT_MID, -10, 0);
    }
    add_hint(scr, flow->metro_edit ? "上/下 调值 · 确定 完成"
                                   : "上/下 选择 · 长按确定 退出", 286);
}

static void rangefinder_rect(lv_obj_t *scr, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *line = lv_obj_create(scr);
    lv_obj_set_pos(line, x, y);
    lv_obj_set_size(line, w, h);
    lv_obj_set_style_bg_color(line, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
}

static void render_range_intro(lv_obj_t *scr, const archery_flow_t *flow) {
    (void)flow;
    add_title(scr, "测距");
    centered_label(scr, "使用方法", ARCHERY_FONT_BODY, C_MUTED, 38);
    centered_label(scr, "1 单手握住设备", ARCHERY_FONT_BODY, C_TEXT, 68);
    centered_label(scr, "2 手臂伸直", ARCHERY_FONT_BODY, C_TEXT, 96);
    centered_label(scr, "3 屏幕垂直正对眼睛", ARCHERY_FONT_BODY, C_TEXT, 124);
    centered_label(scr, "与眼同高", ARCHERY_FONT_BODY, C_TEXT, 152);
    centered_label(scr, "4 0线对靶面下沿", ARCHERY_FONT_BODY, C_TEXT, 184);
    centered_label(scr, "5 游标卡住上沿", ARCHERY_FONT_BODY, C_TEXT, 212);
    centered_label(scr, "眼距≈身高×0.35", ARCHERY_FONT_BODY, C_MUTED, 246);
    add_hint(scr, "确定 参数 · 长按返回", 284);
}

static void render_range_config(lv_obj_t *scr, const archery_flow_t *flow) {
    add_title(scr, "测距参数");
    static const char *target_names[] = {"122全", "80全", "60全", "40全", "40半"};
    static const char *names[] = {"靶面", "眼到屏幕", "开始测距"};
    char line[32];
    for (uint8_t row = 0; row < 3; row++) {
        const archery_rect_t rect = {20, (int16_t)(96 + row * 54), 200, 40};
        const bool selected = flow->range_row == row;
        const bool editing = selected && flow->range_edit;
        lv_obj_t *card = make_card(scr, rect, selected, false);
        if (editing) {
            lv_obj_set_style_border_width(card, 3, 0);
            lv_obj_set_style_border_color(card, lv_color_hex(C_GOLD), 0);
        }
        lv_obj_t *name = make_label(card, names[row], ARCHERY_FONT_BODY,
                                    selected ? C_GOLD : C_TEXT);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 10, 0);
        switch (row) {
        case 0: snprintf(line, sizeof(line), "%s", target_names[flow->range_target]); break;
        case 1: snprintf(line, sizeof(line), "%u cm", (unsigned)flow->range_eye_cm); break;
        default: snprintf(line, sizeof(line), "%s", "进入"); break;
        }
        lv_obj_t *val = make_label(card, line,
                                   editing ? ARCHERY_FONT_TITLE : ARCHERY_FONT_BODY,
                                   selected ? C_GOLD : C_TEXT);
        lv_obj_align(val, LV_ALIGN_RIGHT_MID, -10, 0);
    }
    add_hint(scr, "上/下 选择 · 确定 编辑", 264);
    add_hint(scr, "长按确定 返回工具", 292);
}

static void render_range_view(lv_obj_t *scr, const archery_flow_t *flow) {
    add_title(scr, "测距刻度");
    static const uint16_t full_mm[] = {1220, 800, 600, 400, 400};
    static const char *target_names[] = {"122全", "80全", "60全", "40全", "40半"};
    const uint8_t target = flow->range_target < 5u ? flow->range_target : 0u;
    const uint16_t size_mm = full_mm[target];
    const int bottom = 286;
    const int axis_x = 232;
    const uint32_t distance = flow->range_distance_m ? flow->range_distance_m : 30u;
    // 0 线对靶面下沿，游标卡上沿：按完整靶面高度计算。
    const int height = (int)((uint32_t)flow->range_eye_cm * 10u * size_mm * 8u /
                             (distance * 1000u));
    int cursor = bottom - height;
    if (cursor < 12) cursor = 12;

    char line[64];
    snprintf(line, sizeof(line), "%u m  %s", (unsigned)distance,
             target_names[target]);
    centered_label(scr, line, ARCHERY_FONT_TITLE, C_GOLD, 34);

    // 0 线在底部，距离刻度向上增长。
    rangefinder_rect(scr, 140, bottom, axis_x - 140 + 1, 1, C_GOLD);
    rangefinder_rect(scr, axis_x, 0, 1, bottom + 1, C_GOLD);
    lv_obj_t *zero = make_label(scr, "0", ARCHERY_FONT_BODY, C_MUTED);
    lv_obj_set_pos(zero, axis_x - 26, bottom - 18);

    // 10 m 参考刻度。
    for (int d = 20; d <= 70; d += 10) {
        const int h = (int)((uint32_t)flow->range_eye_cm * 10u * size_mm * 8u /
                            ((uint32_t)d * 1000u));
        const int y = bottom - h;
        if (y < 10 || y > bottom - 4) continue;
        rangefinder_rect(scr, axis_x - 8, y, 8, 1, C_MUTED);
        snprintf(line, sizeof(line), "%d", d);
        lv_obj_t *label = make_label(scr, line, ARCHERY_FONT_BODY, C_MUTED);
        lv_obj_set_pos(label, axis_x - 40, y - 9);
    }

    // 游标：对准靶子上边缘。
    rangefinder_rect(scr, 140, cursor, axis_x - 140 + 1, 2, C_GOLD);
    snprintf(line, sizeof(line), "%u m", (unsigned)distance);
    lv_obj_t *value = make_label(scr, line, ARCHERY_FONT_BODY, C_GOLD);
    lv_obj_set_pos(value, axis_x - 70, cursor - 20);
    if (height > bottom - 12) {
        centered_label(scr, "靶面超出屏幕，请加大距离",
                       ARCHERY_FONT_BODY, C_MUTED, 60);
    }
    add_hint(scr, "0线对下沿 · 游标卡上沿", 290);
}

static void render_sight(lv_obj_t *scr, const archery_flow_t *flow) {
    add_title(scr, "瞄具刻度");
    const uint8_t count = flow->sight.count;
    for (uint8_t i = 0; i < count; i++) {
        const archery_rect_t rect = {20, (int16_t)(50 + i * 28), 200, 26};
        const bool selected = flow->sight_row == i;
        const bool editing = selected && flow->sight_edit_field != 0;
        lv_obj_t *card = make_card(scr, rect, selected, false);
        const bool configured = flow->sight.items[i].distance_tenths != 0 ||
                                flow->sight.items[i].mark_tenths != 0;
        if (configured) {
            const bool valid = archery_sight_entry_valid(&flow->sight, i);
            lv_obj_set_style_border_color(card, lv_color_hex(valid ? C_BLUE : C_RED), 0);
            lv_obj_set_style_border_width(card, editing ? 3 : 2, 0);
        } else if (editing) {
            lv_obj_set_style_border_width(card, 3, 0);
            lv_obj_set_style_border_color(card, lv_color_hex(C_GOLD), 0);
        }
        char distance[16];
        const uint16_t dt = flow->sight.items[i].distance_tenths;
        if (dt == 0) {
            snprintf(distance, sizeof(distance), "-- m");
        } else if (dt % 10u == 0) {
            snprintf(distance, sizeof(distance), "%u m", (unsigned)(dt / 10u));
        } else {
            snprintf(distance, sizeof(distance), "%u.%u m",
                     (unsigned)(dt / 10u), (unsigned)(dt % 10u));
        }
        const bool edit_distance = flow->sight_edit_field == 2;
        lv_obj_t *name = make_label(card, distance,
                                    (selected && edit_distance) ? ARCHERY_FONT_TITLE
                                                                : ARCHERY_FONT_BODY,
                                    selected ? C_GOLD : C_TEXT);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 12, 0);
        char mark[16];
        const bool unset = flow->sight.items[i].mark_tenths == 0;
        if (unset) snprintf(mark, sizeof(mark), "--");
        else archery_sight_format(flow->sight.items[i].mark_tenths, mark, sizeof(mark));
        const bool edit_mark = flow->sight_edit_field == 1;
        lv_obj_t *value = make_label(card, mark,
                                     (selected && edit_mark) ? ARCHERY_FONT_TITLE
                                                             : ARCHERY_FONT_BODY,
                                     selected ? C_GOLD : C_TEXT);
        lv_obj_align(value, LV_ALIGN_RIGHT_MID, -12, 0);
    }
    if (flow->sight_edit_field == 2) {
        add_hint(scr, "上/下 调距离 · 确定 下一步", 226);
    } else if (flow->sight_edit_field == 1) {
        add_hint(scr, "上/下 调刻度 · 确定 下一步", 226);
    } else {
        add_hint(scr, "上/下 选行 · 确定 编辑", 226);
    }
    add_hint(scr, flow->sight_edit_field != 0 ? "长按确定 清除当前条目"
                                               : "长按确定 保存返回", 254);
}

static void render_setup(lv_obj_t *scr, const archery_flow_t *flow) {
    const bool faces = archery_flow_target_has_faces(flow->settings.target);
    add_title(scr, "新训练");
    centered_label(scr, faces ? "先定组数与上/中/下箭数" : "先定组数与每组箭数",
                   ARCHERY_FONT_BODY, C_MUTED, 40);

    static const char *single_names[] = {"组数", "每组箭数", "开始记录"};
    static const char *face_names[] = {"组数", "每组箭数",
                                       "上靶箭数", "中靶箭数", "下靶箭数", "开始记录"};
    const uint8_t rows = faces ? 6u : 3u;
    const int16_t y0 = faces ? 44 : 78;
    const int16_t stride = faces ? 34 : 46;
    const int16_t height = faces ? 30 : 40;

    char value[20];
    for (uint8_t row = 0; row < rows; row++) {
        const archery_rect_t rect = {20, (int16_t)(y0 + row * stride), 200, height};
        const bool selected = flow->setup_row == row;
        const bool is_start = row + 1u == rows;
        lv_obj_t *card = make_card(scr, rect, selected, false);
        lv_obj_t *name = make_label(card, faces ? face_names[row] : single_names[row],
                                    ARCHERY_FONT_BODY, selected ? C_GOLD : C_TEXT);
        if (is_start && flow->settings.timer_seconds > 0u) {
            lv_label_set_text(name, "开始");
        }
        if (!is_start) {
            lv_obj_align(name, LV_ALIGN_LEFT_MID, 12, 0);
            if (row == 0) {
                snprintf(value, sizeof(value), "%u", (unsigned)flow->setup_groups);
            } else if (row == 1) {
                snprintf(value, sizeof(value), "%u", (unsigned)flow->setup_arrows);
            } else {
                snprintf(value, sizeof(value), "%u",
                         (unsigned)flow->setup_face[row - 2u]);
            }
            lv_obj_t *number = make_label(card, value, ARCHERY_FONT_TITLE,
                                          selected ? C_GOLD : C_TEXT);
            lv_obj_align(number, LV_ALIGN_RIGHT_MID, -14, 0);
        } else if (faces) {
            // 三联靶：上/中/下合计必须等于每组箭数，否则不允许开始。
            lv_obj_align(name, LV_ALIGN_LEFT_MID, 12, 0);
            const uint8_t total = archery_flow_setup_faces_total(flow);
            const bool ready = total == flow->setup_arrows;
            snprintf(value, sizeof(value), "%u/%u 箭", (unsigned)total,
                     (unsigned)flow->setup_arrows);
            lv_obj_t *number = make_label(card, value, ARCHERY_FONT_BODY,
                                          ready ? (selected ? C_GOLD : C_TEXT) : C_RED);
            lv_obj_align(number, LV_ALIGN_RIGHT_MID, -12, 0);
        } else {
            lv_obj_center(name);
        }
    }
    add_hint(scr, "上/下 调整 · 确定 下一项", faces ? 250 : 262);
    add_hint(scr, "长按 上一项 · 首行返回", faces ? 276 : 288);
}

// 本组按"每个环值几根箭"录入：环值槽位两列排布，最后一行是提交行。
// 三联靶按上/中/下三个靶面分别录入，每次只显示当前靶面。
static void render_entry(lv_obj_t *scr, const archery_flow_t *flow) {
    const archery_session_t *session = &flow->session;
    const bool faces = archery_flow_target_has_faces(session->target);
    const uint8_t face = faces ? flow->entry_face : 0;
    const uint8_t slots = archery_target_slot_count(session->target);
    // 关闭"记录内十"时隐藏 X 行：录入页只显示从 10 环开始的槽位。
    const uint8_t first = archery_target_entry_first_slot(session->target,
                                                          flow->settings.record_x);
    const uint8_t visible = (uint8_t)(slots - first);
    const uint8_t face_arrows = faces ? flow->setup_face[face] : session->arrows;
    static const char *face_names[ARCHERY_FACES] = {"上靶", "中靶", "下靶"};

    char title[44];
    if (faces) {
        snprintf(title, sizeof(title), "第 %u/%u 组 · %s",
                 (unsigned)(session->group + 1u), (unsigned)session->groups,
                 face_names[face]);
    } else {
        snprintf(title, sizeof(title), "第 %u/%u 组",
                 (unsigned)(session->group + 1u), (unsigned)session->groups);
    }
    add_title(scr, title);

    const uint8_t *counts = flow->counts[face];
    for (uint8_t i = 0; i < visible; i++) {
        const uint8_t slot = (uint8_t)(first + i);
        const uint8_t row = (uint8_t)(i / 2u);
        const uint8_t col = (uint8_t)(i % 2u);
        const archery_rect_t rect = {(int16_t)(14 + col * 110),
                                     (int16_t)(40 + row * 30), 102, 28};
        const bool selected = flow->entry_row == i;
        lv_obj_t *cell = make_card(scr, rect, selected, false);
        lv_obj_t *name = make_label(cell, slot_label(session->target, slot),
                                    ARCHERY_FONT_BODY, selected ? C_GOLD : C_TEXT);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 10, 0);
        char count_text[8];
        snprintf(count_text, sizeof(count_text), "%u", (unsigned)counts[slot]);
        lv_obj_t *count = make_label(cell, count_text, ARCHERY_FONT_NUMBER,
                                     selected ? C_GOLD : C_TEXT);
        lv_obj_align(count, LV_ALIGN_RIGHT_MID, -10, 0);
    }

    const uint8_t slot_rows = (uint8_t)((visible + 1u) / 2u);
    const archery_rect_t commit = {14, (int16_t)(40 + slot_rows * 30), 212, 28};
    const bool commit_selected = flow->entry_row == visible;
    lv_obj_t *card = make_card(scr, commit, commit_selected, false);
    const uint8_t total = archery_counts_total(counts);
    const bool ready = total == face_arrows;
    char text[56];
    if (!ready) {
        snprintf(text, sizeof(text), "还差 %u 箭（共 %u）",
                 (unsigned)(face_arrows - total), (unsigned)face_arrows);
    } else if (faces && face + 1u < ARCHERY_FACES) {
        snprintf(text, sizeof(text), "%s %u 环 · 下一靶", face_names[face],
                 (unsigned)archery_counts_points(counts, session->target));
    } else if (faces) {
        snprintf(text, sizeof(text), "确认提交本组（共 %u 箭）", (unsigned)session->arrows);
    } else {
        snprintf(text, sizeof(text), "确认提交本组（%u 环）",
                 (unsigned)archery_counts_points(counts, session->target));
    }
    lv_obj_t *label = make_label(card, text, ARCHERY_FONT_BODY,
                                 commit_selected ? C_GOLD : (ready ? C_TEXT : C_MUTED));
    lv_obj_center(label);

    add_hint(scr, "上/下 数量 · 确定 下一项", 256);
    add_hint(scr, s_voice_status && s_voice_status[0] ? s_voice_status
                                                      : "长按 上一项 · 首行放弃", 282);
}

static void render_summary(lv_obj_t *scr, const archery_flow_t *flow) {
    const archery_session_t *session = &flow->session;
    char title[32];
    snprintf(title, sizeof(title), "第 %u/%u 组 完成",
             (unsigned)session->group, (unsigned)session->groups);
    add_title(scr, title);

    char points[8];
    snprintf(points, sizeof(points), "%u",
             (unsigned)archery_session_group_points(session,
                 (uint8_t)(session->group - 1u)));
    centered_label(scr, points, ARCHERY_FONT_BIG, C_GOLD, 62);
    centered_label(scr, "环", ARCHERY_FONT_BODY, C_MUTED, 112);

    char total[48];
    snprintf(total, sizeof(total), "累计 %u 环",
             (unsigned)archery_session_total_points(session));
    centered_label(scr, total, ARCHERY_FONT_TITLE, C_TEXT, 142);

    char average[48];
    format_average(average, sizeof(average), archery_session_average_x10(session));
    char line[64];
    snprintf(line, sizeof(line), "平均 %s 环/箭", average);
    centered_label(scr, line, ARCHERY_FONT_BODY, C_MUTED, 172);

    add_group_dots(scr, session->groups, session->group, 206);

    const bool complete = archery_session_complete(session);
    const archery_rect_t button = {40, 228, 160, 30};
    lv_obj_t *next_card = make_card(scr, button, true, false);
    lv_obj_t *next_label = make_label(next_card, complete ? "查看总结" : "开始下一组",
                                      ARCHERY_FONT_BODY, C_GOLD);
    lv_obj_center(next_label);

    add_hint(scr, "长按确定 放弃本次训练", 266);
}

static void render_finish(lv_obj_t *scr, const archery_flow_t *flow) {
    const archery_session_t *session = &flow->session;
    add_title(scr, "训练完成");

    char points[8];
    snprintf(points, sizeof(points), "%u",
             (unsigned)archery_session_total_points(session));
    centered_label(scr, points, ARCHERY_FONT_BIG, C_GOLD, 50);
    centered_label(scr, "环", ARCHERY_FONT_BODY, C_MUTED, 100);

    char line[64];
    snprintf(line, sizeof(line), "%u 组 × %u 箭 · 共 %u 箭",
             (unsigned)session->groups, (unsigned)session->arrows,
             (unsigned)archery_session_arrow_count(session));
    centered_label(scr, line, ARCHERY_FONT_BODY, C_TEXT, 132);

    char average[16];
    format_average(average, sizeof(average), archery_session_average_x10(session));
    if (flow->settings.record_x) {
        snprintf(line, sizeof(line), "平均 %s 环 · 内十 %u · 脱靶 %u", average,
                 (unsigned)archery_session_total_x(session),
                 (unsigned)archery_session_total_miss(session));
    } else {
        snprintf(line, sizeof(line), "平均 %s 环 · 脱靶 %u", average,
                 (unsigned)archery_session_total_miss(session));
    }
    centered_label(scr, line, ARCHERY_FONT_BODY, C_MUTED, 158);

    snprintf(line, sizeof(line), "最佳单组 %u 环",
             (unsigned)archery_session_best_group(session));
    centered_label(scr, line, ARCHERY_FONT_BODY, C_MUTED, 184);

    const archery_rect_t saved = {36, 216, 168, 34};
    lv_obj_t *card = make_card(scr, saved, false, false);
    const uint32_t card_color = flow->save_failed ? C_RED : C_GREEN;
    lv_obj_set_style_border_color(card, lv_color_hex(card_color), 0);
    lv_obj_t *label = make_label(card,
        flow->save_failed ? "保存失败，检查存储" : "已保存到历史记录",
        ARCHERY_FONT_BODY, card_color);
    lv_obj_center(label);

    add_hint(scr, "确定 返回主页", 278);
}

static void render_history(lv_obj_t *scr, const archery_flow_t *flow,
                           const archery_history_t *history) {
    add_title(scr, "历史记录");

    const uint16_t count = archery_history_visible_count(
        history, flow->history_filter, flow->clock_now);
    static const char *filter_names[] = {"全部", "今天", "最近 7 天"};
    char subtitle[48];
    snprintf(subtitle, sizeof(subtitle), "共 %u 条 · %s",
             (unsigned)count, filter_names[flow->history_filter]);
    centered_label(scr, subtitle, ARCHERY_FONT_BODY, C_MUTED, 40);

    if (count == 0) {
        centered_label(scr, "暂无训练记录", ARCHERY_FONT_TITLE, C_TEXT, 96);
        centered_label(scr, "完成一次训练后会自动保存", ARCHERY_FONT_BODY, C_MUTED, 132);
        // 空历史也保留一个可见的退出入口，避免只剩长按操作。
        const archery_rect_t rect = {16, 180, 208, 42};
        const bool selected = flow->history_cursor == 0;
        lv_obj_t *card = make_card(scr, rect, selected, false);
        lv_obj_t *label = make_label(card, "返回主页", ARCHERY_FONT_BODY,
                                     selected ? C_GOLD : C_TEXT);
        lv_obj_center(label);
        add_hint(scr, "确定 返回 · 双击 切换筛选", 284);
        return;
    }

    // 4 行可视窗口，记录之后固定"清空全部记录"和"返回主页"两行。
    const uint8_t visible = 4;
    const uint16_t total_rows = (uint16_t)(count + 2u);
    const uint16_t cursor = flow->history_cursor;
    uint16_t offset = 0;
    if (cursor >= visible) offset = (uint16_t)(cursor - visible + 1u);
    for (uint8_t row = 0; row < visible; row++) {
        const uint16_t index = (uint16_t)(offset + row);
        if (index >= total_rows) break;
        const archery_rect_t rect = {16, (int16_t)(66 + row * 48), 208, 42};
        const bool selected = index == cursor;
        if (index >= count) {
            const bool is_clear = index == count;
            lv_obj_t *card = make_card(scr, rect, selected, is_clear);
            lv_obj_t *label = make_label(card, is_clear ? "清空全部记录" : "返回主页",
                                         ARCHERY_FONT_BODY,
                                         is_clear ? (selected ? C_RED : C_MUTED) : C_TEXT);
            lv_obj_center(label);
            continue;
        }
        const archery_record_t *record = archery_history_visible_record(
            history, flow->history_filter, flow->clock_now, index);
        if (!record) continue;
        lv_obj_t *card = make_card(scr, rect, selected, false);
        char when_text[16];
        archery_clock_format_short(record->when, when_text, sizeof(when_text));
        lv_obj_t *when = make_label(card, when_text, ARCHERY_FONT_NUMBER,
                                    selected ? C_GOLD : C_MUTED);
        lv_obj_align(when, LV_ALIGN_LEFT_MID, 8, 0);

        char summary[24];
        snprintf(summary, sizeof(summary), "#%u %u 环", (unsigned)record->seq,
                 (unsigned)archery_record_total_points(record));
        lv_obj_t *sum = make_label(card, summary, ARCHERY_FONT_BODY,
                                   selected ? C_GOLD : C_TEXT);
        lv_obj_align(sum, LV_ALIGN_RIGHT_MID, -8, 0);
    }
    add_hint(scr, "上/下 选择 · 按住 快滚", 262);
    add_hint(scr, "双击 筛选 · 长按 回主页", 284);
}

static void render_detail(lv_obj_t *scr, const archery_flow_t *flow,
                          const archery_history_t *history) {
    const archery_record_t *record = archery_history_visible_record(
        history, flow->history_filter, flow->clock_now, flow->history_cursor);
    if (!record) {
        add_title(scr, "记录不可用");
        add_hint(scr, "确定 返回", 284);
        return;
    }
    const uint8_t group = flow->detail_group  < record->groups
                              ? flow->detail_group : (uint8_t)(record->groups - 1u);

    char title[32];
    snprintf(title, sizeof(title), "#%u %s", (unsigned)record->seq,
             target_name(record->target));
    add_title(scr, title);

    char when_text[16];
    char duration[16];
    archery_clock_format_short(record->when, when_text, sizeof(when_text));
    format_duration(duration, sizeof(duration), record->duration_s);
    char line[72];
    snprintf(line, sizeof(line), "%s · %u×%u · 用时 %s", when_text,
             (unsigned)record->groups, (unsigned)record->arrows, duration);
    centered_label(scr, line, ARCHERY_FONT_BODY, C_MUTED, 36);

    if (archery_record_total_x(record) > 0) {
        snprintf(line, sizeof(line), "总 %u 环 · 内十 %u · 脱靶 %u",
                 (unsigned)archery_record_total_points(record),
                 (unsigned)archery_record_total_x(record),
                 (unsigned)archery_record_total_miss(record));
    } else {
        snprintf(line, sizeof(line), "总 %u 环 · 脱靶 %u",
                 (unsigned)archery_record_total_points(record),
                 (unsigned)archery_record_total_miss(record));
    }
    centered_label(scr, line, ARCHERY_FONT_BODY, C_TEXT, 60);

    char group_title[32];
    snprintf(group_title, sizeof(group_title), "第 %u/%u 组",
             (unsigned)(group + 1u), (unsigned)record->groups);
    centered_label(scr, group_title, ARCHERY_FONT_TITLE, C_GOLD, 88);

    // 与录入页一致，按"每个环值几根箭"展示；全靶每行 4 项、共 3 行。
    // 没有内十的记录不再列出"X 0"，老记录里的 X 仍按实际数据展示。
    const uint8_t slots = archery_target_slot_count(record->target);
    char counts_text[192];
    size_t used = 0;
    uint8_t shown = 0;
    counts_text[0] = '\0';
    for (uint8_t slot = 0; slot < slots; slot++) {
        const uint8_t count = archery_record_ring_count(record, group, slot);
        if (archery_target_slot_code(record->target, slot) == ARCHERY_SCORE_X &&
            count == 0) {
            continue;
        }
        if (shown > 0 && shown % 4u == 0) {
            used += (size_t)snprintf(&counts_text[used], sizeof(counts_text) - used, "\n");
        } else if (shown > 0) {
            used += (size_t)snprintf(&counts_text[used], sizeof(counts_text) - used,
                                     " · ");
        }
        used += (size_t)snprintf(&counts_text[used], sizeof(counts_text) - used,
                                 "%s %u", slot_label(record->target, slot),
                                 (unsigned)count);
        shown++;
    }
    const uint8_t lines = (uint8_t)((shown + 3u) / 4u > 0u ? (shown + 3u) / 4u : 1u);
    const archery_rect_t counts_card = {20, 124, 200,
                                        (int16_t)(lines * 24 + 16)};
    lv_obj_t *card = make_card(scr, counts_card, false, false);
    lv_obj_t *counts = make_label(card, counts_text, ARCHERY_FONT_BODY, C_TEXT);
    lv_obj_set_style_text_align(counts, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(counts);

    if (archery_record_group_x(record, group) > 0) {
        snprintf(line, sizeof(line), "本组 %u 环 · 内十 %u",
                 (unsigned)archery_record_group_points(record, group),
                 (unsigned)archery_record_group_x(record, group));
    } else {
        snprintf(line, sizeof(line), "本组 %u 环",
                 (unsigned)archery_record_group_points(record, group));
    }
    centered_label(scr, line, ARCHERY_FONT_BODY, C_TEXT, 230);

    add_hint(scr, "上/下 换组 · 确定 返回", 284);
}

// 联网：状态 + 扫描列表 / 手机扫码配网 / 立即对时。
void archery_ui_set_wifi_view(const archery_wifi_view_t *view) { s_wifi_view = view; }
void archery_ui_set_voice_status(const char *text) { s_voice_status = text; }

static void render_wifi(lv_obj_t *scr, const archery_flow_t *flow) {
    add_title(scr, "联网");
    const archery_wifi_view_t *view = s_wifi_view;
    if (!view) {
        centered_label(scr, "联网模块未初始化", ARCHERY_FONT_TITLE, C_TEXT, 120);
        add_hint(scr, "长按确定 返回主页", 284);
        return;
    }

    if (view->state == ARCHERY_WIFI_AP_READY) {
        lv_obj_t *img = lv_image_create(scr);
        lv_image_set_src(img, &archery_qr_image);
        lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 44);
        centered_label(scr, "手机 Wi-Fi 连接热点", ARCHERY_FONT_BODY, C_TEXT, 198);
        centered_label(scr, "Archery-Setup（无需密码）", ARCHERY_FONT_BODY, C_GOLD, 220);
        centered_label(scr, "浏览器打开 192.168.4.1", ARCHERY_FONT_BODY, C_TEXT, 242);
        centered_label(scr, archery_voice_status(), ARCHERY_FONT_BODY, C_MUTED, 264);
        add_hint(scr, "确定 停止配网", 292);
        return;
    }

    if (view->state == ARCHERY_WIFI_CONNECTING) {
        centered_label(scr, "正在连接", ARCHERY_FONT_TITLE, C_TEXT, 96);
        centered_label(scr, view->ssid[view->selected][0] ? view->ssid[view->selected]
                                                         : "选中的网络",
                       ARCHERY_FONT_BODY, C_GOLD, 136);
        centered_label(scr, "请稍候…", ARCHERY_FONT_BODY, C_MUTED, 170);
        add_hint(scr, "长按确定 取消", 284);
        return;
    }

    if (flow->wifi_mode == 2) {
        if (view->state == ARCHERY_WIFI_CONNECTING) {
            centered_label(scr, "正在连接", ARCHERY_FONT_TITLE, C_TEXT, 96);
            centered_label(scr, view->ssid[view->selected][0] ? view->ssid[view->selected]
                                                             : "选中的网络",
                           ARCHERY_FONT_BODY, C_GOLD, 136);
            centered_label(scr, "请稍候…", ARCHERY_FONT_BODY, C_MUTED, 170);
        } else if (view->sta_connected) {
            centered_label(scr, "连接成功", ARCHERY_FONT_TITLE, C_GOLD, 110);
            centered_label(scr, view->ip[0] ? view->ip : "已获取 IP",
                           ARCHERY_FONT_BODY, C_TEXT, 150);
        } else {
            centered_label(scr, "连接失败", ARCHERY_FONT_TITLE, C_TEXT, 110);
            centered_label(scr, view->message[0] ? view->message : "请检查密码后重试",
                           ARCHERY_FONT_BODY, C_MUTED, 150);
        }
        add_hint(scr, "确定 返回扫描列表", 284);
        return;
    }

    if (flow->wifi_mode == 1) {
        if (view->state == ARCHERY_WIFI_SCANNING) {
            centered_label(scr, "正在扫描附近 Wi-Fi…", ARCHERY_FONT_BODY, C_MUTED, 40);
        } else {
            char subtitle[40];
            snprintf(subtitle, sizeof(subtitle), "共 %u 个网络", (unsigned)view->ap_count);
            centered_label(scr, subtitle, ARCHERY_FONT_BODY, C_MUTED, 40);
        }
        const uint8_t visible = 4;
        const uint16_t total = (uint16_t)(view->ap_count + 1u);  // 末行"返回"
        const uint16_t cursor = flow->wifi_row;
        uint16_t offset = 0;
        if (cursor >= visible) offset = (uint16_t)(cursor - visible + 1u);
        for (uint8_t row = 0; row < visible; row++) {
            const uint16_t index = (uint16_t)(offset + row);
            if (index >= total) break;
            const archery_rect_t rect = {14, (int16_t)(58 + row * 48), 212, 42};
            const bool selected = index == cursor;
            lv_obj_t *card = make_card(scr, rect, selected, false);
            if (index >= view->ap_count) {
                lv_obj_t *label = make_label(card, "返回", ARCHERY_FONT_BODY,
                                             selected ? C_GOLD : C_TEXT);
                lv_obj_center(label);
                continue;
            }
            lv_obj_t *name = make_label(card, view->ssid[index], ssid_font(),
                                        selected ? C_GOLD : C_TEXT);
            lv_obj_align(name, LV_ALIGN_LEFT_MID, 10, -8);
            char meta[24];
            snprintf(meta, sizeof(meta), "%s  %d dBm",
                     view->secure[index] ? "加密" : "开放", (int)view->rssi[index]);
            lv_obj_t *info = make_label(card, meta, ARCHERY_FONT_BODY, C_MUTED);
            lv_obj_align(info, LV_ALIGN_LEFT_MID, 10, 10);
            if (selected) lv_obj_set_style_text_color(info, lv_color_hex(C_GOLD), 0);
        }
        add_hint(scr, "上/下 选择 · 确定 连接", 262);
        add_hint(scr, "长按确定 返回", 288);
        return;
    }

    // 菜单 / 状态
    const bool connected = view->sta_connected;
    const archery_rect_t status = {14, 40, 212, 40};
    lv_obj_t *card = make_card(scr, status, false, false);
    lv_obj_t *name = make_label(card, connected ? "已连接" : "未连接", ARCHERY_FONT_BODY,
                                connected ? C_GOLD : C_MUTED);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 10, -8);
    char detail[64];
    if (connected) {
        snprintf(detail, sizeof(detail), "%s", view->ip[0] ? view->ip : "获取 IP 中");
    } else if (view->message[0]) {
        snprintf(detail, sizeof(detail), "%s", view->message);
    } else {
        snprintf(detail, sizeof(detail), "选择下面的方式连接 Wi-Fi");
    }
    lv_obj_t *info = make_label(card, detail, ARCHERY_FONT_BODY, C_MUTED);
    lv_obj_align(info, LV_ALIGN_LEFT_MID, 10, 10);

    static const char *rows[] = {"扫描网络", "手机配网 / 导出", "立即对时", "断开连接", "验证语音"};
    for (uint8_t row = 0; row < 5; row++) {
        const archery_rect_t rect = {20, (int16_t)(88 + row * 34), 200, 30};
        const bool selected = flow->wifi_row == row;
        lv_obj_t *item = make_card(scr, rect, selected, false);
        lv_obj_t *label = make_label(item, rows[row], ARCHERY_FONT_BODY,
                                     selected ? C_GOLD : C_TEXT);
        lv_obj_center(label);
    }
    add_hint(scr, "上/下 选择 · 确定 执行", 260);
    if (view->time_synced) {
        add_hint(scr, "已通过网络校准时间", 286);
    } else {
        add_hint(scr, "v" ARCHERY_FIRMWARE_VERSION " · 长按确定 返回主页", 286);
    }
}
// 设备端 Wi-Fi 密码输入：上/下换字符，确定下一位，空位确定即连接。
static void render_wifi_password(lv_obj_t *scr, const archery_flow_t *flow) {
    add_title(scr, "密码");
    const archery_wifi_view_t *view = s_wifi_view;
    if (view && flow->wifi_target < view->ap_count) {
        centered_label(scr, view->ssid[flow->wifi_target], ssid_font(), C_GOLD, 38);
    }

    char shown[24];
    uint8_t n = 0;
    const uint8_t start = flow->wifi_pw_pos >= 18u
                              ? (uint8_t)(flow->wifi_pw_pos - 17u) : 0u;
    for (uint8_t i = start; i < flow->wifi_pw_len && n < 18u; i++) {
        shown[n++] = flow->wifi_password[i];
    }
    if (flow->wifi_pw_pos == flow->wifi_pw_len || n < 18u) shown[n++] = '_';
    shown[n] = '\0';
    lv_obj_t *pw = make_label(scr, shown, ARCHERY_FONT_NUMBER, C_TEXT);
    lv_obj_align(pw, LV_ALIGN_TOP_MID, 0, 86);

    char line[48];
    snprintf(line, sizeof(line), "第 %u 位", (unsigned)(flow->wifi_pw_pos + 1u));
    centered_label(scr, line, ARCHERY_FONT_BODY, C_MUTED, 128);

    if (flow->wifi_pw_pos < flow->wifi_pw_len) {
        snprintf(line, sizeof(line), "当前字符 %c",
                 flow->wifi_password[flow->wifi_pw_pos]);
        centered_label(scr, line, ARCHERY_FONT_TITLE, C_GOLD, 160);
    } else if (flow->wifi_pw_len > 0) {
        centered_label(scr, "确定 连接", ARCHERY_FONT_TITLE, C_GOLD, 160);
    } else {
        centered_label(scr, "上/下 选择第一个字符", ARCHERY_FONT_BODY, C_MUTED, 160);
    }

    add_hint(scr, "上/下 换字符 · 确定 下一位", 212);
    add_hint(scr, "长按删除 · 双击确定连接", 238);
    add_hint(scr, "长按上/下 快速换字符", 264);
}

// 设置：靶子类型 / 屏幕方向 / 亮度 / 息屏时间 / 日期时间 / 返回。
// 调整立即生效，离开时写 NVS；"日期时间"进入单独的手动对时页。
static void render_settings(lv_obj_t *scr, const archery_flow_t *flow) {
    add_title(scr, "设置");

    static const char *row_names[] = {"靶子类型", "屏幕方向", "屏幕亮度",
                                      "息屏时间", "记录内十", "每组计时",
                                      "倒数十秒语音", "语音录靶", "滚动录入",
                                      "提示音量", "保存Wi-Fi密码", "Beta功能测试"};
    char value[24];
    const uint8_t visible = 6;
    uint8_t offset = flow->settings_row >= visible ? (uint8_t)(flow->settings_row - visible + 1u) : 0u;
    if (offset > ARCHERY_SETTINGS_ROWS - visible) offset = ARCHERY_SETTINGS_ROWS - visible;
    char page[16];
    snprintf(page, sizeof(page), "%u-%u / %u", (unsigned)(offset + 1u),
             (unsigned)(offset + visible), (unsigned)ARCHERY_SETTINGS_ROWS);
    centered_label(scr, page, ARCHERY_FONT_BODY, C_MUTED, 34);
    for (uint8_t i = 0; i < visible; i++) {
        const uint8_t row = (uint8_t)(offset + i);
        const archery_rect_t rect = {20, (int16_t)(54 + i * 32), 200, 28};
        const bool selected = flow->settings_row == row;
        lv_obj_t *card = make_card(scr, rect, selected, false);
        lv_obj_t *name = make_label(card, row_names[row], ARCHERY_FONT_BODY,
                                    selected ? C_GOLD : C_TEXT);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 12, 0);
        if (row == 0) {
            snprintf(value, sizeof(value), "%s", target_name(flow->settings.target));
        } else if (row == 1) {
            snprintf(value, sizeof(value), "%s",
                     flow->settings.rotate ? "倒转" : "正常");
        } else if (row == 2) {
            snprintf(value, sizeof(value), "%u%%",
                     (unsigned)flow->settings.brightness);
        } else if (row == 3 && flow->settings.off_seconds == 0) {
            snprintf(value, sizeof(value), "不息屏");
        } else if (row == 3) {
            snprintf(value, sizeof(value), "%u 秒",
                     (unsigned)flow->settings.off_seconds);
        } else if (row == 4) {
            snprintf(value, sizeof(value), "%s", flow->settings.record_x ? "开" : "关");
        } else if (row == 5) {
            if (flow->settings.timer_seconds == 0u) {
                snprintf(value, sizeof(value), "关");
            } else {
                snprintf(value, sizeof(value), "%u 秒/支",
                         (unsigned)flow->settings.timer_seconds);
            }
        } else if (row == 6) {
            static const char *const voicing[] = {"关", "系统", "小豆", "豆包"};
            snprintf(value, sizeof(value), "%s",
                     voicing[flow->settings.countdown_voice <= 3u
                                 ? flow->settings.countdown_voice : 0u]);
        } else if (row == 7) {
            snprintf(value, sizeof(value), "%s",
                     flow->settings.voice_enabled ? "开" : "关");
        } else if (row == 8) {
            snprintf(value, sizeof(value), "%s",
                     flow->settings.roll_entry ? "开" : "关");
        } else if (row == 9) {
            snprintf(value, sizeof(value), "%u%%",
                     (unsigned)(flow->settings.beep_volume * 20u));
        } else if (row == 10) {
            snprintf(value, sizeof(value), "%s",
                     flow->settings.wifi_save ? "开" : "关");
        } else if (row == 11) {
            snprintf(value, sizeof(value), "%s", "进入");
        }
        lv_obj_t *number = make_label(card, value, ARCHERY_FONT_BODY,
                                      selected ? C_GOLD : C_TEXT);
        lv_obj_align(number, LV_ALIGN_RIGHT_MID, -12, 0);
    }
    add_hint(scr, "上/下 选择 · 确定 循环", 262);
    add_hint(scr, "长按确定 保存返回", 288);
}

static void render_beta(lv_obj_t *scr, const archery_flow_t *flow) {
    add_title(scr, "Beta 功能测试");
    static const char *names[] = {"自动训练模式", "自动重连网络"};
    char value[16];
    for (uint8_t row = 0; row < 2; row++) {
        const archery_rect_t rect = {20, (int16_t)(80 + row * 40), 200, 32};
        const bool selected = flow->beta_row == row;
        lv_obj_t *card = make_card(scr, rect, selected, false);
        lv_obj_t *name = make_label(card, names[row], ARCHERY_FONT_BODY,
                                    selected ? C_GOLD : C_TEXT);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 12, 0);
        const bool on = row == 0 ? flow->settings.auto_mode : flow->settings.auto_reconnect;
        snprintf(value, sizeof(value), "%s", on ? "开" : "关");
        lv_obj_t *state = make_label(card, value, ARCHERY_FONT_BODY,
                                     selected ? C_GOLD : C_TEXT);
        lv_obj_align(state, LV_ALIGN_RIGHT_MID, -12, 0);
    }
    add_hint(scr, "上/下 选择 · 确定 切换", 262);
    add_hint(scr, "长按确定 返回设置", 288);
}

static void render_auto_check(lv_obj_t *scr, const archery_flow_t *flow) {
    add_title(scr, "训练前检测");
    if (!flow->auto_check_done) {
        centered_label(scr, "正在检测语音服务…", ARCHERY_FONT_BODY, C_MUTED, 110);
    } else if (flow->auto_check_ok) {
        centered_label(scr, "语音可用", ARCHERY_FONT_TITLE, C_GOLD, 96);
        centered_label(scr, "已开启语音录靶，关闭滚动录入",
                       ARCHERY_FONT_BODY, C_TEXT, 140);
    } else {
        centered_label(scr, "语音不可用", ARCHERY_FONT_TITLE, C_TEXT, 96);
        centered_label(scr, "已关闭语音录靶，开启滚动录入",
                       ARCHERY_FONT_BODY, C_TEXT, 140);
    }
    add_hint(scr, "确定 立即开始 · 3 秒后自动进入", 284);
}

// 手动对时：年 / 月 / 日 / 时 / 分。设备没有 RTC 电池，断电后从上次时间继续。
static void render_clock(lv_obj_t *scr, const archery_flow_t *flow) {
    add_title(scr, "日期时间");
    centered_label(scr, "断电后从上次时间继续", ARCHERY_FONT_BODY, C_MUTED, 36);

    static const char *row_names[] = {"年", "月", "日", "时", "分"};
    const uint16_t values[] = {
        flow->clock_fields.year, flow->clock_fields.month, flow->clock_fields.day,
        flow->clock_fields.hour, flow->clock_fields.minute,
    };
    char value[8];
    for (uint8_t row = 0; row < ARCHERY_CLOCK_ROWS; row++) {
        const archery_rect_t rect = {24, (int16_t)(60 + row * 38), 192, 34};
        const bool selected = flow->clock_row == row;
        lv_obj_t *card = make_card(scr, rect, selected, false);
        lv_obj_t *name = make_label(card, row_names[row], ARCHERY_FONT_BODY,
                                    selected ? C_GOLD : C_TEXT);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 14, 0);
        snprintf(value, sizeof(value), "%u", (unsigned)values[row]);
        lv_obj_t *number = make_label(card, value, ARCHERY_FONT_TITLE,
                                      selected ? C_GOLD : C_TEXT);
        lv_obj_align(number, LV_ALIGN_RIGHT_MID, -16, 0);
    }
    add_hint(scr, "上/下 调整 · 确定 下一项", 254);
    add_hint(scr, "长按确定 保存并返回", 280);
}

static void render_confirm(lv_obj_t *scr, const archery_flow_t *flow,
                           const archery_history_t *history, bool abort_page) {
    if (abort_page) {
        centered_label(scr, "放弃本次训练", ARCHERY_FONT_TITLE, C_RED, 52);
        centered_label(scr, "本次已录入的成绩不会保存", ARCHERY_FONT_BODY, C_TEXT, 112);
        centered_label(scr, "确定要放弃吗？", ARCHERY_FONT_BODY, C_MUTED, 138);
    } else {
        centered_label(scr, "清空全部记录", ARCHERY_FONT_TITLE, C_RED, 52);
        char line[48];
        snprintf(line, sizeof(line), "将删除 %u 条训练记录",
                 (unsigned)history->meta.count);
        centered_label(scr, line, ARCHERY_FONT_BODY, C_TEXT, 112);
        centered_label(scr, "清空后无法恢复", ARCHERY_FONT_BODY, C_MUTED, 138);
    }

    const char *options[2];
    if (abort_page) {
        options[0] = "继续训练";
        options[1] = "放弃本次训练";
    } else {
        options[0] = "否，返回";
        options[1] = "是，全部清空";
    }
    for (int i = 0; i < 2; i++) {
        const bool selected = flow->confirm_yes == (i == 1);
        const archery_rect_t rect = {30, (int16_t)(172 + i * 52), 180, 40};
        lv_obj_t *card = make_card(scr, rect, selected, i == 1);
        lv_obj_t *label = make_label(card, options[i], ARCHERY_FONT_BODY,
            i == 1 ? C_RED : (selected ? C_GOLD : C_TEXT));
        lv_obj_center(label);
    }
    add_hint(scr, "上/下 选择 · 确定 执行", 278);
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------

static const char *diag_reset_text(esp_reset_reason_t reason) {
    switch (reason) {
    case ESP_RST_POWERON: return "上电";
    case ESP_RST_SW: return "软件复位";
    case ESP_RST_PANIC: return "异常";
    case ESP_RST_INT_WDT: return "中断看门狗";
    case ESP_RST_TASK_WDT: return "任务看门狗";
    case ESP_RST_WDT: return "看门狗";
    case ESP_RST_DEEPSLEEP: return "深睡唤醒";
    case ESP_RST_BROWNOUT: return "欠压";
    case ESP_RST_EXT: return "外部复位";
    default: return "未知";
    }
}

static void render_diag(lv_obj_t *scr, const archery_flow_t *flow,
                        const archery_history_t *history, int battery_soc) {
    add_title(scr, "诊断");
    static const char *names[ARCHERY_DIAG_ROWS] = {
        "Wi-Fi", "SSID", "信号", "蓝牙", "空闲内存", "内存最低",
        "最大块", "LVGL", "NVS", "历史记录", "电量", "复位/运行", "语音服务",
    };
    const uint8_t visible = 6;
    uint8_t offset = flow->diag_row >= visible ? (uint8_t)(flow->diag_row - visible + 1u) : 0u;
    if (offset > ARCHERY_DIAG_ROWS - visible) offset = ARCHERY_DIAG_ROWS - visible;
    char page[16];
    snprintf(page, sizeof(page), "%u-%u / %u", (unsigned)(offset + 1u),
             (unsigned)(offset + visible), (unsigned)ARCHERY_DIAG_ROWS);
    centered_label(scr, page, ARCHERY_FONT_BODY, C_MUTED, 34);

    char value[40];
    for (uint8_t i = 0; i < visible; i++) {
        const uint8_t row = (uint8_t)(offset + i);
        const archery_rect_t rect = {20, (int16_t)(54 + i * 32), 200, 28};
        const bool selected = flow->diag_row == row;
        lv_obj_t *card = make_card(scr, rect, selected, false);
        lv_obj_t *name = make_label(card, names[row], ARCHERY_FONT_BODY,
                                    selected ? C_GOLD : C_TEXT);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 10, 0);
        value[0] = '\0';
        switch (row) {
        case 0:
            if (!s_wifi_view) snprintf(value, sizeof(value), "未初始化");
            else if (s_wifi_view->state == ARCHERY_WIFI_AP_READY) snprintf(value, sizeof(value), "配网中");
            else if (s_wifi_view->sta_connected) snprintf(value, sizeof(value), "已连接");
            else snprintf(value, sizeof(value), "未连接");
            break;
        case 1: {
            const char *ssid = archery_wifi_current_ssid();
            snprintf(value, sizeof(value), "%s", ssid && ssid[0] ? ssid : "--");
            break;
        }
        case 2:
            if (s_wifi_view && s_wifi_view->sta_connected) {
                snprintf(value, sizeof(value), "%d dBm", (int)s_wifi_view->sta_rssi);
            } else {
                snprintf(value, sizeof(value), "--");
            }
            break;
        case 3: {
            const esp_bt_controller_status_t bt = esp_bt_controller_get_status();
            snprintf(value, sizeof(value), "%s",
                     bt == ESP_BT_CONTROLLER_STATUS_ENABLED ? "已启用" : "未启用");
            break;
        }
        case 4:
            snprintf(value, sizeof(value), "%u KB",
                     (unsigned)(esp_get_free_heap_size() / 1024));
            break;
        case 5:
            snprintf(value, sizeof(value), "%u KB",
                     (unsigned)(esp_get_minimum_free_heap_size() / 1024));
            break;
        case 6:
            snprintf(value, sizeof(value), "%u KB",
                     (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024));
            break;
        case 7: {
            lv_mem_monitor_t mon;
            lv_mem_monitor(&mon);
            snprintf(value, sizeof(value), "%u/%u KB",
                     (unsigned)(mon.total_size / 1024),
                     (unsigned)((mon.total_size - mon.free_size) / 1024));
            break;
        }
        case 8: {
            nvs_stats_t stats;
            if (nvs_get_stats(NULL, &stats) == ESP_OK) {
                snprintf(value, sizeof(value), "%u/%u",
                         (unsigned)stats.used_entries, (unsigned)stats.total_entries);
            } else {
                snprintf(value, sizeof(value), "--");
            }
            break;
        }
        case 9:
            snprintf(value, sizeof(value), "%u 条", (unsigned)history->meta.count);
            break;
        case 10:
            snprintf(value, sizeof(value), "%d%%", battery_soc);
            break;
        case 11: {
            const int64_t seconds = esp_timer_get_time() / 1000000;
            snprintf(value, sizeof(value), "%s %u:%02u",
                     diag_reset_text(esp_reset_reason()), (unsigned)(seconds / 60),
                     (unsigned)(seconds % 60));
            break;
        }
        default:
            if (archery_voice_has_credentials()) {
                const int result = archery_ws_last_open_result();
                if (result == 1) {
                    snprintf(value, sizeof(value), "已连接");
                } else if (result == 0) {
                    snprintf(value, sizeof(value), "失败 %d", archery_ws_last_status());
                } else {
                    snprintf(value, sizeof(value), "待测试");
                }
            } else if (archery_voice_url()[0]) {
                snprintf(value, sizeof(value), "代理");
            } else {
                snprintf(value, sizeof(value), "未配置");
            }
            break;
        }
        lv_obj_t *number = make_label(card, value, ARCHERY_FONT_BODY,
                                      selected ? C_GOLD : C_TEXT);
        lv_obj_align(number, LV_ALIGN_RIGHT_MID, -8, 0);
    }
    add_hint(scr, "上/下 选择 · 确定 日志", 262);
    add_hint(scr, "长按确定 保存返回", 288);
}

static void render_diag_log(lv_obj_t *scr, const archery_flow_t *flow) {
    add_title(scr, "日志");
    static char log_text[ARCHERY_LOG_LINES * ARCHERY_LOG_LINE_CHARS];
    archery_logbuf_snapshot(log_text, sizeof(log_text));

    static const char *lines[ARCHERY_LOG_LINES];
    uint8_t count = 0;
    char *cursor = log_text;
    while (cursor && *cursor && count < ARCHERY_LOG_LINES) {
        lines[count++] = cursor;
        char *newline = strchr(cursor, '\n');
        if (!newline) break;
        *newline = '\0';
        cursor = newline + 1;
    }

    char count_text[16];
    snprintf(count_text, sizeof(count_text), "%u 条", (unsigned)count);
    centered_label(scr, count_text, ARCHERY_FONT_BODY, C_MUTED, 34);

    archery_font_audit_set_enabled(false);
    const lv_font_t *font = ssid_font();
    int16_t y = 44;
    for (uint8_t index = flow->diag_log_row; index < count; index++) {
        char clean[ARCHERY_LOG_LINE_CHARS * 3];
        archery_font_filter(font, lines[index], clean, sizeof(clean));
        lv_point_t size;
        lv_text_get_size(&size, clean, font, 0, 2, 224, LV_TEXT_FLAG_NONE);
        if (y + size.y > 262) break;
        lv_obj_t *label = make_label(scr, clean, font,
                                     index == flow->diag_log_row ? C_GOLD : C_TEXT);
        lv_obj_set_pos(label, 8, y);
        lv_obj_set_width(label, 224);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        y += (int16_t)(size.y + 4);
    }
    archery_font_audit_set_enabled(true);
    if (count == 0) {
        centered_label(scr, "暂无日志", ARCHERY_FONT_BODY, C_MUTED, 120);
    }
    add_hint(scr, "上/下 滚动 · 确定 返回", 266);
    add_hint(scr, "长按确定 返回主页", 290);
}

static void render_timer(lv_obj_t *scr, const archery_flow_t *flow) {
    const bool shooting = flow->timer_phase == 2;
    char clock[8];
    snprintf(clock, sizeof(clock), shooting ? "%u:%02u" : "%u",
             shooting ? (unsigned)(flow->timer_remaining / 60u)
                      : (unsigned)flow->timer_remaining,
             shooting ? (unsigned)(flow->timer_remaining % 60u) : 0u);
    s_timer_title = make_label(scr, shooting ? "射击" : "准备", ARCHERY_FONT_TITLE, C_TEXT);
    lv_obj_align(s_timer_title, LV_ALIGN_TOP_MID, 0, 8);
    s_timer_clock = make_label(scr, clock, ARCHERY_FONT_BIG, C_GOLD);
    lv_obj_align(s_timer_clock, LV_ALIGN_TOP_MID, 0, 105);
    s_timer_hint = make_label(scr, shooting ? "每支箭结束后再记分" : "听到两声后准备",
                              ARCHERY_FONT_BODY, C_MUTED);
    lv_obj_align(s_timer_hint, LV_ALIGN_TOP_MID, 0, 160);
    add_hint(scr, "确定 跳过计时", 240);
    add_hint(scr, "长按确定 返回新训练", 266);
}

void archery_ui_timer_tick(const archery_flow_t *flow) {
    if (!flow || !s_timer_title || !s_timer_clock || !s_timer_hint) return;
    if (!bsp_lvgl_lock(500)) return;
    const bool shooting = flow->timer_phase == 2;
    char clock[8];
    snprintf(clock, sizeof(clock), shooting ? "%u:%02u" : "%u",
             shooting ? (unsigned)(flow->timer_remaining / 60u)
                      : (unsigned)flow->timer_remaining,
             shooting ? (unsigned)(flow->timer_remaining % 60u) : 0u);
    lv_label_set_text(s_timer_title, shooting ? "射击" : "准备");
    lv_label_set_text(s_timer_clock, clock);
    lv_label_set_text(s_timer_hint, shooting ? "每支箭结束后再记分" : "听到两声后准备");
    bsp_lvgl_unlock();
}

void archery_ui_show(const archery_flow_t *flow, const archery_history_t *history,
                     int battery_soc) {
    if (!flow || !history) return;
    // 先把新页面挂上去再删旧页面：直接删除当前活动屏幕会让 LVGL 打
    // "the active screen was deleted" 警告，并在一帧内没有活动屏幕。
    lv_obj_t *previous = s_screen;
    s_timer_title = NULL;
    s_timer_clock = NULL;
    s_timer_hint = NULL;
    lv_obj_t *scr = base_screen();
    s_screen = scr;

    switch (flow->page) {
    case ARCHERY_PAGE_HOME: render_home(scr, flow); break;
    case ARCHERY_PAGE_SETUP: render_setup(scr, flow); break;
    case ARCHERY_PAGE_ENTRY: render_entry(scr, flow); break;
    case ARCHERY_PAGE_SUMMARY: render_summary(scr, flow); break;
    case ARCHERY_PAGE_FINISH: render_finish(scr, flow); break;
    case ARCHERY_PAGE_HISTORY: render_history(scr, flow, history); break;
    case ARCHERY_PAGE_DETAIL: render_detail(scr, flow, history); break;
    case ARCHERY_PAGE_CLEAR_CONFIRM: render_confirm(scr, flow, history, false); break;
    case ARCHERY_PAGE_ABORT_CONFIRM: render_confirm(scr, flow, history, true); break;
    case ARCHERY_PAGE_SETTINGS: render_settings(scr, flow); break;
    case ARCHERY_PAGE_AUTO_CHECK: render_auto_check(scr, flow); break;
    case ARCHERY_PAGE_BETA: render_beta(scr, flow); break;
    case ARCHERY_PAGE_SIGHT: render_sight(scr, flow); break;
    case ARCHERY_PAGE_TOOLS: render_tools(scr, flow); break;
    case ARCHERY_PAGE_RULER: render_ruler(scr, flow); break;
    case ARCHERY_PAGE_SIGHT_CALC: render_sight_calc(scr, flow); break;
    case ARCHERY_PAGE_METRONOME: render_metronome(scr, flow); break;
    case ARCHERY_PAGE_RANGE_INTRO: render_range_intro(scr, flow); break;
    case ARCHERY_PAGE_RANGE_CONFIG: render_range_config(scr, flow); break;
    case ARCHERY_PAGE_RANGE_VIEW: render_range_view(scr, flow); break;
    case ARCHERY_PAGE_CLOCK: render_clock(scr, flow); break;
    case ARCHERY_PAGE_WIFI: render_wifi(scr, flow); break;
    case ARCHERY_PAGE_WIFI_PASSWORD: render_wifi_password(scr, flow); break;
    case ARCHERY_PAGE_DIAG: render_diag(scr, flow, history, battery_soc); break;
    case ARCHERY_PAGE_DIAG_LOG: render_diag_log(scr, flow); break;
    case ARCHERY_PAGE_TIMER: render_timer(scr, flow); break;
    }
    if (flow->page != ARCHERY_PAGE_RULER) {
        add_battery(scr, battery_soc);
        add_status_bar(scr, flow, s_wifi_view);
        if (flow->page == ARCHERY_PAGE_HOME) {
            lv_obj_t *version = make_label(scr, "v" ARCHERY_FIRMWARE_VERSION,
                                           ARCHERY_FONT_BODY, C_MUTED);
            lv_obj_set_pos(version, 8, 4);
        }
    } else {
        s_clock_label = NULL;
    }
    lv_screen_load(scr);
    if (previous) lv_obj_delete(previous);
}

void archery_ui_shift_ruler(int dx) {
    for (uint8_t i = 0; i < s_ruler_count; i++) {
        lv_obj_t *obj = s_ruler_items[i];
        if (obj) lv_obj_set_y(obj, lv_obj_get_y(obj) + dx);
    }
}

void archery_ui_clear(void) {
    s_ruler_count = 0;
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = NULL;
    }
    s_clock_label = NULL;
}


void archery_ui_update_clock(uint32_t now, bool set) {
    if (!s_clock_label) return;
    char text[8] = "--:--";
    if (set) {
        archery_datetime_t dt;
        archery_seconds_to_datetime(now, &dt);
        snprintf(text, sizeof(text), "%02u:%02u", (unsigned)dt.hour, (unsigned)dt.minute);
    }
    lv_label_set_text(s_clock_label, text);
}

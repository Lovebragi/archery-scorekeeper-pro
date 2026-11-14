// main/archery_flow.c —— 页面/按键状态机实现（无 LVGL、无 ESP-IDF 依赖）。
#include "archery_flow.h"
#include "archery_logbuf.h"

#include <string.h>

// 三个物理键的语义（界面按用户视角布局，屏幕倒装由 BSP 处理）：
//   上/下短按  调整当前项（设置值、各环箭数、列表选择、确认选项）
//   确定短按  推进/提交/进入
//   确定长按  返回/取消（危险操作先弹确认页）

bool archery_flow_target_has_faces(uint8_t target) {
    return target == ARCHERY_TARGET_TRIPLE_40H;
}

// 把每组箭数默认平均分到上/中/下三个靶面，余数优先给上、再给中。
static void split_faces(uint8_t total, uint8_t faces[ARCHERY_FACES]) {
    const uint8_t base = (uint8_t)(total / ARCHERY_FACES);
    const uint8_t rest = (uint8_t)(total % ARCHERY_FACES);
    faces[0] = (uint8_t)(base + (rest > 0 ? 1u : 0u));
    faces[1] = (uint8_t)(base + (rest > 1 ? 1u : 0u));
    faces[2] = base;
}

uint8_t archery_flow_setup_faces_total(const archery_flow_t *flow) {
    uint8_t total = 0;
    for (uint8_t i = 0; i < ARCHERY_FACES; i++) {
        total = (uint8_t)(total + flow->setup_face[i]);
    }
    return total;
}

void archery_flow_init(archery_flow_t *flow) {
    memset(flow, 0, sizeof(*flow));
    archery_sight_defaults(&flow->sight);
    flow->metro_bpm = 60;
    flow->metro_accent = 4;
    flow->metro_countin = 0;
    flow->metro_stop_min = 0;
    flow->metro_volume = 100;
    flow->range_target = 0;  // 默认 40 半，向上递增
    flow->range_mode = 0;
    flow->range_eye_cm = 60;
    flow->range_cursor_h = 0;
    flow->range_tilt_deg = 0;
    flow->range_cal_m = 18;
    flow->range_cal_row = 0;
    flow->range_cal_edit = 0;
    flow->range_cal_h = 0;
    flow->range_cal_k = 0;
    flow->page = ARCHERY_PAGE_HOME;
    flow->abort_return = ARCHERY_PAGE_ENTRY;
    flow->setup_groups = ARCHERY_DEFAULT_GROUPS;
    flow->setup_arrows = ARCHERY_DEFAULT_ARROWS;
    flow->setup_row = 0;
    split_faces(flow->setup_arrows, flow->setup_face);
    flow->home_select = 0;
    flow->history_cursor = 0;
    flow->history_filter = ARCHERY_HISTORY_FILTER_ALL;
    flow->detail_group = 0;
    flow->detail_groups = 1;
    flow->entry_row = 0;
    flow->entry_face = 0;
    flow->settings_row = 0;
    flow->clock_fields.year = ARCHERY_CLOCK_EPOCH_YEAR;
    flow->clock_fields.month = 1;
    flow->clock_fields.day = 1;
    flow->clock_row = 0;
    flow->clock_set = false;
    archery_settings_defaults(&flow->settings);
}

void archery_flow_set_settings(archery_flow_t *flow, const archery_settings_t *settings) {
    if (!flow || !settings || !archery_settings_valid(settings)) return;
    flow->settings = *settings;
}

void archery_flow_set_history_count(archery_flow_t *flow, uint16_t count) {
    flow->history_count = count;
    // 空历史只有"返回主页"一行；否则记录后还有"清空"与"返回"两行。
    const uint16_t max_cursor = count == 0 ? 0 : (uint16_t)(count + 1u);
    if (flow->history_cursor > max_cursor) flow->history_cursor = max_cursor;
}

void archery_flow_set_clock(archery_flow_t *flow, uint32_t now, bool time_set) {
    if (!flow) return;
    flow->clock_now = now;
    flow->clock_set = time_set;
}

static const char WIFI_PW_CHARS[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
#define WIFI_PW_CHAR_COUNT ((uint8_t)(sizeof(WIFI_PW_CHARS) - 1u))

char archery_wifi_password_char(uint8_t index) {
    return WIFI_PW_CHARS[index % WIFI_PW_CHAR_COUNT];
}

uint8_t archery_wifi_password_next(char current, int delta) {
    int index = 0;
    for (uint8_t i = 0; i < WIFI_PW_CHAR_COUNT; i++) {
        if (WIFI_PW_CHARS[i] == current) {
            index = i;
            break;
        }
    }
    int next = index + delta;
    while (next < 0) next += WIFI_PW_CHAR_COUNT;
    while (next >= WIFI_PW_CHAR_COUNT) next -= WIFI_PW_CHAR_COUNT;
    return (uint8_t)next;
}

void archery_flow_set_wifi_security(archery_flow_t *flow, uint8_t secure_mask) {
    if (!flow) return;
    flow->wifi_secure_mask = secure_mask;
}

void archery_flow_set_wifi_scan(archery_flow_t *flow, uint8_t ap_count) {
    if (!flow) return;
    flow->wifi_ap_count = ap_count;
    // 只有扫描列表的光标才受条数限制；菜单行（0..4）不能被 ap_count=0 拉回。
    if (flow->wifi_mode == 1 && flow->wifi_row > ap_count) {
        flow->wifi_row = ap_count;
    }
}

void archery_flow_set_wifi_provisioning(archery_flow_t *flow, bool active) {
    if (!flow) return;
    flow->wifi_provisioning = active;
}

// 物理按键角色映射：自然方向就是"上/下/确定"；整机倒装时角色转 180°。
archery_key_t archery_flow_button_key(archery_button_t button, bool rotate_roles) {
    if (rotate_roles) {
        switch (button) {
        case ARCHERY_BUTTON_UP: return ARCHERY_KEY_OK;
        case ARCHERY_BUTTON_DOWN: return ARCHERY_KEY_DOWN;
        default: return ARCHERY_KEY_UP;
        }
    }
    switch (button) {
    case ARCHERY_BUTTON_UP: return ARCHERY_KEY_UP;
    case ARCHERY_BUTTON_DOWN: return ARCHERY_KEY_DOWN;
    default: return ARCHERY_KEY_OK;
    }
}

bool archery_flow_button_is_ok(archery_button_t button, bool rotate_roles) {
    return rotate_roles ? button == ARCHERY_BUTTON_UP : button == ARCHERY_BUTTON_OK;
}

static void move_cursor(uint8_t *value, int delta, uint8_t min, uint8_t max) {
    int next = (int)*value + delta;
    if (next < min) next = min;
    if (next > max) next = max;
    *value = (uint8_t)next;
}

static void move_cursor16(uint16_t *value, int delta, uint16_t min, uint16_t max) {
    int next = (int)*value + delta;
    if (next < (int)min) next = (int)min;
    if (next > (int)max) next = (int)max;
    *value = (uint16_t)next;
}

// 历史页行数：空历史只显示一行"返回主页"；有记录时是 记录 + 清空 + 返回。
static uint16_t history_rows(const archery_flow_t *flow) {
    return flow->history_count == 0 ? 1u : (uint16_t)(flow->history_count + 2u);
}

static uint16_t history_max_cursor(const archery_flow_t *flow) {
    return (uint16_t)(history_rows(flow) - 1u);
}

// 进入新一组的录入：各靶面各环箭数清零，光标回到最高环。
static void begin_group_input(archery_flow_t *flow) {
    memset(flow->counts, 0, sizeof(flow->counts));
    flow->entry_row = 0;
    flow->entry_face = 0;
}

// 三联靶某一靶面在本组里的起始箭位。
static uint8_t face_arrow_offset(const archery_flow_t *flow, uint8_t face) {
    uint8_t offset = 0;
    for (uint8_t i = 0; i < face; i++) offset = (uint8_t)(offset + flow->setup_face[i]);
    return offset;
}

uint8_t archery_flow_voice_enabled(const archery_flow_t *flow) {
    return flow ? (flow->runtime_override ? flow->runtime_voice : flow->settings.voice_enabled) : 0u;
}

uint8_t archery_flow_roll_entry(const archery_flow_t *flow) {
    return flow ? (flow->runtime_override ? flow->runtime_roll : flow->settings.roll_entry) : 0u;
}

void archery_flow_set_runtime_entries(archery_flow_t *flow, bool voice, bool roll) {
    if (!flow) return;
    flow->runtime_override = 1;
    flow->runtime_voice = voice ? 1u : 0u;
    flow->runtime_roll = roll ? 1u : 0u;
}

void archery_flow_clear_runtime_entries(archery_flow_t *flow) {
    if (!flow) return;
    flow->runtime_override = 0;
    flow->runtime_voice = 0;
    flow->runtime_roll = 0;
}

void archery_flow_enter_setup(archery_flow_t *flow) {
    flow->setup_row = 0;
    if (archery_flow_target_has_faces(flow->settings.target) &&
        archery_flow_setup_faces_total(flow) != flow->setup_arrows) {
        split_faces(flow->setup_arrows, flow->setup_face);
    }
    flow->page = ARCHERY_PAGE_SETUP;
}

static archery_act_t handle_beta(archery_flow_t *flow, archery_key_t key) {
    switch (key) {
    case ARCHERY_KEY_UP:
#ifdef ARCHERY_MUSIC_EMBEDDED
        flow->beta_row = flow->beta_row == 0 ? 2u : (uint8_t)(flow->beta_row - 1u);
#else
        flow->beta_row = flow->beta_row == 0 ? 1u : 0u;
#endif
        break;
    case ARCHERY_KEY_DOWN:
#ifdef ARCHERY_MUSIC_EMBEDDED
        flow->beta_row = flow->beta_row >= 2u ? 0u : (uint8_t)(flow->beta_row + 1u);
#else
        flow->beta_row = flow->beta_row == 0 ? 1u : 0u;
#endif
        break;
    case ARCHERY_KEY_OK:
        if (flow->beta_row == 0) flow->settings.auto_mode = flow->settings.auto_mode ? 0u : 1u;
        else if (flow->beta_row == 1) flow->settings.auto_reconnect = flow->settings.auto_reconnect ? 0u : 1u;
#ifdef ARCHERY_MUSIC_EMBEDDED
        else return ARCHERY_ACT_MUSIC_TOGGLE;
#endif
        return ARCHERY_ACT_SETTINGS_CHANGED;
    case ARCHERY_KEY_OK_LONG:
        flow->settings_row = 11;
        flow->page = ARCHERY_PAGE_SETTINGS;
        return ARCHERY_ACT_SETTINGS_SAVE;
    }
    return ARCHERY_ACT_NONE;
}

static archery_act_t handle_auto_check(archery_flow_t *flow, archery_key_t key) {
    if (!flow->auto_check_done) return ARCHERY_ACT_NONE;
    if (key == ARCHERY_KEY_OK || key == ARCHERY_KEY_OK_LONG) {
        archery_flow_enter_setup(flow);
    }
    return ARCHERY_ACT_NONE;
}

static archery_act_t handle_home(archery_flow_t *flow, archery_key_t key) {
    switch (key) {
    case ARCHERY_KEY_UP:
        flow->home_select = flow->home_select == 0
                                ? (uint8_t)(ARCHERY_HOME_ITEMS - 1u)
                                : (uint8_t)(flow->home_select - 1u);
        break;
    case ARCHERY_KEY_DOWN:
        flow->home_select = (uint8_t)((flow->home_select + 1u) % ARCHERY_HOME_ITEMS);
        break;
    case ARCHERY_KEY_OK:
        if (flow->home_select == 0) {
            if (flow->settings.auto_mode) {
                flow->auto_check_done = 0;
                flow->auto_check_ok = 0;
                flow->page = ARCHERY_PAGE_AUTO_CHECK;
                return ARCHERY_ACT_AUTO_CHECK;
            }
            archery_flow_clear_runtime_entries(flow);
            archery_flow_enter_setup(flow);
        } else if (flow->home_select == 1) {
            flow->history_cursor = 0;
            flow->page = ARCHERY_PAGE_HISTORY;
        } else if (flow->home_select == 2) {
            archery_seconds_to_datetime(flow->clock_now, &flow->clock_fields);
            flow->clock_row = 0;
            flow->page = ARCHERY_PAGE_CLOCK;
        } else if (flow->home_select == 3) {
            flow->tools_row = 0;
            flow->page = ARCHERY_PAGE_TOOLS;
        } else if (flow->home_select == 4) {
            flow->settings_row = 0;
            flow->page = ARCHERY_PAGE_SETTINGS;
        } else {
            flow->wifi_mode = 0;
            flow->wifi_row = 0;
            flow->page = ARCHERY_PAGE_WIFI;
            return ARCHERY_ACT_WIFI_ENTER;
        }
        break;
    case ARCHERY_KEY_OK_LONG:
        // 主页长按确定进入诊断页。
        archery_flow_open_diag(flow);
        break;
    }
    return ARCHERY_ACT_NONE;
}

// 手动对时的字段调整：年/月/日会顺带把"日"钳到当月天数内。
static void clock_adjust(archery_datetime_t *dt, uint8_t field, int delta) {
    switch (field) {
    case 0: {
        int year = (int)dt->year + delta;
        if (year < (int)ARCHERY_CLOCK_MIN_YEAR) year = (int)ARCHERY_CLOCK_MIN_YEAR;
        if (year > (int)ARCHERY_CLOCK_MAX_YEAR) year = (int)ARCHERY_CLOCK_MAX_YEAR;
        dt->year = (uint16_t)year;
        break;
    }
    case 1: {
        int month = (int)dt->month + delta;
        if (month < 1) month = 1;
        if (month > 12) month = 12;
        dt->month = (uint8_t)month;
        break;
    }
    case 2: {
        int day = (int)dt->day + delta;
        const int max = (int)archery_clock_days_in_month(dt->year, dt->month);
        if (day < 1) day = 1;
        if (day > max) day = max;
        dt->day = (uint8_t)day;
        return;
    }
    case 3: {
        int hour = (int)dt->hour + delta;
        if (hour < 0) hour = 0;
        if (hour > 23) hour = 23;
        dt->hour = (uint8_t)hour;
        return;
    }
    default: {
        int minute = (int)dt->minute + delta;
        if (minute < 0) minute = 0;
        if (minute > 59) minute = 59;
        dt->minute = (uint8_t)minute;
        return;
    }
    }
    // 年/月变化后，把日钳进当月范围（例如 1/31 -> 2 月变成 2/28）。
    const int max = (int)archery_clock_days_in_month(dt->year, dt->month);
    if (dt->day > max) dt->day = (uint8_t)max;
}


// 联网页：菜单（扫描 / 手机扫码 / 立即对时 / 断开 / 返回）或扫描结果列表。
static archery_act_t handle_wifi(archery_flow_t *flow, archery_key_t key) {
    if (flow->wifi_mode == 2) {
        if (key == ARCHERY_KEY_OK) {
            flow->wifi_mode = 1;
            flow->wifi_row = 0;
            return ARCHERY_ACT_WIFI_SCAN;
        }
        if (key == ARCHERY_KEY_OK_LONG) {
            flow->wifi_mode = 0;
            flow->wifi_row = 0;
            flow->page = ARCHERY_PAGE_HOME;
            return ARCHERY_ACT_WIFI_LEAVE;
        }
        return ARCHERY_ACT_NONE;
    }
    if (flow->wifi_mode == 0) {
        switch (key) {
        case ARCHERY_KEY_UP:
            move_cursor(&flow->wifi_row, -1, 0, 5);
            break;
        case ARCHERY_KEY_DOWN:
            move_cursor(&flow->wifi_row, 1, 0, 5);
            break;
        case ARCHERY_KEY_OK:
            switch (flow->wifi_row) {
            case 0:
                flow->wifi_mode = 1;
                flow->wifi_row = 0;
                return ARCHERY_ACT_WIFI_SCAN;
            case 1:
                // 配网热点已开：再按"确定"是停止配网并留在联网菜单（UI 提示
                // "确定 停止配网"）。不区分状态地重复 start 会让短按看起来没反应。
                return flow->wifi_provisioning ? ARCHERY_ACT_WIFI_STOP
                                               : ARCHERY_ACT_WIFI_PROVISION;
            case 2:
                return ARCHERY_ACT_WIFI_SYNC_TIME;
            case 3:
                return ARCHERY_ACT_WIFI_STOP;
            case 4:
                return ARCHERY_ACT_VOICE_VERIFY;
            default:
                flow->wifi_mode = 0;
                flow->wifi_row = 0;
                flow->page = ARCHERY_PAGE_HOME;
                return ARCHERY_ACT_WIFI_LEAVE;
            }
            break;
        case ARCHERY_KEY_OK_LONG:
            flow->wifi_mode = 0;
            flow->wifi_row = 0;
            flow->page = ARCHERY_PAGE_HOME;
            return ARCHERY_ACT_WIFI_LEAVE;
        }
        return ARCHERY_ACT_NONE;
    }

    // 列表：0..ap_count-1 是网络，ap_count 是"返回"。
    switch (key) {
    case ARCHERY_KEY_UP:
        move_cursor(&flow->wifi_row, -1, 0, flow->wifi_ap_count);
        break;
    case ARCHERY_KEY_DOWN:
        move_cursor(&flow->wifi_row, 1, 0, flow->wifi_ap_count);
        break;
    case ARCHERY_KEY_OK:
        if (flow->wifi_row < flow->wifi_ap_count) {
            // 先进入密码页；开放网络可在密码页直接双击确定连接。
            flow->wifi_target = flow->wifi_row;
            memset(flow->wifi_password, 0, sizeof(flow->wifi_password));
            flow->wifi_pw_pos = 0;
            flow->wifi_pw_len = 0;
            flow->page = ARCHERY_PAGE_WIFI_PASSWORD;
            return ARCHERY_ACT_NONE;
        }
        flow->wifi_mode = 0;
        flow->wifi_row = 0;
        break;
    case ARCHERY_KEY_OK_LONG:
        flow->wifi_mode = 0;
        flow->wifi_row = 0;
        break;
    }
    return ARCHERY_ACT_NONE;
}
// 设备端密码输入：上/下换字符，确定下一位；空位按确定开始连接。
static archery_act_t handle_wifi_password(archery_flow_t *flow, archery_key_t key) {
    switch (key) {
    case ARCHERY_KEY_UP:
    case ARCHERY_KEY_DOWN: {
        if (flow->wifi_pw_pos >= 64u) break;
        const char current = flow->wifi_pw_pos < flow->wifi_pw_len
                                 ? flow->wifi_password[flow->wifi_pw_pos]
                                 : '0';
        const uint8_t next =
            archery_wifi_password_next(current, key == ARCHERY_KEY_UP ? 1 : -1);
        flow->wifi_password[flow->wifi_pw_pos] = archery_wifi_password_char(next);
        if (flow->wifi_pw_pos >= flow->wifi_pw_len) {
            flow->wifi_pw_len = (uint8_t)(flow->wifi_pw_pos + 1u);
        }
        flow->wifi_password[flow->wifi_pw_len] = '\0';
        break;
    }
    case ARCHERY_KEY_OK:
        if (flow->wifi_pw_pos < 64u) flow->wifi_pw_pos++;
        break;
    case ARCHERY_KEY_OK_LONG:
        if (flow->wifi_pw_pos > 0) {
            flow->wifi_pw_pos--;
            flow->wifi_pw_len = flow->wifi_pw_pos;
            flow->wifi_password[flow->wifi_pw_len] = '\0';
        } else {
            flow->page = ARCHERY_PAGE_WIFI;
            flow->wifi_mode = 1;
            flow->wifi_row = 0;
            return ARCHERY_ACT_WIFI_SCAN;
        }
        break;
    }
    return ARCHERY_ACT_NONE;
}

static archery_act_t handle_sight(archery_flow_t *flow, archery_key_t key) {
    const uint8_t count = flow->sight.count ? flow->sight.count : 1u;
    switch (key) {
    case ARCHERY_KEY_UP:
    case ARCHERY_KEY_DOWN: {
        const int delta = key == ARCHERY_KEY_UP ? 1 : -1;
        if (flow->sight_edit_field == 1) {
            int value = flow->sight.items[flow->sight_row].mark_tenths + delta;
            if (value < 0) value = 0;
            if (value > 9999) value = 9999;
            flow->sight.items[flow->sight_row].mark_tenths = (int16_t)value;
        } else if (flow->sight_edit_field == 2) {
            int distance = (int)flow->sight.items[flow->sight_row].distance_tenths + delta;
            if (distance < 10) distance = 10;
            if (distance > 2000) distance = 2000;
            flow->sight.items[flow->sight_row].distance_tenths = (uint16_t)distance;
        } else {
            int row = (int)flow->sight_row + (key == ARCHERY_KEY_UP ? -1 : 1);
            if (row < 0) row = (int)count - 1;
            if (row >= (int)count) row = 0;
            flow->sight_row = (uint8_t)row;
        }
        break;
    }
    case ARCHERY_KEY_OK:
        if (flow->sight_edit_field == 0) {
            flow->sight_edit_field = flow->sight_row >= 4u ? 2u : 1u;
        } else if (flow->sight_edit_field == 2) {
            flow->sight_edit_field = 1;
        } else {
            flow->sight_edit_field = 0;
        }
        flow->sight_editing = flow->sight_edit_field ? 1u : 0u;
        break;
    case ARCHERY_KEY_OK_LONG:
        if (flow->sight_edit_field != 0) {
            // 编辑态长按：只清除当前条目，留在瞄具页继续编辑。
            flow->sight.items[flow->sight_row].mark_tenths = 0;
            if (flow->sight_row >= 4u) flow->sight.items[flow->sight_row].distance_tenths = 0;
            return ARCHERY_ACT_SIGHT_SAVE;
        }
        flow->sight_editing = 0;
        flow->sight_edit_field = 0;
        flow->page = ARCHERY_PAGE_TOOLS;
        return ARCHERY_ACT_SIGHT_SAVE;
    }
    return ARCHERY_ACT_NONE;
}

static archery_act_t handle_tools(archery_flow_t *flow, archery_key_t key) {
    switch (key) {
    case ARCHERY_KEY_UP:
        flow->tools_row = flow->tools_row == 0 ? 4u : (uint8_t)(flow->tools_row - 1u);
        break;
    case ARCHERY_KEY_DOWN:
        flow->tools_row = flow->tools_row >= 4u ? 0u : (uint8_t)(flow->tools_row + 1u);
        break;
    case ARCHERY_KEY_OK:
        if (flow->tools_row == 0) {
            flow->sight_row = 0;
            flow->sight_editing = 0;
            flow->sight_edit_field = 0;
            flow->page = ARCHERY_PAGE_SIGHT;
        } else if (flow->tools_row == 1) {
            flow->calc_distance = 40;
            flow->calc_mode = ARCHERY_SIGHT_LINEAR;
            flow->page = ARCHERY_PAGE_SIGHT_CALC;
        } else if (flow->tools_row == 2) {
            flow->metro_row = 0;
            flow->metro_edit = 0;
            flow->page = ARCHERY_PAGE_METRONOME;
        } else if (flow->tools_row == 3) {
            flow->range_row = 0;
            flow->range_edit = 0;
            flow->page = ARCHERY_PAGE_RANGE_INTRO;
        } else {
            flow->ruler_offset = 0;
            flow->ruler_unit = 0;
            flow->ruler_mode = 0;
            flow->ruler_angle_tenths = 0;
            flow->page = ARCHERY_PAGE_RULER;
        }
        break;
    case ARCHERY_KEY_OK_LONG:
        flow->page = ARCHERY_PAGE_HOME;
        break;
    }
    return ARCHERY_ACT_NONE;
}

static archery_act_t handle_ruler(archery_flow_t *flow, archery_key_t key) {
    if (key == ARCHERY_KEY_UP) {
        if (flow->ruler_mode == 0) {
            flow->ruler_offset--;
            if (flow->ruler_offset < -19) flow->ruler_offset = -19;
        } else {
            flow->ruler_angle_tenths += 5;
            if (flow->ruler_angle_tenths > 150) flow->ruler_angle_tenths = 150;
        }
    } else if (key == ARCHERY_KEY_DOWN) {
        if (flow->ruler_mode == 0) {
            flow->ruler_offset++;
            if (flow->ruler_offset > 19) flow->ruler_offset = 19;
        } else {
            flow->ruler_angle_tenths -= 5;
            if (flow->ruler_angle_tenths < -150) flow->ruler_angle_tenths = -150;
        }
    } else if (key == ARCHERY_KEY_OK) {
        flow->ruler_mode = flow->ruler_mode ? 0u : 1u;
    } else if (key == ARCHERY_KEY_OK_LONG) {
        flow->page = ARCHERY_PAGE_TOOLS;
    }
    return ARCHERY_ACT_NONE;
}

static archery_act_t handle_sight_calc(archery_flow_t *flow, archery_key_t key) {
    if (key == ARCHERY_KEY_UP) {
        if (flow->calc_distance < 90) flow->calc_distance++;
    } else if (key == ARCHERY_KEY_DOWN) {
        if (flow->calc_distance > 10) flow->calc_distance--;
    } else if (key == ARCHERY_KEY_OK) {
        flow->calc_mode = flow->calc_mode == ARCHERY_SIGHT_LINEAR ? ARCHERY_SIGHT_QUADRATIC
                                                                  : ARCHERY_SIGHT_LINEAR;
    } else if (key == ARCHERY_KEY_OK_LONG) {
        flow->page = ARCHERY_PAGE_TOOLS;
    }
    return ARCHERY_ACT_NONE;
}

static archery_act_t handle_metronome(archery_flow_t *flow, archery_key_t key) {
    static const uint8_t stop_values[] = {0, 1, 3, 5, 10, 15};
    static const uint16_t preset_bpm[] = {40, 60, 90};
    switch (key) {
    case ARCHERY_KEY_UP:
    case ARCHERY_KEY_DOWN: {
        const int delta = key == ARCHERY_KEY_UP ? 1 : -1;
        if (!flow->metro_edit) {
            int row = (int)flow->metro_row + (key == ARCHERY_KEY_UP ? -1 : 1);
            if (row < 0) row = 7;
            if (row > 7) row = 0;
            flow->metro_row = (uint8_t)row;
            break;
        }
        switch (flow->metro_row) {
        case 1: {
            int v = (int)flow->metro_bpm + delta;
            if (v < 30) v = 30;
            if (v > 240) v = 240;
            flow->metro_bpm = (uint16_t)v;
            flow->metro_preset = 0;
            break;
        }
        case 2: {
            int v = (int)flow->metro_accent + delta;
            if (v < 1) v = 1;
            if (v > 16) v = 16;
            flow->metro_accent = (uint8_t)v;
            break;
        }
        case 3: {
            int v = (int)flow->metro_countin + delta;
            if (v < 0) v = 0;
            if (v > 8) v = 8;
            flow->metro_countin = (uint8_t)v;
            break;
        }
        case 4: {
            int v = (int)flow->settings.beep_volume + delta;
            if (v < 0) v = 0;
            if (v > 5) v = 5;
            if (v == flow->settings.beep_volume) break;
            flow->settings.beep_volume = (uint8_t)v;
            return ARCHERY_ACT_SETTINGS_CHANGED;
        }
        case 5: {
            uint8_t idx = 0;
            for (uint8_t i = 0; i < 6; i++) if (stop_values[i] == flow->metro_stop_min) idx = i;
            if (delta > 0 && idx + 1u < 6u) idx++;
            if (delta < 0 && idx > 0) idx--;
            flow->metro_stop_min = stop_values[idx];
            break;
        }
        case 6: {
            int p = (int)flow->metro_preset + delta;
            if (p < 0) p = 3;
            if (p > 3) p = 0;
            flow->metro_preset = (uint8_t)p;
            if (p >= 1 && p <= 3) flow->metro_bpm = preset_bpm[p - 1u];
            break;
        }
        case 7: {
            const uint8_t before = flow->metro_count_voice;
            flow->metro_count_voice = (uint8_t)((flow->metro_count_voice + (delta > 0 ? 1 : 5)) % 6u);
            if (flow->metro_count_voice != before) return ARCHERY_ACT_SETTINGS_CHANGED;
            break;
        }
        default:
            break;
        }
        break;
    }
    case ARCHERY_KEY_OK:
        if (flow->metro_row == 0) return ARCHERY_ACT_METRONOME_TOGGLE;
        flow->metro_edit = flow->metro_edit ? 0u : 1u;
        break;
    case ARCHERY_KEY_OK_LONG:
        flow->metro_edit = 0;
        flow->page = ARCHERY_PAGE_TOOLS;
        return ARCHERY_ACT_METRONOME_STOP;
    }
    return ARCHERY_ACT_NONE;
}

static archery_act_t handle_range_intro(archery_flow_t *flow, archery_key_t key) {
    if (key == ARCHERY_KEY_OK) flow->page = ARCHERY_PAGE_RANGE_CONFIG;
    else if (key == ARCHERY_KEY_OK_LONG) flow->page = ARCHERY_PAGE_TOOLS;
    return ARCHERY_ACT_NONE;
}

static uint16_t range_target_mm(uint8_t target) {
    static const uint16_t sizes[] = {200, 400, 400, 800, 1220};  // 40半/40全/80半/80全/122全
    return sizes[target < 5u ? target : 0u];
}

static uint16_t range_effective_mm(const archery_flow_t *flow) {
    static const uint16_t cos_permille[16] = {1000,1000,999,999,998,996,995,993,
                                              990,988,985,982,978,974,970,966};
    const uint8_t deg = flow->range_tilt_deg > 15u ? 15u : flow->range_tilt_deg;
    return (uint16_t)((uint32_t)range_target_mm(flow->range_target) *
                      cos_permille[deg] / 1000u);
}

static uint16_t range_cursor_height(const archery_flow_t *flow, uint16_t distance_m) {
    const uint32_t d_mm = (uint32_t)distance_m * 1000u;
    const uint32_t size = range_effective_mm(flow);
    if (d_mm == 0) return 0;
    const uint32_t k = flow->range_cal_k ? flow->range_cal_k :
                       (uint32_t)flow->range_eye_cm * 10u * 8u;
    const uint32_t total = (uint32_t)((uint64_t)k * size / d_mm);
    // 两条 2px 黄线之间的净空；不足时留最小净空。
    return (uint16_t)(total > 4u ? total - 4u : 1u);
}

static archery_act_t handle_range_config(archery_flow_t *flow, archery_key_t key) {
    switch (key) {
    case ARCHERY_KEY_UP:
    case ARCHERY_KEY_DOWN: {
        const int delta = key == ARCHERY_KEY_UP ? 1 : -1;
        if (!flow->range_edit) {
            int row = (int)flow->range_row + (key == ARCHERY_KEY_UP ? -1 : 1);
            if (row < 0) row = 4;
            if (row > 4) row = 0;
            flow->range_row = (uint8_t)row;
            break;
        }
        switch (flow->range_row) {
        case 0:
            flow->range_target = (uint8_t)((flow->range_target + (delta > 0 ? 1 : 4)) % 5u);
            flow->range_mode = 0;
            break;
        case 1: {
            int v = (int)flow->range_eye_cm + delta;
            if (v < 30) v = 30;
            if (v > 90) v = 90;
            if (v != flow->range_eye_cm) flow->range_cal_k = 0;  // 眼距变了，旧标定失效
            flow->range_eye_cm = (uint8_t)v;
            break;
        }
        case 3: {
            int v = (int)flow->range_tilt_deg + delta;
            if (v < 0) v = 0;
            if (v > 15) v = 15;
            flow->range_tilt_deg = (uint8_t)v;
            break;
        }
        default: break;
        }
        break;
    }
    case ARCHERY_KEY_OK:
        if (flow->range_row == 2) {
            flow->range_cal_row = 0;
            flow->range_cal_edit = 0;
            flow->range_cal_h = range_cursor_height(flow, flow->range_cal_m);
            flow->range_cal_h = 140;  // 默认半屏，方便上下移动
            if (flow->range_cal_h < 10) flow->range_cal_h = 10;
            if (flow->range_cal_h > 270) flow->range_cal_h = 270;
            flow->page = ARCHERY_PAGE_RANGE_CAL;
        } else if (flow->range_row == 4) {
            flow->range_edit = 0;
            if (flow->range_cursor_h == 0) {
                flow->range_cursor_h = range_cursor_height(flow, 30);
                if (flow->range_cursor_h < 10) flow->range_cursor_h = 10;
                if (flow->range_cursor_h > 270) flow->range_cursor_h = 270;
            }
            flow->page = ARCHERY_PAGE_RANGE_VIEW;
        } else {
            flow->range_edit = flow->range_edit ? 0u : 1u;
        }
        break;
    case ARCHERY_KEY_OK_LONG:
        flow->range_edit = 0;
        flow->page = ARCHERY_PAGE_TOOLS;
        break;
    }
    return ARCHERY_ACT_NONE;
}

static archery_act_t handle_range_cal(archery_flow_t *flow, archery_key_t key) {
    switch (key) {
    case ARCHERY_KEY_UP:
    case ARCHERY_KEY_DOWN: {
        const int delta = key == ARCHERY_KEY_UP ? 1 : -1;
        if (!flow->range_cal_edit) {
            int row = (int)flow->range_cal_row + (key == ARCHERY_KEY_UP ? -1 : 1);
            if (row < 0) row = 2;
            if (row > 2) row = 0;
            flow->range_cal_row = (uint8_t)row;
            break;
        }
        if (flow->range_cal_row == 0) {
            int v = (int)flow->range_cal_m + delta;
            if (v < 5) v = 5;
            if (v > 100) v = 100;
            flow->range_cal_m = (uint8_t)v;
        } else {
            int v = (int)flow->range_cal_h + delta;
            if (v < 10) v = 10;
            if (v > 270) v = 270;
            flow->range_cal_h = (uint16_t)v;
        }
        break;
    }
    case ARCHERY_KEY_OK:
        if (flow->range_cal_row == 2) {
            const uint32_t d_mm = (uint32_t)flow->range_cal_m * 1000u;
            const uint32_t size = range_effective_mm(flow);
            if (size > 0 && flow->range_cal_h >= 10) {
                const uint32_t a_cm = (uint32_t)((uint64_t)d_mm * flow->range_cal_h /
                                                 ((uint64_t)size * 8u * 10u));
                flow->range_cal_k = (uint32_t)((uint64_t)d_mm * flow->range_cal_h / size);
                if (a_cm < 30u) flow->range_eye_cm = 30;
                else if (a_cm > 90u) flow->range_eye_cm = 90;
                else flow->range_eye_cm = (uint8_t)a_cm;
            }
            flow->range_cal_edit = 0;
            flow->page = ARCHERY_PAGE_RANGE_CONFIG;
            flow->range_row = 2;
        } else {
            flow->range_cal_edit = flow->range_cal_edit ? 0u : 1u;
        }
        break;
    case ARCHERY_KEY_OK_LONG:
        flow->range_cal_edit = 0;
        flow->page = ARCHERY_PAGE_RANGE_CONFIG;
        flow->range_row = 3;
        break;
    }
    return ARCHERY_ACT_NONE;
}

static archery_act_t handle_range_view(archery_flow_t *flow, archery_key_t key) {
    if (key == ARCHERY_KEY_UP) {
        // 上键让游标上移 1 px，距离变小。
        if (flow->range_cursor_h < 270) flow->range_cursor_h++;
    } else if (key == ARCHERY_KEY_DOWN) {
        if (flow->range_cursor_h > 10) flow->range_cursor_h--;
    } else if (key == ARCHERY_KEY_OK) {
        flow->page = ARCHERY_PAGE_RANGE_CONFIG;
    } else if (key == ARCHERY_KEY_OK_LONG) {
        flow->page = ARCHERY_PAGE_TOOLS;
    }
    return ARCHERY_ACT_NONE;
}

static archery_act_t handle_clock(archery_flow_t *flow, archery_key_t key) {
    switch (key) {
    case ARCHERY_KEY_UP:
    case ARCHERY_KEY_DOWN:
        clock_adjust(&flow->clock_fields, flow->clock_row,
                     key == ARCHERY_KEY_UP ? 1 : -1);
        break;
    case ARCHERY_KEY_OK:
        if (flow->clock_row + 1u < ARCHERY_CLOCK_ROWS) {
            flow->clock_row++;
        } else {
            // 分字段按确定 = 保存对时并回到主页。
            flow->page = ARCHERY_PAGE_HOME;
            flow->home_select = 2;
            return ARCHERY_ACT_CLOCK_SAVE;
        }
        break;
    case ARCHERY_KEY_OK_LONG:
        flow->page = ARCHERY_PAGE_HOME;
        flow->home_select = 2;
        return ARCHERY_ACT_CLOCK_SAVE;
    }
    return ARCHERY_ACT_NONE;
}

static bool settings_cycle_value(archery_settings_t *settings, uint8_t row) {
    if (archery_settings_adjust(settings, row, 1)) return true;
    // 已到最大值：反向走到最小值，实现循环。
    while (archery_settings_adjust(settings, row, -1)) {
    }
    return true;
}

static archery_act_t handle_settings(archery_flow_t *flow, archery_key_t key) {
    switch (key) {
    case ARCHERY_KEY_UP:
        flow->settings_row = flow->settings_row == 0
                                 ? (uint8_t)(ARCHERY_SETTINGS_ROWS - 1u)
                                 : (uint8_t)(flow->settings_row - 1u);
        break;
    case ARCHERY_KEY_DOWN:
        flow->settings_row = flow->settings_row + 1u >= ARCHERY_SETTINGS_ROWS
                                 ? 0u : (uint8_t)(flow->settings_row + 1u);
        break;
    case ARCHERY_KEY_OK:
        if (flow->settings_row == 11u) {
            flow->beta_row = 0;
            flow->page = ARCHERY_PAGE_BETA;
            break;
        }
        if (flow->settings_row < ARCHERY_SETTINGS_ROWS &&
            settings_cycle_value(&flow->settings, flow->settings_row)) {
            // 靶型在下一组开始时生效；方向/亮度立即生效；息屏时间立即被空闲计时采用。
            return ARCHERY_ACT_SETTINGS_CHANGED;
        }
        break;
    case ARCHERY_KEY_OK_LONG:
        flow->page = ARCHERY_PAGE_HOME;
        return ARCHERY_ACT_SETTINGS_SAVE;
    }
    return ARCHERY_ACT_NONE;
}

static archery_act_t handle_setup(archery_flow_t *flow, archery_key_t key) {
    const bool faces = archery_flow_target_has_faces(flow->settings.target);
    const uint8_t start_row = faces ? 5u : 2u;
    switch (key) {
    case ARCHERY_KEY_UP:
    case ARCHERY_KEY_DOWN: {
        const int delta = key == ARCHERY_KEY_UP ? 1 : -1;
        if (flow->setup_row == 0) {
            flow->setup_groups = archery_groups_clamp((int)flow->setup_groups + delta);
        } else if (flow->setup_row == 1) {
            flow->setup_arrows = archery_arrows_clamp((int)flow->setup_arrows + delta);
            if (faces) split_faces(flow->setup_arrows, flow->setup_face);
        } else if (faces && flow->setup_row >= 2 && flow->setup_row <= 4) {
            const uint8_t face = (uint8_t)(flow->setup_row - 2u);
            move_cursor(&flow->setup_face[face], delta, 0, flow->setup_arrows);
        }
        break;
    }
    case ARCHERY_KEY_OK:
        if (flow->setup_row < start_row) {
            flow->setup_row++;
        } else if (!faces ||
                   archery_flow_setup_faces_total(flow) == flow->setup_arrows) {
            archery_session_begin(&flow->session, flow->setup_groups, flow->setup_arrows,
                                  flow->settings.target);
            if (flow->settings.timer_seconds > 0u) {
                flow->timer_phase = 0;
                flow->timer_remaining = 1;
                flow->timer_total = 0;
                flow->page = ARCHERY_PAGE_TIMER;
                return ARCHERY_ACT_TIMER_START;
            }
            begin_group_input(flow);
            flow->page = ARCHERY_PAGE_ENTRY;
            return ARCHERY_ACT_SESSION_START;
        }
        // 三联靶上/中/下合计与每组箭数不符时停在原地，由界面提示。
        break;
    case ARCHERY_KEY_OK_LONG:
        // 长按确定 = 返回上一项；在第一项上长按返回主页。
        if (flow->setup_row > 0) {
            flow->setup_row--;
        } else {
            flow->page = ARCHERY_PAGE_HOME;
        }
        break;
    }
    return ARCHERY_ACT_NONE;
}

static archery_act_t handle_entry(archery_flow_t *flow, archery_key_t key) {
    const bool faces = archery_flow_target_has_faces(flow->session.target);
    const uint8_t face = faces ? flow->entry_face : 0;
    const uint8_t slots = archery_target_slot_count(flow->session.target);
    // 关闭"记录内十"时，录入页从 10 环开始；entry_row 是可见行号，槽位要加上偏移。
    const uint8_t first = archery_target_entry_first_slot(flow->session.target,
                                                          flow->settings.record_x);
    const uint8_t visible = (uint8_t)(slots - first);
    const uint8_t face_arrows = faces ? flow->setup_face[face] : flow->session.arrows;
    uint8_t *counts = flow->counts[face];

    switch (key) {
    case ARCHERY_KEY_UP:
    case ARCHERY_KEY_DOWN:
        if (flow->entry_row < visible) {
            archery_counts_adjust(counts, (uint8_t)(first + flow->entry_row),
                                  key == ARCHERY_KEY_UP ? 1 : -1,
                                  face_arrows, flow->session.target);
        }
        break;
    case ARCHERY_KEY_OK:
        if (flow->entry_row < visible) {
            // 该靶面的箭数已经凑满：直接落到提交行。
            if (archery_counts_total(counts) == face_arrows) {
                flow->entry_row = visible;
            } else {
                flow->entry_row++;
            }
        } else if (archery_counts_total(counts) == face_arrows &&
                   archery_session_apply_face_counts(
                       &flow->session, face_arrow_offset(flow, face), face_arrows,
                       counts)) {
            // 三联靶先切到下一靶面；最后一个靶面提交整组。
            if (faces && face + 1u < ARCHERY_FACES) {
                flow->entry_face = (uint8_t)(face + 1u);
                flow->entry_row = 0;
            } else {
                archery_session_commit_group(&flow->session);
                flow->page = ARCHERY_PAGE_SUMMARY;
            }
        } else {
            // 还没记满：回到该靶面第一行继续补。
            flow->entry_row = 0;
        }
        break;
    case ARCHERY_KEY_OK_LONG:
        // 长按确定 = 返回上一项；三联靶在第一行长按会退回上一靶的提交行。
        if (flow->entry_row > 0) {
            flow->entry_row--;
        } else if (faces && flow->entry_face > 0) {
            flow->entry_face--;
            flow->entry_row = visible;
        } else {
            flow->abort_return = ARCHERY_PAGE_ENTRY;
            flow->confirm_yes = false;
            flow->page = ARCHERY_PAGE_ABORT_CONFIRM;
        }
        break;
    }
    return ARCHERY_ACT_NONE;
}

static archery_act_t handle_summary(archery_flow_t *flow, archery_key_t key) {
    switch (key) {
    case ARCHERY_KEY_OK:
        if (archery_session_complete(&flow->session)) {
            flow->save_failed = false;
            flow->page = ARCHERY_PAGE_FINISH;
            return ARCHERY_ACT_SAVE_RECORD;
        }
        if (flow->settings.timer_seconds > 0u) {
            flow->timer_phase = 0;
            flow->timer_remaining = 1;
            flow->timer_total = 0;
            flow->page = ARCHERY_PAGE_TIMER;
            return ARCHERY_ACT_TIMER_START;
        }
        begin_group_input(flow);
        flow->page = ARCHERY_PAGE_ENTRY;
        break;
    case ARCHERY_KEY_UP:
    case ARCHERY_KEY_DOWN:
        break;
    case ARCHERY_KEY_OK_LONG:
        flow->abort_return = ARCHERY_PAGE_SUMMARY;
        flow->confirm_yes = false;
        flow->page = ARCHERY_PAGE_ABORT_CONFIRM;
        break;
    }
    return ARCHERY_ACT_NONE;
}

static archery_act_t handle_history(archery_flow_t *flow, archery_key_t key) {
    switch (key) {
    case ARCHERY_KEY_UP:
    case ARCHERY_KEY_DOWN: {
        const int delta = key == ARCHERY_KEY_UP ? -1 : 1;
        const uint16_t max = history_max_cursor(flow);
        // 首尾循环：到底再往下回到第一条，到顶再往上跳到最后一条。
        if (delta > 0 && flow->history_cursor >= max) {
            flow->history_cursor = 0;
        } else if (delta < 0 && flow->history_cursor == 0) {
            flow->history_cursor = max;
        } else {
            move_cursor16(&flow->history_cursor, delta, 0, max);
        }
        break;
    }
    case ARCHERY_KEY_OK:
        if (flow->history_count == 0) {
            // 空历史的唯一一行是"返回主页"。
            flow->page = ARCHERY_PAGE_HOME;
        } else if (flow->history_cursor < flow->history_count) {
            flow->detail_group = 0;
            flow->page = ARCHERY_PAGE_DETAIL;
        } else if (flow->history_cursor == flow->history_count) {
            // "清空全部记录"行
            flow->confirm_yes = false;
            flow->page = ARCHERY_PAGE_CLEAR_CONFIRM;
        } else {
            // "返回主页"行
            flow->page = ARCHERY_PAGE_HOME;
        }
        break;
    case ARCHERY_KEY_OK_LONG:
        // 长按确定 = 直接回主页，和其他页面保持一致。
        flow->page = ARCHERY_PAGE_HOME;
        break;
    }
    return ARCHERY_ACT_NONE;
}

archery_act_t archery_flow_history_cycle_filter(archery_flow_t *flow) {
    if (!flow || flow->page != ARCHERY_PAGE_HISTORY) return ARCHERY_ACT_NONE;
    flow->history_filter = (uint8_t)((flow->history_filter + 1u) %
                                     ARCHERY_HISTORY_FILTER_COUNT);
    flow->history_cursor = 0;
    return ARCHERY_ACT_HISTORY_FILTER;
}

static archery_act_t handle_detail(archery_flow_t *flow, archery_key_t key) {
    const uint8_t max_group =
        flow->detail_groups == 0 ? 0 : (uint8_t)(flow->detail_groups - 1u);
    switch (key) {
    case ARCHERY_KEY_UP:
        if (flow->detail_group > 0) flow->detail_group--;
        break;
    case ARCHERY_KEY_DOWN:
        if (flow->detail_group < max_group) flow->detail_group++;
        break;
    case ARCHERY_KEY_OK:
    case ARCHERY_KEY_OK_LONG:
        flow->page = ARCHERY_PAGE_HISTORY;
        break;
    }
    return ARCHERY_ACT_NONE;
}

static archery_act_t handle_clear_confirm(archery_flow_t *flow, archery_key_t key) {
    switch (key) {
    case ARCHERY_KEY_UP:
    case ARCHERY_KEY_DOWN:
        flow->confirm_yes = !flow->confirm_yes;
        break;
    case ARCHERY_KEY_OK:
        if (flow->confirm_yes) {
            flow->page = ARCHERY_PAGE_HISTORY;
            flow->history_cursor = 0;
            flow->history_count = 0;
            return ARCHERY_ACT_CLEAR_HISTORY;
        }
        flow->page = ARCHERY_PAGE_HISTORY;
        break;
    case ARCHERY_KEY_OK_LONG:
        flow->page = ARCHERY_PAGE_HISTORY;
        break;
    }
    return ARCHERY_ACT_NONE;
}

static archery_act_t handle_abort_confirm(archery_flow_t *flow, archery_key_t key) {
    switch (key) {
    case ARCHERY_KEY_UP:
    case ARCHERY_KEY_DOWN:
        flow->confirm_yes = !flow->confirm_yes;
        break;
    case ARCHERY_KEY_OK:
        if (flow->confirm_yes) {
            // 放弃本次训练：回到主页，未提交的成绩不写历史。
            flow->page = ARCHERY_PAGE_HOME;
        } else {
            flow->page = flow->abort_return;
        }
        break;
    case ARCHERY_KEY_OK_LONG:
        flow->page = flow->abort_return;
        break;
    }
    return ARCHERY_ACT_NONE;
}

void archery_flow_set_log_count(archery_flow_t *flow, uint8_t count) {
    if (!flow) return;
    flow->diag_log_count = count > ARCHERY_LOG_LINES ? ARCHERY_LOG_LINES : count;
    if (flow->diag_log_count > 0 && flow->diag_log_row >= flow->diag_log_count) {
        flow->diag_log_row = (uint8_t)(flow->diag_log_count - 1u);
    }
}

void archery_flow_open_diag(archery_flow_t *flow) {
    if (!flow) return;
    flow->diag_row = 0;
    flow->diag_log_row = 0;
    flow->page = ARCHERY_PAGE_DIAG;
}

void archery_flow_enter_entry_from_timer(archery_flow_t *flow) {
    if (!flow || flow->page != ARCHERY_PAGE_TIMER) return;
    begin_group_input(flow);
    flow->page = ARCHERY_PAGE_ENTRY;
}

static archery_act_t handle_timer(archery_flow_t *flow, archery_key_t key) {
    switch (key) {
    case ARCHERY_KEY_OK:
        archery_flow_enter_entry_from_timer(flow);
        return ARCHERY_ACT_TIMER_STOP;
    case ARCHERY_KEY_OK_LONG:
        flow->page = ARCHERY_PAGE_SETUP;
        return ARCHERY_ACT_TIMER_STOP;
    default:
        break;
    }
    return ARCHERY_ACT_NONE;
}

static archery_act_t handle_diag(archery_flow_t *flow, archery_key_t key) {
    switch (key) {
    case ARCHERY_KEY_UP:
        flow->diag_row = flow->diag_row == 0 ? (uint8_t)(ARCHERY_DIAG_ROWS - 1u)
                                             : (uint8_t)(flow->diag_row - 1u);
        break;
    case ARCHERY_KEY_DOWN:
        flow->diag_row = flow->diag_row + 1u >= ARCHERY_DIAG_ROWS
                             ? 0u : (uint8_t)(flow->diag_row + 1u);
        break;
    case ARCHERY_KEY_OK:
        flow->diag_log_row = 0;
        flow->page = ARCHERY_PAGE_DIAG_LOG;
        break;
    case ARCHERY_KEY_OK_LONG:
        flow->page = ARCHERY_PAGE_HOME;
        break;
    }
    return ARCHERY_ACT_NONE;
}

static archery_act_t handle_diag_log(archery_flow_t *flow, archery_key_t key) {
    const uint8_t max = flow->diag_log_count > 0 ? (uint8_t)(flow->diag_log_count - 1u) : 0u;
    switch (key) {
    case ARCHERY_KEY_UP:
        flow->diag_log_row = flow->diag_log_row == 0 ? max
                                                     : (uint8_t)(flow->diag_log_row - 1u);
        break;
    case ARCHERY_KEY_DOWN:
        flow->diag_log_row = flow->diag_log_row >= max ? 0u
                                                       : (uint8_t)(flow->diag_log_row + 1u);
        break;
    case ARCHERY_KEY_OK:
        flow->page = ARCHERY_PAGE_DIAG;
        break;
    case ARCHERY_KEY_OK_LONG:
        flow->page = ARCHERY_PAGE_HOME;
        break;
    }
    return ARCHERY_ACT_NONE;
}

archery_act_t archery_flow_handle(archery_flow_t *flow, archery_key_t key) {
    switch (flow->page) {
    case ARCHERY_PAGE_HOME: return handle_home(flow, key);
    case ARCHERY_PAGE_SETUP: return handle_setup(flow, key);
    case ARCHERY_PAGE_ENTRY: return handle_entry(flow, key);
    case ARCHERY_PAGE_SUMMARY: return handle_summary(flow, key);
    case ARCHERY_PAGE_FINISH:
        // 完成页：任意确定回到主页。
        if (key == ARCHERY_KEY_OK || key == ARCHERY_KEY_OK_LONG) {
            flow->page = ARCHERY_PAGE_HOME;
        }
        return ARCHERY_ACT_NONE;
    case ARCHERY_PAGE_HISTORY: return handle_history(flow, key);
    case ARCHERY_PAGE_DETAIL: return handle_detail(flow, key);
    case ARCHERY_PAGE_CLEAR_CONFIRM: return handle_clear_confirm(flow, key);
    case ARCHERY_PAGE_ABORT_CONFIRM: return handle_abort_confirm(flow, key);
    case ARCHERY_PAGE_SETTINGS: return handle_settings(flow, key);
    case ARCHERY_PAGE_CLOCK: return handle_clock(flow, key);
    case ARCHERY_PAGE_WIFI: return handle_wifi(flow, key);
    case ARCHERY_PAGE_WIFI_PASSWORD: return handle_wifi_password(flow, key);
    case ARCHERY_PAGE_DIAG: return handle_diag(flow, key);
    case ARCHERY_PAGE_DIAG_LOG: return handle_diag_log(flow, key);
    case ARCHERY_PAGE_TIMER: return handle_timer(flow, key);
    case ARCHERY_PAGE_AUTO_CHECK: return handle_auto_check(flow, key);
    case ARCHERY_PAGE_BETA: return handle_beta(flow, key);
    case ARCHERY_PAGE_SIGHT: return handle_sight(flow, key);
    case ARCHERY_PAGE_TOOLS: return handle_tools(flow, key);
    case ARCHERY_PAGE_RULER: return handle_ruler(flow, key);
    case ARCHERY_PAGE_SIGHT_CALC: return handle_sight_calc(flow, key);
    case ARCHERY_PAGE_METRONOME: return handle_metronome(flow, key);
    case ARCHERY_PAGE_RANGE_INTRO: return handle_range_intro(flow, key);
    case ARCHERY_PAGE_RANGE_CONFIG: return handle_range_config(flow, key);
    case ARCHERY_PAGE_RANGE_VIEW: return handle_range_view(flow, key);
    case ARCHERY_PAGE_RANGE_CAL: return handle_range_cal(flow, key);
    }
    return ARCHERY_ACT_NONE;
}

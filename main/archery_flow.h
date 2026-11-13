// main/archery_flow.h —— 页面与按键的纯状态机。
//
// UI 层只负责把 flow 画出来；持久化和计时等副作用由 archery_app.c 根据返回的
// action 执行。这样页面流转、边界钳制和按钮语义都可以在主机上单测。
#pragma once

#include "archery_clock.h"
#include "archery_session.h"
#include "archery_settings.h"
#include "archery_sight.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ARCHERY_PAGE_HOME = 0,
    ARCHERY_PAGE_SETUP,
    ARCHERY_PAGE_ENTRY,
    ARCHERY_PAGE_SUMMARY,
    ARCHERY_PAGE_FINISH,
    ARCHERY_PAGE_HISTORY,
    ARCHERY_PAGE_DETAIL,
    ARCHERY_PAGE_CLEAR_CONFIRM,
    ARCHERY_PAGE_ABORT_CONFIRM,
    ARCHERY_PAGE_SETTINGS,
    ARCHERY_PAGE_CLOCK,
    ARCHERY_PAGE_WIFI,
    ARCHERY_PAGE_WIFI_PASSWORD,
    ARCHERY_PAGE_DIAG,
    ARCHERY_PAGE_DIAG_LOG,
    ARCHERY_PAGE_TIMER,
    ARCHERY_PAGE_AUTO_CHECK,
    ARCHERY_PAGE_BETA,
    ARCHERY_PAGE_SIGHT,
    ARCHERY_PAGE_TOOLS,
    ARCHERY_PAGE_RULER,
    ARCHERY_PAGE_SIGHT_CALC,
    ARCHERY_PAGE_METRONOME,
    ARCHERY_PAGE_RANGE_INTRO,
    ARCHERY_PAGE_RANGE_CONFIG,
    ARCHERY_PAGE_RANGE_VIEW,
} archery_page_t;

// 主页入口数：开始训练 / 历史记录 / 设置 / 联网。
#define ARCHERY_HOME_ITEMS 6u

// 录入页行数上限：最多 12 个环值槽位 + 1 行"确认提交"。
#define ARCHERY_ENTRY_ROWS_MAX (ARCHERY_RING_SLOT_MAX + 1u)
// 三联靶的靶面数：上 / 中 / 下。
#define ARCHERY_FACES 3u
// 新训练页行数上限：组数 + 箭数 + 上/中/下 + 开始。
#define ARCHERY_SETUP_ROWS_MAX 6u
// 手动对时页的字段数：年 / 月 / 日 / 时 / 分。
#define ARCHERY_CLOCK_ROWS 5u
#define ARCHERY_DIAG_ROWS 13u

typedef enum {
    ARCHERY_KEY_UP = 0,
    ARCHERY_KEY_DOWN,
    ARCHERY_KEY_OK,
    ARCHERY_KEY_OK_LONG,
} archery_key_t;

// 物理按键在机身侧边按键列中的位置（自然方向自上而下：上、下、确定）。
typedef enum {
    ARCHERY_BUTTON_UP = 0,
    ARCHERY_BUTTON_DOWN,
    ARCHERY_BUTTON_OK,
} archery_button_t;

// 物理按键 → 逻辑键。整机倒装时按键列的上下顺序也反了，rotate_roles 会把角色
// 整体旋转 180°：物理上键当确定、物理下键仍是下、物理确定键当上。
archery_key_t archery_flow_button_key(archery_button_t button, bool rotate_roles);

// 该物理键在逻辑上是否承担"确定"（长按 = 返回/取消）。
bool archery_flow_button_is_ok(archery_button_t button, bool rotate_roles);

typedef enum {
    ARCHERY_ACT_NONE = 0,
    ARCHERY_ACT_SESSION_START,  // 开始新训练：上层记录起始时间
    ARCHERY_ACT_SAVE_RECORD,    // 训练完成：上层把会话写入历史
    ARCHERY_ACT_CLEAR_HISTORY,  // 清空全部历史记录
    ARCHERY_ACT_SETTINGS_CHANGED,  // 设置项改了：上层立即应用亮度/方向/息屏
    ARCHERY_ACT_SETTINGS_SAVE,     // 离开设置页：上层把设置写入 NVS
    ARCHERY_ACT_CLOCK_SAVE,        // 手动对时完成：上层设置时钟并持久化
    ARCHERY_ACT_HISTORY_FILTER,    // 历史筛选变了：上层重算可见条数
    ARCHERY_ACT_WIFI_ENTER,        // 进入联网页：上层刷新连接状态
    ARCHERY_ACT_WIFI_SCAN,         // 扫描附近 Wi-Fi
    ARCHERY_ACT_WIFI_CONNECT,      // 连接选中的网络（flow->wifi_row）
    ARCHERY_ACT_WIFI_PROVISION,    // 打开手机扫码配网（设备热点 + 网页）
    ARCHERY_ACT_WIFI_SYNC_TIME,    // 立即用 NTP 对时
    ARCHERY_ACT_MUSIC_TOGGLE,      // Beta：播放/停止内置音乐
    ARCHERY_ACT_WIFI_STOP,         // 断开/停止配网
    ARCHERY_ACT_WIFI_LEAVE,
    ARCHERY_ACT_AUTO_CHECK,
    ARCHERY_ACT_SIGHT_SAVE,
    ARCHERY_ACT_METRONOME_TOGGLE,
    ARCHERY_ACT_METRONOME_STOP,
    ARCHERY_ACT_VOICE_VERIFY,        // 离开联网页：停止配网热点
    ARCHERY_ACT_WIFI_CONNECT_PASSWORD,  // 用设备上输入的密码连接（flow->wifi_password）
    ARCHERY_ACT_TIMER_START,            // 进入计时页：上层启动 1 秒 + 10 秒准备 + 射击倒计时
    ARCHERY_ACT_TIMER_STOP,             // 离开计时页：上层停止计时
} archery_act_t;

typedef struct {
    archery_page_t page;
    archery_page_t abort_return;  // 取消"放弃训练"后回到的页面
    archery_session_t session;
    uint8_t setup_groups;   // 设置页当前值，默认 6
    uint8_t setup_arrows;   // 每组箭数，默认 6
    uint8_t setup_row;      // 0 = 组数, 1 = 每组箭数, 2..4 = 上/中/下, 最后 = 开始
    uint8_t setup_face[ARCHERY_FACES];  // 三联靶上/中/下各几根箭
    uint8_t home_select;    // 0 训练, 1 历史, 2 时间, 3 设置, 4 联网, 5 射箭工具
    uint16_t history_count; // 由上层同步：当前筛选下的记录数
    uint8_t diag_row;
    uint8_t diag_log_row;
    uint8_t diag_log_count;
    uint8_t timer_phase;      // 0 = 1 秒延迟，1 = 10 秒准备，2 = 射击倒计时
    uint16_t timer_remaining; // 当前阶段剩余秒数
    uint16_t timer_total;     // 射击阶段总秒数
    uint16_t history_cursor; // 0..count+1（count = 清空行，count+1 = 返回行）
    uint8_t history_filter; // archery_history_filter_t
    uint8_t detail_group;   // 历史详情当前查看的组
    uint8_t detail_groups;  // 当前查看记录的总组数（进入详情时由上层设置）
    bool confirm_yes;       // 确认页当前选择
    bool save_failed;       // 上层写入历史失败时置位，完成页据此提示
    uint8_t counts[ARCHERY_FACES][ARCHERY_RING_SLOT_MAX];  // 各靶面各环箭数
    uint8_t entry_face;     // 三联靶当前录入的靶面 0..2
    uint8_t entry_row;      // 0..槽位数-1 为环值行，槽位数为提交行
    archery_settings_t settings;  // 靶型与省电设置（由上层从 NVS 载入）
    uint8_t settings_row;   // 0 = 靶型, 1 = 方向, 2 = 亮度, 3 = 息屏, 4 = 日期时间, 5 = 返回
    uint32_t clock_now;     // 上层每次处理按键/刷新界面前同步的当前时间
    archery_datetime_t clock_fields;  // 手动对时页正在编辑的字段
    uint8_t clock_row;      // 0..4 = 年/月/日/时/分
    bool clock_set;         // 是否已经手动校准过（显示"时间未校准"提示）
    uint8_t wifi_row;       // 联网页光标
    uint8_t wifi_mode;      // 0 = 菜单, 1 = 扫描结果列表
    bool wifi_provisioning; // 配网热点是否正在运行（由上层同步，决定"确定"是开还是停）
    uint8_t wifi_ap_count;  // 扫描到的网络数（列表末行是"返回"）
    uint8_t runtime_override; // 1 = 使用本次运行覆盖（不写用户设置）
    uint8_t runtime_voice;
    uint8_t runtime_roll;
    uint8_t auto_check_ok;    // 自动训练检测结果
    uint8_t auto_check_done;
    uint8_t beta_row;
    uint8_t tools_row;
    uint8_t sight_row;
    uint8_t sight_editing;
    uint8_t sight_edit_field;  // 0 无, 1 刻度, 2 距离
    int16_t ruler_offset;
    uint16_t calc_distance;
    uint8_t calc_mode;
    uint8_t metro_row;
    uint8_t metro_edit;
    uint8_t metro_running;
    uint8_t metro_beat;
    uint16_t metro_bpm;
    uint8_t metro_accent;
    uint8_t metro_countin;
    uint8_t metro_stop_min;
    uint8_t metro_volume;
    uint8_t metro_preset;
    uint8_t metro_count_voice;
    uint8_t music_playing;
    uint8_t range_row;
    uint8_t range_edit;
    uint8_t range_target;
    uint8_t range_mode;
    uint8_t range_eye_cm;
    uint8_t range_distance_m;
    archery_sight_t sight;
    uint8_t wifi_secure_mask;  // 扫描结果里哪些是加密网络（bit0 = 第 1 个）
    uint8_t wifi_target;    // 正在输入密码的网络下标
    char wifi_password[65]; // 设备上输入的密码
    uint8_t wifi_pw_pos;    // 当前编辑位置
    uint8_t wifi_pw_len;    // 已输入长度
} archery_flow_t;

void archery_flow_init(archery_flow_t *flow);

// 该靶型是否按上/中/下三个靶面分别录入（目前只有三联靶）。
bool archery_flow_target_has_faces(uint8_t target);

// 三联靶设置页里上/中/下箭数的合计。
uint8_t archery_flow_setup_faces_total(const archery_flow_t *flow);

// 用 NVS 里读到的设置覆盖 flow 中的副本（开机时调用，设置页直接改这个副本）。
void archery_flow_set_settings(archery_flow_t *flow, const archery_settings_t *settings);

// 处理一次按键：更新状态并返回需要上层执行的副作用。
archery_act_t archery_flow_handle(archery_flow_t *flow, archery_key_t key);

// 历史记录数量变化（写入新记录、清空或切换筛选后）由上层同步进来，重新钳制光标。
void archery_flow_set_history_count(archery_flow_t *flow, uint16_t count);

// 历史页双击确定：循环切换筛选并回到第一条。
archery_act_t archery_flow_history_cycle_filter(archery_flow_t *flow);

// 打开诊断页（主页长按确定 3 秒后调用）。
void archery_flow_open_diag(archery_flow_t *flow);
void archery_flow_set_log_count(archery_flow_t *flow, uint8_t count);

// 计时结束或跳过：进入本组录入页。
void archery_flow_enter_entry_from_timer(archery_flow_t *flow);

// 当前时间（秒）由上层同步，供手动对时页初始化和"今天/最近 7 天"筛选使用。
void archery_flow_set_clock(archery_flow_t *flow, uint32_t now, bool time_set);

// 扫描结果条数由上层同步，列表光标会随之钳制。
void archery_flow_set_wifi_scan(archery_flow_t *flow, uint8_t ap_count);
void archery_flow_set_wifi_security(archery_flow_t *flow, uint8_t secure_mask);
void archery_flow_enter_setup(archery_flow_t *flow);
uint8_t archery_flow_voice_enabled(const archery_flow_t *flow);
uint8_t archery_flow_roll_entry(const archery_flow_t *flow);
void archery_flow_set_runtime_entries(archery_flow_t *flow, bool voice, bool roll);
void archery_flow_clear_runtime_entries(archery_flow_t *flow);
// 配网热点开关状态由上层同步；菜单第 2 行在配网中再按"确定"表示停止配网。
void archery_flow_set_wifi_provisioning(archery_flow_t *flow, bool active);

// 设备端密码输入使用的小工具（也便于主机测试）。
char archery_wifi_password_char(uint8_t index);
uint8_t archery_wifi_password_next(char current, int delta);

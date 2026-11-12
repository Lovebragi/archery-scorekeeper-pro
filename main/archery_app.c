// main/archery_app.c —— 把 BSP、纯状态机、NVS 与界面装配成应用。
//
// 线程模型：
//   * button 组件的回调（共享 esp_timer 任务）只把按键事件放进队列；
//   * 唯一的 archery 任务消费队列，在其中更新 flow、读写 NVS 并重建 LVGL 页面
//     （重建前获取 bsp_lvgl_lock）；
//   * 队列空闲 500ms 时刷新电量；超过设置的空闲时间后息屏并进入浅睡，
//     任意键唤醒并回到原来的页面。
#include "archery_app.h"

#include "archery_flow.h"
#include "archery_logbuf.h"
#include "archery_name.h"
#include "archery_history.h"
#include "archery_metro.h"
#include "archery_score.h"
#include "archery_settings.h"
#include "archery_store.h"
#include "archery_ui.h"
#include "archery_version.h"
#include "archery_voice.h"
#include "archery_wifi.h"
#include "archery_wifi_store.h"
#include "archery_wifi_view.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "key_guard.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "archery_app";

#define KEY_QUEUE_DEPTH 8
#define BATTERY_POLL_MS 10000
#define WORKER_STACK_BYTES 6144
// 息屏浅睡后，唤醒键还按着时先吞掉按键动作，直到松手（最长兜底 5 秒）。
#define WAKE_GUARD_MAX_US (5LL * 1000 * 1000)

typedef enum {
    APP_MSG_KEY = 0,
    APP_MSG_WIFI,
    APP_MSG_VOICE_START,
    APP_MSG_VOICE_STOP,
    APP_MSG_HISTORY_FILTER,
    APP_MSG_DIAG,
    APP_MSG_TIMER_TICK,
    APP_MSG_REFRESH,
    APP_MSG_VOICE_OK,
    APP_MSG_VOICE_FAIL,
    APP_MSG_AUTO_CHECK_RESULT,
    APP_MSG_METRONOME_TICK,
} app_msg_type_t;

typedef struct {
    uint8_t type;
    archery_key_t key;
} app_message_t;

static QueueHandle_t s_queue;
static TaskHandle_t s_worker;
static archery_flow_t s_flow;
static archery_history_t s_history;
static int s_battery_soc = -1;
static int64_t s_session_start_us;
static int64_t s_last_input_us;
static int64_t s_next_battery_us;
static volatile bool s_ready;
// 按键角色是否随屏幕 180° 倒转。按钮任务读取，设置页修改，用独立的 volatile
// 变量避免和 flow 结构体的其它字段一起被读到中间状态。
static volatile bool s_rotate_keys = (ARCHERY_SETTINGS_DEFAULT_ROTATE != 0);
// 联网页/配网中不浅睡，但长时间停留时只关背光，保持热点。
static bool s_wifi_dimmed;
static esp_timer_handle_t s_ptt_timer;
static volatile bool s_ptt_used;
// 历史列表按住上/下自动连发；400 ms 后启动，之后每 60 ms 一步。
static esp_timer_handle_t s_repeat_timer;
static volatile bool s_repeat_active;
static volatile bool s_repeat_used;
static volatile bool s_repeat_slow;
static volatile bool s_repeat_inflight;
static uint32_t s_repeat_period_us = 60000;
static archery_key_t s_repeat_key;
// 按键唤醒后，那颗键在整个启动期间还按着；在它松手前丢弃按键动作，
// 避免把"唤醒键仍在按"误判成长按。
static key_guard_t s_key_guard;
static int64_t s_wake_click_until_us;
static int64_t s_next_wifi_retry_us;
static uint8_t s_wifi_retry_step;
static bool s_wifi_prev_connected;
static bool s_wifi_need_all;
static esp_timer_handle_t s_diag_timer;
static esp_timer_handle_t s_timer_tick;
static bool s_timer_running;
static bool s_timer_warned;
static uint8_t s_timer_voice_left;
static QueueHandle_t s_countdown_q;
static int64_t s_timer_deadline_us;
// 墙上时钟：基准时间 + 单调计时器。设备没有 RTC 电池，断电后时间从上次保存
// 的时刻继续；设置页可手动校准。
static uint32_t s_clock_base;
static int64_t s_clock_base_us;
static bool s_clock_set;
static uint32_t s_clock_saved;
static int64_t s_next_clock_save_us;
static esp_timer_handle_t s_metro_timer;
static uint32_t s_metro_tick_count;
static int64_t s_metro_start_us;
static uint32_t s_last_clock_minute;
static uint32_t s_session_when;
static archery_wifi_view_t s_wifi_view;
// 密码页长按上/下连发：方向、下一次触发时间、是否吞掉松开时的单击。
static uint8_t s_pw_dir;
static int64_t s_pw_next_step;
static bool s_pw_swallow_click;

// Wi-Fi 任务发来的通知：只入队，界面在主任务里刷新。
static void on_wifi_event(void *user) {
    (void)user;
    if (!s_queue) return;
    const app_message_t message = {.type = APP_MSG_WIFI};
    (void)xQueueSend(s_queue, &message, 0);
}

static uint32_t clock_now(void) {
    const int64_t elapsed = esp_timer_get_time() - s_clock_base_us;
    if (elapsed <= 0) return s_clock_base;
    return s_clock_base + (uint32_t)(elapsed / 1000000);
}

static void clock_set_to(uint32_t seconds) {
    s_clock_base = seconds;
    s_clock_base_us = esp_timer_get_time();
    s_clock_set = seconds != 0;
}

// 首次开机没有保存过时间时，用固件编译时间做近似种子（比 1970 年更接近真实）。
static uint32_t clock_build_seed(void) {
    archery_datetime_t dt;
    if (!archery_clock_parse_build(__DATE__, __TIME__, &dt)) return 0;
    return archery_datetime_to_seconds(&dt);
}

static void clock_persist(bool force) {
    const uint32_t now = clock_now();
    if (!force && now <= s_clock_saved + 60u) return;
    if (archery_store_clock_save(now, s_clock_set) == ESP_OK) {
        s_clock_saved = now;
    }
}

// 把 Wi-Fi 视图同步到 flow（扫描条数、加密网络与配网状态）。
static void sync_wifi_flow(void) {
    archery_wifi_fill_view(&s_wifi_view);
    archery_flow_set_wifi_scan(&s_flow, s_wifi_view.ap_count);
    uint8_t mask = 0;
    for (uint8_t i = 0; i < s_wifi_view.ap_count && i < 8; i++) {
        if (s_wifi_view.secure[i]) mask = (uint8_t)(mask | (1u << i));
    }
    archery_flow_set_wifi_security(&s_flow, mask);
    archery_flow_set_wifi_provisioning(&s_flow, archery_wifi_provisioning());
}

static void redraw(void) {
    archery_flow_set_log_count(&s_flow, (uint8_t)archery_logbuf_count());
    archery_flow_set_clock(&s_flow, clock_now(), s_clock_set);
    sync_wifi_flow();
    archery_ui_set_wifi_view(&s_wifi_view);
    archery_ui_set_voice_status(archery_flow_voice_enabled(&s_flow) ? archery_voice_status()
                                                              : "");
    if (!bsp_lvgl_lock(500)) {
        ESP_LOGW(TAG, "获取 LVGL 锁超时，跳过本次重绘");
        return;
    }
    archery_ui_show(&s_flow, &s_history, s_battery_soc);
    bsp_lvgl_unlock();
}

static void refresh_battery(void) {
    const int soc = bsp_battery_soc();
    if (soc == s_battery_soc) return;
    s_battery_soc = soc;
    redraw();
}

static void sync_detail_group(void) {
    if (s_flow.page != ARCHERY_PAGE_DETAIL) return;
    const archery_record_t *record = archery_history_visible_record(
        &s_history, s_flow.history_filter, clock_now(), s_flow.history_cursor);
    s_flow.detail_groups = record ? record->groups : 1;
    if (s_flow.detail_group >= s_flow.detail_groups) {
        s_flow.detail_group = (uint8_t)(s_flow.detail_groups - 1u);
    }
}

// 按当前筛选重新统计可见条数，并让 flow 的光标保持在范围内。
static void refresh_history_count(void) {
    const uint16_t count =
        archery_history_visible_count(&s_history, s_flow.history_filter, clock_now());
    archery_flow_set_history_count(&s_flow, count);
}

static void apply_settings(const archery_settings_t *settings) {
    archery_flow_clear_runtime_entries(&s_flow);
    // 画面方向与亮度立即生效；息屏时间由 worker 的空闲判断直接读取 flow 里的副本。
    // 改 MADCTL 时先拿 LVGL 锁，避免和正在进行的刷屏并发。
    s_rotate_keys = settings->rotate != 0;
    archery_voice_set_beep_volume((uint8_t)(settings->beep_volume * 20u));
    archery_voice_set_countdown_voice(settings->countdown_voice);
    archery_wifi_set_auto_reconnect(settings->auto_reconnect != 0);
    archery_wifi_set_save_enabled(settings->wifi_save != 0);
    if (!settings->wifi_save) archery_wifi_forget_credentials();
    if (bsp_lvgl_lock(200)) {
        bsp_display_backlight(settings->brightness);
        (void)bsp_display_set_flip_180(settings->rotate != 0);
        bsp_lvgl_unlock();
    }
}

static bool s_saved_pending;
static uint8_t s_saved_row;

static bool try_saved_wifi(archery_key_t key) {
    if (key != ARCHERY_KEY_OK || s_flow.page != ARCHERY_PAGE_WIFI ||
        s_flow.wifi_mode != 1 || s_flow.wifi_row >= s_flow.wifi_ap_count) {
        return false;
    }
    if (!s_flow.settings.wifi_save) return false;
    const uint8_t row = s_flow.wifi_row;
    if (!s_wifi_view.secure[row]) return false;
    char pass[ARCHERY_WIFI_PASS_SIZE];
    const bool found =
        archery_wifi_store_lookup(s_wifi_view.ssid[row], pass, sizeof(pass));
    ESP_LOGI(TAG, "saved wifi row=%u found=%d passlen=%u", (unsigned)row,
             (int)found, (unsigned)strlen(pass));
    if (!found) return false;
    if (pass[0] == 0) {
        // 旧记录可能保存了空密码（安全网络无法用空密码连接），直接让用户重输。
        s_flow.page = ARCHERY_PAGE_WIFI_PASSWORD;
        s_flow.wifi_mode = 1;
        s_flow.wifi_target = row;
        s_flow.wifi_pw_pos = 0;
        s_flow.wifi_pw_len = 0;
        memset(s_flow.wifi_password, 0, sizeof(s_flow.wifi_password));
        snprintf(s_wifi_view.message, sizeof(s_wifi_view.message), "请重新输入密码");
        redraw();
        return true;
    }
    // 回到联网状态页，让"正在连接 / 已连接"可见。
    s_flow.wifi_mode = 0;
    s_flow.wifi_row = 0;
    const esp_err_t err = archery_wifi_connect(s_wifi_view.ssid[row], pass);
    ESP_LOGI(TAG, "saved wifi connect err=%d", (int)err);
    if (err == ESP_OK) {
        s_saved_pending = true;
        s_saved_row = row;
    } else {
        // 保存的密码无法发起连接：直接回到密码页，允许重输。
        s_flow.page = ARCHERY_PAGE_WIFI_PASSWORD;
        s_flow.wifi_mode = 1;
        s_flow.wifi_target = row;
        s_flow.wifi_pw_pos = 0;
        s_flow.wifi_pw_len = 0;
        memset(s_flow.wifi_password, 0, sizeof(s_flow.wifi_password));
        snprintf(s_wifi_view.message, sizeof(s_wifi_view.message), "保存的密码连接失败，请重输");
    }
    redraw();
    return true;
}

// Wi-Fi 任务的事件：刷新视图、同步扫描条数，若拿到 NTP 时间就更新时钟。
// 密码页：按住上/下时自动连发，避免 62 个字符一个个点。
static void password_repeat_poll(void) {
    if (s_flow.page != ARCHERY_PAGE_WIFI_PASSWORD) {
        s_pw_dir = 0;
        s_pw_swallow_click = false;
        return;
    }
    // 先按电压判断"物理键"，再套用屏幕方向对应的角色旋转：
    // 倒装时物理上键是逻辑确定、物理确定键才是逻辑上。
    const int mv = bsp_button_read_mv();
    archery_button_t physical = ARCHERY_BUTTON_OK;
    bool pressed = false;
    if (mv >= 0 && mv < 150) {
        physical = ARCHERY_BUTTON_UP;
        pressed = true;
    } else if (mv >= 150 && mv < 447) {
        physical = ARCHERY_BUTTON_DOWN;
        pressed = true;
    } else if (mv >= 447 && mv < BSP_BTN_MV_RELEASED_MIN) {
        physical = ARCHERY_BUTTON_OK;
        pressed = true;
    }
    uint8_t dir = 0;
    if (pressed) {
        const archery_key_t logical = archery_flow_button_key(physical, s_rotate_keys);
        if (logical == ARCHERY_KEY_UP) {
            dir = 1;
        } else if (logical == ARCHERY_KEY_DOWN) {
            dir = 2;
        }
    }
    const int64_t now = esp_timer_get_time();
    if (dir == 0) {
        s_pw_dir = 0;
        return;
    }
    if (dir != s_pw_dir) {
        s_pw_dir = dir;
        s_pw_next_step = now + 350000;  // 按住 350ms 后开始连发
        s_pw_swallow_click = true;
        (void)archery_flow_handle(&s_flow,
                                  dir == 1 ? ARCHERY_KEY_UP : ARCHERY_KEY_DOWN);
        redraw();
        return;
    }
    if (now >= s_pw_next_step) {
        (void)archery_flow_handle(&s_flow,
                                  dir == 1 ? ARCHERY_KEY_UP : ARCHERY_KEY_DOWN);
        s_pw_next_step = now + 60000;  // 约 16 次/秒
        redraw();
    }
}

static void handle_wifi_event(void) {
    sync_wifi_flow();
    if (s_wifi_view.state == ARCHERY_WIFI_CONNECTED) archery_wifi_commit_credentials();
    if (s_saved_pending) {
        if (s_wifi_view.state == ARCHERY_WIFI_FAILED) {
            s_saved_pending = false;
            s_flow.page = ARCHERY_PAGE_WIFI_PASSWORD;
            s_flow.wifi_mode = 1;
            s_flow.wifi_target = s_saved_row;
            s_flow.wifi_pw_pos = 0;
            s_flow.wifi_pw_len = 0;
            memset(s_flow.wifi_password, 0, sizeof(s_flow.wifi_password));
            archery_voice_set_status("密码错误，请重新输入");
            snprintf(s_wifi_view.message, sizeof(s_wifi_view.message), "密码错误，请重新输入");
        } else if (s_wifi_view.state == ARCHERY_WIFI_CONNECTED) {
            s_saved_pending = false;
        }
    }
    if (!s_saved_pending && s_wifi_view.state == ARCHERY_WIFI_FAILED &&
        s_flow.page == ARCHERY_PAGE_WIFI_PASSWORD) {
        s_flow.page = ARCHERY_PAGE_WIFI;
        s_flow.wifi_mode = 2;  // 显示连接失败结果，确定后回扫描
    }
    uint32_t seconds = 0;
    if (archery_wifi_take_time(&seconds)) {
        clock_set_to(seconds);
        clock_persist(true);
    }
    redraw();
}

static void beep_sequence(int count) {
    for (int i = 0; i < count; i++) {
        archery_voice_preview_beep();
        if (i + 1 < count) vTaskDelay(pdMS_TO_TICKS(400));
    }
}

static void countdown_say(uint8_t number) {
    if (s_countdown_q) (void)xQueueSend(s_countdown_q, &number, 0);
}

static void countdown_voice_task(void *arg) {
    (void)arg;
    uint8_t number;
    for (;;) {
        if (xQueueReceive(s_countdown_q, &number, portMAX_DELAY) == pdTRUE) {
            switch (number) {
            case 0xFFu: archery_voice_beep_warning(); break;
            case 0xFEu: beep_sequence(2); break;
            case 0xFDu: beep_sequence(1); break;
            case 0xFCu: beep_sequence(3); break;
            default: archery_voice_play_countdown(number); break;
            }
        }
    }
}

static void timer_stop_sequence(void) {
    s_timer_running = false;
    if (s_countdown_q) (void)xQueueReset(s_countdown_q);
    if (s_timer_tick) (void)esp_timer_stop(s_timer_tick);
}

static void timer_start_sequence(void) {
    s_timer_running = true;
    s_timer_warned = false;
    s_timer_voice_left = 0;
    s_flow.timer_remaining = 10;
    s_timer_deadline_us = esp_timer_get_time() + 1000000;
    if (s_timer_tick) {
        (void)esp_timer_stop(s_timer_tick);
        (void)esp_timer_start_periodic(s_timer_tick, 50000);
    }
}

static uint16_t timer_seconds_left(int64_t now) {
    const int64_t left = s_timer_deadline_us - now;
    if (left <= 0) return 0;
    return (uint16_t)((left + 999999) / 1000000);
}

static void handle_timer_tick(void) {
    if (!s_timer_running || s_flow.page != ARCHERY_PAGE_TIMER) {
        timer_stop_sequence();
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (s_flow.timer_phase == 0) {
        if (now < s_timer_deadline_us) return;
        beep_sequence(2);
        s_flow.timer_phase = 1;
        s_flow.timer_remaining = 10;
        s_timer_deadline_us = now + 10000000;
    } else if (s_flow.timer_phase == 1) {
        const uint16_t left = timer_seconds_left(now);
        if (left != s_flow.timer_remaining) {
            s_flow.timer_remaining = left;
            if (left == 0) {
                uint32_t total = (uint32_t)s_flow.session.arrows *
                                 (uint32_t)s_flow.settings.timer_seconds;
                if (total == 0) total = 1;
                if (total > UINT16_MAX) total = UINT16_MAX;
                s_flow.timer_phase = 2;
                s_flow.timer_total = (uint16_t)total;
                s_flow.timer_remaining = (uint16_t)total;
                s_timer_deadline_us = now + (int64_t)total * 1000000;
                s_timer_warned = false;
                s_timer_voice_left = 0;
                beep_sequence(1);
            }
        }
    } else {
        const uint16_t left = timer_seconds_left(now);
        const bool changed = left != s_flow.timer_remaining;
        s_flow.timer_remaining = left;
        if (!s_timer_warned && left <= 30 && s_flow.timer_total > 30) {
            s_timer_warned = true;
            countdown_say(0xFFu);
        }
        if (s_flow.settings.countdown_voice && left >= 1 && left <= 10 &&
            left != s_timer_voice_left) {
            s_timer_voice_left = (uint8_t)left;
            countdown_say((uint8_t)left);
        }
        if (left == 0) {
            s_timer_voice_left = 0;
            beep_sequence(3);
            timer_stop_sequence();
            archery_flow_enter_entry_from_timer(&s_flow);
        } else if (!changed && left == s_flow.timer_remaining) {
            return;
        }
    }
    if (s_flow.page == ARCHERY_PAGE_TIMER) {
        archery_ui_timer_tick(&s_flow);
    } else {
        redraw();
    }
}

void archery_app_voice_verified(bool ok) {
    if (!s_queue) return;
    const app_message_t message = {.type = ok ? APP_MSG_VOICE_OK : APP_MSG_VOICE_FAIL};
    (void)xQueueSend(s_queue, &message, 0);
}

void archery_app_refresh(void) {
    if (!s_queue) return;
    const app_message_t message = {.type = APP_MSG_REFRESH};
    (void)xQueueSend(s_queue, &message, 0);
}

static volatile bool s_auto_checking;
static int64_t s_auto_deadline_us;
static int64_t s_auto_check_started_us;

static void metro_stop(void) {
    if (s_metro_timer) (void)esp_timer_stop(s_metro_timer);
    s_flow.metro_running = 0;
    s_flow.metro_beat = 0;
    s_metro_tick_count = 0;
    const archery_metro_config_t cfg = {
        .bpm = s_flow.metro_bpm,
        .accent = s_flow.metro_accent,
        .countin = s_flow.metro_countin,
        .stop_min = s_flow.metro_stop_min,
        .volume = s_flow.metro_volume,
        .preset = s_flow.metro_preset,
        .count_voice = s_flow.metro_count_voice,
    };
    (void)archery_metro_save(&cfg);
    (void)archery_settings_save(&s_flow.settings);
    redraw();
}

static void metro_timer_cb(void *arg) {
    (void)arg;
    if (!s_queue) return;
    const app_message_t message = {.type = APP_MSG_METRONOME_TICK};
    (void)xQueueSend(s_queue, &message, 0);
}

static void metro_start(void) {
    if (s_metro_timer == NULL) {
        const esp_timer_create_args_t args = {.callback = metro_timer_cb, .name = "metro"};
        if (esp_timer_create(&args, &s_metro_timer) != ESP_OK) return;
    }
    (void)esp_timer_stop(s_metro_timer);
    s_metro_tick_count = 0;
    s_metro_start_us = esp_timer_get_time();
    s_flow.metro_beat = 0;
    s_flow.metro_running = 1;
    const uint32_t period_us = 60000000u / (s_flow.metro_bpm ? s_flow.metro_bpm : 60u);
    if (esp_timer_start_periodic(s_metro_timer, period_us) != ESP_OK) {
        s_flow.metro_running = 0;
    }
    redraw();
}

static void metro_tick(void) {
    if (!s_flow.metro_running) return;
    s_metro_tick_count++;
    const uint8_t countin = s_flow.metro_countin;
    if (countin > 0 && s_metro_tick_count <= countin) {
        s_flow.metro_beat = 0;
        const uint8_t accent_every = s_flow.metro_accent ? s_flow.metro_accent : 4u;
        const bool accent = ((s_metro_tick_count - 1u) % accent_every) == 0u;
        archery_voice_metronome_click(accent, (uint8_t)(s_flow.settings.beep_volume * 20u));
    } else {
        const uint32_t music_tick = s_metro_tick_count - countin - 1u;
        const uint8_t accent = s_flow.metro_accent ? s_flow.metro_accent : 4u;
        const uint8_t beat = (uint8_t)(music_tick % accent + 1u);
        s_flow.metro_beat = beat;
        const uint8_t volume = (uint8_t)(s_flow.settings.beep_volume * 20u);
        if (s_flow.metro_count_voice) {
            if (beat == 1u) {
                const uint8_t bar = (uint8_t)((music_tick / accent) % 4u + 1u);
                if (s_flow.metro_count_voice == 2u) {
                    archery_voice_metronome_bao_bar(bar, volume);
                } else {
                    archery_voice_metronome_bar(bar, volume);
                }
            } else {
                archery_voice_metronome_click(false, volume);
            }
        } else {
            archery_voice_metronome_click(beat == 1u, volume);
        }
    }
    if (s_flow.metro_stop_min > 0 && s_metro_start_us > 0 &&
        esp_timer_get_time() - s_metro_start_us >=
            (int64_t)s_flow.metro_stop_min * 60 * 1000000) {
        metro_stop();
        return;
    }
    redraw();
}

static void auto_check_task(void *arg) {
    (void)arg;
    // 后台只做网络检测，不碰设置和界面。
    const bool ok = archery_voice_test_credentials();
    if (s_queue) {
        const app_message_t message = {
            .type = APP_MSG_AUTO_CHECK_RESULT,
            .key = ok ? ARCHERY_KEY_OK : ARCHERY_KEY_DOWN,
        };
        (void)xQueueSend(s_queue, &message, 0);
    }
    vTaskDelete(NULL);
}

static void apply_auto_check_result(bool ok) {
    s_auto_checking = false;
    archery_flow_set_runtime_entries(&s_flow, ok, !ok);
    s_flow.auto_check_ok = ok ? 1u : 0u;
    s_flow.auto_check_done = 1;
    s_auto_check_started_us = 0;
    s_auto_deadline_us = esp_timer_get_time() + 3000000;
    archery_voice_set_status(ok ? "自动检测：语音可用" : "自动检测：语音不可用");
    redraw();
}

static void auto_check_start(void) {
    if (s_auto_checking) return;
    if (!archery_voice_has_credentials()) {
        apply_auto_check_result(false);
        return;
    }
    s_auto_checking = true;
    s_flow.auto_check_done = 0;
    s_auto_check_started_us = esp_timer_get_time();
    s_auto_deadline_us = 0;
    archery_voice_set_status("正在检测语音服务");
    if (xTaskCreate(auto_check_task, "auto_check", 8192, NULL, 3, NULL) != pdPASS) {
        apply_auto_check_result(false);
    }
    redraw();
}

static void handle_key(archery_key_t key) {
    const bool was_ruler = s_flow.page == ARCHERY_PAGE_RULER;
    const int16_t ruler_before = s_flow.ruler_offset;
    const bool was_calc = s_flow.page == ARCHERY_PAGE_SIGHT_CALC;
    const uint16_t calc_distance_before = s_flow.calc_distance;
    const uint8_t calc_mode_before = s_flow.calc_mode;
    const bool was_metro = s_flow.page == ARCHERY_PAGE_METRONOME;
    const uint16_t metro_bpm_before = s_flow.metro_bpm;
    const archery_act_t action = archery_flow_handle(&s_flow, key);
    const bool ruler_moved = was_ruler && s_flow.page == ARCHERY_PAGE_RULER &&
                             s_flow.ruler_offset != ruler_before;
    const bool calc_changed = was_calc && s_flow.page == ARCHERY_PAGE_SIGHT_CALC &&
                              (s_flow.calc_distance != calc_distance_before ||
                               s_flow.calc_mode != calc_mode_before);
    switch (action) {
    case ARCHERY_ACT_SESSION_START:
        s_session_start_us = esp_timer_get_time();
        s_session_when = clock_now();
        break;
    case ARCHERY_ACT_TIMER_START:
        if (s_flow.session.group == 0) {
            s_session_start_us = esp_timer_get_time();
            s_session_when = clock_now();
        }
        timer_start_sequence();
        break;
    case ARCHERY_ACT_TIMER_STOP:
        timer_stop_sequence();
        break;
    case ARCHERY_ACT_SAVE_RECORD: {
        const int64_t elapsed_us = esp_timer_get_time() - s_session_start_us;
        uint32_t seconds = elapsed_us > 0 ? (uint32_t)(elapsed_us / 1000000) : 0u;
        if (seconds > UINT16_MAX) seconds = UINT16_MAX;
        archery_record_t record;
        archery_record_from_session(&record, &s_flow.session, 0, (uint16_t)seconds,
                                    s_session_when);
        if (archery_store_save(&s_history, &record) != ESP_OK) {
            s_flow.save_failed = true;
            ESP_LOGE(TAG, "训练记录写入失败");
        }
        clock_persist(false);
        refresh_history_count();
        break;
    }
    case ARCHERY_ACT_CLEAR_HISTORY:
        if (archery_store_clear(&s_history) != ESP_OK) {
            ESP_LOGE(TAG, "清空历史失败，保留原有记录");
        }
        refresh_history_count();
        break;
    case ARCHERY_ACT_SETTINGS_CHANGED:
        apply_settings(&s_flow.settings);
        if (s_flow.settings_row == ARCHERY_SETTINGS_ROW_BEEP_VOLUME) {
            archery_voice_preview_beep();
        } else if (s_flow.page == ARCHERY_PAGE_METRONOME && (s_flow.metro_row == 4u || s_flow.metro_row == 7u)) {
            if (!s_flow.metro_running) {
                if (s_flow.metro_row == 7u && s_flow.metro_count_voice == 2u) {
                    archery_voice_metronome_bao_bar(1, (uint8_t)(s_flow.settings.beep_volume * 20u));
                } else if (s_flow.metro_row == 7u && s_flow.metro_count_voice == 1u) {
                    archery_voice_metronome_bar(1, (uint8_t)(s_flow.settings.beep_volume * 20u));
                } else {
                    archery_voice_metronome_click(
                        true, (uint8_t)(s_flow.settings.beep_volume * 20u));
                }
            }
        }
        break;
    case ARCHERY_ACT_SETTINGS_SAVE:
        apply_settings(&s_flow.settings);
        if (archery_settings_save(&s_flow.settings) != ESP_OK) {
            ESP_LOGE(TAG, "设置保存失败");
        }
        break;
    case ARCHERY_ACT_CLOCK_SAVE: {
        const uint32_t seconds = archery_datetime_to_seconds(&s_flow.clock_fields);
        if (seconds != 0) {
            clock_set_to(seconds);
            clock_persist(true);
            archery_flow_set_clock(&s_flow, seconds, true);
        }
        break;
    }
    case ARCHERY_ACT_HISTORY_FILTER:
        refresh_history_count();
        break;

    case ARCHERY_ACT_WIFI_ENTER:
        archery_wifi_fill_view(&s_wifi_view);
        archery_flow_set_wifi_scan(&s_flow, s_wifi_view.ap_count);
        break;
    case ARCHERY_ACT_WIFI_SCAN:
        (void)archery_wifi_scan();
        break;
    case ARCHERY_ACT_WIFI_CONNECT:
        (void)archery_wifi_connect_index(s_flow.wifi_row);
        break;
    case ARCHERY_ACT_WIFI_CONNECT_PASSWORD: {
        const uint8_t index = s_flow.wifi_target;
        if (index < ARCHERY_WIFI_AP_MAX) {
            char ssid[ARCHERY_WIFI_SSID_MAX];
            strncpy(ssid, s_wifi_view.ssid[index], sizeof(ssid) - 1);
            ssid[sizeof(ssid) - 1] = '\0';
            (void)archery_wifi_connect(ssid, s_flow.wifi_password);
        }
        break;
    }
    case ARCHERY_ACT_WIFI_PROVISION:
        // flow 依据 wifi_provisioning 选择开或停；这里再查一次运行态，
        // 避免事件回调尚未同步时把"停止"又变成一次幂等的 start。
        if (archery_wifi_provisioning()) {
            (void)archery_wifi_stop_provisioning();
        } else {
            (void)archery_wifi_start_provisioning(NULL);
        }
        break;
    case ARCHERY_ACT_WIFI_SYNC_TIME:
        (void)archery_wifi_sync_time();
        break;
    case ARCHERY_ACT_WIFI_STOP:
        if (archery_wifi_provisioning()) {
            (void)archery_wifi_stop_provisioning();
        } else {
            (void)archery_wifi_disconnect();
        }
        break;
    case ARCHERY_ACT_WIFI_LEAVE:
        (void)archery_wifi_stop_provisioning();
        break;
    case ARCHERY_ACT_VOICE_VERIFY:
        archery_wifi_verify_voice_async();
        break;
    case ARCHERY_ACT_AUTO_CHECK:
        auto_check_start();
        break;
    case ARCHERY_ACT_SIGHT_SAVE:
        if (!archery_sight_save(&s_flow.sight)) ESP_LOGW(TAG, "sight save failed");
        break;
    case ARCHERY_ACT_METRONOME_TOGGLE:
        if (s_flow.metro_running) metro_stop();
        else metro_start();
        break;
    case ARCHERY_ACT_METRONOME_STOP:
        metro_stop();
        break;
    case ARCHERY_ACT_NONE:
        break;
    }
    // BPM 在播放中也能立即生效。
    if (was_metro && s_flow.page == ARCHERY_PAGE_METRONOME && s_flow.metro_running &&
        s_flow.metro_bpm != metro_bpm_before && s_metro_timer) {
        (void)esp_timer_stop(s_metro_timer);
        const uint32_t period_us = 60000000u / (s_flow.metro_bpm ? s_flow.metro_bpm : 60u);
        (void)esp_timer_start_periodic(s_metro_timer, period_us);
    }
    s_repeat_inflight = false;
    sync_detail_group();
    if (s_flow.page == ARCHERY_PAGE_HOME) {
        archery_flow_clear_runtime_entries(&s_flow);
    }
    if (was_ruler && s_flow.page == ARCHERY_PAGE_RULER) {
        if (ruler_moved && bsp_lvgl_lock(100)) {
            archery_ui_shift_ruler((int)(s_flow.ruler_offset - ruler_before) * 8);
            bsp_lvgl_unlock();
        }
        // 到达边界时 offset 不再变化，绝不能重绘整页，否则连发会压垮 LVGL。
    } else if (was_calc && s_flow.page == ARCHERY_PAGE_SIGHT_CALC) {
        if (calc_changed) redraw();
        // 距离到边界后不再重绘，避免长按连发堆积。
    } else {
        redraw();
    }
}

// 物理按键 → 逻辑键。屏幕倒装时按 180° 旋转整列按键的角色。
static archery_button_t physical_button(bsp_btn_t btn) {
    switch (btn) {
    case BSP_BTN_UP: return ARCHERY_BUTTON_UP;
    case BSP_BTN_DOWN: return ARCHERY_BUTTON_DOWN;
    default: return ARCHERY_BUTTON_OK;
    }
}

static void repeat_timer_cb(void *arg) {
    (void)arg;
    const bool entry_repeat = s_flow.page == ARCHERY_PAGE_ENTRY &&
                              archery_flow_roll_entry(&s_flow);
    const bool settings_repeat = s_flow.page == ARCHERY_PAGE_SETTINGS;
    const bool setup_repeat = s_flow.page == ARCHERY_PAGE_SETUP &&
                              archery_flow_roll_entry(&s_flow) && s_flow.setup_row <= 1u;
    const bool clock_repeat = s_flow.page == ARCHERY_PAGE_CLOCK;
    const bool sight_repeat = s_flow.page == ARCHERY_PAGE_SIGHT && s_flow.sight_editing;
    const bool ruler_repeat = s_flow.page == ARCHERY_PAGE_RULER;
    const bool calc_repeat = s_flow.page == ARCHERY_PAGE_SIGHT_CALC;
    const bool metro_repeat = s_flow.page == ARCHERY_PAGE_METRONOME && s_flow.metro_edit;
    const bool range_repeat = (s_flow.page == ARCHERY_PAGE_RANGE_CONFIG && s_flow.range_edit) ||
                               s_flow.page == ARCHERY_PAGE_RANGE_VIEW;
    if (!s_repeat_active || (s_flow.page != ARCHERY_PAGE_HISTORY &&
                             s_flow.page != ARCHERY_PAGE_DIAG_LOG && !entry_repeat && !settings_repeat &&
                             !setup_repeat && !clock_repeat && !sight_repeat && !ruler_repeat &&
                             !calc_repeat && !metro_repeat && !range_repeat)) {
        if (s_repeat_timer) (void)esp_timer_stop(s_repeat_timer);
        s_repeat_active = false;
        return;
    }
    s_repeat_used = true;
    if (s_repeat_inflight) return;
    if (s_queue) {
        s_repeat_inflight = true;
        const app_message_t step = {.type = APP_MSG_KEY, .key = s_repeat_key};
        (void)xQueueSend(s_queue, &step, 0);
    }
    // 第一次触发时从 400 ms 延时切到 60 ms 周期。
    (void)esp_timer_stop(s_repeat_timer);
    (void)esp_timer_start_periodic(s_repeat_timer, s_repeat_period_us);
}

static void timer_tick_cb(void *arg) {
    (void)arg;
    if (!s_queue) return;
    const app_message_t message = {.type = APP_MSG_TIMER_TICK};
    (void)xQueueSend(s_queue, &message, 0);
}

static void diag_timer_cb(void *arg) {
    (void)arg;
    if (!s_queue || s_flow.page != ARCHERY_PAGE_HOME) return;
    const app_message_t message = {.type = APP_MSG_DIAG};
    (void)xQueueSend(s_queue, &message, 0);
}

static void ptt_timer_cb(void *arg) {
    (void)arg;
    s_ptt_used = true;
    if (!s_queue) return;
    const app_message_t start = {.type = APP_MSG_VOICE_START};
    (void)xQueueSend(s_queue, &start, 0);
}

// button 回调运行在共享 esp_timer 任务：只做映射与入队，不碰 LVGL、不阻塞。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t event, void *user) {
    (void)user;
    // 先消费唤醒守卫再判断就绪：松开事件可能早于队列就绪到达，若在这里被
    // 提前返回丢掉，守卫会一直拦到兜底时限。
    const bool was_armed = s_key_guard.armed;
    const int64_t now = esp_timer_get_time();
    if (key_guard_consume(&s_key_guard, event == BSP_BTN_RELEASE, now)) {
        if (event == BSP_BTN_RELEASE) {
            // 驱动可能先发 release、再补一个 click；把紧跟其后的 click 一并吞掉。
            s_wake_click_until_us = now + 500000;
        }
        if (was_armed && !s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键已松手，按键恢复响应");
        }
        return;
    }
    if ((event == BSP_BTN_CLICK || event == BSP_BTN_DOUBLE) &&
        now < s_wake_click_until_us) {
        s_wake_click_until_us = 0;
        return;
    }
    const archery_button_t physical = physical_button(btn);
    if (!s_ready || !s_queue) return;

    // PTT：按下 200 ms 后开始录音（短按仍算单击），松手结束。
    if (event == BSP_BTN_PRESS) {
        const archery_key_t pressed = archery_flow_button_key(physical, s_rotate_keys);
        if (pressed == ARCHERY_KEY_OK) s_repeat_used = false;
        if (s_flow.page == ARCHERY_PAGE_HOME &&
            (pressed == ARCHERY_KEY_UP || pressed == ARCHERY_KEY_DOWN)) {
            s_repeat_used = true;  // 按下即移动，吞掉松手后的 click
            const app_message_t step = {.type = APP_MSG_KEY, .key = pressed};
            (void)xQueueSend(s_queue, &step, 0);
        }
        if (pressed == ARCHERY_KEY_OK && s_flow.page == ARCHERY_PAGE_HOME &&
            s_diag_timer) {
            (void)esp_timer_stop(s_diag_timer);
            (void)esp_timer_start_once(s_diag_timer, 3000000);
        }
        if (pressed == ARCHERY_KEY_UP && s_flow.page == ARCHERY_PAGE_ENTRY &&
            archery_flow_voice_enabled(&s_flow) && !archery_flow_roll_entry(&s_flow) && s_ptt_timer) {
            (void)esp_timer_stop(s_ptt_timer);
            (void)esp_timer_start_once(s_ptt_timer, 200000);
        }
        if ((pressed == ARCHERY_KEY_UP || pressed == ARCHERY_KEY_DOWN) &&
            s_flow.page == ARCHERY_PAGE_WIFI) {
            s_repeat_used = true;
            const app_message_t step = {.type = APP_MSG_KEY, .key = pressed};
            (void)xQueueSend(s_queue, &step, 0);
        } else if ((pressed == ARCHERY_KEY_UP || pressed == ARCHERY_KEY_DOWN) &&
            s_flow.page == ARCHERY_PAGE_CLOCK && s_repeat_timer) {
            s_repeat_key = pressed;
            s_repeat_active = true;
            s_repeat_used = true;
            s_repeat_slow = false;
            s_repeat_period_us = 300000;
            (void)esp_timer_stop(s_repeat_timer);
            const app_message_t step = {.type = APP_MSG_KEY, .key = pressed};
            (void)xQueueSend(s_queue, &step, 0);
            (void)esp_timer_start_periodic(s_repeat_timer, 300000);
        } else if ((pressed == ARCHERY_KEY_UP || pressed == ARCHERY_KEY_DOWN) &&
            s_flow.page == ARCHERY_PAGE_SETUP && archery_flow_roll_entry(&s_flow) &&
            s_flow.setup_row <= 1u && s_repeat_timer) {
            s_repeat_key = pressed;
            s_repeat_active = true;
            s_repeat_used = true;
            s_repeat_slow = true;
            s_repeat_period_us = 500000;
            (void)esp_timer_stop(s_repeat_timer);
            const app_message_t step = {.type = APP_MSG_KEY, .key = pressed};
            (void)xQueueSend(s_queue, &step, 0);
            (void)esp_timer_start_periodic(s_repeat_timer, 500000);
        } else if ((pressed == ARCHERY_KEY_UP || pressed == ARCHERY_KEY_DOWN) &&
            s_flow.page == ARCHERY_PAGE_SETTINGS && s_repeat_timer) {
            s_repeat_key = pressed;
            s_repeat_active = true;
            s_repeat_used = false;
            s_repeat_slow = true;
            s_repeat_period_us = 500000;
            s_repeat_used = true;
            (void)esp_timer_stop(s_repeat_timer);
            const app_message_t step = {.type = APP_MSG_KEY, .key = pressed};
            (void)xQueueSend(s_queue, &step, 0);
            (void)esp_timer_start_once(s_repeat_timer, 500000);
        } else if ((pressed == ARCHERY_KEY_UP || pressed == ARCHERY_KEY_DOWN) &&
            s_flow.page == ARCHERY_PAGE_ENTRY && archery_flow_roll_entry(&s_flow) &&
            s_repeat_timer) {
            s_repeat_key = pressed;
            s_repeat_active = true;
            s_repeat_used = true;
            s_repeat_slow = true;
            s_repeat_period_us = 500000;
            (void)esp_timer_stop(s_repeat_timer);
            const app_message_t step = {.type = APP_MSG_KEY, .key = pressed};
            (void)xQueueSend(s_queue, &step, 0);
            (void)esp_timer_start_periodic(s_repeat_timer, 500000);
        } else if ((pressed == ARCHERY_KEY_UP || pressed == ARCHERY_KEY_DOWN) &&
                   s_flow.page == ARCHERY_PAGE_RANGE_VIEW && s_repeat_timer) {
            s_repeat_key = pressed;
            s_repeat_active = true;
            s_repeat_used = true;
            s_repeat_slow = false;
            s_repeat_period_us = 100000;
            (void)esp_timer_stop(s_repeat_timer);
            const app_message_t step = {.type = APP_MSG_KEY, .key = pressed};
            (void)xQueueSend(s_queue, &step, 0);
            (void)esp_timer_start_once(s_repeat_timer, 400000);
        } else if ((pressed == ARCHERY_KEY_UP || pressed == ARCHERY_KEY_DOWN) &&
                   s_flow.page == ARCHERY_PAGE_RANGE_CONFIG && s_flow.range_edit && s_repeat_timer) {
            s_repeat_key = pressed;
            s_repeat_active = true;
            s_repeat_used = true;
            s_repeat_slow = false;
            s_repeat_period_us = 100000;
            (void)esp_timer_stop(s_repeat_timer);
            const app_message_t step = {.type = APP_MSG_KEY, .key = pressed};
            (void)xQueueSend(s_queue, &step, 0);
            (void)esp_timer_start_once(s_repeat_timer, 400000);
        } else if ((pressed == ARCHERY_KEY_UP || pressed == ARCHERY_KEY_DOWN) &&
                   s_flow.page == ARCHERY_PAGE_RANGE_CONFIG) {
            s_repeat_used = true;
            const app_message_t step = {.type = APP_MSG_KEY, .key = pressed};
            (void)xQueueSend(s_queue, &step, 0);
        } else if ((pressed == ARCHERY_KEY_UP || pressed == ARCHERY_KEY_DOWN) &&
                   s_flow.page == ARCHERY_PAGE_METRONOME && s_flow.metro_edit && s_repeat_timer) {
            s_repeat_key = pressed;
            s_repeat_active = true;
            s_repeat_used = true;
            s_repeat_slow = false;
            s_repeat_period_us = 100000;
            (void)esp_timer_stop(s_repeat_timer);
            const app_message_t step = {.type = APP_MSG_KEY, .key = pressed};
            (void)xQueueSend(s_queue, &step, 0);
            (void)esp_timer_start_once(s_repeat_timer, 400000);
        } else if ((pressed == ARCHERY_KEY_UP || pressed == ARCHERY_KEY_DOWN) &&
                   s_flow.page == ARCHERY_PAGE_METRONOME) {
            s_repeat_used = true;
            const app_message_t step = {.type = APP_MSG_KEY, .key = pressed};
            (void)xQueueSend(s_queue, &step, 0);
        } else if ((pressed == ARCHERY_KEY_UP || pressed == ARCHERY_KEY_DOWN) &&
                   s_flow.page == ARCHERY_PAGE_SIGHT_CALC && s_repeat_timer) {
            s_repeat_key = pressed;
            s_repeat_active = true;
            s_repeat_used = true;
            s_repeat_slow = false;
            s_repeat_period_us = 100000;
            (void)esp_timer_stop(s_repeat_timer);
            const app_message_t step = {.type = APP_MSG_KEY, .key = pressed};
            (void)xQueueSend(s_queue, &step, 0);
            (void)esp_timer_start_once(s_repeat_timer, 400000);
        } else if ((pressed == ARCHERY_KEY_UP || pressed == ARCHERY_KEY_DOWN) &&
                   (s_flow.page == ARCHERY_PAGE_TOOLS ||
                    (s_flow.page == ARCHERY_PAGE_SIGHT && !s_flow.sight_editing))) {
            s_repeat_used = true;
            const app_message_t step = {.type = APP_MSG_KEY, .key = pressed};
            (void)xQueueSend(s_queue, &step, 0);
        } else if ((pressed == ARCHERY_KEY_UP || pressed == ARCHERY_KEY_DOWN) &&
                   s_flow.page == ARCHERY_PAGE_RULER && s_repeat_timer) {
            s_repeat_key = pressed;
            s_repeat_active = true;
            s_repeat_used = true;
            s_repeat_slow = false;
            s_repeat_period_us = 80000;
            (void)esp_timer_stop(s_repeat_timer);
            const app_message_t step = {.type = APP_MSG_KEY, .key = pressed};
            (void)xQueueSend(s_queue, &step, 0);
            (void)esp_timer_start_once(s_repeat_timer, 400000);
        } else if ((pressed == ARCHERY_KEY_UP || pressed == ARCHERY_KEY_DOWN) &&
                   s_flow.page == ARCHERY_PAGE_SIGHT && s_flow.sight_editing && s_repeat_timer) {
            s_repeat_key = pressed;
            s_repeat_active = true;
            s_repeat_used = true;
            s_repeat_slow = false;
            s_repeat_period_us = 80000;
            (void)esp_timer_stop(s_repeat_timer);
            const app_message_t step = {.type = APP_MSG_KEY, .key = pressed};
            (void)xQueueSend(s_queue, &step, 0);
            (void)esp_timer_start_once(s_repeat_timer, 400000);
        } else if ((pressed == ARCHERY_KEY_UP || pressed == ARCHERY_KEY_DOWN) &&
                   (s_flow.page == ARCHERY_PAGE_HISTORY || s_flow.page == ARCHERY_PAGE_DIAG ||
                    s_flow.page == ARCHERY_PAGE_DIAG_LOG) && s_repeat_timer) {
            s_repeat_key = pressed;
            s_repeat_active = true;
            s_repeat_used = true;
            s_repeat_slow = false;
            s_repeat_period_us = 60000;
            (void)esp_timer_stop(s_repeat_timer);
            const app_message_t step = {.type = APP_MSG_KEY, .key = pressed};
            (void)xQueueSend(s_queue, &step, 0);
            (void)esp_timer_start_once(s_repeat_timer, 400000);
        }
        return;
    }
    if (event == BSP_BTN_RELEASE) {
        if (s_ptt_timer) (void)esp_timer_stop(s_ptt_timer);
        if (s_repeat_timer) (void)esp_timer_stop(s_repeat_timer);
        if (s_diag_timer) (void)esp_timer_stop(s_diag_timer);
        s_repeat_active = false;
        if (archery_voice_active()) {
            const app_message_t stop = {.type = APP_MSG_VOICE_STOP};
            (void)xQueueSend(s_queue, &stop, 0);
        }
        return;
    }

    archery_key_t key;
    if (event == BSP_BTN_LONG) {
        if (archery_flow_button_is_ok(physical, s_rotate_keys)) {
            // 主页的诊断入口改为按住 3 秒，由 s_diag_timer 触发。
            if (s_flow.page == ARCHERY_PAGE_HOME) return;
            key = ARCHERY_KEY_OK_LONG;
        } else {
            return;  // 上键长按已是 PTT，不再产生普通事件
        }
    } else if (event == BSP_BTN_CLICK || event == BSP_BTN_DOUBLE) {
        if ((s_flow.page == ARCHERY_PAGE_ENTRY ||
             s_flow.page == ARCHERY_PAGE_CLOCK ||
             (s_flow.page == ARCHERY_PAGE_SETUP && s_flow.setup_row <= 1u)) &&
            (s_flow.page == ARCHERY_PAGE_CLOCK || archery_flow_roll_entry(&s_flow)) &&
            !archery_flow_button_is_ok(physical, s_rotate_keys)) {
            return;  // 滚动录入开启时上/下没有短按行为
        }
        if (s_repeat_used) {
            s_repeat_used = false;  // 长按连发已经处理过，吞掉松手后的单击
            return;
        }
        if (s_ptt_used) {
            s_ptt_used = false;  // 这次按键已经用于语音，吞掉单击
            return;
        }
        if (event == BSP_BTN_DOUBLE && s_flow.page == ARCHERY_PAGE_WIFI_PASSWORD &&
            archery_flow_button_is_ok(physical, s_rotate_keys)) {
            const uint8_t index = s_flow.wifi_target;
            if (s_flow.wifi_pw_len == 0 &&
                (s_flow.wifi_secure_mask & (1u << index))) {
                return;  // 加密网络还没输密码，忽略双击
            }
            if (index < ARCHERY_WIFI_AP_MAX) {
                char ssid[ARCHERY_WIFI_SSID_MAX];
                strncpy(ssid, s_wifi_view.ssid[index], sizeof(ssid) - 1);
                ssid[sizeof(ssid) - 1] = '\0';
                s_flow.page = ARCHERY_PAGE_WIFI;
                s_flow.wifi_mode = 2;
                s_flow.wifi_row = 0;
                (void)archery_wifi_connect(ssid, s_flow.wifi_password);
                archery_app_refresh();
            }
            return;
        }
        if (event == BSP_BTN_DOUBLE && s_flow.page == ARCHERY_PAGE_HISTORY &&
            archery_flow_button_is_ok(physical, s_rotate_keys)) {
            const app_message_t filter = {.type = APP_MSG_HISTORY_FILTER};
            (void)xQueueSend(s_queue, &filter, 0);
            return;
        }
        // 双击窗口内的快速连按会被驱动合并成 DOUBLE；其它页面按单击处理，
        // 避免快速调整数值时"按键失灵"。
        key = archery_flow_button_key(physical, s_rotate_keys);
    } else {
        return;
    }
    const app_message_t message = {.type = APP_MSG_KEY, .key = key};
    (void)xQueueSend(s_queue, &message, 0);
}

// 息屏 + 浅睡；任意键唤醒。返回后页面与训练状态都还在（RAM 不掉电）。
// 全程持有 LVGL 锁：确保没有正在进行的刷屏，唤醒后再重绘一帧。
static void light_sleep_now(void) {
    s_last_input_us = esp_timer_get_time();

    // 有键按着时不要睡：低电平唤醒条件会在入睡瞬间成立，导致立刻醒。
    const int mv = bsp_button_read_mv();
    if (mv < BSP_BTN_MV_RELEASED_MIN) {
        if (mv < 0) ESP_LOGW(TAG, "按键电压读取失败，本次不休眠");
        return;
    }
    if (!bsp_lvgl_lock(200)) return;

    app_message_t pending;
    while (xQueueReceive(s_queue, &pending, 0) == pdTRUE) {
    }

    // 手动浅睡前必须先停 Wi-Fi，否则属于 ESP-IDF 未定义行为。
    if (archery_wifi_suspend_for_light_sleep() != ESP_OK) {
        bsp_lvgl_unlock();
        ESP_LOGW(TAG, "停止 Wi-Fi 失败，本次不休眠");
        return;
    }

    bsp_display_backlight(0);
    const esp_err_t display_err = bsp_display_set_power(false);
    esp_err_t wake_err = gpio_wakeup_enable((gpio_num_t)BSP_BTN_GPIO, GPIO_INTR_LOW_LEVEL);
    if (wake_err == ESP_OK) wake_err = esp_sleep_enable_gpio_wakeup();
    int level = 1;
    const esp_err_t suspend_err = bsp_button_suspend_for_light_sleep(&level);

    if (display_err != ESP_OK || wake_err != ESP_OK || suspend_err != ESP_OK) {
        // 回滚到可操作状态：重新打开显示、恢复按键。
        (void)gpio_wakeup_disable((gpio_num_t)BSP_BTN_GPIO);
        (void)bsp_display_set_power(true);
        bsp_display_backlight(s_flow.settings.brightness);
        (void)archery_wifi_resume_after_light_sleep();
        if (suspend_err == ESP_OK) {
            (void)bsp_button_resume_after_light_sleep(on_key, NULL);
        }
        bsp_lvgl_unlock();
        ESP_LOGE(TAG, "准备浅睡失败(display=%s wake=%s button=%s)，保持唤醒",
                 esp_err_to_name(display_err), esp_err_to_name(wake_err),
                 esp_err_to_name(suspend_err));
        return;
    }

    ESP_LOGI(TAG, "息屏浅睡，低电平唤醒");
    const esp_err_t sleep_err = esp_light_sleep_start();
    const uint32_t causes = esp_sleep_get_wakeup_causes();

    // ---- 唤醒 ----
    (void)gpio_wakeup_disable((gpio_num_t)BSP_BTN_GPIO);
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    (void)bsp_display_set_power(true);
    bsp_display_backlight(s_flow.settings.brightness);
    // 唤醒那一下的按键还在按着：先吞掉，直到松手（或兜底时限）。
    key_guard_arm(&s_key_guard, esp_timer_get_time(), WAKE_GUARD_MAX_US);
    const esp_err_t resume_err = bsp_button_resume_after_light_sleep(on_key, NULL);
    const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
    key_guard_sync_released(&s_key_guard, released);
    while (xQueueReceive(s_queue, &pending, 0) == pdTRUE) {
    }
    archery_ui_show(&s_flow, &s_history, s_battery_soc);
    bsp_lvgl_unlock();
    // 先恢复 Wi-Fi 栈，再自动重连保存的网络。
    (void)archery_wifi_resume_after_light_sleep();
    (void)archery_wifi_reconnect();

    s_last_input_us = esp_timer_get_time();
    s_next_battery_us = 0;  // 醒来后立刻刷新电量
    ESP_LOGI(TAG, "已唤醒(sleep=%s resume=%s causes=0x%02x)",
             esp_err_to_name(sleep_err), esp_err_to_name(resume_err),
             (unsigned)causes);
}

// 把语音识别结果写入当前靶面的录入计数；只接受正好凑满本组箭数的结果。
static void apply_voice_result(const archery_voice_result_t *result) {
    const char *failure = NULL;
    if (!result->ok) {
        failure = "没听清，请重说";
    } else if (s_flow.page != ARCHERY_PAGE_ENTRY) {
        failure = "已忽略（不在录入页）";
    }

    const uint8_t target = s_flow.session.target;
    const bool faces = archery_flow_target_has_faces(target);
    const uint8_t face = faces ? s_flow.entry_face : 0;
    const uint8_t slots = archery_target_slot_count(target);
    const uint8_t face_arrows = faces ? s_flow.setup_face[face] : s_flow.session.arrows;
    uint8_t counts[ARCHERY_RING_SLOT_MAX];
    memset(counts, 0, sizeof(counts));

    uint16_t total = 0;
    bool has_all = false;
    uint8_t all_slot = 0xFF;

    if (!failure) {
        for (uint8_t i = 0; i < result->count; i++) {
            const archery_voice_item_t *item = &result->items[i];
            uint8_t slot = 0xFF;
            for (uint8_t s = 0; s < slots; s++) {
                if (archery_target_slot_code(target, s) == item->ring) {
                    slot = s;
                    break;
                }
            }
            if (slot == 0xFF) {
                failure = "环值不在本靶型";
                break;
            }
            if (item->all) {
                if (has_all) {
                    failure = "重复的全量项";
                    break;
                }
                has_all = true;
                all_slot = slot;
                continue;
            }
            counts[slot] = (uint8_t)(counts[slot] + item->count);
            total += item->count;
        }
    }
    if (!failure && has_all) {
        if (total >= face_arrows) {
            failure = "数量超出本组";
        } else {
            counts[all_slot] = (uint8_t)(face_arrows - total);
            total = face_arrows;
        }
    }
    if (!failure && total > face_arrows) {
        failure = "数量超出本组";
    }

    if (failure) {
        const char *detail = archery_voice_status();
        if (detail && (strstr(detail, "没听清") || strstr(detail, "数量对不上"))) {
            failure = detail;
        }
        archery_voice_set_status(failure);
    } else {
        memcpy(s_flow.counts[face], counts, slots);
        const uint8_t first = archery_target_entry_first_slot(target,
                                                             s_flow.settings.record_x);
        if (total == face_arrows) {
            s_flow.entry_row = (uint8_t)(slots - first);
            archery_voice_set_status("已填入，确定提交");
        } else {
            s_flow.entry_row = 0;
            char status[40];
            snprintf(status, sizeof(status), "已填入，还差 %u 支",
                     (unsigned)(face_arrows - total));
            archery_voice_set_status(status);
        }
    }
    redraw();
}

static void worker_task(void *argument) {
    (void)argument;
    s_next_battery_us = esp_timer_get_time();
    for (;;) {
        archery_voice_result_t voice;
        if (archery_voice_take_result(&voice)) {
            apply_voice_result(&voice);
        }
        app_message_t message;
        TickType_t wait = archery_voice_active() ? pdMS_TO_TICKS(100) : pdMS_TO_TICKS(500);
        if (s_flow.page == ARCHERY_PAGE_WIFI_PASSWORD) wait = pdMS_TO_TICKS(60);
        if (xQueueReceive(s_queue, &message, wait) == pdTRUE) {
            s_last_input_us = esp_timer_get_time();
            if (s_wifi_dimmed) {
                if (bsp_lvgl_lock(200)) {
                    bsp_display_backlight(s_flow.settings.brightness);
                    bsp_lvgl_unlock();
                }
                s_wifi_dimmed = false;
            }
            if (message.type == APP_MSG_KEY) {
                if (s_flow.page == ARCHERY_PAGE_WIFI_PASSWORD &&
                    (message.key == ARCHERY_KEY_UP || message.key == ARCHERY_KEY_DOWN) &&
                    s_pw_swallow_click) {
                    s_pw_swallow_click = false;  // 本次按下已由连发处理
                } else {
                    if (!try_saved_wifi(message.key)) handle_key(message.key);
                }
            } else if (message.type == APP_MSG_VOICE_START) {
                if (s_flow.page == ARCHERY_PAGE_ENTRY && archery_flow_voice_enabled(&s_flow) &&
                    (archery_voice_enabled() || archery_voice_has_credentials()) &&
                    !archery_voice_active()) {
                    const bool faces = archery_flow_target_has_faces(s_flow.session.target);
                    const uint8_t face = faces ? s_flow.entry_face : 0;
                    archery_voice_set_context(s_flow.session.target,
                                              faces ? s_flow.setup_face[face]
                                                    : s_flow.session.arrows);
                    archery_voice_set_status("录音中，松手结束");
                    redraw();
                    (void)archery_voice_start();
                }
            } else if (message.type == APP_MSG_VOICE_STOP) {
                archery_voice_stop();
            } else if (message.type == APP_MSG_REFRESH) {
                redraw();
            } else if (message.type == APP_MSG_VOICE_OK) {
                ESP_LOGI(TAG, "voice result message OK");
                archery_voice_play_saved();
                redraw();
            } else if (message.type == APP_MSG_VOICE_FAIL) {
                ESP_LOGI(TAG, "voice result message FAIL");
                archery_voice_play_config_failed();
                redraw();
            } else if (message.type == APP_MSG_AUTO_CHECK_RESULT) {
                if (s_flow.page == ARCHERY_PAGE_AUTO_CHECK && !s_flow.auto_check_done) {
                    apply_auto_check_result(message.key == ARCHERY_KEY_OK);
                } else {
                    s_auto_checking = false;
                }
            } else if (message.type == APP_MSG_HISTORY_FILTER) {
                if (archery_flow_history_cycle_filter(&s_flow) ==
                    ARCHERY_ACT_HISTORY_FILTER) {
                    refresh_history_count();
                    redraw();
                }
            } else if (message.type == APP_MSG_DIAG) {
                if (s_flow.page == ARCHERY_PAGE_HOME) {
                    archery_flow_open_diag(&s_flow);
                    redraw();
                }
            } else if (message.type == APP_MSG_METRONOME_TICK) {
                metro_tick();
            } else if (message.type == APP_MSG_TIMER_TICK) {
                handle_timer_tick();
            } else {
                handle_wifi_event();
            }
            continue;
        }
        password_repeat_poll();
        const int64_t now = esp_timer_get_time();
        if (now >= s_next_battery_us) {
            refresh_battery();
            s_next_battery_us = now + (int64_t)BATTERY_POLL_MS * 1000;
        }
        const uint32_t minute_now = clock_now() / 60u;
        if (minute_now != s_last_clock_minute) {
            s_last_clock_minute = minute_now;
            if (bsp_lvgl_lock(100)) {
                archery_ui_update_clock(clock_now(), s_clock_set);
                bsp_lvgl_unlock();
            }
        }
        if (now >= s_next_clock_save_us) {
            // 每 5 分钟把时钟基准写一次 NVS；写入内容很小，磨损可忽略。
            clock_persist(false);
            s_next_clock_save_us = now + (int64_t)300 * 1000000;
        }
        // 录入页若发现 Wi-Fi 掉了（浅睡、路由器重启），自动重连，避免语音前还要手动连。
        if (s_flow.page == ARCHERY_PAGE_AUTO_CHECK && !s_flow.auto_check_done &&
            s_auto_check_started_us > 0 && now - s_auto_check_started_us >= 10 * 1000000) {
            apply_auto_check_result(false);
        }
        if (s_flow.page == ARCHERY_PAGE_AUTO_CHECK && s_flow.auto_check_done &&
            s_auto_deadline_us > 0 && now >= s_auto_deadline_us) {
            s_auto_deadline_us = 0;
            archery_flow_enter_setup(&s_flow);
            redraw();
        }
        const bool wifi_now = archery_wifi_connected();
        if (wifi_now) {
            s_wifi_retry_step = 0;
            s_next_wifi_retry_us = 0;
        } else {
            if (s_wifi_prev_connected) s_wifi_need_all = true;
            if (s_flow.settings.auto_reconnect &&
                archery_wifi_auto_reconnect_enabled() &&
                !archery_wifi_retry_active() &&
                !archery_wifi_resume_pending() &&
                s_flow.page != ARCHERY_PAGE_WIFI &&
                s_flow.page != ARCHERY_PAGE_WIFI_PASSWORD &&
                !archery_wifi_provisioning() &&
                now >= s_next_wifi_retry_us) {
                static const uint32_t backoff[] = {30, 60, 120, 300, 600};
                if (s_wifi_need_all || (s_wifi_retry_step % 3u) == 0u) {
                    archery_wifi_retry_saved_async();
                    s_wifi_need_all = false;
                } else {
                    (void)archery_wifi_reconnect();
                }
                const uint32_t seconds = backoff[s_wifi_retry_step < 5 ? s_wifi_retry_step : 4];
                if (s_wifi_retry_step < 5) s_wifi_retry_step++;
                s_next_wifi_retry_us = now + (int64_t)seconds * 1000000;
            }
        }
        s_wifi_prev_connected = wifi_now;
        const uint16_t off_seconds = s_flow.settings.off_seconds;
        if (off_seconds > 0 &&
            now - s_last_input_us >= (int64_t)off_seconds * 1000000 &&
            s_flow.page != ARCHERY_PAGE_WIFI &&
            s_flow.page != ARCHERY_PAGE_WIFI_PASSWORD &&
            s_flow.page != ARCHERY_PAGE_TIMER &&
            s_flow.page != ARCHERY_PAGE_METRONOME &&
            s_flow.page != ARCHERY_PAGE_RULER &&
            !archery_wifi_provisioning()) {
            light_sleep_now();
        }
        // 联网页/配网中不浅睡（会断热点），只关背光；任意键恢复。
        const bool wifi_page = s_flow.page == ARCHERY_PAGE_WIFI ||
                               s_flow.page == ARCHERY_PAGE_WIFI_PASSWORD ||
                               archery_wifi_provisioning();
        if (wifi_page && !s_wifi_dimmed &&
            now - s_last_input_us >= (int64_t)120 * 1000000) {
            if (bsp_lvgl_lock(200)) {
                bsp_display_backlight(0);
                bsp_lvgl_unlock();
                s_wifi_dimmed = true;
            }
        }
    }
}

void archery_app_start(void) {
    archery_logbuf_init();
    archery_name_init();
    if (esp_sleep_get_wakeup_causes() & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), WAKE_GUARD_MAX_US);
        ESP_LOGI(TAG, "按键唤醒：先屏蔽按键动作，直到该键松手");
    }

    archery_flow_init(&s_flow);
    (void)archery_sight_load(&s_flow.sight);
    archery_metro_config_t metro_cfg;
    if (archery_metro_load(&metro_cfg)) {
        s_flow.metro_bpm = metro_cfg.bpm;
        s_flow.metro_accent = metro_cfg.accent;
        s_flow.metro_countin = metro_cfg.countin;
        s_flow.metro_stop_min = metro_cfg.stop_min;
        s_flow.metro_volume = metro_cfg.volume;
        s_flow.metro_preset = metro_cfg.preset;
        s_flow.metro_count_voice = metro_cfg.count_voice;
    }
    ESP_LOGI(TAG, "%s %s", ARCHERY_FIRMWARE_CHANNEL, ARCHERY_FIRMWARE_VERSION);

    if (archery_store_load(&s_history) != ESP_OK) {
        ESP_LOGW(TAG, "历史记录载入失败，从空历史开始");
        memset(&s_history, 0, sizeof(s_history));
        archery_history_meta_init(&s_history.meta);
    }

    archery_wifi_set_history(&s_history);

    archery_settings_t stored;
    if (archery_settings_load(&stored) != ESP_OK) {
        ESP_LOGW(TAG, "设置读取失败，使用默认亮度/息屏时间");
    }
    archery_flow_set_settings(&s_flow, &stored);
    apply_settings(&s_flow.settings);

    // 时钟：优先用上次保存的时间；没有就用固件编译时间近似。
    uint32_t saved_clock = 0;
    bool saved_set = false;
    if (archery_store_clock_load(&saved_clock, &saved_set) != ESP_OK || saved_clock == 0) {
        saved_clock = clock_build_seed();
        saved_set = false;
    }
    s_clock_base = saved_clock;
    s_clock_base_us = esp_timer_get_time();
    s_clock_set = saved_set;
    s_clock_saved = saved_clock;
    s_next_clock_save_us = s_clock_base_us + (int64_t)300 * 1000000;
    archery_flow_set_clock(&s_flow, clock_now(), s_clock_set);

    s_battery_soc = bsp_battery_soc();
    s_session_start_us = esp_timer_get_time();
    s_last_input_us = s_session_start_us;
    refresh_history_count();

    if (archery_wifi_init(on_wifi_event, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi 初始化失败，联网页不可用");
    }
    archery_wifi_fill_view(&s_wifi_view);
    archery_flow_set_wifi_scan(&s_flow, s_wifi_view.ap_count);

    if (archery_voice_init() != ESP_OK) {
        ESP_LOGW(TAG, "语音模块初始化失败，语音报靶不可用");
    }

    redraw();

    s_queue = xQueueCreate(KEY_QUEUE_DEPTH, sizeof(app_message_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "按键队列创建失败，界面只读");
        return;
    }
    const esp_timer_create_args_t ptt_args = {.callback = ptt_timer_cb, .name = "ptt"};
    if (esp_timer_create(&ptt_args, &s_ptt_timer) != ESP_OK) {
        ESP_LOGW(TAG, "PTT 定时器创建失败，语音改用长按");
        s_ptt_timer = NULL;
    }
    const esp_timer_create_args_t repeat_args = {.callback = repeat_timer_cb,
                                                 .name = "repeat"};
    if (esp_timer_create(&repeat_args, &s_repeat_timer) != ESP_OK) {
        ESP_LOGW(TAG, "历史连发定时器创建失败，改用逐行滚动");
        s_repeat_timer = NULL;
    }
    const esp_timer_create_args_t diag_args = {.callback = diag_timer_cb, .name = "diag"};
    if (esp_timer_create(&diag_args, &s_diag_timer) != ESP_OK) {
        ESP_LOGW(TAG, "诊断入口定时器创建失败");
        s_diag_timer = NULL;
    }
    const esp_timer_create_args_t timer_args = {.callback = timer_tick_cb, .name = "match"};
    if (esp_timer_create(&timer_args, &s_timer_tick) != ESP_OK) {
        ESP_LOGW(TAG, "比赛计时器创建失败");
        s_timer_tick = NULL;
    }
    s_countdown_q = xQueueCreate(4, sizeof(uint8_t));
    if (s_countdown_q) {
        (void)xTaskCreate(countdown_voice_task, "cd_voice", 4096, NULL, 4, NULL);
    }
    if (xTaskCreate(worker_task, "archery", WORKER_STACK_BYTES, NULL, 5, &s_worker) != pdPASS) {
        ESP_LOGE(TAG, "应用任务创建失败，界面只读");
        vQueueDelete(s_queue);
        s_queue = NULL;
        return;
    }
    s_ready = true;

    const esp_err_t err = bsp_button_init(on_key, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(err));
        s_ready = false;
    } else if (s_key_guard.armed) {
        // 唤醒键若在按键驱动启动前就已松手，组件不会产生 release 事件；
        // 初始化完成后按真实电压同步一次，已松手立即撤防。
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
        if (!s_key_guard.armed) ESP_LOGI(TAG, "唤醒键已提前松手，撤防");
    }
    ESP_LOGI(TAG, "就绪：历史 %u 条，电量 %d，亮度 %u%%，息屏 %u 秒",
             (unsigned)s_history.meta.count, s_battery_soc,
             (unsigned)s_flow.settings.brightness,
             (unsigned)s_flow.settings.off_seconds);
}

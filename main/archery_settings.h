// main/archery_settings.h —— 靶型与省电设置（靶子类型、亮度、息屏时间）的纯数据与编解码。
//
// 只放"影响记分方式或功耗、用户能感知"的项：靶子类型、屏幕亮度、空闲息屏时间。
// 息屏时间 0 表示不息屏；其它值为秒数。设置由 archery_store.c 写入 NVS。
#pragma once

#include "archery_score.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 设置页行数：0 = 靶子类型，1 = 屏幕方向，2 = 屏幕亮度，3 = 息屏时间，
// 4 = 记录内十，5 = 每组计时，6 = 倒数十秒语音，7 = 语音录靶，8 = 滚动录入，9 = 提示音量，10 = 保存 Wi-Fi 密码。
#define ARCHERY_SETTINGS_ROWS 12u

#define ARCHERY_SETTINGS_DEFAULT_BRIGHTNESS 20u
#define ARCHERY_SETTINGS_DEFAULT_OFF_SECONDS 0u
#define ARCHERY_SETTINGS_DEFAULT_TARGET ((uint8_t)ARCHERY_TARGET_HALF_40)
// 默认画面倒转(整机屏幕倒装);与 bsp_pins.h 的 BSP_LCD_ROTATE_180 一致。
#define ARCHERY_SETTINGS_DEFAULT_ROTATE 1u
#define ARCHERY_SETTINGS_DEFAULT_VOICE 1u
#define ARCHERY_SETTINGS_DEFAULT_BEEP_VOLUME 3u  // 0/20/40/60/80/100 的第 3 档 = 60%
#define ARCHERY_SETTINGS_DEFAULT_WIFI_SAVE 1u
// 每组计时：0 = 关闭，否则为每支箭秒数（20/30/40）。
#define ARCHERY_SETTINGS_DEFAULT_TIMER 0u
#define ARCHERY_SETTINGS_DEFAULT_COUNTDOWN_VOICE 3u
#define ARCHERY_SETTINGS_DEFAULT_ROLL_ENTRY 0u
#define ARCHERY_SETTINGS_DEFAULT_AUTO_MODE 0u
#define ARCHERY_SETTINGS_DEFAULT_AUTO_RECONNECT 0u
// v6：magic + 版本 + 靶型 + 方向 + 亮度 + 息屏(2B) + 记录内十 + 语音 + 音量 + 保存Wi-Fi + CRC。
// 解码兼容 v1（无靶型）、v2（无方向）、v3（无记录内十）、v4（无语音项）与 v5（无保存Wi-Fi）格式。
#define ARCHERY_SETTINGS_ENCODED_BYTES 18u
#define ARCHERY_SETTINGS_ENCODED_BYTES_V10 17u
#define ARCHERY_SETTINGS_ENCODED_BYTES_V9 16u
#define ARCHERY_SETTINGS_ENCODED_BYTES_V8 15u
#define ARCHERY_SETTINGS_ENCODED_BYTES_V7 14u
#define ARCHERY_SETTINGS_ENCODED_BYTES_V6 13u
#define ARCHERY_SETTINGS_ENCODED_BYTES_V5 12u
#define ARCHERY_SETTINGS_ENCODED_BYTES_V4 10u
#define ARCHERY_SETTINGS_ENCODED_BYTES_V2 9u
#define ARCHERY_SETTINGS_ENCODED_BYTES_V1 8u

typedef struct {
    uint8_t target;        // archery_target_t
    uint8_t rotate;        // 1 = 画面旋转 180°(默认), 0 = 正常方向
    uint8_t brightness;    // 20/40/60/80/100 (%)
    uint16_t off_seconds;  // 0 = 不息屏；15/30/60/120
    uint8_t record_x;      // 1 = 录入页保留 X（内十）行（默认），0 = 隐藏
    uint8_t voice_enabled; // 1 = 启用语音报靶（默认），0 = 只用按键
    uint8_t beep_volume;   // 提示音音量档位，对应 0/20/40/60/80/100（0 = 静音）
    uint8_t wifi_save;     // 1 = 保存 Wi-Fi 密码（默认），0 = 不保存
    uint8_t timer_seconds; // 0 = 不计时，否则 20/30/40 秒/支
    uint8_t countdown_voice; // 0 = 关闭，1 = 系统女声，2 = 小豆女声，3 = 豆包女声（默认）
    uint8_t roll_entry;      // 1 = 录入页长按上/下连续加减，短按无效
    uint8_t auto_mode;       // 1 = 开始训练前自动检测语音并选择录入方式
    uint8_t auto_reconnect;  // 1 = 断线后自动重连已保存网络
} archery_settings_t;

void archery_settings_defaults(archery_settings_t *settings);
bool archery_settings_valid(const archery_settings_t *settings);

// 可选值表（靶型、亮度百分比、息屏秒数），供界面与按键调整使用。
uint8_t archery_settings_target_count(void);
uint8_t archery_settings_target_at(uint8_t index);
uint8_t archery_settings_brightness_count(void);
uint8_t archery_settings_brightness_at(uint8_t index);
uint8_t archery_settings_off_count(void);
uint16_t archery_settings_off_at(uint8_t index);

// 调整某一项：row 0 = 靶型，row 1 = 屏幕方向，row 2 = 亮度，row 3 = 息屏时间，
// row 4 = 记录内十，row 6 = 语音输入，row 7 = 提示音量。在可选值表上钳制，不回绕；返回值是否真的改了。
bool archery_settings_adjust(archery_settings_t *settings, uint8_t row, int delta);
// 归一化互斥项：语音录靶开启时关闭滚动录入。
void archery_settings_normalize(archery_settings_t *settings);

// 编码/解码（magic + 版本 + 字段 + CRC16），用于 NVS 中的一条 blob。
// 解码兼容旧版 8 字节格式（靶型按 40 半靶处理）。
size_t archery_settings_encode(const archery_settings_t *settings,
                               uint8_t *buffer, size_t capacity);
bool archery_settings_decode(archery_settings_t *settings,
                             const uint8_t *buffer, size_t length);

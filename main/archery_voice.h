// main/archery_voice.h —— 实验功能：按住上键说话，整组报靶。
//
// 设备把 16 kHz/16 bit 单声道 PCM 通过 HTTP chunked 直传局域网代理，
// 代理负责云端识别与结构化，返回固定 JSON；本模块只做传输和最小解析。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define ARCHERY_VOICE_MAX_ITEMS 12

typedef struct {
    uint8_t ring;   // 0..10，或 11 = X（内十）
    uint8_t count;  // 箭数；all 时由设备按剩余数填充
    bool all;
} archery_voice_item_t;

typedef struct {
    bool ok;
    uint8_t count;
    archery_voice_item_t items[ARCHERY_VOICE_MAX_ITEMS];
    char error[24];
} archery_voice_result_t;

// 从 NVS 读取语音服务地址；未配置时功能关闭。
esp_err_t archery_voice_init(void);
bool archery_voice_enabled(void);
const char *archery_voice_url(void);
esp_err_t archery_voice_save_url(const char *url);

// PTT：按下（长按触发）与松开。
esp_err_t archery_voice_start(void);
void archery_voice_stop(void);
bool archery_voice_active(void);

// 本次识别使用的靶型与箭数（代理据此解析与校验）。
void archery_voice_set_context(uint8_t target, uint8_t arrows);
uint8_t archery_voice_context_target(void);
uint8_t archery_voice_context_arrows(void);

// 直连豆包流式识别所需的凭据（AppID + Access Token），存 NVS。
esp_err_t archery_voice_save_credentials(const char *appid, const char *token);
const char *archery_voice_appid(void);
const char *archery_voice_api_key(void);
esp_err_t archery_voice_save_api_key(const char *api_key);
const char *archery_voice_token(void);
bool archery_voice_has_credentials(void);
// 用当前凭据做一次 WebSocket 握手，验证配置。
bool archery_voice_test_credentials(void);

// 进入录入页时预热 WSS 连接，按下后能立刻开始录音。
void archery_voice_prewarm(void);

// 提示音音量（0/20/40/60/80/100），由设置页驱动。
void archery_voice_set_beep_volume(uint8_t percent);
// 0=关闭，1=系统女声，2=豆包女声。
void archery_voice_set_countdown_voice(uint8_t mode);

// 用当前音量响一声，供设置页试听；0 时为静音。
void archery_voice_preview_beep(void);
// 配置保存成功的柔和提示音。
void archery_voice_play_saved(void);
// 提前打开音频，避免 TLS 握手后再初始化失败。
void archery_voice_prepare_audio(void);
void archery_voice_reopen_audio(void);
// 配置失败提示音（sol -> do）。
void archery_voice_play_config_failed(void);
// 比赛计时剩 30 秒的特殊提示音（三声高频）。
void archery_voice_beep_warning(void);
// 播报倒计时数字（10..1），音频为预生成的豆包语音片段。
void archery_voice_play_countdown(uint8_t number);

// 识别完成的结果（消费一次）。
bool archery_voice_take_result(archery_voice_result_t *out);

// 录入页显示的状态文字；NULL 表示不显示。
const char *archery_voice_status(void);
void archery_voice_set_status(const char *text);

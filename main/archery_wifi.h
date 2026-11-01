// main/archery_wifi.h —— Wi-Fi 配网、扫描与 NTP 对时（实验版）。
#pragma once

#include "archery_history.h"
#include "archery_store.h"
#include "archery_wifi_view.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// 状态变化回调：在 Wi-Fi/HTTP 任务里调用，回调里不要直接碰 LVGL，交给主任务。
typedef void (*archery_wifi_event_cb_t)(void *user);

// 配网网页同时提供历史记录 CSV 下载；传 NULL 表示下载空表。
void archery_wifi_set_history(const archery_history_t *history);

esp_err_t archery_wifi_init(archery_wifi_event_cb_t cb, void *user);

// 把当前状态复制给界面。
void archery_wifi_fill_view(archery_wifi_view_t *view);

// 异步扫描；完成后通过回调通知。
esp_err_t archery_wifi_scan(void);

// 连接扫描列表里的第 index 个网络（开放网络直接连，加密网络走扫码配网）。
esp_err_t archery_wifi_connect_index(uint8_t index);

// 直接连接（供网页配网调用）。
esp_err_t archery_wifi_connect(const char *ssid, const char *password);

// 是否保存 Wi-Fi 密码（设置页控制）；关闭后不再写入，并清除已保存列表。
void archery_wifi_set_save_enabled(bool enabled);
bool archery_wifi_save_enabled(void);
void archery_wifi_forget_credentials(void);

// 打开设备热点 + 配网网页（preset_ssid 可为 NULL，网页会预填）。
esp_err_t archery_wifi_start_provisioning(const char *preset_ssid);
esp_err_t archery_wifi_stop_provisioning(void);

// 异步 NTP 对时；成功后可用 archery_wifi_take_time() 取结果。
esp_err_t archery_wifi_sync_time(void);
bool archery_wifi_take_time(uint32_t *seconds);

const char *archery_wifi_current_ssid(void);
esp_err_t archery_wifi_suspend_for_light_sleep(void);
esp_err_t archery_wifi_resume_after_light_sleep(void);
esp_err_t archery_wifi_reconnect(void);
esp_err_t archery_wifi_disconnect(void);
bool archery_wifi_connected(void);
void archery_wifi_commit_credentials(void);
void archery_wifi_set_auto_reconnect(bool enabled);
bool archery_wifi_provisioning(void);
void archery_wifi_verify_voice_async(void);

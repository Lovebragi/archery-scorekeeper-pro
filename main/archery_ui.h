// main/archery_ui.h —— 应用界面渲染入口（必须在 LVGL 锁内调用）。
#pragma once

#include "archery_flow.h"
#include "archery_store.h"
#include "archery_wifi_view.h"

// 按 flow 当前页面重建整屏。数字类页面数据（历史记录）来自 history。
// battery_soc < 0 表示电量不可用，此时只画空心电池图标。
void archery_ui_show(const archery_flow_t *flow, const archery_history_t *history,
                     int battery_soc);

// 只更新状态栏时间标签，不重建页面（调用方需持有 LVGL 锁）。
void archery_ui_update_clock(uint32_t now, bool set);

// 联网页的只读视图（应用在刷新前设置；为空时显示"模块未初始化"）。
void archery_ui_set_wifi_view(const archery_wifi_view_t *view);

// 录入页的语音状态文字（实验功能）；NULL 或空串表示不显示。
void archery_ui_set_voice_status(const char *text);

// 删除当前应用界面（退出应用时调用）。
void archery_ui_timer_tick(const archery_flow_t *flow);
void archery_ui_clear(void);

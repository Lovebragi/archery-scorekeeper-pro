// main/archery_app.h —— 应用装配：按键队列、页面渲染调度、NVS 读写与背光节电。
#pragma once

#include <stdbool.h>

// 在 BSP 显示/LVGL、NVS 就绪后调用；内部创建按键工作队列与渲染任务。
void archery_app_start(void);
// 请求主任务重绘当前页面（可在 Wi-Fi/HTTP 任务中调用）。
void archery_app_refresh(void);
// 验证任务回报结果，由主任务播放提示音。
void archery_app_voice_verified(bool ok);

// main/archery_power.h —— 深度睡眠时间设置。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint8_t archery_power_deep_minutes(void);
void archery_power_cycle_deep(void);

#ifdef __cplusplus
}
#endif

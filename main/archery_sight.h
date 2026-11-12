// main/archery_sight.h —— 瞄具纵向刻度表（每距离一个刻度，单位 0.1）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ARCHERY_SIGHT_MAX 8u

typedef struct {
    uint16_t distance_tenths;  // 0.1 m 单位
    int16_t mark_tenths;  // 例如 4.2 -> 42
} archery_sight_entry_t;

typedef struct {
    uint8_t count;
    archery_sight_entry_t items[ARCHERY_SIGHT_MAX];
} archery_sight_t;

// 默认距离：18 / 30 / 50 / 70 m，刻度为 0。
void archery_sight_defaults(archery_sight_t *sight);
bool archery_sight_load(archery_sight_t *sight);
bool archery_sight_save(const archery_sight_t *sight);

// 格式化刻度，例如 42 -> "4.2"。
void archery_sight_format(int16_t mark_tenths, char *buffer, unsigned capacity);

typedef enum {
    ARCHERY_SIGHT_LINEAR = 0,
    ARCHERY_SIGHT_QUADRATIC = 1,
} archery_sight_fit_t;

// 仅在已知距离范围内插值；超出范围或点数不足返回 false。
bool archery_sight_entry_valid(const archery_sight_t *sight, uint8_t index);
bool archery_sight_estimate(const archery_sight_t *sight, uint16_t distance_tenths,
                            archery_sight_fit_t fit, int16_t *mark_tenths);

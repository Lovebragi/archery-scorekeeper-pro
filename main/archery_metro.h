// main/archery_metro.h —— 节拍器参数持久化。
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t bpm;
    uint8_t accent;
    uint8_t countin;
    uint8_t stop_min;
    uint8_t volume;
    uint8_t preset;
    uint8_t count_voice;
} archery_metro_config_t;

void archery_metro_defaults(archery_metro_config_t *config);
bool archery_metro_load(archery_metro_config_t *config);
bool archery_metro_save(const archery_metro_config_t *config);

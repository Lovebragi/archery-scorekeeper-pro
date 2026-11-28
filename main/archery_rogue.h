// main/archery_rogue.h —— 肉鸽模式的主挑战/附加条件过滤配置。
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARCHERY_ROGUE_MAIN_COUNT 28u
#define ARCHERY_ROGUE_MOD_COUNT 8u

void archery_rogue_init(void);
uint32_t archery_rogue_block_main(void);
uint8_t archery_rogue_block_mod(void);
void archery_rogue_save(uint32_t block_main, uint8_t block_mod, bool show_story);
bool archery_rogue_show_story(void);
uint8_t archery_rogue_main_id(uint8_t index);
uint8_t archery_rogue_main_index(uint8_t main_id);

#ifdef __cplusplus
}
#endif

// main/archery_challenge.h —— 随机挑战的纯数据与生成逻辑。
//
// 设备只生成并显示训练任务，不记录或提交成绩。随机源由调用方注入，
// 便于主机测试复现；所有环值只使用 6～10 环。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARCHERY_CHALLENGE_MAIN_COUNT 33u
#define ARCHERY_CHALLENGE_MOD_COUNT 8u
#define ARCHERY_CHALLENGE_MAX_MODS 2u

typedef struct {
    bool generated;       // false = 尚未生成
    uint8_t main_id;      // 0..ARCHERY_CHALLENGE_MAIN_COUNT-1
    uint8_t mod_ids[ARCHERY_CHALLENGE_MAX_MODS];
    uint8_t mod_count;    // 0..2
    uint8_t difficulty;   // 1..10
    uint8_t arrows;
    uint8_t prev_main;    // 用于避免连续生成完全相同的内容
    uint8_t prev_mod_count;
    uint8_t prev_mod_ids[ARCHERY_CHALLENGE_MAX_MODS];
} archery_challenge_t;

void archery_challenge_init(archery_challenge_t *challenge);

// 生成一项挑战。seed 由上层提供（设备用 esp_random，主机测试用固定值）。
// 连续生成时会尽量避免与上一项完全相同；失败时保留原状态并返回 false。
bool archery_challenge_generate(archery_challenge_t *challenge, uint32_t seed,
                                uint8_t hidden_percent);

uint8_t archery_challenge_main_count(void);
uint8_t archery_challenge_mod_count_total(void);
const char *archery_challenge_main_text(uint8_t main_id);
const char *archery_challenge_mod_text(uint8_t mod_id);
uint8_t archery_challenge_main_arrows(uint8_t main_id);
uint8_t archery_challenge_main_base_difficulty(uint8_t main_id);
bool archery_challenge_main_needs_confirm(uint8_t main_id);
bool archery_challenge_main_is_hidden(uint8_t main_id);

#ifdef __cplusplus
}
#endif

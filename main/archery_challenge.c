// main/archery_challenge.c —— 随机挑战生成器。
#include "archery_challenge.h"

#include <string.h>

typedef struct {
    const char *text;
    uint8_t arrows;
    uint8_t base_difficulty;
    bool positional;      // 某一箭的位置有要求，P1 不可用
    bool score_only;      // 总分类
    uint8_t score_target; // 总分目标，0 表示非总分
    uint8_t last_ring;    // 最后一箭固定环值，0 表示不固定
    bool no_ten;          // 整组不允许出现 10
    bool all_must_ten;    // 每箭都必须是 10/内十
} challenge_main_def_t;

typedef struct {
    const char *text;
    uint8_t kind;  // 0 = 时间, 1 = 心理/流程
    uint8_t add_difficulty;
} challenge_mod_def_t;

static const challenge_main_def_t s_main[] = {
    {"恰好两支 10、两支 9、两支 8，顺序不限", 6, 8, false, false, 0, 0, false, false},
    {"五星连珠：6→7→8→9→10，恰好，依次", 5, 10, true, false, 0, 10, false, false},
    {"连中三元：三支都是内十", 3, 11, false, false, 0, 0, false, true},
    {"6 支全部上靶，最后一箭 10 环", 6, 5, true, false, 0, 10, false, false},
    {"6 支全部 8 环以上", 6, 4, false, false, 0, 0, false, false},
    {"6 支全部上靶", 6, 1, false, false, 0, 0, false, false},
    {"6 箭总分 ≥ 52", 6, 4, false, true, 52, 0, false, false},
    {"6 箭总分 ≥ 54", 6, 6, false, true, 54, 0, false, false},
    {"10→10→9→9→8→8", 6, 9, true, false, 0, 8, false, false},
    {"10→9→8→10→9→8", 6, 9, true, false, 0, 8, false, false},
    {"8→9→10→8→9→10", 6, 9, true, false, 0, 10, false, false},
    {"恰好一支 10、一支 9、一支 8，顺序不限", 3, 6, false, false, 0, 0, false, false},
    {"恰好两支 10、一支 9，顺序不限", 3, 7, false, false, 0, 0, false, false},
    {"恰好两支 9、两支 8，顺序不限", 4, 7, false, false, 0, 0, true, false},
    {"6 支里至少三支 10 环", 6, 6, false, false, 0, 0, false, false},
    {"6 支里至少四支 9 环以上", 6, 4, false, false, 0, 0, false, false},
    {"6 支全部上靶，第一箭 10 环", 6, 4, true, false, 0, 0, false, false},
    {"6 支全部上靶，前两箭 10 环", 6, 6, true, false, 0, 0, false, false},
    {"一箭 10 环", 1, 5, true, false, 0, 0, false, true},
    {"6 支全部 9 环以上", 6, 7, false, false, 0, 0, false, false},
    {"9 支全部上靶，且至少 7 支 9 环以上", 9, 6, false, false, 0, 0, false, false},
    {"9 支全部 8 环以上", 9, 5, false, false, 0, 0, false, false},
    {"6 支全部 8 环以上，且低于 9 的最多一支", 6, 5, false, false, 0, 0, false, false},
    {"6 箭总分 ≥ 48", 6, 3, false, true, 48, 0, false, false},
    {"6 箭总分 ≥ 56", 6, 7, false, true, 56, 0, false, false},
    {"3 箭总分 ≥ 28", 3, 5, false, true, 28, 0, false, false},
    {"3 箭总分 ≥ 29", 3, 6, false, true, 29, 0, false, false},
    {"9 箭总分 ≥ 78", 9, 5, false, true, 78, 0, false, false},
    {"9 箭总分 ≥ 80", 9, 6, false, true, 80, 0, false, false},
    {"五子登科：五支都是 10 环", 5, 10, false, false, 0, 0, false, true},
    {"一箭定乾坤：一支内十", 1, 7, true, false, 0, 0, false, true},
    {"十全十美：十支全部 9 环以上", 10, 10, false, false, 0, 0, false, false},
    {"二龙戏珠：两支都是内十", 2, 9, false, false, 0, 0, false, true},
    {"6 箭总分 ≥ 42", 6, 2, false, true, 42, 0, false, false},
};

static const challenge_mod_def_t s_mod[] = {
    {"每箭 30 秒内完成", 0, 1},
    {"每箭 20 秒内完成", 0, 2},
    {"全部射完前不看靶", 1, 1},
    {"最后一箭必须 10 环", 1, 1},
    {"第一箭高于 8 环时按 8 环计", 1, 1},
    {"不允许中途休息或调整心态", 1, 1},
    {"每箭前深呼吸三次再射", 1, 1},
    {"后退约 30cm，不调整瞄具", 1, 2},
};

static uint32_t next_random(uint32_t *state) {
    uint32_t x = *state ? *state : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

enum {
    MOD_T4 = 0,
    MOD_T5,
    MOD_P1,
    MOD_P3,
    MOD_P4,
    MOD_P5,
    MOD_P6,
    MOD_P8,
};

static bool mod_compatible(uint8_t main_id, uint8_t mod_id) {
    if (main_id >= ARCHERY_CHALLENGE_MAIN_COUNT ||
        mod_id >= ARCHERY_CHALLENGE_MOD_COUNT) return false;
    const challenge_main_def_t *m = &s_main[main_id];
    switch (mod_id) {
    case MOD_T4:
    case MOD_T5:
    case MOD_P5:
        return m->arrows > 1u;
    case MOD_P1:
        return !m->positional;
    case MOD_P3:
        return m->last_ring == 0u && !m->no_ten && !m->all_must_ten;
    case MOD_P4:
        return m->score_only && m->score_target != 29u;
    case MOD_P6:
    case MOD_P8:
        return true;
    default:
        return false;
    }
}

static bool mod_pair_compatible(uint8_t a, uint8_t b) {
    if (a == MOD_T5 && b == MOD_P6) return false;
    if (b == MOD_T5 && a == MOD_P6) return false;
    return true;
}

static bool mod_fits(uint8_t main_id, uint8_t mod_id, bool hidden) {
    if (!mod_compatible(main_id, mod_id)) return false;
    if (hidden) return true;
    const unsigned total = (unsigned)s_main[main_id].base_difficulty +
                           s_mod[mod_id].add_difficulty;
    return total <= 10u;
}

bool archery_challenge_generate_range(archery_challenge_t *challenge, uint32_t seed,
                                      uint8_t hidden_percent, uint8_t min_difficulty,
                                      uint8_t max_difficulty) {
    const uint16_t hundredths = (uint16_t)((hidden_percent > 100u ? 100u : hidden_percent) * 100u);
    return archery_challenge_generate_range_chance(challenge, seed, hundredths,
                                                  min_difficulty, max_difficulty);
}

bool archery_challenge_generate(archery_challenge_t *challenge, uint32_t seed,
                                uint8_t hidden_percent) {
    return archery_challenge_generate_range(challenge, seed, hidden_percent, 1u, 15u);
}

void archery_challenge_init(archery_challenge_t *challenge) {
    if (!challenge) return;
    memset(challenge, 0, sizeof(*challenge));
}

bool archery_challenge_generate_range_chance(archery_challenge_t *challenge, uint32_t seed,
                                             uint16_t hidden_hundredths, uint8_t min_difficulty,
                                             uint8_t max_difficulty) {
    if (!challenge) return false;
    uint32_t rng = seed ? seed : 1u;
    for (unsigned attempt = 0; attempt < 64; attempt++) {
        static const uint8_t normal_ids[] = {
            0u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u,
            15u, 16u, 17u, 18u, 19u, 20u, 21u, 22u, 23u, 24u, 25u,
            26u, 27u, 28u, 33u,
        };
        static const uint8_t hidden_ids[] = {1u, 2u, 29u, 30u, 31u, 32u};
        const bool hidden = (next_random(&rng) % 10000u) <
                            (hidden_hundredths > 10000u ? 10000u : hidden_hundredths);
        const uint8_t main_id = hidden
            ? hidden_ids[next_random(&rng) % (uint8_t)(sizeof(hidden_ids))]
            : normal_ids[next_random(&rng) % (uint8_t)(sizeof(normal_ids))];
        uint8_t mod_ids[ARCHERY_CHALLENGE_MAX_MODS] = {0};
        uint8_t mod_count = (uint8_t)(next_random(&rng) % 3u);
        uint8_t chosen = 0;
        if (mod_count == 2u) {
            uint8_t time_ids[ARCHERY_CHALLENGE_MOD_COUNT];
            uint8_t time_count = 0;
            uint8_t mind_ids[ARCHERY_CHALLENGE_MOD_COUNT];
            uint8_t mind_count = 0;
            for (uint8_t i = 0; i < ARCHERY_CHALLENGE_MOD_COUNT; i++) {
                if (!mod_fits(main_id, i, hidden)) continue;
                if (s_mod[i].kind == 0u) time_ids[time_count++] = i;
                else mind_ids[mind_count++] = i;
            }
            if (time_count == 0u || mind_count == 0u) {
                mod_count = 1u;
            } else {
                const uint8_t time_id = time_ids[next_random(&rng) % time_count];
                uint8_t compatible_mind[ARCHERY_CHALLENGE_MOD_COUNT];
                uint8_t compatible_count = 0;
                for (uint8_t i = 0; i < mind_count; i++) {
                    const unsigned total = (unsigned)s_main[main_id].base_difficulty +
                                           s_mod[time_id].add_difficulty +
                                           s_mod[mind_ids[i]].add_difficulty;
                    if (mod_pair_compatible(time_id, mind_ids[i]) &&
                        (hidden || total <= 10u)) {
                        compatible_mind[compatible_count++] = mind_ids[i];
                    }
                }
                if (compatible_count == 0u) {
                    mod_count = 1u;
                } else {
                    mod_ids[chosen++] = time_id;
                    mod_ids[chosen++] = compatible_mind[next_random(&rng) % compatible_count];
                }
            }
        }
        if (mod_count == 1u) {
            uint8_t valid[ARCHERY_CHALLENGE_MOD_COUNT];
            uint8_t valid_count = 0;
            for (uint8_t i = 0; i < ARCHERY_CHALLENGE_MOD_COUNT; i++) {
                if (mod_fits(main_id, i, hidden)) valid[valid_count++] = i;
            }
            if (valid_count == 0u) {
                mod_count = 0u;
                chosen = 0;
            } else {
                mod_ids[0] = valid[next_random(&rng) % valid_count];
                chosen = 1;
            }
        }
        if (chosen != mod_count) mod_count = chosen;

        uint8_t difficulty = s_main[main_id].base_difficulty;
        for (uint8_t i = 0; i < mod_count; i++) {
            difficulty = (uint8_t)(difficulty + s_mod[mod_ids[i]].add_difficulty);
        }
        if (difficulty == 0u) difficulty = 1u;
        if (difficulty > 15u) difficulty = 15u;
        const bool hidden_result = archery_challenge_main_is_hidden(main_id);
        if (!hidden_result && min_difficulty > 0u &&
            (difficulty < min_difficulty || difficulty > max_difficulty)) {
            continue;
        }

        const bool same_as_previous =
            challenge->generated &&
            main_id == challenge->prev_main &&
            mod_count == challenge->prev_mod_count &&
            (mod_count == 0u ||
             (mod_ids[0] == challenge->prev_mod_ids[0] &&
              (mod_count == 1u || mod_ids[1] == challenge->prev_mod_ids[1])));
        if (same_as_previous && attempt < 63u) continue;

        challenge->main_id = main_id;
        challenge->mod_count = mod_count;
        challenge->mod_ids[0] = mod_count > 0u ? mod_ids[0] : 0u;
        challenge->mod_ids[1] = mod_count > 1u ? mod_ids[1] : 0u;
        challenge->difficulty = difficulty;
        challenge->arrows = s_main[main_id].arrows;
        challenge->generated = true;
        challenge->prev_main = main_id;
        challenge->prev_mod_count = mod_count;
        challenge->prev_mod_ids[0] = challenge->mod_ids[0];
        challenge->prev_mod_ids[1] = challenge->mod_ids[1];
        return true;
    }
    return false;
}

uint8_t archery_challenge_main_count(void) {
    return (uint8_t)(sizeof(s_main) / sizeof(s_main[0]));
}

uint8_t archery_challenge_mod_count_total(void) {
    return (uint8_t)(sizeof(s_mod) / sizeof(s_mod[0]));
}

const char *archery_challenge_main_text(uint8_t main_id) {
    return main_id < ARCHERY_CHALLENGE_MAIN_COUNT ? s_main[main_id].text : "";
}

const char *archery_challenge_mod_text(uint8_t mod_id) {
    return mod_id < ARCHERY_CHALLENGE_MOD_COUNT ? s_mod[mod_id].text : "";
}

uint8_t archery_challenge_main_arrows(uint8_t main_id) {
    return main_id < ARCHERY_CHALLENGE_MAIN_COUNT ? s_main[main_id].arrows : 0u;
}

uint8_t archery_challenge_main_base_difficulty(uint8_t main_id) {
    return main_id < ARCHERY_CHALLENGE_MAIN_COUNT
               ? s_main[main_id].base_difficulty : 0u;
}

bool archery_challenge_main_needs_confirm(uint8_t main_id) {
    return main_id < ARCHERY_CHALLENGE_MAIN_COUNT
               ? s_main[main_id].positional : false;
}

bool archery_challenge_main_is_hidden(uint8_t main_id) {
    return main_id == 1u || main_id == 2u ||
           (main_id >= 29u && main_id <= 32u);
}

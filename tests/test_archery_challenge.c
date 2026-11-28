#include "archery_challenge.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_counts_and_text(void) {
    assert(archery_challenge_main_count() == 34u);
    assert(archery_challenge_mod_count_total() == 8u);
    for (uint8_t i = 0; i < archery_challenge_main_count(); i++) {
        assert(archery_challenge_main_text(i)[0] != '\0');
        assert(archery_challenge_main_arrows(i) > 0u);
        assert(archery_challenge_main_base_difficulty(i) >= 1u);
        assert(archery_challenge_main_base_difficulty(i) <= 12u);
    }
    for (uint8_t i = 0; i < archery_challenge_mod_count_total(); i++) {
        assert(archery_challenge_mod_text(i)[0] != '\0');
    }
}

static void test_generation_constraints(void) {
    archery_challenge_t c;
    archery_challenge_init(&c);
    uint64_t seen_main = 0;
    uint16_t seen_mods = 0;
    for (uint32_t seed = 1; seed < 4000; seed++) {
        assert(archery_challenge_generate(&c, seed, 1u));
        assert(c.generated);
        assert(c.main_id < archery_challenge_main_count());
        assert(c.mod_count <= ARCHERY_CHALLENGE_MAX_MODS);
        assert(c.difficulty >= 1u && c.difficulty <= 15u);
        if (!archery_challenge_main_is_hidden(c.main_id)) {
            assert(c.difficulty <= 10u);
        }
        assert(c.arrows == archery_challenge_main_arrows(c.main_id));
        seen_main |= 1ULL << c.main_id;
        for (uint8_t i = 0; i < c.mod_count; i++) {
            assert(c.mod_ids[i] < archery_challenge_mod_count_total());
            seen_mods |= (uint16_t)(1u << c.mod_ids[i]);
            if (c.mod_ids[i] == 0u || c.mod_ids[i] == 1u) assert(c.arrows > 1u);
            if (c.mod_ids[i] == 2u) {
                assert(!archery_challenge_main_needs_confirm(c.main_id));
            }
            if (c.mod_ids[i] == 4u) {
                assert(strstr(archery_challenge_main_text(c.main_id), "总分") != NULL);
            }
        }
        if (c.mod_count == 2u) {
            assert(c.mod_ids[0] != c.mod_ids[1]);
            // T5 (id 1) 与 P6 (id 6) 不能同时出现
            assert(!((c.mod_ids[0] == 1u && c.mod_ids[1] == 6u) ||
                     (c.mod_ids[0] == 6u && c.mod_ids[1] == 1u)));
        }
    }
    assert(seen_main == 0x3FFFFFFFFULL);
    assert(seen_mods == 0xFFu);
}

static void test_no_immediate_repeat(void) {
    archery_challenge_t c;
    archery_challenge_init(&c);
    assert(archery_challenge_generate(&c, 12345u, 1u));
    for (uint32_t seed = 12346u; seed < 12400u; seed++) {
        const uint8_t prev_main = c.main_id;
        const uint8_t prev_count = c.mod_count;
        const uint8_t prev0 = c.mod_ids[0];
        const uint8_t prev1 = c.mod_ids[1];
        assert(archery_challenge_generate(&c, seed, 1u));
        const bool same = c.main_id == prev_main && c.mod_count == prev_count &&
                          (c.mod_count == 0u ||
                           (c.mod_ids[0] == prev0 &&
                            (c.mod_count == 1u || c.mod_ids[1] == prev1)));
        assert(!same);
    }
}

static void test_single_arrow_modifiers(void) {
    archery_challenge_t c;
    archery_challenge_init(&c);
    for (uint32_t seed = 1; seed < 2000; seed++) {
        assert(archery_challenge_generate(&c, seed, seed % 2u ? 0u : 100u));
        if (c.arrows != 1u) continue;
        for (uint8_t i = 0; i < c.mod_count; i++) {
            assert(c.mod_ids[i] != 0u);
            assert(c.mod_ids[i] != 1u);
            assert(c.mod_ids[i] != 5u);
        }
    }
}

static void test_difficulty_ranges(void) {
    static const uint8_t mins[12] = {1,2,3,4,4,5,5,6,7,8,8,9};
    static const uint8_t maxs[12] = {3,4,5,6,6,7,7,8,9,9,10,10};
    archery_challenge_t c;
    for (uint8_t level = 0; level < 12; level++) {
        for (uint32_t seed = 1; seed < 40; seed++) {
            archery_challenge_init(&c);
            assert(archery_challenge_generate_range(&c, seed, 0u,
                                                    mins[level], maxs[level]));
            assert(!archery_challenge_main_is_hidden(c.main_id));
            assert(c.difficulty >= mins[level]);
            assert(c.difficulty <= maxs[level]);
        }
    }
}

static void test_hidden_guarantee(void) {
    archery_challenge_t c;
    archery_challenge_init(&c);
    for (uint32_t seed = 1; seed < 30; seed++) {
        assert(archery_challenge_generate(&c, seed, 100u));
        assert(archery_challenge_main_is_hidden(c.main_id));
        assert(c.difficulty <= 15u);
    }
}

int main(void) {
    test_counts_and_text();
    test_generation_constraints();
    test_no_immediate_repeat();
    test_difficulty_ranges();
    test_single_arrow_modifiers();
    test_hidden_guarantee();
    puts("archery_challenge tests passed");
    return 0;
}
